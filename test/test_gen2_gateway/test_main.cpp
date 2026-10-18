/**
 * Native unit tests for the gateway's generation 2 dispatch decision and the
 * "waiting to be added" list (docs/protocol-v2.md sections 5.1 and 6).
 * Run:   pio test -e native
 */

#include <unity.h>
#include <cstring>
#include "ExploraProtocol.h"
#include "../../src/Protocol/Gen2Dispatch.h"
#include "../../src/Protocol/WaitingList.h"
#include "../test_protocol_v2/vectors.h"

using Gen2Dispatch::Route;

void setUp(void) {}
void tearDown(void) {}

static Route route(const uint8_t *f, size_t len, uint32_t knownId)
{
    ep::FrameInfo info;
    return Gen2Dispatch::classify(f, len, [knownId](uint32_t id) { return id == knownId; }, info);
}

static void test_dispatch_known_and_unknown(void)
{
    TEST_ASSERT_EQUAL((int)Route::Gen2Known, (int)route(kVecJoinReq, sizeof(kVecJoinReq), kVecDevId));
    TEST_ASSERT_EQUAL((int)Route::JoinUnknown, (int)route(kVecJoinReq, sizeof(kVecJoinReq), 0x12345678));
    // UP of a known DevID goes to gen2 even without a session (REJOIN in M3)
    TEST_ASSERT_EQUAL((int)Route::Gen2Known, (int)route(kVecUp, sizeof(kVecUp), kVecDevId));
    // UP of an unknown DevID: not ours, unchanged gen1 path
    TEST_ASSERT_EQUAL((int)Route::Gen1, (int)route(kVecUp, sizeof(kVecUp), 0x12345678));
}

static void test_dispatch_gen1_frames(void)
{
    // A gen1 packet (random byte 0, no gen2 marker)
    const uint8_t gen1[13] = {0x13, 0x05, 0x53, 0x55, 0x78, 0x0c, 0x12, 0x02, 0x08, 0x5c, 0x17, 0x44, 0x00};
    TEST_ASSERT_EQUAL((int)Route::Gen1, (int)route(gen1, sizeof(gen1), kVecDevId));
    // Marker present but wrong protocol version
    uint8_t f[sizeof(kVecJoinReq)];
    memcpy(f, kVecJoinReq, sizeof(f));
    f[0] = (uint8_t)((f[0] & ~0x07) | 3);
    TEST_ASSERT_EQUAL((int)Route::Gen1, (int)route(f, sizeof(f), kVecDevId));
    // JOIN_REQ with a wrong length or magic is not a JOIN_REQ
    TEST_ASSERT_EQUAL((int)Route::Gen1, (int)route(kVecJoinReq, sizeof(kVecJoinReq) - 1, 0x12345678));
    memcpy(f, kVecJoinReq, sizeof(f));
    f[5] = 'X';
    TEST_ASSERT_EQUAL((int)Route::Gen1, (int)route(f, sizeof(f), 0x12345678));
    TEST_ASSERT_EQUAL((int)Route::Gen1, (int)route(nullptr, 0, kVecDevId));
}

static ep::JoinRequest req(uint32_t devId, uint32_t devNonce, uint8_t fwMinor = 1)
{
    ep::JoinRequest r = {devId, devNonce, 3, 0, fwMinor, 0, ep::kCapRx2, 0};
    return r;
}

static void test_waiting_needs_two_consistent_frames(void)
{
    WaitingList<4> w;
    WaitingEntry out[4];
    TEST_ASSERT_FALSE(w.record(req(0xA, 1), -70, 1000));
    TEST_ASSERT_EQUAL(0, w.listed(out, 4));
    TEST_ASSERT_TRUE(w.record(req(0xA, 2), -71, 2000));
    TEST_ASSERT_EQUAL(1, w.listed(out, 4));
    TEST_ASSERT_EQUAL_HEX32(0xA, out[0].devId);
    TEST_ASSERT_EQUAL_UINT16(2, out[0].frames);
    TEST_ASSERT_EQUAL_INT16(-71, out[0].rssi);
}

static void test_waiting_inconsistent_frames_restart(void)
{
    WaitingList<4> w;
    WaitingEntry out[4];
    w.record(req(0xA, 5), -70, 1000);
    // Replayed frame (same devNonce): restart, not listed
    TEST_ASSERT_FALSE(w.record(req(0xA, 5), -70, 2000));
    TEST_ASSERT_EQUAL(0, w.listed(out, 4));
    // Different firmware version: restart
    TEST_ASSERT_FALSE(w.record(req(0xA, 6, 2), -70, 3000));
    TEST_ASSERT_TRUE(w.record(req(0xA, 7, 2), -70, 4000));
    // devNonce going backwards: restart again
    TEST_ASSERT_FALSE(w.record(req(0xA, 3, 2), -70, 5000));
    TEST_ASSERT_EQUAL(0, w.listed(out, 4));
}

static void test_waiting_lru_and_order(void)
{
    WaitingList<2> w;
    WaitingEntry out[2];
    w.record(req(0xA, 1), -70, 1000);
    w.record(req(0xA, 2), -70, 1100); // A listed, last seen 1100
    w.record(req(0xB, 1), -70, 1200);
    w.record(req(0xB, 2), -70, 1300); // B listed, last seen 1300
    TEST_ASSERT_EQUAL(2, w.listed(out, 2));
    TEST_ASSERT_EQUAL_HEX32(0xB, out[0].devId); // newest first
    TEST_ASSERT_EQUAL_HEX32(0xA, out[1].devId);
    // Full and all listed: C replaces the least recently seen entry (A)
    w.record(req(0xC, 1), -70, 1400);
    TEST_ASSERT_EQUAL(1, w.listed(out, 2));
    TEST_ASSERT_EQUAL_HEX32(0xB, out[0].devId);
    // A comes back: starts from one frame again
    TEST_ASSERT_FALSE(w.record(req(0xA, 3), -70, 1500));
}

static void test_waiting_remove(void)
{
    WaitingList<4> w;
    WaitingEntry out[4];
    w.record(req(0xA, 1), -70, 1000);
    w.record(req(0xA, 2), -70, 2000);
    w.remove(0xA);
    TEST_ASSERT_EQUAL(0, w.listed(out, 4));
    TEST_ASSERT_FALSE(w.record(req(0xA, 3), -70, 3000));
}

static void test_waiting_forged_flood_keeps_listed(void)
{
    WaitingList<3> w;
    WaitingEntry out[3];
    w.record(req(0xA, 1), -70, 1000);
    TEST_ASSERT_TRUE(w.record(req(0xA, 2), -70, 1100)); // A listed (oldest)
    TEST_ASSERT_FALSE(w.record(req(0xA, 3), -70, 1150)); // already listed: no second "listed" event
    // A flood of fresh DevIDs, one frame each, newer than A
    for (uint32_t id = 0x100; id < 0x120; id++)
    {
        w.record(req(id, 1), -90, 2000 + id);
    }
    TEST_ASSERT_EQUAL(1, w.listed(out, 3));
    TEST_ASSERT_EQUAL_HEX32(0xA, out[0].devId); // survived
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_dispatch_known_and_unknown);
    RUN_TEST(test_dispatch_gen1_frames);
    RUN_TEST(test_waiting_needs_two_consistent_frames);
    RUN_TEST(test_waiting_inconsistent_frames_restart);
    RUN_TEST(test_waiting_lru_and_order);
    RUN_TEST(test_waiting_remove);
    RUN_TEST(test_waiting_forged_flood_keeps_listed);
    return UNITY_END();
}
