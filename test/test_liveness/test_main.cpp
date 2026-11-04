/**
 * Native unit tests for LivenessPolicy (the lwIP thread / radio task
 * heartbeats of Hardware/Liveness.cpp).
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Hardware/LivenessPolicy.h"

using namespace LivenessPolicy;

void setUp(void) {}
void tearDown(void) {}

static void test_advancing_counter_has_no_age(void)
{
    Beat b;
    for (uint32_t t = 0; t < 100000; t += 1000)
    {
        TEST_ASSERT_EQUAL_UINT32(0, b.sample(t / 1000, t));
    }
    TEST_ASSERT_EQUAL_UINT32(0, b.maxAge);
}

static void test_stalled_counter_ages(void)
{
    Beat b;
    b.sample(7, 1000);
    TEST_ASSERT_EQUAL_UINT32(29000, b.sample(7, 30000));
    TEST_ASSERT_FALSE(hung(29000, 30000));
    TEST_ASSERT_EQUAL_UINT32(30001, b.sample(7, 31001));
    TEST_ASSERT_TRUE(hung(30001, 30000));
    TEST_ASSERT_EQUAL_UINT32(0, b.sample(8, 32000)); // advanced again
    TEST_ASSERT_EQUAL_UINT32(30001, b.maxAge);
}

static void test_first_sample_starts_the_clock(void)
{
    Beat b;
    TEST_ASSERT_EQUAL_UINT32(0, b.sample(0, 500000)); // counter 0 is a value too
    TEST_ASSERT_EQUAL_UINT32(1000, b.sample(0, 501000));
}

static void test_millis_wrap(void)
{
    Beat b;
    b.sample(3, 0xFFFFF000u);
    TEST_ASSERT_EQUAL_UINT32(0x2000, b.sample(3, 0x1000));
    TEST_ASSERT_FALSE(hung(0x2000, 30000));
}

static void test_tcpip_probe_rules(void)
{
    // accepted and not run for 30 s: hung
    TEST_ASSERT_TRUE(tcpipHung(true, 30001, 30001, 30000, 120000));
    TEST_ASSERT_FALSE(tcpipHung(true, 30000, 30000, 30000, 120000));
    // every post refused (mailbox full): only after the longer limit
    TEST_ASSERT_FALSE(tcpipHung(false, 0, 119000, 30000, 120000));
    TEST_ASSERT_TRUE(tcpipHung(false, 0, 120001, 30000, 120000));
    // accepted 10 s ago after 115 s of refused posts: judged by the pending
    // probe, not by the 120 s rule
    TEST_ASSERT_FALSE(tcpipHung(true, 10000, 125000, 30000, 120000));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_advancing_counter_has_no_age);
    RUN_TEST(test_stalled_counter_ages);
    RUN_TEST(test_first_sample_starts_the_clock);
    RUN_TEST(test_millis_wrap);
    RUN_TEST(test_tcpip_probe_rules);
    return UNITY_END();
}
