/**
 * expLORA Gateway
 *
 * ForwardUrl - the "Forward to URL" placeholders (both sensor
 * generations): the stored URL with *TEMP*, *HUM*, ... replaced by the
 * reading that was just received, sent as one GET (SensorManager::
 * forwardSensorData). Pure C++ for the native tests (test/test_forward_url).
 *
 * Placeholders and their format (the values are the corrected ones the
 * pages, /api and MQTT show):
 *   *TEMP* *HUM* *PRESS*  2 decimals    *PPM*  0    *LUX* 1
 *   *WIND_SPEED* 1  *WIND_DIR* 0  *RAIN* 1  *DAILY_RAIN* 1  *RAIN_RATE* 1
 *   *BAT* 2 decimals  *RSSI* 0  *SN* the ID as in /api (6 hex digits, a
 *   generation 2 DevID 8)  *TYPE* the deviceType number of /api (0 for
 *   generation 2)
 * A quantity without a current value - the sensor type does not have it,
 * or the generation 2 UP being forwarded did not carry it with a valid
 * value (left out, e.g. a failed measuring chip, or the invalid-value
 * sentinel) - is not replaced: the placeholder stays in the URL as it is
 * (the generation 1 behaviour), never an older number. A generation 2 UP
 * without any valid reading (only the battery / RSSI, or only invalid
 * values) is not forwarded at all (gen2Due). Text between asterisks that
 * is no placeholder is copied unchanged.
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
#include <cstring>

#include "Quantity.h"

namespace ForwardUrl
{
// The field on the sensor forms: empty, or http:// / https:// with at most
// MAX_LEN characters
constexpr size_t MAX_LEN = 200;
inline bool valid(const char *url, size_t len)
{
    return len == 0 || (len <= MAX_LEN && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0));
}

// Generation 2: fresh = Qty::mask() of the quantities the verified UP
// carried with a valid value (Gen2Response::decodeUp). Due only when one of
// them is a reading: the battery alone (sent even when the measuring chip
// failed) or the RSSI is no reason to call the URL
constexpr uint16_t GEN2_NO_READING = Qty::mask(Qty::Battery) | Qty::mask(Qty::Rssi);
inline bool gen2Due(uint16_t fresh)
{
    return (fresh & (uint16_t)~GEN2_NO_READING) != 0;
}
// ... and what may be sent: only those quantities (still current: qNow) and
// the RSSI of the frame
inline uint16_t gen2Valid(uint16_t fresh, uint16_t qNow)
{
    return (uint16_t)((fresh & qNow) | Qty::mask(Qty::Rssi));
}

// After a request without an answer (DNS, connect, TLS or timeout - the
// ones that block for seconds; any HTTP status is an answer) the sensor's
// forwards are skipped for BACKOFF_MS, so a dead URL does not block the
// gateway's main loop on every report (RAM only; both generations, keyed by
// the ID - a generation 1 serial number equal to a generation 2 DevID would
// share the entry). One entry per failing sensor; with all N in use the
// oldest goes.
constexpr uint32_t BACKOFF_MS = 5UL * 60 * 1000;
template <size_t N> class Backoff
{
public:
    bool skip(uint32_t id, uint32_t nowMs) const
    {
        for (const Entry &x : e)
        {
            if (x.atMs != 0 && x.id == id && nowMs - x.atMs < BACKOFF_MS)
            {
                return true;
            }
        }
        return false;
    }
    void result(uint32_t id, bool ok, uint32_t nowMs)
    {
        Entry *slot = nullptr;
        for (Entry &x : e)
        {
            if (x.atMs != 0 && x.id == id)
            {
                slot = &x;
                break;
            }
        }
        if (ok)
        {
            if (slot)
            {
                slot->atMs = 0;
            }
            return;
        }
        for (size_t i = 0; !slot && i < N; i++)
        {
            if (e[i].atMs == 0 || nowMs - e[i].atMs >= BACKOFF_MS)
            {
                slot = &e[i]; // free or expired
            }
        }
        if (!slot)
        {
            slot = &e[0]; // the oldest
            for (Entry &x : e)
            {
                if (nowMs - x.atMs > nowMs - slot->atMs)
                {
                    slot = &x;
                }
            }
        }
        slot->id = id;
        slot->atMs = nowMs ? nowMs : 1; // 0 = free
    }

private:
    struct Entry
    {
        uint32_t id = 0;
        uint32_t atMs = 0;
    };
    Entry e[N];
};

struct Values
{
    uint16_t valid = 0;         // Qty::mask() of the quantities with a current value
    float v[Qty::COUNT] = {};
    const char *sn = "";        // the ID as in /api
    unsigned type = 0;          // the deviceType number of /api
};

// The expanded URL in out (cap bytes incl. the terminator): its length, or
// 0 when it does not fit (nothing is sent then)
inline size_t expand(const char *t, const Values &val, char *out, size_t cap)
{
    struct Token
    {
        const char *name;
        int8_t q; // Qty::Id, -1 *SN*, -2 *TYPE*
        uint8_t decimals;
    };
    static const Token TOKENS[] = {
        {"TEMP", Qty::Temperature, 2}, {"HUM", Qty::Humidity, 2},        {"PRESS", Qty::Pressure, 2},
        {"PPM", Qty::Co2, 0},          {"LUX", Qty::Lux, 1},             {"WIND_SPEED", Qty::WindSpeed, 1},
        {"WIND_DIR", Qty::WindDir, 0}, {"RAIN", Qty::Rain, 1},           {"DAILY_RAIN", Qty::DailyRain, 1},
        {"RAIN_RATE", Qty::RainRate, 1}, {"BAT", Qty::Battery, 2},       {"RSSI", Qty::Rssi, 0},
        {"SN", -1, 0},                 {"TYPE", -2, 0},
    };
    size_t n = 0;
    while (*t)
    {
        char num[48];
        const char *piece = t;
        size_t len = 1;
        const char *end = *t == '*' ? strchr(t + 1, '*') : nullptr;
        for (const Token &k : TOKENS)
        {
            const size_t kl = strlen(k.name);
            if (!end || (size_t)(end - t - 1) != kl || memcmp(t + 1, k.name, kl) != 0)
            {
                continue;
            }
            if (k.q == -1)
            {
                snprintf(num, sizeof(num), "%s", val.sn);
            }
            else if (k.q == -2)
            {
                snprintf(num, sizeof(num), "%u", val.type);
            }
            else if (val.valid & Qty::mask((Qty::Id)k.q))
            {
                snprintf(num, sizeof(num), "%.*f", (int)k.decimals, (double)val.v[k.q]);
            }
            else
            {
                break; // no current value: the placeholder stays
            }
            piece = num;
            len = strlen(num);
            t = end;
            break;
        }
        if (n + len >= cap)
        {
            return 0;
        }
        memcpy(out + n, piece, len);
        n += len;
        t++;
    }
    if (cap)
    {
        out[n < cap ? n : cap - 1] = 0;
    }
    return n;
}
} // namespace ForwardUrl
