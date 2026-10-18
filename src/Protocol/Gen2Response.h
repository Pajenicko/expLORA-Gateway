/**
 * expLORA Gateway
 *
 * Generation 2 responses (protocol v2, docs/protocol-v2.md section 5)
 *
 * Builds the frames the radio task sends in RX1 / RX2 from the sensor model:
 * JOIN_ACC ACCEPT, REJOIN and the DOWN acknowledging an UP. Runs in loop()
 * under sensorMutex; the radio task only stages and launches the result.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "../Data/Gen2Sensor.h"
#include "../Hardware/RadioTask.h"

namespace Gen2Response
{
// At most one JOIN_ACC ACCEPT and one REJOIN per DevID per 10 s, counted
// separately (spec 5.1)
constexpr unsigned long JOIN_ACC_INTERVAL_MS = 10000;

// Unix time at the start of a transmission in the given window, 0 when the
// gateway has no valid time
uint32_t unixAtLaunch(int64_t rxTimeUs, bool rx2);

// JOIN_ACC ACCEPT for a verified JOIN_REQ: draws gwNonce, starts the pending
// session and fills both windows. false: nothing changed
bool buildJoinAccept(Gen2Sensor &g, uint32_t devNonce, int64_t rxTimeUs, TxRequest &req);

// REJOIN bound to the triggering UP (FCnt16 + MIC), same frame in both windows
bool buildRejoin(const Gen2Sensor &g, const uint8_t *upFrame, size_t upLen, uint16_t upFcnt16, TxRequest &req);

// A verified UP needs a DOWN: ACK requested, configuration version
// differs, or a command is queued (spec 5.2)
bool needsDown(const Gen2Sensor &g, bool ackRequested);

// DOWN answering a verified UP: ACK bit when requested, time, the complete
// configuration when the reported version differs, queued commands, "more
// pending" when not everything fits. Uses the next downlink counter.
// confirmed: the UP carried a status report or an event - its ACK has
// priority over a reception in progress (spec 7)
bool buildDown(Gen2Sensor &g, uint32_t upFcnt32, bool ackRequested, bool confirmed, int64_t rxTimeUs,
               TxRequest &req);

// Store the reported values of a decrypted UP payload in the status; true
// when it carried a status report or an event (confirmed UP)
bool decodeUp(Gen2Sensor &g, const uint8_t *payload, size_t len, unsigned long nowMs);
} // namespace Gen2Response
