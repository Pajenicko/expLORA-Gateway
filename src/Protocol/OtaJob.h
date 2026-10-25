/**
 * expLORA Gateway
 *
 * OTA job of the gateway (protocol v2, docs/protocol-v2.md 14.5): the state
 * machine of the 14.5 matrix with its order of evaluation, deadlines in unix
 * time or uptime, baseline, binding, no-progress counters, laps, the starved
 * back-off, the 6 h battery retry, abortPending / manualPause, orphan
 * sessions, block choice over the confirmed and sent-since views and the
 * anchoring by the accepting UP or a status slot index.
 *
 * Pure C++ (no Arduino, no flash, no radio): unit-tested natively in
 * test/test_ota_job, including one test per cell of the 14.5 matrix.
 * SensorManager feeds it the UPs of the target, the admin actions, the
 * launch outcomes and the clock; it persists the persistent fields in
 * /ota/job.json on every change it reports.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <cstring>

#include "ExploraProtocol.h"

namespace OtaJob
{

enum class State : uint8_t
{
    None, // no job (or a cleared one)
    Waiting,
    Transfer,
    Paused,
    Verified,
    Swapping,
    Confirming,
    Done,
    Failed,
    Aborted,
    Reverted,
};

inline bool terminal(State s)
{
    return s == State::None || s == State::Done || s == State::Failed || s == State::Aborted ||
           s == State::Reverted;
}


// Why a job ended or is paused (shown in words on the page)
enum class Why : uint8_t
{
    None,
    Refused,        // arg: refusal reason
    SensorFailed,   // arg: failure reason of the sensor
    NoAccept,       // Waiting deadline
    NoProgress,     // 4 counted statuses or 3 laps without progress
    NoJoin,         // Swapping deadline: "no join after the swap"
    NotConfirmed,   // Confirming deadline
    PausedTooLong,  // 7 days Paused
    VerifiedTooLong, // 7 days Verified
    Admin,          // Abort by the admin
    Left,           // arg: leave reason (Paused)
    Missed,         // 2 status slots without a status (Paused)
    NewSession,     // the sensor joined again (Paused)
    Starved,        // the gateway could not transmit (Paused, back-off)
    Battery,        // refusal 6 (Paused / Verified, retry after 6 h)
    Reboot,         // gateway reboot (Paused)
    ManualPause,    // the admin paused
    StartCrc,       // the stored image failed its CRC at the start
    Unsigned,       // "Signed images only" is on and the image is unsigned (Paused, held like a manual pause)
};

// --- clock: unix time when valid (0 otherwise), uptime, esp_timer us -------------

struct Now
{
    uint32_t unixS = 0;  // 0: the gateway has no valid clock (not "unix": a predefined macro in GNU C++)
    uint32_t uptimeS = 0;
    int64_t us = 0;      // esp_timer (anchors, slots)
};

// A deadline in unix time when the clock was valid when it was armed,
// otherwise in uptime (restarted from the full duration after a reboot)
struct Deadline
{
    uint8_t kind = 0; // 0 none, 1 unix, 2 uptime
    uint32_t at = 0;
    uint32_t durS = 0;

    void arm(const Now &n, uint32_t seconds)
    {
        durS = seconds;
        if (n.unixS != 0)
        {
            kind = 1;
            at = n.unixS + seconds;
        }
        else
        {
            kind = 2;
            at = n.uptimeS + seconds;
        }
    }
    void clear() { kind = 0; }
    bool armed() const { return kind != 0; }
    bool reached(const Now &n) const
    {
        if (kind == 1)
        {
            return n.unixS != 0 && (int32_t)(n.unixS - at) >= 0;
        }
        if (kind == 2)
        {
            return (int32_t)(n.uptimeS - at) >= 0;
        }
        return false;
    }
    // Seconds left (0 when reached or unknown)
    uint32_t leftS(const Now &n) const
    {
        if (kind == 1 && n.unixS != 0)
        {
            return (int32_t)(at - n.unixS) > 0 ? at - n.unixS : 0;
        }
        if (kind == 2)
        {
            return (int32_t)(at - n.uptimeS) > 0 ? at - n.uptimeS : 0;
        }
        return 0;
    }
    // After a gateway reboot the uptime restarts: an uptime deadline runs again
    void afterReboot(const Now &n)
    {
        if (kind == 2)
        {
            at = n.uptimeS + durS;
        }
    }
};

// --- one UP of the target, after its TLVs were applied ------------------------------

struct Up
{
    bool joinReq = false;      // a verified JOIN_REQ: a new session
    bool hasFw = false;        // firmware version known (JOIN_REQ or status report)
    uint32_t fw = 0;           // major << 16 | minor << 8 | patch
    bool hasStatus = false;    // a status report (format 1, >= 24 bytes)
    uint8_t boot = 0xFF;       // its boot state
    uint8_t flags = 0;         // its flags byte
    bool bit4 = false;         // UP FCtrl bit 4 (OTA session active)
    // OTA status TLVs
    bool accepted = false;
    uint8_t acceptedArg = 0;
    bool progress = false;
    uint16_t base = 0;
    uint64_t window = 0;
    uint16_t slot = 0xFFFF;
    bool refused = false;
    uint8_t refusedArg = 0;
    bool failed = false;
    uint8_t failedArg = 0;
    bool left = false;
    uint8_t leftArg = 0;
    bool complete = false;
    bool batteryLow = false;
    uint8_t batteryArg = 0;
    // The frame
    uint32_t fcnt32 = 0;
    int64_t rxUs = 0;           // RxDone (esp_timer)
    bool timeReliable = false;
    uint32_t airUs = 0;         // its time on air
    uint32_t sessionNonce = 0;  // devNonce of the session it verified under
    uint32_t curFw = 0;         // the firmware the sensor runs (last JOIN_REQ / status report)
    bool canStart = false;      // session established (not SESSION_NEW) and the configuration in sync
    bool signedOnly = false;    // "Signed images only" is on (with Job::imageVerified: no new transfer)

    bool event() const { return accepted || refused || failed || complete || batteryLow; } // "left" does not suppress
};

enum class Down : uint8_t
{
    None,
    Start,    // an OTA start in this DOWN (binding = this UP's FCnt32 + 1)
    EndPause, // an OTA end pause in this DOWN
    EndAbort, // an OTA end abort in this DOWN
};

// What the UP led to
struct Result
{
    Down down = Down::None;
    bool changed = false;       // persist job.json
    bool revertRecord = false;  // write the per-sensor reverted record (version + CRC of the job image)
    bool log = false;           // a state change worth a log line
    bool bindMismatch = false;  // the accepting UP's FCnt differs from the expected one (log)
    bool adopted = false;       // a status took over the binding of a start whose accepting UP was lost
    bool abortKept = false;     // an abort ended without reaching the sensor (it keeps its START, log)
};

constexpr uint32_t STATUS_GRACE_US = 4000000;            // T_status = status slot + 4 s
constexpr uint32_t SEVEN_DAYS_S = 7 * 86400;
constexpr uint32_t BATTERY_RETRY_S = 6 * 3600;
constexpr uint32_t BACKOFF_MIN_S = 3600;
constexpr uint32_t BACKOFF_MAX_S = 6 * 3600;
constexpr uint32_t SWAP_WAIT_S = 4 * 3600;               // Swapping: 4 h + 2 intervals
constexpr uint8_t NO_PROGRESS_STATUSES = 4;
constexpr uint8_t NO_PROGRESS_LAPS = 3;
constexpr uint8_t LAUNCH_RING = 16;                      // >= K/4 for K <= 64
constexpr uint8_t START_ARMS = 4;                        // starts that re-arm the Waiting deadline
constexpr uint8_t SHRINKS_FREE = 4;                      // base decreases not counted as "no progress"

// T_confirm of the sensor (spec 10.3): max(6 h, 4 x interval)
inline uint32_t tConfirmS(uint32_t intervalS)
{
    return intervalS * 4 > 6 * 3600 ? intervalS * 4 : 6 * 3600;
}

enum class PendingEnd : uint8_t
{
    None,
    Pause,
    Abort,
};

struct Job
{
    // --- persisted (job.json) ---
    State state = State::None;
    uint32_t devId = 0;
    uint16_t hwType = 0;
    uint32_t fw = 0;       // image firmware version
    uint32_t crc = 0;      // image CRC (trailer)
    uint32_t len = 0;      // image + trailer
    uint32_t baseFw = 0;   // the target's firmware when the job started
    uint8_t baseBoot = 0xFF;
    uint8_t baseFlags = 0;
    bool baseFlaggedRevert = false; // baseline boot state 3/4/5/7 with flags bit 3
    uint32_t intervalS = 600;       // the target's report interval (deadlines)
    uint8_t k = ep::ota::kMinK;
    uint16_t cadence100ms = 260;
    uint16_t startDelayMs = 4000;
    bool bindValid = false;
    uint32_t bindFcnt = 0;          // the accepting UP's FCnt32 (expected, then verified)
    uint32_t sessionNonce = 0;      // devNonce of the session the accepting UP verified under
    bool startValid = false;        // a start went out and its accepting UP was not seen yet
    uint32_t startNonce = 0;        // ... under this session
    uint32_t expectBind = 0;        // ... its accepting UP's FCnt32 (the UP it answered + 1)
    uint8_t startsArmed = 0;        // starts that re-armed the "no accept" deadline (at most START_ARMS)
    uint8_t shrinks = 0;            // statuses with a lower base not counted (at most SHRINKS_FREE)
    bool manualPause = false;
    bool abortPending = false;
    bool kept = false;              // Done: installed, unconfirmed, cannot be reverted (KEPT)
    bool despiteAbort = false;      // Done after an Abort
    bool abortQuiet = false;        // aborted (Transfer / Paused / Verified), no OTA end abort answered since
    bool abortKept = false;         // ... ended by an UP with bit 4 = 0 or expired: the sensor keeps its state
    bool swapSeen = false;
    bool ghost = false;             // a cleared Swapping / Confirming job: only the revert watch is left
    uint16_t maxConfirmed = 0;
    uint8_t noProgress = 0;         // counted statuses without progress
    uint8_t laps = 0;               // "accepted -> left 1" laps without progress
    bool progressSinceAccept = false;
    uint32_t backoffS = 0;          // the next starved back-off
    Why why = Why::None;
    uint8_t whyArg = 0;
    Deadline dlState;               // the deadline of the current state
    Deadline dlPaused;              // 7 days from the first entry into Paused (not re-armed)
    bool pausedOnce = false;
    Deadline dlBackoff;             // no start before (starved back-off)
    Deadline dlBattery;             // no start before (6 h after a refusal 6)
    Deadline dlAbortPending;        // abortPending expires (2 rounds)
    Deadline dlRevertWatch;         // 3 x T_confirm after Failed not-confirmed / no-join / despite abort
    // statistics
    uint32_t blocksSent = 0, repeats = 0, statuses = 0, missedTotal = 0, rounds = 0, airMs = 0;
    uint32_t skipped = 0;           // block slots that went unused (radio, loop, uplink slot, ...)
    uint32_t startedUnix = 0, lastStatusUnix = 0;

    // --- RAM only ---
    ep::ota::Bitmap confirmed;
    ep::ota::Bitmap sentSince;      // sent since the last status
    ep::ota::Bitmap everSent;       // sent at least once in this job (repeat statistics)
    uint16_t viewBase = 0;          // base of the last status (block choice window)
    bool anchored = false;          // A known
    bool anchorReliable = false;    // blocks may be sent
    int64_t anchorUs = 0;           // A (esp_timer)
    int64_t anchorEventUs = 0;      // RxDone of the UP the anchor came from (gateway drift bound)
    uint32_t anchorBind = 0;        // binding and session the anchor belongs to
    uint32_t anchorNonce = 0;
    uint16_t anchorRejects = 0;     // statuses whose implied shift was out of bound (not re-anchored)
    int64_t lastStatusUs = 0;
    uint8_t missedSinceStatus = 0;
    int32_t lastCheckedStatusSlot = -1;
    uint16_t sentSinceStatus = 0;   // blocks launched since the last status (K/4 rule)
    uint32_t launchS[LAUNCH_RING] = {}; // uptime s of the last launches (starved rule)
    uint8_t launchCount = 0, launchNext = 0;
    PendingEnd pendingEnd = PendingEnd::None; // an OTA end in the next block slot
    uint16_t lastIndex = 0xFFFF;    // last block launched (the repeat pass cycles from here)
    uint32_t pausedUptimeS = 0;     // when Transfer -> Paused by missed statuses (starved rule)
    uint32_t transferUptimeS = 0;   // the (re)entry into Transfer, or a lost accept's start (starved rule)
    uint32_t startUptimeS = 0;      // when the pending start went out
    bool pausedByMisses = false;
    uint32_t lastStatusUptimeS = 0;
    // The stored image of this job is verified (signed by a key of this
    // firmware). Set when the job is created and when it is restored at
    // boot; false (unsigned) until then - fail closed
    bool imageVerified = false;

    uint16_t blocks() const { return ep::ota::blockCount(len, ep::ota::kBlockSize); }
    uint32_t cadenceMs() const { return (uint32_t)cadence100ms * 100; }
    uint32_t roundS() const { return ((uint32_t)k + 1) * cadence100ms / 10; }
    ep::ota::Start start() const
    {
        ep::ota::Start s;
        s.imageLen = len;
        s.imageCrc = crc;
        s.fwVersion = fw;
        s.hwType = hwType;
        s.k = k;
        s.cadence100ms = cadence100ms;
        s.startDelayMs = startDelayMs;
        return s;
    }
    // esp_timer time of slot s (the sensor's slot time; the radio task
    // subtracts its chip offset)
    int64_t slotUs(uint32_t s) const
    {
        return anchorUs + (int64_t)ep::ota::slotOffsetMs(s, start()) * 1000;
    }
};


// The target and the image of a new job
struct Target
{
    uint32_t devId = 0;
    uint16_t hwType = 0;
    uint32_t fw = 0;
    uint8_t boot = 0xFF;
    uint8_t flags = 0;
    uint32_t intervalS = 600;
};

struct Image
{
    uint32_t fw = 0, crc = 0, len = 0;
};

// --- names --------------------------------------------------------------------------

inline const char *stateName(State s)
{
    switch (s)
    {
    case State::None:
        return "none";
    case State::Waiting:
        return "Waiting";
    case State::Transfer:
        return "Transfer";
    case State::Paused:
        return "Paused";
    case State::Verified:
        return "Verified";
    case State::Swapping:
        return "Swapping";
    case State::Confirming:
        return "Confirming";
    case State::Done:
        return "Done";
    case State::Failed:
        return "Failed";
    case State::Aborted:
        return "Aborted";
    case State::Reverted:
        return "Reverted";
    }
    return "?";
}

inline const char *refusalText(uint8_t r)
{
    switch (r)
    {
    case 1:
        return "transfer version not supported";
    case 2:
        return "image or schedule parameters out of range";
    case 3:
        return "HW type differs from the sensor";
    case 4:
        return "swap state not idle";
    case 5:
        return "running image not confirmed yet";
    case 6:
        return "battery below 2.4 V";
    case 7:
        return "RTC not usable";
    case 8:
        return "flash erase failed";
    }
    return "unknown reason";
}

inline const char *failureText(uint8_t r)
{
    switch (r)
    {
    case 1:
        return "CRC of the received image wrong";
    case 2:
        return "trailer invalid (incl. the OTA flags)";
    case 3:
        return "image differs from the start";
    case 4:
        return "flash error";
    }
    return "unknown reason";
}

inline const char *leaveText(uint8_t r)
{
    switch (r)
    {
    case 1:
        return "no block for 2 rounds";
    case 2:
        return "battery below 2.3 V";
    case 3:
        return "OTA end";
    }
    return "unknown reason";
}

inline const char *whyText(Why w)
{
    switch (w)
    {
    case Why::None:
        return "";
    case Why::Refused:
        return "refused";
    case Why::SensorFailed:
        return "failed on the sensor";
    case Why::NoAccept:
        return "no accept";
    case Why::NoProgress:
        return "no progress";
    case Why::NoJoin:
        return "no join after the swap";
    case Why::NotConfirmed:
        return "not confirmed";
    case Why::PausedTooLong:
        return "paused for 7 days";
    case Why::VerifiedTooLong:
        return "verified, waited 7 days for the battery";
    case Why::Admin:
        return "aborted by the admin";
    case Why::Left:
        return "the sensor left";
    case Why::Missed:
        return "2 status slots without a status";
    case Why::NewSession:
        return "the sensor joined again";
    case Why::Starved:
        return "the gateway could not transmit (back-off)";
    case Why::Battery:
        return "battery low, retry after 6 h";
    case Why::Reboot:
        return "gateway restarted";
    case Why::ManualPause:
        return "paused by the admin";
    case Why::StartCrc:
        return "stored image CRC wrong";
    case Why::Unsigned:
        return "unsigned image - \"Signed images only\" is on";
    }
    return "";
}

// --- internals ------------------------------------------------------------------------

inline bool flaggedRevert(uint8_t boot, uint8_t flags)
{
    return (boot == 3 || boot == 4 || boot == 5 || boot == 7) && (flags & 0x08);
}

inline uint32_t waitingDeadlineS(const Job &j)
{
    const uint32_t three = 3 * j.intervalS;
    const uint32_t rounds = 3 * j.roundS(); // a lost accept and a lost first status still fit
    return three > rounds ? three : rounds;
}

inline void setState(Job &j, State s, const Now &n, Why why = Why::None, uint8_t arg = 0)
{
    j.state = s;
    j.why = why;
    j.whyArg = arg;
    j.dlState.clear();
    switch (s)
    {
    case State::Waiting:
        j.dlState.arm(n, waitingDeadlineS(j));
        break;
    case State::Transfer:
        j.missedSinceStatus = 0;
        j.pausedByMisses = false;
        j.transferUptimeS = n.uptimeS; // starved rule: no window into a Paused period
        break;
    case State::Paused:
        if (!j.pausedOnce)
        {
            j.pausedOnce = true;
            j.dlPaused.arm(n, SEVEN_DAYS_S);
        }
        break;
    case State::Verified:
        j.dlState.arm(n, SEVEN_DAYS_S);
        j.manualPause = false; // nothing to pause any more (Resume exists only in Paused)
        break;
    case State::Swapping:
        j.dlState.arm(n, SWAP_WAIT_S + 2 * j.intervalS);
        j.manualPause = false;
        break;
    case State::Confirming:
        j.dlState.arm(n, 3 * tConfirmS(j.intervalS) + 2 * j.intervalS);
        j.manualPause = false;
        break;
    default:
        break;
    }
    if (s == State::Done)
    {
        j.maxConfirmed = j.blocks(); // the sensor verified the whole image
    }
    if (terminal(s))
    {
        j.pendingEnd = j.pendingEnd == PendingEnd::Abort ? PendingEnd::Abort : PendingEnd::None;
        if ((s == State::Failed && (why == Why::NotConfirmed || why == Why::NoJoin)) ||
            (s == State::Done && j.despiteAbort))
        {
            j.dlRevertWatch.arm(n, 3 * tConfirmS(j.intervalS));
        }
    }
}

inline void recordLaunch(Job &j, uint32_t uptimeS)
{
    j.launchS[j.launchNext] = uptimeS;
    j.launchNext = (uint8_t)((j.launchNext + 1) % LAUNCH_RING);
    if (j.launchCount < LAUNCH_RING)
    {
        j.launchCount++;
    }
}

inline uint16_t launchesSince(const Job &j, uint32_t fromS)
{
    uint16_t c = 0;
    for (uint8_t i = 0; i < j.launchCount; i++)
    {
        if ((int32_t)(j.launchS[i] - fromS) >= 0)
        {
            c++;
        }
    }
    return c;
}

inline uint16_t progressValue(const Job &j, uint16_t base, uint64_t window)
{
    if (base >= j.blocks())
    {
        return j.blocks();
    }
    uint16_t v = base;
    for (int i = 0; i < 64; i++)
    {
        if ((window >> i) & 1)
        {
            v++;
        }
    }
    return v;
}

// Status slot index q (0-based round) of status slot s
inline int32_t statusRound(const Job &j, uint32_t s)
{
    return (int32_t)(s / ((uint32_t)j.k + 1));
}

inline void anchorAt(Job &j, int64_t aUs, bool reliable, int64_t eventUs)
{
    j.anchorUs = aUs;
    j.anchored = true;
    j.anchorReliable = reliable;
    j.anchorEventUs = eventUs;
    j.anchorBind = j.bindFcnt;
    j.anchorNonce = j.sessionNonce;
}

// Spec 14.3: a status re-anchors only when the implied shift lies within
// +-(20 ms + d_gw), d_gw = 20 ppm x time since the last anchor + 5 ms - a
// status that left late (preparation overrun) must not drag the schedule. Only
// a reliable anchor of the same binding and session is kept; any other (none,
// unreliable, a binding taken over from a lost accepting UP) is replaced.
constexpr int64_t ANCHOR_BOUND_US = 20000;
constexpr int64_t ANCHOR_DRIFT_PPM = 20;
constexpr int64_t ANCHOR_DRIFT_FLOOR_US = 5000;

inline bool anchorShiftOk(const Job &j, int64_t aUs, int64_t eventUs)
{
    if (!j.anchored || !j.anchorReliable || j.anchorBind != j.bindFcnt || j.anchorNonce != j.sessionNonce)
    {
        return true;
    }
    const int64_t since = eventUs > j.anchorEventUs ? eventUs - j.anchorEventUs : 0;
    const int64_t bound = ANCHOR_BOUND_US + since * ANCHOR_DRIFT_PPM / 1000000 + ANCHOR_DRIFT_FLOOR_US;
    const int64_t shift = aUs - j.anchorUs;
    return shift <= bound && shift >= -bound;
}

// A progress status in Transfer (or bringing Paused back): view replaced,
// re-anchored, the no-progress rule. false: the job failed
inline bool applyProgress(Job &j, const Up &u, const Now &n)
{
    j.statuses++;
    j.lastStatusUnix = n.unixS;
    j.lastStatusUptimeS = n.uptimeS;
    j.lastStatusUs = u.rxUs;
    if (!j.confirmed.applyStatus(u.base, u.window))
    {
        j.missedSinceStatus++; // base past the block count: counted as missed
        j.missedTotal++;
        return true;
    }
    j.missedSinceStatus = 0;
    const bool shrank = u.base < j.viewBase; // the sensor erased a page below the old base
    j.viewBase = u.base;
    j.sentSince.reset(j.blocks());
    if (u.slot != 0xFFFF && ep::ota::isStatusSlot(u.slot, j.k) && u.timeReliable)
    {
        const int64_t a = u.rxUs - (int64_t)u.airUs - (int64_t)ep::ota::slotOffsetMs(u.slot, j.start()) * 1000;
        if (anchorShiftOk(j, a, u.rxUs))
        {
            anchorAt(j, a, true, u.rxUs);
        }
        else if (j.anchorRejects < 0xFFFF)
        {
            j.anchorRejects++; // progress and binding still apply
        }
        j.lastCheckedStatusSlot = statusRound(j, u.slot);
    }
    const bool counted = j.sentSinceStatus >= j.k / 4;
    j.sentSinceStatus = 0;
    const uint16_t v = progressValue(j, u.base, u.window);
    if (u.base >= j.blocks())
    {
        j.maxConfirmed = v; // complete: exempt
        j.noProgress = 0;
        return true;
    }
    if (shrank && j.shrinks < SHRINKS_FREE)
    {
        // A page erased below the base: not counted (the maximum stays the
        // high-water mark, so a page that keeps failing still ends Failed)
        j.shrinks++;
        return true;
    }
    if (v > j.maxConfirmed)
    {
        j.maxConfirmed = v;
        j.noProgress = 0;
        j.progressSinceAccept = true;
        j.laps = 0;
        j.backoffS = 0;
        return true;
    }
    if (counted && ++j.noProgress >= NO_PROGRESS_STATUSES)
    {
        setState(j, State::Failed, n, Why::NoProgress);
        return false;
    }
    return true;
}

// An accepting UP: binding, anchor, view rebuilt
inline void applyAccepted(Job &j, const Up &u, const Now &n, Result &r)
{
    if (j.startValid && u.fcnt32 != j.expectBind)
    {
        r.bindMismatch = true; // the actual FCnt binds (the sensor binds to its real accepting UP)
    }
    j.startValid = false;
    j.bindValid = true;
    j.bindFcnt = u.fcnt32;
    j.sessionNonce = u.sessionNonce;
    j.transferUptimeS = n.uptimeS;
    anchorAt(j, u.rxUs, u.timeReliable, u.rxUs); // the accepting UP always anchors
    j.lastCheckedStatusSlot = -1;
    j.missedSinceStatus = 0;
    j.lastStatusUs = u.rxUs;
    j.sentSince.reset(j.blocks());
    j.sentSinceStatus = 0;
    j.confirmed.reset(j.blocks());
    j.viewBase = 0;
    uint16_t v = 0;
    if (u.acceptedArg == 1 && u.progress && j.confirmed.applyStatus(u.base, u.window))
    {
        j.viewBase = u.base;
        v = progressValue(j, u.base, u.window);
    }
    if (v > j.maxConfirmed)
    {
        j.noProgress = 0; // blocks received since the last status count as progress
    }
    j.maxConfirmed = v; // less: the sensor restarted the transfer (the maximum resets)
    j.progressSinceAccept = false;
    j.pendingEnd = PendingEnd::None;
}

inline bool startAllowed(const Job &j, const Up &u, const Now &n)
{
    return u.canStart && !j.manualPause && !(j.dlBackoff.armed() && !j.dlBackoff.reached(n)) &&
           !(j.dlBattery.armed() && !j.dlBattery.reached(n));
}

inline void refusal(Job &j, const Up &u, const Now &n)
{
    if (u.refusedArg == ep::ota::kRefuseBattery)
    {
        j.dlBattery.arm(n, BATTERY_RETRY_S);
        if (j.state == State::Paused && j.manualPause)
        {
            return; // paused by the admin (or held for an unsigned image): that stays the reason
        }
        if (j.state != State::Verified)
        {
            setState(j, State::Paused, n, Why::Battery);
        }
        return;
    }
    if (j.state == State::Verified)
    {
        setState(j, State::Failed, n, Why::Refused, u.refusedArg);
        return;
    }
    if (u.refusedArg == ep::ota::kRefuseUnconfirmed && u.curFw == j.fw)
    {
        setState(j, State::Confirming, n); // only this job's image can be the unconfirmed one
        return;
    }
    setState(j, State::Failed, n, Why::Refused, u.refusedArg);
}

// --- lifecycle --------------------------------------------------------------------------

inline void begin(Job &j, const Target &t, const Image &img, const Now &n)
{
    Job fresh;
    j = fresh;
    j.devId = t.devId;
    j.hwType = t.hwType;
    j.fw = img.fw;
    j.crc = img.crc;
    j.len = img.len;
    j.baseFw = t.fw;
    j.baseBoot = t.boot;
    j.baseFlags = t.flags;
    j.baseFlaggedRevert = flaggedRevert(t.boot, t.flags);
    j.intervalS = t.intervalS ? t.intervalS : 600;
    j.k = ep::ota::kMinK;
    j.cadence100ms = ep::ota::cadenceFor(ep::ota::airtimeUs(ep::ota::kBlockFrameLen, 9), j.k);
    j.startDelayMs = 4000;
    j.startedUnix = n.unixS;
    j.confirmed.reset(j.blocks());
    j.sentSince.reset(j.blocks());
    j.everSent.reset(j.blocks());
    setState(j, State::Waiting, n);
}

inline void afterReboot(Job &j, const Now &n)
{
    j.dlState.afterReboot(n);
    j.dlPaused.afterReboot(n);
    j.dlBackoff.afterReboot(n);
    j.dlBattery.afterReboot(n);
    j.dlAbortPending.afterReboot(n);
    j.dlRevertWatch.afterReboot(n);
    j.confirmed.reset(j.blocks());
    j.sentSince.reset(j.blocks());
    j.everSent.reset(j.blocks());
    j.anchored = j.anchorReliable = false;
    j.pendingEnd = PendingEnd::None;
    j.launchCount = j.launchNext = 0;
    j.startUptimeS = 0; // a start from before the reboot: its moment is unknown (RAM)
    if (j.state == State::Transfer)
    {
        setState(j, State::Paused, n, Why::Reboot);
    }
}

// The DOWN with a start was built for the UP upFcnt32 under the session
// sessionNonce: its accepting UP is the sensor's next UP (expected binding)
inline void startSent(Job &j, uint32_t upFcnt32, uint32_t sessionNonce, const Now &n)
{
    j.startValid = true;
    j.startNonce = sessionNonce;
    j.expectBind = upFcnt32 + 1;
    j.startUptimeS = n.uptimeS;
    // The "no accept" deadline of Waiting runs from the first starts (not
    // from every start: a sensor that never receives one must end Failed)
    if (j.state == State::Waiting && j.startsArmed < START_ARMS)
    {
        j.startsArmed++;
        j.dlState.arm(n, waitingDeadlineS(j));
    }
}

// A Verified job would transfer again (the sensor accepted with less than
// the whole image, or it left and would get a new start): with "Signed
// images only" on and an image that is not verified it is held instead -
// Paused like holdUnsigned(), nothing more goes to the sensor until the
// admin resumes (with the option off) or aborts. true: held
inline bool holdBeforeTransfer(Job &j, const Up &u, const Now &n)
{
    if (j.state != State::Verified || !u.signedOnly || j.imageVerified)
    {
        return false;
    }
    j.manualPause = true;
    j.laps = 0;
    j.pausedByMisses = false;
    setState(j, State::Paused, n, Why::Unsigned);
    return true;
}

// --- the UP ----------------------------------------------------------------------------

inline Result onUp(Job &j, const Up &u, const Now &n)
{
    Result r;
    const bool active = !terminal(j.state);

    // Abort delivery: an UP with bit 4 = 0 or "left 3" ends abortPending
    // before the DOWN is chosen
    if (j.abortPending && (!u.bit4 || (u.left && u.leftArg == ep::ota::kLeaveEnd)))
    {
        if (j.abortQuiet && !(u.left && u.leftArg == ep::ota::kLeaveEnd))
        {
            // No OTA end abort answered and no "left 3": the sensor did not
            // confirm the abort; its transfer state (page 118) may stay, and a
            // job with the same image would resume it (identity + CRC: safe)
            j.abortKept = true;
            r.abortKept = true;
        }
        j.abortQuiet = false;
        j.abortPending = false;
        j.dlAbortPending.clear();
        r.changed = true;
    }

    // Firmware facts: swap seen, installed, revert report
    const uint32_t fw = u.hasFw ? u.fw : u.curFw;
    const bool afterComplete = j.state == State::Swapping || j.state == State::Confirming;
    const bool flag3 = u.hasStatus && (u.flags & 0x08);
    const bool reinstall = j.fw == j.baseFw;
    bool swapSeen = false, installed = false, revert = false;
    if (u.hasFw && fw == j.fw)
    {
        if (fw != j.baseFw && (u.joinReq || u.hasStatus))
        {
            swapSeen = true;
        }
        if (u.hasStatus && reinstall && u.boot == 1 && flag3)
        {
            swapSeen = true;
        }
        if (u.hasStatus && (u.boot == 2 || u.boot == 6) &&
            (j.swapSeen || fw != j.baseFw || (flag3 && (afterComplete || !(j.baseFlags & 0x08)))))
        {
            installed = true;
        }
    }
    if (u.hasStatus && flaggedRevert(u.boot, u.flags))
    {
        revert = afterComplete || !j.baseFlaggedRevert;
    }

    if (!active)
    {
        // Terminal or no job: the revert watch, "installed despite abort", orphans
        // The watch runs only after the swap phase: every flagged revert counts
        if (u.hasStatus && flaggedRevert(u.boot, u.flags) && j.dlRevertWatch.armed() && !j.dlRevertWatch.reached(n))
        {
            r.revertRecord = true;
        }
        if (j.state == State::Aborted && (swapSeen || installed))
        {
            j.abortKept = r.abortKept = false; // it swapped: nothing kept to resume
            j.despiteAbort = true;
            setState(j, State::Done, n, Why::Admin);
            r.changed = r.log = true;
        }
        if (u.bit4)
        {
            r.down = j.abortPending ? Down::EndAbort : Down::EndPause; // orphan session
            if (j.abortPending && j.abortQuiet)
            {
                j.abortQuiet = false; // the sensor is in a session after all: the abort goes out
                r.changed = true;
            }
        }
        return r;
    }

    // Revert report and installed win over everything else
    if (revert)
    {
        setState(j, State::Reverted, n);
        r.revertRecord = r.changed = r.log = true;
        return r;
    }
    if (installed)
    {
        j.kept = u.boot == 6;
        setState(j, State::Done, n);
        r.changed = r.log = true;
        return r;
    }
    if (swapSeen)
    {
        j.swapSeen = true;
        if (j.state != State::Confirming)
        {
            setState(j, State::Confirming, n);
            r.changed = r.log = true;
        }
        return r;
    }
    if (j.state == State::Swapping || j.state == State::Confirming)
    {
        // Everything else is ignored here ("complete" again keeps Swapping)
        return r;
    }

    // A new session (JOIN_REQ, old version): no DOWN of ours follows it
    if (u.joinReq)
    {
        if (j.state == State::Transfer)
        {
            setState(j, State::Paused, n, Why::NewSession);
            r.changed = r.log = true;
        }
        return r;
    }

    // OTA status events
    if (u.failed)
    {
        setState(j, State::Failed, n, Why::SensorFailed, u.failedArg);
        r.changed = r.log = true;
        return r;
    }
    if (u.complete)
    {
        setState(j, State::Swapping, n);
        r.changed = r.log = true;
        return r;
    }
    if (u.batteryLow)
    {
        if (j.state != State::Verified)
        {
            setState(j, State::Verified, n, Why::Battery, u.batteryArg);
            r.changed = r.log = true;
        }
        return r;
    }
    if (u.refused)
    {
        if (j.state != State::Transfer)
        {
            refusal(j, u, n);
            r.changed = r.log = true;
        }
        return r;
    }
    if (u.accepted)
    {
        switch (j.state)
        {
        case State::Waiting:
            applyAccepted(j, u, n, r);
            setState(j, State::Transfer, n);
            r.changed = r.log = true;
            break;
        case State::Transfer:
            applyAccepted(j, u, n, r);
            r.changed = true;
            break;
        case State::Paused:
            applyAccepted(j, u, n, r);
            if (j.manualPause)
            {
                r.down = Down::EndPause; // the admin paused: the sensor leaves again
            }
            else
            {
                setState(j, State::Transfer, n);
                r.changed = r.log = true;
            }
            break;
        case State::Verified:
            if (!(u.progress && u.base >= j.blocks()))
            {
                applyAccepted(j, u, n, r);
                if (holdBeforeTransfer(j, u, n))
                {
                    // The sensor lost the image: no new transfer of an
                    // unsigned image under "Signed images only"
                    r.down = Down::EndPause;
                }
                else
                {
                    setState(j, State::Transfer, n);
                }
                r.changed = r.log = true;
            }
            break;
        default:
            break;
        }
        return r;
    }
    if (u.progress && u.bit4)
    {
        switch (j.state)
        {
        case State::Waiting:
            // The accepting UP was missed: a new start answers this UP, and the
            // "no accept" deadline runs again from this progress status
            if (u.canStart)
            {
                r.down = Down::Start;
            }
            j.dlState.arm(n, waitingDeadlineS(j));
            r.changed = true;
            break;
        case State::Transfer:
            if (!applyProgress(j, u, n))
            {
                r.changed = r.log = true;
                return r;
            }
            r.changed = true;
            break;
        case State::Paused:
            if (j.manualPause)
            {
                r.down = Down::EndPause;
                break;
            }
            if (j.startValid && j.startNonce == u.sessionNonce)
            {
                // A start went out in this session and its accepting UP was
                // lost (a start is sent only to a sensor that left its
                // session, so this status answers it): take over its binding
                j.sessionNonce = u.sessionNonce;
                j.bindFcnt = j.expectBind;
                j.bindValid = true;
                j.startValid = false;
                r.adopted = true;
            }
            else if (!(j.bindValid && j.sessionNonce == u.sessionNonce))
            {
                // A session nothing of ours is bound to: a new start
                if (startAllowed(j, u, n))
                {
                    r.down = Down::Start;
                }
                break;
            }
            if (applyProgress(j, u, n))
            {
                setState(j, State::Transfer, n);
                if (r.adopted && j.startUptimeS != 0)
                {
                    j.transferUptimeS = j.startUptimeS; // the start whose accepting UP was lost
                }
            }
            r.changed = r.log = true;
            break;
        default:
            break; // Verified: not sent in the wait
        }
        return r;
    }
    if (u.left)
    {
        const bool silence = u.leftArg == ep::ota::kLeaveSilence;
        const bool battery = u.leftArg == ep::ota::kLeaveBattery;
        const bool endLeft = u.leftArg == ep::ota::kLeaveEnd;
        switch (j.state)
        {
        case State::Waiting:
            if (!endLeft && startAllowed(j, u, n))
            {
                r.down = Down::Start;
            }
            break;
        case State::Transfer:
        case State::Paused:
        {
            const bool wasTransfer = j.state == State::Transfer;
            if (battery)
            {
                // The battery is below 2.3 V: a start now would only be
                // refused (6). Held like a refusal 6, retried after 6 h
                // (or after a Resume by the admin)
                j.dlBattery.arm(n, BATTERY_RETRY_S);
                if (wasTransfer)
                {
                    setState(j, State::Paused, n, Why::Battery);
                }
                else if (!j.manualPause) // paused by the admin: that stays the reason
                {
                    j.why = Why::Battery;
                    j.whyArg = 0;
                }
                r.changed = r.log = true;
                break;
            }
            bool starved = false;
            if (silence && (wasTransfer || j.pausedByMisses))
            {
                // The 2 rounds before the leave (or the pause by missed
                // statuses), but not before the job (re)entered Transfer
                // (launches before went to another schedule, none go while
                // Paused); shorter than one round it says nothing about the
                // gateway: a real leave
                const uint32_t end = j.pausedByMisses ? j.pausedUptimeS : n.uptimeS;
                j.pausedByMisses = false; // this pause is accounted: a second leave is not classified again
                uint32_t from = end - 2 * j.roundS();
                if ((int32_t)(from - j.transferUptimeS) < 0)
                {
                    from = j.transferUptimeS;
                }
                starved = (int32_t)(end - from) >= (int32_t)j.roundS() && launchesSince(j, from) < j.k / 4;
                if (starved)
                {
                    j.backoffS = j.backoffS == 0 ? BACKOFF_MIN_S
                                                 : (j.backoffS * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : j.backoffS * 2);
                    j.dlBackoff.arm(n, j.backoffS);
                }
                else if (!j.progressSinceAccept && ++j.laps >= NO_PROGRESS_LAPS)
                {
                    setState(j, State::Failed, n, Why::NoProgress);
                    r.changed = r.log = true;
                    return r;
                }
            }
            if (wasTransfer)
            {
                setState(j, State::Paused, n, starved ? Why::Starved : Why::Left, u.leftArg);
                r.log = true;
            }
            else if (starved && !j.manualPause)
            {
                j.why = Why::Starved; // paused by the admin (or held): that stays the reason
            }
            r.changed = true;
            // "left 1" / "left 3" get a start unless the admin paused (startAllowed)
            if (startAllowed(j, u, n))
            {
                r.down = Down::Start;
            }
            break;
        }
        case State::Verified:
            if (!endLeft && startAllowed(j, u, n))
            {
                if (holdBeforeTransfer(j, u, n))
                {
                    r.changed = r.log = true;
                }
                else
                {
                    r.down = Down::Start;
                }
            }
            break;
        default:
            break;
        }
        return r;
    }

    // No OTA event
    if (!u.bit4)
    {
        switch (j.state)
        {
        case State::Waiting:
            if (u.canStart)
            {
                r.down = Down::Start;
            }
            break;
        case State::Transfer:
            setState(j, State::Paused, n, Why::Left);
            r.changed = r.log = true;
            if (startAllowed(j, u, n))
            {
                r.down = Down::Start;
            }
            break;
        case State::Paused:
        case State::Verified:
            if (startAllowed(j, u, n))
            {
                if (j.state == State::Verified && holdBeforeTransfer(j, u, n))
                {
                    r.changed = r.log = true;
                }
                else
                {
                    r.down = Down::Start;
                }
            }
            break;
        default:
            break;
        }
    }
    return r;
}

// --- admin ----------------------------------------------------------------------------

inline bool canAbort(const Job &j)
{
    return j.state == State::Waiting || j.state == State::Transfer || j.state == State::Paused ||
           j.state == State::Verified;
}
inline bool canPause(const Job &j) { return j.state == State::Transfer; }
// Paused and held by the battery rule (refusal 6 or "left 2"): no start before dlBattery
inline bool batteryHold(const Job &j, const Now &n)
{
    return j.state == State::Paused && j.dlBattery.armed() && !j.dlBattery.reached(n);
}

// What a radio pause of a few minutes would cost a Waiting job
// (UpdateModePolicy::waitingAllows): a start out (its accepting UP would be
// lost) and how long the "no accept" deadline still runs
struct WaitView
{
    bool startOut = false;
    bool deadlineArmed = false;
    uint32_t deadlineLeftS = 0;
    uint32_t intervalS = 0;
};
inline WaitView waitView(const Job &j, const Now &n)
{
    WaitView w;
    w.startOut = j.startValid;
    w.deadlineArmed = j.dlState.armed();
    w.deadlineLeftS = j.dlState.leftS(n);
    w.intervalS = j.intervalS;
    return w;
}
// Resume: a manual pause, or the battery hold (the admin lifts it; a sensor
// still low refuses 6 again, which re-arms it)
inline bool canResume(const Job &j, const Now &n)
{
    return j.state == State::Paused && (j.manualPause || batteryHold(j, n));
}
inline bool canClear(const Job &j)
{
    return j.state == State::Verified || j.state == State::Swapping || j.state == State::Confirming;
}
inline bool canRetry(const Job &j)
{
    return j.state == State::Reverted || j.state == State::Failed || j.state == State::Aborted;
}

inline bool abort(Job &j, const Now &n)
{
    if (!canAbort(j))
    {
        return false;
    }
    if (j.state == State::Transfer)
    {
        j.pendingEnd = PendingEnd::Abort; // OTA end abort in the next block slot
    }
    j.abortPending = true;
    // Waiting: no transfer of this job on the sensor - unless a start went
    // out. The same for a job held by holdUnsigned() before the sensor ever
    // accepted a start (no binding, nothing confirmed)
    const bool noTransfer = j.state == State::Waiting ||
                            (j.state == State::Paused && j.why == Why::Unsigned && !j.bindValid && j.maxConfirmed == 0);
    j.abortQuiet = !noTransfer || j.startValid;
    j.abortKept = false;
    j.dlAbortPending.arm(n, 2 * j.roundS());
    setState(j, State::Aborted, n, Why::Admin);
    return true;
}

inline bool pause(Job &j, const Now &n)
{
    if (!canPause(j))
    {
        return false;
    }
    j.manualPause = true;
    j.laps = 0;
    j.pendingEnd = PendingEnd::Pause; // OTA end pause in the next block slot
    setState(j, State::Paused, n, Why::ManualPause);
    return true;
}

// "Signed images only" (M4c S2) with an unsigned image: a job that has not
// transferred the whole image yet (Waiting, Transfer, Paused) is held like a
// manual pause - nothing more goes to the sensor until the admin resumes
// (refused while the option is on and the image is unsigned) or aborts it.
// A job past the transfer (Verified and later) is not stopped: the image is
// already on the sensor. false: nothing to hold (or already held)
inline bool holdUnsigned(Job &j, const Now &n)
{
    if (j.state != State::Waiting && j.state != State::Transfer && j.state != State::Paused)
    {
        return false;
    }
    if (j.state == State::Paused && j.manualPause && j.why == Why::Unsigned)
    {
        return false;
    }
    if (j.state == State::Transfer)
    {
        j.pendingEnd = PendingEnd::Pause; // OTA end pause in the next block slot
    }
    j.manualPause = true;
    j.laps = 0;
    j.pausedByMisses = false; // a later "left 1" is not classified (the hold stays the reason)
    setState(j, State::Paused, n, Why::Unsigned);
    return true;
}

// The admin's confirmation of a start covers the reasons shown on the page
// (downgrade, reinstall, reverted before, unsigned image) for that image:
// the page carries this token, the start compares it with the reasons and
// image of that moment and asks again on any difference. Never 0 (FNV-1a)
inline uint32_t startConfirmToken(const char *reasons, size_t len, uint32_t imageCrc)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
    {
        h = (h ^ (uint8_t)reasons[i]) * 16777619u;
    }
    for (int i = 0; i < 4; i++)
    {
        h = (h ^ (uint8_t)(imageCrc >> (8 * i))) * 16777619u;
    }
    return h ? h : 1;
}

inline bool resume(Job &j, const Now &n)
{
    if (!canResume(j, n))
    {
        return false;
    }
    j.manualPause = false;
    j.dlBattery.clear();
    j.laps = 0;
    j.why = Why::None;
    if (j.pendingEnd == PendingEnd::Pause)
    {
        j.pendingEnd = PendingEnd::None; // not sent yet: nothing to pause
    }
    return true;
}

inline bool clear(Job &j, const Now &n)
{
    if (!canClear(j))
    {
        return false;
    }
    const bool watch = j.state == State::Swapping || j.state == State::Confirming;
    j.state = State::None;
    j.pendingEnd = PendingEnd::None;
    j.dlState.clear();
    j.ghost = watch;
    if (watch)
    {
        j.dlRevertWatch.arm(n, 3 * tConfirmS(j.intervalS));
    }
    return true;
}

// --- time ----------------------------------------------------------------------------

inline bool tick(Job &j, const Now &n)
{
    bool changed = false;
    if (j.abortPending && j.dlAbortPending.reached(n))
    {
        if (j.abortQuiet)
        {
            j.abortKept = true; // nothing answered the abort: the sensor keeps its state (as in onUp)
            j.abortQuiet = false;
        }
        j.abortPending = false;
        j.dlAbortPending.clear();
        if (j.pendingEnd == PendingEnd::Abort)
        {
            j.pendingEnd = PendingEnd::None;
        }
        changed = true;
    }
    if (j.dlRevertWatch.armed() && j.dlRevertWatch.reached(n))
    {
        j.dlRevertWatch.clear();
        if (j.ghost)
        {
            j.ghost = false;
        }
        changed = true;
    }
    switch (j.state)
    {
    case State::Waiting:
        if (j.dlState.reached(n))
        {
            setState(j, State::Failed, n, Why::NoAccept);
            changed = true;
        }
        break;
    case State::Transfer:
    {
        if (!j.anchored)
        {
            break;
        }
        // Status slots whose T_status (slot + 4 s) passed
        const int64_t first = j.slotUs(j.k) + STATUS_GRACE_US;
        if (n.us < first)
        {
            break;
        }
        const int64_t roundUs = (int64_t)j.roundS() * 1000000;
        const int32_t qMax = (int32_t)((n.us - first) / roundUs);
        for (int32_t q = j.lastCheckedStatusSlot + 1; q <= qMax; q++)
        {
            const uint32_t s = (uint32_t)q * ((uint32_t)j.k + 1) + j.k;
            j.rounds++;
            if (j.lastStatusUs < j.slotUs(s) - (int64_t)j.cadenceMs() * 500)
            {
                j.missedSinceStatus++;
                j.missedTotal++;
                changed = true;
            }
        }
        if (qMax > j.lastCheckedStatusSlot)
        {
            j.lastCheckedStatusSlot = qMax;
        }
        if (j.missedSinceStatus >= ep::ota::kStatusMissesToPause)
        {
            j.pausedByMisses = true;
            j.pausedUptimeS = n.uptimeS;
            setState(j, State::Paused, n, Why::Missed);
            changed = true;
        }
        break;
    }
    case State::Paused:
        if (j.dlPaused.reached(n))
        {
            setState(j, State::Aborted, n, Why::PausedTooLong);
            changed = true;
        }
        break;
    case State::Verified:
        if (j.dlState.reached(n))
        {
            setState(j, State::Aborted, n, Why::VerifiedTooLong);
            changed = true;
        }
        break;
    case State::Swapping:
        if (j.dlState.reached(n))
        {
            setState(j, State::Failed, n, Why::NoJoin);
            changed = true;
        }
        break;
    case State::Confirming:
        if (j.dlState.reached(n))
        {
            setState(j, State::Failed, n, Why::NotConfirmed);
            changed = true;
        }
        break;
    default:
        break;
    }
    return changed;
}

// --- blocks ----------------------------------------------------------------------------

inline bool nextBlockSlot(const Job &j, int64_t earliestUs, uint32_t &slot)
{
    // Blocks in Transfer; an OTA end (pause / abort by the admin) in the
    // next block slot of the session
    if (!j.anchored || !j.anchorReliable || j.cadence100ms == 0 ||
        (j.state != State::Transfer && j.pendingEnd == PendingEnd::None))
    {
        return false;
    }
    const int64_t s0 = j.slotUs(0);
    const int64_t cUs = (int64_t)j.cadenceMs() * 1000;
    uint32_t s = earliestUs <= s0 ? 0 : (uint32_t)((earliestUs - s0 + cUs - 1) / cUs);
    if (ep::ota::isStatusSlot(s, j.k))
    {
        s++;
    }
    slot = s;
    return true;
}

inline bool chooseBlock(const Job &j, uint16_t &index)
{
    const uint16_t n = j.blocks();
    const uint16_t base = j.viewBase;
    const uint32_t end = (uint32_t)base + ep::ota::kWindowBlocks < n ? (uint32_t)base + ep::ota::kWindowBlocks : n;
    for (uint32_t i = base; i < end; i++)
    {
        if (!j.confirmed.test((uint16_t)i) && !j.sentSince.test((uint16_t)i))
        {
            index = (uint16_t)i;
            return true;
        }
    }
    // Everything in the window sent since the status: unconfirmed blocks
    // again, cycling on from the last one launched (a lost one recurs, not
    // the lowest in every slot)
    const uint32_t span = end > base ? end - base : 0;
    const uint32_t from = (j.lastIndex >= base && j.lastIndex < end) ? (uint32_t)j.lastIndex + 1 - base : 0;
    for (uint32_t k = 0; k < span; k++)
    {
        const uint32_t i = base + (from + k) % span;
        if (!j.confirmed.test((uint16_t)i))
        {
            index = (uint16_t)i;
            return true;
        }
    }
    return false;
}

// A block slot that went unused (ETA statistics)
inline void skippedSlot(Job &j)
{
    j.skipped++;
}

inline void launched(Job &j, bool block, uint16_t index, uint32_t airMs, const Now &n)
{
    j.airMs += airMs;
    if (!block)
    {
        return;
    }
    if (j.everSent.test(index))
    {
        j.repeats++;
    }
    j.everSent.set(index);
    j.sentSince.set(index);
    j.lastIndex = index;
    j.blocksSent++;
    j.sentSinceStatus++;
    recordLaunch(j, n.uptimeS);
}

inline uint32_t etaS(const Job &j)
{
    const uint16_t n = j.blocks();
    const uint16_t done = j.maxConfirmed < n ? j.maxConfirmed : n;
    const uint32_t missing = n - done;
    // Every missing block needs 1 / (1 - r) launches (r = repeats per launch)
    // and every launch 1 / (1 - s) block slots (s = skipped slots), each
    // C x (K + 1) / K long; both rates capped at 80 %
    uint64_t r = j.blocksSent > 0 ? (uint64_t)j.repeats * 1000 / j.blocksSent : 0;
    const uint64_t slots = (uint64_t)j.blocksSent + j.skipped;
    uint64_t sk = slots > 0 ? (uint64_t)j.skipped * 1000 / slots : 0;
    r = r > 800 ? 800 : r;
    sk = sk > 800 ? 800 : sk;
    const uint64_t ms = (uint64_t)missing * j.cadenceMs() * ((uint32_t)j.k + 1) / j.k * 1000 / (1000 - r) * 1000 /
                        (1000 - sk);
    return (uint32_t)(ms / 1000);
}

} // namespace OtaJob
