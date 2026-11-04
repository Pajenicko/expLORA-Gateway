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
 *    "last":{"text":"rolled back from v2.0.1","at":1761200300},
 *    "seq":42,
 *    "check":{"text":"ok (seq 42): v2.0.2 available","at":1761200500},
 *    "auto":{"day":20386,"cursor":1199945858,"gwVer":131074,"gwFails":1,
 *            "job":1199945858,"jobFw":525,"run":"ok: up to date","runAt":1761200600,
 *            "gwDay":20386,"gwAct":1,"pendGw":131074,"tryDay":20386,"tryStep":1,"retryAt":1761201500,
 *            "gwDl":131074,"gwDlDay":20386,"grace":20386,
 *            "sf":[3,525,1,20386]}}
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
 * seq (S5, optional - missing = 0): the highest seq of a package manifest
 * accepted so far (Update/Manifest.h, replay protection: a lower one is
 * refused). check (S5, optional): the result of the last update check.
 * auto (S6, optional - missing = nothing known): the automatic updates.
 * day: the last local window day (days since 1970) whose nightly run is
 * done or given up; cursor: the DevID of the last automatic sensor start
 * (round robin); gwVer / gwFails: automatic install attempts of one
 * gateway version (three stop it until a newer one is offered); job /
 * jobFw: the automatic sensor job in progress (its end is counted for the
 * sensor); run / runAt: the result of the last nightly run. Optional
 * inside "auto" (missing = 0, written only when set): gwDay - the window
 * day of the last strike of gwVer; gwAct - gwVer was activated and is not
 * validated yet (a start of another version = rolled back); pendGw - the
 * gateway version the last check left pending (sensor jobs wait for it
 * also after a restart); tryDay / tryStep / retryAt - the retry wait of
 * the night in progress (window day, failures, unix time; written before
 * each run); gwDl / gwDlDay - the gateway version whose automatic
 * download runs and its window day (S7: set before the download, cleared
 * at its end; still set at a start = the download crashed the gateway, a
 * strike); grace - the window day whose run may start up to 1 h past the
 * window's end after a deferral by a sensor job (S7); sf -
 * sensor image fetch strikes, [hw, version, nights, last day] per HW type.
 * "auto" is written only when one of them is set, so a state without them
 * keeps its layout.
 * An unreadable file (also one whose "seq" is there but not an integer
 * 0..2^32-1) starts a new state, but its seq is taken over if the text
 * still holds a complete one (salvageSeq), else 0 with an error in the log (only flash corruption or a
 * hand-made file gets there), so a damaged file does not silently lower
 * the replay floor. A lost or formatted filesystem,
 * or a rollback to a firmware before S5 (it drops the fields it does not
 * know), resets seq to 0: acceptable, the manifest only names versions and
 * nothing older than the running firmware is ever installed from it.
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
constexpr size_t TEXT_CAPACITY = 1792; // the file (worst case ~1.63 kB, test_gateway_image)
constexpr size_t JSON_CAPACITY = 3072; // the document (on the heap: the callers run on 8 kB task stacks)
constexpr size_t TEXT_MAX = 96;
constexpr size_t RUN_TEXT_MAX = 128; // "auto" run (a deferral or check error after its prefix)
constexpr size_t FETCH_SLOTS = 8;    // "sf" records (one per HW type of the manifest)

struct Image
{
    bool set = false;
    char ver[24] = "";
    char part[17] = "";
    char sig[12] = ""; // "verified", "unsigned", "nokey"
    uint32_t key = 0;
    uint32_t at = 0;   // pending: unix time of the activation (0 = clock not set)
};

// A sensor image version whose nightly fetch failed (AutoUpdatePolicy)
struct FetchStrike
{
    uint16_t hw = 0;
    uint32_t ver = 0;
    uint8_t n = 0;   // nights failed (0 = slot free)
    int32_t day = 0; // the window day of the last strike
};

// The automatic updates' record ("auto")
struct Auto
{
    int32_t day = 0;      // last window day done (days since 1970)
    uint32_t cursor = 0;  // DevID of the last automatic sensor start
    uint32_t gwVer = 0;   // gateway version of the install attempts
    uint8_t gwFails = 0;  // ... their count
    uint32_t job = 0;     // DevID of the automatic sensor job in progress (0 = none)
    uint32_t jobFw = 0;   // ... its image version
    char runText[RUN_TEXT_MAX] = "";
    uint32_t runAt = 0;
    int32_t gwDay = 0;    // window day of the last strike of gwVer
    bool gwAct = false;   // gwVer activated, not validated yet
    uint32_t pendGw = 0;  // gateway version the last check left pending
    int32_t tryDay = 0;   // retry wait of the night in progress: its window day,
    uint8_t tryStep = 0;  // ... failures
    uint32_t retryAt = 0; // ... not before (unix)
    uint32_t gwDl = 0;    // gateway version whose automatic download runs (S7; cleared at its end)
    int32_t gwDlDay = 0;  // ... its window day
    int32_t graceDay = 0; // window day with the grace after a sensor-job deferral (S7)
    FetchStrike sf[FETCH_SLOTS];
};

struct State
{
    Image running;
    Image pending;
    Image written; // part + at only
    char lastText[TEXT_MAX] = "";
    uint32_t lastAt = 0;
    uint32_t seq = 0; // highest accepted manifest seq
    char checkText[TEXT_MAX] = "";
    uint32_t checkAt = 0;
    Auto au; // S6 automatic updates ("auto")
};

// The numbers of "auto" (the first AUTO_ALWAYS always written, the rest
// only when not 0); a negative day reads back through the uint32 cast
constexpr size_t AUTO_NUMS = 16, AUTO_ALWAYS = 7;
constexpr const char *AUTO_KEYS[AUTO_NUMS] = {"day",   "cursor", "gwVer",  "gwFails", "job",     "jobFw",
                                              "runAt", "gwDay",  "gwAct",  "pendGw",  "tryDay",  "tryStep",
                                              "retryAt", "gwDl", "gwDlDay", "grace"};

// An array of at most max unsigned 32-bit integers: their count, else 0
inline size_t readNums(JsonArrayConst a, uint32_t *out, size_t max)
{
    size_t k = 0;
    for (JsonVariantConst v : a)
    {
        if (k == max || !v.is<uint32_t>())
        {
            return 0;
        }
        out[k++] = v.as<uint32_t>();
    }
    return k;
}

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

// false: not a state of this layout (missing / other version / damaged,
// incl. a "seq" that is not an integer 0..2^32-1) - the state stays empty
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
    JsonVariantConst seq = doc["seq"];
    if (!seq.isNull() && !seq.is<uint32_t>())
    {
        out = State();
        return false;
    }
    out.seq = seq | 0u;
    JsonObjectConst check = doc["check"].as<JsonObjectConst>();
    if (!check.isNull())
    {
        copyText(out.checkText, sizeof(out.checkText), check["text"] | "");
        out.checkAt = check["at"] | 0u;
    }
    JsonObjectConst a = doc["auto"].as<JsonObjectConst>();
    if (!a.isNull())
    {
        Auto &u = out.au;
        uint32_t n[AUTO_NUMS];
        for (size_t i = 0; i < AUTO_NUMS; i++)
        {
            n[i] = a[AUTO_KEYS[i]] | 0u;
        }
        u.day = (int32_t)n[0];
        u.cursor = n[1];
        u.gwVer = n[2];
        u.gwFails = n[3] > 255 ? 255 : (uint8_t)n[3];
        u.job = n[4];
        u.jobFw = n[5];
        u.runAt = n[6];
        u.gwDay = (int32_t)n[7];
        u.gwAct = n[8] != 0;
        u.pendGw = n[9];
        u.tryDay = (int32_t)n[10];
        u.tryStep = n[11] > 255 ? 255 : (uint8_t)n[11];
        u.retryAt = n[12];
        u.gwDl = n[13];
        u.gwDlDay = (int32_t)n[14];
        u.graceDay = (int32_t)n[15];
        copyText(u.runText, sizeof(u.runText), a["run"] | "");
        uint32_t f[FETCH_SLOTS * 4];
        const size_t k = readNums(a["sf"], f, FETCH_SLOTS * 4);
        for (size_t i = 0; k % 4 == 0 && i * 4 < k; i++)
        {
            FetchStrike &r = u.sf[i];
            r.hw = (uint16_t)f[i * 4];
            r.ver = f[i * 4 + 1];
            r.n = f[i * 4 + 2] > 255 ? 255 : (uint8_t)f[i * 4 + 2];
            r.day = (int32_t)f[i * 4 + 3];
        }
    }
    return true;
}

// The seq of a file that does not parse: the largest complete number after
// a "seq": in the text (this layout writes the key once, compact) - digits
// followed by ',' or '}' (a number cut by the damage is not taken). 0 if
// there is none; found tells the two apart
inline uint32_t salvageSeq(const char *text, size_t len, bool &found)
{
    found = false;
    uint32_t best = 0;
    static const char KEY[] = "\"seq\":";
    const size_t k = sizeof(KEY) - 1;
    for (size_t i = 0; i + k <= len; i++)
    {
        if (memcmp(text + i, KEY, k) != 0)
        {
            continue;
        }
        size_t j = i + k;
        uint64_t v = 0;
        size_t digits = 0;
        while (j < len && text[j] >= '0' && text[j] <= '9' && digits < 11)
        {
            v = v * 10 + (uint64_t)(text[j] - '0');
            j++;
            digits++;
        }
        if (digits > 0 && v <= 0xFFFFFFFFull && j < len && (text[j] == ',' || text[j] == '}'))
        {
            found = true;
            best = (uint32_t)v > best ? (uint32_t)v : best;
        }
    }
    return best;
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
    if (s.seq)
    {
        doc["seq"] = s.seq;
    }
    if (s.checkText[0])
    {
        JsonObject check = doc.createNestedObject("check");
        check["text"] = s.checkText;
        check["at"] = s.checkAt;
    }
    const Auto &u = s.au;
    bool sf = false;
    for (const FetchStrike &r : u.sf)
    {
        sf = sf || r.n;
    }
    if (u.day || u.cursor || u.gwFails || u.job || u.runText[0] || u.gwAct || u.pendGw || u.tryDay || u.gwDl ||
        u.graceDay || sf)
    {
        JsonObject a = doc.createNestedObject("auto");
        const uint32_t n[AUTO_NUMS] = {(uint32_t)u.day,    u.cursor,  u.gwVer,  u.gwFails,         u.job,
                                       u.jobFw,            u.runAt,   (uint32_t)u.gwDay, u.gwAct, u.pendGw,
                                       (uint32_t)u.tryDay, u.tryStep, u.retryAt, u.gwDl, (uint32_t)u.gwDlDay,
                                       (uint32_t)u.graceDay};
        for (size_t i = 0; i < AUTO_NUMS; i++)
        {
            if (i < AUTO_ALWAYS || n[i])
            {
                a[AUTO_KEYS[i]] = n[i];
            }
        }
        a["run"] = u.runText;
        if (sf)
        {
            JsonArray f = a.createNestedArray("sf");
            for (const FetchStrike &r : u.sf)
            {
                if (r.n)
                {
                    f.add((uint32_t)r.hw);
                    f.add(r.ver);
                    f.add((uint32_t)r.n);
                    f.add((uint32_t)r.day);
                }
            }
        }
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
