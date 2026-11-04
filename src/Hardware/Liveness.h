/**
 * expLORA Gateway
 *
 * Liveness - restarts the gateway when the lwIP thread or the radio task
 * stops making progress
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <Arduino.h>
#include "../Data/Logging.h"

/*
 * The task watchdog does not watch the CPU0 idle task (main.cpp: a LAN
 * connection flood keeps the lwIP thread busy on CPU0 for as long as it
 * lasts), and nothing else noticed a hung lwIP thread: the AsyncTCP task
 * feeds its watchdog on a 1 s queue timeout, the loop task touches lwIP
 * only through the uplink's DNS / route check (skipped without an uplink,
 * e.g. AP-only recovery) or inside WatchdogPause. The radio task was not
 * watched at all.
 *
 * A 1 s esp_timer (the esp_timer task: priority 22, above the lwIP thread,
 * independent of loop() and of WatchdogPause):
 * - posts a preallocated tcpip callback that advances a counter in the
 *   lwIP thread. A probe the mailbox accepted that has not run after
 *   TCPIP_HANG_MS restarts the gateway; a post the mailbox refuses (full)
 *   counts as busy, and with no probe pending only TCPIP_BUSY_HANG_MS
 *   without the counter advancing (every post refused that long) restarts
 *   it
 * - samples the radio task's loop counter (it passes its loop at least
 *   once a second; TX waits and flash permits stay below a few seconds):
 *   no advance for RADIO_HANG_MS restarts the gateway
 * The reason survives the restart (RTC memory, copied and cleared first
 * thing in setup()) and is logged at boot. If esp_restart() does not finish
 * within 5 s (a shutdown handler blocks), a FreeRTOS timer (created in
 * start(), static) aborts; the reason survives that panic reset too and the
 * boot log says the restart hung.
 */
namespace Liveness
{
    constexpr uint32_t TCPIP_HANG_MS = 30000;
    constexpr uint32_t TCPIP_BUSY_HANG_MS = 120000;
    constexpr uint32_t RADIO_HANG_MS = 120000;

    // Once, after the network stack is up (and before or after the radio task)
    void start();

    // First thing in setup(): takes (and clears) the reason of a restart
    // this module caused
    void captureLastRestart();
    // setup(), after the logger: logs it
    void logLastRestart(Logger &logger);

    struct Status
    {
        uint32_t tcpipAgeMs, tcpipMaxAgeMs, tcpipBusyPosts;
        uint32_t radioAgeMs, radioMaxAgeMs; // 0 without a radio task
    };
    Status status();
}
