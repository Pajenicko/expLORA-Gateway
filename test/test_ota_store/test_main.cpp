/**
 * Native tests for the OTA image store's boot recovery of a commit cut by a
 * reset (src/Storage/OtaCommitRecovery.h, docs/protocol-v2.md 14.5): every
 * reset point of uploadCommit and which CRC passes the decision needs, and
 * the boot check of a stored pair; the image description JSON
 * (src/Storage/OtaMeta.h) incl. the signature fields and files written by an
 * older firmware.
 * Run:   pio test -e native -f test_ota_store
 */

#include <unity.h>
#include <cstring>
#include <string>
#include "../../src/Storage/OtaCommitRecovery.h"
#include "../../src/Storage/OtaMeta.h"

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

static OtaMeta::Meta roundTrip(const OtaMeta::Meta &m, std::string *text = nullptr)
{
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> out;
    OtaMeta::toJson(m, out);
    TEST_ASSERT_FALSE(out.overflowed());
    std::string s;
    serializeJson(out, s);
    if (text)
    {
        *text = s;
    }
    // Read back like readMeta() does (from a stream: strings are copied)
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> in;
    TEST_ASSERT_TRUE(deserializeJson(in, s.c_str(), s.size()) == DeserializationError::Ok);
    TEST_ASSERT_FALSE(in.overflowed());
    OtaMeta::Meta back;
    TEST_ASSERT_TRUE(OtaMeta::fromJson(in, back));
    return back;
}

// A .json of the M4b firmware (no signature fields): unsigned
static void test_meta_migration(void)
{
    const char *old = "{\"hw\":3,\"fw\":524,\"len\":93528,\"crc\":2401558300,\"flags\":257,\"uploaded\":1761200000}";
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, old) == DeserializationError::Ok);
    OtaMeta::Meta m;
    TEST_ASSERT_TRUE(OtaMeta::fromJson(doc, m));
    TEST_ASSERT_EQUAL_UINT16(3, m.hwType);
    TEST_ASSERT_EQUAL_UINT32(524, m.fw);
    TEST_ASSERT_EQUAL_UINT32(93528, m.len);
    TEST_ASSERT_EQUAL_UINT32(2401558300u, m.crc); // >= 2^31 (the "| 0u" rule)
    TEST_ASSERT_EQUAL_UINT32(0, m.signedKey);
    TEST_ASSERT_EQUAL_UINT32(0, m.unknownKey);
    TEST_ASSERT_FALSE(m.isSigned());
    // Written again by this firmware: "signed":0, no note
    std::string text;
    OtaMeta::Meta back = roundTrip(m, &text);
    TEST_ASSERT_TRUE(OtaMeta::sameContent(m, back));
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"signed\":0"));
    TEST_ASSERT_NULL(strstr(text.c_str(), "note"));
    // An empty description is no image
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> empty;
    TEST_ASSERT_TRUE(deserializeJson(empty, "{}") == DeserializationError::Ok);
    TEST_ASSERT_FALSE(OtaMeta::fromJson(empty, m));
}

static void test_meta_signature_fields(void)
{
    OtaMeta::Meta v;
    v.hwType = 0xFFFF;
    v.fw = v.len = v.crc = v.flags = v.uploadUnix = 0xFFFFFFFFu;
    v.signedKey = 0xFEDCBA98u; // >= 2^31
    OtaMeta::Meta b = roundTrip(v);
    TEST_ASSERT_TRUE(OtaMeta::sameContent(v, b));
    TEST_ASSERT_TRUE(b.isSigned());
    // Unknown key: kept with a note, still unsigned; worst-case numbers fit
    OtaMeta::Meta n = v;
    n.signedKey = 0;
    n.unknownKey = 0xFFFFFFFFu;
    std::string text;
    OtaMeta::Meta nb = roundTrip(n, &text);
    TEST_ASSERT_TRUE(OtaMeta::sameContent(n, nb));
    TEST_ASSERT_FALSE(nb.isSigned());
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"note\":\"signed with an unknown key ffffffff\""));
    // A verified key wins over a stray unknownKey in the file
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, "{\"len\":512,\"signed\":5,\"unknownKey\":7}") == DeserializationError::Ok);
    OtaMeta::Meta m;
    TEST_ASSERT_TRUE(OtaMeta::fromJson(doc, m));
    TEST_ASSERT_EQUAL_UINT32(5, m.signedKey);
    TEST_ASSERT_EQUAL_UINT32(0, m.unknownKey);
    // A changed field is noticed by the read-back compare
    OtaMeta::Meta c = v;
    c.signedKey = 1;
    TEST_ASSERT_FALSE(OtaMeta::sameContent(v, c));
}

// A description written by a later firmware with extra fields (and an
// unexpected structure) still reads through the filter readMeta uses - a
// downgraded gateway must not take it for damage and delete the image
static void test_meta_later_firmware_fields(void)
{
    std::string later = "{\"hw\":3,\"fw\":524,\"len\":96456,\"crc\":4000000000,\"flags\":257,"
                        "\"uploaded\":1761200000,\"signed\":0,\"unknownKey\":3721796218,"
                        "\"note\":\"signed with an unknown key ddd71e7a\",\"sha256\":\"";
    later += std::string(64, 'a');
    later += "\",\"source\":{\"url\":\"https://pajenicko.cz/webflash/explora/v2/sensor/hw0003-0.2.12.bin\","
             "\"seq\":12345,\"channel\":\"stable\"},\"history\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16],"
             "\"autoUpdate\":true,\"extra1\":\"";
    later += std::string(200, 'x');
    later += "\"}";
    // Unfiltered it would not fit (what the old readMeta did)
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> plain;
    TEST_ASSERT_TRUE(deserializeJson(plain, later.c_str(), later.size()) == DeserializationError::NoMemory);
    StaticJsonDocument<OtaMeta::FILTER_CAPACITY> filter;
    OtaMeta::filter(filter);
    TEST_ASSERT_FALSE(filter.overflowed());
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, later.c_str(), later.size(), DeserializationOption::Filter(filter)) ==
                     DeserializationError::Ok);
    TEST_ASSERT_FALSE(doc.overflowed());
    OtaMeta::Meta m;
    TEST_ASSERT_TRUE(OtaMeta::fromJson(doc, m));
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_UINT16(3, m.hwType);
    TEST_ASSERT_EQUAL_UINT32(96456, m.len);
    TEST_ASSERT_EQUAL_UINT32(4000000000u, m.crc);
    TEST_ASSERT_EQUAL_UINT32(0, m.signedKey);
    TEST_ASSERT_EQUAL_UINT32(3721796218u, m.unknownKey);
    // The full record of this firmware fits twice over
    OtaMeta::Meta full;
    full.hwType = 0xFFFF;
    full.fw = full.len = full.crc = full.flags = full.uploadUnix = 0xFFFFFFFFu;
    full.unknownKey = 0xFFFFFFFFu;
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> out;
    OtaMeta::toJson(full, out);
    TEST_ASSERT_TRUE(2 * out.memoryUsage() <= OtaMeta::JSON_CAPACITY);
}

// valid follows the result: a description without an image is not valid
static void test_meta_valid_flag(void)
{
    StaticJsonDocument<OtaMeta::JSON_CAPACITY> doc;
    TEST_ASSERT_TRUE(deserializeJson(doc, "{\"hw\":3,\"fw\":524,\"len\":0}") == DeserializationError::Ok);
    OtaMeta::Meta m;
    m.valid = true;
    TEST_ASSERT_FALSE(OtaMeta::fromJson(doc, m));
    TEST_ASSERT_FALSE(m.valid);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_meta_later_firmware_fields);
    RUN_TEST(test_meta_valid_flag);
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
    RUN_TEST(test_meta_migration);
    RUN_TEST(test_meta_signature_fields);
    return UNITY_END();
}
