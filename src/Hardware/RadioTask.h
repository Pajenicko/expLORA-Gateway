/**
 * expLORA Gateway
 *
 * Radio task - the only code that talks to the LoRa radio after setup()
 *
 * A statically created FreeRTOS task (core 1, priority RADIO_TASK_PRIORITY,
 * above AsyncTCP, below esp_timer) owns the radio driver and its SPI bus:
 * it reads every packet right after the RxDone interrupt, together with
 * RSSI, SNR, frequency error and the interrupt time, and queues it for
 * loop(). It also runs the periodic radio health check. A stalled loop()
 * (LTE modem, TLS handshake) therefore no longer loses or delays packets in
 * the radio. The task never takes sensorMutex, never touches LittleFS/NVS
 * and never logs (protocol v2, docs/protocol-v2.md section 7).
 *
 * Timed responses (protocol v2 downlink): for a frame with the generation 2
 * marker and a reliable timestamp the task takes the flash write permit at
 * RxDone (FlashPermit::tryHoldForRadio) and gives the packet a responseSlot.
 * loop() must then answer the slot - respond() with the finished frames
 * (built from the sensor model, well before RX1 - 20 ms) or cancel(). The
 * task only checks the duty cycle, stages and launches the frame on the
 * downlink channel and reports the outcome through takeResult().
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
#include "../Data/Logging.h"
#include "LoRaRadio.h"

constexpr UBaseType_t RADIO_TASK_PRIORITY = 14;
constexpr BaseType_t RADIO_TASK_CORE = 1;
constexpr uint32_t RADIO_TASK_STACK = 6144;     // bytes
constexpr UBaseType_t RADIO_QUEUE_LENGTH = 8;   // packets
constexpr uint32_t RADIO_HEALTH_CHECK_MS = 600000; // radio working
constexpr uint32_t RADIO_RETRY_MS = 30000;         // radio not initialized / not answering
constexpr size_t RADIO_TX_MAX = 96;                // longest downlink frame staged by the task

// One received packet with its reception metadata
struct RadioPacket
{
    uint8_t data[255];
    uint8_t length;
    int16_t rssi;      // dBm
    float snr;         // dB
    float freqError;   // Hz (transmitter vs. gateway)
    int64_t rxTimeUs;  // esp_timer time of the RxDone interrupt
    bool timeReliable; // false: RxDone may have been held off by a flash write
    uint32_t responseSlot; // != 0: a timed response is possible; loop() must respond() or cancel()
};

// A response for one responseSlot, built by loop()
struct TxRequest
{
    uint32_t slot;
    uint32_t devId;
    bool priority; // JOIN_ACC, REJOIN, event ACK: may pre-empt a reception in progress
    bool join;     // counts against the JOIN_ACC share of the duty cycle
    uint8_t len1;  // RX1 frame (T + 1000 ms)
    uint8_t len2;  // RX2 frame (T + 2000 ms; the time TLV one second later), 0 = no RX2
    uint8_t frame1[RADIO_TX_MAX];
    uint8_t frame2[RADIO_TX_MAX];
};

enum class TxOutcome : uint8_t
{
    Rx1,         // sent in RX1
    Rx2,         // sent in RX2
    LateRequest, // the request came after the last window could be prepared
    Busy,        // a reception was in progress in both windows
    Budget,      // duty cycle: data share exhausted
    JoinBudget,  // duty cycle: JOIN_ACC share exhausted
    Pacing,      // duty cycle: off-time after the previous downlink too short
    NoHold,      // a flash write was running at T2 - 20 ms
    Late,        // the launch would have been > 40 ms late (flash stall, preemption)
    Failed,      // driver error
};

struct TxResult
{
    uint32_t slot;
    uint32_t devId;
    TxOutcome outcome;
    bool join;
    uint16_t airtimeMs; // 0 when nothing was sent
};


namespace RadioTask
{
struct Stats
{
    uint32_t received;      // packets queued
    uint32_t crcErrors;     // RxDone with a CRC error
    uint32_t readErrors;    // RxDone without a valid packet (read error, length)
    uint32_t queueDrops;    // queue full (loop() stalled for more than 8 packets): newest dropped
    uint32_t queueHigh;     // most packets waiting at once
    uint32_t maxLatencyMs;  // longest RxDone -> loop() pickup (packets received after loop() started)
    uint32_t resetsOk;      // health check re-initializations that worked
    uint32_t resetsFailed;  // ... that failed (retried every RADIO_RETRY_MS)
    uint32_t selfHeals;     // RxDone found by polling the DIO line (missed interrupt edge)
    uint32_t unreliableTime; // RxDone during / right after a flash write (no timed response possible)
    uint32_t holdFails;     // gen2 frame, reliable time, but the permit could not be taken at RxDone
    uint32_t slotsBusy;     // gen2 frame while another response was pending (no timed response)
    uint32_t txRx1, txRx2;  // responses sent per window
    uint32_t txBusy, txBudget, txPacing, txLate, txNoHold, txFailed, txLateRequest; // responses not sent
    uint32_t txDoneTimeouts; // no TxDone within airtime + 100 ms
    uint32_t dutyUsedMs;    // downlink airtime in the last hour (all)
    uint32_t joinUsedMs;    // ... JOIN_ACC / REJOIN share
    RadioError lastError;   // last driver error (read or reset)
    uint32_t stackFree;     // task stack high-water mark, bytes
};

// Start the task for an initialized radio (once, from setup())
bool start(LoRaRadio *radio, Logger &logger);

// loop(): next queued packet, false when none. The first call marks the
// start of loop(): packets received while setup() was still running (up to
// ~1 s after the task started) are not counted in maxLatencyMs.
bool receive(RadioPacket &packet);

Stats stats();

// loop(): answer a responseSlot. false: queue full (the slot then expires)
bool respond(const TxRequest &request);
// loop(): no response to this slot (releases the flash permit at once)
void cancel(uint32_t slot);
// loop(): outcome of a respond(), false when none is waiting
bool takeResult(TxResult &result);
const char *txOutcomeName(TxOutcome outcome);

// loop(): log what the radio task counted since the last call (the task
// itself never logs - Logger formats Strings and takes the UART lock)
void logEvents(Logger &logger);
} // namespace RadioTask
