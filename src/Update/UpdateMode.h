/**
 * expLORA Gateway
 *
 * Update mode (TLS quiesce): every outbound TLS session of the gateway that
 * checks for or downloads an update runs inside one (design 7, stage S1b).
 *
 * enter(reason):
 * - refused while another session runs, offline, without a set clock,
 *   during a firmware / sensor image upload, or while a sensor OTA job
 *   needs the radio (Waiting, Transfer, Verified, Swapping, Confirming or a
 *   queued start; Paused and terminal jobs are fine)
 * - always: MQTT disconnected and held (no reconnect), the radio task puts
 *   the radio in standby (after a response in progress), the web server
 *   answers heavy pages 503 "update in progress" (login, the firmware page
 *   and its status polls, /diag stay); uploads are refused
 * - then the TLS heap check (HeapGuard::tlsFits plus the session's own
 *   needs). Only if it does not fit, every interface that does not carry
 *   the uplink is switched off (Ethernet uplink: WiFi station + AP and
 *   LTE; WiFi station: AP and LTE; LTE: WiFi station + AP), the heap is
 *   given up to 3 s to settle and checked again; still short = the session
 *   is aborted with a logged reason and everything restored (no partial
 *   session). The Lite (no LTE, WiFi uplink) can only lose its AP
 *
 * leave(): in reverse - interfaces (LTE attaches again, the station
 * reconnects, the AP per the WiFi policy), radio receive, MQTT, web. A
 * successful gateway install restarts instead. Hard cap HARD_CAP_MS: a
 * session still open then is left by process() (sessions that block the
 * loop check expired() themselves). When interfaces were switched off and
 * the connectivity they gave is not back RESTORE_LIMIT_MS after leave(),
 * the gateway restarts (logged at the next boot; see process()).
 *
 * The radio task keeps its liveness heartbeat while paused; blocking
 * sessions run inside WatchdogPause as before.
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
#include <functional>
#include <mutex>
#include "../Data/Logging.h"
#include "UpdateModePolicy.h"

namespace UpdateMode
{
using Reason = UpdateModePolicy::Reason;

// One session at most this long (a 1.8 MB download over PPP at ~10 kB/s
// takes ~3 min, the download itself gives up after 15 min)
constexpr uint32_t HARD_CAP_MS = 15UL * 60 * 1000;
// After leave(): the LTE modem needs ~20-60 s to attach again (registration
// timeout 180 s + PPP 60 s for a healthy modem in the worst case), the
// station ~5-10 s, the AP its retry interval (5 s). 5 min covers the worst
// healthy case with margin; what is not back then (e.g. a WiFi driver that
// cannot initialise on a fragmented heap) is fixed by a restart
constexpr uint32_t RESTORE_LIMIT_MS = 5UL * 60 * 1000;

// What a session allocates next to TLS (on top of HeapGuard::TLS_MIN_FREE
// = 52 kB session peak + 8 kB reserve). Measured on the Pro (S1b bench,
// 8-bit free at entry -> lowest during the session): update check 96.6 ->
// 36.3 kB (60.3 kB), Lite 54.5 kB; a 1.67 MB gateway download 95.9 -> 26.4
// kB over Ethernet (69.5 kB), 72.9 -> 2.2 kB and 85.6 -> 8.6 kB over WiFi
// (70.7 / 77.0 kB); the Lite over WiFi 81.1 -> 6.6 kB (74.5 kB, fragmented
// heap: largest block 31.7 kB). The download buffer, Update's 4 kB sector
// buffer and the TCP receive data in flight come on top of the session
// Update check: 8 kB keeps the reserve
constexpr size_t EXTRA_CHECK = 8192;
// Gateway image download: 77 - 52 = 25 kB, rounded up to 28 kB, so the
// lowest point keeps the 8 kB reserve with ~3 kB to spare. The Pro with
// every interface up (~95 kB idle) still fits without switching anything off
constexpr size_t EXTRA_GATEWAY_DOWNLOAD = 28672;
// Sensor image download (S4): the 2 kB download buffer, the upload state
// (~0.4 kB), the LittleFS file and the TCP data in flight next to TLS; the
// signature check runs after TLS is closed. Measured with the bench server
// (P-256 certificate): Pro over Ethernet 97.6-98.4 -> 36.3-39.4 kB (at most
// 61.6 kB, i.e. 9.6 kB above the 52 kB TLS peak), the Lite over WiFi 182 ->
// 125 kB (57 kB). 16 kB leaves room for longer certificate chains and more
// data in flight over WiFi
constexpr size_t EXTRA_SENSOR_FETCH = 16384;

// Loop task: enter / leave a session. false: refused or aborted, error says
// why. Sessions are numbered (generation, never 0): leave() and expired()
// act only on their own session, so a session the hard cap ended (process())
// cannot end a newer one. result: the outcome of the session's work, shown
// as the last result on the firmware page and in /diag/heap
bool enter(Reason reason, size_t extraHeap, String &error, uint32_t &generation);
void leave(uint32_t generation, const String &result = "ok");
// Loop task: this session is over for its owner: the hard cap is reached,
// or process() already ended it (blocking sessions stop their work then)
bool expired(uint32_t generation);

// RAII: enter() in the constructor, leave() in the destructor
class Session
{
public:
    Session(Reason reason, size_t extraHeap) { entered = enter(reason, extraHeap, why, generation); }
    ~Session()
    {
        if (entered)
        {
            leave(generation, outcome);
        }
    }
    bool ok() const { return entered; }
    bool expired() const { return !entered || UpdateMode::expired(generation); }
    // What the session's work came to (default "ok")
    void setResult(const String &text) { outcome = text; }
    const String &error() const { return why; }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

private:
    bool entered = false;
    uint32_t generation = 0;
    String why;
    String outcome = "ok";
};

// Any task: why enter() would be refused right now, nullptr if it would
// start (the memory check is not part of it: it runs in enter(), after
// MQTT and the radio stopped and possibly interfaces went off). Lets a web
// handler answer at once instead of queueing work that will be refused.
// A constant text: nothing is allocated, so the answer cannot fail open
const char *wouldRefuse(Reason reason = Reason::None);

// Any task: a session is active (from the start of enter() to the end of leave())
bool active();

// Loop task, every pass: hard cap, connectivity watch after leave(). The
// watch expects back only what a session switched off and what worked
// before it, and only while it is still enabled with the same settings;
// the uplink only if it was online before. A restart is deferred while a
// firmware / sensor image upload runs or a sensor OTA job is in Transfer,
// Swapping or Confirming
void process();

// Web: uploads start under this lock, check active() and then claim the
// upload; enter() asks uploadBusy() under it (no upload can slip in between)
std::mutex &uploadGate();
void setUploadBusy(std::function<bool()> busy);
// Web: a restart is requested (after an upload, a settings change); no
// session starts then
void setRestartPending(std::function<bool()> pending);
// Web: heavy responses still running (enter() lets them finish, <= 3 s)
void setHeavyActive(std::function<unsigned()> count);

// 503 text for refused pages (any task, logged-in clients only: it names the reason)
String refusalText();
// /diag/heap "updateMode" object (any task); always valid JSON
void printDiagJson(Print &out);
// /firmware page: a short status line, empty when nothing to show (any task)
String statusText();

// setup(): restart caused by the connectivity watch, logged once
void logLastRestart(Logger &logger);
} // namespace UpdateMode
