/**
 * Native tests for the OTA image store's boot recovery of a commit cut by a
 * reset (src/Storage/OtaCommitRecovery.h, docs/protocol-v2.md 14.5): every
 * reset point of uploadCommit and which CRC passes the decision needs, and
 * the boot check of a stored pair.
 * Run:   pio test -e native -f test_ota_store
 */

#include <unity.h>
#include "../../src/Storage/OtaCommitRecovery.h"

using namespace OtaCommitRecovery;

void setUp(void) {}
void tearDown(void) {}

struct Check
{
    bool result;
    int calls = 0;
    bool operator()()
    {
        calls++;
        return result;
    }
};

static Action run(bool uploadTmp, bool tmpValid, Check &n, Check &o)
{
    return decide(uploadTmp, tmpValid, [&]() { return n(); }, [&]() { return o(); });
}

// Reset after meta.tmp, before the .bin rename: the old pair, no CRC pass
static void test_reset_before_bin_rename(void)
{
    Check n{true}, o{true};
    TEST_ASSERT_EQUAL((int)Action::DropTmp, (int)run(true, true, n, o));
    TEST_ASSERT_EQUAL(0, n.calls);
    TEST_ASSERT_EQUAL(0, o.calls);
}

// Reset while meta.tmp was written (truncated): the old pair, no CRC pass
static void test_reset_during_tmp_write(void)
{
    Check n{false}, o{false};
    TEST_ASSERT_EQUAL((int)Action::DropTmp, (int)run(true, false, n, o));
    TEST_ASSERT_EQUAL((int)Action::DropTmp, (int)run(false, false, n, o));
    TEST_ASSERT_EQUAL(0, n.calls + o.calls);
}

// Reset between the two renames: the new .bin gets its description
static void test_reset_between_renames(void)
{
    Check n{true}, o{false};
    TEST_ASSERT_EQUAL((int)Action::FinishTmp, (int)run(false, true, n, o));
    TEST_ASSERT_EQUAL(1, n.calls);
    TEST_ASSERT_EQUAL(0, o.calls);
}

// The .bin is still the old image: a second reset after begin() had already
// deleted upload.tmp (but not yet meta.tmp) - the old pair stays
static void test_bin_still_old(void)
{
    Check n{false}, o{true};
    TEST_ASSERT_EQUAL((int)Action::DropTmp, (int)run(false, true, n, o));
    TEST_ASSERT_EQUAL(1, n.calls);
    TEST_ASSERT_EQUAL(1, o.calls);
}

// The .bin matches neither description (or is missing): no image is kept
static void test_bin_matches_neither(void)
{
    Check n{false}, o{false};
    TEST_ASSERT_EQUAL((int)Action::DropAll, (int)run(false, true, n, o));
    TEST_ASSERT_EQUAL(1, n.calls);
    TEST_ASSERT_EQUAL(1, o.calls);
}

// The same image uploaded again (both descriptions match): the new one wins
static void test_same_image_again(void)
{
    Check n{true}, o{true};
    TEST_ASSERT_EQUAL((int)Action::FinishTmp, (int)run(false, true, n, o));
    TEST_ASSERT_EQUAL(1, n.calls);
    TEST_ASSERT_EQUAL(0, o.calls);
}

static void assertPair(Pair want, Pair got)
{
    TEST_ASSERT_EQUAL((int)want, (int)got);
}

// A complete pair stays
static void test_pair_keep(void)
{
    assertPair(Pair::Keep, checkPair(true, 4096, true, 0x0102, 0x0102, 4096));
}

// A .bin or .json alone (a reset within remove()) goes
static void test_pair_orphan(void)
{
    assertPair(Pair::Orphan, checkPair(true, 0, true, 0x0102, 0x0102, 4096));
    assertPair(Pair::Orphan, checkPair(true, 4096, false, 0, 0x0102, 0));
}

// The new .bin with the old .json of an older firmware: another length goes
static void test_pair_wrong_length(void)
{
    assertPair(Pair::WrongLength, checkPair(true, 4128, true, 0x0102, 0x0102, 4096));
    assertPair(Pair::WrongLength, checkPair(true, 4064, true, 0x0102, 0x0102, 4096));
}

// A .json of another HW type or a non-canonical name goes
static void test_pair_other_hw_or_name(void)
{
    assertPair(Pair::OtherHw, checkPair(true, 4096, true, 0x0103, 0x0102, 4096));
    assertPair(Pair::BadName, checkPair(false, 4096, true, 0x0102, 0x0102, 4096));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_reset_before_bin_rename);
    RUN_TEST(test_reset_during_tmp_write);
    RUN_TEST(test_reset_between_renames);
    RUN_TEST(test_bin_still_old);
    RUN_TEST(test_bin_matches_neither);
    RUN_TEST(test_same_image_again);
    RUN_TEST(test_pair_keep);
    RUN_TEST(test_pair_orphan);
    RUN_TEST(test_pair_wrong_length);
    RUN_TEST(test_pair_other_hw_or_name);
    return UNITY_END();
}
