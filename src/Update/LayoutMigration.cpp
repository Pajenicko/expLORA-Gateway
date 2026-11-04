/**
 * expLORA Gateway
 *
 * LayoutMigration - see LayoutMigration.h and PartitionTable.h
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

#include "LayoutMigration.h"

#include <atomic>
#include <esp_flash.h>
#include <esp_rom_md5.h>
#include <memory>
#include <new>

#include "../Data/Logging.h"
#include "../config.h"
#include "PartitionTable.h"

#ifdef LAYOUT_MIGRATION
#include <LittleFS.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "../Data/SensorManager.h"
#include "../Hardware/FlashPermit.h"
#include "GatewayOta.h"
#include "UpdateMode.h"

extern SensorManager *sensorManager;
#endif

namespace LayoutMigration
{
namespace
{
using namespace PartitionTable;

Layout layout = Layout::Damaged;
// The text for /diag/heap and the Firmware page: the layout, and why it
// is not migrated when it should be
char shown[128] = "unknown";
// Set only by a failed table write (restartRefusal()); any task reads it
std::atomic<bool> held{false};
// At most 73 characters (the automatic update's "deferred: " note)
constexpr const char *HOLD_TEXT = "partition table write failed - a restart may not boot (reflash over USB)";

void md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    md5_context_t c;
    esp_rom_md5_init(&c);
    esp_rom_md5_update(&c, data, len);
    esp_rom_md5_final(out, &c);
}

void setShown(const char *note)
{
    snprintf(shown, sizeof(shown), "%s%s%s", layoutText(layout), note ? " - " : "", note ? note : "");
}

#ifdef LAYOUT_MIGRATION
constexpr const char *MARKER = "/update/layout.txt";
constexpr uint32_t CHUNK = 4096;
constexpr uint32_t CHECK_MS = 5000;

constexpr uint32_t RETRY_MS = 600000; // write-back of the v1 table while held
constexpr uint8_t RETRY_MAX = 36;

bool waitValidation = false; // process(): restart once the running image is validated
bool restarting = false;
uint32_t lastCheckMs = 0;
// held: the v1 table to write back, the attempts from process() so far
std::unique_ptr<uint8_t[]> heldTable;
uint8_t heldTries = 0;
uint32_t heldLastMs = 0;

bool readMarker(Marker &m)
{
    m = Marker();
    File f = LittleFS.open(MARKER, "r");
    if (!f)
    {
        return false;
    }
    char t[32];
    const size_t n = f.readBytes(t, sizeof(t) - 1);
    f.close();
    t[n] = 0;
    return parseMarker(t, n, m);
}

// Written and read back: a step runs only with its marker on the flash
bool writeMarker(const Marker &m)
{
    char t[32];
    const size_t n = formatMarker(m, t, sizeof(t));
    if (n == 0)
    {
        return false;
    }
    {
        FlashWriteGuard guard;
        if (!LittleFS.exists("/update"))
        {
            LittleFS.mkdir("/update");
        }
        File f = LittleFS.open(MARKER, "w");
        if (!f)
        {
            return false;
        }
        const bool ok = f.write((const uint8_t *)t, n) == n;
        f.close();
        if (!ok)
        {
            return false;
        }
    }
    Marker back;
    return readMarker(back) && back.stage == m.stage && back.tries == m.tries &&
           strcmp(back.version, m.version) == 0;
}

void removeMarker()
{
    FlashWriteGuard guard;
    LittleFS.remove(MARKER);
}

void restartNow()
{
    Logger::logf(LogLevel::INFO, "Partition layout migration: restarting");
    Serial.flush();
    delay(200);
    esp_restart();
}

// The app image in a partition: its length (header, segments, checksum,
// appended SHA-256), verified. 0: no valid image
uint32_t imageLen(const esp_partition_t *p)
{
    if (!p)
    {
        return 0;
    }
    const esp_partition_pos_t pos = {p->address, p->size};
    esp_image_metadata_t md = {};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) == ESP_OK ? md.image_len : 0;
}

// The first len bytes of both partitions are equal
bool sameBytes(const esp_partition_t *a, const esp_partition_t *b, uint32_t len)
{
    std::unique_ptr<uint8_t[]> x(new (std::nothrow) uint8_t[CHUNK]), y(new (std::nothrow) uint8_t[CHUNK]);
    if (!x || !y)
    {
        return false;
    }
    for (uint32_t at = 0; at < len; at += CHUNK)
    {
        const uint32_t n = len - at < CHUNK ? len - at : CHUNK;
        if (esp_partition_read(a, at, x.get(), n) != ESP_OK || esp_partition_read(b, at, y.get(), n) != ESP_OK ||
            memcmp(x.get(), y.get(), n) != 0)
        {
            return false;
        }
        delay(1);
    }
    return true;
}

// Step Copy: the running image (app1) into app0, verified, made the boot
// partition. false: why says what failed (app0 may be partly written; the
// running app1 and otadata are untouched then)
bool copyToApp0(const esp_partition_t *src, const esp_partition_t *dst, const char *&why)
{
    const uint32_t len = imageLen(src);
    if (len == 0 || len > dst->size)
    {
        why = len == 0 ? "the running image could not be verified" : "the running image is larger than app0";
        return false;
    }
    const uint32_t span = (len + CHUNK - 1) / CHUNK * CHUNK;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[CHUNK]);
    if (!buf)
    {
        why = "no memory";
        return false;
    }
    {
        FlashWriteGuard guard;
        for (uint32_t at = 0; at < span; at += 0x10000)
        {
            const uint32_t n = span - at < 0x10000 ? span - at : 0x10000;
            if (esp_partition_erase_range(dst, at, n) != ESP_OK)
            {
                why = "erasing app0 failed";
                return false;
            }
            delay(1); // setup() is not watched; lets the idle tasks run
        }
        for (uint32_t at = 0; at < len; at += CHUNK)
        {
            const uint32_t n = len - at < CHUNK ? len - at : CHUNK;
            if (esp_partition_read(src, at, buf.get(), n) != ESP_OK ||
                esp_partition_write(dst, at, buf.get(), n) != ESP_OK)
            {
                why = "writing app0 failed";
                return false;
            }
            delay(1); // setup() is not watched; lets the idle tasks run
        }
    }
    if (!sameBytes(src, dst, len) || imageLen(dst) != len)
    {
        why = "the copy in app0 does not verify";
        return false;
    }
    esp_err_t err;
    {
        FlashWriteGuard guard; // otadata
        err = esp_ota_set_boot_partition(dst);
    }
    if (err != ESP_OK)
    {
        why = "esp_ota_set_boot_partition failed";
        return false;
    }
    return true;
}

// Writes the table sector (erase + write with the driver's protection of
// the table region lifted for this call only: the prebuilt framework has
// CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS, so esp_flash_* would abort() on
// 0x8000) and reads it back. true: the flash holds exactly sector
bool writeSector(const uint8_t *sector, uint8_t *readBack)
{
    esp_flash_t *chip = esp_flash_default_chip;
    if (!chip || !chip->os_func)
    {
        return false;
    }
    // The table region check is the only change: every other hook (cache
    // and interrupt guard, other-CPU stall, delays) is the driver's own
    static esp_flash_os_functions_t unguarded;
    const esp_flash_os_functions_t *own = chip->os_func;
    unguarded = *own;
    unguarded.region_protected = nullptr;
    esp_err_t e1, e2;
    {
        FlashWriteGuard guard;
        chip->os_func = &unguarded;
        e1 = esp_flash_erase_region(chip, OFFSET, SECTOR);
        e2 = e1 == ESP_OK ? esp_flash_write(chip, sector, OFFSET, SECTOR) : e1;
        chip->os_func = own;
    }
    return e1 == ESP_OK && e2 == ESP_OK && esp_flash_read(chip, readBack, OFFSET, SECTOR) == ESP_OK &&
           memcmp(sector, readBack, SECTOR) == 0;
}

// The v1 table is on the flash again (after a failed v2 write): the
// migration is not tried again by this version
void writtenBack()
{
    Marker f;
    f.stage = Stage::Failed;
    strncpy(f.version, FIRMWARE_VERSION, sizeof(f.version) - 1);
    writeMarker(f);
    Logger::logf(LogLevel::ERROR, "Partition layout migration failed: the v1 table is written back - the layout stays v1");
    setShown("migration failed (table write)");
}

// Step Table. Restarts on success; returns on failure (logged)
void writeTable()
{
    std::unique_ptr<uint8_t[]> old(new (std::nothrow) uint8_t[SECTOR]), neu(new (std::nothrow) uint8_t[SECTOR]),
        back(new (std::nothrow) uint8_t[SECTOR]);
    if (!old || !neu || !back)
    {
        Logger::logf(LogLevel::ERROR, "Partition layout migration: no memory - not migrated");
        return;
    }
    if (esp_flash_read(nullptr, old.get(), OFFSET, SECTOR) != ESP_OK ||
        classify(old.get(), SECTOR, md5) != Layout::LiteV1)
    {
        Logger::logf(LogLevel::ERROR, "Partition layout migration: the table could not be read again - not migrated");
        return;
    }
    memcpy(neu.get(), old.get(), SECTOR); // the bytes after the table (0xFF) stay as they are
    build(LITE_V2, LITE_V2_N, md5, neu.get());
    if (classify(neu.get(), SECTOR, md5) != Layout::LiteV2)
    {
        Logger::logf(LogLevel::ERROR, "Partition layout migration: the new table does not verify - not migrated");
        return;
    }
    Logger::logf(LogLevel::WARNING, "Partition layout migration: writing the v2 table (do not power off)");
    Serial.flush();
    for (int attempt = 1; attempt <= 2; attempt++)
    {
        if (writeSector(neu.get(), back.get()))
        {
            Logger::logf(LogLevel::INFO, "Partition layout migration: v2 table written and read back (attempt %d)",
                         attempt);
            restartNow();
        }
        Logger::logf(LogLevel::ERROR, "Partition layout migration: the v2 table did not read back (attempt %d)",
                     attempt);
    }
    // Back to v1 (the running firmware still uses its copy in RAM)
    for (int attempt = 1; attempt <= 5; attempt++)
    {
        if (writeSector(old.get(), back.get()))
        {
            writtenBack();
            return;
        }
    }
    // Every restart the firmware decides is refused from now on, the
    // write-back is retried from process()
    heldTable = std::move(old);
    heldTries = 0;
    heldLastMs = millis();
    held = true;
    Logger::logf(LogLevel::ERROR, "Partition layout migration: the v1 table could not be written back either - "
                                  "restarts blocked, retried every %u min (else: full image over USB)",
                 (unsigned)(RETRY_MS / 60000));
    setShown("TABLE WRITE FAILED - restarts blocked, do not power off; else reflash over USB");
}

// process(), while held: one more write-back of the v1 table
void retryWriteBack()
{
    if (heldTries >= RETRY_MAX || millis() - heldLastMs < RETRY_MS)
    {
        return;
    }
    heldLastMs = millis();
    heldTries++;
    std::unique_ptr<uint8_t[]> back(new (std::nothrow) uint8_t[SECTOR]);
    if (back && writeSector(heldTable.get(), back.get()))
    {
        held = false;
        heldTable.reset();
        Logger::logf(LogLevel::WARNING, "Partition layout migration: the v1 table is written back (retry %u) - "
                                        "restarts are allowed again",
                     (unsigned)heldTries);
        writtenBack();
        return;
    }
    if (heldTries < RETRY_MAX)
    {
        Logger::logf(LogLevel::ERROR, "Partition layout migration: writing the v1 table back failed again (retry %u "
                                      "of %u) - restarts stay blocked",
                     (unsigned)heldTries, (unsigned)RETRY_MAX);
    }
    else
    {
        Logger::logf(LogLevel::ERROR, "Partition layout migration: writing the v1 table back failed %u times - no more "
                                      "retries, restarts stay blocked (recovery: a full image over USB)",
                     (unsigned)RETRY_MAX);
        setShown("TABLE WRITE FAILED - restarts blocked; reflash over USB");
    }
}

Slot slotOf(const esp_partition_t *p)
{
    if (p && p->type == ESP_PARTITION_TYPE_APP && p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        p->address == LITE_V1[2].offset)
    {
        return Slot::App0;
    }
    if (p && p->type == ESP_PARTITION_TYPE_APP && p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 &&
        p->address == LITE_V1[3].offset)
    {
        return Slot::App1;
    }
    return Slot::Other;
}

RunState stateOf(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    const esp_err_t e = p ? esp_ota_get_state_partition(p, &st) : ESP_FAIL;
    if (e == ESP_ERR_NOT_FOUND)
    {
        return RunState::Valid; // no otadata entry (blank otadata): no rollback state
    }
    if (e != ESP_OK)
    {
        return RunState::Other;
    }
    if (st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED)
    {
        return RunState::Valid;
    }
    return st == ESP_OTA_IMG_PENDING_VERIFY ? RunState::Pending : RunState::Other;
}

BootIn inputs()
{
    BootIn in;
    in.board = true;
    uint32_t phys = 0, conf = 0;
    in.flashOk = esp_flash_get_physical_size(nullptr, &phys) == ESP_OK && esp_flash_get_size(nullptr, &conf) == ESP_OK &&
                 phys >= FLASH_MIN && conf >= FLASH_MIN;
    in.layout = layout;
    const esp_partition_t *running = esp_ota_get_running_partition();
    in.running = slotOf(running);
    in.state = stateOf(running);
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    in.bootIsRunning = boot && running && boot->address == running->address;
    readMarker(in.marker);
    in.markerThisVersion = in.marker.stage == Stage::Failed && strcmp(in.marker.version, FIRMWARE_VERSION) == 0;
    in.otaRecordOther = GatewayOta::activationElsewhere();
    return in;
}

// Step Accept: the running copy in app0 equals the validated image in app1
bool acceptCopy()
{
    const esp_partition_t *app0 = esp_ota_get_running_partition();
    const esp_partition_t *app1 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    const uint32_t len = imageLen(app0);
    if (!app1 || stateOf(app1) != RunState::Valid || len == 0 || imageLen(app1) != len || !sameBytes(app0, app1, len))
    {
        Logger::logf(LogLevel::WARNING, "Partition layout migration: the running image in app0 is not the validated "
                                        "copy - it is validated normally first");
        return false;
    }
    esp_err_t err;
    {
        FlashWriteGuard guard; // otadata
        err = esp_ota_mark_app_valid_cancel_rollback();
    }
    if (err != ESP_OK)
    {
        Logger::logf(LogLevel::WARNING, "Partition layout migration: marking the copy valid failed (%s) - it is "
                                        "validated normally first",
                     esp_err_to_name(err));
        return false;
    }
    GatewayOta::runningMoved(app1->label, app0->label);
    Logger::logf(LogLevel::INFO, "Partition layout migration: the copy in app0 runs and is the validated image");
    return true;
}

void migrate()
{
    for (int pass = 0; pass < 2; pass++)
    {
        const BootIn in = inputs();
        const Action a = decide(in);
        switch (a)
        {
        case Action::None:
            return;
        case Action::Finished:
            removeMarker();
            Logger::logf(LogLevel::WARNING, "Partition layout migrated to v2 (2752K slots): the previous firmware "
                                            "(rollback image) is gone, settings and files are kept");
            return;
        case Action::Wait:
            waitValidation = true;
            if (in.state == RunState::Pending)
            {
                setShown("migrates after the validation of this firmware");
                Logger::logf(LogLevel::INFO, "Partition layout v1: migrated to v2 once this firmware is validated");
            }
            else
            {
                // GatewayOta::begin() reports the other slot's image first
                setShown("migrates after the result of the last update is recorded");
                Logger::logf(LogLevel::INFO, "Partition layout v1: migrated to v2 at a restart once the result of the "
                                             "last firmware update is recorded");
            }
            return;
        case Action::Note:
            if (in.layout == Layout::LiteV1)
            {
                const char *n = !in.flashOk                                 ? "not migrated: flash smaller than 8 MB"
                                : in.marker.stage == Stage::Failed          ? "not migrated: the migration failed earlier"
                                : in.state != RunState::Valid               ? "not migrated: unexpected boot state"
                                                                            : "not migrated: unexpected boot slot";
                setShown(n);
                Logger::logf(LogLevel::WARNING, "Partition layout v1 %s", n);
            }
            else
            {
                setShown("no migration");
                Logger::logf(LogLevel::WARNING, "Partition table %s - no layout migration", layoutText(in.layout));
            }
            return;
        case Action::GiveUp:
        {
            Marker f;
            f.stage = Stage::Failed;
            strncpy(f.version, FIRMWARE_VERSION, sizeof(f.version) - 1);
            writeMarker(f);
            setShown("not migrated: the migration failed earlier");
            Logger::logf(LogLevel::ERROR, "Partition layout migration given up after %u attempts of the %s step - "
                                          "the layout stays v1",
                         (unsigned)MAX_TRIES, in.marker.stage == Stage::Copy ? "copy" : "table");
            return;
        }
        case Action::Accept:
            if (!acceptCopy())
            {
                waitValidation = true;
                setShown("migrates after the validation of this firmware");
                return;
            }
            continue; // inputs again: the copy is valid now -> Table
        case Action::Copy:
        case Action::Table:
        {
            const Marker m = nextMarker(in.marker, a == Action::Copy ? Stage::Copy : Stage::Table);
            if (!writeMarker(m))
            {
                setShown("not migrated: the progress file cannot be written");
                Logger::logf(LogLevel::ERROR, "Partition layout migration: %s cannot be written - not migrated",
                             MARKER);
                return;
            }
            if (a == Action::Table)
            {
                Logger::logf(LogLevel::WARNING, "Partition layout migration v1 -> v2 (attempt %u): the previous "
                                                "firmware (rollback image) is dropped",
                             (unsigned)m.tries);
                writeTable(); // restarts on success
                return;
            }
            Logger::logf(LogLevel::WARNING, "Partition layout migration v1 -> v2 (attempt %u): copying the running "
                                            "firmware from app1 to app0 (replaces the previous firmware)",
                         (unsigned)m.tries);
            const char *why = "";
            const esp_partition_t *src = esp_ota_get_running_partition();
            const esp_partition_t *dst =
                esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
            if (dst && copyToApp0(src, dst, why))
            {
                Logger::logf(LogLevel::INFO, "Partition layout migration: copy verified, starting it from app0");
                restartNow();
            }
            setShown("copy failed - tried again at the next start");
            Logger::logf(LogLevel::ERROR, "Partition layout migration: copy to app0 failed (%s) - tried again at the "
                                          "next start",
                         why);
            return;
        }
        }
    }
}
#endif
} // namespace

void begin()
{
    std::unique_ptr<uint8_t[]> t(new (std::nothrow) uint8_t[MAX_LEN]);
    layout = t && esp_flash_read(nullptr, t.get(), OFFSET, MAX_LEN) == ESP_OK ? classify(t.get(), MAX_LEN, md5)
                                                                             : Layout::Damaged;
    t.reset();
    setShown(nullptr);
#ifdef LAYOUT_MIGRATION
    migrate();
#endif
}

void process()
{
#ifdef LAYOUT_MIGRATION
    if (held)
    {
        retryWriteBack();
        return;
    }
    if (!waitValidation || restarting || millis() - lastCheckMs < CHECK_MS)
    {
        return;
    }
    lastCheckMs = millis();
    String why;
    if (GatewayOta::validationPending() || UpdateMode::active() || UpdateMode::restartWouldInterrupt() ||
        GatewayOta::waitingConfirm() || GatewayOta::installRefused(why) || GatewayOta::activationElsewhere())
    {
        return;
    }
    restarting = true;
    Logger::logf(LogLevel::WARNING, "Firmware validated - restarting for the partition layout migration");
    if (sensorManager)
    {
        sensorManager->saveSensors(); // keep the rain totals
    }
    restartNow();
#endif
}

const char *text()
{
    return shown;
}

const char *restartRefusal()
{
    return held ? HOLD_TEXT : nullptr;
}
} // namespace LayoutMigration
