/**
 * expLORA Gateway
 *
 * Generation 2 sensor configuration (protocol v2, docs/protocol-v2.md 4.2)
 *
 * The desired configuration of one sensor, its version logic (4 bits,
 * gateway versions 1-15, the sensor reports the version it applied in every
 * UP), the limits the web form enforces and the DOWN payload builder: ACK,
 * complete configuration on a version mismatch, queued commands, "more
 * pending" when not everything fits one frame. Pure C++ (native tests).
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
#include "ExploraProtocol.h"

namespace Gen2Config
{
// Limits (web form and JSON load)
constexpr uint16_t MIN_INTERVAL_S = 60;
constexpr uint16_t MAX_INTERVAL_S = 43200;          // 12 h
constexpr uint16_t DUTY_SAFE_INTERVAL_S = 300;     // below: the sensor stretches to its 0.1 % limit
constexpr int8_t MIN_TX_POWER_DBM = -9;            // sensor hardware range (STM32WLE5 high-power PA)
constexpr int8_t MAX_TX_POWER_DBM = 22;
constexpr int8_t DEFAULT_TX_POWER_DBM = 14;
constexpr int8_t EU_TX_POWER_DBM = 14;             // EU limit on 868.0 MHz: 14 dBm ERP (spec 1); above: warning
constexpr uint8_t MAX_RETRIES = 15;
constexpr uint8_t MIN_BACKOFF_S = 1;
constexpr uint8_t MAX_BACKOFF_S = 15;
// DOWN payload limit: keeps one response at about 250 ms on air
constexpr size_t MAX_DOWN_PAYLOAD = 40;

struct Values
{
    uint16_t intervalS = 600;
    uint16_t slotS = 0;
    int8_t txPowerDbm = DEFAULT_TX_POWER_DBM;
    uint8_t ackEvery = 6;
    uint8_t ackRetries = 3;
    uint8_t ackBackoffS = 5;

    bool operator==(const Values &o) const
    {
        return intervalS == o.intervalS && slotS == o.slotS && txPowerDbm == o.txPowerDbm &&
               ackEvery == o.ackEvery && ackRetries == o.ackRetries && ackBackoffS == o.ackBackoffS;
    }
    bool operator!=(const Values &o) const { return !(*this == o); }
};

// Bring values into the allowed ranges (hardware range for the power);
// true when nothing had to change
inline bool clamp(Values &v)
{
    Values in = v;
    if (v.intervalS < MIN_INTERVAL_S)
        v.intervalS = MIN_INTERVAL_S;
    if (v.intervalS > MAX_INTERVAL_S)
        v.intervalS = MAX_INTERVAL_S;
    if (v.slotS >= v.intervalS)
        v.slotS = (uint16_t)(v.slotS % v.intervalS);
    if (v.txPowerDbm < MIN_TX_POWER_DBM)
        v.txPowerDbm = MIN_TX_POWER_DBM;
    if (v.txPowerDbm > MAX_TX_POWER_DBM)
        v.txPowerDbm = MAX_TX_POWER_DBM;
    if (v.ackRetries > MAX_RETRIES)
        v.ackRetries = MAX_RETRIES;
    if (v.ackBackoffS < MIN_BACKOFF_S)
        v.ackBackoffS = MIN_BACKOFF_S;
    if (v.ackBackoffS > MAX_BACKOFF_S)
        v.ackBackoffS = MAX_BACKOFF_S;
    return in == v;
}

// Gateway versions are 1-15 (0 = the sensor reverted or has none). The
// next version never equals the one the sensor reports, so 15 changes while
// it is offline cannot wrap back to a number it already runs.
inline uint8_t nextVersion(uint8_t v, uint8_t reported)
{
    reported &= ep::kFctrlCfgMask;
    uint8_t n = (uint8_t)(v >= 15 ? 1 : v + 1);
    if (n == reported)
    {
        n = (uint8_t)(n >= 15 ? 1 : n + 1);
    }
    return n;
}

inline bool validVersion(uint8_t v)
{
    return v >= 1 && v <= 15;
}

// The sensor runs the desired configuration
inline bool inSync(uint8_t reported, uint8_t desired)
{
    return (reported & ep::kFctrlCfgMask) == desired;
}

// Complete configuration TLVs (interval + slot, TX power, ACK policy, time)
inline size_t configTlvs(const Values &v, uint32_t unixTime, uint8_t *out, size_t cap)
{
    ep::TlvWriter w(out, cap);
    w.addU32(ep::down::kInterval, ((uint32_t)v.intervalS << 16) | v.slotS);
    w.addI8(ep::down::kTxPower, v.txPowerDbm);
    w.addU16(ep::down::kAckPolicy,
             (uint16_t)(v.ackEvery << 8 | (v.ackRetries & 0x0F) << 4 | (v.ackBackoffS & 0x0F)));
    if (unixTime != 0)
    {
        w.addU32(ep::down::kTime, unixTime);
    }
    return w.ok() ? w.size() : 0;
}

// What the gateway still has to tell a sensor
struct Queue
{
    bool config = false;        // configuration differs from the reported version
    bool statusRequest = false; // admin asked for a status report
};

// DOWN payload: time first (cheap, keeps the clock right), then the complete
// configuration, then commands. Each part goes in whole or waits; the parts
// sent are cleared from q, morePending tells the sensor to poll again.
inline size_t buildDown(const Values &v, uint32_t unixTime, Queue &q, uint8_t *out, size_t cap, bool &morePending)
{
    if (cap > MAX_DOWN_PAYLOAD)
    {
        cap = MAX_DOWN_PAYLOAD;
    }
    size_t len = 0;
    if (unixTime != 0)
    {
        ep::TlvWriter w(out, cap);
        if (w.addU32(ep::down::kTime, unixTime))
        {
            len = w.size();
        }
    }
    if (q.config)
    {
        uint8_t tmp[24];
        size_t n = configTlvs(v, 0, tmp, sizeof(tmp));
        if (n > 0 && len + n <= cap)
        {
            for (size_t i = 0; i < n; i++)
            {
                out[len + i] = tmp[i];
            }
            len += n;
            q.config = false;
        }
    }
    if (q.statusRequest && len + 2 <= cap)
    {
        out[len++] = ep::down::kStatusRequest;
        out[len++] = 0;
        q.statusRequest = false;
    }
    morePending = q.config || q.statusRequest;
    return len;
}
// Status report (up::kStatus) bytes 22-23: boot state and bootloader
// version (spec 4.1 / 10.3). A shorter (22-byte) report from older firmware
// clears them: 0xFF = unknown, 0 = unknown bootloader.
inline void statusBootInfo(const uint8_t *value, size_t len, uint8_t &bootState, uint8_t &blVersion)
{
    if (len >= 24 && value[0] == 1)
    {
        bootState = value[22];
        blVersion = value[23];
    }
    else
    {
        bootState = 0xFF;
        blVersion = 0;
    }
}
} // namespace Gen2Config
