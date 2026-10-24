/**
 * expLORA Gateway
 *
 * UpdateState - /update/state.json (M4c S3; later stages add their fields
 * under the same version): the gateway firmware's install record.
 *
 *   {"v":1,
 *    "running":{"ver":"2.0.1","part":"app1","sig":"verified","key":305419896},
 *    "pending":{"ver":"2.0.1","part":"app1","sig":"unsigned","key":0,"at":1761200000},
 *    "written":{"part":"app0","at":1761200400},
 *    "last":{"text":"rolled back from v2.0.1","at":1761200300}}
 *
 * running: how the running image was installed (written when it is
 * validated; it describes the running firmware only while ver and part
 * match it - a USB flash leaves an old record). pending: an activated image
 * that has not been validated yet (written before the boot partition is
 * switched, completed with the version by the new firmware when it starts,
 * moved to running when it is validated; the old firmware finds it after a
 * rollback). written: an image written to the other slot that waits for
 * the admin's confirmation (never activated) - if the gateway restarts
 * before it is confirmed or dropped, the next start erases its first sector
 * (GatewayImage::leftImageErased). last: the last install result shown on
 * the Firmware page.
 * Pure C++ with ArduinoJson (test/test_gateway_image).
 *
 * Contract for later stages (S5 adds its fields here): extend version 1 in
 * place with OPTIONAL fields - a missing field means "not known". This
 * firmware reads only the fields above and rewrites the whole file (tmp +
 * rename) on every change, so fields it does not know are DROPPED (e.g.
 * after a rollback to this firmware); a later stage must never rely on its
 * fields surviving a start of an older firmware. "v" changes only for an
 * incompatible layout; this firmware treats any other "v" as an empty state
 * (and overwrites it on its next change).
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
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace UpdateState
{
constexpr int VERSION = 1;
constexpr size_t TEXT_CAPACITY = 640;  // the file (worst case ~470 B)
constexpr size_t JSON_CAPACITY = 1024; // the document (on the heap: the callers run on 8 kB task stacks)
constexpr size_t TEXT_MAX = 96;

struct Image
{
    bool set = false;
    char ver[24] = "";
    char part[17] = "";
    char sig[12] = ""; // "verified", "unsigned", "nokey"
    uint32_t key = 0;
    uint32_t at = 0;   // pending: unix time of the activation (0 = clock not set)
};

struct State
{
    Image running;
    Image pending;
    Image written; // part + at only
    char lastText[TEXT_MAX] = "";
    uint32_t lastAt = 0;
};

inline void copyText(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

inline void imageFrom(JsonObjectConst o, Image &img)
{
    img = Image();
    if (o.isNull())
    {
        return;
    }
    img.set = true;
    copyText(img.ver, sizeof(img.ver), o["ver"] | "");
    copyText(img.part, sizeof(img.part), o["part"] | "");
    copyText(img.sig, sizeof(img.sig), o["sig"] | "");
    img.key = o["key"] | 0u;
    img.at = o["at"] | 0u;
}

inline void imageTo(JsonObject o, const Image &img, bool withAt)
{
    o["ver"] = img.ver;
    o["part"] = img.part;
    o["sig"] = img.sig;
    o["key"] = img.key;
    if (withAt)
    {
        o["at"] = img.at;
    }
}

// false: not a state of this layout (missing / other version / damaged) -
// the state stays empty
inline bool parse(const char *json, size_t len, State &out)
{
    out = State();
    DynamicJsonDocument doc(JSON_CAPACITY);
    if (doc.capacity() == 0 || deserializeJson(doc, json, len) != DeserializationError::Ok || !doc.is<JsonObject>())
    {
        return false;
    }
    if ((doc["v"] | 0) != VERSION)
    {
        return false;
    }
    imageFrom(doc["running"].as<JsonObjectConst>(), out.running);
    imageFrom(doc["pending"].as<JsonObjectConst>(), out.pending);
    imageFrom(doc["written"].as<JsonObjectConst>(), out.written);
    JsonObjectConst last = doc["last"].as<JsonObjectConst>();
    if (!last.isNull())
    {
        copyText(out.lastText, sizeof(out.lastText), last["text"] | "");
        out.lastAt = last["at"] | 0u;
    }
    return true;
}

// Bytes written (0: does not fit)
inline size_t format(const State &s, char *out, size_t cap)
{
    DynamicJsonDocument doc(JSON_CAPACITY);
    if (doc.capacity() == 0)
    {
        return 0;
    }
    doc["v"] = VERSION;
    if (s.running.set)
    {
        imageTo(doc.createNestedObject("running"), s.running, false);
    }
    if (s.pending.set)
    {
        imageTo(doc.createNestedObject("pending"), s.pending, true);
    }
    if (s.written.set)
    {
        JsonObject w = doc.createNestedObject("written");
        w["part"] = s.written.part;
        w["at"] = s.written.at;
    }
    if (s.lastText[0])
    {
        JsonObject last = doc.createNestedObject("last");
        last["text"] = s.lastText;
        last["at"] = s.lastAt;
    }
    if (doc.overflowed() || measureJson(doc) + 1 > cap)
    {
        return 0;
    }
    return serializeJson(doc, out, cap);
}

// "verified (key 1a2b3c4d)", "unsigned (unknown key 1a2b3c4d)", "unsigned"
inline void sigText(const Image &img, char *out, size_t cap)
{
    if (strcmp(img.sig, "verified") == 0)
    {
        snprintf(out, cap, "verified (key %08lx)", (unsigned long)img.key);
    }
    else if (strcmp(img.sig, "nokey") == 0)
    {
        snprintf(out, cap, "unsigned (unknown key %08lx)", (unsigned long)img.key);
    }
    else if (img.sig[0])
    {
        snprintf(out, cap, "unsigned");
    }
    else
    {
        snprintf(out, cap, "unknown");
    }
}
} // namespace UpdateState
