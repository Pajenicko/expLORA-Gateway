/**
 * expLORA Gateway
 *
 * Generation 2 link budget: uplink / downlink margin of one sensor and the
 * TX power recommendation shown on its page (docs/protocol-v2.md 4.3).
 * A hint only - the gateway never changes the power by itself.
 *
 * History: the last HISTORY uplinks (RSSI and SNR at the gateway, the
 * downlink RSSI / SNR the sensor reported in that UP, the sensor's TX power
 * at the time, UPs lost before it), RAM only. Evaluated over the samples of
 * the last 7 days, so the worst value comes from the last 20 UPs or the last
 * 7 days, whichever holds fewer. Timestamps are seconds of a 64-bit
 * monotonic clock (the caller passes them), stored as uint32 - no wrap in
 * practice. Uplink samples are normalised to the current TX power (an RSSI
 * measured at 8 dBm counts 6 dB higher when the sensor now sends at 14 dBm).
 * Pure C++ (native tests).
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
#include "Gen2Config.h"

namespace Gen2Link
{
// LoRa SF9 / 125 kHz limits, nominal (Semtech SX1276 datasheet: sensitivity
// -129 dBm, demodulation down to SNR -12.5 dB; the SX1262 / STM32WL figures
// are about the same - one value for both directions; -12 keeps whole dB
// and is 0.5 dB conservative)
constexpr int SENSITIVITY_DBM = -129;
constexpr int SNR_LIMIT_DB = -12;
// The reported SNR is a lower bound of the true SNR: the reading saturates
// at about +10 dB on a strong signal (bench: +10...+12 dB from -41 to
// -69 dBm) and is never above the truth. Normalised to a higher power it
// stays a bound (snr + shift); normalised to a lower power only a reading
// below saturation does (a saturated one is skipped). While the reading
// saturates, a Lower hint therefore steps at most (worst SNR + 12 - 15) dB
// at a time and the samples at the new power decide the next step. A skipped
// reading still proves "true SNR >= reading at its own power": the Lower
// step is also limited by the worst snr + shift over all samples
// (lowerBoundSnr), so an old deep fade is never pushed below the limit.
constexpr int SNR_SAT_DB = 10;
constexpr int FADE_MARGIN_DB = 15;   // margin kept after lowering
constexpr int RAISE_BELOW_DB = 10;   // recommend raising below this margin
constexpr int MIN_STEP_DB = 3;       // smaller changes are not worth a hint
constexpr uint8_t HISTORY = 20;
constexpr uint32_t WINDOW_S = 7UL * 24UL * 3600UL;
constexpr uint8_t MIN_UP_SAMPLES = 5;
constexpr uint8_t MIN_DOWN_SAMPLES = 3;
constexpr int8_t NONE = INT8_MAX;    // value absent (127 dBm / dB never occurs)

struct History
{
    uint32_t s[HISTORY] = {};
    int8_t up[HISTORY] = {};      // RSSI at the gateway, dBm
    int8_t upSnr[HISTORY] = {};   // SNR at the gateway, whole dB (floor)
    int8_t down[HISTORY] = {};    // downlink RSSI reported by the sensor, dBm, or NONE
    int8_t downSnr[HISTORY] = {}; // downlink SNR reported by the sensor, whole dB, or NONE
    int8_t tx[HISTORY] = {};      // sensor TX power when the UP was sent, dBm
    uint8_t lost[HISTORY] = {};   // UPs lost (FCnt gap) since the previous verified UP
    uint8_t next = 0;
    uint8_t count = 0;

    void clear()
    {
        next = 0;
        count = 0;
    }

    // A sensor silent for longer than the window starts a fresh history
    void add(uint32_t nowS, int rssi, int snr, int8_t downRssi, int8_t downSnrDb, int8_t txPower, uint32_t lostUps)
    {
        if (count > 0 && nowS - s[(uint8_t)(next + HISTORY - 1) % HISTORY] > WINDOW_S)
        {
            clear();
        }
        s[next] = nowS;
        up[next] = clamp8(rssi);
        upSnr[next] = clamp8(snr);
        down[next] = downRssi;
        downSnr[next] = downSnrDb;
        tx[next] = txPower;
        lost[next] = (uint8_t)(lostUps > 255 ? 255 : lostUps);
        next = (uint8_t)((next + 1) % HISTORY);
        if (count < HISTORY)
        {
            count++;
        }
    }

    static int8_t clamp8(int v)
    {
        return (int8_t)(v < -128 ? -128 : (v > 126 ? 126 : v)); // 127 is NONE
    }
};

struct Margins
{
    uint8_t upSamples = 0;     // samples within the window
    uint8_t upSnrSamples = 0;  // ... whose SNR gives a lower bound at the current power
    uint8_t downSamples = 0;   // ... that carried a downlink RSSI
    uint8_t downSnrSamples = 0;
    uint32_t lost = 0;         // UPs lost within the window
    int worstUp = 0;           // dBm, normalised to the current TX power
    int worstUpSnr = 0;        // dB, lower bound at the current power (valid when upSnrSamples > 0)
    int worstDown = 0;         // dBm
    int worstDownSnr = 0;      // dB (valid when downSnrSamples > 0)
    int lowerBoundSnr = 0;     // worst snr + shift over all samples, skipped ones too (limits Lower only)
    bool upValid() const { return upSamples >= MIN_UP_SAMPLES; }
    bool downValid() const { return downSamples >= MIN_DOWN_SAMPLES; }
    bool upSnrLimits() const { return upSnrSamples > 0 && worstUpSnr - SNR_LIMIT_DB < worstUp - SENSITIVITY_DBM; }
    bool downSnrLimits() const
    {
        return downSnrSamples > 0 && worstDownSnr - SNR_LIMIT_DB < worstDown - SENSITIVITY_DBM;
    }
    // Margin = the smaller of the RSSI margin over the sensitivity and the SNR
    // margin over the demodulation limit
    int upMargin() const { return upSnrLimits() ? worstUpSnr - SNR_LIMIT_DB : worstUp - SENSITIVITY_DBM; }
    int downMargin() const
    {
        return downSnrLimits() ? worstDownSnr - SNR_LIMIT_DB : worstDown - SENSITIVITY_DBM;
    }
};

inline Margins margins(const History &h, uint32_t nowS, int txNow)
{
    Margins m;
    for (uint8_t i = 0; i < h.count; i++)
    {
        uint8_t k = (uint8_t)((h.next + HISTORY - 1 - i) % HISTORY);
        if (nowS - h.s[k] > WINDOW_S)
        {
            break; // chronological ring: older entries are older still
        }
        const int shift = txNow - h.tx[k];
        const int up = h.up[k] + shift;
        if (m.upSamples == 0 || up < m.worstUp)
        {
            m.worstUp = up;
        }
        m.upSamples++;
        m.lost += h.lost[k];
        if (m.upSamples == 1 || h.upSnr[k] + shift < m.lowerBoundSnr)
        {
            m.lowerBoundSnr = h.upSnr[k] + shift;
        }
        if (shift >= 0 || h.upSnr[k] < SNR_SAT_DB)
        {
            const int snr = h.upSnr[k] + shift;
            if (m.upSnrSamples == 0 || snr < m.worstUpSnr)
            {
                m.worstUpSnr = snr;
            }
            m.upSnrSamples++;
        }
        if (h.down[k] != NONE)
        {
            if (m.downSamples == 0 || h.down[k] < m.worstDown)
            {
                m.worstDown = h.down[k];
            }
            m.downSamples++;
        }
        if (h.downSnr[k] != NONE) // the gateway's power does not change: always a lower bound
        {
            if (m.downSnrSamples == 0 || h.downSnr[k] < m.worstDownSnr)
            {
                m.worstDownSnr = h.downSnr[k];
            }
            m.downSnrSamples++;
        }
    }
    return m;
}

enum class Advice : uint8_t
{
    None,      // too few samples, or the power fits
    Lower,     // margin stays >= FADE_MARGIN_DB at `power`
    Raise,     // margin below RAISE_BELOW_DB: `power` restores FADE_MARGIN_DB (or the maximum)
    AtMaximum, // margin below RAISE_BELOW_DB at the maximum power already
};

struct Recommendation
{
    Advice advice = Advice::None;
    int power = 0; // recommended TX power, dBm
};

// Uses the uplink margin only (the downlink is sent by the gateway at its own
// power; its margin is shown, not acted on). No Lower while UPs were lost in
// the window (the received samples alone would flatter a lossy link) and no
// Lower without an SNR bound at the current power.
inline Recommendation recommend(const Margins &m, int txNow)
{
    Recommendation r;
    if (!m.upValid())
    {
        return r;
    }
    const int margin = m.upMargin();
    if (margin - FADE_MARGIN_DB >= MIN_STEP_DB)
    {
        if (m.lost > 0 || m.upSnrSamples == 0)
        {
            return r; // a lossy link, or no SNR bound at this power (only saturated
                      // readings taken at a higher power): wait for samples at this power
        }
        int target = txNow - (margin - FADE_MARGIN_DB);
        // every sample, also a skipped saturated one, keeps its SNR bound
        // >= SNR_LIMIT_DB + FADE_MARGIN_DB at the new power
        const int boundTarget = txNow - (m.lowerBoundSnr - SNR_LIMIT_DB - FADE_MARGIN_DB);
        if (target < boundTarget)
        {
            target = boundTarget;
        }
        if (target < Gen2Config::MIN_TX_POWER_DBM)
        {
            target = Gen2Config::MIN_TX_POWER_DBM;
        }
        if (txNow - target >= MIN_STEP_DB)
        {
            r.advice = Advice::Lower;
            r.power = target;
        }
        return r;
    }
    if (margin < RAISE_BELOW_DB)
    {
        if (txNow >= Gen2Config::MAX_TX_POWER_DBM)
        {
            r.advice = Advice::AtMaximum;
            r.power = Gen2Config::MAX_TX_POWER_DBM;
            return r;
        }
        int target = txNow + (FADE_MARGIN_DB - margin);
        r.advice = Advice::Raise;
        r.power = target > Gen2Config::MAX_TX_POWER_DBM ? Gen2Config::MAX_TX_POWER_DBM : target;
    }
    return r;
}

// UPs lost between two verified UPs of the same session (every transmission,
// retransmissions included, has a new FCnt - spec 3.2); the first UP of a new
// session counts the FCnts before it
inline uint32_t lostBetween(bool sameSession, uint32_t previous, uint32_t fcnt32)
{
    if (!sameSession)
    {
        return fcnt32;
    }
    return fcnt32 > previous ? fcnt32 - previous - 1 : 0;
}

} // namespace Gen2Link
