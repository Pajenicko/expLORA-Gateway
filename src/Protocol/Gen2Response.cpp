/**
 * expLORA Gateway
 *
 * Generation 2 responses
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Gen2Response.h"
#include "Gen2Values.h"
#include "Gen2Crypto.h"
#include "Gen2Downlink.h"

#include <esp_random.h>
#include <esp_timer.h>
#include <sys/time.h>

namespace Gen2Response
{

static constexpr time_t VALID_TIME = 1700000000; // after 2023-11: SNTP has set the clock

uint32_t unixAtLaunch(int64_t rxTimeUs, bool rx2)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec < VALID_TIME)
    {
        return 0;
    }
    int64_t nowUs = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    int64_t launchUs = rxTimeUs + (rx2 ? Gen2Downlink::RX2_DELAY_US : Gen2Downlink::RX1_DELAY_US);
    int64_t unixUs = nowUs + (launchUs - esp_timer_get_time());
    return (uint32_t)((unixUs + 500000) / 1000000);
}

bool buildJoinAccept(Gen2Sensor &g, uint32_t devNonce, int64_t rxTimeUs, TxRequest &req)
{
    uint32_t gwNonce = esp_random();
    uint8_t tlv[32];
    for (int w = 0; w < 2; w++)
    {
        size_t tlvLen = Gen2Config::configTlvs(g.config, unixAtLaunch(rxTimeUs, w == 1), tlv, sizeof(tlv));
        uint8_t *out = w == 0 ? req.frame1 : req.frame2;
        size_t len = tlvLen == 0 ? 0
                                 : ep::buildJoinAccept(Gen2Crypto::aes, g.joinKeys, g.devId, devNonce, gwNonce,
                                                       g.cfgVersion, tlv, tlvLen, out, RADIO_TX_MAX);
        if (len == 0)
        {
            return false;
        }
        (w == 0 ? req.len1 : req.len2) = (uint8_t)len;
    }
    ep::KeyPair keys;
    Gen2Crypto::deriveSessionKeys(g, devNonce, gwNonce, keys);
    g.sessions.startPending(keys, devNonce);
    ep::wipe(&keys, sizeof(keys));
    req.devId = g.devId;
    req.priority = true;
    req.join = true;
    return true;
}

bool buildRejoin(const Gen2Sensor &g, const uint8_t *upFrame, size_t upLen, uint16_t upFcnt16, TxRequest &req)
{
    if (upLen < ep::kDataOverhead)
    {
        return false;
    }
    size_t len = ep::buildRejoin(Gen2Crypto::aes, g.joinKeys, g.devId, upFcnt16, upFrame + upLen - ep::kMicLen,
                                 req.frame1, RADIO_TX_MAX);
    if (len == 0)
    {
        return false;
    }
    memcpy(req.frame2, req.frame1, len);
    req.len1 = req.len2 = (uint8_t)len;
    req.devId = g.devId;
    req.priority = true;
    req.join = true;
    return true;
}

bool needsDown(const Gen2Sensor &g, bool ackRequested)
{
    return ackRequested || !Gen2Config::inSync(g.configVersion, g.cfgVersion) || g.statusRequested;
}

bool buildDown(Gen2Sensor &g, uint32_t upFcnt32, bool ackRequested, bool confirmed, int64_t rxTimeUs,
               TxRequest &req, const uint8_t *ota, uint8_t otaLen, bool otaOnly, bool *otaPlaced)
{
    Gen2Session &s = g.sessions.current;
    if (!s.valid)
    {
        return false;
    }
    // One downlink counter for both windows: only one of them is ever sent
    uint32_t fcnt = s.fcntDown++;
    bool configSent = false;
    for (int w = 0; w < 2; w++)
    {
        Gen2Config::Queue q;
        q.config = !Gen2Config::inSync(g.configVersion, g.cfgVersion);
        q.statusRequest = g.statusRequested;
        q.ota = ota;
        q.otaLen = otaLen;
        q.otaOnly = otaOnly;
        bool more = false;
        uint8_t tlv[Gen2Config::MAX_DOWN_PAYLOAD];
        size_t tlvLen = Gen2Config::buildDown(g.config, unixAtLaunch(rxTimeUs, w == 1), q, tlv, sizeof(tlv), more);
        if (w == 0 && otaPlaced != nullptr)
        {
            *otaPlaced = ota != nullptr && q.ota == nullptr; // an OTA TLV that did not fit waits for the next DOWN
        }
        configSent = configSent || (!q.config && !Gen2Config::inSync(g.configVersion, g.cfgVersion));
        uint8_t fctrl = (uint8_t)((ackRequested ? ep::kFctrlAck : 0) | (more ? ep::kFctrlPending : 0) |
                                  (g.cfgVersion & ep::kFctrlCfgMask));
        uint8_t *out = w == 0 ? req.frame1 : req.frame2;
        size_t len = ep::buildData(Gen2Crypto::aes, s.keys, ep::FrameType::Down, g.devId, fcnt, upFcnt32, fctrl, tlv,
                                   tlvLen, out, RADIO_TX_MAX);
        if (len == 0)
        {
            return false;
        }
        (w == 0 ? req.len1 : req.len2) = (uint8_t)len;
    }
    if (configSent)
    {
        g.configDowns++;
    }
    req.devId = g.devId;
    req.priority = confirmed; // ACK of a confirmed UP (status report, event) may pre-empt a reception
    req.join = false;
    return true;
}

bool decodeUp(Gen2Sensor &g, const uint8_t *payload, size_t len, unsigned long nowMs, OtaJob::Up *ota, bool *values)
{
    bool confirmed = false;
    bool pressure = false;  // a valid pressure in this UP (converted to the altitude below)
    bool hasValues = false; // a measured quantity (valid or invalid) in this UP
    g.value[Qty::Rssi] = g.rssi; // of this frame (set by the caller)
    g.qMask |= Qty::mask(Qty::Rssi);
    g.qNow |= Qty::mask(Qty::Rssi);
    g.lastUpLen = (uint8_t)(len < sizeof(g.lastUp) ? len : sizeof(g.lastUp));
    memcpy(g.lastUp, payload, g.lastUpLen);
    ep::TlvReader r(payload, len);
    ep::Tlv t;
    while (r.next(t) == 1)
    {
        if (t.type == ep::up::kEvent)
        {
            confirmed = true; // event ACKs pre-empt a reception (spec 7); events themselves: phase B
        }
        if (t.type == ep::up::kStatus)
        {
            confirmed = true;
            g.statusLen = (uint8_t)(t.len < sizeof(g.status) ? t.len : sizeof(g.status));
            memcpy(g.status, t.value, g.statusLen);
            g.statusMs = nowMs;
            g.statusRequested = false;
            Gen2Config::statusBootInfo(t.value, t.len, g.bootState, g.blVersion);
            if (t.len >= 24 && t.value[0] == 1)
            {
                g.statusFlags = t.value[21];
                g.statusFw = true;
                if (ota != nullptr)
                {
                    ota->hasStatus = true;
                    ota->hasFw = true;
                    ota->fw = (uint32_t)t.value[1] << 16 | (uint32_t)t.value[2] << 8 | t.value[3];
                    ota->boot = t.value[22];
                    ota->flags = t.value[21];
                }
            }
            if (t.len >= 16 && Gen2Config::inSync(g.configVersion, g.cfgVersion))
            {
                g.reportedTx = (int8_t)t.value[15]; // the power the sensor really uses (spec 4.3)
                g.reportedTxVersion = g.cfgVersion;
            }
            continue;
        }
        if (t.channel != 0)
        {
            continue; // other channels: shown from lastUp on the detail page
        }
        if (t.type == ep::up::kOtaStatus)
        {
            ep::ota::Status st;
            if (!ep::ota::decodeStatus(t.value, t.len, st))
            {
                continue; // unknown form: ignored
            }
            g.otaStatuses++;
            if (ota == nullptr)
            {
                continue;
            }
            switch (st.kind)
            {
            case ep::ota::StatusKind::Progress:
                ota->progress = true;
                ota->base = st.base;
                ota->window = st.window;
                ota->slot = st.slot;
                break;
            case ep::ota::StatusKind::Accepted:
                ota->accepted = true;
                ota->acceptedArg = st.arg;
                break;
            case ep::ota::StatusKind::Refused:
                ota->refused = true;
                ota->refusedArg = st.arg;
                break;
            case ep::ota::StatusKind::Complete:
                ota->complete = true;
                break;
            case ep::ota::StatusKind::Failed:
                ota->failed = true;
                ota->failedArg = st.arg;
                break;
            case ep::ota::StatusKind::Left:
                ota->left = true;
                ota->leftArg = st.arg;
                break;
            case ep::ota::StatusKind::BatteryLow:
                ota->batteryLow = true;
                ota->batteryArg = st.arg;
                break;
            default:
                break; // unknown kinds are skipped
            }
            continue;
        }
        // Corrected here, so the pages, /api and MQTT all see it; an invalid
        // value (sentinel) makes the quantity missing until the next value
        const Gen2Values::Decoded d = Gen2Values::apply(g, t);
        if (d != Gen2Values::Decoded::None)
        {
            hasValues = true;
            pressure = pressure || (d == Gen2Values::Decoded::Value && t.type == ep::up::kPressure);
            continue;
        }
        switch (t.type)
        {
        case ep::up::kDownlinkRssi:
            g.downlinkRssi = (int8_t)t.value[0];
            g.upDownlinkRssi = Gen2Link::History::clamp8(g.downlinkRssi);
            break;
        case ep::up::kDownlinkSnr:
            g.downlinkSnr = (int8_t)t.value[0];
            if (g.downlinkSnr != INT8_MIN) // invalid-value sentinel
            {
                int q = g.downlinkSnr;     // 0.25 dB -> whole dB, rounded down
                g.upDownlinkSnr = (int8_t)(q >= 0 ? q / 4 : -((-q + 3) / 4));
            }
            break;
        default:
            break;
        }
    }
    if (pressure)
    {
        Gen2Values::pressureToAltitude(g);
    }
    if (values != nullptr)
    {
        *values = hasValues;
    }
    return confirmed;
}

} // namespace Gen2Response
