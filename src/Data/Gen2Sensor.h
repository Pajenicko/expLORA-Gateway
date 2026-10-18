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
#include "ExploraProtocol.h"
#include "../Protocol/Gen2Config.h"
#include "../Protocol/Gen2Session.h"

#ifndef MAX_GEN2_SENSORS
#define MAX_GEN2_SENSORS 10 // RAM budget of the Pro (no PSRAM); separate from MAX_SENSORS
#endif

struct Gen2Sensor
{
    // Persisted
    bool configured = false;
    uint32_t devId = 0;
    String name;
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
    // Last values reported in an UP (ep::kInvalid* when absent)
    int16_t temperature = INT16_MIN; // 0.01 degC
    uint16_t humidity = 0xFFFF;      // 0.01 %RH
    uint16_t batteryMv = 0xFFFF;
    int8_t downlinkRssi = INT8_MIN;  // dBm of the last downlink the sensor received
    int8_t downlinkSnr = INT8_MIN;   // 0.25 dB
    uint8_t lastUp[48] = {};         // TLVs of the last UP (all channels, shown on the detail page)
    uint8_t lastUpLen = 0;
    bool statusRequested = false;    // admin asked for a status report: sent in every DOWN until it arrives
    uint8_t status[48] = {};         // last status report (up::kStatus value)
    uint8_t statusLen = 0;
    unsigned long statusMs = 0;
    uint32_t configDowns = 0;        // DOWNs that carried the configuration
    unsigned long lastLogMs = 0;   // per-DevID log rate limit (one line per kind per 10 min)
    unsigned long lastWarnMs = 0;
    bool logged = false, warned = false;

    void wipeKeys()
    {
        ep::wipe(label, sizeof(label));
        ep::wipe(&joinKeys, sizeof(joinKeys));
        sessions.clear();
    }
};
