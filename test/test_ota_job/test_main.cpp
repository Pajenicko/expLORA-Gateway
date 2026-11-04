/**
 * Native tests for the gateway's OTA job (src/Protocol/OtaJob.h,
 * docs/protocol-v2.md 14.5): every cell of the state x event matrix
 * (table-driven), then the rules around it - no progress, laps, starved
 * back-off, battery retry, abort delivery, orphans, deadlines across a
 * reboot, the revert and reinstall rules, block choice and anchoring.
 * Run:   pio test -e native -f test_ota_job
 */

#include <unity.h>
#include <cstdio>
#include <initializer_list>
#include "../../src/Protocol/OtaJob.h"

using namespace OtaJob;

void setUp(void) {}
void tearDown(void) {}

static const uint32_t OLD_FW = 0x000200; // 0.2.0
static const uint32_t NEW_FW = 0x000300; // 0.3.0
static const uint32_t LEN = 92048;       // 397 blocks

static Now at(uint32_t s)
{
    Now n;
    n.unixS = 1800000000u + s;
    n.uptimeS = 1000 + s;
    n.us = (int64_t)(1000 + s) * 1000000;
    return n;
}

static Target target()
{
    Target t;
    t.devId = 0x1234;
    t.hwType = 3;
    t.fw = OLD_FW;
    t.boot = 2;
    t.flags = 0;
    t.intervalS = 600;
    return t;
}

static Image image(uint32_t fw = NEW_FW)
{
    Image i;
    i.fw = fw;
    i.crc = 0xABCD1234;
    i.len = LEN;
    return i;
}

// A normal UP of the target (session up, configuration in sync)
static Up up(const Now &n, bool bit4)
{
    Up u;
    u.bit4 = bit4;
    u.fcnt32 = 100;
    u.rxUs = n.us;
    u.timeReliable = true;
    u.airUs = 280000;
    u.curFw = OLD_FW;
    u.canStart = true;
    return u;
}

static Up accepted(const Now &n, uint8_t arg = 0, uint16_t base = 0, uint64_t window = 0)
{
    Up u = up(n, true);
    u.accepted = true;
    u.acceptedArg = arg;
    u.progress = true;
    u.base = base;
    u.window = window;
    u.slot = 0xFFFF;
    return u;
}

static Up progress(const Now &n, uint16_t base, uint64_t window, uint16_t slot)
{
    Up u = up(n, true);
    u.progress = true;
    u.base = base;
    u.window = window;
    u.slot = slot;
    return u;
}

static Up status(const Now &n, uint32_t fw, uint8_t boot, uint8_t flags, bool bit4 = false)
{
    Up u = up(n, bit4);
    u.hasStatus = true;
    u.hasFw = true;
    u.fw = fw;
    u.curFw = fw;
    u.boot = boot;
    u.flags = flags;
    return u;
}

static Up joinReq(const Now &n, uint32_t fw)
{
    Up u;
    u.joinReq = true;
    u.hasFw = true;
    u.fw = fw;
    u.curFw = fw;
    u.rxUs = n.us;
    return u;
}

static Job inState(State s)
{
    Job j;
    Now n = at(0);
    begin(j, target(), image(), n);
    switch (s)
    {
    case State::Waiting:
        break;
    case State::Transfer:
        onUp(j, accepted(at(10)), at(10));
        break;
    case State::Paused:
    {
        onUp(j, accepted(at(10)), at(10));
        Up l = up(at(20), false); // "left 3": Paused without a hold ("left 2" arms the battery hold)
        l.left = true;
        l.leftArg = 3;
        onUp(j, l, at(20));
        break;
    }
    case State::Verified:
    {
        Up b = up(at(10), true);
        b.batteryLow = true;
        b.batteryArg = 23;
        onUp(j, b, at(10));
        break;
    }
    case State::Swapping:
    {
        Up c = up(at(10), false);
        c.complete = true;
        onUp(j, c, at(10));
        break;
    }
    case State::Confirming:
    {
        Up c = up(at(10), false);
        c.complete = true;
        onUp(j, c, at(10));
        onUp(j, joinReq(at(300), NEW_FW), at(300));
        break;
    }
    default:
        break;
    }
    TEST_ASSERT_EQUAL_STRING(stateName(s), stateName(j.state));
    return j;
}

enum class Ev : uint8_t
{
    Accepted,
    Progress,
    Refused6,
    Refused3,
    Failed,
    Left2,
    Left3,
    Complete,
    BatteryLow,
    SwapSeen,
    Installed,
    Revert,
    Bit4Zero,
    Bit4One,
    NewSession,
    Deadline,
    Abort,
    Pause,
    Resume,
    Clear,
    Reboot,
    Count,
};

static const char *evName(Ev e)
{
    static const char *names[] = {"accepted", "progress", "refused 6", "refused 3", "failed", "left 2",
                                  "left 3", "complete", "battery low", "swap seen", "installed", "revert",
                                  "bit4=0", "bit4=1", "new session", "deadline", "abort", "pause", "resume",
                                  "clear", "reboot"};
    return names[(int)e];
}

// Apply one event at t = 1000 s; returns the DOWN action
static bool lastChanged; // Result.changed / the admin or tick return value of the last apply()

static Down apply(Job &j, Ev e)
{
    Now n = at(1000);
    Result r;
    bool admin = false;
    switch (e)
    {
    case Ev::Accepted:
        r = onUp(j, accepted(n), n);
        break;
    case Ev::Progress:
        r = onUp(j, progress(n, 10, 0x3, 32), n);
        break;
    case Ev::Refused6:
    case Ev::Refused3:
    {
        Up u = up(n, false);
        u.refused = true;
        u.refusedArg = e == Ev::Refused6 ? 6 : 3;
        r = onUp(j, u, n);
        break;
    }
    case Ev::Failed:
    {
        Up u = up(n, false);
        u.failed = true;
        u.failedArg = 1;
        r = onUp(j, u, n);
        break;
    }
    case Ev::Left2:
    case Ev::Left3:
    {
        Up u = up(n, false);
        u.left = true;
        u.leftArg = e == Ev::Left2 ? 2 : 3;
        r = onUp(j, u, n);
        break;
    }
    case Ev::Complete:
    {
        Up u = up(n, false);
        u.complete = true;
        r = onUp(j, u, n);
        break;
    }
    case Ev::BatteryLow:
    {
        Up u = up(n, true);
        u.batteryLow = true;
        u.batteryArg = 23;
        r = onUp(j, u, n);
        break;
    }
    case Ev::SwapSeen:
        r = onUp(j, joinReq(n, NEW_FW), n);
        break;
    case Ev::Installed:
        r = onUp(j, status(n, NEW_FW, 2, 0), n);
        break;
    case Ev::Revert:
        r = onUp(j, status(n, OLD_FW, 3, 0x08), n);
        break;
    case Ev::Bit4Zero:
        r = onUp(j, up(n, false), n);
        break;
    case Ev::Bit4One:
        r = onUp(j, up(n, true), n);
        break;
    case Ev::NewSession:
        r = onUp(j, joinReq(n, OLD_FW), n);
        break;
    case Ev::Deadline:
    {
        // Far enough for every state's deadline (Confirming: 3 x 6 h + 2 intervals)
        Now late = at(9 * 86400);
        admin = tick(j, late);
        break;
    }
    case Ev::Abort:
        admin = abort(j, n);
        break;
    case Ev::Pause:
        admin = pause(j, n);
        break;
    case Ev::Resume:
        admin = resume(j, n);
        break;
    case Ev::Clear:
        admin = clear(j, n);
        break;
    case Ev::Reboot:
        afterReboot(j, n);
        admin = true;
        break;
    default:
        break;
    }
    lastChanged = r.changed || admin;
    return r.down;
}

struct Cell
{
    State next;
    Down down;
};

using S = State;
using D = Down;

// Rows: events; columns: Waiting, Transfer, Paused, Verified, Swapping, Confirming
static const State COLS[6] = {S::Waiting, S::Transfer, S::Paused, S::Verified, S::Swapping, S::Confirming};
static const Cell MATRIX[(int)Ev::Count][6] = {
    // accepted (+ progress): W -> T, T stays (view rebuilt), P -> T, V incomplete -> T
    {{S::Transfer, D::None}, {S::Transfer, D::None}, {S::Transfer, D::None}, {S::Transfer, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // progress status (bit 4 = 1): W -> new start, T stays, P -> T, V ignored
    {{S::Waiting, D::Start}, {S::Transfer, D::None}, {S::Transfer, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // refused 6: Paused (retry after 6 h), Transfer cannot get one, Verified stays
    {{S::Paused, D::None}, {S::Transfer, D::None}, {S::Paused, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // refused 3 (other): Failed
    {{S::Failed, D::None}, {S::Transfer, D::None}, {S::Failed, D::None}, {S::Failed, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // failed
    {{S::Failed, D::None}, {S::Failed, D::None}, {S::Failed, D::None}, {S::Failed, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // left 2 (battery): Transfer -> Paused, Transfer / Paused: the 6 h battery hold, no start
    {{S::Waiting, D::Start}, {S::Paused, D::None}, {S::Paused, D::None}, {S::Verified, D::Start},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // left 3 (OTA end): a start unless the admin paused (Waiting / Verified: none)
    {{S::Waiting, D::None}, {S::Paused, D::Start}, {S::Paused, D::Start}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // complete
    {{S::Swapping, D::None}, {S::Swapping, D::None}, {S::Swapping, D::None}, {S::Swapping, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // verified, battery low
    {{S::Verified, D::None}, {S::Verified, D::None}, {S::Verified, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // swap seen
    {{S::Confirming, D::None}, {S::Confirming, D::None}, {S::Confirming, D::None}, {S::Confirming, D::None},
     {S::Confirming, D::None}, {S::Confirming, D::None}},
    // installed (boot state 2)
    {{S::Done, D::None}, {S::Done, D::None}, {S::Done, D::None}, {S::Done, D::None}, {S::Done, D::None},
     {S::Done, D::None}},
    // revert report
    {{S::Reverted, D::None}, {S::Reverted, D::None}, {S::Reverted, D::None}, {S::Reverted, D::None},
     {S::Reverted, D::None}, {S::Reverted, D::None}},
    // UP with bit 4 = 0, no event
    {{S::Waiting, D::Start}, {S::Paused, D::Start}, {S::Paused, D::Start}, {S::Verified, D::Start},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // UP with bit 4 = 1, no event
    {{S::Waiting, D::None}, {S::Transfer, D::None}, {S::Paused, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // new session (JOIN_REQ, old version)
    {{S::Waiting, D::None}, {S::Paused, D::None}, {S::Paused, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // deadline
    {{S::Failed, D::None}, {S::Paused, D::None}, {S::Aborted, D::None}, {S::Aborted, D::None},
     {S::Failed, D::None}, {S::Failed, D::None}},
    // Abort (admin): Swapping / Confirming: Clear job only
    {{S::Aborted, D::None}, {S::Aborted, D::None}, {S::Aborted, D::None}, {S::Aborted, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // Pause (admin): Transfer only
    {{S::Waiting, D::None}, {S::Paused, D::None}, {S::Paused, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // Resume (admin): Paused only (stays Paused, manualPause cleared)
    {{S::Waiting, D::None}, {S::Transfer, D::None}, {S::Paused, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
    // Clear job (admin): Verified, Swapping, Confirming
    {{S::Waiting, D::None}, {S::Transfer, D::None}, {S::Paused, D::None}, {S::None, D::None}, {S::None, D::None},
     {S::None, D::None}},
    // gateway reboot
    {{S::Waiting, D::None}, {S::Paused, D::None}, {S::Paused, D::None}, {S::Verified, D::None},
     {S::Swapping, D::None}, {S::Confirming, D::None}},
};

static void test_matrix(void)
{
    char msg[96];
    for (int e = 0; e < (int)Ev::Count; e++)
    {
        for (int c = 0; c < 6; c++)
        {
            Job j = inState(COLS[c]);
            const Job before = j;
            Down d = apply(j, (Ev)e);
            snprintf(msg, sizeof(msg), "%s in %s", evName((Ev)e), stateName(COLS[c]));
            TEST_ASSERT_EQUAL_STRING_MESSAGE(stateName(MATRIX[e][c].next), stateName(j.state), msg);
            TEST_ASSERT_EQUAL_MESSAGE((int)MATRIX[e][c].down, (int)d, msg);
            // Persisted fields: a state change is always reported for job.json
            if (j.state != before.state)
            {
                TEST_ASSERT_TRUE_MESSAGE(lastChanged, msg);
            }
            // The state's own deadline is armed exactly in the states that have one
            const bool hasDl = j.state == S::Waiting || j.state == S::Verified || j.state == S::Swapping ||
                               j.state == S::Confirming;
            TEST_ASSERT_EQUAL_MESSAGE(hasDl, j.dlState.armed(), msg);
            // manualPause only in Paused (set by Pause), abortPending only after Abort
            TEST_ASSERT_TRUE_MESSAGE(!j.manualPause || (j.state == S::Paused && (Ev)e == Ev::Pause), msg);
            TEST_ASSERT_EQUAL_MESSAGE((Ev)e == Ev::Abort && j.state == S::Aborted, j.abortPending, msg);
            // A Paused job has its 7-day clock
            if (j.state == S::Paused)
            {
                TEST_ASSERT_TRUE_MESSAGE(j.pausedOnce && j.dlPaused.armed(), msg);
            }
            // The counters never decrease
            TEST_ASSERT_TRUE_MESSAGE(j.blocksSent >= before.blocksSent && j.statuses >= before.statuses, msg);
        }
    }
}

static void test_waiting_details(void)
{
    // A start answers the UP; its binding is the next UP
    Job j = inState(State::Waiting);
    startSent(j, 41, 5, at(0));
    TEST_ASSERT_EQUAL_UINT32(42, j.expectBind);
    TEST_ASSERT_TRUE(j.startValid);
    // No start without SESSION / inSync
    Up u = up(at(10), false);
    u.canStart = false;
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(j, u, at(10)).down);
    // An event suppresses the start ("complete" never gets one)
    Up c = up(at(20), false);
    c.complete = true;
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(j, c, at(20)).down);
    // Deadline: max(3 intervals = 1800 s, 3 rounds = 3 x 33 x 26 s = 2574 s)
    Job w = inState(State::Waiting);
    tick(w, at(2573));
    TEST_ASSERT_EQUAL((int)State::Waiting, (int)w.state);
    tick(w, at(2574));
    TEST_ASSERT_EQUAL((int)State::Failed, (int)w.state);
    TEST_ASSERT_EQUAL((int)Why::NoAccept, (int)w.why);
    // The accepting UP binds and anchors (reliable: blocks may go)
    Job a = inState(State::Waiting);
    Up acc = accepted(at(30));
    acc.fcnt32 = 77;
    acc.sessionNonce = 9;
    onUp(a, acc, at(30));
    TEST_ASSERT_TRUE(a.bindValid);
    TEST_ASSERT_EQUAL_UINT32(77, a.bindFcnt);
    TEST_ASSERT_EQUAL_UINT32(9, a.sessionNonce);
    TEST_ASSERT_TRUE(a.anchorReliable);
    TEST_ASSERT_EQUAL_INT64(at(30).us, a.anchorUs);
    // ... unreliable: no blocks until a status re-anchors
    Job b = inState(State::Waiting);
    Up acc2 = accepted(at(30));
    acc2.timeReliable = false;
    onUp(b, acc2, at(30));
    uint32_t slot = 0;
    TEST_ASSERT_FALSE(nextBlockSlot(b, at(31).us, slot));
    onUp(b, progress(at(900), 0, 0, 32), at(900));
    TEST_ASSERT_TRUE(b.anchorReliable);
    TEST_ASSERT_TRUE(nextBlockSlot(b, at(901).us, slot));
}

static void test_refusal_rules(void)
{
    // Refusal 6: Paused, start again only after 6 h
    Job j = inState(State::Waiting);
    Up r = up(at(10), false);
    r.refused = true;
    r.refusedArg = 6;
    onUp(j, r, at(10));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(j, up(at(3600), false), at(3600)).down);
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, up(at(6 * 3600 + 10), false), at(6 * 3600 + 10)).down);
    // Refusal 5 of this job's version -> Confirming; of another version -> Failed
    Job c = inState(State::Waiting);
    Up r5 = up(at(10), false);
    r5.refused = true;
    r5.refusedArg = 5;
    r5.curFw = NEW_FW;
    onUp(c, r5, at(10));
    TEST_ASSERT_EQUAL((int)State::Confirming, (int)c.state);
    Job f = inState(State::Waiting);
    r5.curFw = OLD_FW;
    onUp(f, r5, at(10));
    TEST_ASSERT_EQUAL((int)State::Failed, (int)f.state);
    TEST_ASSERT_EQUAL((int)Why::Refused, (int)f.why);
    TEST_ASSERT_EQUAL(5, f.whyArg);
    // Verified + refused 6 stays, retry not before 6 h
    Job v = inState(State::Verified);
    Up r6 = up(at(100), false);
    r6.refused = true;
    r6.refusedArg = 6;
    onUp(v, r6, at(100));
    TEST_ASSERT_EQUAL((int)State::Verified, (int)v.state);
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(v, up(at(200), false), at(200)).down);
}

static void test_no_progress_and_laps(void)
{
    Job j = inState(State::Transfer);
    // 4 counted statuses (>= K/4 blocks launched before each) without progress -> Failed
    onUp(j, progress(at(900), 5, 0, 32), at(900));
    TEST_ASSERT_EQUAL(5, j.maxConfirmed);
    for (int i = 0; i < 4; i++)
    {
        for (int b = 0; b < 8; b++)
        {
            launched(j, true, (uint16_t)(5 + b), 1263, at(901 + i * 900 + b));
        }
        onUp(j, progress(at(1700 + i * 900), 5, 0, (uint16_t)(65 + i * 33)), at(1700 + i * 900));
        if (i < 3)
        {
            TEST_ASSERT_EQUAL((int)State::Transfer, (int)j.state);
        }
    }
    TEST_ASSERT_EQUAL((int)State::Failed, (int)j.state);
    TEST_ASSERT_EQUAL((int)Why::NoProgress, (int)j.why);

    // Statuses with fewer than K/4 launches do not count
    Job s = inState(State::Transfer);
    onUp(s, progress(at(900), 5, 0, 32), at(900));
    for (int i = 0; i < 6; i++)
    {
        launched(s, true, 6, 1263, at(901 + i * 900));
        onUp(s, progress(at(1700 + i * 900), 5, 0, (uint16_t)(65 + i * 33)), at(1700 + i * 900));
    }
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)s.state);

    // Laps: 3 x "accepted -> left 1" without progress (not starved) -> Failed
    Job l = inState(State::Waiting);
    for (int lap = 0; lap < 3; lap++)
    {
        uint32_t t0 = 10 + lap * 4000;
        onUp(l, accepted(at(t0)), at(t0));
        for (int b = 0; b < 8; b++)
        {
            launched(l, true, (uint16_t)b, 1263, at(t0 + 100 + b));
        }
        Up left = up(at(t0 + 1800), false);
        left.left = true;
        left.leftArg = 1;
        onUp(l, left, at(t0 + 1800));
        if (lap < 2)
        {
            TEST_ASSERT_EQUAL((int)State::Paused, (int)l.state);
        }
    }
    TEST_ASSERT_EQUAL((int)State::Failed, (int)l.state);

    // An accepted that reports less than the maximum resets it
    Job m = inState(State::Transfer);
    onUp(m, progress(at(900), 50, 0, 32), at(900));
    TEST_ASSERT_EQUAL(50, m.maxConfirmed);
    onUp(m, accepted(at(1000), 1, 10, 0), at(1000));
    TEST_ASSERT_EQUAL(10, m.maxConfirmed);
    // ... and one that reports more raises it
    onUp(m, accepted(at(1100), 1, 60, 0x1), at(1100));
    TEST_ASSERT_EQUAL(61, m.maxConfirmed);
}

static void test_starved_backoff(void)
{
    // The sensor left for silence while the gateway launched nothing: Paused
    // with a back-off of 1 h (doubling to 6 h), not a lap, no start
    Job j = inState(State::Transfer);
    Up left = up(at(1800), false);
    left.left = true;
    left.leftArg = 1;
    Result r = onUp(j, left, at(1800));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    TEST_ASSERT_EQUAL((int)Why::Starved, (int)j.why);
    TEST_ASSERT_EQUAL((int)Down::None, (int)r.down);
    TEST_ASSERT_EQUAL(0, j.laps);
    TEST_ASSERT_EQUAL_UINT32(3600, j.backoffS);
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(j, up(at(3000), false), at(3000)).down);
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, up(at(1800 + 3600), false), at(1800 + 3600)).down);
    // Next starved lap: 2 h
    onUp(j, accepted(at(6000)), at(6000));
    onUp(j, left, at(8000));
    TEST_ASSERT_EQUAL_UINT32(7200, j.backoffS);
}

static void test_missed_statuses(void)
{
    Job j = inState(State::Transfer); // anchor at t = 10 s
    // Status slot 32 at 10 + 4 + 32 x 26 = 846 s; T_status 850 s
    tick(j, at(849));
    TEST_ASSERT_EQUAL(0, j.missedSinceStatus);
    tick(j, at(850));
    TEST_ASSERT_EQUAL(1, j.missedSinceStatus);
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)j.state);
    // Slot 65 at 846 + 858 = 1704 s; T_status 1708 s: the second miss pauses
    tick(j, at(1708));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    TEST_ASSERT_EQUAL((int)Why::Missed, (int)j.why);
    // A status in its slot resets the count
    Job k = inState(State::Transfer);
    onUp(k, progress(at(846), 0, 1, 32), at(846));
    tick(k, at(851));
    TEST_ASSERT_EQUAL(0, k.missedSinceStatus);
}

static void test_anchor_from_status(void)
{
    Job j = inState(State::Transfer);
    Now n = at(846);
    n.us += 280000 + 3000; // RxDone: the UP started 3 ms late, 280 ms on air
    Up p = progress(n, 0, 1, 32);
    p.airUs = 280000;
    onUp(j, p, n);
    // A = RxDone - airtime - (start delay + 32 x C)
    TEST_ASSERT_EQUAL_INT64(at(846).us + 3000 - (4000 + 32 * 26000) * 1000LL, j.anchorUs);
    // Slot 32 of the new anchor is 3 ms later than before
    TEST_ASSERT_EQUAL_INT64(at(846).us + 3000, j.slotUs(32));
    // The accepting UP (slot 0xFFFF) and a non-status slot never re-anchor through the formula
    Job k = inState(State::Transfer);
    int64_t a = k.anchorUs;
    onUp(k, progress(at(900), 0, 1, 31), at(900));
    TEST_ASSERT_EQUAL_INT64(a, k.anchorUs);
}

// Spec 14.3: a status re-anchors only within +-(20 ms + d_gw); a late status
// (preparation overrun, bench: +160 ms) keeps the anchor but still counts
static void test_anchor_bound(void)
{
    // +15 ms: inside the bound, the anchor moves
    Job j = inState(State::Transfer);
    const int64_t a0 = j.anchorUs;
    Now n = at(846);
    n.us += 280000 + 15000;
    Up p = progress(n, 5, 1, 32);
    p.airUs = 280000;
    onUp(j, p, n);
    TEST_ASSERT_EQUAL_INT64(a0 + 15000, j.anchorUs);
    TEST_ASSERT_EQUAL(0, j.anchorRejects);

    // +160 ms: out of bound (20 ms + 20 ppm x 846 s + 5 ms = 41.9 ms), anchor kept,
    // the progress applied
    Job k = inState(State::Transfer);
    const int64_t b0 = k.anchorUs;
    Now m = at(846);
    m.us += 280000 + 160000;
    Up q = progress(m, 5, 1, 32);
    q.airUs = 280000;
    onUp(k, q, m);
    TEST_ASSERT_EQUAL_INT64(b0, k.anchorUs);
    TEST_ASSERT_EQUAL(1, k.anchorRejects);
    TEST_ASSERT_EQUAL(1, k.statuses);
    TEST_ASSERT_EQUAL(5, k.viewBase);
    TEST_ASSERT_TRUE(k.maxConfirmed >= 5);
    TEST_ASSERT_EQUAL(State::Transfer, k.state);

    // The bound grows with the time since the last anchor: after 10 rounds
    // (≈ 8 580 s, 20 ppm ≈ 172 ms) a +160 ms shift is accepted
    Job w = inState(State::Transfer);
    const int64_t c0 = w.anchorUs;
    const uint32_t s = 10 * 33 + 32; // status slot of round 11
    Now late = at(14 + s * 26);
    late.us += 280000 + 160000;
    Up r = progress(late, 0, 1, (uint16_t)s);
    r.airUs = 280000;
    onUp(w, r, late);
    TEST_ASSERT_EQUAL_INT64(c0 + 160000, w.anchorUs);

    // A binding taken over from a lost accepting UP replaces any anchor
    Job t = inState(State::Transfer);
    t.bindFcnt += 7; // the anchor belongs to another binding
    const int64_t d0 = t.anchorUs;
    Now tn = at(846);
    tn.us += 280000 + 160000;
    Up tu = progress(tn, 0, 1, 32);
    tu.airUs = 280000;
    onUp(t, tu, tn);
    TEST_ASSERT_EQUAL_INT64(d0 + 160000, t.anchorUs);
    TEST_ASSERT_EQUAL(0, t.anchorRejects);
}

static void test_block_choice(void)
{
    Job j = inState(State::Transfer);
    uint16_t i = 0;
    TEST_ASSERT_TRUE(chooseBlock(j, i));
    TEST_ASSERT_EQUAL(0, i);
    launched(j, true, 0, 1263, at(20));
    launched(j, true, 1, 1263, at(46));
    TEST_ASSERT_TRUE(chooseBlock(j, i));
    TEST_ASSERT_EQUAL(2, i);
    // Every block of the window sent: the lowest unconfirmed again
    for (uint16_t b = 2; b < 64; b++)
    {
        launched(j, true, b, 1263, at(50));
    }
    TEST_ASSERT_TRUE(chooseBlock(j, i));
    TEST_ASSERT_EQUAL(0, i);
    // A status replaces the view: base 10, bit 0 (=10) missing, bit 1 (=11) received
    onUp(j, progress(at(846), 10, 0x2, 32), at(846));
    TEST_ASSERT_TRUE(chooseBlock(j, i));
    TEST_ASSERT_EQUAL(10, i);
    launched(j, true, 10, 1263, at(900));
    TEST_ASSERT_TRUE(chooseBlock(j, i));
    TEST_ASSERT_EQUAL(12, i); // 11 confirmed
    TEST_ASSERT_TRUE(j.repeats > 0);
    // Never beyond the window
    Job w = inState(State::Transfer);
    for (uint16_t b = 0; b < 64; b++)
    {
        launched(w, true, b, 1263, at(50));
    }
    TEST_ASSERT_TRUE(chooseBlock(w, i));
    TEST_ASSERT_TRUE(i < 64);
    // A base past the block count is ignored (a missed status)
    Job x = inState(State::Transfer);
    onUp(x, progress(at(846), 5000, 0, 32), at(846));
    TEST_ASSERT_EQUAL(1, x.missedSinceStatus);
}

static void test_block_slots(void)
{
    Job j = inState(State::Transfer); // anchor 10 s, slot 0 at 14 s
    uint32_t s = 99;
    TEST_ASSERT_TRUE(nextBlockSlot(j, at(5).us, s));
    TEST_ASSERT_EQUAL_UINT32(0, s);
    TEST_ASSERT_TRUE(nextBlockSlot(j, at(15).us, s));
    TEST_ASSERT_EQUAL_UINT32(1, s);
    // Never a status slot (32)
    TEST_ASSERT_TRUE(nextBlockSlot(j, j.slotUs(31) + 1, s));
    TEST_ASSERT_EQUAL_UINT32(33, s);
    // Not in Paused (unless an OTA end of the admin waits)
    Job p = inState(State::Transfer);
    pause(p, at(20));
    TEST_ASSERT_TRUE(nextBlockSlot(p, at(21).us, s));
    TEST_ASSERT_EQUAL((int)PendingEnd::Pause, (int)p.pendingEnd);
    resume(p, at(22));
    TEST_ASSERT_FALSE(nextBlockSlot(p, at(23).us, s));
    Job a = inState(State::Transfer);
    abort(a, at(20));
    TEST_ASSERT_TRUE(nextBlockSlot(a, at(21).us, s));
    TEST_ASSERT_EQUAL((int)PendingEnd::Abort, (int)a.pendingEnd);
}

static void test_abort_delivery_and_orphans(void)
{
    Job j = inState(State::Transfer);
    abort(j, at(20));
    TEST_ASSERT_TRUE(j.abortPending);
    // Every UP with bit 4 = 1 gets an OTA end abort
    TEST_ASSERT_EQUAL((int)Down::EndAbort, (int)onUp(j, progress(at(846), 0, 0, 32), at(846)).down);
    TEST_ASSERT_TRUE(j.abortPending);
    // "left 3" ends it
    Up l3 = up(at(900), false);
    l3.left = true;
    l3.leftArg = 3;
    onUp(j, l3, at(900));
    TEST_ASSERT_FALSE(j.abortPending);
    // An UP with bit 4 = 0 ends it before the DOWN is chosen: no abort to an out-of-session sensor
    Job k = inState(State::Paused);
    abort(k, at(30));
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(k, up(at(40), false), at(40)).down);
    TEST_ASSERT_FALSE(k.abortPending);
    // Expires after 2 rounds; then an orphan UP gets a pause
    Job e = inState(State::Verified);
    abort(e, at(30));
    tick(e, at(30 + 2 * 858));
    TEST_ASSERT_FALSE(e.abortPending);
    Up b = up(at(4000), true);
    b.batteryLow = true;
    TEST_ASSERT_EQUAL((int)Down::EndPause, (int)onUp(e, b, at(4000)).down);
    // Clear job in Verified: the next UP with bit 4 = 1 is an orphan -> pause
    Job c = inState(State::Verified);
    clear(c, at(50));
    TEST_ASSERT_EQUAL((int)Down::EndPause, (int)onUp(c, b, at(60)).down);
    // A Retry / new job overwrites abortPending
    Job r = inState(State::Transfer);
    abort(r, at(20));
    begin(r, target(), image(), at(30));
    TEST_ASSERT_FALSE(r.abortPending);
    TEST_ASSERT_EQUAL((int)State::Waiting, (int)r.state);
}

static void test_manual_pause(void)
{
    Job j = inState(State::Transfer);
    pause(j, at(20));
    TEST_ASSERT_TRUE(j.manualPause);
    // A progress status while paused by the admin is answered with an OTA end pause
    TEST_ASSERT_EQUAL((int)Down::EndPause, (int)onUp(j, progress(at(846), 0, 0, 32), at(846)).down);
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    // No start while paused
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(j, up(at(900), false), at(900)).down);
    resume(j, at(950));
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, up(at(1000), false), at(1000)).down);
}

static void test_deadlines_and_reboot(void)
{
    // Paused: 7 days from the first entry, not re-armed by another pause
    Job j = inState(State::Paused); // first Paused at 20 s
    onUp(j, accepted(at(100)), at(100));
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)j.state);
    tick(j, at(20 + SEVEN_DAYS_S)); // Transfer: not aborted on day 7
    TEST_ASSERT_TRUE(j.state != State::Aborted);
    // Swapping: 4 h + 2 intervals
    Job s = inState(State::Swapping); // complete at 10 s
    tick(s, at(10 + 4 * 3600 + 1199));
    TEST_ASSERT_EQUAL((int)State::Swapping, (int)s.state);
    tick(s, at(10 + 4 * 3600 + 1200));
    TEST_ASSERT_EQUAL((int)State::Failed, (int)s.state);
    TEST_ASSERT_EQUAL((int)Why::NoJoin, (int)s.why);
    // Confirming: 3 x T_confirm (6 h) + 2 intervals
    Job c = inState(State::Confirming); // entered at 300 s
    tick(c, at(300 + 3 * 21600 + 1199));
    TEST_ASSERT_EQUAL((int)State::Confirming, (int)c.state);
    tick(c, at(300 + 3 * 21600 + 1200));
    TEST_ASSERT_EQUAL((int)Why::NotConfirmed, (int)c.why);
    // Uptime deadlines (no valid clock) restart after a reboot; unix ones are kept
    Job u;
    Now n0 = at(0);
    n0.unixS = 0;
    begin(u, target(), image(), n0);
    TEST_ASSERT_EQUAL(2, u.dlState.kind);
    Now after = n0;
    after.uptimeS = 5; // rebooted
    afterReboot(u, after);
    TEST_ASSERT_EQUAL_UINT32(5 + 2574, u.dlState.at);
    Job x = inState(State::Waiting); // unix deadline
    uint32_t at0 = x.dlState.at;
    afterReboot(x, after);
    TEST_ASSERT_EQUAL_UINT32(at0, x.dlState.at);
    // Reboot in Transfer: Paused, RAM views gone, no blocks until re-anchored
    Job t = inState(State::Transfer);
    afterReboot(t, at(50));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)t.state);
    uint32_t slot = 0;
    TEST_ASSERT_FALSE(nextBlockSlot(t, at(60).us, slot));
}

static void test_revert_and_reinstall(void)
{
    // A baseline that is already a flagged revert does not count before "complete"
    Job j;
    Target t = target();
    t.boot = 3;
    t.flags = 0x08;
    begin(j, t, image(), at(0));
    onUp(j, status(at(10), OLD_FW, 3, 0x08), at(10));
    TEST_ASSERT_EQUAL((int)State::Waiting, (int)j.state);
    // ... but in Swapping every flagged revert counts
    Up c = up(at(20), false);
    c.complete = true;
    onUp(j, c, at(20));
    Result r = onUp(j, status(at(30), OLD_FW, 3, 0x08), at(30));
    TEST_ASSERT_EQUAL((int)State::Reverted, (int)j.state);
    TEST_ASSERT_TRUE(r.revertRecord);
    // A revert without flag bit 3 (an old revert) never counts
    Job o = inState(State::Swapping);
    onUp(o, status(at(40), OLD_FW, 3, 0), at(40));
    TEST_ASSERT_EQUAL((int)State::Swapping, (int)o.state);
    // Reinstall of the running version: boot state 1 + flag bit 3 = swap seen,
    // then boot state 2 = installed
    Job ri;
    begin(ri, target(), image(OLD_FW), at(0));
    Up cc = up(at(10), false);
    cc.complete = true;
    onUp(ri, cc, at(10));
    onUp(ri, status(at(300), OLD_FW, 1, 0x08), at(300));
    TEST_ASSERT_EQUAL((int)State::Confirming, (int)ri.state);
    onUp(ri, status(at(900), OLD_FW, 2, 0x08), at(900));
    TEST_ASSERT_EQUAL((int)State::Done, (int)ri.state);
    // A reinstall in Waiting is not "installed" by an old boot state 2
    Job rw;
    begin(rw, target(), image(OLD_FW), at(0));
    onUp(rw, status(at(10), OLD_FW, 2, 0), at(10));
    TEST_ASSERT_EQUAL((int)State::Waiting, (int)rw.state);
    // KEPT (boot state 6) -> Done flagged
    Job k = inState(State::Confirming);
    onUp(k, status(at(900), NEW_FW, 6, 0x08), at(900));
    TEST_ASSERT_EQUAL((int)State::Done, (int)k.state);
    TEST_ASSERT_TRUE(k.kept);
    // Installed despite abort; then the revert watch writes the record
    Job a = inState(State::Verified);
    abort(a, at(20));
    Result da = onUp(a, status(at(600), NEW_FW, 1, 0x08), at(600));
    TEST_ASSERT_EQUAL((int)State::Done, (int)a.state);
    TEST_ASSERT_TRUE(a.despiteAbort);
    TEST_ASSERT_FALSE(a.abortKept); // it swapped: nothing kept
    TEST_ASSERT_FALSE(da.abortKept);
    Result rr = onUp(a, status(at(9000), OLD_FW, 3, 0x08), at(9000));
    TEST_ASSERT_TRUE(rr.revertRecord);
    // Failed "not confirmed": a later revert still writes the record
    Job f = inState(State::Confirming);
    tick(f, at(300 + 3 * 21600 + 1200));
    TEST_ASSERT_TRUE(onUp(f, status(at(70000), OLD_FW, 3, 0x08), at(70000)).revertRecord);
    // Cleared in Confirming: ghost watch
    Job g = inState(State::Confirming);
    clear(g, at(400));
    TEST_ASSERT_EQUAL((int)State::None, (int)g.state);
    TEST_ASSERT_TRUE(onUp(g, status(at(600), OLD_FW, 4, 0x08), at(600)).revertRecord);
}

static void test_verified_paths(void)
{
    // Verified + accepted with a complete progress stays (the shortcut answer)
    Job j = inState(State::Verified);
    onUp(j, accepted(at(100), 1, j.blocks(), 0), at(100));
    TEST_ASSERT_EQUAL((int)State::Verified, (int)j.state);
    // Waiting + battery low -> Verified; 7 days -> Aborted
    Job w = inState(State::Waiting);
    Up b = up(at(10), true);
    b.batteryLow = true;
    onUp(w, b, at(10));
    TEST_ASSERT_EQUAL((int)State::Verified, (int)w.state);
    tick(w, at(10 + SEVEN_DAYS_S));
    TEST_ASSERT_EQUAL((int)State::Aborted, (int)w.state);
}

// M-1: a lost accepting UP at a 600 s interval with a late report phase resumes, not "no accept"
static void test_lost_accept_late_phase(void)
{
    Job j = inState(State::Waiting); // queued at 0, deadline 1800 s
    // start with the report at 590 s, the accepting UP at 808 s is lost,
    // the first status at 808 + 836 = 1644 s gets a new start
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, up(at(590), false), at(590)).down);
    startSent(j, 10, 5, at(590));
    Up p = progress(at(1644), 0, 0, 32);
    p.sessionNonce = 5;
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, p, at(1644)).down);
    startSent(j, 12, 5, at(1644));
    // the second accepting UP comes 218 s later - after the old 1800 s deadline
    tick(j, at(1861));
    TEST_ASSERT_EQUAL((int)State::Waiting, (int)j.state);
    Up a = accepted(at(1862));
    a.fcnt32 = 13;
    a.sessionNonce = 5;
    Result r = onUp(j, a, at(1862));
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)j.state);
    TEST_ASSERT_FALSE(r.bindMismatch);
    // the deadline is still a deadline: no accept for 2574 s after the status
    Job k = inState(State::Waiting);
    onUp(k, p, at(1644));
    tick(k, at(1644 + 2574));
    TEST_ASSERT_EQUAL((int)State::Failed, (int)k.state);
}

// M-2: a new session with a lost accepting UP - the status takes the start's binding
static void test_new_session_lost_accept(void)
{
    Job j = inState(State::Transfer); // accepted under session 0 (Up default nonce)
    j.sessionNonce = 7;
    j.bindFcnt = 100;
    afterReboot(j, at(500)); // gateway reboot -> Paused
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    // the sensor's first UP of the new session (nonce 8): bit 4 = 0 -> a start
    Up u = up(at(600), false);
    u.sessionNonce = 8;
    u.fcnt32 = 0;
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, u, at(600)).down);
    startSent(j, 0, 8, at(600));
    // the accepting UP (FCnt 1) is lost; the first status of that session arrives
    Up p = progress(at(1440), 3, 0, 32);
    p.sessionNonce = 8;
    p.fcnt32 = 2;
    Result r = onUp(j, p, at(1440));
    TEST_ASSERT_TRUE(r.adopted);
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)j.state);
    TEST_ASSERT_EQUAL_UINT32(8, j.sessionNonce);
    TEST_ASSERT_EQUAL_UINT32(1, j.bindFcnt);
    TEST_ASSERT_TRUE(j.bindValid);
    uint32_t slot = 0;
    TEST_ASSERT_TRUE(nextBlockSlot(j, at(1441).us, slot));
    // A status of a session we sent no start in gets a new start instead
    Job k = inState(State::Transfer);
    k.sessionNonce = 7;
    afterReboot(k, at(500));
    Up q = progress(at(1440), 3, 0, 32);
    q.sessionNonce = 9;
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(k, q, at(1440)).down);
    TEST_ASSERT_EQUAL((int)State::Paused, (int)k.state);
    // Same session (Paused by missed statuses): unchanged behaviour
    Job m = inState(State::Transfer);
    m.sessionNonce = 4;
    tick(m, at(2000));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)m.state);
    Up s = progress(at(2600), 5, 0, 98);
    s.sessionNonce = 4;
    onUp(m, s, at(2600));
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)m.state);
    // A mismatching accepting UP binds with its own FCnt and is reported
    Job w = inState(State::Waiting);
    startSent(w, 20, 3, at(0));
    Up acc = accepted(at(100));
    acc.fcnt32 = 23;
    TEST_ASSERT_TRUE(onUp(w, acc, at(100)).bindMismatch);
    TEST_ASSERT_EQUAL_UINT32(23, w.bindFcnt);
}

static void test_resume_and_stale_pause(void)
{
    // Pause, Resume before "left 3" arrives: that UP gets the start
    Job j = inState(State::Transfer);
    pause(j, at(20));
    resume(j, at(30));
    TEST_ASSERT_FALSE(j.manualPause);
    Up l3 = up(at(40), false);
    l3.left = true;
    l3.leftArg = 3;
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, l3, at(40)).down);
    // ... but not while paused
    Job p = inState(State::Transfer);
    pause(p, at(20));
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(p, l3, at(40)).down);
    // Pause, then "verified, battery low": manualPause goes, a reset in the wait gets a start
    Job v = inState(State::Transfer);
    pause(v, at(20));
    Up b = up(at(30), true);
    b.batteryLow = true;
    onUp(v, b, at(30));
    TEST_ASSERT_EQUAL((int)State::Verified, (int)v.state);
    TEST_ASSERT_FALSE(v.manualPause);
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(v, up(at(100), false), at(100)).down);
}

static void test_shrink_and_cycle(void)
{
    Job j = inState(State::Transfer);
    onUp(j, progress(at(846), 100, 0, 32), at(846));
    TEST_ASSERT_EQUAL(100, j.maxConfirmed);
    for (int b = 0; b < 8; b++)
    {
        launched(j, true, (uint16_t)(100 + b), 1263, at(850 + b));
    }
    // a dirty page below the base: base 90 - not counted, the maximum stays
    onUp(j, progress(at(1704), 90, 0, 65), at(1704));
    TEST_ASSERT_EQUAL(0, j.noProgress);
    TEST_ASSERT_EQUAL(100, j.maxConfirmed);
    TEST_ASSERT_EQUAL(1, j.shrinks);
    for (int b = 0; b < 8; b++)
    {
        launched(j, true, (uint16_t)(90 + b), 1263, at(1710 + b));
    }
    // the refill is progress again
    onUp(j, progress(at(2562), 108, 0, 98), at(2562));
    TEST_ASSERT_EQUAL(0, j.noProgress);
    // second pass cycles through the unconfirmed blocks
    Job c = inState(State::Transfer);
    for (uint16_t b = 0; b < 64; b++)
    {
        launched(c, true, b, 1263, at(50));
    }
    uint16_t i = 0;
    chooseBlock(c, i);
    TEST_ASSERT_EQUAL(0, i);
    launched(c, true, 0, 1263, at(60));
    chooseBlock(c, i);
    TEST_ASSERT_EQUAL(1, i);
    launched(c, true, 1, 1263, at(70));
    chooseBlock(c, i);
    TEST_ASSERT_EQUAL(2, i);
}

static void test_watch_after_flagged_baseline(void)
{
    // Retry right after a revert (baseline boot 3 + flag 3), then Failed
    // "not confirmed": a flagged revert in the watch still writes the record
    Job j;
    Target t = target();
    t.boot = 3;
    t.flags = 0x08;
    begin(j, t, image(), at(0));
    Up c = up(at(10), false);
    c.complete = true;
    onUp(j, c, at(10));
    onUp(j, joinReq(at(300), NEW_FW), at(300));
    tick(j, at(300 + 3 * 21600 + 1200));
    TEST_ASSERT_EQUAL((int)Why::NotConfirmed, (int)j.why);
    TEST_ASSERT_TRUE(onUp(j, status(at(70000), OLD_FW, 3, 0x08), at(70000)).revertRecord);
}

// Round 2: a start in the SAME session whose accepting UP was lost
static void test_same_session_lost_accept(void)
{
    for (int kind = 0; kind < 3; kind++) // left 1, left 3, a plain bit 4 = 0 UP
    {
        Job j = inState(State::Transfer); // accepted: bindFcnt 100, session 0
        for (int b = 0; b < 8; b++)
        {
            launched(j, true, (uint16_t)b, 1263, at(1500 + b * 26));
        }
        Up l = up(at(1800), false);
        if (kind < 2)
        {
            l.left = true;
            l.leftArg = kind == 0 ? 1 : 3;
        }
        l.fcnt32 = 120;
        Result r = onUp(j, l, at(1800));
        TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
        TEST_ASSERT_EQUAL((int)Down::Start, (int)r.down);
        startSent(j, 120, 0, at(1800));
        // accepting UP (121) lost; the first status of the new schedule
        Up p = progress(at(2700), 8, 0, 32);
        p.fcnt32 = 122;
        r = onUp(j, p, at(2700));
        TEST_ASSERT_TRUE(r.adopted);
        TEST_ASSERT_EQUAL((int)State::Transfer, (int)j.state);
        TEST_ASSERT_EQUAL_UINT32(121, j.bindFcnt);
        TEST_ASSERT_FALSE(j.startValid);
    }
    // Paused by missed statuses, the sensor still in its session (no start
    // sent): the old binding stays
    Job m = inState(State::Transfer);
    tick(m, at(2000));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)m.state);
    onUp(m, progress(at(2600), 5, 0, 98), at(2600));
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)m.state);
    TEST_ASSERT_EQUAL_UINT32(100, m.bindFcnt);
}

// Round 2: the Waiting deadline is armed by the first 4 starts (3 rounds)
static void test_waiting_deadline_arming(void)
{
    // 600 s interval, the report just before the interval ends (phase 0.99):
    // start at 594 s, accepting UP lost, first status (at 594 + 218 + 836) lost,
    // the second status at + 858 s gets a new start - still Waiting
    Job j = inState(State::Waiting);
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, up(at(594), false), at(594)).down);
    startSent(j, 10, 5, at(594));
    tick(j, at(594 + 218 + 836 + 858));
    TEST_ASSERT_EQUAL((int)State::Waiting, (int)j.state);
    // A sensor that never receives the start: the 5th start no longer re-arms
    Job d = inState(State::Waiting);
    for (int i = 0; i < 6; i++)
    {
        startSent(d, (uint32_t)(10 + i), 5, at((uint32_t)(600 * (i + 1))));
    }
    TEST_ASSERT_EQUAL(START_ARMS, d.startsArmed);
    tick(d, at(600 * 4 + 2574));
    TEST_ASSERT_EQUAL((int)State::Failed, (int)d.state);
    TEST_ASSERT_EQUAL((int)Why::NoAccept, (int)d.why);
}

// Round 2: a page that keeps failing (oscillating base) still ends Failed
static void test_oscillating_base(void)
{
    Job j = inState(State::Transfer);
    onUp(j, progress(at(846), 100, 0, 32), at(846));
    uint32_t t = 900;
    uint16_t slot = 65;
    for (int cycle = 0; cycle < 12 && j.state == State::Transfer; cycle++)
    {
        for (int b = 0; b < 8; b++)
        {
            launched(j, true, (uint16_t)(90 + b), 1263, at(t + b));
        }
        // shrink to 90, then refill to 100 (never beyond the high-water mark)
        onUp(j, progress(at(t + 800), cycle % 2 == 0 ? 90 : 100, 0, slot), at(t + 800));
        t += 858;
        slot += 33;
    }
    TEST_ASSERT_EQUAL((int)State::Failed, (int)j.state);
    TEST_ASSERT_EQUAL((int)Why::NoProgress, (int)j.why);
}

// Part F: "left 2" holds the job like a refusal 6 - no useless start in that DOWN
static void test_battery_leave(void)
{
    for (int c = 0; c < 2; c++) // from Transfer and from Paused
    {
        Job j = inState(c == 0 ? State::Transfer : State::Paused);
        Up l = up(at(100), false);
        l.left = true;
        l.leftArg = 2;
        Result r = onUp(j, l, at(100));
        TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
        TEST_ASSERT_EQUAL((int)Why::Battery, (int)j.why);
        TEST_ASSERT_EQUAL((int)Down::None, (int)r.down);
        TEST_ASSERT_TRUE(r.changed);
        TEST_ASSERT_TRUE(j.dlBattery.armed());
        TEST_ASSERT_FALSE(j.dlBackoff.armed()); // not starved, not a lap
        TEST_ASSERT_EQUAL(0, j.laps);
        // No start for 6 h, then the next report gets one
        TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(j, up(at(100 + 6 * 3600 - 1), false), at(100 + 6 * 3600 - 1)).down);
        TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, up(at(100 + 6 * 3600), false), at(100 + 6 * 3600)).down);
    }
    // Paused by the admin: the hold is armed, the reason stays the manual pause
    Job m = inState(State::Transfer);
    pause(m, at(50));
    Up l2 = up(at(100), false);
    l2.left = true;
    l2.leftArg = 2;
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(m, l2, at(100)).down);
    TEST_ASSERT_EQUAL((int)Why::ManualPause, (int)m.why);
    TEST_ASSERT_TRUE(m.manualPause);
    TEST_ASSERT_TRUE(m.dlBattery.armed());
    // ... Resume lifts both
    TEST_ASSERT_TRUE(resume(m, at(200)));
    TEST_ASSERT_FALSE(m.dlBattery.armed());
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(m, up(at(300), false), at(300)).down);
    // "left 1" / "left 3" unchanged: a start in this DOWN
    for (uint8_t arg = 1; arg <= 3; arg += 2)
    {
        Job j = inState(State::Transfer);
        for (int b = 0; b < 8; b++)
        {
            launched(j, true, (uint16_t)b, 1263, at(1500 + b * 26));
        }
        Up l = up(at(1800), false);
        l.left = true;
        l.leftArg = arg;
        TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, l, at(1800)).down);
        TEST_ASSERT_FALSE(j.dlBattery.armed());
    }
}

// Part F: the admin's Resume lifts the battery hold (refusal 6 or "left 2")
static void test_resume_battery_hold(void)
{
    // Refusal 6: Resume offered, the next report gets the start
    Job j = inState(State::Waiting);
    Up r6 = up(at(10), false);
    r6.refused = true;
    r6.refusedArg = 6;
    onUp(j, r6, at(10));
    TEST_ASSERT_TRUE(batteryHold(j, at(20)));
    TEST_ASSERT_TRUE(canResume(j, at(20)));
    TEST_ASSERT_TRUE(resume(j, at(20)));
    TEST_ASSERT_FALSE(j.dlBattery.armed());
    TEST_ASSERT_EQUAL((int)Why::None, (int)j.why);
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(j, up(at(30), false), at(30)).down);
    // ... a sensor still low refuses again: the hold is armed again
    onUp(j, r6, at(40));
    TEST_ASSERT_TRUE(batteryHold(j, at(50)));
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(j, up(at(60), false), at(60)).down);
    // "left 2": the same
    Job l = inState(State::Transfer);
    Up l2 = up(at(100), false);
    l2.left = true;
    l2.leftArg = 2;
    onUp(l, l2, at(100));
    TEST_ASSERT_TRUE(canResume(l, at(200)));
    TEST_ASSERT_TRUE(resume(l, at(200)));
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(l, up(at(300), false), at(300)).down);
    // After the 6 h the hold is over: nothing to resume
    Job e = inState(State::Waiting);
    onUp(e, r6, at(10));
    TEST_ASSERT_FALSE(canResume(e, at(10 + 6 * 3600)));
    TEST_ASSERT_FALSE(resume(e, at(10 + 6 * 3600)));
    // Verified + refusal 6: no Resume (Resume exists only in Paused)
    Job v = inState(State::Verified);
    onUp(v, r6, at(100));
    TEST_ASSERT_FALSE(canResume(v, at(200)));
    TEST_ASSERT_FALSE(resume(v, at(200)));
    // Paused without a hold or a manual pause: no Resume
    Job p = inState(State::Paused);
    TEST_ASSERT_FALSE(canResume(p, at(100)));
}

// Part F (fix 1): the starved window is the 2 rounds before the leave (or the
// pause by missed statuses), starting no earlier than this Transfer's accept,
// at least one round long
static Up leave1(uint32_t t)
{
    Up l = up(at(t), false);
    l.left = true;
    l.leftArg = 1;
    return l;
}

static void test_starved_window_from_transfer(void)
{
    // (a) A re-accept at 5000 s (the old status at 846 s predates it), no
    // status, nothing launched, the leave 2 rounds later: starved
    Job a = inState(State::Transfer); // accepted at 10 s
    onUp(a, progress(at(846), 5, 0, 32), at(846));
    Up l3 = up(at(1000), false);
    l3.left = true;
    l3.leftArg = 3;
    onUp(a, l3, at(1000));
    onUp(a, accepted(at(5000), 1, 5, 0), at(5000));
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)a.state);
    onUp(a, leave1(5000 + 1720), at(5000 + 1720));
    TEST_ASSERT_EQUAL((int)Why::Starved, (int)a.why);
    TEST_ASSERT_TRUE(a.dlBackoff.armed());
    TEST_ASSERT_EQUAL(0, a.laps);

    // (b) Blocks only before the round-0 status, then nothing; the leave at
    // about accept + 2 rounds: the window before the leave is empty - starved
    Job b = inState(State::Transfer);
    for (int k = 0; k < 8; k++)
    {
        launched(b, true, (uint16_t)k, 1263, at(14 + k * 26));
    }
    onUp(b, progress(at(846), 0, 0xFF, 32), at(846));
    onUp(b, leave1(10 + 1716 + 300), at(10 + 1716 + 300));
    TEST_ASSERT_EQUAL((int)Why::Starved, (int)b.why);
    TEST_ASSERT_TRUE(b.dlBackoff.armed());

    // (c) Accept at 5000 s, its first status at 5836 s, nothing launched, the
    // leave at 7600 s: the 2 rounds before the leave lie inside the Transfer - starved
    Job c = inState(State::Paused);
    onUp(c, accepted(at(5000), 1, 5, 0), at(5000));
    onUp(c, progress(at(5836), 5, 0, 32), at(5836));
    Result r = onUp(c, leave1(7600), at(7600));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)c.state);
    TEST_ASSERT_EQUAL((int)Why::Starved, (int)c.why);
    TEST_ASSERT_EQUAL((int)Down::None, (int)r.down);

    // (d) A leave less than one round after the accept (e.g. an RTC fault):
    // the clamped window says nothing about the gateway - a real leave, a lap
    Job d = inState(State::Transfer); // accepted at 10 s
    r = onUp(d, leave1(500), at(500));
    TEST_ASSERT_EQUAL((int)Why::Left, (int)d.why);
    TEST_ASSERT_FALSE(d.dlBackoff.armed());
    TEST_ASSERT_EQUAL(1, d.laps);
    TEST_ASSERT_EQUAL((int)Down::Start, (int)r.down);

    // A full round inside the Transfer and nothing launched: starved
    Job k = inState(State::Transfer); // accepted at 10 s
    Up s1 = leave1(10 + 900);
    onUp(k, s1, at(10 + 900));
    TEST_ASSERT_EQUAL((int)Why::Starved, (int)k.why);
    TEST_ASSERT_TRUE(k.dlBackoff.armed());
    // (e) ... and with K/4 launches in that window it is a lap
    Job m = inState(State::Transfer);
    for (int b = 0; b < 8; b++)
    {
        launched(m, true, (uint16_t)b, 1263, at(500 + b * 26));
    }
    onUp(m, s1, at(10 + 900));
    TEST_ASSERT_EQUAL((int)Why::Left, (int)m.why);
    TEST_ASSERT_EQUAL(1, m.laps);
    // A start whose accepting UP was lost: the window starts at that start
    Job ad = inState(State::Transfer);
    Up b0 = up(at(1800), false);
    b0.fcnt32 = 120;
    onUp(ad, b0, at(1800)); // bit 4 = 0: Paused + start
    startSent(ad, 120, 0, at(1800));
    Up p = progress(at(2700), 8, 0, 32);
    p.fcnt32 = 122;
    TEST_ASSERT_TRUE(onUp(ad, p, at(2700)).adopted);
    TEST_ASSERT_EQUAL_UINT32(at(1800).uptimeS, ad.transferUptimeS);
}

// Part F (fix 2): the window never reaches into a Paused period - every
// (re)entry into Transfer starts it; a pause by misses is accounted once
static void test_starved_after_return(void)
{
    // Gateway reboot: Paused (Reboot), the RAM fields gone; a progress status
    // brings the job back, a quick "left 1" after it is a real leave
    Job j = inState(State::Transfer); // accepted at 10 s
    onUp(j, progress(at(900), 5, 0, 32), at(900));
    afterReboot(j, at(4000));
    j.transferUptimeS = j.startUptimeS = j.pausedUptimeS = 0;
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    onUp(j, progress(at(5000), 5, 0x1, 32 + 5 * 33), at(5000));
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)j.state);
    onUp(j, leave1(5100), at(5100));
    TEST_ASSERT_EQUAL((int)Why::Left, (int)j.why);
    TEST_ASSERT_FALSE(j.dlBackoff.armed());
    // Paused by missed statuses, a long Paused period, back by a status of
    // the same session, then a quick "left 1": a real leave
    Job m = inState(State::Transfer);
    tick(m, at(1708));
    TEST_ASSERT_EQUAL((int)Why::Missed, (int)m.why);
    onUp(m, progress(at(9000), 5, 0, 32 + 10 * 33), at(9000));
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)m.state);
    onUp(m, leave1(9100), at(9100));
    TEST_ASSERT_EQUAL((int)Why::Left, (int)m.why);
    TEST_ASSERT_FALSE(m.dlBackoff.armed());
}

// Part F (fix 2): "left 1" after a pause by missed statuses - the window ends at the pause
static void test_starved_paused_by_misses(void)
{
    // 8 launches in the 2 rounds before the pause: a real leave, a lap, a start
    Job j = inState(State::Transfer); // accepted at 10 s
    for (int b = 0; b < 8; b++)
    {
        launched(j, true, (uint16_t)b, 1263, at(1400 + b * 26));
    }
    tick(j, at(1708));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)j.state);
    TEST_ASSERT_TRUE(j.pausedByMisses);
    Result r = onUp(j, leave1(3500), at(3500));
    TEST_ASSERT_EQUAL((int)Why::Missed, (int)j.why);
    TEST_ASSERT_EQUAL(1, j.laps);
    TEST_ASSERT_FALSE(j.dlBackoff.armed());
    TEST_ASSERT_EQUAL((int)Down::Start, (int)r.down);
    TEST_ASSERT_FALSE(j.pausedByMisses); // accounted
    // A second leave without a new Transfer is not classified again
    onUp(j, leave1(4500), at(4500));
    TEST_ASSERT_EQUAL(1, j.laps);
    // Nothing launched: starved
    Job k = inState(State::Transfer);
    tick(k, at(1708));
    r = onUp(k, leave1(3500), at(3500));
    TEST_ASSERT_EQUAL((int)Why::Starved, (int)k.why);
    TEST_ASSERT_TRUE(k.dlBackoff.armed());
    TEST_ASSERT_EQUAL((int)Down::None, (int)r.down);
    const uint32_t backoff = k.backoffS;
    onUp(k, leave1(3600), at(3600));
    TEST_ASSERT_EQUAL_UINT32(backoff, k.backoffS); // not doubled by a second leave
}

// Part F: Done shows the whole image confirmed
static void test_done_counters(void)
{
    Job j = inState(State::Transfer);
    onUp(j, progress(at(846), 391, 0, 32), at(846));
    TEST_ASSERT_EQUAL(391, j.maxConfirmed);
    Up c = up(at(1000), false);
    c.complete = true;
    onUp(j, c, at(1000));
    onUp(j, joinReq(at(1300), NEW_FW), at(1300));
    onUp(j, status(at(1900), NEW_FW, 2, 0), at(1900));
    TEST_ASSERT_EQUAL((int)State::Done, (int)j.state);
    TEST_ASSERT_EQUAL(j.blocks(), j.maxConfirmed);
    TEST_ASSERT_EQUAL_UINT32(0, etaS(j));
    // ... also installed despite abort
    Job a = inState(State::Verified);
    abort(a, at(20));
    onUp(a, status(at(600), NEW_FW, 1, 0x08), at(600));
    TEST_ASSERT_EQUAL((int)State::Done, (int)a.state);
    TEST_ASSERT_EQUAL(a.blocks(), a.maxConfirmed);
}

// Part F: an abort in Paused that ends with the sensor out of its session
// leaves its START record - reported, not claimed as discarded
static void test_abort_out_of_session(void)
{
    Job j = inState(State::Paused);
    abort(j, at(30));
    TEST_ASSERT_TRUE(j.abortQuiet);
    Result r = onUp(j, up(at(40), false), at(40));
    TEST_ASSERT_TRUE(r.abortKept);
    TEST_ASSERT_TRUE(j.abortKept);
    TEST_ASSERT_FALSE(j.abortPending);
    // In its session (Paused by missed statuses): the end abort goes out, nothing kept
    Job m = inState(State::Transfer);
    tick(m, at(2000));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)m.state);
    abort(m, at(2010));
    TEST_ASSERT_EQUAL((int)Down::EndAbort, (int)onUp(m, up(at(2100), true), at(2100)).down);
    TEST_ASSERT_FALSE(m.abortQuiet);
    r = onUp(m, up(at(2200), false), at(2200));
    TEST_ASSERT_FALSE(r.abortKept);
    TEST_ASSERT_FALSE(m.abortKept);
    // "left 3" ends it: the sensor received an OTA end
    Job e = inState(State::Paused);
    abort(e, at(30));
    Up l3 = up(at(40), false);
    l3.left = true;
    l3.leftArg = 3;
    TEST_ASSERT_FALSE(onUp(e, l3, at(40)).abortKept);
    // Transfer: an UP with bit 4 = 0 ends it without "left 3" (the end abort of
    // the block slot did not arrive): kept
    Job t = inState(State::Transfer);
    abort(t, at(20));
    TEST_ASSERT_TRUE(t.abortQuiet);
    TEST_ASSERT_TRUE(onUp(t, up(at(900), false), at(900)).abortKept);
    // Verified: the flag expires before the sensor's next report - kept
    Job v = inState(State::Verified);
    abort(v, at(20));
    TEST_ASSERT_TRUE(v.abortQuiet);
    TEST_ASSERT_TRUE(tick(v, at(20 + 2 * 858)));
    TEST_ASSERT_FALSE(v.abortPending);
    TEST_ASSERT_TRUE(v.abortKept);
    TEST_ASSERT_FALSE(v.abortQuiet);
    // ... an in-session UP got the end abort first: expiry claims nothing
    Job w = inState(State::Verified);
    abort(w, at(20));
    Up bl = up(at(100), true);
    bl.batteryLow = true;
    TEST_ASSERT_EQUAL((int)Down::EndAbort, (int)onUp(w, bl, at(100)).down);
    tick(w, at(20 + 2 * 858));
    TEST_ASSERT_FALSE(w.abortKept);
    // Waiting: no transfer of this job on the sensor - never kept
    Job q = inState(State::Waiting);
    abort(q, at(20));
    TEST_ASSERT_FALSE(q.abortQuiet);
    TEST_ASSERT_FALSE(onUp(q, up(at(100), false), at(100)).abortKept);
    // ... unless a start went out (its accept may be lost): unconfirmed
    Job qs = inState(State::Waiting);
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(qs, up(at(50), false), at(50)).down);
    startSent(qs, 100, 0, at(50));
    abort(qs, at(60));
    TEST_ASSERT_TRUE(qs.abortQuiet);
    TEST_ASSERT_TRUE(onUp(qs, up(at(900), false), at(900)).abortKept);
    // A new job starts clean
    begin(j, target(), image(), at(100));
    TEST_ASSERT_FALSE(j.abortKept);
    TEST_ASSERT_FALSE(j.abortQuiet);
}

static void test_eta(void)
{
    Job j = inState(State::Transfer);
    // 397 blocks x 26 s x 33/32, no repeats: ~10,644 s
    uint32_t e = etaS(j);
    TEST_ASSERT_TRUE(e > 10500 && e < 10800);
    // 20 % repeats -> x 1.25; 20 % skipped slots -> x 1.25 again
    j.blocksSent = 100;
    j.repeats = 20;
    uint32_t r = etaS(j);
    TEST_ASSERT_TRUE(r > e * 124 / 100 && r < e * 126 / 100);
    j.skipped = 25; // 25 of 125 slots
    uint32_t s = etaS(j);
    TEST_ASSERT_TRUE(s > r * 124 / 100 && s < r * 126 / 100);
}

// A start out of Paused, accept lost, gateway reboot (startUptimeS is RAM only), the
// progress status adopts the start after the reboot: the window must not reach before the re-entry
static Now rb(uint32_t up)
{
    Now n;
    n.unixS = 1800100000u + up;
    n.uptimeS = up;
    n.us = (int64_t)up * 1000000;
    return n;
}
static void test_starved_adopted_after_reboot(void)
{
    Job ad = inState(State::Transfer);
    Up b0 = up(at(1800), false);
    b0.fcnt32 = 120;
    onUp(ad, b0, at(1800)); // bit 4 = 0: Paused + start
    startSent(ad, 120, 0, at(1800));
    TEST_ASSERT_TRUE(ad.startValid);
    // reboot 30 s later: uptime restarts, the RAM-only fields are gone
    ad.transferUptimeS = ad.startUptimeS = ad.pausedUptimeS = 0; // a fresh process: restored from job.json
    afterReboot(ad, rb(5));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)ad.state);
    Up p = progress(rb(900), 8, 0, 32);
    p.fcnt32 = 122;
    p.rxUs = rb(900).us;
    TEST_ASSERT_TRUE(onUp(ad, p, rb(900)).adopted);
    TEST_ASSERT_EQUAL((int)State::Transfer, (int)ad.state);
    // a "left 1" 60 s after the re-entry: the clamped window is shorter than one round -> Left
    Up l = up(rb(960), false);
    l.left = true;
    l.leftArg = 1;
    l.rxUs = rb(960).us;
    Result r = onUp(ad, l, rb(960));
    TEST_ASSERT_EQUAL((int)Why::Left, (int)ad.why);
    TEST_ASSERT_FALSE(ad.dlBackoff.armed());
    TEST_ASSERT_EQUAL((int)Down::Start, (int)r.down);
}

// "Signed images only" with an unsigned image: an unfinished job is held like
// a manual pause (switched on, restored after a restart); later states go on
static void test_hold_unsigned(void)
{
    // Waiting, no start sent yet: held, no start until a resume, a quiet abort
    Job w = inState(State::Waiting);
    TEST_ASSERT_TRUE(holdUnsigned(w, at(5)));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)w.state);
    TEST_ASSERT_EQUAL((int)Why::Unsigned, (int)w.why);
    TEST_ASSERT_TRUE(w.manualPause);
    TEST_ASSERT_FALSE(holdUnsigned(w, at(6))); // already held
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(w, up(at(30), false), at(30)).down);
    TEST_ASSERT_TRUE(canResume(w, at(31)));
    Job wa = w;
    TEST_ASSERT_TRUE(abort(wa, at(32)));
    TEST_ASSERT_FALSE(wa.abortQuiet); // nothing of this job on the sensor
    // ... a resume lets the start go out with the next report
    TEST_ASSERT_TRUE(resume(w, at(40)));
    TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(w, up(at(50), false), at(50)).down);

    // Transfer: an OTA end pause goes out, the sensor's progress is answered with it
    Job t = inState(State::Transfer);
    TEST_ASSERT_TRUE(holdUnsigned(t, at(20)));
    TEST_ASSERT_EQUAL((int)State::Paused, (int)t.state);
    TEST_ASSERT_TRUE(t.pendingEnd == PendingEnd::Pause);
    TEST_ASSERT_EQUAL((int)Down::EndPause, (int)onUp(t, progress(at(846), 0, 0, 32), at(846)).down);
    Job ta = t;
    TEST_ASSERT_TRUE(abort(ta, at(850)));
    TEST_ASSERT_TRUE(ta.abortQuiet); // the sensor holds a transfer state

    // Restored after a restart: Transfer -> Paused (reboot) would start again by
    // itself; held, it does not
    Job r = inState(State::Transfer);
    afterReboot(r, at(100));
    TEST_ASSERT_EQUAL((int)Why::Reboot, (int)r.why);
    TEST_ASSERT_TRUE(holdUnsigned(r, at(100)));
    TEST_ASSERT_EQUAL((int)Why::Unsigned, (int)r.why);
    TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(r, up(at(700), false), at(700)).down);
    // A battery refusal keeps the reason (paused by the rule, not by the battery)
    Up low = up(at(1300), false);
    low.refused = true;
    low.refusedArg = ep::ota::kRefuseBattery;
    onUp(r, low, at(1300));
    TEST_ASSERT_EQUAL((int)Why::Unsigned, (int)r.why);

    // Paused (left) without a hold: held as well
    Job p = inState(State::Paused);
    TEST_ASSERT_FALSE(p.manualPause);
    TEST_ASSERT_TRUE(holdUnsigned(p, at(30)));
    TEST_ASSERT_TRUE(p.manualPause);

    // The image is complete on the sensor: not stopped
    for (State s : {State::Verified, State::Swapping, State::Confirming, State::Aborted, State::None})
    {
        Job v;
        if (s == State::Aborted)
        {
            v = inState(State::Waiting);
            abort(v, at(20));
        }
        else if (s != State::None)
        {
            v = inState(s);
        }
        const State before = v.state;
        TEST_ASSERT_FALSE(holdUnsigned(v, at(30)));
        TEST_ASSERT_EQUAL((int)before, (int)v.state);
    }
    TEST_ASSERT_EQUAL_STRING("unsigned image - \"Signed images only\" is on", whyText(Why::Unsigned));
}

// S3 item 0: a Verified job does not transfer again an unsigned image while
// "Signed images only" is on (the sensor lost the image, or it left and would
// get a new start); held like holdUnsigned(). A hold keeps its reason
static void test_hold_verified_unsigned(void)
{
    // Re-accept with less than the whole image: held, the sensor is sent away
    for (int variant = 0; variant < 3; variant++)
    {
        Job v = inState(State::Verified);
        TEST_ASSERT_EQUAL((int)State::Verified, (int)v.state);
        v.imageVerified = variant == 1;
        Up a = accepted(at(100), 1, 5, 0);
        a.signedOnly = variant != 2;
        Result r = onUp(v, a, at(100));
        if (variant == 0)
        {
            TEST_ASSERT_EQUAL((int)State::Paused, (int)v.state);
            TEST_ASSERT_EQUAL((int)Why::Unsigned, (int)v.why);
            TEST_ASSERT_TRUE(v.manualPause);
            TEST_ASSERT_EQUAL((int)Down::EndPause, (int)r.down);
            TEST_ASSERT_TRUE(r.log);
            // Resume refused by the caller while the option is on; after a resume
            // (option off) the transfer goes on
            TEST_ASSERT_TRUE(canResume(v, at(110)));
            TEST_ASSERT_TRUE(resume(v, at(110)));
            TEST_ASSERT_EQUAL((int)State::Transfer, (int)(onUp(v, accepted(at(120), 1, 5, 0), at(120)), v.state));
        }
        else
        {
            // verified image, or the option off: transfers again as before
            TEST_ASSERT_EQUAL((int)State::Transfer, (int)v.state);
            TEST_ASSERT_FALSE(v.manualPause);
        }
    }
    // Accepted with the whole image: stays Verified (nothing to transfer)
    {
        Job v = inState(State::Verified);
        Up a = accepted(at(100), 1, (uint16_t)v.blocks(), 0);
        a.signedOnly = true;
        onUp(v, a, at(100));
        TEST_ASSERT_EQUAL((int)State::Verified, (int)v.state);
    }
    // "left 1" and an UP without bit 4: no new start, held
    for (int kind = 0; kind < 2; kind++)
    {
        Job v = inState(State::Verified);
        Up u = kind == 0 ? leave1(200) : up(at(200), false);
        u.signedOnly = true;
        Result r = onUp(v, u, at(200));
        TEST_ASSERT_EQUAL((int)Down::None, (int)r.down);
        TEST_ASSERT_EQUAL((int)State::Paused, (int)v.state);
        TEST_ASSERT_EQUAL((int)Why::Unsigned, (int)v.why);
        // ... and stays quiet
        Up again = up(at(300), false);
        again.signedOnly = true;
        TEST_ASSERT_EQUAL((int)Down::None, (int)onUp(v, again, at(300)).down);
        // The same without the option: the start goes out
        Job w = inState(State::Verified);
        Up u2 = kind == 0 ? leave1(200) : up(at(200), false);
        TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(w, u2, at(200)).down);
        TEST_ASSERT_EQUAL((int)State::Verified, (int)w.state);
        // A verified image: the start goes out with the option on
        Job x = inState(State::Verified);
        x.imageVerified = true;
        Up u3 = kind == 0 ? leave1(200) : up(at(200), false);
        u3.signedOnly = true;
        TEST_ASSERT_EQUAL((int)Down::Start, (int)onUp(x, u3, at(200)).down);
    }

    // A hold keeps its reason: a job paused by missed statuses, then held, then
    // a "left 1" that would classify as starved
    Job m = inState(State::Transfer);
    tick(m, at(1708));
    TEST_ASSERT_TRUE(m.pausedByMisses);
    TEST_ASSERT_TRUE(holdUnsigned(m, at(1710)));
    TEST_ASSERT_FALSE(m.pausedByMisses);
    onUp(m, leave1(3500), at(3500));
    TEST_ASSERT_EQUAL((int)Why::Unsigned, (int)m.why);
    // ... also if the flag were still set (defence in depth)
    m.pausedByMisses = true;
    m.pausedUptimeS = at(3600).uptimeS;
    Result r = onUp(m, leave1(3700), at(3700));
    TEST_ASSERT_EQUAL((int)Why::Unsigned, (int)m.why);
    TEST_ASSERT_EQUAL((int)Down::None, (int)r.down);
}

// The confirmation token: bound to the reasons and the image, never 0
static void test_confirm_token(void)
{
    const char *a = "Reinstall of the running version v0.2.12. ";
    const char *b = "Reinstall of the running version v0.2.12. Unsigned image. ";
    const uint32_t ta = startConfirmToken(a, strlen(a), 0x11223344u);
    TEST_ASSERT_EQUAL_UINT32(ta, startConfirmToken(a, strlen(a), 0x11223344u));
    TEST_ASSERT_NOT_EQUAL(ta, startConfirmToken(b, strlen(b), 0x11223344u)); // a reason more
    TEST_ASSERT_NOT_EQUAL(ta, startConfirmToken(a, strlen(a), 0x11223345u)); // another image
    TEST_ASSERT_NOT_EQUAL(0u, ta);
    TEST_ASSERT_NOT_EQUAL(0u, startConfirmToken("", 0, 0));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_matrix);
    RUN_TEST(test_waiting_details);
    RUN_TEST(test_refusal_rules);
    RUN_TEST(test_no_progress_and_laps);
    RUN_TEST(test_starved_backoff);
    RUN_TEST(test_missed_statuses);
    RUN_TEST(test_anchor_from_status);
    RUN_TEST(test_anchor_bound);
    RUN_TEST(test_block_choice);
    RUN_TEST(test_block_slots);
    RUN_TEST(test_abort_delivery_and_orphans);
    RUN_TEST(test_manual_pause);
    RUN_TEST(test_deadlines_and_reboot);
    RUN_TEST(test_revert_and_reinstall);
    RUN_TEST(test_verified_paths);
    RUN_TEST(test_lost_accept_late_phase);
    RUN_TEST(test_new_session_lost_accept);
    RUN_TEST(test_resume_and_stale_pause);
    RUN_TEST(test_shrink_and_cycle);
    RUN_TEST(test_watch_after_flagged_baseline);
    RUN_TEST(test_same_session_lost_accept);
    RUN_TEST(test_waiting_deadline_arming);
    RUN_TEST(test_oscillating_base);
    RUN_TEST(test_battery_leave);
    RUN_TEST(test_resume_battery_hold);
    RUN_TEST(test_starved_window_from_transfer);
    RUN_TEST(test_starved_after_return);
    RUN_TEST(test_starved_adopted_after_reboot);
    RUN_TEST(test_starved_paused_by_misses);
    RUN_TEST(test_done_counters);
    RUN_TEST(test_abort_out_of_session);
    RUN_TEST(test_eta);
    RUN_TEST(test_hold_unsigned);
    RUN_TEST(test_hold_verified_unsigned);
    RUN_TEST(test_confirm_token);
    return UNITY_END();
}
