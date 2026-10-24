/**
 * expLORA Gateway - native tests for Update/GatewayImage.h (gateway firmware
 * stream classification, activation, confirmation, validation, boot record)
 * and Update/UpdateState.h (/update/state.json). Fixtures: the signed
 * synthetic Pro image of test_sigverify (throwaway test key, public part
 * only).
 */

#include <unity.h>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/Security/SigVerify.cpp"
#include "../../src/Security/Sha256.h"
#include "../../src/Update/GatewayImage.h"
#include "../../src/Update/UpdateState.h"
#include "../../src/version.h"
#include "../test_sigverify/fixtures.h"

using namespace GatewayImage;

void setUp(void) {}
void tearDown(void) {}

static ReleaseKeys::Key keys[1];

static void initKeys()
{
    keys[0].keyId = TEST_KEY_ID;
    memcpy(keys[0].point, TEST_KEY_POINT, sizeof(TEST_KEY_POINT));
}

static std::vector<uint8_t> signedFile()
{
    return std::vector<uint8_t>(GW_SIGNED, GW_SIGNED + sizeof(GW_SIGNED));
}

static std::vector<uint8_t> payload()
{
    return std::vector<uint8_t>(GW_SIGNED, GW_SIGNED + sizeof(GW_SIGNED) - SigBlock::SIZE);
}

static void sha(const std::vector<uint8_t> &d, size_t len, uint8_t out[32])
{
    Sha256 h;
    h.update(d.data(), len);
    TEST_ASSERT_TRUE(h.finish(out));
}

// Feeds the file in pieces of `chunk` bytes (0 = one piece; a list of cut
// points otherwise)
static Outcome run(const std::vector<uint8_t> &f, size_t chunk, uint64_t announced = 0,
                   uint32_t target = SigBlock::TARGET_PRO, const ReleaseKeys::Key *k = keys, size_t n = 1)
{
    Sha256 h;
    Stream<Sha256> s;
    size_t off = 0;
    while (off < f.size())
    {
        size_t len = chunk == 0 ? f.size() - off : std::min(chunk, f.size() - off);
        s.feed(f.data() + off, len, h);
        off += len;
    }
    TEST_ASSERT_EQUAL_UINT64(f.size(), s.total());
    return s.finish(h, announced, target, k, n);
}

static void test_verified_every_chunking(void)
{
    const std::vector<uint8_t> f = signedFile();
    uint8_t want[32];
    sha(f, f.size() - SigBlock::SIZE, want);
    for (size_t chunk = 0; chunk <= f.size(); chunk += (chunk < 300 ? 1 : 37))
    {
        Outcome o = run(f, chunk);
        TEST_ASSERT_EQUAL_MESSAGE((int)Class::Verified, (int)o.cls, o.text);
        TEST_ASSERT_EQUAL_UINT32(TEST_KEY_ID, o.keyId);
        TEST_ASSERT_EQUAL_UINT32(2u << 16 | 1u << 8 | 3u, o.fwVersion); // the block's version
        TEST_ASSERT_EQUAL_MEMORY(want, o.digest, 32);
        TEST_ASSERT_EQUAL_UINT64(f.size(), o.length);
    }
    // Two pieces cut at every position (the block split anywhere)
    for (size_t cut = 1; cut < f.size(); cut++)
    {
        Sha256 h;
        Stream<Sha256> s;
        s.feed(f.data(), cut, h);
        s.feed(f.data() + cut, f.size() - cut, h);
        Outcome o = s.finish(h, f.size(), SigBlock::TARGET_PRO, keys, 1);
        TEST_ASSERT_EQUAL((int)Class::Verified, (int)o.cls);
        TEST_ASSERT_EQUAL_MEMORY(want, o.digest, 32);
    }
}

static void test_unsigned_and_tails(void)
{
    const std::vector<uint8_t> p = payload();
    uint8_t want[32];
    sha(p, p.size(), want);
    for (size_t chunk : {(size_t)0, (size_t)1, (size_t)7, (size_t)127, (size_t)128, (size_t)129, (size_t)500})
    {
        Outcome o = run(p, chunk);
        TEST_ASSERT_EQUAL((int)Class::Unsigned, (int)o.cls);
        TEST_ASSERT_EQUAL_MEMORY(want, o.digest, 32); // every byte
    }
    // 127 / 128 / 129 bytes of a file (shorter than or exactly the ring)
    for (size_t len : {(size_t)127, (size_t)128, (size_t)129})
    {
        std::vector<uint8_t> small(p.begin(), p.begin() + len);
        uint8_t w[32];
        sha(small, len, w);
        Outcome o = run(small, 1);
        TEST_ASSERT_EQUAL((int)Class::Unsigned, (int)o.cls);
        TEST_ASSERT_EQUAL_MEMORY(w, o.digest, 32);
    }
    // A signed file cut by 1 byte (127 bytes of the block): the magic is not
    // where a block starts -> unsigned when the length is not known ...
    std::vector<uint8_t> cut = signedFile();
    cut.pop_back();
    TEST_ASSERT_EQUAL((int)Class::Unsigned, (int)run(cut, 0).cls);
    // ... refused, never unsigned, against the announced length
    Outcome o = run(cut, 0, sizeof(GW_SIGNED));
    TEST_ASSERT_EQUAL((int)Class::Refused, (int)o.cls);
    TEST_ASSERT_NOT_NULL(strstr(o.text, "incomplete"));
    // Too long against the announced length
    std::vector<uint8_t> longer = signedFile();
    longer.push_back(0);
    TEST_ASSERT_EQUAL((int)Class::Refused, (int)run(longer, 3, sizeof(GW_SIGNED)).cls);
    // One byte after the block (129 bytes after the payload): the last 128
    // bytes start inside the block -> no magic there, unsigned
    TEST_ASSERT_EQUAL((int)Class::Unsigned, (int)run(longer, 3).cls);
    // A block, then one more byte before it: the block's length disagrees
    std::vector<uint8_t> shifted = payload();
    shifted.push_back(0x55);
    shifted.insert(shifted.end(), GW_SIGNED + sizeof(GW_SIGNED) - SigBlock::SIZE, GW_SIGNED + sizeof(GW_SIGNED));
    o = run(shifted, 64);
    TEST_ASSERT_EQUAL((int)Class::Refused, (int)o.cls);
    TEST_ASSERT_TRUE(o.severe);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongLength, (int)o.result);
    // Shorter than an image header
    std::vector<uint8_t> tiny(p.begin(), p.begin() + 10);
    TEST_ASSERT_EQUAL((int)Class::Refused, (int)run(tiny, 0).cls);
}

static void test_classification_matrix(void)
{
    const size_t B = sizeof(GW_SIGNED) - SigBlock::SIZE; // block offset
    // No key in the table (the production state): unsigned with the key id
    Outcome o = run(signedFile(), 0, 0, SigBlock::TARGET_PRO, keys, 0);
    TEST_ASSERT_EQUAL((int)Class::NoKey, (int)o.cls);
    TEST_ASSERT_EQUAL_UINT32(TEST_KEY_ID, o.keyId);
    TEST_ASSERT_EQUAL((int)Activation::Confirm, (int)activation(o.cls, false, o.fwVersion, 0x020000));
    // Lite: the block is for the Pro (binding before the key: also without a key)
    o = run(signedFile(), 0, 0, SigBlock::TARGET_LITE);
    TEST_ASSERT_EQUAL((int)Class::Refused, (int)o.cls);
    TEST_ASSERT_TRUE(o.severe);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongTarget, (int)o.result);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongTarget,
                      (int)run(signedFile(), 0, 0, SigBlock::TARGET_LITE, keys, 0).result);
    // Changed payload byte
    std::vector<uint8_t> f = signedFile();
    f[300] ^= 1;
    o = run(f, 0);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongHash, (int)o.result);
    TEST_ASSERT_TRUE(o.severe);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongHash, (int)run(f, 0, 0, SigBlock::TARGET_PRO, keys, 0).result);
    // Kind sensor in the block
    f = signedFile();
    f[B + 6] = 2;
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongKind, (int)run(f, 0).result);
    // Target field changed to the Lite (the Pro sees a wrong target)
    f = signedFile();
    f[B + 8] = 2;
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongTarget, (int)run(f, 0).result);
    // Version field changed: binding ok (trusted field), the signature fails
    f = signedFile();
    f[B + 12] ^= 1;
    o = run(f, 0);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::BadSig, (int)o.result);
    TEST_ASSERT_TRUE(o.severe);
    // ... with no key in the table: unsigned (cannot be checked)
    TEST_ASSERT_EQUAL((int)Class::NoKey, (int)run(f, 0, 0, SigBlock::TARGET_PRO, keys, 0).cls);
    // Length field changed: the tail says it is no block of this file
    f = signedFile();
    f[B + 16] ^= 1;
    o = run(f, 0);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::WrongLength, (int)o.result);
    // Reserved byte set / key id 0: malformed
    f = signedFile();
    f[B + 125] = 1;
    TEST_ASSERT_EQUAL((int)SigVerify::Result::BadBlock, (int)run(f, 0).result);
    f = signedFile();
    memset(&f[B + 20], 0, 4);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::BadBlock, (int)run(f, 0).result);
    // Signed by another key claiming the test key's id
    f = payload();
    f.insert(f.end(), GW_WRONG_KEY_BLOCK, GW_WRONG_KEY_BLOCK + 128);
    o = run(f, 0);
    TEST_ASSERT_EQUAL((int)SigVerify::Result::BadSig, (int)o.result);
    TEST_ASSERT_EQUAL((int)Activation::Refuse, (int)activation(o.cls, false, o.fwVersion, 0x020000));
}

static void test_finish_idempotent(void)
{
    const std::vector<uint8_t> f = signedFile();
    Sha256 h;
    Stream<Sha256> s;
    s.feed(f.data(), f.size(), h);
    Outcome a = s.finish(h, 0, SigBlock::TARGET_PRO, keys, 1);
    Outcome b = s.finish(h, 0, SigBlock::TARGET_PRO, keys, 1);
    TEST_ASSERT_EQUAL((int)Class::Verified, (int)b.cls);
    TEST_ASSERT_EQUAL_MEMORY(a.digest, b.digest, 32);
    // TailRing itself: a second finish passes nothing more to the sink and
    // returns the same tail; writes after it are ignored
    struct Count
    {
        size_t n = 0;
        void update(const uint8_t *, size_t len) { n += len; }
    } sink;
    SigBlock::TailRing ring;
    const std::vector<uint8_t> p = payload();
    ring.write(p.data(), p.size(), sink);
    SigBlock::Block blk;
    SigBlock::Format fmt;
    TEST_ASSERT_EQUAL((int)SigBlock::Tail::Unsigned, (int)ring.finish(sink, blk, fmt));
    TEST_ASSERT_EQUAL(p.size(), sink.n);
    TEST_ASSERT_EQUAL((int)SigBlock::Tail::Unsigned, (int)ring.finish(sink, blk, fmt));
    ring.write(p.data(), 10, sink);
    TEST_ASSERT_EQUAL(p.size(), sink.n);
    TEST_ASSERT_EQUAL_UINT64(p.size(), ring.total());
    SigBlock::TailRing ring2;
    Count sink2;
    ring2.write(f.data(), f.size(), sink2);
    TEST_ASSERT_EQUAL((int)SigBlock::Tail::Signed, (int)ring2.finish(sink2, blk, fmt));
    SigBlock::Block blk2;
    TEST_ASSERT_EQUAL((int)SigBlock::Tail::Signed, (int)ring2.finish(sink2, blk2, fmt));
    TEST_ASSERT_EQUAL((int)SigBlock::Format::Ok, (int)fmt);
    TEST_ASSERT_EQUAL_UINT32(blk.keyId, blk2.keyId);
    TEST_ASSERT_EQUAL(f.size() - SigBlock::SIZE, sink2.n);
}

static void test_head_check(void)
{
    std::vector<uint8_t> f = signedFile();
    TEST_ASSERT_EQUAL((int)HeadError::None, (int)checkHead(f.data(), f.size(), 0));
    TEST_ASSERT_EQUAL((int)HeadError::OtherChip, (int)checkHead(f.data(), f.size(), 9)); // ESP32-S3
    TEST_ASSERT_EQUAL((int)HeadError::Short, (int)checkHead(f.data(), 23, 0));
    f[0] = 0;
    TEST_ASSERT_EQUAL((int)HeadError::NotImage, (int)checkHead(f.data(), f.size(), 0));
}

static void test_activation_rule(void)
{
    const uint32_t run = 0x020000; // 2.0.0
    TEST_ASSERT_EQUAL((int)Activation::Activate, (int)activation(Class::Verified, false, 0x020001, run));
    TEST_ASSERT_EQUAL((int)Activation::Activate, (int)activation(Class::Verified, true, 0x020100, run));
    TEST_ASSERT_EQUAL((int)Activation::Confirm, (int)activation(Class::Unsigned, false, 0, run));
    TEST_ASSERT_EQUAL((int)Activation::Confirm, (int)activation(Class::NoKey, false, 0x020001, run));
    TEST_ASSERT_EQUAL((int)Activation::Refuse, (int)activation(Class::Unsigned, true, 0, run));
    TEST_ASSERT_EQUAL((int)Activation::Refuse, (int)activation(Class::NoKey, true, 0x020001, run));
    TEST_ASSERT_EQUAL((int)Activation::Refuse, (int)activation(Class::Refused, false, 0x020001, run));
    Outcome none;
    TEST_ASSERT_EQUAL((int)Activation::Refuse, (int)activation(none.cls, false, 0, run)); // value-initialised
}

// A verified older version waits for the confirmation (also with "Signed
// images only" on - it is signed); the same version is a reinstall
static void test_downgrade_rule(void)
{
    uint32_t run = 0;
    TEST_ASSERT_TRUE(parseVersion("2.0.0", run));
    TEST_ASSERT_EQUAL_HEX32(0x020000, run);
    TEST_ASSERT_EQUAL((int)Activation::Activate, (int)activation(Class::Verified, false, 0x020000, run)); // same
    TEST_ASSERT_EQUAL((int)Activation::Activate, (int)activation(Class::Verified, true, 0x020000, run));
    TEST_ASSERT_EQUAL((int)Activation::Confirm, (int)activation(Class::Verified, false, 0x01FFFF, run)); // 1.255.255
    TEST_ASSERT_EQUAL((int)Activation::Confirm, (int)activation(Class::Verified, true, 0x010200, run));  // 1.2.0
    TEST_ASSERT_TRUE(isDowngrade(Class::Verified, 0x010200, run));
    TEST_ASSERT_FALSE(isDowngrade(Class::Verified, 0x020000, run));
    TEST_ASSERT_FALSE(isDowngrade(Class::NoKey, 0x010200, run)); // unsigned: the confirmation anyway
    // Running version text not maj.min.pat (e.g. "2.1.0-rc1"): fails closed -
    // every verified image needs the confirmation, also with "Signed images only"
    const uint32_t unknown = runningVersionOf("2.1.0-rc1");
    TEST_ASSERT_EQUAL_HEX32(VERSION_UNKNOWN, unknown);
    TEST_ASSERT_EQUAL_HEX32(VERSION_UNKNOWN, runningVersionOf(nullptr));
    TEST_ASSERT_EQUAL_HEX32(0x020100, runningVersionOf("2.1.0"));
    TEST_ASSERT_TRUE(isDowngrade(Class::Verified, 0x010200, unknown));
    TEST_ASSERT_TRUE(isDowngrade(Class::Verified, 0x020100, unknown));
    TEST_ASSERT_TRUE(isDowngrade(Class::Verified, 0xFFFFFF, unknown));
    TEST_ASSERT_FALSE(isDowngrade(Class::NoKey, 0x020100, unknown));
    TEST_ASSERT_EQUAL((int)Activation::Confirm, (int)activation(Class::Verified, false, 0x020100, unknown));
    TEST_ASSERT_EQUAL((int)Activation::Confirm, (int)activation(Class::Verified, true, 0x030000, unknown));
    TEST_ASSERT_EQUAL((int)Activation::Refuse, (int)activation(Class::NoKey, true, 0x020100, unknown));
    TEST_ASSERT_FALSE(isDowngrade(Class::Verified, 0x000000, 0)); // 0.0.0 is a real version
    // Version text
    uint32_t v = 7;
    TEST_ASSERT_TRUE(parseVersion("1.2.13", v));
    TEST_ASSERT_EQUAL_HEX32(0x01020D, v);
    TEST_ASSERT_TRUE(parseVersion("255.255.255", v));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, v);
    const char *bad[] = {"", "2", "2.0", "2.0.", "2.0.0.1", "2.0.256", "a.b.c", "2..0", "2.0.0 ", " 2.0.0", "-1.0.0"};
    for (const char *b : bad)
    {
        TEST_ASSERT_FALSE_MESSAGE(parseVersion(b, v), b);
        TEST_ASSERT_EQUAL_UINT32(0, v);
    }
    TEST_ASSERT_FALSE(parseVersion(nullptr, v));
    // Its confirmation: same binding; "Signed images only" does not refuse it
    Pending p;
    uint8_t d[32] = {1, 2, 3};
    p.valid = true;
    memcpy(p.digest, d, 32);
    p.session = 42;
    p.createdMs = 1000;
    p.signedImage = true;
    TEST_ASSERT_EQUAL((int)Confirm::Ok, (int)checkConfirm(p, d, 42, 2000, true));
    TEST_ASSERT_EQUAL((int)Confirm::Ok, (int)checkConfirm(p, d, 42, 2000, false));
    TEST_ASSERT_EQUAL((int)Confirm::OtherSession, (int)checkConfirm(p, d, 41, 2000, true));
    TEST_ASSERT_EQUAL((int)Confirm::Expired, (int)checkConfirm(p, d, 42, 1000 + CONFIRM_VALID_MS, true));
    p.signedImage = false;
    TEST_ASSERT_EQUAL((int)Confirm::SignedOnly, (int)checkConfirm(p, d, 42, 2000, true));
}

// An image left waiting across a restart: erased unless it runs or otadata
// starts it
static void test_left_image_erase(void)
{
    // recorded, running, boot target, running PENDING_VERIFY, slot VALID
    TEST_ASSERT_TRUE(leftImageErased(true, false, false, false, false));
    TEST_ASSERT_FALSE(leftImageErased(false, false, false, false, false)); // no record
    TEST_ASSERT_FALSE(leftImageErased(true, true, false, false, false));   // the running slot
    TEST_ASSERT_FALSE(leftImageErased(true, false, true, false, false));   // the boot target
    TEST_ASSERT_FALSE(leftImageErased(true, true, true, false, false));
    // A stale record: the running image is still being validated - the other
    // slot is its rollback target
    TEST_ASSERT_FALSE(leftImageErased(true, false, false, true, false));
    // ... or otadata marks the slot VALID (it ran and was validated; a
    // never-activated image has no otadata entry)
    TEST_ASSERT_FALSE(leftImageErased(true, false, false, false, true));
    TEST_ASSERT_FALSE(leftImageErased(true, false, false, true, true));
    for (int m = 0; m < 32; m++)
    {
        const bool expect = m == 1; // only recorded alone
        TEST_ASSERT_EQUAL(expect, leftImageErased(m & 1, m & 2, m & 4, m & 8, m & 16));
    }
}

// The committed FIRMWARE_VERSION is maj.min.pat (the downgrade check and the
// release tools parse it; a pre-release text makes every verified install
// need a confirmation)
static void test_firmware_version_parses(void)
{
    uint32_t v = 0;
    TEST_ASSERT_TRUE_MESSAGE(parseVersion(FIRMWARE_VERSION, v), FIRMWARE_VERSION);
    TEST_ASSERT_NOT_EQUAL(VERSION_UNKNOWN, runningVersionOf(FIRMWARE_VERSION));
}

static void test_confirm_binding(void)
{
    Pending p;
    uint8_t d[32], other[32];
    for (int i = 0; i < 32; i++)
    {
        d[i] = (uint8_t)i;
        other[i] = (uint8_t)(i + 1);
    }
    TEST_ASSERT_EQUAL((int)Confirm::None, (int)checkConfirm(p, d, 7, 1000, false));
    p.valid = true;
    memcpy(p.digest, d, 32);
    p.length = 1830000;
    p.session = 0xABCDEF;
    p.createdMs = 0xFFFFF000u; // wraps during the 10 minutes
    TEST_ASSERT_EQUAL((int)Confirm::Ok, (int)checkConfirm(p, d, 0xABCDEF, 0xFFFFF000u + 1000, false));
    TEST_ASSERT_EQUAL((int)Confirm::Ok, (int)checkConfirm(p, d, 0xABCDEF, 0xFFFFF000u + 599999, false));
    TEST_ASSERT_EQUAL((int)Confirm::Expired, (int)checkConfirm(p, d, 0xABCDEF, 0xFFFFF000u + 600000, false));
    TEST_ASSERT_EQUAL((int)Confirm::OtherImage, (int)checkConfirm(p, other, 0xABCDEF, 0xFFFFF000u + 10, false));
    TEST_ASSERT_EQUAL((int)Confirm::OtherSession, (int)checkConfirm(p, d, 0xABCDEE, 0xFFFFF000u + 10, false));
    TEST_ASSERT_EQUAL((int)Confirm::OtherSession, (int)checkConfirm(p, d, 0, 0xFFFFF000u + 10, false));
    TEST_ASSERT_EQUAL((int)Confirm::SignedOnly, (int)checkConfirm(p, d, 0xABCDEF, 0xFFFFF000u + 10, true));
    TEST_ASSERT_EQUAL_UINT32(600000 - 10, confirmLeftMs(p, 0xFFFFF000u + 10));
    // Written without a session: never confirmable
    Pending q = p;
    q.session = 0;
    TEST_ASSERT_EQUAL((int)Confirm::OtherSession, (int)checkConfirm(q, d, 0, 0xFFFFF000u + 10, false));
    // Digest text
    char hex[65];
    digestHex(d, hex);
    TEST_ASSERT_EQUAL_STRING("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", hex);
    uint8_t back[32];
    TEST_ASSERT_TRUE(parseDigest(hex, back));
    TEST_ASSERT_EQUAL_MEMORY(d, back, 32);
    TEST_ASSERT_TRUE(parseDigest("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", back));
    TEST_ASSERT_EQUAL_MEMORY(d, back, 32);
    TEST_ASSERT_FALSE(parseDigest("00", back));
    TEST_ASSERT_FALSE(parseDigest("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1g", back));
    TEST_ASSERT_FALSE(parseDigest(nullptr, back));
    TEST_ASSERT_FALSE(parseDigest("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0", back));
}

static void test_validation_policy(void)
{
    Validation v;
    TEST_ASSERT_EQUAL_UINT32(120, v.leftS(0));
    TEST_ASSERT_FALSE(v.step(1000, false, true)); // web server not up yet
    TEST_ASSERT_FALSE(v.step(2000, true, true));  // counting from here
    TEST_ASSERT_EQUAL_UINT32(120, v.leftS(2000));
    TEST_ASSERT_FALSE(v.step(60000, true, true));
    TEST_ASSERT_EQUAL_UINT32(62, v.leftS(60000));
    TEST_ASSERT_FALSE(v.step(61000, true, false)); // liveness unhealthy: starts again
    TEST_ASSERT_FALSE(v.step(62000, true, true));
    TEST_ASSERT_FALSE(v.step(62000 + 119999, true, true));
    TEST_ASSERT_TRUE(v.step(62000 + 120000, true, true));
    TEST_ASSERT_TRUE(v.done());
    TEST_ASSERT_FALSE(v.step(62000 + 130000, true, true)); // once
    TEST_ASSERT_EQUAL_UINT32(0, v.leftS(62000 + 130000));
    // millis() wrap
    Validation w;
    w.step(0xFFFFFF00u, true, true);
    TEST_ASSERT_FALSE(w.step(0xFFFFFF00u + 1000, true, true));
    TEST_ASSERT_TRUE(w.step(0xFFFFFF00u + 120000, true, true));
    // A deliberate restart marks it done
    Validation r;
    r.step(0, true, true);
    r.markDone();
    TEST_ASSERT_TRUE(r.done());
    TEST_ASSERT_FALSE(r.step(200000, true, true));
}

static void test_boot_outcome(void)
{
    TEST_ASSERT_EQUAL((int)Boot::Nothing, (int)bootOutcome(false, false, false));
    TEST_ASSERT_EQUAL((int)Boot::Nothing, (int)bootOutcome(false, true, true));
    TEST_ASSERT_EQUAL((int)Boot::Running, (int)bootOutcome(true, true, false));
    TEST_ASSERT_EQUAL((int)Boot::RolledBack, (int)bootOutcome(true, false, true));
    TEST_ASSERT_EQUAL((int)Boot::NotRunning, (int)bootOutcome(true, false, false));
}

static void test_update_state_json(void)
{
    UpdateState::State s;
    s.running.set = true;
    UpdateState::copyText(s.running.ver, sizeof(s.running.ver), "2.0.0");
    UpdateState::copyText(s.running.part, sizeof(s.running.part), "app0");
    UpdateState::copyText(s.running.sig, sizeof(s.running.sig), "verified");
    s.running.key = 0x98FA42C1u;
    s.pending.set = true;
    UpdateState::copyText(s.pending.ver, sizeof(s.pending.ver), "2.0.1");
    UpdateState::copyText(s.pending.part, sizeof(s.pending.part), "app1");
    UpdateState::copyText(s.pending.sig, sizeof(s.pending.sig), "unsigned");
    s.pending.at = 1761200000;
    std::string longText(UpdateState::TEXT_MAX - 1, 'x');
    longText[3] = '"';
    UpdateState::copyText(s.lastText, sizeof(s.lastText), longText.c_str());
    s.lastAt = 1761200300;
    char buf[UpdateState::TEXT_CAPACITY];
    size_t n = UpdateState::format(s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    UpdateState::State r;
    TEST_ASSERT_TRUE(UpdateState::parse(buf, n, r));
    TEST_ASSERT_TRUE(r.running.set && r.pending.set);
    TEST_ASSERT_EQUAL_STRING("2.0.0", r.running.ver);
    TEST_ASSERT_EQUAL_STRING("app1", r.pending.part);
    TEST_ASSERT_EQUAL_STRING("unsigned", r.pending.sig);
    TEST_ASSERT_EQUAL_UINT32(0x98FA42C1u, r.running.key);
    TEST_ASSERT_EQUAL_UINT32(1761200000u, r.pending.at);
    TEST_ASSERT_EQUAL_STRING(longText.c_str(), r.lastText);
    char t[48];
    UpdateState::sigText(r.running, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("verified (key 98fa42c1)", t);
    UpdateState::sigText(r.pending, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("unsigned", t);
    // Empty state, no pending
    UpdateState::State e;
    n = UpdateState::format(e, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"v\":1}", buf);
    TEST_ASSERT_TRUE(UpdateState::parse(buf, n, r));
    TEST_ASSERT_FALSE(r.pending.set);
    // Other version / damaged: refused, empty
    TEST_ASSERT_FALSE(UpdateState::parse("{\"v\":2,\"pending\":{\"ver\":\"1\"}}", 30, r));
    TEST_ASSERT_FALSE(r.pending.set);
    TEST_ASSERT_FALSE(UpdateState::parse("{\"v\":1,", 7, r));
    // Over-long fields are cut, never overflow
    const char *big = "{\"v\":1,\"pending\":{\"ver\":\"123456789012345678901234567890\",\"part\":\"abcdefghijklmnopqrstuvwxyz\"}}";
    TEST_ASSERT_TRUE(UpdateState::parse(big, strlen(big), r));
    TEST_ASSERT_EQUAL(sizeof(r.pending.ver) - 1, strlen(r.pending.ver));
    TEST_ASSERT_EQUAL(sizeof(r.pending.part) - 1, strlen(r.pending.part));
    // "written" (an image left waiting) round trip
    UpdateState::State w;
    w.written.set = true;
    UpdateState::copyText(w.written.part, sizeof(w.written.part), "app0");
    w.written.at = 1761200400;
    n = UpdateState::format(w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"v\":1,\"written\":{\"part\":\"app0\",\"at\":1761200400}}", buf);
    TEST_ASSERT_TRUE(UpdateState::parse(buf, n, r));
    TEST_ASSERT_TRUE(r.written.set);
    TEST_ASSERT_EQUAL_STRING("app0", r.written.part);
    TEST_ASSERT_EQUAL_UINT32(1761200400u, r.written.at);
    // Worst case (every field full, the last text all quotes) fits the file buffer
    UpdateState::State m;
    UpdateState::Image *imgs[] = {&m.running, &m.pending, &m.written};
    for (UpdateState::Image *im : imgs)
    {
        im->set = true;
        memset(im->ver, '"', sizeof(im->ver) - 1);
        memset(im->part, '"', sizeof(im->part) - 1);
        memset(im->sig, '"', sizeof(im->sig) - 1);
        im->key = 0xFFFFFFFFu;
        im->at = 0xFFFFFFFFu;
    }
    memset(m.lastText, '"', sizeof(m.lastText) - 1);
    m.lastAt = 0xFFFFFFFFu;
    n = UpdateState::format(m, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0 && n < UpdateState::TEXT_CAPACITY);
    TEST_ASSERT_TRUE(UpdateState::parse(buf, n, r));
    TEST_ASSERT_EQUAL_STRING(m.lastText, r.lastText);
    // Unknown fields (a later stage's) are read past; this firmware drops them
    // when it writes the file (UpdateState.h: the S5 contract)
    const char *later = "{\"v\":1,\"seq\":7,\"pending\":{\"ver\":\"2.1.0\",\"part\":\"app1\",\"x\":1}}";
    TEST_ASSERT_TRUE(UpdateState::parse(later, strlen(later), r));
    TEST_ASSERT_EQUAL_STRING("2.1.0", r.pending.ver);
}

int main(int, char **)
{
    initKeys();
    UNITY_BEGIN();
    RUN_TEST(test_verified_every_chunking);
    RUN_TEST(test_unsigned_and_tails);
    RUN_TEST(test_classification_matrix);
    RUN_TEST(test_finish_idempotent);
    RUN_TEST(test_head_check);
    RUN_TEST(test_activation_rule);
    RUN_TEST(test_downgrade_rule);
    RUN_TEST(test_left_image_erase);
    RUN_TEST(test_firmware_version_parses);
    RUN_TEST(test_confirm_binding);
    RUN_TEST(test_validation_policy);
    RUN_TEST(test_boot_outcome);
    RUN_TEST(test_update_state_json);
    return UNITY_END();
}
