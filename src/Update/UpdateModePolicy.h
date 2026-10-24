/**
 * expLORA Gateway
 *
 * UpdateModePolicy - pure decisions of the update mode (Update/UpdateMode.h):
 * who may enter, what to switch off when TLS does not fit, when to give up,
 * the hard cap and the connectivity check after leaving. Free of Arduino so
 * the native tests can pin them (test/test_update_mode).
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../Hardware/HeapGuard.h"
#include "../Protocol/OtaJob.h"

namespace UpdateModePolicy
{
// Why a session runs (shown in the UI and /diag/heap)
enum class Reason : uint8_t
{
    None,
    UpdateCheck,  // "Check for update" (manifest of the 1.x line)
    GatewayUrl,   // gateway image from a URL (incl. the 1.x autoupdate path)
    SensorFetch,  // sensor image from a URL (S4)
    Manifest,     // signed package manifest (S5)
    Scheduled,    // nightly session of the automatic updates (S6)
};

inline const char *reasonName(Reason r)
{
    switch (r)
    {
    case Reason::None:
        return "none";
    case Reason::UpdateCheck:
        return "update check";
    case Reason::GatewayUrl:
        return "gateway firmware download";
    case Reason::SensorFetch:
        return "sensor image download";
    case Reason::Manifest:
        return "package manifest check";
    case Reason::Scheduled:
        return "scheduled update";
    }
    return "unknown";
}

// The interface carrying traffic (Uplink::Type)
enum class Link : uint8_t
{
    None,
    Ethernet,
    WiFi,
    LTE,
};

// Interfaces the update mode can switch off (bit mask). WIFI_STA off means
// the whole WiFi driver (station and access point); WIFI_AP alone keeps a
// station that carries the uplink. Ethernet is never switched off: it is
// either the uplink or has no address
constexpr uint8_t IF_WIFI_STA = 1;
constexpr uint8_t IF_WIFI_AP = 2;
constexpr uint8_t IF_LTE = 4;

// Every running interface that does not carry the uplink (design 7):
// Ethernet -> WiFi station + AP and LTE; WiFi station -> AP and LTE;
// LTE -> WiFi station + AP. Interfaces a board lacks are never running
// (the Lite: no LTE, WiFi is its uplink)
inline uint8_t switchOffPlan(Link uplink, uint8_t running)
{
    uint8_t keep = 0;
    if (uplink == Link::WiFi)
    {
        keep = IF_WIFI_STA;
    }
    else if (uplink == Link::LTE)
    {
        keep = IF_LTE;
    }
    return running & (uint8_t)~keep;
}

// A sensor OTA job allows the update mode only when it is terminal (or
// there is none) or Paused: the radio is put in standby for the session,
// and a job in Waiting / Transfer / Verified / Swapping / Confirming needs
// it (a lost accepting UP or status costs a round, a lost confirmation
// can end in a revert). A queued start (startPending) counts as active
inline bool jobAllows(OtaJob::State s, bool startPending)
{
    return !startPending && (OtaJob::terminal(s) || s == OtaJob::State::Paused);
}

enum class Refusal : uint8_t
{
    None,
    Active,        // another update-mode session runs
    RestartPending, // a restart is requested (e.g. after a completed upload)
    Offline,       // no uplink to download over
    ClockNotSet,   // certificates cannot be checked
    UploadRunning, // a firmware or sensor image upload runs
    SensorJob,     // a sensor OTA job needs the radio
};

inline const char *refusalText(Refusal r)
{
    switch (r)
    {
    case Refusal::None:
        return "";
    case Refusal::Active:
        return "another update session is running";
    case Refusal::RestartPending:
        return "the gateway is about to restart";
    case Refusal::Offline:
        return "the gateway is offline";
    case Refusal::ClockNotSet:
        return "the clock is not synchronized yet (needed to check certificates)";
    case Refusal::UploadRunning:
        return "a firmware or sensor image upload is running";
    case Refusal::SensorJob:
        return "a sensor OTA job is running (pause it or wait until it ends)";
    }
    return "refused";
}

struct Precheck
{
    bool active;
    bool restartPending;
    Link uplink;
    bool clockSet;
    bool uploadRunning;
    OtaJob::State job;
    bool jobStartPending;
};

// In this order: the cheapest and most specific reason first
inline Refusal precheck(const Precheck &p)
{
    if (p.active)
    {
        return Refusal::Active;
    }
    if (p.restartPending)
    {
        return Refusal::RestartPending;
    }
    if (p.uplink == Link::None)
    {
        return Refusal::Offline;
    }
    if (!p.clockSet)
    {
        return Refusal::ClockNotSet;
    }
    if (p.uploadRunning)
    {
        return Refusal::UploadRunning;
    }
    if (!jobAllows(p.job, p.jobStartPending))
    {
        return Refusal::SensorJob;
    }
    return Refusal::None;
}

// TLS session plus what the session's own work allocates next to it (the
// download buffer, the Update sector buffer, the manifest text)
inline bool fits(HeapGuard::Heap tlsHeap, size_t extra)
{
    return tlsHeap.free >= HeapGuard::TLS_MIN_FREE + extra && tlsHeap.largest >= HeapGuard::TLS_MIN_BLOCK;
}

enum class MemStep : uint8_t
{
    Proceed,   // the session fits
    SwitchOff, // switch the planned interfaces off, let the heap settle, check again
    Abort,     // does not fit and nothing (more) to switch off
};

// Graduated: interfaces go off only when the session does not fit as it
// is, and all of the plan at once (the margin for a 15-minute download
// matters more than keeping one more interface up for a few minutes)
inline MemStep memoryStep(bool fitsNow, uint8_t plan, bool switchedAlready)
{
    if (fitsNow)
    {
        return MemStep::Proceed;
    }
    if (!switchedAlready && plan != 0)
    {
        return MemStep::SwitchOff;
    }
    return MemStep::Abort;
}

// Hard cap of one session (uint32_t milliseconds: the arithmetic wraps)
inline bool capReached(uint32_t now, uint32_t startedAt, uint32_t cap)
{
    return now - startedAt >= cap;
}

// Connectivity of the gateway, before entering and after leaving
struct LinkState
{
    bool online;        // some uplink has an address
    bool stationUp;     // the WiFi station has an address
    bool apUp;          // the access point runs
    bool lteOnline;     // the PPP session has an address
};

// Not an interface the update mode switches: "the uplink" in the
// connectivity watch's missing mask and in interfacesText()
constexpr uint8_t IF_UPLINK = 0x80;

// Connectivity watch after leave(): only armed when a session switched
// interfaces off. It expects back what was working before and went off;
// the uplink itself is never switched off, so it counts only if it was
// online before the session
struct Watch
{
    bool armed;
    uint8_t expect;  // IF_* bits that worked before and were switched off
    bool needOnline; // the uplink was online before the session
    uint32_t leftAt; // millis() of the leave() that (re)started the window
    uint8_t dropped; // IF_* bits dropped since the watch was armed (disabled / changed)
};

// leave() of a session that switched `off` off. A watch still running from
// an earlier session is kept and merged (its interfaces may still be
// reattaching, so this session's `before` would not expect them); the window
// starts again from this leave(). A session that switched nothing off
// arms no watch, but restarts the window of a running one: it blocked the
// loop, so the reattach of the watched interfaces (LTE) was frozen meanwhile
inline void armWatch(Watch &w, uint8_t off, const LinkState &before, uint32_t now)
{
    if (off == 0)
    {
        if (w.armed)
        {
            w.leftAt = now;
        }
        return;
    }
    if (!w.armed)
    {
        w.dropped = 0;
    }
    uint8_t expect = 0;
    if ((off & IF_WIFI_STA) && before.stationUp)
    {
        expect |= IF_WIFI_STA;
    }
    if ((off & (IF_WIFI_STA | IF_WIFI_AP)) && before.apUp)
    {
        expect |= IF_WIFI_AP;
    }
    if ((off & IF_LTE) && before.lteOnline)
    {
        expect |= IF_LTE;
    }
    w.expect = (w.armed ? w.expect : 0) | expect;
    w.needOnline = (w.armed && w.needOnline) || before.online;
    w.armed = true;
    w.leftAt = now;
}

// Whether each interface is still wanted at check time: enabled in the
// settings, settings unchanged since the session (the station's network,
// the LTE APN / PIN), the access point wanted by the WiFi policy. An
// interface the user disabled or changed after the session is not the
// update mode's to bring back
struct Wanted
{
    bool station;
    bool ap;
    bool lte;
};

// What is not back yet (IF_* bits, IF_UPLINK for the uplink). Interfaces
// no longer wanted are dropped from the watch for good (dropped: their bits)
inline uint8_t watchMissing(Watch &w, const LinkState &now, const Wanted &wanted, uint8_t *dropped = nullptr)
{
    uint8_t drop = 0;
    if ((w.expect & IF_WIFI_STA) && !wanted.station)
    {
        drop |= IF_WIFI_STA;
    }
    if ((w.expect & IF_LTE) && !wanted.lte)
    {
        drop |= IF_LTE;
    }
    w.expect &= (uint8_t)~drop;
    w.dropped |= drop;
    if (dropped)
    {
        *dropped = drop;
    }
    uint8_t missing = 0;
    if (w.needOnline && !now.online)
    {
        missing |= IF_UPLINK;
    }
    if ((w.expect & IF_WIFI_STA) && !now.stationUp)
    {
        missing |= IF_WIFI_STA;
    }
    // The AP may have timed out meanwhile: only while the policy wants it
    // (not dropped: the policy may want it again later in the window)
    if ((w.expect & IF_WIFI_AP) && wanted.ap && !now.apUp)
    {
        missing |= IF_WIFI_AP;
    }
    if ((w.expect & IF_LTE) && !now.lteOnline)
    {
        missing |= IF_LTE;
    }
    return missing;
}

// Nothing left to wait for because the user disabled / changed every
// watched interface: "watch cancelled", not "connectivity restored"
inline bool watchCancelled(const Watch &w)
{
    return w.expect == 0 && w.dropped != 0;
}

enum class After : uint8_t
{
    Watching, // not back yet, still within the limit
    Restored, // back: stop watching
    Reboot,   // not back within the limit: a restart re-initialises everything
    Deferred, // not back within the limit, but a restart now would break
              // work in progress (an upload, a sensor OTA job that needs the
              // radio): checked again later
};

inline After afterLeave(uint8_t missing, uint32_t now, uint32_t leftAt, uint32_t limit, bool busy)
{
    if (missing == 0)
    {
        return After::Restored;
    }
    if (now - leftAt < limit)
    {
        return After::Watching;
    }
    return busy ? After::Deferred : After::Reboot;
}

// A sensor OTA job a restart must not interrupt: data on its way, the
// swap or its confirmation (a restart there costs a round or more)
inline bool jobBlocksRestart(OtaJob::State s)
{
    return s == OtaJob::State::Transfer || s == OtaJob::State::Swapping || s == OtaJob::State::Confirming;
}

// What a watch restart now would break: an upload, a sensor job that needs
// the radio, or a start queued by the admin (it becomes a job any moment)
inline bool restartWouldInterrupt(bool uploadRunning, OtaJob::State s, bool startPending)
{
    return uploadRunning || startPending || jobBlocksRestart(s);
}

// The watch is checked at most this often (millis(); it reads the link state
// and the settings of every interface)
constexpr uint32_t WATCH_CHECK_MS = 1000;

// URLs are split exactly like HTTPClient::beginInternal() does (arduino-esp32
// 3.x): the authority runs to the first '/' (not to '?' or '#'), the user
// info ends at the FIRST '@' of the authority, the port at the first ':'
// (after "]" for an IPv6 literal). So the DNS pre-resolve and the log name
// the host the connection actually goes to.
struct UrlParts
{
    const char *scheme = nullptr; // up to "://" (excluded)
    size_t schemeLen = 0;
    const char *host = nullptr;   // host[:port], without the user info
    size_t hostLen = 0;
    const char *path = nullptr;   // from the first '/' after the authority (may be empty)
};

inline UrlParts splitUrl(const char *url)
{
    UrlParts p;
    const char *sep = strstr(url, "://");
    const char *a = url;
    if (sep)
    {
        p.scheme = url;
        p.schemeLen = (size_t)(sep - url);
        a = sep + 3;
    }
    const char *end = a;
    while (*end && *end != '/')
    {
        end++;
    }
    const char *h = a;
    for (const char *c = a; c < end; c++)
    {
        if (*c == '@')
        {
            h = c + 1; // the first '@'
            break;
        }
    }
    p.host = h;
    p.hostLen = (size_t)(end - h);
    p.path = end;
    return p;
}

// A host part HTTPClient would take but no valid URL has: '?', '#' or '@'
// left in it (a query without a path, a raw '@' in the password). Never
// looked up or logged - it may carry part of a password or a token
inline bool oddHost(const char *h, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (h[i] == '?' || h[i] == '#' || h[i] == '@')
        {
            return true;
        }
    }
    return false;
}

// The host of an https:// URL for the DNS pre-resolve (no user info, no
// port). IP literals ("[v6]", dotted v4) need no lookup: literal = true.
// false: no usable host (none, an odd one, or longer than cap - 1)
inline bool urlHost(const char *url, char *host, size_t cap, bool &literal)
{
    literal = false;
    if (!url || cap == 0 || strncmp(url, "https://", 8) != 0)
    {
        return false;
    }
    host[0] = 0;
    const UrlParts p = splitUrl(url);
    if (p.hostLen > 0 && p.host[0] == '[')
    {
        literal = true;
        return true;
    }
    size_t n = 0;
    while (n < p.hostLen && p.host[n] != ':')
    {
        n++;
    }
    if (n == 0 || n >= cap || oddHost(p.host, n))
    {
        return false;
    }
    memcpy(host, p.host, n);
    host[n] = 0;
    bool dotted = true;
    for (size_t i = 0; i < n; i++)
    {
        if (!((host[i] >= '0' && host[i] <= '9') || host[i] == '.'))
        {
            dotted = false;
        }
    }
    literal = dotted;
    return true;
}

// A URL for the log: scheme://host[:port]/path - the host as the connection
// takes it, never the user info, the path without query and fragment; an
// odd host shows as "(invalid host)". out gets at most cap - 1 characters
// (strlen(url) + 15 always suffices)
inline void urlForLog(const char *url, char *out, size_t cap)
{
    if (!url || cap == 0)
    {
        return;
    }
    size_t o = 0;
    auto put = [&](const char *s, size_t n) {
        for (size_t i = 0; i < n && o + 1 < cap; i++)
        {
            out[o++] = s[i];
        }
    };
    const UrlParts p = splitUrl(url);
    if (p.scheme)
    {
        put(p.scheme, p.schemeLen);
        put("://", 3);
    }
    if (oddHost(p.host, p.hostLen))
    {
        put("(invalid host)", 14);
    }
    else
    {
        put(p.host, p.hostLen);
    }
    size_t n = 0;
    while (p.path[n] && p.path[n] != '?' && p.path[n] != '#')
    {
        n++;
    }
    put(p.path, n);
    out[o] = 0;
}

// Sessions are numbered: leave(), expired() and the RAII Session act only
// on their own session. A session ended by the hard cap (process()) and a
// newer one entered meanwhile are not theirs
inline bool ownsSession(uint32_t mine, uint32_t current, bool active)
{
    return mine != 0 && mine == current && active;
}

// The next session number (never 0, which means "no session")
inline uint32_t nextGeneration(uint32_t current)
{
    uint32_t n = current + 1;
    return n == 0 ? 1 : n;
}

// "WiFi station, access point, LTE" (or "nothing"); out has >= 48 bytes
inline void interfacesText(uint8_t mask, char *out, size_t cap)
{
    static const struct
    {
        uint8_t bit;
        const char *name;
    } names[] = {{IF_UPLINK, "the uplink"}, {IF_WIFI_STA, "WiFi station"}, {IF_WIFI_AP, "access point"}, {IF_LTE, "LTE"}};
    size_t n = 0;
    out[0] = 0;
    for (const auto &e : names)
    {
        if (mask & e.bit)
        {
            int w = snprintf(out + n, cap - n, "%s%s", n ? ", " : "", e.name);
            if (w < 0 || (size_t)w >= cap - n)
            {
                return;
            }
            n += (size_t)w;
        }
    }
    if (n == 0)
    {
        snprintf(out, cap, "nothing");
    }
}
constexpr size_t INTERFACES_TEXT_MAX = 48;

// Result and refusal texts end up in JSON as they are: quotes, backslashes
// and control characters are replaced (never longer than the input)
inline void jsonSafeCopy(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; in && in[n] && n + 1 < cap; n++)
    {
        char c = in[n];
        out[n] = (c == '"' || c == '\\') ? '\'' : ((unsigned char)c < 0x20 ? ' ' : c);
    }
    out[n] = 0;
}

constexpr size_t RESULT_MAX = 112;  // last session's result text incl. the terminator
constexpr size_t REFUSAL_MAX = 96;  // last refusal text incl. the terminator

// /diag/heap "updateMode" object
struct DiagView
{
    bool active;
    // The session now (active only)
    Reason reason;
    uint32_t startedAt; // wall clock, 0 = not set
    uint32_t runningS;
    uint8_t switchedOff;
    uint32_t freeAtEntry, largestAtEntry, minFree;
    // Counters
    uint32_t sessions, aborts, refusals;
    // The last session
    Reason lastReason;
    const char *lastResult;
    uint32_t lastDurationS, lastMinFree;
    uint8_t lastSwitchedOff;
    uint8_t restore; // 0 none, 1 watching, 2 restored, 3 cancelled
    uint32_t restoreS;
    uint8_t watchExpect; // interfaces the watch still waits for
    // The last refusal (shown apart from the last session)
    Reason refusedReason; // None = no refusal yet
    const char *refusedText;
    uint32_t refusedAgoS;
};

// Worst case (every text at its maximum, every number 10 digits) stays well
// below this (test_update_mode builds it and parses it)
constexpr size_t DIAG_JSON_MAX = 1024;

// snprintf semantics: the length the whole object needs; out holds it if < cap
inline int diagJson(const DiagView &v, char *out, size_t cap)
{
    char sw[INTERFACES_TEXT_MAX], lastSw[INTERFACES_TEXT_MAX], watch[INTERFACES_TEXT_MAX];
    char result[RESULT_MAX], refused[REFUSAL_MAX];
    interfacesText(v.switchedOff, sw, sizeof(sw));
    interfacesText(v.lastSwitchedOff, lastSw, sizeof(lastSw));
    interfacesText(v.watchExpect, watch, sizeof(watch));
    jsonSafeCopy(v.lastResult, result, sizeof(result));
    jsonSafeCopy(v.refusedText, refused, sizeof(refused));
    size_t n = 0;
    int total = 0;
    auto put = [&](int w)
    {
        if (w < 0)
        {
            return;
        }
        total += w;
        n = (size_t)total < cap ? (size_t)total : (cap ? cap - 1 : 0);
    };
    put(snprintf(out, cap, "{\"active\":%s", v.active ? "true" : "false"));
    if (v.active)
    {
        put(snprintf(out + n, cap - n,
                     ",\"reason\":\"%s\",\"startedAt\":%lu,\"runningS\":%lu,\"switchedOff\":\"%s\","
                     "\"tlsFreeAtEntry\":%lu,\"tlsLargestAtEntry\":%lu,\"minFree8bit\":%lu",
                     reasonName(v.reason), (unsigned long)v.startedAt, (unsigned long)v.runningS, sw,
                     (unsigned long)v.freeAtEntry, (unsigned long)v.largestAtEntry, (unsigned long)v.minFree));
    }
    static const char *const restoreNames[] = {"none", "watching", "restored", "cancelled"};
    put(snprintf(out + n, cap - n,
                 ",\"sessions\":%lu,\"aborts\":%lu,\"refusals\":%lu,\"last\":{\"reason\":\"%s\",\"result\":\"%s\","
                 "\"durationS\":%lu,\"minFree8bit\":%lu,\"switchedOff\":\"%s\",\"restore\":\"%s\",\"restoreS\":%lu,"
                 "\"watching\":\"%s\"}",
                 (unsigned long)v.sessions, (unsigned long)v.aborts, (unsigned long)v.refusals, reasonName(v.lastReason),
                 result, (unsigned long)v.lastDurationS, (unsigned long)v.lastMinFree, lastSw,
                 restoreNames[v.restore < 4 ? v.restore : 0], (unsigned long)v.restoreS, watch));
    if (v.refusedReason != Reason::None)
    {
        put(snprintf(out + n, cap - n, ",\"lastRefusal\":{\"reason\":\"%s\",\"text\":\"%s\",\"agoS\":%lu}",
                     reasonName(v.refusedReason), refused, (unsigned long)v.refusedAgoS));
    }
    put(snprintf(out + n, cap - n, "}"));
    return total;
}
} // namespace UpdateModePolicy
