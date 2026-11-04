/**
 * expLORA Gateway
 *
 * Liveness - see Liveness.h
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

#include "Liveness.h"
#include "LivenessPolicy.h"
#include "RadioTask.h"
#include "../Update/LayoutMigration.h"
#include <atomic>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/tcpip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

namespace Liveness
{
namespace
{
constexpr uint32_t RESTART_MAGIC = 0x4C495645; // "LIVE"
RTC_NOINIT_ATTR uint32_t restartMagic;
RTC_NOINIT_ATTR uint32_t restartWhat; // 1 lwIP thread (probe not run), 2 radio task, 3 lwIP mailbox full
RTC_NOINIT_ATTR uint32_t restartAgeMs;
RTC_NOINIT_ATTR uint32_t restartStuck; // 1: esp_restart() did not finish, the fallback aborted
// Copied and cleared at the start of setup() (captureLastRestart)
uint32_t lastWhat = 0, lastAgeMs = 0;
bool lastStuck = false;
bool lastWatchdog = false; // a watchdog reset ended the restart

// Abort fallback of restartFor(): created once in start() (static storage,
// nothing to allocate when it is needed)
StaticTimer_t fallbackBuffer;
TimerHandle_t fallback = nullptr;

esp_timer_handle_t timer = nullptr;
struct tcpip_callback_msg *probeMsg = nullptr;
std::atomic<uint32_t> tcpipCounter{0};
std::atomic<bool> probePending{false};
std::atomic<uint32_t> busyPosts{0};
uint32_t postedAt = 0; // timer callback only: when the pending probe was accepted

// Read by status() from other tasks: written only by the timer callback
LivenessPolicy::Beat tcpipBeat;
LivenessPolicy::Beat radioBeat;
std::atomic<uint32_t> tcpipAge{0}, tcpipMax{0}, radioAge{0}, radioMax{0};

const char *whatName(uint32_t what)
{
    return what == 2 ? "radio task" : what == 3 ? "lwIP thread (mailbox full)" : "lwIP thread";
}

// lwIP thread
void probe(void *)
{
    tcpipCounter.fetch_add(1, std::memory_order_relaxed);
    probePending = false;
}

// FreeRTOS timer task: esp_restart() did not finish (a shutdown handler
// blocked the esp_timer task in it). The reason is already in RTC memory;
// the panic reset that follows is accepted by captureLastRestart()
void onRestartStuck(TimerHandle_t)
{
    restartStuck = 1;
    esp_rom_printf("Liveness: restart did not complete - aborting\n");
    abort();
}

// Returns only while a restart is refused (LayoutMigration::restartRefusal(),
// printed once)
void restartFor(uint32_t what, uint32_t age)
{
    if (const char *held = LayoutMigration::restartRefusal())
    {
        static bool printed = false;
        if (!printed)
        {
            printed = true;
            esp_rom_printf("\nLiveness: %s made no progress for %u ms - not restarting: %s\n", whatName(what),
                           (unsigned)age, held);
        }
        return;
    }
    restartMagic = RESTART_MAGIC;
    restartWhat = what;
    restartAgeMs = age;
    esp_rom_printf("\nLiveness: %s made no progress for %u ms - restarting\n", whatName(what), (unsigned)age);
    restartStuck = 0;
    if (!fallback || xTimerStart(fallback, 0) != pdPASS)
    {
        esp_rom_printf("Liveness: abort fallback not armed\n");
    }
    esp_restart();
}

// esp_timer task, every second
void tick(void *)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!probePending.load())
    {
        probePending = true;
        if (tcpip_callbackmsg_trycallback(probeMsg) == ERR_OK)
        {
            postedAt = now;
        }
        else
        {
            probePending = false; // mailbox full: busy, sampled again next second
            busyPosts.fetch_add(1, std::memory_order_relaxed);
        }
    }
    uint32_t age = tcpipBeat.sample(tcpipCounter.load(std::memory_order_relaxed), now);
    tcpipAge = age;
    tcpipMax = tcpipBeat.maxAge;
    bool pending = probePending.load();
    uint32_t pendingAge = pending ? now - postedAt : 0;
    if (LivenessPolicy::tcpipHung(pending, pendingAge, age, TCPIP_HANG_MS, TCPIP_BUSY_HANG_MS))
    {
        restartFor(pending ? 1 : 3, pending ? pendingAge : age);
    }
    if (RadioTask::running())
    {
        uint32_t rage = radioBeat.sample(RadioTask::beats(), now);
        radioAge = rage;
        radioMax = radioBeat.maxAge;
        if (LivenessPolicy::hung(rage, RADIO_HANG_MS))
        {
            restartFor(2, rage);
        }
    }
}
} // namespace

void start()
{
    if (timer)
    {
        return;
    }
    // Needs the lwIP thread running (the network stack is up after
    // Uplink::begin(); the call is made after it)
    probeMsg = tcpip_callbackmsg_new(probe, nullptr);
    if (!probeMsg)
    {
        log_e("liveness probe not started (no callback message)");
        return;
    }
    fallback = xTimerCreateStatic("liveRst", pdMS_TO_TICKS(5000), pdFALSE, nullptr, onRestartStuck, &fallbackBuffer);
    if (!fallback)
    {
        tcpip_callbackmsg_delete(probeMsg);
        probeMsg = nullptr;
        log_e("liveness probe not started (fallback timer)");
        return;
    }
    esp_timer_create_args_t args = {};
    args.callback = tick;
    args.name = "liveness";
    if (esp_timer_create(&args, &timer) != ESP_OK)
    {
        timer = nullptr;
        xTimerDelete(fallback, 0);
        fallback = nullptr;
        tcpip_callbackmsg_delete(probeMsg);
        probeMsg = nullptr;
        log_e("liveness probe not started (timer)");
        return;
    }
    if (esp_timer_start_periodic(timer, 1000000) != ESP_OK)
    {
        esp_timer_delete(timer);
        timer = nullptr;
        xTimerDelete(fallback, 0);
        fallback = nullptr;
        tcpip_callbackmsg_delete(probeMsg);
        probeMsg = nullptr;
        log_e("liveness probe not started (timer start)");
    }
}

void captureLastRestart()
{
    esp_reset_reason_t reason = esp_reset_reason();
    // A watchdog may also end the restart (esp_restart() stuck with
    // interrupts off or in a task that no longer feeds it)
    bool watchdog = reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT;
    if (restartMagic == RESTART_MAGIC && (reason == ESP_RST_SW || reason == ESP_RST_PANIC || watchdog))
    {
        lastWhat = restartWhat;
        lastAgeMs = restartAgeMs;
        lastStuck = restartStuck == 1;
        lastWatchdog = watchdog;
    }
    restartMagic = 0;
}

void logLastRestart(Logger &logger)
{
    if (lastWhat)
    {
        logger.error(String("Restarted by the liveness check: the ") + whatName(lastWhat) + " made no progress for " +
                     String(lastAgeMs / 1000) + " s" +
                     (lastStuck ? " (the restart itself hung - the abort fallback reset the gateway)" : "") +
                     (lastWatchdog ? " (a watchdog ended the restart)" : ""));
    }
}

Status status()
{
    return {tcpipAge.load(), tcpipMax.load(), busyPosts.load(), radioAge.load(), radioMax.load()};
}
} // namespace Liveness
