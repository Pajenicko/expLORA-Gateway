/**
 * expLORA Gateway
 *
 * Generation 2 sensor (protocol v2): identity, keys and reported status
 *
 * Persisted in sensors.json ("gen2" array): DevID, name, the label key L as
 * entered (shown back to the logged-in admin for checking against the
 * label), the last accepted devNonce and the desired configuration. K and
 * the join keys are derived at load time and never leave RAM; session keys
 * live in RAM only (docs/protocol-v2.md section 2).
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include "ExploraProtocol.h"
#include "../Protocol/Gen2Config.h"
#include "../Protocol/Gen2Link.h"
#include "../Protocol/Gen2Session.h"
#include "../Protocol/OtaJob.h"
#include "Quantity.h"
#include "SensorCalibration.h"

#ifndef MAX_GEN2_SENSORS
#define MAX_GEN2_SENSORS 10 // RAM budget of the Pro (no PSRAM); separate from MAX_SENSORS
#endif

// Clock of the link history (spec 4.3): seconds of the 64-bit esp_timer, no 49-day wrap
inline uint32_t gen2LinkNowS()
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

struct Gen2Sensor
{
    // Persisted
    bool configured = false;
    uint32_t devId = 0;
    String name;
    String customUrl; // "Forward to URL" with placeholders (Data/ForwardUrl.h), empty = off
    uint8_t label[ep::kLabelKeyLen] = {};
    bool hasDevNonce = false;
    uint32_t lastDevNonce = 0; // last devNonce accepted for a join
    // Firmware version from the last verified JOIN_REQ and the boot state from
    // the last status report (status bytes 22-23, spec 10.3); kept across a
    // gateway restart (the bootloader version only in RAM)
    bool hasFw = false;
    uint16_t hwType = 0;
    uint8_t fwMajor = 0, fwMinor = 0, fwPatch = 0;
    uint8_t bootState = 0xFF; // 0xFF unknown
    uint8_t blVersion = 0;    // bootloader version (major << 4 | minor), 0 unknown
    // OTA (spec 14.5): the image that reverted on this sensor - never retried
    // automatically, the admin may start it again explicitly with a warning
    bool hasReverted = false;
    uint32_t revertedFw = 0;
    uint32_t revertedCrc = 0;
    // Automatic updates (S6): failed automatic jobs of image version
    // autoFailFw (two stop them for that version; persisted)
    uint32_t autoFailFw = 0;
    uint8_t autoFails = 0;
    // Leftovers of this sensor's finished OTA job, kept here so that a job for
    // another sensor does not drop them (persisted): the revert watch (3 x
    // T_confirm, image version + CRC) and a pending abort
    OtaJob::Deadline otaWatch;
    uint32_t otaWatchFw = 0, otaWatchCrc = 0;
    OtaJob::Deadline otaAbort;

    // Quantities the sensor has reported (Home Assistant entities, the
    // correction fields; Qty::mask) and its corrections (generation 1 key names)
    uint16_t qMask = 0;
    SensorCalibration::Set cal;

    // Desired configuration (sent in JOIN_ACC and, on a version mismatch, in a DOWN)
    Gen2Config::Values config;
    uint8_t cfgVersion = 1;    // 1-15, raised on every change of config

    // Derived at load, RAM only
    ep::KeyPair joinKeys = {};
    Gen2SessionPair sessions; // current + pending (after a JOIN_ACC, until the first UP verifies)

    // Reported status, RAM only
    bool seen = false;
    unsigned long lastSeenMs = 0;
    int16_t rssi = 0;
    float snr = 0;
    float freqError = 0;
    uint8_t caps = 0;
    uint8_t configVersion = 0;     // reported by the sensor (JOIN_REQ, every UP)
    uint32_t lastJoinDevNonce = 0; // from the last verified JOIN_REQ
    uint32_t joinRequests = 0;     // verified JOIN_REQs
    uint32_t micFailures = 0;      // frames with this DevID whose MIC failed
    uint32_t upWithoutSession = 0; // UP frames without a current session (answered with REJOIN)
    uint32_t replayedJoins = 0;    // MIC-valid JOIN_REQs with devNonce <= the last accepted one
    uint32_t staleJoinStreak = 0;  // ... in a row without an accepted join (hint: sensor lost its store)
    uint32_t joinAccepts = 0;      // JOIN_ACC ACCEPT handed to the radio
    uint32_t rejoins = 0;          // REJOIN handed to the radio
    uint32_t joinLimited = 0;      // JOIN_REQ / UP left unanswered by the 10 s JOIN_ACC limit
    uint32_t noSlot = 0;           // frames that could not get a timed response (flash write, busy)
    // Separate 10 s limits: a forged UP (DevID in clear) that triggers REJOINs
    // must not hold back the answer to a MIC-verified JOIN_REQ
    unsigned long lastAcceptMs = 0, lastRejoinMs = 0;
    bool acceptSent = false, rejoinSent = false;
    uint32_t upFrames = 0;         // UP frames verified
    uint32_t lastFcnt = 0;
    uint32_t downRx1 = 0, downRx2 = 0, downNotSent = 0;
    uint8_t lastDownOutcome = 0xFF; // RadioTask::TxOutcome of the last response, 0xFF none
    bool sessionUp = false;         // an UP verified under the current session
    // Last readings, corrected (lastUp keeps the raw TLVs); valid for the
    // quantities in qNow (reported since this start, the last report not
    // an invalid-value sentinel); qBad: the last report was the sentinel
    // (Gen2Values::apply)
    float value[Qty::COUNT] = {};
    uint16_t qNow = 0;
    uint16_t qBad = 0;
    // MQTT (RAM): an UP with readings to publish; discovery again (the
    // reported quantities grew, a new firmware / HW type from a JOIN);
    // taken by SensorManager::takeGen2Mqtt
    bool mqttDue = false, mqttDiscover = false;
    // Forward to URL (RAM): an UP with a valid reading to send, fwdMask the
    // quantities it carried with a valid value (ForwardUrl::gen2Due /
    // gen2Valid); taken by SensorManager::forwardGen2 (loop task, after the
    // response)
    bool fwdDue = false;
    uint16_t fwdMask = 0;
    int8_t downlinkRssi = INT8_MIN;  // dBm of the last downlink the sensor received
    // Link history (spec 4.3): last 20 UPs - RSSI/SNR, reported downlink RSSI/SNR, TX power, losses
    Gen2Link::History link;
    int8_t upDownlinkRssi = Gen2Link::NONE; // downlink RSSI / SNR (whole dB) carried by the last UP
    int8_t upDownlinkSnr = Gen2Link::NONE;
    uint32_t linkLostPending = 0;           // UPs lost since the last sample (unsampled UPs carry it on)
    // TX power from the last status report sent while the sensor ran config
    // reportedTxVersion (an older firmware clamps to 14 dBm); INT8_MAX unknown
    int8_t reportedTx = INT8_MAX;
    uint8_t reportedTxVersion = 0;
    int8_t downlinkSnr = INT8_MIN;   // 0.25 dB
    uint8_t lastUp[48] = {};         // TLVs of the last UP (all channels, shown on the detail page)
    uint8_t lastUpLen = 0;
    bool statusRequested = false;    // admin asked for a status report: sent in every DOWN until it arrives
    uint8_t statusFlags = 0;         // flags byte of the last status report (spec 4.1)
    bool statusFw = false;           // the last status report carried a firmware version
    uint32_t otaStatuses = 0;        // OTA status TLVs received
    uint8_t status[48] = {};         // last status report (up::kStatus value)
    uint8_t statusLen = 0;
    unsigned long statusMs = 0;
    uint32_t configDowns = 0;        // DOWNs that carried the configuration
    unsigned long lastLogMs = 0;   // per-DevID log rate limit (one line per kind per 10 min)
    unsigned long lastWarnMs = 0;
    bool logged = false, warned = false;

    // TX power the sensor uses: the configured one, unless a status report
    // sent under the current configuration says otherwise (old firmware clamping)
    uint32_t fwVersion() const { return (uint32_t)fwMajor << 16 | (uint32_t)fwMinor << 8 | fwPatch; }

    int8_t effectiveTxPower() const
    {
        return reportedTx != INT8_MAX && reportedTxVersion == cfgVersion ? reportedTx : config.txPowerDbm;
    }

    void wipeKeys()
    {
        ep::wipe(label, sizeof(label));
        ep::wipe(&joinKeys, sizeof(joinKeys));
        sessions.clear();
    }
};
