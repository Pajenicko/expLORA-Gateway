/**
 * Native tests for signed sensor images in the OTA image store
 * (src/Protocol/OtaImageSigned.h, M4c S2): the upload is split into the
 * stored image (byte-identical to the unsigned file) and the tail kept in
 * RAM, then classified Verified / Unsigned / NoKey or refused, with the
 * real SigVerify against the host mbedtls 3.6.6. Fixtures: fixtures.h,
 * made by gen_fixtures.py with throwaway TEST keys (public points only).
 *
 * Run:   pio test -e native -f test_ota_signed
 */

#include <unity.h>
#include <cstring>
#include <vector>

#include "../../src/Security/SigVerify.cpp"
#include "../../src/Security/Sha256.h"
#include "../../src/Protocol/OtaImageSigned.h"
#include "fixtures.h"

using namespace OtaImageSigned;
using SigVerify::Result;
using Bytes = std::vector<uint8_t>;

void setUp(void) {}
void tearDown(void) {}

static const size_t IMAGE_LEN = sizeof(SIGNED_FILE) - SigBlock::SIZE; // 1024
static const uint16_t HW = 0x0003;
static const uint32_t FW = 0x00020C;

static ReleaseKeys::Key makeKey(uint32_t id, const uint8_t *point)
{
    ReleaseKeys::Key k{id, {}};
    memcpy(k.point, point, ReleaseKeys::POINT_SIZE);
    return k;
}

static const ReleaseKeys::Key TEST_KEYS[2] = {{0, {}}, makeKey(TEST_KEY_ID, TEST_KEY_POINT)};

static Bytes image()
{
    return Bytes(SIGNED_FILE, SIGNED_FILE + IMAGE_LEN);
}

static Bytes withTail(const Bytes &img, const uint8_t *tail, size_t n)
{
    Bytes f = img;
    f.insert(f.end(), tail, tail + n);
    return f;
}

// One upload as OtaStore does it: chunks in, the payload prefix "stored"
struct Upload
{
    Bytes stored;
    Early early = Early::None;
    Outcome out;
};

template <class Hash = Sha256>
static Upload run(const Bytes &file, size_t chunk, const ReleaseKeys::Key *keys = TEST_KEYS, size_t keyCount = 2)
{
    Upload u;
    Splitter<Hash> s;
    Hash h;
    for (size_t i = 0; i < file.size(); i += chunk)
    {
        size_t n = file.size() - i < chunk ? file.size() - i : chunk;
        size_t payload = 0;
        u.early = s.feed(file.data() + i, n, payload, h);
        TEST_ASSERT_TRUE(payload <= n);
        u.stored.insert(u.stored.end(), file.data() + i, file.data() + i + payload);
        if (u.early != Early::None)
        {
            return u;
        }
    }
    u.out = classify(s, h, keys, keyCount);
    return u;
}

static void put32(Bytes &d, size_t at, uint32_t v)
{
    d[at] = (uint8_t)v;
    d[at + 1] = (uint8_t)(v >> 8);
    d[at + 2] = (uint8_t)(v >> 16);
    d[at + 3] = (uint8_t)(v >> 24);
}

// The signed file in every chunking: verified, the stored bytes are the
// unsigned image byte for byte
static void test_signed_file_stored_byte_identical(void)
{
    const Bytes file(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    for (size_t chunk : {1, 7, 35, 36, 37, 128, 1000, 1023, 1024, 1025, 1100, 4096})
    {
        Upload u = run(file, chunk);
        TEST_ASSERT_TRUE(u.early == Early::None);
        TEST_ASSERT_TRUE_MESSAGE(u.out.cls == Class::Verified, u.out.text);
        TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, u.out.keyId);
        TEST_ASSERT_EQUAL_size_t(IMAGE_LEN, u.stored.size());
        TEST_ASSERT_EQUAL_MEMORY(SIGNED_FILE, u.stored.data(), IMAGE_LEN);
        TEST_ASSERT_EQUAL_UINT16(HW, u.out.trailer.hwType);
        TEST_ASSERT_EQUAL_HEX32(FW, u.out.trailer.fw);
    }
    char want[40];
    snprintf(want, sizeof(want), "verified (key %08lx)", (unsigned long)TEST_KEY_ID);
    TEST_ASSERT_EQUAL_STRING(want, run(file, 512).out.text);
}

static void test_unsigned_file(void)
{
    Upload u = run(image(), 100);
    TEST_ASSERT_TRUE(u.out.cls == Class::Unsigned);
    TEST_ASSERT_EQUAL_UINT32(0, u.out.keyId);
    TEST_ASSERT_EQUAL_size_t(IMAGE_LEN, u.stored.size());
    TEST_ASSERT_EQUAL_MEMORY(SIGNED_FILE, u.stored.data(), IMAGE_LEN);
    TEST_ASSERT_EQUAL_STRING("unsigned", u.out.text);
}

// Well-formed and bound, but the key is not in the table: stored as unsigned
// with the key id noted (the production table is empty until the owner
// creates the release key, so this is what a signed file gives today)
static void test_unknown_key(void)
{
    const Bytes signedFile(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    Upload u = run(signedFile, 256, ReleaseKeys::KEYS, ReleaseKeys::SLOTS);
    TEST_ASSERT_TRUE(u.out.cls == Class::NoKey);
    TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, u.out.keyId);
    TEST_ASSERT_FALSE(u.out.severe);
    TEST_ASSERT_EQUAL_MEMORY(SIGNED_FILE, u.stored.data(), IMAGE_LEN);
    // A second key's block with its own id
    Upload o = run(withTail(image(), BLOCK_OTHER_KEY, sizeof(BLOCK_OTHER_KEY)), 300);
    TEST_ASSERT_TRUE(o.out.cls == Class::NoKey);
    TEST_ASSERT_EQUAL_HEX32(OTHER_KEY_ID, o.out.keyId);
    char want[60];
    snprintf(want, sizeof(want), "unsigned (signed with an unknown key %08lx)", (unsigned long)OTHER_KEY_ID);
    TEST_ASSERT_EQUAL_STRING(want, o.out.text);
}

// Remainders: empty or exactly 128 bytes with the magic, nothing else
static void test_remainder_lengths(void)
{
    const uint8_t *block = SIGNED_FILE + IMAGE_LEN;
    // 127: refused at the end, not severe (no complete block)
    Upload u127 = run(withTail(image(), block, 127), 64);
    TEST_ASSERT_TRUE(u127.early == Early::None);
    TEST_ASSERT_TRUE(u127.out.cls == Class::Refused);
    TEST_ASSERT_FALSE(u127.out.severe);
    TEST_ASSERT_NOT_NULL(strstr(u127.out.text, "127 bytes"));
    // 128: verified (above); 129: stopped while streaming
    Bytes f129 = withTail(image(), block, 128);
    f129.push_back(0);
    for (size_t chunk : {1, 64, 2000})
    {
        Upload u129 = run(f129, chunk);
        TEST_ASSERT_TRUE(u129.early == Early::TailLong);
        TEST_ASSERT_TRUE(u129.stored.size() <= IMAGE_LEN); // never more than the image stored
    }
    // 1 byte
    Upload u1 = run(withTail(image(), block, 1), 1000);
    TEST_ASSERT_TRUE(u1.out.cls == Class::Refused);
    // 128 bytes without the magic: refused, not severe
    uint8_t junk[128];
    memcpy(junk, block, sizeof(junk));
    junk[0] = 'X';
    Upload uj = run(withTail(image(), junk, sizeof(junk)), 128);
    TEST_ASSERT_TRUE(uj.out.cls == Class::Refused);
    TEST_ASSERT_FALSE(uj.out.severe);
    TEST_ASSERT_NOT_NULL(strstr(uj.out.text, "no magic"));
    // Magic present, malformed (reserved byte set): severe
    uint8_t bad[128];
    memcpy(bad, block, sizeof(bad));
    bad[125] = 1;
    Upload ub = run(withTail(image(), bad, sizeof(bad)), 128);
    TEST_ASSERT_TRUE(ub.out.cls == Class::Refused);
    TEST_ASSERT_TRUE(ub.out.severe);
    TEST_ASSERT_TRUE(ub.out.result == Result::BadBlock);
}

// Present but wrong: always refused, severe
static void test_binding_and_signature_refusals(void)
{
    struct Case
    {
        const uint8_t *block;
        Result want;
        bool severe;
    } cases[] = {
        {BLOCK_WRONG_HW, Result::WrongTarget, true},
        {BLOCK_WRONG_VERSION, Result::WrongVersion, true},
        {BLOCK_WRONG_KIND, Result::WrongKind, true},
        {BLOCK_WRONG_HASH, Result::WrongHash, true},
        {BLOCK_BAD_SIG, Result::BadSig, true},
    };
    for (const Case &c : cases)
    {
        Upload u = run(withTail(image(), c.block, SigBlock::SIZE), 333);
        TEST_ASSERT_TRUE(u.early == Early::None);
        TEST_ASSERT_TRUE(u.out.cls == Class::Refused);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)c.want, (uint8_t)u.out.result);
        TEST_ASSERT_EQUAL(c.severe, u.out.severe);
        TEST_ASSERT_FALSE(storable(u.out.cls, false));
    }
    // A wrong binding is refused even when the key is unknown (the binding
    // is checked first): a HW 4 block never ends as "unsigned"
    Upload hw = run(withTail(image(), BLOCK_WRONG_HW, SigBlock::SIZE), 333, ReleaseKeys::KEYS, ReleaseKeys::SLOTS);
    TEST_ASSERT_TRUE(hw.out.cls == Class::Refused);
    TEST_ASSERT_TRUE(hw.out.result == Result::WrongTarget);
    // Length: the block's payload length is not the image's - said so
    Upload len = run(withTail(image(), BLOCK_WRONG_LENGTH, SigBlock::SIZE), 333);
    TEST_ASSERT_TRUE(len.out.cls == Class::Refused);
    TEST_ASSERT_TRUE(len.out.severe);
    TEST_ASSERT_TRUE(len.out.result == Result::WrongLength);
    TEST_ASSERT_EQUAL_STRING("signature is for a file of another length", len.out.text);
    // Key id 0 (an empty slot's id, never made by the key tool): a malformed
    // block, refused like one - with the test table and the empty production one
    Bytes zero(BLOCK_OTHER_KEY, BLOCK_OTHER_KEY + SigBlock::SIZE);
    put32(zero, 20, 0);
    for (int table = 0; table < 2; table++)
    {
        Upload z = table ? run(withTail(image(), zero.data(), SigBlock::SIZE), 333, ReleaseKeys::KEYS,
                               ReleaseKeys::SLOTS)
                         : run(withTail(image(), zero.data(), SigBlock::SIZE), 333);
        TEST_ASSERT_TRUE(z.out.cls == Class::Refused);
        TEST_ASSERT_TRUE(z.out.severe);
        TEST_ASSERT_TRUE(z.out.result == Result::BadBlock);
        TEST_ASSERT_EQUAL_STRING("malformed signature block", z.out.text);
    }
}

// The image changed after signing (CRC fixed up, so the image itself is
// valid): the hash of the stored bytes differs from the block
static void test_tampered_image(void)
{
    Bytes f(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    f[700] ^= 0x40;
    put32(f, IMAGE_LEN - 4, OtaImage::crc32Update(0, f.data(), IMAGE_LEN - 4));
    Upload u = run(f, 500);
    TEST_ASSERT_TRUE(u.out.image == OtaImage::Error::None);
    TEST_ASSERT_TRUE(u.out.cls == Class::Refused);
    TEST_ASSERT_TRUE(u.out.result == Result::WrongHash);
    TEST_ASSERT_TRUE(u.out.severe);
    // Without the CRC fix the image check refuses it first
    Bytes g(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    g[700] ^= 0x40;
    Upload v = run(g, 500);
    TEST_ASSERT_TRUE(v.out.image == OtaImage::Error::Crc);
    TEST_ASSERT_TRUE(v.out.cls == Class::Refused);
    TEST_ASSERT_FALSE(v.out.severe);
}

// Image problems are found as before
static void test_image_errors(void)
{
    // Size word: too small / too large for the slot - stopped at once
    Bytes small = image();
    put32(small, OtaImage::SIZE_WORD_OFFSET, 100);
    TEST_ASSERT_TRUE(run(small, 10).early == Early::Length);
    Bytes big = image();
    put32(big, OtaImage::SIZE_WORD_OFFSET, OtaImage::MAX_LEN);
    TEST_ASSERT_TRUE(run(big, 10).early == Early::TooLarge);
    Bytes huge = image();
    put32(huge, OtaImage::SIZE_WORD_OFFSET, 0xFFFFFFF0u); // + 32 must not wrap
    TEST_ASSERT_TRUE(run(huge, 1000).early == Early::TooLarge);
    // Cut inside the image: refused by its length, never "unsigned"
    Bytes cut(SIGNED_FILE, SIGNED_FILE + 900);
    Upload c = run(cut, 100);
    TEST_ASSERT_TRUE(c.out.cls == Class::Refused);
    TEST_ASSERT_TRUE(c.out.image == OtaImage::Error::Length);
    // Image complete, block cut: refused (a cut signed file is not unsigned)
    Bytes cutBlock(SIGNED_FILE, SIGNED_FILE + IMAGE_LEN + 60);
    Upload cb = run(cutBlock, 100);
    TEST_ASSERT_TRUE(cb.out.cls == Class::Refused);
    // Shorter than the size word
    Bytes tiny(SIGNED_FILE, SIGNED_FILE + 20);
    Upload t = run(tiny, 7);
    TEST_ASSERT_TRUE(t.out.cls == Class::Refused);
    TEST_ASSERT_TRUE(t.out.image == OtaImage::Error::TooShort);
}

// A hash engine failure refuses the upload (fail closed)
struct FailHash
{
    void update(const uint8_t *, size_t) {}
    bool finish(uint8_t *) { return false; }
};

static void test_hash_failure_refuses(void)
{
    const Bytes file(SIGNED_FILE, SIGNED_FILE + sizeof(SIGNED_FILE));
    Upload u = run<FailHash>(file, 256);
    TEST_ASSERT_TRUE(u.out.cls == Class::Refused);
    TEST_ASSERT_FALSE(u.out.severe);
    // An unsigned file needs no hash
    TEST_ASSERT_TRUE(run<FailHash>(image(), 256).out.cls == Class::Unsigned);
}

// "Signed images only": what may be stored and what a stored image needs
// for a job start
static void test_signed_only_rules(void)
{
    TEST_ASSERT_TRUE(storable(Class::Verified, false));
    TEST_ASSERT_TRUE(storable(Class::Verified, true));
    TEST_ASSERT_TRUE(storable(Class::Unsigned, false));
    TEST_ASSERT_FALSE(storable(Class::Unsigned, true));
    TEST_ASSERT_TRUE(storable(Class::NoKey, false));
    TEST_ASSERT_FALSE(storable(Class::NoKey, true));
    TEST_ASSERT_FALSE(storable(Class::Refused, false));
    TEST_ASSERT_FALSE(storable(Class::Refused, true));
    TEST_ASSERT_TRUE(classOf(TEST_KEY_ID, 0) == Class::Verified);
    TEST_ASSERT_TRUE(classOf(0, OTHER_KEY_ID) == Class::NoKey);
    TEST_ASSERT_TRUE(classOf(0, 0) == Class::Unsigned);
    TEST_ASSERT_TRUE(startRule(TEST_KEY_ID, false) == StartRule::Ok);
    TEST_ASSERT_TRUE(startRule(TEST_KEY_ID, true) == StartRule::Ok);
    TEST_ASSERT_TRUE(startRule(0, false) == StartRule::Confirm);
    TEST_ASSERT_TRUE(startRule(0, true) == StartRule::Block);
    // An existing job (resume, restored, the option switched on): it goes on
    // only with the job's own image, verified; anything else holds it
    const uint32_t C = 0x11223344u, L = 96456;
    TEST_ASSERT_FALSE(jobHeld(false, false, 0, 0, 0, C, L));       // option off: never
    TEST_ASSERT_FALSE(jobHeld(true, true, C, L, TEST_KEY_ID, C, L)); // verified, same image
    TEST_ASSERT_TRUE(jobHeld(true, true, C, L, 0, C, L));            // unsigned (or unknown key)
    TEST_ASSERT_TRUE(jobHeld(true, true, C + 1, L, TEST_KEY_ID, C, L)); // another image
    TEST_ASSERT_TRUE(jobHeld(true, true, C, L + 1, TEST_KEY_ID, C, L));
    TEST_ASSERT_TRUE(jobHeld(true, false, C, L, TEST_KEY_ID, C, L)); // no image stored: fail closed
    // A value-initialised outcome is no acceptance
    Outcome o{};
    TEST_ASSERT_TRUE(o.cls == Class::Refused);
    char t[48];
    stateText(TEST_KEY_ID, 0, t, sizeof(t));
    char want[48];
    snprintf(want, sizeof(want), "verified (key %08lx)", (unsigned long)TEST_KEY_ID);
    TEST_ASSERT_EQUAL_STRING(want, t);
    stateText(0, 0, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("unsigned", t);
    stateText(0, 0xDEADBEEFu, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("unsigned (unknown key deadbeef)", t);
}

// A stored "signed" whose key this firmware does not carry (key rotated
// out, a bench key) reads as an unknown key
struct M
{
    uint32_t signedKey, unknownKey;
};

static void test_key_table_on_read(void)
{
    TEST_ASSERT_TRUE(knownKey(TEST_KEY_ID, TEST_KEYS, 2));
    TEST_ASSERT_FALSE(knownKey(OTHER_KEY_ID, TEST_KEYS, 2));
    TEST_ASSERT_FALSE(knownKey(0, TEST_KEYS, 2)); // the empty slot never matches
    TEST_ASSERT_FALSE(knownKey(TEST_KEY_ID));     // production table: empty
    M kept{TEST_KEY_ID, 0};
    applyKeyTable(kept, TEST_KEYS, 2);
    TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, kept.signedKey);
    TEST_ASSERT_EQUAL_HEX32(0, kept.unknownKey);
    M gone{TEST_KEY_ID, 0};
    applyKeyTable(gone);
    TEST_ASSERT_EQUAL_HEX32(0, gone.signedKey);
    TEST_ASSERT_EQUAL_HEX32(TEST_KEY_ID, gone.unknownKey);
    M plain{0, 0};
    applyKeyTable(plain);
    TEST_ASSERT_EQUAL_HEX32(0, plain.signedKey);
    TEST_ASSERT_EQUAL_HEX32(0, plain.unknownKey);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_signed_file_stored_byte_identical);
    RUN_TEST(test_unsigned_file);
    RUN_TEST(test_unknown_key);
    RUN_TEST(test_remainder_lengths);
    RUN_TEST(test_binding_and_signature_refusals);
    RUN_TEST(test_tampered_image);
    RUN_TEST(test_image_errors);
    RUN_TEST(test_hash_failure_refuses);
    RUN_TEST(test_signed_only_rules);
    RUN_TEST(test_key_table_on_read);
    return UNITY_END();
}
