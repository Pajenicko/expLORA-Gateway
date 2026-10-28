/**
 * expLORA Gateway
 *
 * Manifest - the signed package manifest (M4c S5): one file
 * https://pajenicko.cz/webflash/explora/v2/packages.json that names the
 * current gateway firmware per board and the current sensor image per HW
 * type. Pure C++ (ArduinoJson + SigVerify) for the native tests
 * (test/test_manifest).
 *
 * Format (written by tools/release/explora_manifest.py):
 *
 *   EXSM1 <key id: 8 hex> <signature r || s: 128 hex>\n
 *   {"seq":42,"issued":1761200000,
 *    "gateway":{"pro": {"version":"2.0.1","url":"https://...","size":1830016,"sha256":"<64 hex>"},
 *               "lite":{...}},
 *    "sensor": {"0003":{"version":"0.2.13","url":"https://...","size":96584,"sha256":"<64 hex>"}}}
 *
 * The signature is ECDSA P-256 over SHA-256 of the exact bytes after the
 * first '\n' (the JSON, as served - nothing is normalised). Strict rules:
 * - the file is at most MAX_FILE bytes (refused from its Content-Length
 *   before the body is read);
 * - line 1 is exactly "EXSM1", one space, 8 lowercase hex digits, one
 *   space, 128 lowercase hex digits and '\n' (no '\r', no other
 *   whitespace); key id 0 is malformed; a key id this firmware does not
 *   carry or a signature that does not verify refuses the manifest - there
 *   is no unsigned manifest;
 * - the JSON after it (any JSON whitespace, CRLF too - those bytes are
 *   signed) is an object; "seq" (an integer 0..2^32-1) is required;
 *   "issued" is informational and never compared with the clock; unknown
 *   keys are ignored; bytes after the JSON value are ignored (signed
 *   anyway; explora_manifest.py does the same);
 * - "gateway" and "sensor", when present, are objects;
 * - the whole document fits the parser's memory (JSON_CAPACITY: at least
 *   MAX_NODES JSON values - members and array elements - always fit; with
 *   short strings a few more may) and nesting 10 deep (ArduinoJson's
 *   default) - far more than a 4 kB manifest with realistic fields needs.
 * Anything above refuses the manifest. The entries are checked one by one,
 * so a later release (a new board or HW type with a bigger image, a 9th
 * sensor package, fields this firmware does not know) cannot stop the
 * update check of units already in the field:
 * - a package: "version" maj.min.pat (0..255 each, nothing else), "url"
 *   as UpdateModePolicy::urlAccepted (https://, <= 256 bytes, a host of at
 *   most 127 bytes), "size" within this firmware's limits of its kind,
 *   "sha256" 64 lowercase hex digits (of the whole file as served,
 *   signature block included);
 * - sensor keys are 4 uppercase hex digits (the HW type, not 0000); the
 *   first MAX_PACKAGES valid ones are kept;
 * - an invalid entry of this board means no gateway offer (the check
 *   result names the field; the sensor packages are still used); the other
 *   board's entry and sensor entries that break a rule are skipped, each
 *   reported once (SkipFn) and never offered; a board or HW type that is
 *   missing is simply not offered.
 * Replay: seq must be >= the highest seq accepted before (equal = the same
 * manifest checked again); there is no expiry (owner decision).
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>

#include "../Security/ReleaseKeys.h"
#include "../Security/Sha256.h"
#include "../Security/SigVerify.h"
#include "GatewayImage.h"
#include "SensorFetch.h"
#include "UpdateModePolicy.h"

namespace Manifest
{
constexpr size_t MAX_FILE = 4096;
constexpr size_t HEADER_LEN = 5 + 1 + 8 + 1 + 128 + 1; // "EXSM1 kkkkkkkk ss..ss\n"
constexpr size_t MIN_FILE = HEADER_LEN + 2;            // "{}"
constexpr size_t MAX_PACKAGES = 8;
constexpr size_t URL_CAP = 257;
// The largest gateway file of any board (the Pro's 4 MB app slot): a
// format bound for every board's entry; the install checks its own slot
constexpr uint32_t GATEWAY_MAX = 0x400000;

// Time budget of the download (SensorFetch::budgetMs rules): 20 s plus the
// file at 2 kB/s - 22 s for the largest
constexpr uint32_t budgetMs(uint32_t total)
{
    return 20000 + (uint32_t)(((uint64_t)total * 1000 + 2047) / 2048);
}

// Error is 0: a value-initialised result never reads as Ok
enum class Result : uint8_t
{
    Error = 0, // hash / mbedtls failure, no memory
    Ok,
    TooLarge,  // more than MAX_FILE bytes
    Header,    // line 1 malformed
    KeyIdZero, // key id 0
    NoKey,     // a key id this firmware does not carry
    BadSig,    // the signature does not verify
    Json,      // not a JSON object / too complex
    Field,     // seq missing or invalid, "gateway" / "sensor" not an object (why names it)
    Replay,    // seq below the highest accepted one
};

inline const char *resultText(Result r)
{
    switch (r)
    {
    case Result::Error:
        return "check failed (memory or hash)";
    case Result::Ok:
        return "ok";
    case Result::TooLarge:
        return "manifest too large";
    case Result::Header:
        return "malformed signature line";
    case Result::KeyIdZero:
        return "key id 0";
    case Result::NoKey:
        return "signed with an unknown key";
    case Result::BadSig:
        return "bad signature";
    case Result::Json:
        return "not a valid manifest (JSON)";
    case Result::Field:
        return "invalid manifest field";
    case Result::Replay:
        return "older than the last accepted manifest (replay)";
    }
    return "?";
}

struct Package
{
    uint32_t version = 0; // maj << 16 | min << 8 | pat
    uint32_t size = 0;    // bytes of the whole file
    uint8_t sha256[32] = {};
    char url[URL_CAP] = "";
};

struct SensorPackage
{
    uint16_t hw = 0;
    Package pkg;
};

struct Doc
{
    uint32_t seq = 0;
    uint32_t keyId = 0;
    bool hasGateway = false;
    bool gatewayInvalid = false; // this board's entry is there but breaks a rule (not offered)
    Package gateway; // this board's
    uint16_t skipped = 0; // the other board's / sensor entries skipped (saturates)
    uint8_t sensorCount = 0;
    SensorPackage sensor[MAX_PACKAGES];

    const Package *sensorFor(uint16_t hw) const
    {
        for (uint8_t i = 0; i < sensorCount; i++)
        {
            if (sensor[i].hw == hw)
            {
                return &sensor[i].pkg;
            }
        }
        return nullptr;
    }
};
// ~2.8 kB: emptied in place (never a temporary on a task stack)
static_assert(std::is_trivially_copyable<Doc>::value, "Doc is cleared with memset");
inline void clear(Doc &d)
{
    memset(&d, 0, sizeof(d));
}

// Size limits per kind (whole file)
struct Limits
{
    uint32_t gatewayMin, gatewayMax;
    uint32_t sensorMin, sensorMax;
};

// This firmware's limits (explora_manifest.py GATEWAY_MIN .. SENSOR_MAX
// mirror them; test_manifest checks both agree): a gateway file holds at
// least the image header, one byte and the signature block; a sensor file
// as SensorFetch accepts it
constexpr Limits FIRMWARE_LIMITS = {(uint32_t)(GatewayImage::HEADER_SIZE + 1 + SigBlock::SIZE), GATEWAY_MAX,
                                    SensorFetch::MIN_FILE, SensorFetch::MAX_FILE};

inline int hexValue(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1;
}

// n lowercase hex digits -> n / 2 bytes; false on anything else
inline bool hexBytes(const char *s, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i += 2)
    {
        const int a = hexValue(s[i]), b = hexValue(s[i + 1]);
        if (a < 0 || b < 0)
        {
            return false;
        }
        out[i / 2] = (uint8_t)(a << 4 | b);
    }
    return true;
}

// Line 1. false: malformed
inline bool parseHeader(const char *file, size_t len, uint32_t &keyId, uint8_t sig[64])
{
    if (len < HEADER_LEN || memcmp(file, "EXSM1 ", 6) != 0 || file[14] != ' ' || file[HEADER_LEN - 1] != '\n')
    {
        return false;
    }
    uint8_t k[4];
    if (!hexBytes(file + 6, 8, k) || !hexBytes(file + 15, 128, sig))
    {
        return false;
    }
    keyId = (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | k[3];
    return true;
}

// The signature line against the JSON after it. keyId: the key named
// (also on a refusal, 0 when the line is malformed)
inline Result verify(const char *file, size_t len, uint32_t &keyId, const ReleaseKeys::Key *keys = ReleaseKeys::KEYS,
                     size_t keyCount = ReleaseKeys::SLOTS)
{
    keyId = 0;
    if (len > MAX_FILE)
    {
        return Result::TooLarge;
    }
    uint8_t sig[64];
    if (!parseHeader(file, len, keyId, sig))
    {
        return Result::Header;
    }
    if (keyId == 0)
    {
        return Result::KeyIdZero;
    }
    const ReleaseKeys::Key *key = nullptr;
    for (size_t i = 0; keys && i < keyCount; i++)
    {
        if (keys[i].keyId != 0 && keys[i].keyId == keyId)
        {
            key = &keys[i];
        }
    }
    if (!key)
    {
        return Result::NoKey;
    }
    Sha256 sha;
    sha.update((const uint8_t *)file + HEADER_LEN, len - HEADER_LEN);
    uint8_t digest[32];
    if (!sha.finish(digest))
    {
        return Result::Error;
    }
    switch (SigVerify::verifyDigest(key->point, digest, sig))
    {
    case SigVerify::Result::Ok:
        return Result::Ok;
    case SigVerify::Result::BadSig:
        return Result::BadSig;
    default:
        return Result::Error;
    }
}

// One package object. false: why names the field
inline bool parsePackage(JsonVariantConst v, uint32_t minSize, uint32_t maxSize, Package &out, const char *where,
                         char *why, size_t whyCap)
{
    const char *field;
    const char *ver = v["version"];
    const char *url = v["url"];
    const char *sha = v["sha256"];
    if (!v.is<JsonObjectConst>())
    {
        field = " (not an object)";
    }
    else if (!ver || !GatewayImage::parseVersion(ver, out.version))
    {
        field = ".version";
    }
    else if (!url || !UpdateModePolicy::urlAccepted(url) || strlen(url) >= URL_CAP)
    {
        field = ".url";
    }
    else if (!v["size"].is<uint32_t>() || v["size"].as<uint32_t>() < minSize || v["size"].as<uint32_t>() > maxSize)
    {
        field = ".size";
    }
    else if (!sha || strlen(sha) != 64 || !hexBytes(sha, 64, out.sha256))
    {
        field = ".sha256";
    }
    else
    {
        out.size = v["size"].as<uint32_t>();
        snprintf(out.url, sizeof(out.url), "%s", url);
        return true;
    }
    snprintf(why, whyCap, "%s%s", where, field);
    return false;
}

// "0003" -> 3; false unless 4 uppercase hex digits, not 0000
inline bool parseHw(const char *k, uint16_t &hw)
{
    if (!k || strlen(k) != 4)
    {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        const char c = k[i];
        const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0)
        {
            return false;
        }
        v = v << 4 | (uint32_t)d;
    }
    hw = (uint16_t)v;
    return v != 0;
}

// The document (on the heap, only while parsing): the strings (copied and
// deduplicated: at most the file) and one slot per JSON value below the
// root (object member or array element). A manifest package takes 5 with
// its key, so 384 leave room for ~70 packages or many fields this firmware
// does not know - a 4 kB file holds ~25 realistic packages. The bound is
// memory, not a count: 384 values always fit (the strings take at most the
// file), a body with short strings may parse a few more; one that does not
// fit is refused as "too many entries". explora_manifest.py refuses more
// than 384 values (stricter: what it accepts every gateway parses).
// ~10 kB on the ESP32 (16 B per slot)
constexpr size_t MAX_NODES = 384;
constexpr size_t JSON_CAPACITY = MAX_FILE + JSON_OBJECT_SIZE(MAX_NODES);

// An entry skipped: "sensor.0009 (more than 8 sensor packages)",
// "gateway.lite.size", ... (the caller logs it)
using SkipFn = void (*)(const char *what);

// A sensor key for messages: at most 8 characters, anything but letters
// and digits shown as '?' (the key comes from the manifest)
inline void keyText(const char *k, char *out, size_t cap)
{
    size_t i = 0;
    for (; k && k[i] && i < 8 && i + 1 < cap; i++)
    {
        const char c = k[i];
        out[i] = ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ? c : '?';
    }
    if (cap)
    {
        out[i < cap ? i : cap - 1] = 0;
    }
}

// The JSON after a verified signature line (no filter: the firmware's
// other JSON readers share the same parser code). board: "pro" / "lite".
// Not Ok: why names the reason. Ok: out.gatewayInvalid with why naming
// the field of this board's entry; skip (may be nullptr) gets every entry
// skipped. minSeq: a lower seq is Replay, decided before any entry is
// looked at (nothing of a replayed manifest is reported or kept)
inline Result parse(const char *json, size_t len, const char *board, const Limits &lim, Doc &out, char *why,
                    size_t whyCap, SkipFn skip = nullptr, uint32_t minSeq = 0)
{
    clear(out);
    if (whyCap)
    {
        why[0] = 0;
    }
    DynamicJsonDocument doc(JSON_CAPACITY);
    if (doc.capacity() == 0)
    {
        return Result::Error;
    }
    const DeserializationError e = deserializeJson(doc, json, len);
    if (e != DeserializationError::Ok || !doc.is<JsonObject>())
    {
        snprintf(why, whyCap, "%s", e == DeserializationError::NoMemory ? "too many entries" : e.c_str());
        return Result::Json;
    }
    if (!doc["seq"].is<uint32_t>())
    {
        snprintf(why, whyCap, "seq");
        return Result::Field;
    }
    out.seq = doc["seq"].as<uint32_t>();
    if (out.seq < minSeq)
    {
        snprintf(why, whyCap, "seq %lu < %lu", (unsigned long)out.seq, (unsigned long)minSeq);
        return Result::Replay;
    }
    JsonVariantConst gw = doc["gateway"];
    JsonVariantConst sensors = doc["sensor"];
    if (!gw.isNull() && !gw.is<JsonObjectConst>())
    {
        snprintf(why, whyCap, "gateway");
        return Result::Field;
    }
    if (!sensors.isNull() && !sensors.is<JsonObjectConst>())
    {
        snprintf(why, whyCap, "sensor");
        return Result::Field;
    }
    char item[48];
    auto skipped = [&]() {
        if (out.skipped < UINT16_MAX)
        {
            out.skipped++;
        }
        if (skip)
        {
            skip(item);
        }
    };
    for (const char *b : {"pro", "lite"})
    {
        JsonVariantConst p = gw[b];
        if (p.isNull())
        {
            continue;
        }
        Package pkg;
        char where[16];
        snprintf(where, sizeof(where), "gateway.%s", b);
        const bool ok = parsePackage(p, lim.gatewayMin, lim.gatewayMax, pkg, where, item, sizeof(item));
        if (strcmp(b, board) != 0)
        {
            if (!ok)
            {
                skipped();
            }
        }
        else if (ok)
        {
            out.hasGateway = true;
            out.gateway = pkg;
        }
        else
        {
            out.gatewayInvalid = true;
            snprintf(why, whyCap, "%s", item);
        }
    }
    for (JsonPairConst kv : sensors.as<JsonObjectConst>())
    {
        uint16_t hw = 0;
        char key[9], where[24];
        keyText(kv.key().c_str(), key, sizeof(key));
        snprintf(where, sizeof(where), "sensor.%s", key);
        if (!parseHw(kv.key().c_str(), hw) || out.sensorFor(hw))
        {
            snprintf(item, sizeof(item), "%s (HW type key)", where);
            skipped();
            continue;
        }
        Package pkg;
        if (!parsePackage(kv.value(), lim.sensorMin, lim.sensorMax, pkg, where, item, sizeof(item)))
        {
            skipped();
            continue;
        }
        if (out.sensorCount >= MAX_PACKAGES)
        {
            snprintf(item, sizeof(item), "%s (more than %u sensor packages)", where, (unsigned)MAX_PACKAGES);
            skipped();
            continue;
        }
        out.sensor[out.sensorCount].hw = hw;
        out.sensor[out.sensorCount].pkg = pkg;
        out.sensorCount++;
    }
    return Result::Ok;
}

// The whole check of a downloaded file: signature, then the JSON, then the
// replay rule against storedSeq. out.keyId is set whenever line 1 names a
// key
inline Result load(const char *file, size_t len, const char *board, const Limits &lim, uint32_t storedSeq, Doc &out,
                   char *why, size_t whyCap, const ReleaseKeys::Key *keys = ReleaseKeys::KEYS,
                   size_t keyCount = ReleaseKeys::SLOTS, SkipFn skip = nullptr)
{
    clear(out);
    if (whyCap)
    {
        why[0] = 0;
    }
    uint32_t keyId = 0;
    Result r = verify(file, len, keyId, keys, keyCount);
    if (r == Result::Ok)
    {
        r = parse(file + HEADER_LEN, len - HEADER_LEN, board, lim, out, why, whyCap, skip, storedSeq);
    }
    out.keyId = keyId;
    return r;
}

// What the manifest offers against what is here
enum class Offer : uint8_t
{
    None,    // no package
    Install, // gateway: newer than the running firmware; sensor: not stored, newer, or the same version signed
             // while the stored image is unsigned
    Current, // the same version (verified, for a sensor image)
    Older,   // older than the running firmware / the stored image: never offered
    Unknown, // the running version cannot be compared (not maj.min.pat): never offered
};

inline const char *offerName(Offer o)
{
    switch (o)
    {
    case Offer::None:
        return "none";
    case Offer::Install:
        return "install";
    case Offer::Current:
        return "current";
    case Offer::Older:
        return "older";
    case Offer::Unknown:
        return "unknown";
    }
    return "?";
}

// running: GatewayImage::runningVersionOf(FIRMWARE_VERSION)
inline Offer gatewayOffer(const Doc &d, uint32_t running)
{
    if (!d.hasGateway)
    {
        return Offer::None;
    }
    if (running == GatewayImage::VERSION_UNKNOWN)
    {
        return Offer::Unknown;
    }
    if (d.gateway.version > running)
    {
        return Offer::Install;
    }
    return d.gateway.version == running ? Offer::Current : Offer::Older;
}

// A sensor image of this version (signed) is wanted against the stored
// one: nothing stored, an older one, or the same version unsigned
inline bool sensorWanted(uint32_t version, bool stored, uint32_t storedFw, bool storedSigned)
{
    return !stored || version > storedFw || (version == storedFw && !storedSigned);
}

inline Offer sensorOffer(const Package *p, bool stored, uint32_t storedFw, bool storedSigned)
{
    if (!p)
    {
        return Offer::None;
    }
    if (sensorWanted(p->version, stored, storedFw, storedSigned))
    {
        return Offer::Install;
    }
    return p->version == storedFw ? Offer::Current : Offer::Older;
}

// What a download from the manifest must be (set by the install / fetch
// request from the offered package)
struct Expect
{
    bool set = false;
    uint16_t hw = 0; // sensor image: the HW type
    uint32_t version = 0;
    uint32_t size = 0;
    uint8_t sha256[32] = {};
};

inline Expect expectOf(const Package &p, uint16_t hw = 0)
{
    Expect e;
    e.set = true;
    e.hw = hw;
    e.version = p.version;
    e.size = p.size;
    memcpy(e.sha256, p.sha256, 32);
    return e;
}

// A gateway image downloaded for a manifest install, after the writer's
// finish(): nullptr = it may be activated; else why not (the slot is then
// erased - there is no confirmation for a manifest install). verified: the
// image's own block verified (GatewayImage::Class::Verified); blockVersion:
// its signed version; length / digest: the whole file
inline const char *gatewayInstallRefusal(const Expect &e, uint64_t length, const uint8_t digest[32], bool verified,
                                         uint32_t blockVersion, uint32_t running)
{
    if (length != e.size)
    {
        return "the size differs from the manifest";
    }
    if (memcmp(digest, e.sha256, 32) != 0)
    {
        return "the SHA-256 differs from the manifest";
    }
    if (!verified)
    {
        return "not signed with a release key";
    }
    if (blockVersion != e.version)
    {
        return "the signed version differs from the manifest";
    }
    if (running == GatewayImage::VERSION_UNKNOWN || blockVersion <= running)
    {
        return "not newer than the running firmware";
    }
    return nullptr;
}

// A sensor image downloaded for a manifest fetch, after the store
// validated it: nullptr = it may be stored
inline const char *sensorFetchRefusal(const Expect &e, uint32_t length, const uint8_t digest[32], bool verified,
                                      uint16_t hw, uint32_t fw)
{
    if (length != e.size)
    {
        return "the size differs from the manifest";
    }
    if (memcmp(digest, e.sha256, 32) != 0)
    {
        return "the SHA-256 differs from the manifest";
    }
    if (!verified)
    {
        return "not signed with a release key";
    }
    if (hw != e.hw)
    {
        return "the HW type differs from the manifest";
    }
    if (fw != e.version)
    {
        return "the version differs from the manifest";
    }
    return nullptr;
}
} // namespace Manifest
