/**
 * expLORA Gateway
 *
 * GatewayOta - see GatewayOta.h
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "GatewayOta.h"

#include <LittleFS.h>
#include <atomic>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mutex>
#include <new>
#include <time.h>

#include "../Hardware/FlashPermit.h"
#include "../Security/Sha256.h"
#include "../config.h"
#include "UpdateState.h"

namespace GatewayOta
{
namespace
{
constexpr const char *DIR = "/update";
constexpr const char *PATH = "/update/state.json";
constexpr const char *TMP = "/update/state.tmp";

Logger *log = nullptr;
std::mutex mtx; // everything below
UpdateState::State state;
bool stateLoaded = false;
GatewayImage::Pending waiting; // an unsigned image waiting for the confirmation
const esp_partition_t *waitingPart = nullptr;
GatewayImage::Class waitingCls = GatewayImage::Class::Refused;
uint32_t waitingKey = 0;
uint32_t waitingVersion = 0; // NoKey / Verified (older version): the block's version
uint8_t expiredDigest[32] = {}; // the last image whose confirmation expired (for the answer to a late confirm)
bool expiredKnown = false;
bool writing = false;   // a Writer holds the slot (until conclude() applied its decision)
bool activated = false; // an image was activated: the gateway restarts, no other install
// A save of the state file failed in this run: the "written" record on disk
// may be stale, so the boot path does not act on it
bool saveFailed = false;
std::atomic<bool> waitingFlag{false}; // waiting.valid, read without the lock by process()

std::mutex valMtx; // validation, markFailures, markRetryAt (loop task writes, web task reads)
GatewayImage::Validation validation;
uint8_t markFailures = 0;
uint32_t markRetryAt = 0;
constexpr uint8_t MARK_TRIES = 5;
constexpr uint32_t MARK_RETRY_MS = 10000;
std::mutex markMtx; // one attempt to mark the image valid at a time
std::atomic<bool> pendingVerify{false};

void logInfo(const String &s)
{
    if (log)
    {
        log->info(s);
    }
}
void logWarning(const String &s)
{
    if (log)
    {
        log->warning(s);
    }
}
void logError(const String &s)
{
    if (log)
    {
        log->error(s);
    }
}

uint32_t unixNow()
{
    const time_t t = time(nullptr);
    return t > 1700000000 ? (uint32_t)t : 0;
}

String versionText(uint32_t v)
{
    return String(v >> 16) + "." + String((v >> 8) & 0xFF) + "." + String(v & 0xFF);
}

// mtx held
void loadLocked()
{
    if (stateLoaded)
    {
        return;
    }
    stateLoaded = true;
    state = UpdateState::State();
    File f = LittleFS.open(PATH, "r");
    if (!f)
    {
        return;
    }
    std::unique_ptr<char[]> buf(new (std::nothrow) char[UpdateState::TEXT_CAPACITY]);
    if (!buf)
    {
        f.close();
        logWarning("Update state: no memory to read it");
        stateLoaded = false;
        return;
    }
    const size_t n = f.readBytes(buf.get(), UpdateState::TEXT_CAPACITY - 1);
    f.close();
    buf[n] = '\0';
    if (!UpdateState::parse(buf.get(), n, state))
    {
        logWarning("Update state " + String(PATH) + " unreadable - starting a new one");
    }
}

bool saveWrite();

// mtx held. tmp + rename (a reset meanwhile keeps the old file)
bool saveLocked()
{
    if (saveWrite())
    {
        return true;
    }
    saveFailed = true;
    return false;
}

bool saveWrite()
{
    std::unique_ptr<char[]> buf(new (std::nothrow) char[UpdateState::TEXT_CAPACITY]);
    const size_t n = buf ? UpdateState::format(state, buf.get(), UpdateState::TEXT_CAPACITY) : 0;
    if (n == 0)
    {
        logError("Update state could not be formatted (memory) - not saved");
        return false;
    }
    FlashWriteGuard guard;
    if (!LittleFS.exists(DIR))
    {
        LittleFS.mkdir(DIR);
    }
    File f = LittleFS.open(TMP, "w");
    if (!f)
    {
        logError("Update state: cannot write " + String(TMP));
        return false;
    }
    const bool ok = f.write((const uint8_t *)buf.get(), n) == n;
    f.close();
    if (!ok || !LittleFS.rename(TMP, PATH))
    {
        LittleFS.remove(TMP);
        logError("Update state: saving " + String(PATH) + " failed");
        return false;
    }
    return true;
}

void setLast(const String &text)
{
    UpdateState::copyText(state.lastText, sizeof(state.lastText), text.c_str());
    state.lastAt = unixNow();
}

bool invalidState(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    return p && esp_ota_get_state_partition(p, &st) == ESP_OK &&
           (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED);
}

// A recorded "verified" counts only with a key this firmware carries (as for
// stored sensor images): a bench or rotated-out key reads as unknown key
String sigOf(const UpdateState::Image &img)
{
    UpdateState::Image shown = img;
    if (strcmp(shown.sig, "verified") == 0)
    {
        bool known = false;
        for (size_t i = 0; i < ReleaseKeys::SLOTS; i++)
        {
            known = known || (shown.key != 0 && ReleaseKeys::KEYS[i].keyId == shown.key);
        }
        if (!known)
        {
            UpdateState::copyText(shown.sig, sizeof(shown.sig), "nokey");
        }
    }
    char t[48];
    UpdateState::sigText(shown, t, sizeof(t));
    return String(t);
}

// mtx held: the pending record becomes the running one
void finalizeLocked(const char *how)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    UpdateState::Image r;
    if (state.pending.set && running && strcmp(state.pending.part, running->label) == 0)
    {
        r = state.pending;
    }
    else
    {
        r.set = true; // installed by another firmware or over USB: the signature is not known
    }
    r.set = true;
    r.at = 0;
    UpdateState::copyText(r.ver, sizeof(r.ver), FIRMWARE_VERSION);
    UpdateState::copyText(r.part, sizeof(r.part), running ? running->label : "");
    state.running = r;
    state.pending = UpdateState::Image();
    setLast("v" + String(FIRMWARE_VERSION) + (r.sig[0] ? " (" + sigOf(r) + ")" : String("")) + " validated " + how);
    saveLocked();
}

// The otadata write first; the image counts as valid (pendingVerify
// cleared, record moved) only when it succeeded. A failure is retried from
// process() (MARK_TRIES in all, MARK_RETRY_MS apart)
bool markValid(const char *how)
{
    std::lock_guard<std::mutex> once(markMtx);
    if (!pendingVerify)
    {
        return true;
    }
    esp_err_t err;
    {
        FlashWriteGuard guard; // writes the otadata partition
        err = esp_ota_mark_app_valid_cancel_rollback();
    }
    if (err != ESP_OK)
    {
        uint8_t n;
        {
            std::lock_guard<std::mutex> l(valMtx);
            validation.markDone(); // the 120 s are over; retries follow
            if (markFailures < 255)
            {
                markFailures++;
            }
            n = markFailures;
            markRetryAt = millis() + MARK_RETRY_MS;
        }
        logError("Marking firmware v" + String(FIRMWARE_VERSION) + " valid failed (" + String(esp_err_to_name(err)) +
                 ", attempt " + String(n) + ")" +
                 (n < MARK_TRIES ? String(" - tried again in ") + String(MARK_RETRY_MS / 1000) + " s"
                                 : String(" - giving up; a restart rolls back to the previous firmware")));
        return false;
    }
    {
        std::lock_guard<std::mutex> l(valMtx);
        validation.markDone();
    }
    pendingVerify = false;
    logInfo("Firmware v" + String(FIRMWARE_VERSION) + " validated " + how + " - no rollback any more");
    std::lock_guard<std::mutex> lock(mtx);
    loadLocked();
    finalizeLocked(how);
    return true;
}

// The slot's first sector erased: a refused or abandoned image never boots
// (the bootloader may fall back to any valid app image if the running one
// is damaged)
void eraseHead(const esp_partition_t *p)
{
    if (!p)
    {
        return;
    }
    FlashWriteGuard guard;
    esp_partition_erase_range(p, 0, 4096); // one flash sector
}

bool samePart(const esp_partition_t *a, const esp_partition_t *b)
{
    return a && b && a->address == b->address;
}

// mtx held. An image that waits for its confirmation and is given up: its
// first sector erased (it must not stay bootable) - never the running slot
// or the slot otadata starts - and the "written" record cleared
void dropWaitingLocked(const String &why)
{
    if (!waiting.valid)
    {
        return;
    }
    loadLocked();
    const esp_partition_t *p = waitingPart;
    waiting = GatewayImage::Pending();
    waitingPart = nullptr;
    waitingFlag = false;
    const bool erase = !writing && p && !samePart(p, esp_ota_get_running_partition()) &&
                       !samePart(p, esp_ota_get_boot_partition());
    if (erase)
    {
        eraseHead(p);
    }
    if (state.written.set && p && strcmp(state.written.part, p->label) == 0)
    {
        state.written = UpdateState::Image();
    }
    saveLocked();
    logWarning("Firmware image waiting for a confirmation dropped (" + why + ")" +
               (erase ? " - slot " + String(p->label) + " erased" : String("")));
}

// mtx held. A waiting image whose 10 minutes are over is dropped at once
// (process() checks every loop pass, so the millis() age never wraps into a
// valid one again)
void expireWaitingLocked()
{
    if (!waiting.valid || GatewayImage::confirmLeftMs(waiting, millis()) != 0)
    {
        return;
    }
    loadLocked();
    memcpy(expiredDigest, waiting.digest, 32);
    expiredKnown = true;
    setLast("not activated: the confirmation expired (10 minutes)");
    dropWaitingLocked("the confirmation expired after 10 minutes");
}

// mtx held. "" or the reason
String activateLocked(const esp_partition_t *part, GatewayImage::Class cls, uint32_t key, uint32_t fwVersion,
                      const String &how)
{
    if (!part)
    {
        return "no OTA partition";
    }
    loadLocked();
    UpdateState::Image p;
    p.set = true;
    // The version: the signed block's (Verified / NoKey); an unsigned image
    // writes its own when it starts
    if (cls == GatewayImage::Class::Verified || cls == GatewayImage::Class::NoKey)
    {
        UpdateState::copyText(p.ver, sizeof(p.ver), versionText(fwVersion).c_str());
    }
    UpdateState::copyText(p.part, sizeof(p.part), part->label);
    UpdateState::copyText(p.sig, sizeof(p.sig), GatewayImage::className(cls));
    p.key = cls == GatewayImage::Class::Unsigned ? 0 : key;
    p.at = unixNow();
    state.pending = p;
    state.written = UpdateState::Image(); // activated, no abandoned image
    setLast("activated " + String(p.ver[0] ? "v" + String(p.ver) + ", " : String("")) + sigOf(p) + ", " + how);
    if (!saveLocked())
    {
        // Activated anyway: the record only names the image (the bootloader's
        // otadata state does the rollback); refusing a good image because the
        // filesystem is full would be worse. The next start then reports
        // "started after an update by another firmware"
        logWarning("Firmware install record not saved - activating anyway (the next start cannot name the image)");
    }
    esp_err_t err;
    {
        FlashWriteGuard guard; // writes the otadata partition
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK)
    {
        // The image is valid but not activated: it must not stay bootable
        // (the bootloader may fall back to any valid image) - never the
        // running slot or the one otadata starts
        const bool erase =
            !samePart(part, esp_ota_get_running_partition()) && !samePart(part, esp_ota_get_boot_partition());
        if (erase)
        {
            eraseHead(part);
        }
        logError("Firmware activation failed (esp_ota_set_boot_partition: " + String(esp_err_to_name(err)) + ")" +
                 (erase ? " - slot " + String(part->label) + " erased" : String("")));
        state.pending = UpdateState::Image();
        setLast("activation failed: " + String(esp_err_to_name(err)));
        saveLocked();
        return String("activation failed (") + esp_err_to_name(err) + ")";
    }
    activated = true; // the restart follows: no other install meanwhile
    return "";
}

void jsonText(Print &out, const char *s)
{
    out.print('"');
    for (; *s; s++)
    {
        const char c = *s;
        if (c == '"' || c == '\\')
        {
            out.print('\\');
            out.print(c);
        }
        else if ((uint8_t)c < 0x20)
        {
            out.print(' ');
        }
        else
        {
            out.print(c);
        }
    }
    out.print('"');
}
} // namespace

struct Writer::State
{
    Sha256 hash;
    GatewayImage::Stream<Sha256> stream;
};

void begin(Logger &logger)
{
    log = &logger;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    const bool verifying = running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
                           st == ESP_OTA_IMG_PENDING_VERIFY;
    pendingVerify = verifying;
    if (GatewayImage::runningVersionOf(FIRMWARE_VERSION) == GatewayImage::VERSION_UNKNOWN)
    {
        logWarning("Firmware version text \"" + String(FIRMWARE_VERSION) +
                   "\" is not maj.min.pat - the downgrade check cannot compare, so every verified image needs the "
                   "admin's confirmation");
    }
    std::lock_guard<std::mutex> lock(mtx);
    loadLocked();
    const esp_partition_t *pp = nullptr;
    if (state.pending.set && state.pending.part[0])
    {
        pp = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, state.pending.part);
    }
    const bool runningIsPending = pp && running && pp->address == running->address;
    const GatewayImage::Boot boot = GatewayImage::bootOutcome(state.pending.set, runningIsPending, invalidState(pp));
    const String ver = state.pending.ver[0] ? "v" + String(state.pending.ver) : String("the new firmware");
    switch (boot)
    {
    case GatewayImage::Boot::Running:
        if (verifying)
        {
            if (strcmp(state.pending.ver, FIRMWARE_VERSION) != 0)
            {
                // Known now (an unsigned image carries no version the
                // installer could read): the old firmware names it after a rollback
                UpdateState::copyText(state.pending.ver, sizeof(state.pending.ver), FIRMWARE_VERSION);
                saveLocked();
            }
            logInfo("Firmware v" + String(FIRMWARE_VERSION) + " (" + sigOf(state.pending) +
                    ") started after an update - it is validated after " +
                    String(GatewayImage::VALIDATE_AFTER_MS / 1000) +
                    " s of normal operation; a reset before that rolls back to the previous firmware");
        }
        else
        {
            // No rollback state (a bootloader without it): valid as it runs
            finalizeLocked("at start (no rollback state)");
        }
        break;
    case GatewayImage::Boot::RolledBack:
    {
        const String text = "rolled back from " + ver + " (" + sigOf(state.pending) + ") - it did not run for " +
                            String(GatewayImage::VALIDATE_AFTER_MS / 1000) + " s; v" + FIRMWARE_VERSION +
                            " runs again";
        logWarning("Firmware update " + text);
        state.pending = UpdateState::Image();
        setLast(text);
        saveLocked();
        break;
    }
    case GatewayImage::Boot::NotRunning:
        // Also a reset between the record and the otadata switch (the
        // activation never took effect)
        logInfo("The firmware activated earlier (" + ver +
                ") is not running - replaced by another install, or the activation was interrupted");
        state.pending = UpdateState::Image();
        setLast(ver + " did not start (replaced by another install, or the activation was interrupted)");
        saveLocked();
        break;
    case GatewayImage::Boot::Nothing:
        if (verifying)
        {
            logInfo("Firmware v" + String(FIRMWARE_VERSION) +
                    " started after an update by another firmware - it is validated after " +
                    String(GatewayImage::VALIDATE_AFTER_MS / 1000) + " s of normal operation");
        }
        break;
    }
    // The running record describes the running image only if this firmware
    // validated it in this partition and nothing else installed one since:
    // the bootloader state is VALID (a USB flash resets otadata), and the
    // image was not installed by another firmware (same version text in the
    // same partition would pass the other checks)
    const bool runningValid = running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_VALID;
    if (state.running.set &&
        (!runningValid || strcmp(state.running.part, running->label) != 0 || boot == GatewayImage::Boot::NotRunning ||
         (boot == GatewayImage::Boot::Nothing && verifying)))
    {
        state.running = UpdateState::Image();
        saveLocked();
    }
    // An image written and left waiting for a confirmation (never activated):
    // erased so it can never boot - unless it runs or otadata starts it. The
    // record is acted on only if every earlier save of this run succeeded
    // (else it is kept for a later start), never while this image is still
    // being validated (the other slot is its rollback target) or for a slot
    // otadata marks VALID; erased first, then the record cleared
    if (state.written.set)
    {
        const esp_partition_t *wp =
            state.written.part[0]
                ? esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, state.written.part)
                : nullptr;
        esp_ota_img_states_t wst;
        const bool slotValid = wp && esp_ota_get_state_partition(wp, &wst) == ESP_OK && wst == ESP_OTA_IMG_VALID;
        if (saveFailed)
        {
            // Kept for a later start that can save
            logWarning("Update state could not be saved - the record of an image left waiting for a confirmation "
                       "is not trusted, nothing erased");
        }
        else
        {
            if (GatewayImage::leftImageErased(wp != nullptr, samePart(wp, running),
                                              samePart(wp, esp_ota_get_boot_partition()), verifying, slotValid))
            {
                eraseHead(wp);
                logWarning("Firmware image written but not confirmed before the restart: slot " + String(wp->label) +
                           " erased - install it again to use it");
                setLast("not activated: the gateway restarted before the confirmation");
            }
            else if (wp && !samePart(wp, running) && (verifying || slotValid))
            {
                logInfo("Outdated record of an image left waiting in slot " + String(wp->label) + " ignored (" +
                        (verifying ? "the running firmware is still being validated"
                                   : "the slot holds a validated image") +
                        ")");
            }
            state.written = UpdateState::Image();
            saveLocked();
        }
    }
}

void process(bool webUp, bool healthy)
{
    if (waitingFlag)
    {
        // Never blocks the loop behind a flash write of the web task
        std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
        if (lock.owns_lock())
        {
            expireWaitingLocked();
        }
    }
    if (!pendingVerify)
    {
        return;
    }
    bool due;
    {
        std::lock_guard<std::mutex> l(valMtx);
        if (!validation.done())
        {
            due = validation.step(millis(), webUp, healthy);
        }
        else
        {
            due = markFailures > 0 && markFailures < MARK_TRIES && (int32_t)(millis() - markRetryAt) >= 0;
        }
    }
    if (due)
    {
        markValid("after 120 s of normal operation");
    }
}

void beforeRestart(const char *why)
{
    if (pendingVerify)
    {
        markValid((String("before a restart from the ") + why).c_str());
    }
}

bool validationPending(uint32_t *leftS, bool *failed)
{
    const bool p = pendingVerify;
    if (leftS || failed)
    {
        std::lock_guard<std::mutex> l(valMtx);
        if (leftS)
        {
            *leftS = p ? validation.leftS(millis()) : 0;
        }
        if (failed)
        {
            *failed = p && markFailures >= MARK_TRIES; // markValid() gave up
        }
    }
    return p;
}

String validationRefusal()
{
    uint32_t left = 0;
    bool failed = false;
    if (!validationPending(&left, &failed))
    {
        return "";
    }
    if (failed)
    {
        return "the running firmware v" + String(FIRMWARE_VERSION) +
               " could not be marked valid: validation failed - the next restart rolls back to the previous firmware";
    }
    if (left == 0)
    {
        // The 120 s are over; marking it valid failed and is retried
        return "the running firmware v" + String(FIRMWARE_VERSION) +
               " is being marked valid (retrying) - try again in a minute";
    }
    return "the running firmware v" + String(FIRMWARE_VERSION) + " is still being validated (about " + String(left) +
           " s left) - try again after that";
}

String installRefusal()
{
    String v = validationRefusal();
    if (v.length() > 0)
    {
        return v;
    }
    std::lock_guard<std::mutex> lock(mtx);
    if (activated)
    {
        return "a firmware update was just activated - the gateway restarts";
    }
    if (writing)
    {
        return "another firmware update is running";
    }
    return "";
}

void signedOnlyOn()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (waiting.valid && !waiting.signedImage)
    {
        loadLocked();
        setLast("not activated: \"Signed images only\" was switched on");
        dropWaitingLocked("\"Signed images only\" was switched on");
    }
}

// --- Writer ----------------------------------------------------------------------------

Writer::Writer() = default;

Writer::~Writer()
{
    abort();
}

void Writer::releaseLocked()
{
    handle_ = 0;
    st_.reset();
    if (owns_)
    {
        owns_ = false;
        writing = false;
    }
}

void Writer::release()
{
    std::lock_guard<std::mutex> lock(mtx);
    releaseLocked();
}

String Writer::begin()
{
    String refusal = installRefusal();
    if (refusal.length() > 0)
    {
        return refusal;
    }
    const esp_partition_t *p = esp_ota_get_next_update_partition(nullptr);
    if (!p)
    {
        return "no OTA partition";
    }
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (activated)
        {
            return "a firmware update was just activated - the gateway restarts";
        }
        if (writing)
        {
            return "another firmware update is running";
        }
        // Erased and its record cleared now: a write that never reaches the
        // slot (wrong board, too short, cut transfer, URL failure) must not
        // leave the old unconfirmed image bootable without a record
        dropWaitingLocked("another install writes the slot");
        writing = true;
        owns_ = true;
        part_ = p; // only once the slot is claimed (a refused writer names no slot)
    }
    st_.reset(new (std::nothrow) State());
    if (!st_)
    {
        release();
        return "not enough memory for the firmware check";
    }
    esp_ota_handle_t h = 0;
    esp_err_t err;
    {
        FlashWriteGuard guard;
        // Sequential: each sector is erased when the write reaches it (no
        // multi-second erase of the whole slot up front)
        err = esp_ota_begin(part_, OTA_WITH_SEQUENTIAL_WRITES, &h);
    }
    if (err != ESP_OK)
    {
        release();
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE)
        {
            return "the running firmware is still being validated - install again in two minutes";
        }
        return String("esp_ota_begin failed (") + esp_err_to_name(err) + ")";
    }
    handle_ = h;
    received_ = 0;
    headLen_ = 0;
    haveOutcome_ = false;
    outcome_ = GatewayImage::Outcome();
    return "";
}

String Writer::writeFlash(const uint8_t *data, size_t len)
{
    esp_err_t err;
    {
        FlashWriteGuard guard;
        err = esp_ota_write(handle_, data, len);
    }
    if (err != ESP_OK)
    {
        abort();
        return String("flash write failed (") + esp_err_to_name(err) + ")";
    }
    return "";
}

String Writer::write(const uint8_t *data, size_t len)
{
    if (!handle_ || haveOutcome_)
    {
        return "no firmware write in progress";
    }
    if (len == 0)
    {
        return "";
    }
    if (received_ + len > part_->size)
    {
        abort();
        return "Data exceed OTA slot size";
    }
    received_ += len;
    st_->stream.feed(data, len, st_->hash);
    // The image header is checked against this chip before anything is written
    if (headLen_ < GatewayImage::HEADER_SIZE)
    {
        const size_t take = std::min(GatewayImage::HEADER_SIZE - headLen_, len);
        memcpy(head_ + headLen_, data, take);
        headLen_ += take;
        data += take;
        len -= take;
        if (headLen_ < GatewayImage::HEADER_SIZE)
        {
            return "";
        }
        switch (GatewayImage::checkHead(head_, headLen_, CONFIG_IDF_FIRMWARE_CHIP_ID))
        {
        case GatewayImage::HeadError::None:
            break;
        case GatewayImage::HeadError::OtherChip:
            abort();
            return "Firmware is for a different board (" BOARD_NAME " expected)";
        default:
            abort();
            return "Not an ESP32 firmware image";
        }
        String e = writeFlash(head_, headLen_);
        if (e.length() > 0)
        {
            return e;
        }
    }
    return len ? writeFlash(data, len) : String();
}

GatewayImage::Outcome Writer::finish(uint64_t announced)
{
    if (haveOutcome_)
    {
        return outcome_;
    }
    haveOutcome_ = true;
    if (!handle_ || !st_)
    {
        snprintf(outcome_.text, sizeof(outcome_.text), "no firmware write in progress");
        return outcome_;
    }
    if (headLen_ < GatewayImage::HEADER_SIZE)
    {
        abort();
        snprintf(outcome_.text, sizeof(outcome_.text), "file too short for a firmware image");
        return outcome_;
    }
    outcome_ = st_->stream.finish(st_->hash, announced, BOARD_SIGN_TARGET);
    st_.reset(); // the hash context (the Pro's SHA engine) is free before the image check
    if (outcome_.cls == GatewayImage::Class::Refused)
    {
        eraseHead(part_);
        abort();
        return outcome_;
    }
    esp_err_t err;
    {
        FlashWriteGuard guard;
        err = esp_ota_end(handle_); // the image check (segments, checksum, SHA-256); frees the handle
    }
    handle_ = 0;
    if (err != ESP_OK)
    {
        eraseHead(part_);
        release();
        outcome_.cls = GatewayImage::Class::Refused;
        outcome_.severe = false;
        snprintf(outcome_.text, sizeof(outcome_.text), "the firmware image is damaged or incomplete (%s)",
                 esp_err_to_name(err));
        return outcome_;
    }
    // writing stays set until conclude() decided (no other writer meanwhile)
    return outcome_;
}

void Writer::abort()
{
    if (handle_)
    {
        esp_ota_abort(handle_);
    }
    release();
}

Result conclude(Writer &w, const GatewayImage::Outcome &o, uint64_t session, bool signedOnly, const char *source)
{
    Result r;
    GatewayImage::digestHex(o.digest, r.digestHex);
    const uint32_t runningVersion = GatewayImage::runningVersionOf(FIRMWARE_VERSION);
    r.action = GatewayImage::activation(o.cls, signedOnly, o.fwVersion, runningVersion);
    const bool downgrade = GatewayImage::isDowngrade(o.cls, o.fwVersion, runningVersion);
    const esp_partition_t *part = w.partition();
    // The decision is applied with the slot still claimed: the slot is
    // released inside the same locked section (no other writer can start on
    // it before the boot partition is set or the waiting image registered)
    std::lock_guard<std::mutex> lock(mtx);
    struct Release
    {
        Writer &w;
        ~Release() { w.releaseLocked(); }
    } release{w};
    loadLocked();
    if (!w.owns_ && r.action != GatewayImage::Activation::Refuse)
    {
        r.action = GatewayImage::Activation::Refuse;
        r.message = "Firmware refused: the write did not complete";
        logError("Firmware " + String(source) + ": " + r.message);
        return r;
    }
    if (r.action == GatewayImage::Activation::Refuse && o.cls != GatewayImage::Class::Refused && w.owns_)
    {
        eraseHead(part); // a valid image, not activated: it must not boot either
    }
    switch (r.action)
    {
    case GatewayImage::Activation::Activate:
    {
        String err = activateLocked(part, o.cls, o.keyId, o.fwVersion, String(source));
        if (err.length() > 0)
        {
            r.message = "Firmware " + String(o.text) + " but " + err;
            logError("Firmware " + String(source) + ": " + r.message);
            r.action = GatewayImage::Activation::Refuse;
            return r;
        }
        r.activated = true;
        r.message = "Firmware v" + versionText(o.fwVersion) + " " + o.text +
                    (o.fwVersion == runningVersion ? " (the same version - reinstalled)" : "") +
                    " - activated, the gateway restarts";
        logInfo("Firmware " + String(source) + ": " + r.message);
        return r;
    }
    case GatewayImage::Activation::Confirm:
        waiting = GatewayImage::Pending();
        waiting.valid = true;
        memcpy(waiting.digest, o.digest, 32);
        waiting.length = o.length;
        waiting.session = session;
        waiting.createdMs = millis();
        waiting.signedImage = downgrade;
        waitingPart = part;
        waitingCls = o.cls;
        waitingKey = o.keyId;
        waitingVersion = o.fwVersion;
        waitingFlag = true;
        state.written = UpdateState::Image();
        state.written.set = true;
        UpdateState::copyText(state.written.part, sizeof(state.written.part), part ? part->label : "");
        state.written.at = unixNow();
        if (downgrade && runningVersion == GatewayImage::VERSION_UNKNOWN)
        {
            r.message = "Firmware image " + String(o.text) + " is v" + versionText(o.fwVersion) +
                        "; the running version " + FIRMWARE_VERSION + " is not maj.min.pat, so it cannot be compared"
                        " - written but not activated; confirm within 10 minutes to install it";
        }
        else if (downgrade)
        {
            r.message = "Firmware image " + String(o.text) + " is an older version v" + versionText(o.fwVersion) +
                        " (running v" + FIRMWARE_VERSION + ") - written but not activated; confirm within 10 minutes "
                        "to downgrade";
        }
        else
        {
            r.message = "Firmware image " + String(o.text) + " (" + String((unsigned long)o.length) +
                        " B) written but not activated - confirm it within 10 minutes to install it";
        }
        logWarning("Firmware " + String(source) + ": " + r.message + " (SHA-256 " + String(r.digestHex).substring(0, 16) +
                   "...)");
        setLast(String(downgrade ? (runningVersion == GatewayImage::VERSION_UNKNOWN
                                        ? "waiting for confirmation (v" + versionText(o.fwVersion) +
                                              ", cannot compare): "
                                        : "waiting for confirmation (downgrade to v" + versionText(o.fwVersion) + "): ")
                                 : String("waiting for confirmation: ")) +
                o.text + ", " + source);
        saveLocked();
        return r;
    case GatewayImage::Activation::Refuse:
    default:
        if (o.cls == GatewayImage::Class::Refused)
        {
            r.message = "Firmware refused: " + String(o.text);
        }
        else
        {
            r.message = "Firmware refused: " + String(o.text) +
                        " image - \"Signed images only\" is on (Firmware page, Advanced mode)";
        }
        if (o.severe)
        {
            logError("Firmware " + String(source) + ": " + r.message);
        }
        else
        {
            logWarning("Firmware " + String(source) + ": " + r.message);
        }
        setLast(String("refused (") + source + "): " +
                (o.cls == GatewayImage::Class::Refused ? o.text : "unsigned, \"Signed images only\" on"));
        saveLocked();
        return r;
    }
}

void discard(Writer &w)
{
    eraseHead(w.partition());
    w.abort();
}

String confirm(const char *digestHex, uint64_t session, bool signedOnly)
{
    uint8_t digest[32];
    if (!GatewayImage::parseDigest(digestHex, digest))
    {
        return "no image named (64 hex digits of its SHA-256 expected)";
    }
    std::lock_guard<std::mutex> lock(mtx);
    loadLocked();
    expireWaitingLocked();
    GatewayImage::Confirm c = GatewayImage::checkConfirm(waiting, digest, session, millis(), signedOnly);
    if (c == GatewayImage::Confirm::None && expiredKnown && memcmp(expiredDigest, digest, 32) == 0)
    {
        c = GatewayImage::Confirm::Expired; // dropped at its expiry
    }
    if (c != GatewayImage::Confirm::Ok)
    {
        if (c == GatewayImage::Confirm::Expired || c == GatewayImage::Confirm::SignedOnly)
        {
            if (waiting.valid)
            {
                setLast(c == GatewayImage::Confirm::Expired
                            ? "not activated: the confirmation expired (10 minutes)"
                            : "not activated: \"Signed images only\" is on");
            }
            dropWaitingLocked(GatewayImage::confirmText(c));
        }
        logWarning(String("Firmware confirmation refused: ") + GatewayImage::confirmText(c));
        return GatewayImage::confirmText(c);
    }
    if (activated)
    {
        return "a firmware update was just activated - the gateway restarts";
    }
    if (writing)
    {
        return "another firmware update is running";
    }
    const esp_partition_t *part = waitingPart;
    const GatewayImage::Class cls = waitingCls;
    const uint32_t key = waitingKey;
    const uint32_t version = waitingVersion;
    const bool downgrade = waiting.signedImage;
    waiting = GatewayImage::Pending(); // one confirmation, one activation
    waitingPart = nullptr;
    waitingFlag = false;
    const bool unknownRunning = GatewayImage::runningVersionOf(FIRMWARE_VERSION) == GatewayImage::VERSION_UNKNOWN;
    String err = activateLocked(part, cls, key, version,
                                downgrade && !unknownRunning ? "downgrade confirmed by the admin"
                                                             : "confirmed by the admin");
    if (err.length() > 0)
    {
        logError("Firmware confirmation: " + err);
        return err;
    }
    char hex[65];
    GatewayImage::digestHex(digest, hex);
    logWarning(String(downgrade ? (unknownRunning ? "Verified firmware v" + versionText(version) + " (cannot compare)"
                                                  : "Older verified firmware v" + versionText(version))
                                : String("Unsigned firmware")) +
               " image (SHA-256 " + String(hex).substring(0, 16) +
               "...) activated after the admin's confirmation - the gateway restarts");
    return "";
}

View view(uint64_t session)
{
    View v;
    const esp_partition_t *running = esp_ota_get_running_partition();
    v.validating = validationPending(&v.validateLeftS, &v.validateFailed);
    std::lock_guard<std::mutex> lock(mtx);
    loadLocked();
    if (v.validating && state.pending.set && running && strcmp(state.pending.part, running->label) == 0)
    {
        v.runningSig = sigOf(state.pending);
    }
    else if (state.running.set && running && strcmp(state.running.ver, FIRMWARE_VERSION) == 0 &&
             strcmp(state.running.part, running->label) == 0)
    {
        v.runningSig = sigOf(state.running);
    }
    else
    {
        v.runningSig = "unknown (installed over USB or by an older firmware)";
    }
    // An expired image is dropped (and erased) here too; a confirmation
    // attempt for it still answers "expired"
    expireWaitingLocked();
    if (waiting.valid)
    {
        const uint32_t left = GatewayImage::confirmLeftMs(waiting, millis());
        char hex[65];
        GatewayImage::digestHex(waiting.digest, hex);
        v.waiting = true;
        v.waitingMine = session != 0 && session == waiting.session;
        v.waitingDowngrade = waiting.signedImage;
        v.waitingDigest = hex;
        v.waitingLeftS = left / 1000;
        String kind;
        if (waiting.signedImage)
        {
            char key[12];
            snprintf(key, sizeof(key), "%08lx", (unsigned long)waitingKey);
            kind = "verified (key " + String(key) + "), " +
                   (GatewayImage::runningVersionOf(FIRMWARE_VERSION) == GatewayImage::VERSION_UNKNOWN
                        ? "v" + versionText(waitingVersion) + " (the running version " + FIRMWARE_VERSION +
                              " cannot be compared)"
                        : "older version v" + versionText(waitingVersion) + " than the running v" +
                              FIRMWARE_VERSION);
        }
        else
        {
            kind = waitingCls == GatewayImage::Class::NoKey ? "unsigned (unknown key)" : "unsigned";
        }
        v.waitingText = kind + ", " + String((unsigned long)waiting.length) + " B, SHA-256 " +
                        String(hex).substring(0, 16) + "...";
    }
    v.lastText = state.lastText;
    return v;
}

void printDiagJson(Print &out)
{
    View v = view(0);
    out.print("{\"version\":");
    jsonText(out, FIRMWARE_VERSION);
    out.print(",\"installed\":");
    jsonText(out, v.runningSig.c_str());
    out.printf(",\"validating\":%s,\"validateLeftS\":%lu,\"validateFailed\":%s,\"waitingConfirm\":%s,"
               "\"confirmLeftS\":%lu,\"last\":",
               v.validating ? "true" : "false", (unsigned long)v.validateLeftS, v.validateFailed ? "true" : "false",
               v.waiting ? "true" : "false", (unsigned long)v.waitingLeftS);
    jsonText(out, v.lastText.c_str());
    out.print("}");
}
} // namespace GatewayOta
