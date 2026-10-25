/**
 * expLORA Gateway
 *
 * Update mode (TLS quiesce), see UpdateMode.h
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "UpdateMode.h"

#include <atomic>
#include <memory>
#include <new>
#include <esp_heap_caps.h>
#include <time.h>
#include "../Data/SensorManager.h"
#include "../Hardware/HeapGuard.h"
#include "../Hardware/RadioTask.h"
#include "../Hardware/WatchdogPause.h"
#include "../Network/Uplink.h"
#include "../Protocol/MQTTManager.h"

extern Logger logger;
extern SensorManager *sensorManager;
extern MQTTManager *mqttManager;

namespace UpdateMode
{
namespace
{
using namespace UpdateModePolicy;

// The radio finishes a response in progress first (RX2 ends ~3 s after the
// UP, plus the airtime of a frame launched then): 6 s leaves a margin
constexpr uint32_t RADIO_PAUSE_WAIT_MS = 6000;
// Heavy web responses already running get this long to finish
constexpr uint32_t HEAVY_WAIT_MS = 3000;
// The heap after switching interfaces off (driver / PPP teardown frees at once)
constexpr uint32_t SETTLE_MS = 3000;
// A restart the connectivity watch deferred (upload, sensor OTA job) is
// checked again this often
constexpr uint32_t DEFER_LOG_MS = 60000;

constexpr uint32_t RESTART_MAGIC = 0x55504D44; // "UPMD"
RTC_NOINIT_ATTR uint32_t restartMagic;
RTC_NOINIT_ATTR uint32_t restartMissing; // IF_* bits that were not back (IF_UPLINK: the uplink)

std::mutex gate;
std::function<bool()> uploadBusy;
std::function<bool()> restartPending;
std::function<unsigned()> heavyActive;
std::atomic<bool> activeFlag{false};
std::atomic<uint32_t> generation{0}; // the current / last session's number

// Loop task only
uint32_t startedMs = 0;
LinkState before = {};
bool monitoring = false;
bool capLogged = false;
// Connectivity watch after leave() (UpdateModePolicy::Watch)
Watch watch = {};
uint32_t staKey = 0, lteKey = 0; // settings fingerprints when the watch was armed
uint32_t lastDeferLog = 0;
uint32_t lastWatchCheck = 0;
bool watchChecked = false;

// Shown by the web task (printDiagJson / statusText): copied under the spinlock
struct Shown
{
    Reason reason = Reason::None;
    uint8_t switchedOff = 0;
    time_t startedAt = 0;   // wall clock (0 = not set)
    uint32_t startedMs = 0; // millis() at enter (running time, wrap-safe)
    uint32_t freeAtEntry = 0, largestAtEntry = 0;
    uint32_t sessions = 0, aborts = 0, refusals = 0;
    Reason lastReason = Reason::None;
    uint32_t lastDurationS = 0;
    uint32_t lastMinFree = 0;
    uint8_t lastSwitchedOff = 0;
    char lastResult[RESULT_MAX] = "";
    uint8_t restore = 0; // 0 none, 1 watching, 2 restored
    uint32_t restoreS = 0;
    uint8_t watchExpect = 0;
    // The last refusal, apart from the last session
    Reason refusedReason = Reason::None;
    char refusedText[REFUSAL_MAX] = "";
    uint32_t refusedMs = 0;
};
Shown shown;
portMUX_TYPE shownLock = portMUX_INITIALIZER_UNLOCKED;

Link currentLink()
{
    if (!uplink)
    {
        return Link::None;
    }
    switch (uplink->getActive())
    {
    case Uplink::Type::Ethernet:
        return Link::Ethernet;
    case Uplink::Type::WiFi:
        return Link::WiFi;
    case Uplink::Type::LTE:
        return Link::LTE;
    default:
        return Link::None;
    }
}

LinkState linkState()
{
    LinkState s = {};
    if (uplink)
    {
        s.online = uplink->isOnline();
        s.stationUp = uplink->stationHasIP();
        s.apUp = uplink->isApActive();
        s.lteOnline = uplink->lteOnline();
    }
    return s;
}

// Under the gate
Precheck currentPrecheck(Reason reason)
{
    Precheck p = {};
    p.reason = reason;
    p.active = activeFlag.load();
    p.restartPending = restartPending && restartPending();
    p.uplink = currentLink();
    p.clockSet = time(nullptr) >= 1700000000;
    p.uploadRunning = uploadBusy && uploadBusy();
    p.job = sensorManager ? sensorManager->otaJobState(p.jobStartPending, &p.jobWait) : OtaJob::State::None;
    return p;
}

String interfacesText(uint8_t mask)
{
    char t[INTERFACES_TEXT_MAX];
    UpdateModePolicy::interfacesText(mask, t, sizeof(t));
    return String(t);
}

void setResult(const String &text)
{
    char clean[RESULT_MAX];
    jsonSafeCopy(text.c_str(), clean, sizeof(clean));
    portENTER_CRITICAL(&shownLock);
    memcpy(shown.lastResult, clean, sizeof(clean));
    portEXIT_CRITICAL(&shownLock);
}

uint32_t minFreeNow()
{
    return (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

// The second half of leave() and of an aborted enter()
void restore(bool aborted, const String &result)
{
    uint8_t off;
    portENTER_CRITICAL(&shownLock);
    off = shown.switchedOff;
    portEXIT_CRITICAL(&shownLock);

    // Reverse order of enter(): interfaces, radio, MQTT, web
    if (uplink)
    {
        if (off)
        {
            uplink->updateModeResume();
        }
        // Settings fingerprints: an interface the watch already expects
        // keeps the one from the session that switched it off (a change
        // between the sessions must still drop it); a newly expected one
        // takes today's. A session that switched nothing off only restarts
        // the window of a running watch (it blocked the loop meanwhile)
        uint8_t had = watch.armed ? watch.expect : 0;
        armWatch(watch, off, before, millis());
        if (off)
        {
            if (!(had & IF_WIFI_STA))
            {
                staKey = uplink->updateModeConfigKey(IF_WIFI_STA);
            }
            if (!(had & IF_LTE))
            {
                lteKey = uplink->updateModeConfigKey(IF_LTE);
            }
        }
    }
    RadioTask::setPaused(false);
    if (mqttManager)
    {
        mqttManager->holdForUpdate(false);
    }

    uint32_t low = minFreeNow();
    if (monitoring)
    {
        heap_caps_monitor_local_minimum_free_size_stop();
        monitoring = false;
    }
    uint32_t durationS = (millis() - startedMs) / 1000;
    Reason reason;
    portENTER_CRITICAL(&shownLock);
    reason = shown.reason;
    shown.lastReason = shown.reason;
    shown.lastDurationS = durationS;
    shown.lastMinFree = low;
    shown.lastSwitchedOff = off;
    shown.reason = Reason::None;
    shown.switchedOff = 0;
    if (aborted)
    {
        shown.aborts++;
    }
    if (off)
    {
        shown.restore = 1;
        shown.restoreS = 0;
    }
    shown.watchExpect = watch.armed ? watch.expect : 0;
    portEXIT_CRITICAL(&shownLock);
    setResult(result);

    {
        std::lock_guard<std::mutex> lock(gate);
        activeFlag = false;
    }
    logger.info(String("Update mode left (") + reasonName(reason) + ") after " + String(durationS) +
                " s, lowest free heap " + String(low) + " B" +
                (off ? ", restoring " + interfacesText(off) : String()));
}

void abortEnter(const String &why)
{
    logger.error("Update mode aborted: " + why);
    restore(true, "aborted: " + why);
}

// Something a restart now would break
bool restartWouldInterrupt()
{
    bool busy;
    {
        std::lock_guard<std::mutex> lock(gate);
        busy = uploadBusy && uploadBusy();
    }
    bool pending = false;
    OtaJob::State job = sensorManager ? sensorManager->otaJobState(pending) : OtaJob::State::None;
    return UpdateModePolicy::restartWouldInterrupt(busy, job, pending);
}
} // namespace

bool enter(Reason reason, size_t extraHeap, String &error, uint32_t &gen)
{
    gen = 0;
    WatchdogPause pause; // radio / heavy waits, LTE power-down
    {
        std::lock_guard<std::mutex> lock(gate);
        Refusal r = precheck(currentPrecheck(reason));
        if (r != Refusal::None)
        {
            error = String("Update not started: ") + UpdateModePolicy::refusalText(r);
            logger.warning(String("Update mode (") + reasonName(reason) + ") refused: " +
                           UpdateModePolicy::refusalText(r));
            // Shown apart from the last session (whose reason, duration and
            // interfaces stay as they were)
            char text[REFUSAL_MAX];
            jsonSafeCopy(UpdateModePolicy::refusalText(r), text, sizeof(text));
            portENTER_CRITICAL(&shownLock);
            shown.refusals++;
            shown.refusedReason = reason;
            memcpy(shown.refusedText, text, sizeof(text));
            shown.refusedMs = millis();
            portEXIT_CRITICAL(&shownLock);
            return false;
        }
        activeFlag = true; // from here: uploads refused, heavy pages answered 503
        gen = nextGeneration(generation.load());
        generation = gen;
    }

    startedMs = millis();
    capLogged = false;
    // A connectivity watch of an earlier session keeps running (merged
    // with this one's in restore()); process() does not act during a session
    before = linkState();
    monitoring = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
    portENTER_CRITICAL(&shownLock);
    shown.reason = reason;
    shown.switchedOff = 0;
    shown.startedAt = time(nullptr);
    shown.startedMs = startedMs;
    shown.sessions++;
    portEXIT_CRITICAL(&shownLock);
    logger.info(String("Update mode: entering for the ") + reasonName(reason) + " (uplink " +
                (uplink ? uplink->getActiveName() : "none") + ")");

    // Always: MQTT off, radio in standby
    if (mqttManager)
    {
        mqttManager->holdForUpdate(true);
    }
    RadioTask::setPaused(true);
    uint32_t t0 = millis();
    while (!RadioTask::paused() && millis() - t0 < RADIO_PAUSE_WAIT_MS)
    {
        delay(20);
    }
    if (!RadioTask::paused())
    {
        error = "Update aborted: the radio did not pause";
        abortEnter("the radio did not pause within " + String(RADIO_PAUSE_WAIT_MS / 1000) + " s");
        gen = 0;
        return false;
    }
    // The job may have moved between the check above and the pause (an UP
    // the radio task handled meanwhile: an accepting UP, a start sent)
    {
        bool pending = false;
        OtaJob::WaitView wait;
        const OtaJob::State job =
            sensorManager ? sensorManager->otaJobState(pending, &wait) : OtaJob::State::None;
        if (!UpdateModePolicy::jobAllows(job, pending, reason, wait))
        {
            error = "Update aborted: the sensor OTA job changed";
            abortEnter("the sensor OTA job changed while the radio paused");
            gen = 0;
            return false;
        }
    }
    // Heavy pages admitted before the flag still hold their memory
    t0 = millis();
    while (heavyActive && heavyActive() > 0 && millis() - t0 < HEAVY_WAIT_MS)
    {
        delay(20);
    }

    HeapGuard::Heap heap = HeapGuard::tlsHeap();
    bool fitsNow = fits(heap, extraHeap);
    Link link = currentLink();
    uint8_t plan = uplink ? switchOffPlan(link, uplink->updateModeRunning()) : 0;
    MemStep step = memoryStep(fitsNow, plan, false);
    if (step == MemStep::SwitchOff)
    {
        logger.warning("Update mode: TLS needs " + String(HeapGuard::TLS_MIN_FREE + extraHeap) + " B free with a block of " +
                       String(HeapGuard::TLS_MIN_BLOCK) + " B, has " + String(heap.free) + " B / " +
                       String(heap.largest) + " B - switching off " + interfacesText(plan) + " (uplink " +
                       uplink->getActiveName() + " stays)");
        uint8_t off = uplink->updateModeSuspend(plan);
        portENTER_CRITICAL(&shownLock);
        shown.switchedOff = off;
        portEXIT_CRITICAL(&shownLock);
        t0 = millis();
        do
        {
            delay(100);
            heap = HeapGuard::tlsHeap();
            fitsNow = fits(heap, extraHeap);
        } while (!fitsNow && millis() - t0 < SETTLE_MS);
        // A DHCP client torn down with the station (or PPP) may have cleared
        // lwIP's DNS list or taken the default route; Uplink::process()
        // repairs that, but does not run while the session blocks the loop.
        // beginHttps() checks again right before the connection. The uplink
        // is compared after it (refreshRoutes() selects the active one)
        uplink->refreshRoutes();
        if (currentLink() != link)
        {
            error = "Update aborted: the uplink changed while switching interfaces off";
            abortEnter("the uplink changed while switching interfaces off");
            gen = 0;
            return false;
        }
        step = memoryStep(fitsNow, plan, true);
    }
    if (step == MemStep::Abort)
    {
        String why = "not enough memory for TLS: needs " + String(HeapGuard::TLS_MIN_FREE + extraHeap) +
                     " B free with a block of " + String(HeapGuard::TLS_MIN_BLOCK) + " B, has " + String(heap.free) +
                     " B / " + String(heap.largest) + " B" +
                     (plan ? " with " + interfacesText(plan) + " switched off" : String(" (nothing to switch off)"));
        error = "Update aborted: " + why;
        abortEnter(why);
        gen = 0;
        return false;
    }
    portENTER_CRITICAL(&shownLock);
    shown.freeAtEntry = heap.free;
    shown.largestAtEntry = heap.largest;
    uint8_t off = shown.switchedOff;
    portEXIT_CRITICAL(&shownLock);
    logger.info("Update mode active: TLS heap " + String(heap.free) + " B free, largest block " +
                String(heap.largest) + " B; MQTT held, radio paused, switched off: " + interfacesText(off));
    return true;
}

void leave(uint32_t gen, const String &result)
{
    if (!ownsSession(gen, generation.load(), activeFlag.load()))
    {
        return; // already ended (hard cap) or not this session
    }
    restore(false, result);
}

const char *wouldRefuse(Reason reason)
{
    std::lock_guard<std::mutex> lock(gate);
    Refusal r = precheck(currentPrecheck(reason));
    return r == Refusal::None ? nullptr : UpdateModePolicy::refusalText(r);
}

bool active()
{
    return activeFlag.load();
}

bool expired(uint32_t gen)
{
    return !ownsSession(gen, generation.load(), activeFlag.load()) || capReached(millis(), startedMs, HARD_CAP_MS);
}

void process()
{
    if (activeFlag.load())
    {
        if (capReached(millis(), startedMs, HARD_CAP_MS) && !capLogged)
        {
            capLogged = true;
            logger.error("Update mode: open for " + String(HARD_CAP_MS / 60000) + " min - leaving it");
            restore(true, "hard cap reached");
        }
        return;
    }
    if (!watch.armed || !uplink)
    {
        return;
    }
    uint32_t nowMs = millis();
    if (watchChecked && nowMs - lastWatchCheck < WATCH_CHECK_MS)
    {
        return;
    }
    watchChecked = true;
    lastWatchCheck = nowMs;
    Wanted wanted;
    wanted.station = uplink->updateModeWanted(IF_WIFI_STA) && uplink->updateModeConfigKey(IF_WIFI_STA) == staKey;
    wanted.ap = uplink->apWantedNow();
    wanted.lte = uplink->updateModeWanted(IF_LTE) && uplink->updateModeConfigKey(IF_LTE) == lteKey;
    LinkState now = linkState();
    uint8_t dropped = 0;
    uint8_t missing = watchMissing(watch, now, wanted, &dropped);
    if (dropped)
    {
        logger.info("Update mode: " + interfacesText(dropped) +
                    " disabled or changed in the settings since the session - not expected back any more");
        portENTER_CRITICAL(&shownLock);
        shown.watchExpect = watch.expect;
        portEXIT_CRITICAL(&shownLock);
    }
    After a = afterLeave(missing, nowMs, watch.leftAt, RESTORE_LIMIT_MS,
                         missing != 0 && nowMs - watch.leftAt >= RESTORE_LIMIT_MS && restartWouldInterrupt());
    if (a == After::Restored)
    {
        watch.armed = false;
        lastDeferLog = 0;
        uint32_t s = (nowMs - watch.leftAt) / 1000;
        bool cancelled = watchCancelled(watch);
        portENTER_CRITICAL(&shownLock);
        shown.restore = cancelled ? 3 : 2;
        shown.restoreS = s;
        shown.watchExpect = 0;
        portEXIT_CRITICAL(&shownLock);
        if (cancelled)
        {
            logger.info("Update mode: watch cancelled (interfaces disabled: " + interfacesText(watch.dropped) + ")");
        }
        else
        {
            logger.info("Update mode: connectivity restored " + String(s) + " s after the session" +
                        (watch.expect ? " (" + interfacesText(watch.expect) + " back)" : String()));
        }
    }
    else if (a == After::Deferred)
    {
        if (lastDeferLog == 0 || nowMs - lastDeferLog >= DEFER_LOG_MS)
        {
            lastDeferLog = nowMs | 1;
            logger.warning("Update mode: " + interfacesText(missing) + " not back " +
                           String((nowMs - watch.leftAt) / 60000) +
                           " min after the session - restart deferred while an upload or a sensor OTA job runs");
        }
    }
    else if (a == After::Reboot)
    {
        watch.armed = false;
        String detail;
        if (missing & IF_UPLINK)
        {
            detail += " the uplink (online before the session) is offline;";
        }
        if (missing & IF_WIFI_STA)
        {
            detail += " the WiFi station (connected before, switched off by the session) has no address;";
        }
        if (missing & IF_WIFI_AP)
        {
            detail += " the access point (running before, wanted now) is not running;";
        }
        if (missing & IF_LTE)
        {
            detail += " LTE (online before, switched off by the session) is not online;";
        }
        logger.error("Update mode: " + interfacesText(missing) + " not back " + String(RESTORE_LIMIT_MS / 60000) +
                     " min after the session -" + detail + " restarting");
        restartMagic = RESTART_MAGIC;
        restartMissing = missing;
        if (sensorManager)
        {
            sensorManager->saveSensors(); // keep the rain totals
        }
        delay(500);
        ESP.restart();
    }
}

std::mutex &uploadGate()
{
    return gate;
}

void setUploadBusy(std::function<bool()> busy)
{
    uploadBusy = busy;
}

void setRestartPending(std::function<bool()> pending)
{
    restartPending = pending;
}

void setHeavyActive(std::function<unsigned()> count)
{
    heavyActive = count;
}

String refusalText()
{
    Reason r;
    portENTER_CRITICAL(&shownLock);
    r = shown.reason;
    portEXIT_CRITICAL(&shownLock);
    return String("Update in progress (") + reasonName(r) +
           ") - this page is not available until it ends. Try again in a few minutes.";
}

void printDiagJson(Print &out)
{
    Shown s;
    portENTER_CRITICAL(&shownLock);
    s = shown;
    portEXIT_CRITICAL(&shownLock);
    uint32_t nowMs = millis();
    DiagView v = {};
    v.active = activeFlag.load();
    v.reason = s.reason;
    v.startedAt = (uint32_t)s.startedAt;
    v.runningS = (nowMs - s.startedMs) / 1000;
    v.switchedOff = s.switchedOff;
    v.freeAtEntry = s.freeAtEntry;
    v.largestAtEntry = s.largestAtEntry;
    v.minFree = minFreeNow(); // monitored locally from enter() on
    v.sessions = s.sessions;
    v.aborts = s.aborts;
    v.refusals = s.refusals;
    v.lastReason = s.lastReason;
    v.lastResult = s.lastResult;
    v.lastDurationS = s.lastDurationS;
    v.lastMinFree = s.lastMinFree;
    v.lastSwitchedOff = s.lastSwitchedOff;
    v.restore = s.restore;
    v.restoreS = s.restoreS;
    v.watchExpect = s.watchExpect;
    v.refusedReason = s.refusedReason;
    v.refusedText = s.refusedText;
    v.refusedAgoS = (nowMs - s.refusedMs) / 1000;
    std::unique_ptr<char[]> buf(new (std::nothrow) char[DIAG_JSON_MAX]);
    int n = buf ? diagJson(v, buf.get(), DIAG_JSON_MAX) : -1;
    if (n < 0 || (size_t)n >= DIAG_JSON_MAX)
    {
        // No memory for the buffer (cannot be too long: see the native test)
        out.print(v.active ? "{\"active\":true}" : "{\"active\":false}");
        return;
    }
    out.write((const uint8_t *)buf.get(), (size_t)n);
}

String statusText()
{
    Shown s;
    portENTER_CRITICAL(&shownLock);
    s = shown;
    portEXIT_CRITICAL(&shownLock);
    uint32_t nowMs = millis();
    if (activeFlag.load())
    {
        return String("Update in progress: ") + reasonName(s.reason) + ", running for " +
               String((nowMs - s.startedMs) / 1000) + " s; switched off: " + interfacesText(s.switchedOff) +
               ". MQTT and sensor reception are paused; most pages are unavailable until it ends.";
    }
    String t;
    if (s.sessions > 0)
    {
        t = String("Last update session: ") + reasonName(s.lastReason) + " - " + s.lastResult + " (" +
            String(s.lastDurationS) + " s";
        if (s.lastSwitchedOff)
        {
            t += ", switched off: " + interfacesText(s.lastSwitchedOff);
            t += s.restore == 1   ? ", reconnecting"
                 : s.restore == 2 ? ", restored"
                 : s.restore == 3 ? ", watch cancelled (interfaces disabled)"
                                  : "";
        }
        t += ").";
    }
    if (s.refusedReason != Reason::None)
    {
        t += String(t.length() ? " " : "") + "Last refused request: " + reasonName(s.refusedReason) + " - " +
             s.refusedText + " (" + String((nowMs - s.refusedMs) / 1000) + " s ago).";
    }
    return t;
}

void logLastRestart(Logger &log)
{
    if (restartMagic == RESTART_MAGIC && esp_reset_reason() == ESP_RST_SW)
    {
        log.error("Restarted by the update mode: " + interfacesText((uint8_t)restartMissing) +
                  " did not come back within " + String(RESTORE_LIMIT_MS / 60000) + " min after an update session");
    }
    restartMagic = 0;
}
} // namespace UpdateMode
