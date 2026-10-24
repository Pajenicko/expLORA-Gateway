/**
 * expLORA Gateway
 *
 * Description of a stored sensor image (/ota/hw<HHHH>.json, also
 * /ota/meta.tmp during a commit) and its JSON form. Pure (ArduinoJson only),
 * unit-tested natively in test/test_ota_store.
 *
 * Fields: hw, fw, len, crc, flags, uploaded; since M4c S2 also "signed"
 * (key id of a verified signature, 0 = unsigned) and, for a file signed
 * with a key the gateway does not know, "unknownKey" plus a "note". A file
 * written by an older firmware has none of them and reads as unsigned.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <ArduinoJson.h>

namespace OtaMeta
{
struct Meta
{
    bool valid = false;
    uint16_t hwType = 0;
    uint32_t fw = 0;
    uint32_t len = 0;
    uint32_t crc = 0;
    uint32_t flags = 0;
    uint32_t uploadUnix = 0;
    uint32_t signedKey = 0;  // verified signature: the key id; 0 = unsigned
    uint32_t unknownKey = 0; // signed with a key not in the table (still unsigned)

    bool isSigned() const { return signedKey != 0; }
};

// Room for every field incl. the note (copied when read from a file): a
// full record needs about 376 B on a 64-bit host (less on the ESP32); twice
// that, so a later firmware's extra fields still fit when they are written
// by it. Reading keeps only the known fields anyway (filter()), so unknown
// fields of any size never make a description unreadable
constexpr size_t JSON_CAPACITY = 768;
constexpr size_t FILTER_CAPACITY = JSON_OBJECT_SIZE(8);

// The fields readMeta keeps (ArduinoJson filter)
inline void filter(JsonDocument &f)
{
    f["hw"] = true;
    f["fw"] = true;
    f["len"] = true;
    f["crc"] = true;
    f["flags"] = true;
    f["uploaded"] = true;
    f["signed"] = true;
    f["unknownKey"] = true;
}

inline void toJson(const Meta &m, JsonDocument &doc)
{
    doc["hw"] = m.hwType;
    doc["fw"] = m.fw;
    doc["len"] = m.len;
    doc["crc"] = m.crc;
    doc["flags"] = m.flags;
    doc["uploaded"] = m.uploadUnix;
    doc["signed"] = m.signedKey;
    if (!m.signedKey && m.unknownKey)
    {
        char note[48];
        snprintf(note, sizeof(note), "signed with an unknown key %08lx", (unsigned long)m.unknownKey);
        doc["unknownKey"] = m.unknownKey;
        doc["note"] = note; // copied by the document
    }
}

// false: no image description (len 0)
inline bool fromJson(JsonDocument &doc, Meta &m)
{
    // "| 0u": a plain "| 0" reads values >= 2^31 as 0 (108bb2a)
    m.hwType = doc["hw"] | 0u;
    m.fw = doc["fw"] | 0u;
    m.len = doc["len"] | 0u;
    m.crc = doc["crc"] | 0u;
    m.flags = doc["flags"] | 0u;
    m.uploadUnix = doc["uploaded"] | 0u;
    m.signedKey = doc["signed"] | 0u;                         // absent (older firmware): unsigned
    m.unknownKey = m.signedKey ? 0 : (doc["unknownKey"] | 0u); // only meaningful when unsigned
    m.valid = m.len > 0;
    return m.valid;
}

inline bool sameContent(const Meta &a, const Meta &b)
{
    return a.hwType == b.hwType && a.fw == b.fw && a.len == b.len && a.crc == b.crc && a.flags == b.flags &&
           a.uploadUnix == b.uploadUnix && a.signedKey == b.signedKey && a.unknownKey == b.unknownKey;
}
} // namespace OtaMeta
