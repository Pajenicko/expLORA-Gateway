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
               TxRequest &req)
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
        bool more = false;
        uint8_t tlv[Gen2Config::MAX_DOWN_PAYLOAD];
        size_t tlvLen = Gen2Config::buildDown(g.config, unixAtLaunch(rxTimeUs, w == 1), q, tlv, sizeof(tlv), more);
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

bool decodeUp(Gen2Sensor &g, const uint8_t *payload, size_t len, unsigned long nowMs)
{
    bool confirmed = false;
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
            continue;
        }
        if (t.channel != 0)
        {
            continue; // other channels: shown from lastUp on the detail page
        }
        switch (t.type)
        {
        case ep::up::kTemperature:
            g.temperature = (int16_t)ep::tlvU16(t);
            break;
        case ep::up::kHumidity:
            g.humidity = ep::tlvU16(t);
            break;
        case ep::up::kBattery:
            g.batteryMv = ep::tlvU16(t);
            break;
        case ep::up::kDownlinkRssi:
            g.downlinkRssi = (int8_t)t.value[0];
            break;
        case ep::up::kDownlinkSnr:
            g.downlinkSnr = (int8_t)t.value[0];
            break;
        default:
            break;
        }
    }
    return confirmed;
}

} // namespace Gen2Response
