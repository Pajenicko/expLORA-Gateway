/**
 * expLORA Gateway
 *
 * Generation 2 sensor (protocol v2): identity, keys and reported status
 *
 * Persisted in sensors.json ("gen2" array): DevID, name, the label key L as
 * entered (shown back to the logged-in admin for checking against the
 * label) and the last accepted devNonce. K and the join keys are derived at
 * load time and never leave RAM. Sessions are never persisted
 * (docs/protocol-v2.md section 2).
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
    uint32_t lastDevNonce = 0; // last devNonce accepted for a join (M3)

    // Derived at load, RAM only
    ep::KeyPair joinKeys = {};

    // Reported status, RAM only
    bool seen = false;
    unsigned long lastSeenMs = 0;
    int16_t rssi = 0;
    float snr = 0;
    float freqError = 0;
    uint16_t hwType = 0;
    uint8_t fwMajor = 0, fwMinor = 0, fwPatch = 0;
    uint8_t caps = 0;
    uint8_t configVersion = 0;
    uint32_t lastJoinDevNonce = 0; // from the last verified JOIN_REQ
    uint32_t joinRequests = 0;     // verified JOIN_REQs
    uint32_t micFailures = 0;      // frames with this DevID whose MIC failed
    uint32_t upWithoutSession = 0; // UP frames before a session exists (REJOIN in M3)
    uint32_t replayedJoins = 0;    // MIC-valid JOIN_REQs with an old devNonce (replays)
    unsigned long lastLogMs = 0;   // per-DevID log rate limit (one line per kind per 10 min)
    unsigned long lastWarnMs = 0;
    bool logged = false, warned = false;

    void wipeKeys()
    {
        ep::wipe(label, sizeof(label));
        ep::wipe(&joinKeys, sizeof(joinKeys));
    }
};
