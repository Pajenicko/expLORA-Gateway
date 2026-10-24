/**
 * expLORA Gateway
 *
 * OTA image store
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "OtaStore.h"
#include "OtaCommitRecovery.h"
#include "../Hardware/FlashPermit.h"
#include "../Protocol/OtaImageSigned.h"
#include "../Security/Sha256.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>
#include <mutex>
#include <new>
#include <sys/time.h>

namespace OtaStore
{

static const char *const DIR = "/ota";
static const char *const TMP = "/ota/upload.tmp";
static const char *const META_TMP = "/ota/meta.tmp"; // description of a commit in progress
static Logger *log = nullptr;
static std::recursive_mutex storeMutex; // taken after the flash permit
static File upload;
static bool uploading = false;
// Only while an upload runs (~0.4 kB heap): the split / CRC state and the
// SHA-256 context. On the Pro the context holds the hardware SHA engine
// from the first block to its release here; a TLS session meanwhile hashes
// in software (and the update mode refuses sessions during an upload)
struct UploadState
{
    OtaImageSigned::Splitter<Sha256> split;
    Sha256 hash;
};
static std::unique_ptr<UploadState> state;
static Timing times;

static String binPath(uint16_t hw)
{
    char p[24];
    snprintf(p, sizeof(p), "/ota/hw%04X.bin", hw);
    return p;
}

static String metaPath(uint16_t hw)
{
    char p[24];
    snprintf(p, sizeof(p), "/ota/hw%04X.json", hw);
    return p;
}

static String hwText(uint16_t hw)
{
    char t[8];
    snprintf(t, sizeof(t), "0x%04X", hw);
    return t;
}

// Length of the stored .bin, 0 when it is missing
static uint32_t binLen(uint16_t hw)
{
    const String p = binPath(hw);
    if (!LittleFS.exists(p))
    {
        return 0;
    }
    File f = LittleFS.open(p, "r");
    if (!f)
    {
        return 0;
    }
    const uint32_t n = f.size();
    f.close();
    return n;
}

static bool readMeta(const String &path, Meta &meta);
static bool writeMeta(const char *path, const Meta &meta);

// Drop a .bin without a readable .json, a .json without its .bin, a pair whose
// .bin length differs from its .json (a reset within remove(), or a pair cut
// by an older firmware; OtaCommitRecovery::checkPair) and any hw*.bin / hw*.json
// with a non-canonical name. job.json, job.tmp, upload.tmp and meta.tmp do not
// start with "hw" and are never touched here.
static void removeOrphans(Logger &logger)
{
    String names[2 * OtaImage::MAX_HW_TYPES + 8];
    size_t n = 0;
    File dir = LittleFS.open(DIR);
    if (!dir || !dir.isDirectory())
    {
        return;
    }
    for (File f = dir.openNextFile(); f && n < sizeof(names) / sizeof(names[0]); f = dir.openNextFile())
    {
        String name = f.name();
        f.close();
        if (name.startsWith("/"))
        {
            name = name.substring(name.lastIndexOf('/') + 1);
        }
        if (name.startsWith("hw") && (name.endsWith(".bin") || name.endsWith(".json")))
        {
            names[n++] = name;
        }
    }
    dir.close();
    for (size_t i = 0; i < n; i++)
    {
        const String base = String(DIR) + "/" + names[i].substring(0, names[i].lastIndexOf('.'));
        const uint16_t hw = (uint16_t)strtoul(names[i].substring(2, names[i].lastIndexOf('.')).c_str(), nullptr, 16);
        if (!LittleFS.exists(String(DIR) + "/" + names[i]))
        {
            continue; // removed with its pair already
        }
        const bool canonical = base + ".bin" == binPath(hw);
        Meta m;
        const bool readable = readMeta(base + ".json", m);
        const OtaCommitRecovery::Pair p =
            OtaCommitRecovery::checkPair(canonical, canonical ? binLen(hw) : 0, readable, m.hwType, hw, m.len);
        if (p == OtaCommitRecovery::Pair::Keep)
        {
            continue;
        }
        LittleFS.remove(base + ".bin");
        LittleFS.remove(base + ".json");
        switch (p)
        {
        case OtaCommitRecovery::Pair::WrongLength:
            logger.warning("OTA image store: removed the image of HW " + hwText(hw) +
                           " - its length does not match its description");
            break;
        case OtaCommitRecovery::Pair::OtherHw:
            logger.warning("OTA image store: removed the image " + names[i] +
                           " - its description is of another HW type");
            break;
        case OtaCommitRecovery::Pair::BadName:
            logger.warning("OTA image store: removed " + names[i] + " - not a valid image file name");
            break;
        default:
            logger.warning("OTA image store: removed the incomplete image " + names[i]);
            break;
        }
    }
}

// The last commit step: meta.tmp becomes the .json of the new .bin. A failed
// rename falls back to writing the .json directly (a reset meanwhile leaves
// meta.tmp, finished again at boot); meta.tmp goes only once the .json reads
// back as written (writeMeta), else the new .bin goes too, as a new .bin with
// the old .json is never kept. Under the permit and storeMutex.
static bool finishMeta(const Meta &meta)
{
    if (LittleFS.rename(META_TMP, metaPath(meta.hwType)))
    {
        return true;
    }
    if (writeMeta(metaPath(meta.hwType).c_str(), meta))
    {
        LittleFS.remove(META_TMP);
        return true;
    }
    LittleFS.remove(binPath(meta.hwType));
    LittleFS.remove(metaPath(meta.hwType));
    LittleFS.remove(META_TMP);
    return false;
}

void begin(Logger &logger)
{
    log = &logger;
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (!LittleFS.exists(DIR))
    {
        LittleFS.mkdir(DIR);
    }
    const bool uploadTmp = LittleFS.exists(TMP);
    if (uploadTmp)
    {
        LittleFS.remove(TMP);
        logger.info("OTA image store: removed an unfinished upload");
    }
    // A commit cut by a reset (only then a CRC pass at boot)
    if (LittleFS.exists(META_TMP))
    {
        Meta next;
        const bool tmpValid = readMeta(META_TMP, next);
        const uint16_t hw = next.hwType;
        auto matchesNew = [&]() { return verify(hw, next); };
        auto matchesOld = [&]()
        {
            Meta old;
            return readMeta(metaPath(hw), old) && old.hwType == hw && verify(hw, old);
        };
        switch (OtaCommitRecovery::decide(uploadTmp, tmpValid, matchesNew, matchesOld))
        {
        case OtaCommitRecovery::Action::FinishTmp:
            if (finishMeta(next))
            {
                logger.info("OTA image store: finished storing the image of HW " + hwText(hw));
            }
            else
            {
                logger.warning("OTA image store: removed the image of HW " + hwText(hw) +
                               " - cannot store its description");
            }
            break;
        case OtaCommitRecovery::Action::DropAll:
        {
            const bool had = LittleFS.exists(binPath(hw)) || LittleFS.exists(metaPath(hw));
            LittleFS.remove(binPath(hw));
            LittleFS.remove(metaPath(hw));
            LittleFS.remove(META_TMP);
            if (had)
            {
                logger.warning("OTA image store: removed the image of HW " + hwText(hw) + " cut by a reset");
            }
            else
            {
                logger.info("OTA image store: discarded an unfinished image commit");
            }
            break;
        }
        case OtaCommitRecovery::Action::DropTmp:
            LittleFS.remove(META_TMP);
            logger.info("OTA image store: discarded an unfinished image commit");
            break;
        }
    }
    removeOrphans(logger);
}

bool uploadRunning()
{
    return uploading;
}

bool uploadBegin(String &error)
{
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    // A previous upload ends here whatever follows (its file closed, its
    // state - SHA context, on the Pro the hardware engine - released)
    if (uploading)
    {
        upload.close();
    }
    uploading = false;
    state.reset();
    LittleFS.remove(TMP);
    // Room for the largest image plus the file system's own blocks
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < OtaImage::MAX_LEN + 16384)
    {
        error = "not enough flash space for an image - delete one first";
        return false;
    }
    state.reset(new (std::nothrow) UploadState());
    if (!state)
    {
        error = "not enough memory for an upload";
        return false;
    }
    upload = LittleFS.open(TMP, "w");
    if (!upload)
    {
        state.reset();
        error = "cannot create the upload file";
        return false;
    }
    uploading = true;
    return true;
}

bool uploadWrite(const uint8_t *data, size_t len, String &error)
{
    if (!uploading || !state)
    {
        error = "no upload running";
        return false;
    }
    // Only the image bytes are stored; what follows them stays in RAM
    size_t payload = 0;
    const OtaImageSigned::Early early = state->split.feed(data, len, payload, state->hash);
    if (early != OtaImageSigned::Early::None)
    {
        error = OtaImageSigned::earlyText(early);
        return false;
    }
    if (payload == 0)
    {
        return true;
    }
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (upload.write(data, payload) != payload)
    {
        error = "flash full or write error";
        return false;
    }
    return true;
}

void uploadAbort()
{
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    if (uploading)
    {
        upload.close();
    }
    uploading = false;
    state.reset(); // releases the SHA engine
    LittleFS.remove(TMP);
}

bool uploadValidate(Meta &meta, String &error, bool &severe, String &stateText)
{
    severe = false;
    {
        FlashWriteGuard guard;
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        if (!uploading || !state)
        {
            error = "no upload running";
            return false;
        }
        upload.close();
    }
    // Image, then the signature (one ECDSA verify, ~0.35 s on the Pro)
    const OtaImageSigned::Outcome o = OtaImageSigned::classify(state->split, state->hash);
    const uint32_t stored = state->split.streamer().total();
    state.reset(); // releases the SHA engine
    if (o.cls == OtaImageSigned::Class::Refused)
    {
        severe = o.severe;
        error = o.text;
        return false;
    }
    stateText = o.text;
    meta.valid = true;
    meta.hwType = o.trailer.hwType;
    meta.fw = o.trailer.fw;
    meta.len = stored;
    meta.crc = o.trailer.crc;
    meta.flags = o.trailer.flags;
    meta.signedKey = o.cls == OtaImageSigned::Class::Verified ? o.keyId : 0;
    meta.unknownKey = o.cls == OtaImageSigned::Class::NoKey ? o.keyId : 0;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    meta.uploadUnix = tv.tv_sec > 1700000000 ? (uint32_t)tv.tv_sec : 0;
    return true;
}

bool uploadCommit(const Meta &meta, String &error)
{
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    uploading = false;
    state.reset();
    // At most MAX_HW_TYPES images: a new HW type needs a free place
    Meta existing;
    if (!get(meta.hwType, existing))
    {
        Meta all[OtaImage::MAX_HW_TYPES + 1];
        if (list(all, OtaImage::MAX_HW_TYPES + 1) >= OtaImage::MAX_HW_TYPES)
        {
            LittleFS.remove(TMP);
            error = "4 HW types stored already - delete one first";
            return false;
        }
    }
    // The description to meta.tmp first, then the .bin, then the .json: a
    // reset in between is sorted out at boot (begin()), every rename replaces
    // the older file atomically (a failed rename keeps it). verify() of the
    // replaced image cannot run meanwhile: an upload is refused while a job
    // start is pending or a job uses this HW type (otaImageBusy)
    if (!writeMeta(META_TMP, meta))
    {
        LittleFS.remove(META_TMP);
        LittleFS.remove(TMP);
        error = "cannot write the image description";
        return false;
    }
    if (!LittleFS.rename(TMP, binPath(meta.hwType)))
    {
        LittleFS.remove(TMP);
        LittleFS.remove(META_TMP);
        error = "cannot store the image";
        return false;
    }
    if (!finishMeta(meta))
    {
        error = "cannot write the image description - the new and the previous image of HW " +
                hwText(meta.hwType) + " were removed";
        return false;
    }
    return true;
}

static bool writeMeta(const char *path, const Meta &meta)
{
    DynamicJsonDocument doc(OtaMeta::JSON_CAPACITY); // heap: also called on the async_tcp task
    OtaMeta::toJson(meta, doc);
    File f = LittleFS.open(path, "w");
    if (!f)
    {
        return false;
    }
    bool ok = serializeJson(doc, f) == measureJson(doc);
    f.close();
    // A short or lost write (File::write is buffered, close() reports nothing)
    // shows only when the file is read back: success means it is on flash
    Meta back;
    return ok && readMeta(path, back) && OtaMeta::sameContent(back, meta);
}

static bool readMeta(const String &path, Meta &meta)
{
    File f = LittleFS.open(path, "r");
    if (!f)
    {
        return false;
    }
    // Only the fields this firmware knows are kept: a description written by
    // a later firmware with more fields still reads (a downgraded gateway
    // must not take it for damage and delete the image)
    DynamicJsonDocument doc(OtaMeta::JSON_CAPACITY);
    StaticJsonDocument<OtaMeta::FILTER_CAPACITY> filter;
    OtaMeta::filter(filter);
    DeserializationError e = deserializeJson(doc, f, DeserializationOption::Filter(filter));
    f.close();
    if (e || doc.capacity() == 0)
    {
        return false;
    }
    if (!OtaMeta::fromJson(doc, meta))
    {
        return false;
    }
    // Verified by a key this firmware no longer carries: unsigned now
    OtaImageSigned::applyKeyTable(meta);
    return true;
}

size_t list(Meta *out, size_t max)
{
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    size_t n = 0;
    File dir = LittleFS.open(DIR);
    if (!dir || !dir.isDirectory())
    {
        return 0;
    }
    for (File f = dir.openNextFile(); f && n < max; f = dir.openNextFile())
    {
        String name = f.name();
        f.close();
        if (name.startsWith("/"))
        {
            name = name.substring(name.lastIndexOf('/') + 1);
        }
        if (name.startsWith("hw") && name.endsWith(".json"))
        {
            Meta m;
            // a .bin of another length or a .json of another HW type is no
            // image (removed at the next boot)
            if (readMeta(String(DIR) + "/" + name, m) && metaPath(m.hwType) == String(DIR) + "/" + name &&
                binLen(m.hwType) == m.len)
            {
                out[n++] = m;
            }
        }
    }
    dir.close();
    return n;
}

bool get(uint16_t hwType, Meta &meta)
{
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    // meta.valid follows the result: no stored image -> not valid
    meta.valid = readMeta(metaPath(hwType), meta) && meta.hwType == hwType && binLen(hwType) == meta.len;
    return meta.valid;
}

bool remove(uint16_t hwType)
{
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    bool had = LittleFS.exists(binPath(hwType));
    LittleFS.remove(binPath(hwType));
    LittleFS.remove(metaPath(hwType));
    return had;
}

Timing timing()
{
    return times;
}

bool read(uint16_t hwType, uint32_t offset, uint8_t *buf, size_t len)
{
    const uint32_t t0 = micros();
    struct Count
    {
        uint32_t t0;
        ~Count()
        {
            const uint32_t us = micros() - t0;
            times.reads++;
            times.readSumUs += us;
            times.readMaxUs = us > times.readMaxUs ? us : times.readMaxUs;
        }
    } count{t0};
    FlashWriteGuard guard;
    std::lock_guard<std::recursive_mutex> lock(storeMutex);
    File f = LittleFS.open(binPath(hwType), "r");
    if (!f)
    {
        return false;
    }
    bool ok = f.seek(offset) && f.read(buf, len) == len;
    f.close();
    return ok;
}

bool verify(uint16_t hwType, const Meta &meta)
{
    const uint32_t t0 = micros();
    struct Done
    {
        uint32_t t0;
        ~Done() { times.verifyUs = micros() - t0; }
    } done{t0};
    // The file stays open between the chunks without storeMutex: safe only
    // because an upload or delete of this HW type is refused while a job start
    // is pending or a job uses it (otaStartPending, otaImageBusy); at boot
    // nothing else runs yet
    File f;
    {
        FlashWriteGuard guard;
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        f = LittleFS.open(binPath(hwType), "r");
    }
    if (!f)
    {
        return false;
    }
    OtaImage::Streamer s;
    uint8_t buf[512];
    bool ok = true;
    for (uint32_t off = 0; off < meta.len && ok; off += sizeof(buf))
    {
        size_t n = meta.len - off < sizeof(buf) ? meta.len - off : sizeof(buf);
        {
            // one short permit per chunk: timed responses go on meanwhile
            FlashWriteGuard guard;
            std::lock_guard<std::recursive_mutex> lock(storeMutex);
            ok = f.read(buf, n) == n;
        }
        s.feed(buf, n);
        delay(0);
    }
    {
        FlashWriteGuard guard;
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        f.close();
    }
    OtaImage::Trailer t;
    return ok && OtaImage::validate(s, t) == OtaImage::Error::None && t.crc == meta.crc && t.fw == meta.fw &&
           t.hwType == meta.hwType && s.total() == meta.len;
}

uint32_t usedBytes()
{
    Meta all[OtaImage::MAX_HW_TYPES];
    size_t n = list(all, OtaImage::MAX_HW_TYPES);
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++)
    {
        sum += all[i].len;
    }
    return sum;
}

} // namespace OtaStore
