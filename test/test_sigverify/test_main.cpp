/**
 * Native unit tests for SigVerify (src/Security/SigVerify.cpp) against the
 * host build of mbedtls 3.6.6 (scripts/native_mbedtls.py): files signed by
 * the Python release tool (fixtures.h, made by gen_fixtures.py with a
 * throwaway test key) verify through the same stream helpers the gateway
 * will use; every tampering is refused with its own result.
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <cstring>
#include <vector>

#include "../../src/Security/SigVerify.cpp"
#include "../../src/Security/Sha256.h"
#include "fixtures.h"

using namespace SigBlock;
using SigVerify::Result;
using Bytes = std::vector<uint8_t>;

void setUp(void) {}
void tearDown(void) {}

static ReleaseKeys::Key makeKey(uint32_t id, const uint8_t *point)
{
    ReleaseKeys::Key k{id, {}};
    memcpy(k.point, point, ReleaseKeys::POINT_SIZE);
    return k;
}

static const ReleaseKeys::Key TEST_KEYS[2] = {makeKey(OTHER_KEY_ID, OTHER_KEY_POINT),
                                              makeKey(TEST_KEY_ID, TEST_KEY_POINT)};

static const uint32_t GW_VERSION = 0x020103;    // 2.1.3
static const uint32_t SENSOR_VERSION = 0x00020C; // 0.2.12

// A received file split like the gateway does it
struct Received
{
    Tail tail;
    Block block;
    uint8_t hash[32];
    uint32_t payloadLen;
};

static Received viaRing(const uint8_t *file, size_t len, size_t chunk)
{
    Received r{};
    TailRing ring;
    Sha256 sha;
    for (size_t off = 0; off < len; off += chunk)
    {
        ring.write(file + off, len - off < chunk ? len - off : chunk, sha);
    }
    Format f;
    r.tail = ring.finish(sha, r.block, f);
    TEST_ASSERT_TRUE(sha.finish(r.hash));
    r.payloadLen = (uint32_t)(r.tail == Tail::Signed ? ring.total() - SIZE : ring.total());
    return r;
}

static Received viaSensorTail(const uint8_t *file, size_t len, uint32_t payloadLen, size_t chunk)
{
    Received r{};
    SensorTail t(payloadLen);
    Sha256 sha;
    for (size_t off = 0; off < len; off += chunk)
    {
        size_t n = len - off < chunk ? len - off : chunk;
        sha.update(file + off, t.write(file + off, n));
    }
    Format f;
    r.tail = t.finish(r.block, f);
    TEST_ASSERT_TRUE(sha.finish(r.hash));
    r.payloadLen = payloadLen;
    return r;
}

static Expect gwExpect(const Received &r)
{
    return Expect{Kind::Gateway, TARGET_PRO, GW_VERSION, r.payloadLen, r.hash};
}

static Result verifyWith(const Block &b, const Expect &e)
{
    return SigVerify::verify(b, e, TEST_KEYS, 2);
}

static void test_key_ids_match_points(void)
{
    uint32_t id = 0;
    TEST_ASSERT_TRUE(SigVerify::keyId(TEST_KEY_POINT, id));
    TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, id);
    TEST_ASSERT_TRUE(SigVerify::keyId(OTHER_KEY_POINT, id));
    TEST_ASSERT_EQUAL_HEX32(OTHER_KEY_ID, id);
}

static void test_release_table(void)
{
    // Every filled slot carries the id of its own point; the test keys are
    // never release keys
    for (size_t i = 0; i < ReleaseKeys::SLOTS; i++)
    {
        const ReleaseKeys::Key &k = ReleaseKeys::KEYS[i];
        if (k.keyId == 0)
        {
            continue;
        }
        uint32_t id = 0;
        TEST_ASSERT_TRUE(SigVerify::keyId(k.point, id));
        TEST_ASSERT_EQUAL_HEX32(id, k.keyId);
        TEST_ASSERT_NOT_EQUAL(TEST_KEY_ID, k.keyId);
        TEST_ASSERT_NOT_EQUAL(OTHER_KEY_ID, k.keyId);
    }
}

static void test_gateway_fixture_verifies(void)
{
    for (size_t chunk : {1, 7, 100, 127, 128, 129, 4096})
    {
        Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), chunk);
        TEST_ASSERT_TRUE(r.tail == Tail::Signed);
        TEST_ASSERT_EQUAL_UINT32(sizeof(GW_SIGNED) - SIZE, r.payloadLen);
        TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, r.block.keyId);
        TEST_ASSERT_TRUE(verifyWith(r.block, gwExpect(r)) == Result::Ok);
    }
    // From the raw bytes too
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    Expect e = gwExpect(r);
    TEST_ASSERT_TRUE(SigVerify::verifyRaw(GW_SIGNED + r.payloadLen, e, TEST_KEYS, 2) == Result::Ok);
}

static void test_sensor_fixture_verifies(void)
{
    const uint32_t payloadLen = sizeof(SENSOR_SIGNED) - SIZE;
    for (size_t chunk : {1, 36, 128, 4096})
    {
        Received r = viaSensorTail(SENSOR_SIGNED, sizeof(SENSOR_SIGNED), payloadLen, chunk);
        TEST_ASSERT_TRUE(r.tail == Tail::Signed);
        Expect e{Kind::Sensor, 0x0003, SENSOR_VERSION, payloadLen, r.hash};
        TEST_ASSERT_TRUE(verifyWith(r.block, e) == Result::Ok);
        // The same block offered for another HW type or as a gateway image
        Expect hw4 = e;
        hw4.target = 0x0004;
        TEST_ASSERT_TRUE(verifyWith(r.block, hw4) == Result::WrongTarget);
        Expect gw = e;
        gw.kind = Kind::Gateway;
        TEST_ASSERT_TRUE(verifyWith(r.block, gw) == Result::WrongKind);
    }
}

static void test_wrong_key_and_unknown_key(void)
{
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    Expect e = gwExpect(r);
    // Signed by the other key while naming the test key
    TEST_ASSERT_TRUE(SigVerify::verifyRaw(GW_WRONG_KEY_BLOCK, e, TEST_KEYS, 2) == Result::BadSig);
    // Key id not in the table
    TEST_ASSERT_TRUE(SigVerify::verify(r.block, e, TEST_KEYS, 1) == Result::NoKey);
    TEST_ASSERT_TRUE(SigVerify::verify(r.block, e, TEST_KEYS, 0) == Result::NoKey);
    // The release table (no production key yet) never verifies a test image
    TEST_ASSERT_TRUE(SigVerify::verify(r.block, e) == Result::NoKey);
    // Key id 0 (in the signed bytes) never matches, even an empty slot: it
    // marks an empty slot, so a block naming it is malformed
    const ReleaseKeys::Key empty[1] = {{0, {}}};
    uint8_t raw[SIZE];
    memcpy(raw, r.block.raw, SIZE);
    memset(raw + 20, 0, 4);
    TEST_ASSERT_TRUE(SigVerify::verifyRaw(raw, e, empty, 1) == Result::BadBlock);
    TEST_ASSERT_TRUE(SigVerify::verifyRaw(raw, e, TEST_KEYS, 2) == Result::BadBlock);
}

// verify() uses only the raw bytes: fields edited in a parsed Block (not
// covered by the signature) are ignored
static void test_parsed_fields_are_not_trusted(void)
{
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    Expect e = gwExpect(r);
    Block edited = r.block;
    edited.target = TARGET_LITE;
    edited.fwVersion = 0x090000;
    edited.keyId = OTHER_KEY_ID;
    Expect lite = e;
    lite.target = TARGET_LITE;
    lite.fwVersion = 0x090000;
    TEST_ASSERT_TRUE(verifyWith(edited, lite) == Result::WrongTarget); // raw still says Pro
    TEST_ASSERT_TRUE(verifyWith(edited, e) == Result::Ok);            // raw is the signed Pro block
    // A Block that never came from parse() (raw all zero) is malformed
    Block blank;
    TEST_ASSERT_TRUE(verifyWith(blank, e) == Result::BadBlock);
}

static void test_fail_closed_api(void)
{
    // A value-initialised result is never Ok
    Result none{};
    TEST_ASSERT_TRUE(none == Result::Error);
    TEST_ASSERT_TRUE(none != Result::Ok);
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    Expect e = gwExpect(r);
    e.payloadHash = nullptr;
    TEST_ASSERT_TRUE(verifyWith(r.block, e) == Result::Error);
    TEST_ASSERT_TRUE(SigVerify::verify(r.block, gwExpect(r), nullptr, 2) == Result::Error);
    // A default-constructed Expect (null hash, kind 0) never verifies
    Expect none2;
    TEST_ASSERT_NULL(none2.payloadHash);
    TEST_ASSERT_TRUE(verifyWith(r.block, none2) == Result::Error);
}

static void test_bad_signatures(void)
{
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    Expect e = gwExpect(r);
    // Any bit of r or s
    for (size_t i = 0; i < 64; i += 5)
    {
        Block b = r.block;
        b.raw[SIG_OFFSET + i] ^= 0x10;
        TEST_ASSERT_TRUE(verifyWith(b, e) == Result::BadSig);
    }
    // r = 0, s = 0, s = all ones (>= n)
    Block b = r.block;
    memset(b.raw + SIG_OFFSET, 0, 32);
    TEST_ASSERT_TRUE(verifyWith(b, e) == Result::BadSig);
    b = r.block;
    memset(b.raw + SIG_OFFSET + 32, 0, 32);
    TEST_ASSERT_TRUE(verifyWith(b, e) == Result::BadSig);
    b = r.block;
    memset(b.raw + SIG_OFFSET + 32, 0xFF, 32);
    TEST_ASSERT_TRUE(verifyWith(b, e) == Result::BadSig);
    // r == n or s == n (the P-256 group order) is out of range
    static const uint8_t N[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17,
                                  0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51};
    for (size_t half : {0, 32})
    {
        uint8_t raw[SIZE];
        memcpy(raw, r.block.raw, SIZE);
        memcpy(raw + SIG_OFFSET + half, N, 32);
        TEST_ASSERT_TRUE(SigVerify::verifyRaw(raw, e, TEST_KEYS, 2) == Result::BadSig);
    }
}

// Changing a signed field and expecting the changed value passes the
// binding, so only the signature can catch it
static void test_every_signed_field_is_covered(void)
{
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    const size_t offsets[] = {6, 8, 12, 16, 24, 55};
    for (size_t off : offsets)
    {
        uint8_t raw[SIZE];
        memcpy(raw, r.block.raw, SIZE);
        raw[off] ^= (off == 6) ? 0x03 : 0x01; // kind 1 -> 2
        Block b;
        TEST_ASSERT_TRUE(parse(raw, b) == Format::Ok);
        Expect e{b.kind, b.target, b.fwVersion, b.payloadLength, b.payloadHash};
        TEST_ASSERT_TRUE_MESSAGE(verifyWith(b, e) == Result::BadSig, "field change not caught");
    }
}

static void test_binding_failures(void)
{
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    Expect e = gwExpect(r);
    Expect lite = e;
    lite.target = TARGET_LITE;
    TEST_ASSERT_TRUE(verifyWith(r.block, lite) == Result::WrongTarget);
    Expect ver = e;
    ver.fwVersion = 0x020104;
    TEST_ASSERT_TRUE(verifyWith(r.block, ver) == Result::WrongVersion);
    Expect sensorKind = e;
    sensorKind.kind = Kind::Sensor;
    TEST_ASSERT_TRUE(verifyWith(r.block, sensorKind) == Result::WrongKind);
    // A payload byte changed in transit
    Bytes file(GW_SIGNED, GW_SIGNED + sizeof(GW_SIGNED));
    file[100] ^= 0x40;
    Received t = viaRing(file.data(), file.size(), 64);
    TEST_ASSERT_TRUE(t.tail == Tail::Signed);
    TEST_ASSERT_TRUE(verifyWith(t.block, gwExpect(t)) == Result::WrongHash);
    // A payload byte removed: the block's length no longer fits -> the ring
    // calls it a bad block
    file.assign(GW_SIGNED, GW_SIGNED + sizeof(GW_SIGNED));
    file.erase(file.begin() + 50);
    Received s = viaRing(file.data(), file.size(), 64);
    TEST_ASSERT_TRUE(s.tail == Tail::BadBlock);
    // Expected length differs
    Expect len = e;
    len.payloadLength -= 1;
    TEST_ASSERT_TRUE(verifyWith(r.block, len) == Result::WrongLength);
}

static void test_malformed_and_truncated(void)
{
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    Expect e = gwExpect(r);
    uint8_t raw[SIZE];
    memcpy(raw, r.block.raw, SIZE);
    raw[127] = 1;
    TEST_ASSERT_TRUE(SigVerify::verifyRaw(raw, e, TEST_KEYS, 2) == Result::BadBlock);
    memcpy(raw, r.block.raw, SIZE);
    raw[0] = 'X';
    TEST_ASSERT_TRUE(SigVerify::verifyRaw(raw, e, TEST_KEYS, 2) == Result::BadBlock);
    // Truncated download (last byte missing): no block at the end -> the
    // file counts as unsigned and its hash covers everything received
    Received t = viaRing(GW_SIGNED, sizeof(GW_SIGNED) - 1, 100);
    TEST_ASSERT_TRUE(t.tail == Tail::Unsigned);
    TEST_ASSERT_EQUAL_UINT32(sizeof(GW_SIGNED) - 1, t.payloadLen);
    // Sensor: a truncated block is refused
    const uint32_t payloadLen = sizeof(SENSOR_SIGNED) - SIZE;
    Received s = viaSensorTail(SENSOR_SIGNED, sizeof(SENSOR_SIGNED) - 1, payloadLen, 64);
    TEST_ASSERT_TRUE(s.tail == Tail::BadBlock);
}

static void test_unsigned_payload_hash(void)
{
    // The unsigned payload of the fixture through the ring: hash of all of it
    const size_t len = sizeof(GW_SIGNED) - SIZE;
    Received r = viaRing(GW_SIGNED, len, 33);
    TEST_ASSERT_TRUE(r.tail == Tail::Unsigned);
    Received s = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 33);
    TEST_ASSERT_EQUAL_MEMORY(s.hash, r.hash, 32); // same payload hash either way
    TEST_ASSERT_EQUAL_MEMORY(s.block.payloadHash, r.hash, 32);
}

static void test_bad_table_point_is_error(void)
{
    Received r = viaRing(GW_SIGNED, sizeof(GW_SIGNED), 4096);
    ReleaseKeys::Key bad = makeKey(TEST_KEY_ID, TEST_KEY_POINT);
    bad.point[64] ^= 1; // no longer on the curve
    TEST_ASSERT_TRUE(SigVerify::verify(r.block, gwExpect(r), &bad, 1) == Result::Error);
    bad.point[0] = 0x02; // compressed form is not accepted in the table
    TEST_ASSERT_TRUE(SigVerify::verify(r.block, gwExpect(r), &bad, 1) == Result::Error);
    ReleaseKeys::Key zero = makeKey(TEST_KEY_ID, TEST_KEY_POINT);
    memset(zero.point, 0, sizeof(zero.point)); // point at infinity / no point
    TEST_ASSERT_TRUE(SigVerify::verify(r.block, gwExpect(r), &zero, 1) == Result::Error);
}

// Same answers as tools/release/explora_sigblock.py parse_version
static void test_version_parity_with_python(void)
{
    for (const VersionCase &c : VERSION_CASES)
    {
        uint32_t v = 0xDEADBEEF;
        bool ok = parseVersion(c.text, v);
        TEST_ASSERT_EQUAL_MESSAGE(c.ok, ok, c.text);
        if (ok)
        {
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.value, v, c.text);
        }
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_ids_match_points);
    RUN_TEST(test_release_table);
    RUN_TEST(test_gateway_fixture_verifies);
    RUN_TEST(test_sensor_fixture_verifies);
    RUN_TEST(test_wrong_key_and_unknown_key);
    RUN_TEST(test_parsed_fields_are_not_trusted);
    RUN_TEST(test_fail_closed_api);
    RUN_TEST(test_version_parity_with_python);
    RUN_TEST(test_bad_signatures);
    RUN_TEST(test_every_signed_field_is_covered);
    RUN_TEST(test_binding_failures);
    RUN_TEST(test_malformed_and_truncated);
    RUN_TEST(test_unsigned_payload_hash);
    RUN_TEST(test_bad_table_point_is_error);
    return UNITY_END();
}
