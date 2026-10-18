/**
 * Native unit tests for the gateway's generation 2 downlink rules (window
 * choice, duty-cycle budget, pacing) and the session pair (dual session
 * after a join, counter handling). docs/protocol-v2.md sections 5, 7, 9.
 * Run:   pio test -e native
 */

#include <unity.h>
#include <cstring>
#include "ExploraProtocol.h"
#include "../../src/Protocol/Gen2Downlink.h"
#include "../../src/Protocol/Gen2Config.h"
#include "../../src/Protocol/Gen2Session.h"
#include "../../src/Data/DueTime.h"

using namespace Gen2Downlink;

void setUp(void) {}
void tearDown(void) {}

static const int64_t OFF = 6400; // SX1262

static void test_window_choice(void)
{
    const int64_t rx = 10000000;
    TEST_ASSERT_EQUAL_INT64(rx + 1000000 - OFF, launchTime(rx, Window::Rx1, OFF));
    TEST_ASSERT_EQUAL_INT64(rx + 2000000 - OFF, launchTime(rx, Window::Rx2, OFF));
    // Decision at launch - 20 ms: RxDone + 1000 ms - C_chip - 20 ms
    TEST_ASSERT_EQUAL_INT64(rx + 1000000 - OFF - 20000, decisionTime(rx, Window::Rx1, OFF));
    TEST_ASSERT_EQUAL_INT64(rx + 2000000 - OFF - 20000, decisionTime(rx, Window::Rx2, OFF));
    TEST_ASSERT_EQUAL_INT64(decisionTime(rx, Window::Rx1, OFF) + PREPARE_LEAD_US, launchTime(rx, Window::Rx1, OFF));
    // SX1276 offset
    TEST_ASSERT_EQUAL_INT64(rx + 1000000 - 5900 - 20000, decisionTime(rx, Window::Rx1, 5900));
}

static void test_airtime_window_slides(void)
{
    AirtimeWindow w;
    w.record(0, 1000);
    w.record(30000, 500);
    TEST_ASSERT_EQUAL_UINT32(1500, w.usedMs(59000));
    TEST_ASSERT_EQUAL_UINT32(1500, w.usedMs(3599000)); // still within the hour
    TEST_ASSERT_EQUAL_UINT32(0, w.usedMs(3600000));    // first minute bucket dropped
    w.record(3600000, 200);
    TEST_ASSERT_EQUAL_UINT32(200, w.usedMs(3601000));
    TEST_ASSERT_EQUAL_UINT32(0, w.usedMs(3600000 + 2 * 3600000)); // long silence
}

static void test_airtime_window_non_monotonic(void)
{
    AirtimeWindow w;
    w.record(119990, 400);                              // minute 1
    TEST_ASSERT_EQUAL_UINT32(400, w.usedMs(120010));    // check() ~20 ms ahead: minute 2
    TEST_ASSERT_EQUAL_UINT32(400, w.usedMs(119995));    // a later call with an earlier time
    TEST_ASSERT_EQUAL_UINT32(400, w.usedMs(119000));
    // millis() wrap: the window keeps counting across it
    AirtimeWindow x;
    x.record(0xFFFFFFFFu - 30000, 700);
    TEST_ASSERT_EQUAL_UINT32(700, x.usedMs(20000));     // ~50 s later, after the wrap
    TEST_ASSERT_EQUAL_UINT32(700, x.usedMs(3000000));
    TEST_ASSERT_EQUAL_UINT32(0, x.usedMs(3700000));     // an hour after the record
}

static void test_due_time(void)
{
    TEST_ASSERT_FALSE(DueTime::reached(1000, 3500));    // before
    TEST_ASSERT_TRUE(DueTime::reached(3500, 3500));     // at
    TEST_ASSERT_TRUE(DueTime::reached(3600, 3500));     // after
    TEST_ASSERT_FALSE(DueTime::reached(0xFFFFFF00u, 100)); // due after the wrap: not yet
    TEST_ASSERT_TRUE(DueTime::reached(200, 0xFFFFFF00u));  // due before the wrap: reached
    // A retry 60 s in the future is not due right away (M1)
    uint32_t now = 5000;
    TEST_ASSERT_FALSE(DueTime::reached(now + 1, now + 60000));
}

static void test_budget_pacing(void)
{
    Budget b;
    TEST_ASSERT_EQUAL((int)Verdict::Ok, (int)b.check(1000, 200, false));
    b.record(1200, 200, false); // ended at 1200 ms, 200 ms on air
    // Off-time >= 9 x 200 ms after the end
    TEST_ASSERT_EQUAL((int)Verdict::Pacing, (int)b.check(1200 + 1799, 200, false));
    TEST_ASSERT_EQUAL((int)Verdict::Ok, (int)b.check(1200 + 1800, 200, false));
}

static void test_budget_hourly_and_join_share(void)
{
    Budget b;
    uint32_t t = 0;
    // 72 s of JOIN_ACC airtime: the join share is full, other frames still fit
    for (int i = 0; i < 288; i++)
    {
        b.record(t, 250, true);
        t += 2500 + 250 * 9; // respect pacing
    }
    TEST_ASSERT_EQUAL_UINT32(72000, b.joinUsedMs(t));
    TEST_ASSERT_EQUAL((int)Verdict::JoinBudget, (int)b.check(t, 250, true));
    TEST_ASSERT_EQUAL((int)Verdict::Ok, (int)b.check(t, 250, false));
}

static void test_budget_full_hour(void)
{
    Budget b;
    // 4.8 s of data downlinks per minute for an hour = 288 s = the data share
    for (uint32_t minute = 0; minute < 60; minute++)
    {
        b.record(minute * 60000, 4800, false);
    }
    uint32_t now = 59 * 60000 + 10 * 4800; // pacing satisfied, still in minute 59
    TEST_ASSERT_TRUE(b.warn(now));         // 80 % of the 360 s
    TEST_ASSERT_EQUAL((int)Verdict::Budget, (int)b.check(now, 100, false));
    TEST_ASSERT_EQUAL((int)Verdict::Ok, (int)b.check(now, 250, true)); // JOIN_ACC still has its share
    // One minute later the oldest bucket has left the window
    TEST_ASSERT_EQUAL((int)Verdict::Ok, (int)b.check(now + 60000, 100, false));
}

// --- configuration ----------------------------------------------------------

static void test_config_version_and_clamp(void)
{
    TEST_ASSERT_EQUAL_UINT8(2, Gen2Config::nextVersion(1, 1));
    TEST_ASSERT_EQUAL_UINT8(1, Gen2Config::nextVersion(15, 0)); // never 0 (sensor revert marker)
    // Never the version the sensor reports (offline sensor, 15 changes)
    TEST_ASSERT_EQUAL_UINT8(4, Gen2Config::nextVersion(2, 3));
    TEST_ASSERT_EQUAL_UINT8(2, Gen2Config::nextVersion(15, 1));
    TEST_ASSERT_EQUAL_UINT8(5, Gen2Config::nextVersion(4, 0x80 | 3)); // FCtrl bits masked
    uint8_t ver = 3, reported = 3;
    for (int i = 0; i < 40; i++)
    {
        ver = Gen2Config::nextVersion(ver, reported);
        TEST_ASSERT_TRUE(ver != reported && Gen2Config::validVersion(ver));
    }
    TEST_ASSERT_FALSE(Gen2Config::validVersion(0));
    TEST_ASSERT_TRUE(Gen2Config::inSync(0x80 | 3, 3)); // only the low 4 bits of FCtrl
    TEST_ASSERT_FALSE(Gen2Config::inSync(0, 3));
    Gen2Config::Values v;
    TEST_ASSERT_TRUE(Gen2Config::clamp(v)); // defaults are valid
    v.txPowerDbm = 22;                      // regulatory clamp
    v.intervalS = 10;
    v.slotS = 100;
    v.ackBackoffS = 0;
    TEST_ASSERT_FALSE(Gen2Config::clamp(v));
    TEST_ASSERT_EQUAL_INT8(14, v.txPowerDbm);
    TEST_ASSERT_EQUAL_UINT16(60, v.intervalS);
    TEST_ASSERT_EQUAL_UINT16(40, v.slotS);
    TEST_ASSERT_EQUAL_UINT8(1, v.ackBackoffS);
}

static void test_down_payload(void)
{
    Gen2Config::Values v;
    uint8_t out[64];
    bool more = true;
    // ACK only: time TLV
    Gen2Config::Queue q;
    size_t n = Gen2Config::buildDown(v, 1760000000, q, out, sizeof(out), more);
    TEST_ASSERT_EQUAL_UINT32(5, n);
    TEST_ASSERT_FALSE(more);
    // Config + status request fit one frame
    q.config = true;
    q.statusRequest = true;
    n = Gen2Config::buildDown(v, 1760000000, q, out, sizeof(out), more);
    TEST_ASSERT_EQUAL_UINT32(5 + 5 + 2 + 3 + 2, n);
    TEST_ASSERT_FALSE(more);
    TEST_ASSERT_FALSE(q.config);
    ep::TlvReader r(out, n);
    ep::Tlv t;
    int count = 0;
    while (r.next(t) == 1)
    {
        count++;
        if (t.type == ep::down::kInterval)
        {
            TEST_ASSERT_EQUAL_UINT32(600u << 16, ep::tlvU32(t));
        }
    }
    TEST_ASSERT_EQUAL_INT(5, count);
    // Small frame: config does not fit next to the time -> more pending
    q = Gen2Config::Queue();
    q.config = true;
    q.statusRequest = true;
    n = Gen2Config::buildDown(v, 1760000000, q, out, 12, more);
    TEST_ASSERT_TRUE(more);
    TEST_ASSERT_TRUE(q.config);
    TEST_ASSERT_FALSE(q.statusRequest); // the 2-byte command still fitted
    TEST_ASSERT_EQUAL_UINT32(7, n);
    // No valid time: no time TLV
    q = Gen2Config::Queue();
    n = Gen2Config::buildDown(v, 0, q, out, sizeof(out), more);
    TEST_ASSERT_EQUAL_UINT32(0, n);
}

static void test_status_boot_info(void)
{
    uint8_t st[24] = {1};
    st[22] = 2;
    st[23] = 0x05;
    uint8_t bs = 0, bl = 0;
    Gen2Config::statusBootInfo(st, 24, bs, bl);
    TEST_ASSERT_EQUAL_UINT8(2, bs);
    TEST_ASSERT_EQUAL_UINT8(0x05, bl);
    // a 22-byte report (older firmware) clears stale values
    Gen2Config::statusBootInfo(st, 22, bs, bl);
    TEST_ASSERT_EQUAL_UINT8(0xFF, bs);
    TEST_ASSERT_EQUAL_UINT8(0, bl);
    st[0] = 2; // unknown format
    Gen2Config::statusBootInfo(st, 24, bs, bl);
    TEST_ASSERT_EQUAL_UINT8(0xFF, bs);
}

// --- sessions -------------------------------------------------------------

static ep::KeyPair keys(uint8_t seed)
{
    ep::KeyPair k;
    for (int i = 0; i < 16; i++)
    {
        k.enc[i] = (uint8_t)(seed + i);
        k.mic[i] = (uint8_t)(seed * 3 + i);
    }
    return k;
}

static size_t up(const ep::KeyPair &k, uint32_t fcnt, uint8_t *f)
{
    const uint8_t p[3] = {0x46, 0x0c, 0x12};
    return ep::buildData(ep::aesSoftEncrypt, k, ep::FrameType::Up, 0x3EA4191D, fcnt, 0, 0, p, sizeof(p), f, 64);
}

static void test_session_dual_until_first_up(void)
{
    Gen2SessionPair s;
    uint8_t f[64], p[64];
    size_t pl;
    uint32_t fc;
    s.startPending(keys(1), 7);
    s.current = s.pending; // pretend joined earlier with keys(1)
    s.pending.clear();
    TEST_ASSERT_EQUAL((int)SessionUse::Current, (int)s.openUp(ep::aesSoftEncrypt, f, up(keys(1), 0, f), p, pl, fc));
    // New join: pending keys(2), the old session still verifies
    s.startPending(keys(2), 8);
    TEST_ASSERT_EQUAL((int)SessionUse::Current, (int)s.openUp(ep::aesSoftEncrypt, f, up(keys(1), 1, f), p, pl, fc));
    TEST_ASSERT_TRUE(s.pending.valid);
    // First UP under the new keys: promoted, old session gone
    TEST_ASSERT_EQUAL((int)SessionUse::Promoted, (int)s.openUp(ep::aesSoftEncrypt, f, up(keys(2), 0, f), p, pl, fc));
    TEST_ASSERT_FALSE(s.pending.valid);
    TEST_ASSERT_EQUAL_UINT32(8, s.current.devNonce);
    TEST_ASSERT_EQUAL((int)SessionUse::None, (int)s.openUp(ep::aesSoftEncrypt, f, up(keys(1), 2, f), p, pl, fc));
    TEST_ASSERT_EQUAL_UINT32(3, pl);
}

static void test_session_counters_and_replay(void)
{
    Gen2SessionPair s;
    uint8_t f[64], f5[64], p[64];
    size_t pl;
    uint32_t fc;
    s.startPending(keys(4), 1);
    size_t n5 = up(keys(4), 5, f5);
    TEST_ASSERT_EQUAL((int)SessionUse::Promoted, (int)s.openUp(ep::aesSoftEncrypt, f5, n5, p, pl, fc));
    TEST_ASSERT_EQUAL_UINT32(5, fc);
    // Replay of FCnt 5 rejected, the counter did not move
    TEST_ASSERT_EQUAL((int)SessionUse::None, (int)s.openUp(ep::aesSoftEncrypt, f5, n5, p, pl, fc));
    TEST_ASSERT_EQUAL_UINT32(5, s.current.fcntUp);
    // A forged frame with a higher counter does not move it either
    size_t n = up(keys(9), 9, f);
    TEST_ASSERT_EQUAL((int)SessionUse::None, (int)s.openUp(ep::aesSoftEncrypt, f, n, p, pl, fc));
    TEST_ASSERT_EQUAL_UINT32(5, s.current.fcntUp);
    TEST_ASSERT_EQUAL((int)SessionUse::Current, (int)s.openUp(ep::aesSoftEncrypt, f, up(keys(4), 6, f), p, pl, fc));
    // Not an UP (a DOWN with the same keys) is never accepted
    uint8_t d[64];
    size_t dn = ep::buildData(ep::aesSoftEncrypt, keys(4), ep::FrameType::Down, 0x3EA4191D, 7, 6, 0, nullptr, 0, d, 64);
    TEST_ASSERT_EQUAL((int)SessionUse::None, (int)s.openUp(ep::aesSoftEncrypt, d, dn, p, pl, fc));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_window_choice);
    RUN_TEST(test_airtime_window_slides);
    RUN_TEST(test_airtime_window_non_monotonic);
    RUN_TEST(test_due_time);
    RUN_TEST(test_budget_pacing);
    RUN_TEST(test_budget_hourly_and_join_share);
    RUN_TEST(test_budget_full_hour);
    RUN_TEST(test_config_version_and_clamp);
    RUN_TEST(test_down_payload);
    RUN_TEST(test_status_boot_info);
    RUN_TEST(test_session_dual_until_first_up);
    RUN_TEST(test_session_counters_and_replay);
    return UNITY_END();
}
