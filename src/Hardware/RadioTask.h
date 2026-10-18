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
 * the radio. The task never takes sensorMutex, never touches LittleFS and
 * never transmits with a blocking call (protocol v2, docs/protocol-v2.md).
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

// loop(): log what the radio task counted since the last call (the task
// itself never logs - Logger formats Strings and takes the UART lock)
void logEvents(Logger &logger);
} // namespace RadioTask
