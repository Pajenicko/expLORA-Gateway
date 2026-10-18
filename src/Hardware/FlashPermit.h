/**
 * expLORA Gateway
 *
 * Flash write permit
 *
 * Erasing or programming the flash stops both CPU cores' code fetch and
 * defers GPIO interrupts (the Arduino GPIO dispatcher is not in IRAM), which
 * would delay a timed radio response by tens of milliseconds (protocol v2,
 * docs/protocol-v2.md section 7). The rules:
 *
 * - Once launchMode() has been issued the chip transmits on its own; a flash
 *   write then only delays the TxDone handling. The radio task therefore
 *   holds the permit from RxDone until the launch of the RX1 response
 *   (RADIO_HOLD_RX1_MS) and, only when RX2 is used, again from T2 - 20 ms
 *   until the RX2 launch (RADIO_HOLD_RX2_MS). It never waits:
 *   tryHoldForRadio() returns at once, and a failed try means "no timed
 *   response to this uplink".
 * - Every flash writer (LittleFS, Preferences/NVS, OTA, otadata) takes the
 *   permit first with FlashWriteGuard, before any other lock (lock order:
 *   permit, then sensorMutex / configMutex). It waits at most
 *   WRITER_TIMEOUT_MS and then writes anyway (logged once): a missed
 *   response is better than a lost setting. Nested guards of a task that is
 *   already writing without the permit do not wait again.
 *   A writer's section opens before it waits, so a waiting writer already
 *   makes tryHoldForRadio() fail and timestamps unreliable.
 * - OTA chunks are written from the AsyncTCP task; during a pending downlink
 *   that task may block for up to one hold. Accepted: downlinks are rare and
 *   Update.write() does not tell when its 4 kB buffer will hit the flash.
 * - A write already running delays the RxDone interrupt itself. The
 *   interrupt samples flashWriteSections (open write sections) next to its
 *   timestamp; FlashPermit keeps [start, end] of the last section.
 *   timestampReliable() combines both (FlashTiming.h).
 * - Not covered: flash writes inside ESP-IDF (PHY calibration data, NVS of
 *   other components). WiFi credential persistence is switched off.
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
#include "FlashTiming.h"

namespace FlashPermit
{
constexpr uint32_t RADIO_HOLD_RX1_MS = 1010;  // RxDone -> RX1 launch (T = RxDone + 1000 ms - C_chip)
constexpr uint32_t RADIO_HOLD_RX2_MS = 25;    // T2 - 20 ms -> RX2 launch
constexpr uint32_t WRITER_TIMEOUT_MS = 1500;  // writer wait before writing anyway (> RADIO_HOLD_RX1_MS)

// Number of open write sections and of closed ones; read by the DIO
// interrupt (plain DRAM variables, no function call in the ISR)
extern volatile uint32_t flashWriteSections;
extern volatile uint32_t flashSectionsClosed;

void setLogger(Logger *logger);

// Radio task: take the permit if no write is in progress (never blocks).
// Fails while any write section is open - a writer that timed out writes
// without the mutex. M3 rule: a timed response is sent only when
// packet.timeReliable && tryHoldForRadio().
bool tryHoldForRadio();
void releaseFromRadio();

// rxTimeUs, whether a write section was open and the closed-section count
// when the DIO interrupt ran
bool timestampReliable(int64_t rxTimeUs, bool writeActiveAtIsr, uint32_t closedAtIsr);

// Diagnostics
uint32_t writerWaits();    // writers that had to wait (radio hold or another writer)
uint32_t writerTimeouts(); // writers that gave up waiting and wrote anyway

// Writer side (recursive: nested guards in one task are fine). Returns false
// after a timeout; the write section is open either way and must be closed
// with releaseAfterWrite(<return value>)
bool acquireForWrite(uint32_t timeoutMs = WRITER_TIMEOUT_MS);
void releaseAfterWrite(bool permitHeld);
} // namespace FlashPermit

// RAII guard for a flash write section
class FlashWriteGuard
{
public:
    FlashWriteGuard() : held(FlashPermit::acquireForWrite()) {}
    ~FlashWriteGuard() { FlashPermit::releaseAfterWrite(held); }
    FlashWriteGuard(const FlashWriteGuard &) = delete;
    FlashWriteGuard &operator=(const FlashWriteGuard &) = delete;

private:
    bool held;
};
