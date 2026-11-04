/**
 * Native unit tests for the automatic updates (src/Update/AutoUpdatePolicy.h)
 * and their persisted fields (/update/state.json "auto", sensors.json
 * "autoFail"): the check time per MAC and window, DST spring and autumn
 * days, one run per local day, the backoff sequence with its jitter bounds
 * and the window end, the session eligibility matrix, the starvation rule,
 * the sensor choice (reverted / failed / not newer / unsigned / no image),
 * the round robin, the three-strikes gateway rule, the settings and the
 * file compatibility.
 *
 * Run:   pio test -e native -f test_auto_update
 */

#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unity.h>

#include "../../src/Update/AutoUpdatePolicy.h"
#include "../../src/Update/UpdateState.h"

using namespace AutoUpdatePolicy;

void setUp(void) {}
void tearDown(void) {}

static const char *TZ_PRAGUE = "CET-1CEST,M3.5.0,M10.5.0/3";

static void useTz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static Clock at(uint32_t unixNow)
{
    time_t t = (time_t)unixNow;
    struct tm tm;
    localtime_r(&t, &tm);
    return clockOf(tm, unixNow);
}

// Unix time of a local wall time (tm_isdst = -1)
static uint32_t local(int y, int mo, int d, int h, int mi, int s = 0)
{
    struct tm tm = {};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return (uint32_t)mktime(&tm);
}

static void digestOf(const uint8_t mac[6], uint8_t d[32])
{
    TEST_ASSERT_TRUE(seed(mac, d));
}

void test_settings_pack_and_validation()
{
    Settings s;
    TEST_ASSERT_FALSE(s.gateway);
    TEST_ASSERT_FALSE(s.sensors);
    TEST_ASSERT_EQUAL(2, s.startH);
    TEST_ASSERT_EQUAL(5, s.endH);
    TEST_ASSERT_EQUAL_UINT32(3 * 3600, windowLenS(s));
    // Missing key / other layout -> defaults
    Settings d = unpack(0);
    TEST_ASSERT_FALSE(d.gateway);
    TEST_ASSERT_EQUAL(2, d.startH);
    s.gateway = true;
    s.startH = 23;
    s.endH = 2;
    Settings r = unpack(pack(s));
    TEST_ASSERT_TRUE(r.gateway);
    TEST_ASSERT_FALSE(r.sensors);
    TEST_ASSERT_EQUAL(23, r.startH);
    TEST_ASSERT_EQUAL(2, r.endH);
    TEST_ASSERT_EQUAL_UINT32(3 * 3600, windowLenS(r)); // across midnight
    // Invalid windows refused / unpacked to the defaults
    TEST_ASSERT_FALSE(validWindow(3, 3));
    TEST_ASSERT_FALSE(validWindow(-1, 3));
    TEST_ASSERT_FALSE(validWindow(2, 24));
    TEST_ASSERT_TRUE(validWindow(0, 23));
    TEST_ASSERT_TRUE(validWindow(23, 0));
    TEST_ASSERT_EQUAL(2, unpack(PACK_MAGIC | 3u << 8 | 3u << 16 | 3u).startH);
    TEST_ASSERT_FALSE(unpack(PACK_MAGIC | 3u << 8 | 3u << 16 | 3u).gateway);
    TEST_ASSERT_EQUAL(2, unpack(PACK_MAGIC | 30u << 8 | 3u << 16).startH);
}

// The check offset for several MACs and windows: inside the window,
// deterministic, different MACs spread
void test_offset_table()
{
    const uint8_t macs[4][6] = {{0x24, 0x6F, 0x28, 0x01, 0x02, 0x03},
                                {0x24, 0x6F, 0x28, 0x01, 0x02, 0x04},
                                {0xF4, 0x12, 0xFA, 0x9C, 0x00, 0x01},
                                {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    const uint32_t lens[3] = {3600, 3 * 3600, 23 * 3600};
    uint32_t seen[4];
    for (int m = 0; m < 4; m++)
    {
        uint8_t d[32], d2[32];
        digestOf(macs[m], d);
        digestOf(macs[m], d2);
        TEST_ASSERT_EQUAL_MEMORY(d, d2, 32);
        for (uint32_t len : lens)
        {
            const uint32_t o = checkOffsetS(d, len);
            TEST_ASSERT_TRUE(o < len);
        }
        seen[m] = checkOffsetS(d, 3 * 3600);
    }
    // Neighbouring MACs do not share the time
    TEST_ASSERT_NOT_EQUAL(seen[0], seen[1]);
    TEST_ASSERT_NOT_EQUAL(seen[0], seen[2]);
    // The hash is SHA-256(MAC || "explora-check"): pinned for one MAC
    uint8_t d[32];
    digestOf(macs[3], d);
    Sha256 h;
    uint8_t in[19] = {0};
    memcpy(in + 6, "explora-check", 13);
    h.update(in, sizeof(in));
    uint8_t ref[32];
    TEST_ASSERT_TRUE(h.finish(ref));
    TEST_ASSERT_EQUAL_MEMORY(ref, d, 32);
    TEST_ASSERT_EQUAL_UINT32(0, checkOffsetS(d, 0));
}

void test_civil_days()
{
    TEST_ASSERT_EQUAL_INT32(0, daysFromCivil(1970, 1, 1));
    TEST_ASSERT_EQUAL_INT32(20386, daysFromCivil(2025, 10, 25));
    for (int32_t z = 19000; z < 21000; z += 37)
    {
        int32_t y;
        uint32_t m, d;
        civilFromDays(z, y, m, d);
        TEST_ASSERT_EQUAL_INT32(z, daysFromCivil(y, m, d));
    }
}

// Runs a simulated clock from `from` for `seconds` in steps; returns the
// unix times at which a run was due (each run reported Ok)
static int simulate(uint32_t from, uint32_t seconds, uint32_t step, const Settings &s, uint32_t offset, Sched &st,
                    uint32_t *runs, int maxRuns, const uint8_t *digest, Outcome outcome = Outcome::Ok)
{
    int n = 0;
    for (uint32_t t = from; t < from + seconds; t += step)
    {
        const Clock c = at(t);
        Due d = due(st, s, offset, c);
        if (d.due)
        {
            if (n < maxRuns)
            {
                runs[n] = t;
            }
            n++;
            finish(st, s, c, d.day, outcome, digest);
        }
    }
    return n;
}

void test_one_run_per_day_normal_days()
{
    useTz(TZ_PRAGUE);
    Settings s;
    uint8_t dg[32] = {};
    Sched st;
    uint32_t runs[8];
    // Offset 1 h: 03:00 local each day, three days
    const int n = simulate(local(2026, 6, 10, 0, 0), 3 * 86400, 30, s, 3600, st, runs, 8, dg);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT32(local(2026, 6, 10, 3, 0), runs[0]);
    TEST_ASSERT_EQUAL_UINT32(local(2026, 6, 11, 3, 0), runs[1]);
    TEST_ASSERT_EQUAL_UINT32(local(2026, 6, 12, 3, 0), runs[2]);
    // Across midnight: window 23-02, offset 2 h -> 01:00 of the next date,
    // counted for the window day it started on
    s.startH = 23;
    s.endH = 2;
    Sched st2;
    const int m = simulate(local(2026, 6, 10, 12, 0), 2 * 86400, 30, s, 7200, st2, runs, 8, dg);
    TEST_ASSERT_EQUAL(2, m);
    TEST_ASSERT_EQUAL_UINT32(local(2026, 6, 11, 1, 0), runs[0]);
    TEST_ASSERT_EQUAL_UINT32(local(2026, 6, 12, 1, 0), runs[1]);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 6, 11), st2.doneDay);
    // A gateway that starts inside the window after the due time runs at
    // once; one that starts after the window waits for the next night
    s = Settings();
    Sched st3;
    TEST_ASSERT_EQUAL(1, simulate(local(2026, 6, 10, 4, 30), 600, 30, s, 3600, st3, runs, 8, dg));
    TEST_ASSERT_EQUAL_UINT32(local(2026, 6, 10, 4, 30), runs[0]);
    Sched st4;
    TEST_ASSERT_EQUAL(0, simulate(local(2026, 6, 10, 5, 0), 20 * 3600, 30, s, 3600, st4, runs, 8, dg));
    // A done day stays done after a restart (doneDay persisted, no prev)
    Sched st5;
    st5.doneDay = daysFromCivil(2026, 6, 10);
    TEST_ASSERT_EQUAL(0, simulate(local(2026, 6, 10, 3, 30), 3600, 30, s, 3600, st5, runs, 8, dg));
}

// DST start (29 March 2026, 02:00 -> 03:00 in Prague): never skipped
void test_dst_spring()
{
    useTz(TZ_PRAGUE);
    uint8_t dg[32] = {};
    uint32_t runs[8];
    Settings s; // 02-05
    // Due time inside the skipped hour (02:30): runs at the first second after the jump
    Sched st;
    TEST_ASSERT_EQUAL(1, simulate(local(2026, 3, 28, 22, 0), 12 * 3600, 10, s, 1800, st, runs, 8, dg));
    TEST_ASSERT_EQUAL_UINT32(local(2026, 3, 29, 3, 0), runs[0]);
    // Due time after it (04:00): on time
    Sched st2;
    TEST_ASSERT_EQUAL(1, simulate(local(2026, 3, 28, 22, 0), 12 * 3600, 10, s, 7200, st2, runs, 8, dg));
    TEST_ASSERT_EQUAL_UINT32(local(2026, 3, 29, 4, 0), runs[0]);
    // A window wholly inside the skipped hour (02-03): right after the jump
    s.endH = 3;
    Sched st3;
    TEST_ASSERT_EQUAL(1, simulate(local(2026, 3, 28, 22, 0), 12 * 3600, 10, s, 1200, st3, runs, 8, dg));
    TEST_ASSERT_EQUAL_UINT32(local(2026, 3, 29, 3, 0), runs[0]);
    // ... also with a coarse loop (a pass every 60 s)
    Sched st4;
    TEST_ASSERT_EQUAL(1, simulate(local(2026, 3, 28, 22, 0), 12 * 3600, 60, s, 1200, st4, runs, 8, dg));
    // ... but not after a restart past it (no sample before the jump)
    Sched st5;
    TEST_ASSERT_EQUAL(0, simulate(local(2026, 3, 29, 3, 30), 3600, 10, s, 1200, st5, runs, 8, dg));
    // The days around keep one run each
    s.endH = 5;
    Sched st6;
    TEST_ASSERT_EQUAL(3, simulate(local(2026, 3, 28, 0, 0), 3 * 86400, 30, s, 1800, st6, runs, 8, dg));
}

// DST end (25 October 2026, 03:00 -> 02:00): the repeated hour never runs twice
void test_dst_autumn()
{
    useTz(TZ_PRAGUE);
    uint8_t dg[32] = {};
    uint32_t runs[8];
    Settings s;
    const uint32_t offsets[4] = {0, 1800, 3600, 3 * 3600 - 60};
    for (uint32_t o : offsets)
    {
        Sched st;
        TEST_ASSERT_EQUAL(1, simulate(local(2026, 10, 24, 22, 0), 12 * 3600, 10, s, o, st, runs, 8, dg));
    }
    // Failures from the first 02:30 on, retried through the repeated hour:
    // the retries follow the backoff in real time (never a fresh run for
    // the repeated wall time), the night ends with the window
    Sched st;
    const int n = simulate(local(2026, 10, 24, 22, 0), 10 * 3600, 10, s, 1800, st, runs, 8, dg, Outcome::Failed);
    TEST_ASSERT_TRUE(n >= 3 && n <= 5);
    for (int i = 0; i + 1 < n && i + 1 < 8; i++)
    {
        TEST_ASSERT_TRUE(runs[i + 1] - runs[i] >= 810);
    }
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 10, 25), st.doneDay);
}

void test_backoff_sequence_jitter_and_window_end()
{
    uint8_t dg[32];
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    digestOf(mac, dg);
    const uint32_t bases[6] = {900, 1800, 3600, 7200, 14400, 14400};
    for (uint8_t step = 0; step < 6; step++)
    {
        const uint32_t b = backoffS(step, dg);
        TEST_ASSERT_TRUE(b >= bases[step] * 9 / 10 && b <= bases[step] * 11 / 10);
    }
    // The jitter differs between MACs (for most steps)
    uint8_t dg2[32];
    const uint8_t mac2[6] = {1, 2, 3, 4, 5, 7};
    digestOf(mac2, dg2);
    int differ = 0;
    for (uint8_t step = 0; step < 4; step++)
    {
        differ += backoffS(step, dg) != backoffS(step, dg2);
    }
    TEST_ASSERT_TRUE(differ >= 3);
    // Every jitter value stays within +-10 % (all byte pairs)
    uint8_t z[32] = {};
    for (int v = 0; v < 65536; v += 97)
    {
        z[8] = (uint8_t)(v >> 8);
        z[9] = (uint8_t)v;
        const uint32_t b = backoffS(0, z);
        TEST_ASSERT_TRUE(b >= 810 && b <= 990);
    }
    // In the window: failures retried after the backoff, the last one past
    // the window end gives the night up
    useTz(TZ_PRAGUE);
    Settings s; // 02-05, due 02:00
    Sched st;
    uint32_t runs[16];
    const int n = simulate(local(2026, 6, 10, 1, 0), 6 * 3600, 10, s, 0, st, runs, 16, dg, Outcome::Failed);
    // 02:00, +15 min, +30 min, +1 h (03:45), +2 h would be 05:45: given up
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_UINT32(local(2026, 6, 10, 2, 0), runs[0]);
    TEST_ASSERT_UINT32_WITHIN(91, 900, runs[1] - runs[0]);
    TEST_ASSERT_UINT32_WITHIN(181, 1800, runs[2] - runs[1]);
    TEST_ASSERT_UINT32_WITHIN(361, 3600, runs[3] - runs[2]);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 6, 10), st.doneDay);
    // The next day starts with the first step again
    TEST_ASSERT_EQUAL(4, simulate(local(2026, 6, 11, 1, 0), 6 * 3600, 10, s, 0, st, runs, 16, dg, Outcome::Failed));
    TEST_ASSERT_UINT32_WITHIN(91, 900, runs[1] - runs[0]);
    // Deferred: tried every 5 min while the window lasts
    Sched sd;
    const int k = simulate(local(2026, 6, 12, 1, 0), 6 * 3600, 10, s, 0, sd, runs, 16, dg, Outcome::Deferred);
    TEST_ASSERT_EQUAL(36, k);
    TEST_ASSERT_EQUAL_UINT32(DEFER_RETRY_S, runs[1] - runs[0]);
}

void test_next_run()
{
    useTz(TZ_PRAGUE);
    Settings s;
    Sched st;
    Clock c = at(local(2026, 6, 10, 12, 0));
    Next n = next(st, s, 3600, c);
    TEST_ASSERT_FALSE(n.retry);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 6, 11), n.day);
    TEST_ASSERT_EQUAL_UINT32(3 * 3600, n.sec);
    c = at(local(2026, 6, 11, 1, 0));
    n = next(st, s, 3600, c);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 6, 11), n.day);
    uint8_t dg[32] = {};
    c = at(local(2026, 6, 11, 3, 0));
    TEST_ASSERT_TRUE(due(st, s, 3600, c).due);
    finish(st, s, c, c.day, Outcome::Failed, dg);
    n = next(st, s, 3600, c);
    TEST_ASSERT_TRUE(n.retry);
    TEST_ASSERT_UINT32_WITHIN(91, local(2026, 6, 11, 3, 15), n.at);
    finish(st, s, c, c.day, Outcome::Ok, dg);
    n = next(st, s, 3600, c);
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 6, 12), n.day);
}

void test_sensor_job_fits_the_window_rule()
{
    useTz(TZ_PRAGUE);
    Settings s; // 02-05
    Sched st;
    // 12:00, estimate 3 h 25 min: fits (14 h to the window)
    TEST_ASSERT_TRUE(sensorJobFits(st, s, at(local(2026, 6, 10, 12, 0)), 12300));
    // 22:30: 3 h 30 min left, 3 h 25 min + 30 min margin does not fit
    TEST_ASSERT_FALSE(sensorJobFits(st, s, at(local(2026, 6, 10, 22, 30)), 12300));
    TEST_ASSERT_TRUE(sensorJobFits(st, s, at(local(2026, 6, 10, 22, 0)), 12300));
    // Inside the window before its run: never
    TEST_ASSERT_FALSE(sensorJobFits(st, s, at(local(2026, 6, 11, 3, 0)), 60));
    // Inside it after the run: the next window is tomorrow
    st.doneDay = daysFromCivil(2026, 6, 11);
    TEST_ASSERT_TRUE(sensorJobFits(st, s, at(local(2026, 6, 11, 3, 0)), 12300));
}

void test_session_eligibility_matrix()
{
    using S = OtaJob::State;
    const S states[] = {S::None,     S::Waiting, S::Transfer, S::Paused,  S::Verified, S::Swapping,
                        S::Confirming, S::Done,  S::Failed,   S::Aborted, S::Reverted};
    for (S s : states)
    {
        Activity a;
        a.job = s;
        const bool allowed = OtaJob::terminal(s) || s == S::Paused;
        TEST_ASSERT_EQUAL(allowed ? (int)Defer::None : (int)Defer::SensorJob, (int)sessionDefer(a));
        // Every other activity defers, whatever the job
        Activity b = a;
        b.startPending = true;
        TEST_ASSERT_NOT_EQUAL((int)Defer::None, (int)sessionDefer(b));
        b = a;
        b.upload = true;
        TEST_ASSERT_NOT_EQUAL((int)Defer::None, (int)sessionDefer(b));
        b = a;
        b.download = true;
        TEST_ASSERT_NOT_EQUAL((int)Defer::None, (int)sessionDefer(b));
        b = a;
        b.validation = true;
        TEST_ASSERT_EQUAL((int)Defer::Validation, (int)sessionDefer(b));
        b = a;
        b.restart = true;
        TEST_ASSERT_EQUAL((int)Defer::Restart, (int)sessionDefer(b));
        b = a;
        b.session = true;
        TEST_ASSERT_EQUAL((int)Defer::Session, (int)sessionDefer(b));
    }
    Activity q;
    q.startPending = true;
    TEST_ASSERT_EQUAL((int)Defer::StartQueued, (int)sessionDefer(q));
    // An admin's gateway image waiting for its confirmation (the install
    // would drop it): deferred, whatever the job
    for (S s : states)
    {
        Activity c;
        c.job = s;
        c.confirmWait = true;
        TEST_ASSERT_NOT_EQUAL((int)Defer::None, (int)sessionDefer(c));
    }
    Activity cw;
    cw.confirmWait = true;
    TEST_ASSERT_EQUAL((int)Defer::Confirm, (int)sessionDefer(cw));
    for (int d = 0; d <= (int)Defer::Session; d++)
    {
        TEST_ASSERT_TRUE(d == 0 || strlen(deferText((Defer)d)) > 0);
    }
}

void test_three_strikes_and_starvation()
{
    uint32_t ver = 0;
    uint8_t fails = 0;
    const uint32_t v201 = 0x020001, v202 = 0x020002;
    TEST_ASSERT_TRUE(gatewayPending(true, true, v201, ver, fails));
    for (int i = 0; i < 3; i++)
    {
        TEST_ASSERT_FALSE(gatewayStruck(ver, fails, v201));
        gatewayAttempt(ver, fails, v201);
    }
    TEST_ASSERT_TRUE(gatewayStruck(ver, fails, v201));
    TEST_ASSERT_FALSE(gatewayPending(true, true, v201, ver, fails)); // given up: sensors may run again
    // A newer version starts again
    TEST_ASSERT_FALSE(gatewayStruck(ver, fails, v202));
    TEST_ASSERT_TRUE(gatewayPending(true, true, v202, ver, fails));
    gatewayAttempt(ver, fails, v202);
    TEST_ASSERT_EQUAL_UINT32(v202, ver);
    TEST_ASSERT_EQUAL(1, fails);
    // Not pending: switched off, or nothing newer offered
    TEST_ASSERT_FALSE(gatewayPending(false, true, v202, ver, fails));
    TEST_ASSERT_FALSE(gatewayPending(true, false, v202, ver, fails));
    // Saturates
    for (int i = 0; i < 300; i++)
    {
        gatewayAttempt(ver, fails, v202);
    }
    TEST_ASSERT_EQUAL(255, fails);
}

void test_sensor_skip_rules()
{
    SensorInfo s;
    s.devId = 0x4776B882;
    s.hasFw = true;
    s.hw = 3;
    s.fw = 0x00020C; // 0.2.12
    ImageInfo img;
    img.stored = true;
    img.fw = 0x00020D; // 0.2.13
    img.verified = true;
    TEST_ASSERT_EQUAL((int)Skip::None, (int)sensorSkip(s, img));
    SensorInfo n = s;
    n.hasFw = false;
    TEST_ASSERT_EQUAL((int)Skip::NoFw, (int)sensorSkip(n, img));
    ImageInfo none;
    TEST_ASSERT_EQUAL((int)Skip::NoImage, (int)sensorSkip(s, none));
    ImageInfo u = img;
    u.verified = false;
    TEST_ASSERT_EQUAL((int)Skip::Unsigned, (int)sensorSkip(s, u));
    ImageInfo same = img;
    same.fw = s.fw;
    TEST_ASSERT_EQUAL((int)Skip::NotNewer, (int)sensorSkip(s, same));
    same.fw = s.fw - 1;
    TEST_ASSERT_EQUAL((int)Skip::NotNewer, (int)sensorSkip(s, same));
    // Reverted by version: also a rebuild (another CRC) of that version
    SensorInfo r = s;
    r.hasReverted = true;
    r.revertedFw = img.fw;
    TEST_ASSERT_EQUAL((int)Skip::Reverted, (int)sensorSkip(r, img));
    r.revertedFw = 0x00020B;
    TEST_ASSERT_EQUAL((int)Skip::None, (int)sensorSkip(r, img));
    // Two automatic failures of this version
    SensorInfo f = s;
    recordFailure(f.failFw, f.fails, img.fw);
    TEST_ASSERT_EQUAL((int)Skip::None, (int)sensorSkip(f, img));
    recordFailure(f.failFw, f.fails, img.fw);
    TEST_ASSERT_EQUAL((int)Skip::Failed, (int)sensorSkip(f, img));
    // ... a newer image counts again from 0
    ImageInfo newer = img;
    newer.fw = 0x00020E;
    TEST_ASSERT_EQUAL((int)Skip::None, (int)sensorSkip(f, newer));
    recordFailure(f.failFw, f.fails, newer.fw);
    TEST_ASSERT_EQUAL(1, f.fails);
    // Outcomes: Failed (any reason, also no progress) and an abort by the
    // gateway count; an abort by the admin does not
    using W = OtaJob::Why;
    TEST_ASSERT_TRUE(countsAsFailure(OtaJob::State::Failed, W::NoProgress));
    TEST_ASSERT_TRUE(countsAsFailure(OtaJob::State::Failed, W::SensorFailed));
    TEST_ASSERT_TRUE(countsAsFailure(OtaJob::State::Aborted, W::None));
    TEST_ASSERT_FALSE(countsAsFailure(OtaJob::State::Aborted, W::Admin));
    TEST_ASSERT_FALSE(countsAsFailure(OtaJob::State::Done, W::None));
    TEST_ASSERT_FALSE(countsAsFailure(OtaJob::State::Reverted, W::None));
    TEST_ASSERT_FALSE(countsAsFailure(OtaJob::State::Paused, W::ManualPause));
}

// H1 / M1: strikes only for an attempt that reached the activation (or a
// refused image), at most one per night; cleared only once the new
// firmware is validated; a rollback keeps it; three stop the version; a
// good version then installs and only its validation clears the count
void test_gateway_strikes_and_rollback()
{
    const uint32_t v200 = 0x020000, bad = 0x020005, good = 0x020006;
    uint32_t ver = 0;
    uint8_t fails = 0;
    int32_t day = 0;
    bool act = false;
    for (int32_t night = 100; night < 106; night++)
    {
        if (gatewayStruck(ver, fails, bad))
        {
            TEST_ASSERT_EQUAL(103, night); // given up after three nights
            break;
        }
        TEST_ASSERT_FALSE(gatewayTriedTonight(ver, day, bad, night));
        // The attempt reaches the activation: counted and saved first
        gatewayAttempt(ver, fails, bad);
        day = night;
        act = true;
        TEST_ASSERT_TRUE(gatewayTriedTonight(ver, day, bad, night)); // not again tonight
        // The new firmware starts: nothing decided while it waits for its
        // validation (the old code cleared the count here)
        TEST_ASSERT_EQUAL((int)GwBoot::Validating, (int)gatewayBoot(act, fails, ver, bad, true));
        // ... it crashes before the validation: the old firmware runs again
        TEST_ASSERT_EQUAL((int)GwBoot::RolledBack, (int)gatewayBoot(act, fails, ver, v200, false));
        act = false;
        TEST_ASSERT_EQUAL(night - 99, fails);
        TEST_ASSERT_EQUAL((int)GwBoot::None, (int)gatewayBoot(act, fails, ver, v200, false));
    }
    TEST_ASSERT_TRUE(gatewayStruck(ver, fails, bad));
    TEST_ASSERT_FALSE(gatewayPending(true, true, bad, ver, fails));
    // A good version: attempt 1, validated -> cleared only then
    TEST_ASSERT_FALSE(gatewayStruck(ver, fails, good));
    TEST_ASSERT_TRUE(gatewayPending(true, true, good, ver, fails));
    gatewayAttempt(ver, fails, good);
    act = true;
    TEST_ASSERT_EQUAL(1, fails);
    TEST_ASSERT_EQUAL((int)GwBoot::Validating, (int)gatewayBoot(act, fails, ver, good, true));
    TEST_ASSERT_EQUAL((int)GwBoot::Installed, (int)gatewayBoot(act, fails, ver, good, false));
    // A stored day after today is forgotten (L7: also exactly tomorrow)
    TEST_ASSERT_TRUE(futureDay(101, 100));
    TEST_ASSERT_FALSE(futureDay(100, 100));
    TEST_ASSERT_FALSE(futureDay(99, 100));
    // Download failures: no strike (only the RAM release of the starvation)
    TEST_ASSERT_TRUE(gatewayPending(true, true, 0x020007, ver, fails, false));
    TEST_ASSERT_FALSE(gatewayPending(true, true, 0x020007, ver, fails, true));
}

// L2: the retry wait is persisted before each run, so a restart inside the
// window (also a crash in the check) waits for it instead of running again
void test_persisted_retry()
{
    useTz(TZ_PRAGUE);
    uint8_t dg[32] = {};
    Settings s; // 02-05
    Sched st;
    const Clock c = at(local(2026, 6, 10, 2, 30));
    const Due d = due(st, s, 1800, c);
    TEST_ASSERT_TRUE(d.due);
    const Sched pre = ifFailed(st, s, c, d.day, dg); // what is saved before the run
    TEST_ASSERT_EQUAL_INT32(d.day, pre.retryDay);
    TEST_ASSERT_EQUAL(1, pre.step);
    TEST_ASSERT_UINT32_WITHIN(91, c.unixS + 900, pre.retryAt);
    // The run crashes the gateway: the next start restores it
    Sched boot;
    restoreRetry(boot, pre.retryDay, pre.step, pre.retryAt);
    TEST_ASSERT_FALSE(due(boot, s, 1800, at(local(2026, 6, 10, 2, 31))).due);
    TEST_ASSERT_FALSE(due(boot, s, 1800, at(pre.retryAt - 1)).due);
    TEST_ASSERT_TRUE(due(boot, s, 1800, at(pre.retryAt)).due);
    // ... and the next failure doubles the wait
    const Sched pre2 = ifFailed(boot, s, at(pre.retryAt), d.day, dg);
    TEST_ASSERT_EQUAL(2, pre2.step);
    TEST_ASSERT_UINT32_WITHIN(181, pre.retryAt + 1800, pre2.retryAt);
    // A restart loop through the whole window: at most 4 runs (02:30, then
    // 15 / 30 / 60 min later; the next wait passes the end)
    Sched loop;
    int runs = 0;
    for (uint32_t t = local(2026, 6, 10, 1, 0); t < local(2026, 6, 10, 6, 0); t += 30)
    {
        Sched b; // every pass is a fresh start with the saved record
        b.doneDay = loop.doneDay;
        restoreRetry(b, loop.retryDay, loop.step, loop.retryAt);
        const Due dd = due(b, s, 1800, at(t));
        if (dd.due)
        {
            runs++;
            loop = ifFailed(b, s, at(t), dd.day, dg);
        }
    }
    TEST_ASSERT_TRUE(runs >= 3 && runs <= 4);
    // A new night starts again with the first step
    Sched n2;
    restoreRetry(n2, loop.retryDay, loop.step, loop.retryAt);
    TEST_ASSERT_TRUE(due(n2, s, 1800, at(local(2026, 6, 11, 2, 30))).due);
}

// L6: a run deferred by a sensor job may still start up to 1 h after the
// window end; any other deferral gives the night up at the end
void test_job_deferral_grace()
{
    useTz(TZ_PRAGUE);
    uint8_t dg[32] = {};
    Settings s; // 02-05
    Sched st;
    int32_t day = 0;
    for (uint32_t t = local(2026, 6, 10, 2, 0); t < local(2026, 6, 10, 5, 30); t += 30)
    {
        const Clock c = at(t);
        const Due d = due(st, s, 0, c);
        if (d.due)
        {
            day = d.day;
            TEST_ASSERT_FALSE(finish(st, s, c, d.day, Outcome::Deferred, dg, true));
        }
    }
    TEST_ASSERT_TRUE(st.doneDay < day);
    // 05:30: the job ended - the run starts
    const Clock late = at(local(2026, 6, 10, 5, 31));
    Due d = due(st, s, 0, late);
    TEST_ASSERT_TRUE(d.due);
    TEST_ASSERT_TRUE(finish(st, s, late, d.day, Outcome::Ok, dg));
    // Still blocked at 06:00 (the grace is over): given up
    Sched g;
    bool done = false;
    for (uint32_t t = local(2026, 6, 11, 2, 0); t < local(2026, 6, 11, 7, 0) && !done; t += 30)
    {
        const Clock c = at(t);
        const Due dd = due(g, s, 0, c);
        if (dd.due)
        {
            done = finish(g, s, c, dd.day, Outcome::Deferred, dg, true);
            if (done)
            {
                TEST_ASSERT_TRUE(t >= local(2026, 6, 11, 5, 50) && t < local(2026, 6, 11, 6, 0));
            }
        }
    }
    TEST_ASSERT_TRUE(done);
    // Other deferrals: no grace
    Sched o;
    for (uint32_t t = local(2026, 6, 12, 2, 0); t < local(2026, 6, 12, 6, 0); t += 30)
    {
        const Clock c = at(t);
        const Due dd = due(o, s, 0, c);
        if (dd.due)
        {
            finish(o, s, c, dd.day, Outcome::Deferred, dg, false);
        }
    }
    TEST_ASSERT_EQUAL_INT32(daysFromCivil(2026, 6, 12), o.doneDay);
    TEST_ASSERT_FALSE(due(o, s, 0, at(local(2026, 6, 12, 5, 30))).due);
}

// L12: a forward SNTP step of 10 min - 2 h (wall and unix time step alike)
// never counts as the DST jump - only a change of the UTC offset does
void test_sntp_step_is_not_a_dst_jump()
{
    useTz(TZ_PRAGUE);
    uint8_t dg[32] = {};
    Settings s;
    s.endH = 3; // 02-03
    const uint32_t steps[] = {600, 601, 1800, 3600, 7199, 7200};
    for (uint32_t step : steps)
    {
        // 02:55 (the run of the day is done)... the clock jumps over 03:00
        // after the window: no run for a window the step jumped over
        Sched st;
        st.doneDay = daysFromCivil(2026, 6, 9);
        const uint32_t t0 = local(2026, 6, 10, 1, 59, 0);
        TEST_ASSERT_FALSE(due(st, s, 1800, at(t0)).due); // before the due point 02:30
        const uint32_t t1 = t0 + 1 + step + 3600;          // wall and unix past 03:00 alike
        TEST_ASSERT_FALSE(due(st, s, 1800, at(t1)).due);
    }
    // The real DST jump (same real second, wall +1 h) still runs
    Sched sd;
    s.endH = 3;
    TEST_ASSERT_FALSE(due(sd, s, 1200, at(local(2026, 3, 29, 1, 59, 59))).due);
    TEST_ASSERT_TRUE(due(sd, s, 1200, at(local(2026, 3, 29, 1, 59, 59) + 1)).due);
}

// L3: a sensor image version whose nightly fetch failed on three nights is
// not fetched again; one strike per night; success / another version clear
void test_fetch_strikes()
{
    FetchStrike f[FETCH_SLOTS];
    const uint16_t hw = 3;
    const uint32_t v = 0x00020D;
    TEST_ASSERT_FALSE(fetchStruck(f, hw, v));
    TEST_ASSERT_TRUE(fetchResult(f, hw, v, false, 100));
    TEST_ASSERT_FALSE(fetchResult(f, hw, v, false, 100)); // same night: no second strike
    TEST_ASSERT_TRUE(fetchResult(f, hw, v, false, 101));
    TEST_ASSERT_FALSE(fetchStruck(f, hw, v));
    TEST_ASSERT_TRUE(fetchResult(f, hw, v, false, 102));
    TEST_ASSERT_TRUE(fetchStruck(f, hw, v));
    TEST_ASSERT_FALSE(fetchStruck(f, 4, v));
    // Another version for the HW type starts again
    TEST_ASSERT_FALSE(fetchStruck(f, hw, v + 1));
    TEST_ASSERT_TRUE(fetchResult(f, hw, v + 1, false, 103));
    TEST_ASSERT_FALSE(fetchStruck(f, hw, v));
    TEST_ASSERT_EQUAL(1, f[0].n);
    // Success clears it; a success without a record changes nothing
    TEST_ASSERT_TRUE(fetchResult(f, hw, v + 1, true, 104));
    TEST_ASSERT_FALSE(fetchResult(f, hw, v + 1, true, 104));
    // Nine HW types in eight slots: the oldest record makes room
    for (uint16_t h = 1; h <= 9; h++)
    {
        fetchResult(f, h, v, false, 200 + h);
    }
    int used = 0;
    bool first = false;
    for (const FetchStrike &r : f)
    {
        used += r.n != 0;
        first = first || (r.n && r.hw == 1);
    }
    TEST_ASSERT_EQUAL(8, used);
    TEST_ASSERT_FALSE(first);
    // Persisted (state.json "sf") with the retry wait, "gwDay", "gwAct", "pendGw"
    UpdateState::State st, r;
    st.au.sf[2] = f[2];
    st.au.tryDay = 20386;
    st.au.tryStep = 2;
    st.au.retryAt = 1761201500;
    st.au.gwDay = 20385;
    st.au.gwAct = true;
    st.au.pendGw = 0x020006;
    char buf[UpdateState::TEXT_CAPACITY];
    const size_t n = UpdateState::format(st, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(UpdateState::parse(buf, n, r));
    TEST_ASSERT_EQUAL_UINT16(f[2].hw, r.au.sf[0].hw);
    TEST_ASSERT_EQUAL_UINT32(f[2].ver, r.au.sf[0].ver);
    TEST_ASSERT_EQUAL(f[2].n, r.au.sf[0].n);
    TEST_ASSERT_EQUAL_INT32(f[2].day, r.au.sf[0].day);
    TEST_ASSERT_EQUAL(0, r.au.sf[1].n);
    TEST_ASSERT_EQUAL_INT32(20386, r.au.tryDay);
    TEST_ASSERT_EQUAL(2, r.au.tryStep);
    TEST_ASSERT_EQUAL_UINT32(1761201500u, r.au.retryAt);
    TEST_ASSERT_EQUAL_INT32(20385, r.au.gwDay);
    TEST_ASSERT_TRUE(r.au.gwAct);
    TEST_ASSERT_EQUAL_HEX32(0x020006, r.au.pendGw);
    // A malformed "sf" reads as nothing
    const char *bad = "{\"v\":1,\"auto\":{\"day\":1,\"sf\":[3,4,5],\"tryDay\":\"x\"}}";
    TEST_ASSERT_TRUE(UpdateState::parse(bad, strlen(bad), r));
    TEST_ASSERT_EQUAL(0, r.au.sf[0].n);
    TEST_ASSERT_EQUAL_INT32(0, r.au.tryDay);
}

// S7 leftovers: the persisted pending version is bounded (rollback case,
// PEND_HOLD_DAYS), HTTP 404 / 410 refuse, an interrupted download is a strike
// (one per night), sensor fetch strikes only for refused images
void test_s7_pending_http_marker_fetch()
{
    const uint32_t v = 0x020100;
    // pendGw honoured only when it was attempted, not given up, recent
    TEST_ASSERT_FALSE(persistedPending(v, 0, 0, 0, 100));             // offered, never attempted
    TEST_ASSERT_FALSE(persistedPending(v, 0x020000, 1, 100, 100));     // strikes of another version
    TEST_ASSERT_TRUE(persistedPending(v, v, 1, 100, 100));             // rolled back tonight
    TEST_ASSERT_TRUE(persistedPending(v, v, 2, 100, 100 + PEND_HOLD_DAYS));
    TEST_ASSERT_FALSE(persistedPending(v, v, 2, 100, 101 + PEND_HOLD_DAYS)); // the manifest kept failing
    TEST_ASSERT_FALSE(persistedPending(v, v, GATEWAY_TRIES, 100, 100)); // given up
    TEST_ASSERT_FALSE(persistedPending(0, v, 1, 100, 100));
    // A version released in this run (refused image / HTTP 404, download
    // failures) is not persisted as pending: after a restart its refusal
    // strike does not hold the sensors back
    TEST_ASSERT_EQUAL_HEX32(v, pendingToSave(v, false));
    TEST_ASSERT_EQUAL_HEX32(0, pendingToSave(v, true));
    TEST_ASSERT_FALSE(persistedPending(pendingToSave(v, true), v, 1, 100, 100));
    // HTTP answers: only "not there" is a refusal; a CDN challenge (403),
    // 401 and every other 4xx are transient like a network failure
    TEST_ASSERT_TRUE(refusedHttp(404));
    TEST_ASSERT_TRUE(refusedHttp(410));
    TEST_ASSERT_FALSE(refusedHttp(403));
    TEST_ASSERT_FALSE(refusedHttp(401));
    TEST_ASSERT_FALSE(refusedHttp(400));
    TEST_ASSERT_FALSE(refusedHttp(451));
    TEST_ASSERT_FALSE(refusedHttp(408));
    TEST_ASSERT_FALSE(refusedHttp(429));
    TEST_ASSERT_FALSE(refusedHttp(500));
    TEST_ASSERT_FALSE(refusedHttp(503));
    TEST_ASSERT_FALSE(refusedHttp(200));
    TEST_ASSERT_FALSE(refusedHttp(-1)); // connection refused / lost (HTTPClient)
    // A download still marked at the start: one strike per night
    uint32_t ver = 0;
    uint8_t fails = 0;
    int32_t day = 0;
    TEST_ASSERT_FALSE(downloadInterrupted(ver, fails, day, 0, 100)); // no marker
    TEST_ASSERT_TRUE(downloadInterrupted(ver, fails, day, v, 100));
    TEST_ASSERT_EQUAL(1, fails);
    TEST_ASSERT_TRUE(gatewayTriedTonight(ver, day, v, 100)); // not again tonight
    TEST_ASSERT_FALSE(downloadInterrupted(ver, fails, day, v, 100)); // second crash the same night
    TEST_ASSERT_EQUAL(1, fails);
    TEST_ASSERT_TRUE(downloadInterrupted(ver, fails, day, v, 101));
    TEST_ASSERT_TRUE(downloadInterrupted(ver, fails, day, v, 102));
    TEST_ASSERT_TRUE(gatewayStruck(ver, fails, v)); // three nights: given up
    TEST_ASSERT_FALSE(gatewayPending(true, true, v, ver, fails));
    // Sensor fetches: a transport failure is no strike, a refused one is
    FetchStrike f[FETCH_SLOTS];
    const uint16_t hw = 3;
    const uint32_t sv = 0x00020D;
    for (int32_t night = 100; night < 110; night++)
    {
        TEST_ASSERT_FALSE(fetchEnded(f, hw, sv, FetchEnd::Transport, night));
    }
    TEST_ASSERT_FALSE(fetchStruck(f, hw, sv));
    TEST_ASSERT_TRUE(fetchEnded(f, hw, sv, FetchEnd::Refused, 110));
    TEST_ASSERT_FALSE(fetchEnded(f, hw, sv, FetchEnd::Transport, 111));
    TEST_ASSERT_TRUE(fetchEnded(f, hw, sv, FetchEnd::Refused, 112));
    TEST_ASSERT_TRUE(fetchEnded(f, hw, sv, FetchEnd::Refused, 113));
    TEST_ASSERT_TRUE(fetchStruck(f, hw, sv));
    TEST_ASSERT_TRUE(fetchEnded(f, hw, sv, FetchEnd::Stored, 114)); // a manual / later success clears it
    TEST_ASSERT_FALSE(fetchStruck(f, hw, sv));
}

void test_round_robin()
{
    const uint32_t ids[4] = {0xE686F9A7, 0x4776B882, 0x10000000, 0x90000000};
    uint8_t order[4];
    // Cursor 0: ascending
    TEST_ASSERT_EQUAL(4, roundRobin(ids, 4, 0, order));
    TEST_ASSERT_EQUAL(2, order[0]);
    TEST_ASSERT_EQUAL(1, order[1]);
    TEST_ASSERT_EQUAL(3, order[2]);
    TEST_ASSERT_EQUAL(0, order[3]);
    // After 0x4776B882: 0x90000000, 0xE686F9A7, then wraps (0x10000000, 0x4776B882 last)
    TEST_ASSERT_EQUAL(4, roundRobin(ids, 4, 0x4776B882, order));
    TEST_ASSERT_EQUAL(3, order[0]);
    TEST_ASSERT_EQUAL(0, order[1]);
    TEST_ASSERT_EQUAL(2, order[2]);
    TEST_ASSERT_EQUAL(1, order[3]);
    // A cursor that is gone (sensor deleted) still continues above it
    TEST_ASSERT_EQUAL(4, roundRobin(ids, 4, 0x50000000, order));
    TEST_ASSERT_EQUAL(3, order[0]);
    // A full cycle with the cursor persisted: every sensor once
    uint32_t cursor = 0;
    bool got[4] = {};
    for (int i = 0; i < 4; i++)
    {
        roundRobin(ids, 4, cursor, order);
        got[order[0]] = true;
        cursor = ids[order[0]];
    }
    TEST_ASSERT_TRUE(got[0] && got[1] && got[2] && got[3]);
    TEST_ASSERT_EQUAL(0, roundRobin(ids, 0, 0, order));
}

// /update/state.json "auto": round trip; a state without it keeps its exact
// layout (written only when set); an S5 file reads with nothing known
void test_state_auto_fields()
{
    UpdateState::State s, r;
    char buf[UpdateState::TEXT_CAPACITY];
    s.seq = 12;
    size_t n = UpdateState::format(s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"seq\":12}", buf);
    s.au.day = 20386;
    s.au.cursor = 0x4776B882;
    s.au.gwVer = 0x020001;
    s.au.gwFails = 2;
    s.au.job = 0xE686F9A7;
    s.au.jobFw = 0x00020C;
    UpdateState::copyText(s.au.runText, sizeof(s.au.runText), "ok: up to date");
    s.au.runAt = 1761200600;
    n = UpdateState::format(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(UpdateState::parse(buf, n, r));
    TEST_ASSERT_EQUAL_INT32(20386, r.au.day);
    TEST_ASSERT_EQUAL_HEX32(0x4776B882, r.au.cursor);
    TEST_ASSERT_EQUAL_HEX32(0x020001, r.au.gwVer);
    TEST_ASSERT_EQUAL(2, r.au.gwFails);
    TEST_ASSERT_EQUAL_HEX32(0xE686F9A7, r.au.job);
    TEST_ASSERT_EQUAL_HEX32(0x00020C, r.au.jobFw);
    TEST_ASSERT_EQUAL_STRING("ok: up to date", r.au.runText);
    TEST_ASSERT_EQUAL_UINT32(1761200600u, r.au.runAt);
    TEST_ASSERT_EQUAL_UINT32(12, r.seq);
    TEST_ASSERT_EQUAL_UINT32(0, r.au.gwDl);
    TEST_ASSERT_TRUE(strstr(buf, "gwDl") == nullptr && strstr(buf, "grace") == nullptr); // written only when set
    // S7: the download marker and the grace day round-trip
    s.au.gwDl = 0x020100;
    s.au.gwDlDay = 20390;
    s.au.graceDay = 20391;
    n = UpdateState::format(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(UpdateState::parse(buf, n, r));
    TEST_ASSERT_EQUAL_HEX32(0x020100, r.au.gwDl);
    TEST_ASSERT_EQUAL_INT32(20390, r.au.gwDlDay);
    TEST_ASSERT_EQUAL_INT32(20391, r.au.graceDay);
    s.au.gwDl = 0;
    s.au.gwDlDay = s.au.graceDay = 0;
    n = UpdateState::format(s, buf, sizeof(buf));
    // The S5 file (no "auto")
    const char *s5 = "{\"v\":1,\"seq\":14,\"check\":{\"text\":\"ok\",\"at\":1}}";
    TEST_ASSERT_TRUE(UpdateState::parse(s5, strlen(s5), r));
    TEST_ASSERT_EQUAL_INT32(0, r.au.day);
    TEST_ASSERT_EQUAL(0, r.au.gwFails);
    TEST_ASSERT_EQUAL_UINT32(14, r.seq);
    // The seq salvage does not take an "auto" number
    bool found = false;
    TEST_ASSERT_EQUAL_UINT32(12, UpdateState::salvageSeq(buf, n - 5, found));
    TEST_ASSERT_TRUE(found);
}

// sensors.json "autoFail": nothing written without failures (the gen 2
// entry - and so the gen-1-only file - stays byte-identical), round trip,
// tolerant read
void test_sensors_json_auto_fail()
{
    auto entry = [](bool callWriter, uint32_t fw, uint8_t n) {
        DynamicJsonDocument doc(1024);
        JsonArray g = doc.createNestedArray("gen2");
        JsonObject o = g.createNestedObject();
        o["devId"] = "4776B882";
        o["fw"] = "0.2.12";
        o["revFw"] = 525u;
        if (callWriter)
        {
            writeAutoFail(o, fw, n);
        }
        std::string out;
        serializeJson(doc, out);
        return out;
    };
    TEST_ASSERT_EQUAL_STRING(entry(false, 0, 0).c_str(), entry(true, 0x00020D, 0).c_str());
    const std::string with = entry(true, 0x00020D, 2);
    TEST_ASSERT_EQUAL_STRING("{\"gen2\":[{\"devId\":\"4776B882\",\"fw\":\"0.2.12\",\"revFw\":525,\"autoFail\":[525,2]}]}",
                             with.c_str());
    DynamicJsonDocument doc(1024);
    TEST_ASSERT_TRUE(deserializeJson(doc, with) == DeserializationError::Ok);
    uint32_t fw = 1;
    uint8_t n = 1;
    readAutoFail(doc["gen2"][0].as<JsonObjectConst>(), fw, n);
    TEST_ASSERT_EQUAL_HEX32(0x00020D, fw);
    TEST_ASSERT_EQUAL(2, n);
    const char *bad[] = {"{}", "{\"autoFail\":5}", "{\"autoFail\":[1]}", "{\"autoFail\":[1,2,3]}",
                         "{\"autoFail\":[-1,2]}", "{\"autoFail\":[1,300]}", "{\"autoFail\":[\"1\",2]}"};
    for (const char *b : bad)
    {
        DynamicJsonDocument d(256);
        TEST_ASSERT_TRUE(deserializeJson(d, b) == DeserializationError::Ok);
        readAutoFail(d.as<JsonObjectConst>(), fw, n);
        TEST_ASSERT_EQUAL_MESSAGE(0, n, b);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(0, fw, b);
    }
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_settings_pack_and_validation);
    RUN_TEST(test_offset_table);
    RUN_TEST(test_civil_days);
    RUN_TEST(test_one_run_per_day_normal_days);
    RUN_TEST(test_dst_spring);
    RUN_TEST(test_dst_autumn);
    RUN_TEST(test_backoff_sequence_jitter_and_window_end);
    RUN_TEST(test_next_run);
    RUN_TEST(test_sensor_job_fits_the_window_rule);
    RUN_TEST(test_session_eligibility_matrix);
    RUN_TEST(test_three_strikes_and_starvation);
    RUN_TEST(test_sensor_skip_rules);
    RUN_TEST(test_round_robin);
    RUN_TEST(test_gateway_strikes_and_rollback);
    RUN_TEST(test_persisted_retry);
    RUN_TEST(test_job_deferral_grace);
    RUN_TEST(test_sntp_step_is_not_a_dst_jump);
    RUN_TEST(test_fetch_strikes);
    RUN_TEST(test_s7_pending_http_marker_fetch);
    RUN_TEST(test_state_auto_fields);
    RUN_TEST(test_sensors_json_auto_fail);
    return UNITY_END();
}
