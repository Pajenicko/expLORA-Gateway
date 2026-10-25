/**
 * expLORA Gateway
 *
 * SensorFetch - pure rules of the sensor image download from a URL (M4c S4,
 * POST /ota/fetch): what may run at the same time, the Content-Length of
 * the answer, the end of the body, what happens to a validated image (the
 * same verdict for a web upload and a download) and the status JSON. Free
 * of Arduino for the native tests (test/test_sensor_fetch).
 *
 * The download itself runs in the loop task inside an update-mode session
 * (Update/UpdateMode.h, reason SensorFetch) and streams into the image
 * store exactly like a web upload (OtaStore::uploadBegin / uploadWrite,
 * then one shared end: validate, classify the signature, "Signed images
 * only", the job rule, commit).
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

#include "../Protocol/OtaImageSigned.h"
#include "UpdateModePolicy.h"

namespace SensorFetch
{
// The largest file: an image as large as the sensor's slot plus a
// signature block
constexpr uint32_t MAX_FILE = OtaImage::MAX_LEN + SigBlock::SIZE;
constexpr uint32_t MIN_FILE = ep::ota::kMinImageLen;

enum class State : uint8_t
{
    Idle,
    Queued,  // accepted, waiting for the loop task
    Running, // in the update-mode session
    Stored,
    Failed,
};

inline const char *stateName(State s)
{
    switch (s)
    {
    case State::Idle:
        return "idle";
    case State::Queued:
        return "queued";
    case State::Running:
        return "downloading";
    case State::Stored:
        return "stored";
    case State::Failed:
        return "failed";
    }
    return "?";
}

// What else runs that a download may not overlap (besides the update
// mode's own refusals: another session, a restart, offline, no clock, an
// upload, a sensor job that needs the radio)
struct Activity
{
    bool fetch;         // a download is queued or runs
    bool gatewayUpload; // POST /firmware owns the OTA slot
    bool imageUpload;   // POST /ota/upload runs (or the store has an upload open)
    bool gatewayUrl;    // a gateway firmware download is queued or runs
    bool updateCheck;   // "Check for update" is queued or runs
    bool restart;       // a restart is requested
};

enum class Busy : uint8_t
{
    None,
    Fetch,
    Restart,
    GatewayUpload,
    ImageUpload,
    GatewayUrl,
    UpdateCheck,
};

inline Busy busy(const Activity &a)
{
    if (a.fetch)
    {
        return Busy::Fetch;
    }
    if (a.restart)
    {
        return Busy::Restart;
    }
    if (a.gatewayUpload)
    {
        return Busy::GatewayUpload;
    }
    if (a.imageUpload)
    {
        return Busy::ImageUpload;
    }
    if (a.gatewayUrl)
    {
        return Busy::GatewayUrl;
    }
    if (a.updateCheck)
    {
        return Busy::UpdateCheck;
    }
    return Busy::None;
}

inline const char *busyText(Busy b)
{
    switch (b)
    {
    case Busy::None:
        return "";
    case Busy::Fetch:
        return "a download is running";
    case Busy::Restart:
        return "the gateway is about to restart";
    case Busy::GatewayUpload:
        return "a firmware upload is running";
    case Busy::ImageUpload:
        return "an image upload is running";
    case Busy::GatewayUrl:
        return "a firmware download is running";
    case Busy::UpdateCheck:
        return "an update check is running";
    }
    return "busy";
}

// Content-Length of the answer (HTTPClient::getSize(): -1 = none, e.g.
// chunked): required, checked before anything is written. The gateway
// firmware download passes its own limits
enum class Length : uint8_t
{
    Ok,
    Missing,
    TooShort,
    TooLarge,
};

inline Length lengthCheck(long long contentLength, uint32_t minLen = MIN_FILE, uint32_t maxLen = MAX_FILE)
{
    if (contentLength < 0)
    {
        return Length::Missing;
    }
    if (contentLength < (long long)minLen)
    {
        return Length::TooShort;
    }
    if (contentLength > (long long)maxLen)
    {
        return Length::TooLarge;
    }
    return Length::Ok;
}

// Time budget of one download in milliseconds, counted from the request:
// 30 s plus the body at 2 kB/s. Connection, TLS, redirects and headers are
// charged to it. Until the headers are in, the budget of the largest
// allowed file applies; then the budget of the announced length. Both are
// enforced by the download's client (httpsDownload: it reads as closed at
// the deadline, so any phase - also a server that trickles its headers -
// ends at most one read timeout later; a redirect hop is connected only
// while its connect + TLS time still fits before the deadline) and by the
// body loop. The radio is
// paused meanwhile, so a slow or trickling server is cut off instead of
// holding the gateway's 15 minute limit. Measured: about 100 kB in 2 s
// over WiFi / Ethernet and ~10 kB/s over LTE (PPP over the modem UART), so
// the largest file (86 s budget) needs 12 s there
constexpr uint32_t BUDGET_BASE_MS = 30000;
constexpr uint32_t MIN_RATE = 2048;         // bytes per second
constexpr uint32_t READ_TIMEOUT_MS = 10000; // HTTPClient's timeout of one read
constexpr uint32_t CONNECT_MS = 5000;       // HTTPClient's connect timeout
constexpr uint32_t HANDSHAKE_MS = 10000;    // TLS handshake timeout
// A redirect hop is started only with this much left before the deadline
// (its connect and TLS handshake end before it). A redirect to another
// host also looks that host up (lwIP's DNS retries, seconds) - the first
// host is looked up before the request
constexpr uint32_t HOP_RESERVE_MS = CONNECT_MS + HANDSHAKE_MS;
constexpr uint32_t budgetMs(uint32_t total)
{
    return BUDGET_BASE_MS + (uint32_t)(((uint64_t)total * 1000 + MIN_RATE - 1) / MIN_RATE);
}
// The budget, one read timeout past it (no connect or handshake starts
// unless it fits before the deadline: HOP_RESERVE_MS) and the rest of the
// session (entering at most 12 s, the end a few s) fit in the radio pause
// the job rule assumes. Residual, not covered: the DNS lookup of a
// redirect to another host runs inside its hop (lwIP retries, a few s per
// server), so a hop started just within the reserve can end that long
// after the deadline; the 20 s margin here absorbs a few seconds of it, a
// hostile server with a slow DNS name could stretch the pause a few
// seconds beyond SENSOR_FETCH_PAUSE_MAX_S (the Waiting job rule keeps two
// sensor intervals of margin)
static_assert(budgetMs(MAX_FILE) + READ_TIMEOUT_MS + 20000 <= UpdateModePolicy::SENSOR_FETCH_PAUSE_MAX_S * 1000,
              "sensor image download budget vs the radio pause of the job rule");

// The body after the download loop: exactly Content-Length bytes and
// nothing behind them (a short or long body is an error before the image
// is classified: a cut signed file is no unsigned file)
enum class End : uint8_t
{
    Ok,
    Short,
    Long,
};

inline End endCheck(uint32_t contentLength, uint32_t received, bool moreAvailable)
{
    if (received < contentLength)
    {
        return End::Short;
    }
    if (received > contentLength || moreAvailable)
    {
        return End::Long;
    }
    return End::Ok;
}

// A validated image (upload or download): stored, or refused by "Signed
// images only" (unsigned / unknown key), or refused because an unfinished
// job (or a queued start) uses the image of its HW type
enum class Verdict : uint8_t
{
    Store,
    SignedOnly,
    Busy,
};

inline Verdict verdict(OtaImageSigned::Class cls, bool signedOnly, bool hwBusy)
{
    if (!OtaImageSigned::storable(cls, signedOnly))
    {
        return Verdict::SignedOnly;
    }
    return hwBusy ? Verdict::Busy : Verdict::Store;
}

// GET /ota/fetch/status: {"state":..,"bytes":..,"total":..,"result":".."}.
// result: any text (quotes, backslashes and control characters are made
// safe; cut to fit). Returns the length (always < cap, cap >= 64)
constexpr size_t STATUS_MAX = 320;
inline size_t statusJson(char *out, size_t cap, State s, uint32_t bytes, uint32_t total, const char *result)
{
    int n = snprintf(out, cap, "{\"state\":\"%s\",\"bytes\":%lu,\"total\":%lu,\"result\":\"", stateName(s),
                     (unsigned long)bytes, (unsigned long)total);
    size_t o = n > 0 && (size_t)n < cap ? (size_t)n : 0;
    for (const char *p = result ? result : ""; *p && o + 3 < cap; p++)
    {
        const char c = *p;
        out[o++] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '\'' : c;
    }
    out[o++] = '"';
    out[o++] = '}';
    out[o] = 0;
    return o;
}
} // namespace SensorFetch
