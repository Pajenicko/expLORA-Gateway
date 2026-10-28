/**
 * expLORA Gateway
 *
 * AutoUpdatePolicy - pure decisions of the automatic updates (M4c S6,
 * Update/AutoUpdate.h): the settings, the nightly check time derived from
 * the MAC, the window on the local wall clock (DST days included), one run
 * per local day, the backoff after a failure, the gateway install
 * eligibility, the starvation rule, the choice of the next sensor and the
 * failure counters. Free of Arduino for the native tests
 * (test/test_auto_update).
 *
 * Time: the window is given in whole local hours [start, end) and may run
 * across midnight (e.g. 23-02); an empty window (start == end) is refused.
 * A window day D is the local date on which the window starts. The check
 * of day D is due at the wall time start + offset (offset = SHA-256(MAC ||
 * "explora-check") mod the window length) and stays due until the window
 * ends; a run that succeeds, or gives up for the night, marks D done. The
 * wall clock is used as one linear scale (day * 86400 + seconds of the
 * day), so:
 * - a repeated hour (DST end) cannot run the check twice: D is done;
 * - a skipped hour (DST start) cannot skip it: a due time inside the
 *   skipped hour is due at the first second after the jump, and a window
 *   that lies wholly inside it runs right after the jump (detected as a
 *   forward step of the wall clock larger than the real time that passed,
 *   at most 2 h).
 * The jump rule compares the wall step with the real (unix) step: an SNTP
 * step moves both alike and never counts as a jump - only a change of the
 * UTC offset (DST) does.
 * A failure inside the window is retried after 15 min, 30 min, 1 h, 2 h,
 * 4 h (then 4 h), each +-10 % (seeded by the MAC), while the window lasts;
 * past its end the night is given up. The retry time is persisted before
 * each run (as if it failed), so a restart inside the window - also one
 * caused by the run itself - waits for it instead of running again at once.
 * A run deferred by a sensor job that overruns into the window is tried
 * every 5 minutes and may still start up to 1 h after the window's end
 * (then the night is given up).
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "../Protocol/OtaJob.h"
#include "../Security/Sha256.h"
#include "UpdateState.h"

namespace AutoUpdatePolicy
{
// --- settings ---------------------------------------------------------------------

struct Settings
{
    bool gateway = false; // install a newer signed gateway firmware from the manifest
    bool sensors = false; // fetch newer signed sensor images and update the sensors one by one
    uint8_t startH = 2;   // window [startH, endH) local hours
    uint8_t endH = 5;
};

inline bool validWindow(int startH, int endH)
{
    return startH >= 0 && startH <= 23 && endH >= 0 && endH <= 23 && startH != endH;
}

// Window length in seconds (across midnight allowed)
inline uint32_t windowLenS(const Settings &s)
{
    return (uint32_t)((s.endH + 24 - s.startH) % 24) * 3600u;
}

// One Preferences value (written and read back as one key)
constexpr uint32_t PACK_MAGIC = 0xA5000000u;
inline uint32_t pack(const Settings &s)
{
    return PACK_MAGIC | (s.gateway ? 1u : 0u) | (s.sensors ? 2u : 0u) | (uint32_t)s.startH << 8 |
           (uint32_t)s.endH << 16;
}
// Anything else (missing key = 0, another layout, an invalid window): the
// defaults (both off, 02-05)
inline Settings unpack(uint32_t v)
{
    Settings s;
    if ((v & 0xFF000000u) != PACK_MAGIC || !validWindow((int)(v >> 8 & 0xFF), (int)(v >> 16 & 0xFF)))
    {
        return s;
    }
    s.gateway = (v & 1u) != 0;
    s.sensors = (v & 2u) != 0;
    s.startH = (uint8_t)(v >> 8 & 0xFF);
    s.endH = (uint8_t)(v >> 16 & 0xFF);
    return s;
}

// --- MAC seed ---------------------------------------------------------------------

// SHA-256(MAC || "explora-check"): bytes 0-7 the check offset, 8-23 the
// backoff jitter. false: the hash failed (the caller uses offset 0)
inline bool seed(const uint8_t mac[6], uint8_t out[32])
{
    Sha256 h;
    h.update(mac, 6);
    h.update((const uint8_t *)"explora-check", 13);
    return h.finish(out);
}

inline uint32_t checkOffsetS(const uint8_t digest[32], uint32_t lenS)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
    {
        v = v << 8 | digest[i];
    }
    return lenS ? (uint32_t)(v % lenS) : 0;
}

// The wait after failure number step (0 = the first): 15 min doubling up to
// 4 h, +-10 % from the seed (per step)
inline uint32_t backoffS(uint8_t step, const uint8_t digest[32])
{
    const uint32_t base = 900u << (step < 4 ? step : 4);
    const unsigned i = 8 + (step % 8) * 2;
    const int32_t permille = (int32_t)(((uint32_t)digest[i] << 8 | digest[i + 1]) % 201) - 100; // -100..100
    return (uint32_t)((int64_t)base * (1000 + permille) / 1000);
}

// --- local wall clock -------------------------------------------------------------

// Days since 1970-01-01 of a civil date (proleptic Gregorian)
inline int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

inline void civilFromDays(int32_t z, int32_t &y, uint32_t &m, uint32_t &d)
{
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int32_t)yoe + era * 400 + (m <= 2);
}

// A moment on the local wall clock and its real time
struct Clock
{
    int32_t day = 0;   // local date, days since 1970-01-01
    uint32_t sec = 0;  // seconds of the local day
    uint32_t unixS = 0; // real time
    int64_t wall() const { return (int64_t)day * 86400 + sec; }
};

inline Clock clockOf(const struct tm &t, uint32_t unixNow)
{
    Clock c;
    c.day = daysFromCivil(t.tm_year + 1900, (uint32_t)t.tm_mon + 1, (uint32_t)t.tm_mday);
    c.sec = (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + (t.tm_sec > 59 ? 59 : t.tm_sec));
    c.unixS = unixNow;
    return c;
}

// The clock may drive the schedule: SNTP synced in this run and not stale
// (Uplink::isTimeStale: a sync within 24 h), after 2023
inline bool clockValid(bool synced, bool stale, uint32_t unixNow)
{
    return synced && !stale && unixNow > 1700000000u;
}

// --- schedule ---------------------------------------------------------------------

// Persisted: doneDay (the last window day that ran or was given up).
// RAM: the backoff of the current window day and the previous sample
struct Sched
{
    int32_t doneDay = 0;
    int32_t retryDay = 0;  // the window day retryAt / step belong to (0 = none)
    uint32_t retryAt = 0;  // unixS: not before
    uint8_t step = 0;      // failures in retryDay
    int32_t graceDay = 0;  // the window day a sensor job deferred: it may run up to JOB_GRACE_S past the end
    bool havePrev = false;
    Clock prev;
};

// Wall points of window day D
inline int64_t windowStart(const Settings &s, int32_t d)
{
    return (int64_t)d * 86400 + (int64_t)s.startH * 3600;
}

// The end of window day D on the wall clock: with the grace after a
// deferral by a sensor job (the job overran into the window)
constexpr uint32_t JOB_GRACE_S = 3600;
inline int64_t windowEnd(const Sched &st, const Settings &s, int32_t d)
{
    return windowStart(s, d) + windowLenS(s) + (st.graceDay == d ? JOB_GRACE_S : 0);
}

constexpr uint32_t MAX_JUMP_S = 7200; // a DST step (1 h; 30 min somewhere), never a clock correction

struct Due
{
    bool due = false;
    int32_t day = 0; // the window day that is due
};

// Is a run due now? Call every pass (or every few seconds) with a valid
// clock; it records the sample for the jump rule
inline Due due(Sched &st, const Settings &s, uint32_t offsetS, const Clock &now)
{
    Due r;
    const uint32_t lenS = windowLenS(s);
    const int64_t a = now.wall();
    for (int32_t d = now.day - 1; d <= now.day && lenS; d++)
    {
        if (d <= st.doneDay)
        {
            continue;
        }
        const int64_t wDue = windowStart(s, d) + offsetS, wEnd = windowEnd(st, s, d);
        bool inWin = a >= wDue && a < wEnd;
        bool jumped = false;
        if (!inWin && st.havePrev && a >= wEnd)
        {
            // A forward step of the wall clock over this day's due point and
            // the end of its window (DST start): run right after it
            const int64_t pa = st.prev.wall();
            const int64_t wallStep = a - pa;
            const int64_t realStep = (int64_t)now.unixS - (int64_t)st.prev.unixS;
            jumped = pa < wDue && wallStep > realStep + 600 && wallStep <= (int64_t)MAX_JUMP_S + realStep &&
                     realStep >= 0 && realStep < 600;
        }
        if (!inWin && !jumped)
        {
            continue;
        }
        if (st.retryDay == d && (int32_t)(now.unixS - st.retryAt) < 0)
        {
            continue; // backoff
        }
        r.due = true;
        r.day = d;
        break;
    }
    st.prev = now;
    st.havePrev = true;
    return r;
}

enum class Outcome : uint8_t
{
    Ok,       // the run completed (nothing to do counts): the day is done
    Failed,   // the check / an install / a fetch failed: backoff
    Deferred, // could not start (a sensor job, an upload, offline, ...): tried again soon
};

constexpr uint32_t DEFER_RETRY_S = 300;

// After a run of window day d. true: the day is done (persist doneDay).
// jobDefer: deferred by a sensor job (the grace after the window applies)
inline bool finish(Sched &st, const Settings &s, const Clock &now, int32_t d, Outcome o,
                   const uint8_t digest[32], bool jobDefer = false)
{
    if (o == Outcome::Ok)
    {
        st.doneDay = d;
        st.retryDay = 0;
        st.step = 0;
        return true;
    }
    if (o == Outcome::Deferred && jobDefer)
    {
        st.graceDay = d;
    }
    if (st.retryDay != d)
    {
        st.retryDay = d;
        st.step = 0;
    }
    uint32_t wait = DEFER_RETRY_S;
    if (o == Outcome::Failed)
    {
        wait = backoffS(st.step, digest);
        if (st.step < 255)
        {
            st.step++;
        }
    }
    st.retryAt = now.unixS + wait;
    // Past the window's end (on the wall clock): given up for this night
    if (now.wall() + wait >= windowEnd(st, s, d))
    {
        st.doneDay = d;
        st.retryDay = 0;
        st.step = 0;
        return true;
    }
    return false;
}

// Before a run starts: the schedule as if it failed. Persisted (with
// restoreRetry() at the start), so a restart during the run - a crash in
// the check - waits for the backoff instead of running again at once
inline Sched ifFailed(const Sched &st, const Settings &s, const Clock &now, int32_t d, const uint8_t digest[32])
{
    Sched r = st;
    finish(r, s, now, d, Outcome::Failed, digest);
    return r;
}
inline void restoreRetry(Sched &st, int32_t day, uint8_t step, uint32_t at)
{
    st.retryDay = day;
    st.step = step;
    st.retryAt = at;
}

// The next run on the wall clock (day, seconds) or, during a backoff, as
// real time (retry = true, at = unixS)
struct Next
{
    bool retry = false;
    uint32_t at = 0;
    int32_t day = 0;
    uint32_t sec = 0;
};

inline Next next(const Sched &st, const Settings &s, uint32_t offsetS, const Clock &now)
{
    Next n;
    const uint32_t lenS = windowLenS(s);
    const int64_t a = now.wall();
    // Up to the day after tomorrow: the night may be done one day ahead
    // (a run after midnight of a window that starts the evening before)
    for (int32_t d = now.day - 1; d <= now.day + 2; d++)
    {
        if (d <= st.doneDay || a >= windowEnd(st, s, d) || !lenS)
        {
            continue;
        }
        if (st.retryDay == d && (int32_t)(now.unixS - st.retryAt) < 0)
        {
            n.retry = true;
            n.at = st.retryAt;
            return n;
        }
        const int64_t w = windowStart(s, d) + offsetS;
        n.day = (int32_t)(w / 86400);
        n.sec = (uint32_t)(w % 86400);
        return n;
    }
    return n;
}

// Seconds until the next window starts that is not done (0 = inside it
// now). Auto sensor jobs do not start when their estimate would reach it
inline int64_t secondsToWindow(const Sched &st, const Settings &s, const Clock &now)
{
    const uint32_t lenS = windowLenS(s);
    const int64_t a = now.wall();
    for (int32_t d = now.day - 1; d <= now.day + 1; d++)
    {
        const int64_t w0 = windowStart(s, d);
        if (d <= st.doneDay || a >= windowEnd(st, s, d) || !lenS)
        {
            continue;
        }
        return a >= w0 ? 0 : w0 - a;
    }
    return 86400;
}

// An auto sensor job may start: outside a window whose run is not done,
// and not running into the next one (estimate + 30 min margin). Auto jobs
// are radio-paced (no TLS), so they need no window - but a job in Transfer
// blocks the nightly session, which must keep its turn
constexpr uint32_t JOB_MARGIN_S = 1800;
inline bool sensorJobFits(const Sched &st, const Settings &s, const Clock &now, uint32_t estimateS)
{
    return secondsToWindow(st, s, now) >= (int64_t)estimateS + JOB_MARGIN_S;
}

// --- gateway install --------------------------------------------------------------

struct Activity
{
    OtaJob::State job = OtaJob::State::None;
    bool startPending = false; // a sensor OTA start is queued
    bool upload = false;       // a firmware or sensor image upload runs
    bool confirmWait = false;  // a gateway image waits for the admin's confirmation (the install would drop it)
    bool download = false;     // a gateway URL install, sensor image download or update check is queued / runs
    bool validation = false;   // the running firmware waits for its validation
    bool restart = false;      // a restart is requested
    bool session = false;      // an update-mode session runs
};

enum class Defer : uint8_t
{
    None,
    SensorJob,   // a job in Waiting / Transfer / Verified / Swapping / Confirming
    StartQueued, // a queued sensor OTA start
    Upload,
    Confirm,
    Download,
    Validation,
    Restart,
    Session,
};

inline const char *deferText(Defer d)
{
    switch (d)
    {
    case Defer::None:
        return "";
    case Defer::SensorJob:
        return "a sensor OTA job is running";
    case Defer::StartQueued:
        return "a sensor OTA start is queued";
    case Defer::Upload:
        return "an upload is running";
    case Defer::Confirm:
        return "a firmware image waits for the admin's confirmation";
    case Defer::Download:
        return "a manual download or check is running";
    case Defer::Validation:
        return "the running firmware is being validated";
    case Defer::Restart:
        return "a restart is pending";
    case Defer::Session:
        return "another update session is running";
    }
    return "busy";
}

// The nightly session (check + gateway install + sensor fetches) may start
// now: no sensor job that needs the radio (a Paused job does not block: a
// restart is a designed Paused path), no queued start, no upload, manual
// download, image waiting for the admin's confirmation, validation,
// pending restart or other session. Manual actions always win
inline Defer sessionDefer(const Activity &a)
{
    if (a.session)
    {
        return Defer::Session;
    }
    if (a.restart)
    {
        return Defer::Restart;
    }
    if (a.validation)
    {
        return Defer::Validation;
    }
    if (a.upload)
    {
        return Defer::Upload;
    }
    if (a.confirmWait)
    {
        return Defer::Confirm;
    }
    if (a.download)
    {
        return Defer::Download;
    }
    if (a.startPending)
    {
        return Defer::StartQueued;
    }
    if (!OtaJob::terminal(a.job) && a.job != OtaJob::State::Paused)
    {
        return Defer::SensorJob;
    }
    return Defer::None;
}

// Strikes of a gateway version (persisted; three stop its automatic
// install until another version is offered). A strike is counted
// - when an attempt reaches the activation: the image is written, verified
//   and matches the manifest; the strike is saved before the boot partition
//   is switched (the install is refused if it cannot be saved), so a crash,
//   a failed validation or a rollback of the new firmware counts; it is
//   cleared only once the new firmware is validated;
// - when the image itself is refused: larger than the slot, size / SHA-256 /
//   version not as in the manifest, not verified, for another board.
// At most one strike per version and night: after a counted attempt the
// version is not tried again that night. Download and network failures
// (and a refused start: busy, validation) are no strike: they are retried
// with the window's backoff and counted in RAM only (DL_TRIES in a row in
// this run - or a refused image - and the version stops blocking the
// automatic sensor jobs until the gateway restarts)
constexpr uint8_t GATEWAY_TRIES = 3;
constexpr uint8_t DL_TRIES = 3;
inline bool gatewayStruck(uint32_t failVer, uint8_t fails, uint32_t version)
{
    return failVer == version && fails >= GATEWAY_TRIES;
}
inline bool gatewayTriedTonight(uint32_t failVer, int32_t failDay, uint32_t version, int32_t day)
{
    return failVer == version && failDay == day;
}

// How an automatic gateway install ended without an activation
enum class GwEnd : uint8_t
{
    Transport, // the download failed (network, server): no strike, backoff
    Refused,   // the image was refused (size, SHA-256, version, signature, board): a strike
    Busy,      // the install could not start or its strike could not be saved: no strike, backoff
};

// A day done in the future (the clock was set wrong when it was stored)
// would stop the runs until that date: forgotten. A real one is never
// after today (window day = the date its window starts)
inline bool futureDay(int32_t doneDay, int32_t today)
{
    return doneDay > today;
}

// The first passes after a start: what the persisted record says about the
// automatic install of version failVer (act: it was activated and its
// outcome is not known yet)
enum class GwBoot : uint8_t
{
    None,
    Validating, // the new firmware runs and waits for its validation: nothing decided yet
    Installed,  // ... validated: the strikes are cleared
    RolledBack, // the previous firmware runs again: the strike stays
};
inline GwBoot gatewayBoot(bool act, uint8_t fails, uint32_t failVer, uint32_t running, bool validating)
{
    if (fails && failVer == running)
    {
        return validating ? GwBoot::Validating : GwBoot::Installed;
    }
    return act ? GwBoot::RolledBack : GwBoot::None;
}
inline void gatewayAttempt(uint32_t &failVer, uint8_t &fails, uint32_t version)
{
    if (failVer != version)
    {
        failVer = version;
        fails = 0;
    }
    if (fails < 255)
    {
        fails++;
    }
}

// A gateway update is pending: switched on, a newer version offered by the
// last accepted manifest (or, until the next check, by the one before a
// restart: persisted), not given up and not released (dlReleased: DL_TRIES
// download failures in a row in this run, or its image was refused). Then
// no new automatic sensor job starts (a queue of sensors must not starve
// the gateway)
inline bool gatewayPending(bool autoGateway, bool offeredNewer, uint32_t version, uint32_t failVer, uint8_t fails,
                           bool dlReleased = false)
{
    return autoGateway && offeredNewer && !dlReleased && !gatewayStruck(failVer, fails, version);
}

// Without an accepted manifest in this run (after a restart, or after a
// failed check, which drops the offers) the persisted pending version
// still holds the automatic sensor jobs back only in the rollback case -
// it was attempted (strike count > 0), is not given up - and for at most
// PEND_HOLD_DAYS window days after that attempt: a manifest that keeps
// failing cannot hold the sensors back without bound. A successful check
// replaces it (offered again, or not)
constexpr int32_t PEND_HOLD_DAYS = 3;
// What a check persists as pending: the offered version unless it is
// released in this run (DL_TRIES download failures, a refused image) -
// otherwise a restart would hold the sensors back again for a version that
// was struck for a refusal, not rolled back (the activation persists its
// version itself)
inline uint32_t pendingToSave(uint32_t offered, bool released)
{
    return released ? 0 : offered;
}
inline bool persistedPending(uint32_t pendGw, uint32_t failVer, uint8_t fails, int32_t failDay, int32_t today)
{
    return pendGw && fails && failVer == pendGw && !gatewayStruck(failVer, fails, pendGw) &&
           today - failDay <= PEND_HOLD_DAYS;
}

// An HTTP answer that says the file is not there (the manifest names a URL
// without a file): a strike like a refused image. Only 404 and 410: every
// other status - 401 / 403 (a CDN bot challenge, a WAF rule, a permission
// set wrong for a while), 408 / 429, 5xx - is a transport failure, retried
// without a strike
inline bool refusedHttp(int code)
{
    return code == 404 || code == 410;
}

// A download that was still running when the gateway restarted (persisted
// marker "gwDl", cleared at every end): counted at the next start as a
// strike of its version - at most one per night like any other strike
// (gatewayTriedTonight). A download that crashes the gateway thus gives the
// version up after three nights instead of repeating forever. true: a
// strike was counted
inline bool downloadInterrupted(uint32_t &failVer, uint8_t &fails, int32_t &failDay, uint32_t dlVer, int32_t dlDay)
{
    if (!dlVer || gatewayTriedTonight(failVer, failDay, dlVer, dlDay))
    {
        return false;
    }
    gatewayAttempt(failVer, fails, dlVer);
    failDay = dlDay;
    return true;
}

// --- sensors ----------------------------------------------------------------------

constexpr uint8_t SENSOR_TRIES = 2; // auto failures of one version per sensor

struct SensorInfo
{
    uint32_t devId = 0;
    bool hasFw = false;
    uint16_t hw = 0;
    uint32_t fw = 0;
    bool hasReverted = false;
    uint32_t revertedFw = 0;
    uint32_t failFw = 0; // auto failures: of this version
    uint8_t fails = 0;
};

struct ImageInfo
{
    bool stored = false;
    uint32_t fw = 0;
    bool verified = false; // signed with a release key this firmware carries
};

enum class Skip : uint8_t
{
    None,
    NoFw,      // no JOIN_REQ seen yet (HW type and firmware unknown)
    NoImage,   // no image stored for its HW type
    Unsigned,  // the stored image is not verified
    NotNewer,  // the image is not newer than the sensor's firmware
    Reverted,  // this version reverted on the sensor before (by version, also a rebuild)
    Failed,    // SENSOR_TRIES automatic failures of this version
};

inline const char *skipText(Skip k)
{
    switch (k)
    {
    case Skip::None:
        return "";
    case Skip::NoFw:
        return "firmware unknown";
    case Skip::NoImage:
        return "no image for its HW type";
    case Skip::Unsigned:
        return "image not signed";
    case Skip::NotNewer:
        return "up to date";
    case Skip::Reverted:
        return "this version reverted on it before";
    case Skip::Failed:
        return "failed twice with this version";
    }
    return "?";
}

inline Skip sensorSkip(const SensorInfo &s, const ImageInfo &img)
{
    if (!s.hasFw)
    {
        return Skip::NoFw;
    }
    if (!img.stored)
    {
        return Skip::NoImage;
    }
    if (!img.verified)
    {
        return Skip::Unsigned;
    }
    if (img.fw <= s.fw)
    {
        return Skip::NotNewer;
    }
    if (s.hasReverted && s.revertedFw == img.fw)
    {
        return Skip::Reverted;
    }
    if (s.failFw == img.fw && s.fails >= SENSOR_TRIES)
    {
        return Skip::Failed;
    }
    return Skip::None;
}

// Round robin by DevID: the indices of devIds in the order to try, the
// first DevID above cursor first, wrapping (cursor = the DevID of the last
// automatic start). n <= 32
inline size_t roundRobin(const uint32_t *devIds, size_t n, uint32_t cursor, uint8_t *order)
{
    size_t k = 0;
    bool used[32] = {};
    n = n > 32 ? 32 : n;
    for (int pass = 0; pass < 2; pass++)
    {
        for (;;)
        {
            int best = -1;
            for (size_t i = 0; i < n; i++)
            {
                const bool side = pass == 0 ? devIds[i] > cursor : devIds[i] <= cursor;
                if (!used[i] && side && (best < 0 || devIds[i] < devIds[best]))
                {
                    best = (int)i;
                }
            }
            if (best < 0)
            {
                break;
            }
            used[best] = true;
            order[k++] = (uint8_t)best;
        }
    }
    return k;
}

// How an automatic job ended counts for its sensor: Failed (also no
// progress) and Aborted by the gateway are failures; an abort by the admin
// is not (manual actions win, the sensor is not given up for it), nor is a
// start refused before the job existed (busy, a manual action queued) -
// such a sensor is tried again later. Reverted is recorded by the job code
// (never retried automatically), Done needs nothing
inline bool countsAsFailure(OtaJob::State s, OtaJob::Why why)
{
    return s == OtaJob::State::Failed || (s == OtaJob::State::Aborted && why != OtaJob::Why::Admin);
}

inline void recordFailure(uint32_t &failFw, uint8_t &fails, uint32_t fw)
{
    if (failFw != fw)
    {
        failFw = fw;
        fails = 0;
    }
    if (fails < 255)
    {
        fails++;
    }
}

// Sensor image fetches from the manifest: a version of a HW type whose
// nightly fetch failed on FETCH_TRIES nights (one strike per night) is not
// fetched again until the manifest offers another version for it. A
// success or another version clears its record (one per HW type, at most
// FETCH_SLOTS; persisted in state.json)
constexpr uint8_t FETCH_TRIES = 3;
constexpr size_t FETCH_SLOTS = UpdateState::FETCH_SLOTS;
using FetchStrike = UpdateState::FetchStrike;

inline bool fetchStruck(const FetchStrike *f, uint16_t hw, uint32_t ver)
{
    for (size_t i = 0; i < FETCH_SLOTS; i++)
    {
        if (f[i].n && f[i].hw == hw && f[i].ver == ver && f[i].n >= FETCH_TRIES)
        {
            return true;
        }
    }
    return false;
}

// true: the record changed (save it)
inline bool fetchResult(FetchStrike *f, uint16_t hw, uint32_t ver, bool ok, int32_t day)
{
    // at: this HW type's record; victim: a free slot, else the oldest strike
    size_t at = FETCH_SLOTS, victim = 0;
    for (size_t i = 0; i < FETCH_SLOTS; i++)
    {
        if (f[i].n && f[i].hw == hw)
        {
            at = i;
        }
        if (f[victim].n && (f[i].n == 0 || f[i].day < f[victim].day))
        {
            victim = i;
        }
    }
    if (ok)
    {
        if (at == FETCH_SLOTS)
        {
            return false;
        }
        f[at] = FetchStrike();
        return true;
    }
    if (at == FETCH_SLOTS || f[at].ver != ver)
    {
        at = at == FETCH_SLOTS ? victim : at;
        f[at] = FetchStrike();
        f[at].hw = hw;
        f[at].ver = ver;
    }
    else if (f[at].day == day)
    {
        return false; // one strike per night
    }
    f[at].day = day;
    if (f[at].n < 255)
    {
        f[at].n++;
    }
    return true;
}

// How a nightly sensor image fetch ended: only a refused image (not
// stored - size, SHA-256, version, signature, HW type) or an HTTP answer
// that says the file is not there (refusedHttp: 404 / 410) is a strike; a transport failure
// (network, server, too slow, session limit) is none, like the gateway's
// download failures - it is retried with the window's backoff
enum class FetchEnd : uint8_t
{
    Stored,
    Transport,
    Refused,
};
inline bool fetchEnded(FetchStrike *f, uint16_t hw, uint32_t ver, FetchEnd e, int32_t day)
{
    return e != FetchEnd::Transport && fetchResult(f, hw, ver, e == FetchEnd::Stored, day);
}

// sensors.json, gen 2 entry: "autoFail":[fw, count], written only when the
// count is not 0 (a file without automatic failures stays byte-identical
// to the earlier firmware's); an older firmware ignores the member
inline void writeAutoFail(JsonObject o, uint32_t failFw, uint8_t fails)
{
    if (fails)
    {
        JsonArray a = o.createNestedArray("autoFail");
        a.add(failFw);
        a.add(fails);
    }
}

inline void readAutoFail(JsonObjectConst o, uint32_t &failFw, uint8_t &fails)
{
    JsonArrayConst a = o["autoFail"];
    failFw = 0;
    fails = 0;
    if (!a.isNull() && a.size() == 2 && a[0].is<uint32_t>() && a[1].is<uint8_t>())
    {
        failFw = a[0].as<uint32_t>();
        fails = a[1].as<uint8_t>();
    }
}
} // namespace AutoUpdatePolicy
