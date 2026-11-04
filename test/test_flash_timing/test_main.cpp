/**
 * Native unit tests for FlashTiming::rxTimeReliable (flash write permit,
 * docs/protocol-v2.md section 7).
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Hardware/FlashTiming.h"

using FlashTiming::rxTimeReliable;

void setUp(void) {}
void tearDown(void) {}

static const int64_t NONE = INT64_MIN / 2;

static void test_idle_is_reliable(void)
{
    TEST_ASSERT_TRUE(rxTimeReliable(1000000, false, 0, NONE, NONE, false, 0));
    // last write long before
    TEST_ASSERT_TRUE(rxTimeReliable(1000000, false, 0, 100000, 200000, false, 0));
}

static void test_interrupt_during_write_section(void)
{
    // Sampled by the ISR: a section was open
    TEST_ASSERT_FALSE(rxTimeReliable(1000000, true, 0, NONE, NONE, false, 0));
}

static void test_deferred_isr_writer_closed_later(void)
{
    // Writer on the other core: flash op ends, the deferred ISR runs at
    // 1 000 000 (section flag already dropped in this race), the writer closes
    // its section 3 ms later -> rx lies inside [start, end]
    TEST_ASSERT_FALSE(rxTimeReliable(1000000, false, 0, 950000, 1003000, false, 0));
}

static void test_deferred_into_gap_between_flash_ops(void)
{
    // One section, several flash operations: rx in the middle of it
    TEST_ASSERT_FALSE(rxTimeReliable(1020000, false, 0, 1000000, 1080000, false, 0));
}

static void test_margin_after_section_end(void)
{
    TEST_ASSERT_FALSE(rxTimeReliable(1002000, false, 0, 900000, 1000000, false, 0)); // end + 2 ms
    TEST_ASSERT_TRUE(rxTimeReliable(1002001, false, 0, 900000, 1000000, false, 0));  // just after the margin
}

static void test_write_after_rxdone(void)
{
    // Section started after the interrupt and closed again before the
    // evaluation: the interval alone would accept it, but the closed-section
    // counter moved, so it is (conservatively) unreliable
    TEST_ASSERT_TRUE(rxTimeReliable(1000000, false, 0, 1000500, 1040000, false, 0));
    TEST_ASSERT_FALSE(rxTimeReliable(1000000, false, 5, 1000500, 1040000, false, 6));
    // ... but still open while the packet is evaluated: unreliable
    TEST_ASSERT_FALSE(rxTimeReliable(1000000, false, 0, 1000500, NONE, true, 0));
}

static void test_short_section_after_isr(void)
{
    // Section S1 deferred the interrupt (flag already dropped in the race);
    // a short S2 opens and closes before the evaluation and becomes the "last
    // section", hiding S1: the closed-section counter changed -> unreliable
    TEST_ASSERT_FALSE(rxTimeReliable(1000000, false, 41, 1010000, 1012000, false, 43));
    // Same interval data, no section closed since the interrupt -> reliable
    TEST_ASSERT_TRUE(rxTimeReliable(1000000, false, 43, 1010000, 1012000, false, 43));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_is_reliable);
    RUN_TEST(test_interrupt_during_write_section);
    RUN_TEST(test_deferred_isr_writer_closed_later);
    RUN_TEST(test_deferred_into_gap_between_flash_ops);
    RUN_TEST(test_margin_after_section_end);
    RUN_TEST(test_write_after_rxdone);
    RUN_TEST(test_short_section_after_isr);
    return UNITY_END();
}
