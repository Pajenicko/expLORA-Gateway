/**
 * Native unit tests for SigBlock (src/Security/SigBlock.h): block parsing,
 * version strings, the binding checks and the two stream helpers (gateway
 * tail ring, sensor tail) with every chunking around the block boundary.
 * The signature itself: test_sigverify.
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <type_traits>
#include <vector>
#include "../../src/Security/SigBlock.h"

using namespace SigBlock;
using Bytes = std::vector<uint8_t>;

void setUp(void) {}
void tearDown(void) {}

struct Collect
{
    Bytes got;
    void update(const uint8_t *d, size_t n) { got.insert(got.end(), d, d + n); }
};

static Bytes payload(size_t n, uint8_t seed = 1)
{
    Bytes p(n);
    for (size_t i = 0; i < n; i++)
    {
        p[i] = (uint8_t)(i * 31 + seed);
    }
    return p;
}

static Bytes blockFor(size_t payloadLen, Kind kind = Kind::Gateway)
{
    Block b;
    b.kind = kind;
    b.target = 1;
    b.fwVersion = 0x020103;
    b.payloadLength = (uint32_t)payloadLen;
    b.keyId = 0x11223344;
    for (int i = 0; i < 32; i++)
    {
        b.payloadHash[i] = (uint8_t)(0xA0 + i);
    }
    for (int i = 0; i < 64; i++)
    {
        b.signature[i] = (uint8_t)(i + 1);
    }
    Bytes raw(SIZE);
    encode(b, raw.data());
    return raw;
}

static Bytes cat(const Bytes &a, const Bytes &b)
{
    Bytes r = a;
    r.insert(r.end(), b.begin(), b.end());
    return r;
}

// Feeds file in chunks of `chunk` (0 = one write) through a TailRing
static Tail ring(const Bytes &file, size_t chunk, Collect &sink, Block &block, Format &format)
{
    TailRing r;
    size_t step = chunk ? chunk : (file.empty() ? 1 : file.size());
    for (size_t off = 0; off < file.size(); off += step)
    {
        size_t n = file.size() - off < step ? file.size() - off : step;
        r.write(file.data() + off, n, sink);
    }
    TEST_ASSERT_EQUAL_UINT64(file.size(), r.total());
    return r.finish(sink, block, format);
}

// ---------------------------------------------------------------- parse

static void test_round_trip(void)
{
    Bytes raw = blockFor(1234, Kind::Sensor);
    Block b;
    TEST_ASSERT_TRUE(parse(raw.data(), b) == Format::Ok);
    TEST_ASSERT_TRUE(b.kind == Kind::Sensor);
    TEST_ASSERT_EQUAL_UINT32(1, b.target);
    TEST_ASSERT_EQUAL_UINT32(0x020103, b.fwVersion);
    TEST_ASSERT_EQUAL_UINT32(1234, b.payloadLength);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, b.keyId);
    TEST_ASSERT_EQUAL_UINT8(0xA0, b.payloadHash[0]);
    TEST_ASSERT_EQUAL_UINT8(64, b.signature[63]);
    TEST_ASSERT_EQUAL_MEMORY(raw.data(), b.raw, SIZE);
    // Little-endian fields at their offsets
    TEST_ASSERT_EQUAL_UINT8('E', raw[0]);
    TEST_ASSERT_EQUAL_UINT8(1, raw[4]);
    TEST_ASSERT_EQUAL_UINT8(2, raw[6]);
    TEST_ASSERT_EQUAL_UINT8(0xD2, raw[16]); // 1234 = 0x04D2
    TEST_ASSERT_EQUAL_UINT8(0x44, raw[20]);
}

static void test_malformed_blocks(void)
{
    Block b;
    Bytes raw = blockFor(10);
    raw[3] = 'H';
    TEST_ASSERT_TRUE(parse(raw.data(), b) == Format::NoMagic);
    raw = blockFor(10);
    raw[4] = 2;
    TEST_ASSERT_TRUE(parse(raw.data(), b) == Format::BadVersion);
    for (uint16_t kind : {0, 3, 0x100})
    {
        raw = blockFor(10);
        wr16(raw.data() + 6, kind);
        TEST_ASSERT_TRUE(parse(raw.data(), b) == Format::BadKind);
    }
    for (size_t i = RESERVED_OFFSET; i < SIZE; i++)
    {
        raw = blockFor(10);
        raw[i] = 0x80;
        TEST_ASSERT_TRUE(parse(raw.data(), b) == Format::BadReserved);
    }
    // Key id 0 marks an empty key slot: no block carries it
    raw = blockFor(10);
    wr32(raw.data() + 20, 0);
    TEST_ASSERT_TRUE(parse(raw.data(), b) == Format::BadKeyId);
}

static void test_versions(void)
{
    uint32_t v = 0;
    TEST_ASSERT_TRUE(parseVersion("2.0.0", v));
    TEST_ASSERT_EQUAL_HEX32(0x020000, v);
    TEST_ASSERT_TRUE(parseVersion("0.2.12", v));
    TEST_ASSERT_EQUAL_HEX32(0x00020C, v);
    TEST_ASSERT_TRUE(parseVersion("255.255.255", v));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF, v);
    TEST_ASSERT_TRUE(parseVersion("2.0.0-dev", v));
    TEST_ASSERT_EQUAL_HEX32(0x020000, v);
    TEST_ASSERT_TRUE(parseVersion("1.3.0+g12ab", v));
    TEST_ASSERT_EQUAL_HEX32(0x010300, v);
    for (const char *bad : {"", "1", "1.2", "1.2.", "1.2.3.4", "256.0.0", "1.2.3x", "a.b.c", "1..3", "-1.2.3",
                            "1000.0.0", "6671d0b", " 1.2.3"})
    {
        TEST_ASSERT_FALSE_MESSAGE(parseVersion(bad, v), bad);
    }
}

static void test_binding(void)
{
    Block b;
    parse(blockFor(500).data(), b);
    uint8_t hash[32];
    memcpy(hash, b.payloadHash, 32);
    Expect e{Kind::Gateway, 1, 0x020103, 500, hash};
    TEST_ASSERT_TRUE(checkBinding(b, e) == Binding::Ok);
    Expect k = e;
    k.kind = Kind::Sensor;
    TEST_ASSERT_TRUE(checkBinding(b, k) == Binding::WrongKind);
    Expect t = e;
    t.target = 2;
    TEST_ASSERT_TRUE(checkBinding(b, t) == Binding::WrongTarget);
    Expect v = e;
    v.fwVersion = 0x020104;
    TEST_ASSERT_TRUE(checkBinding(b, v) == Binding::WrongVersion);
    Expect l = e;
    l.payloadLength = 499;
    TEST_ASSERT_TRUE(checkBinding(b, l) == Binding::WrongLength);
    uint8_t other[32];
    memcpy(other, hash, 32);
    other[31] ^= 1;
    Expect h = e;
    h.payloadHash = other;
    TEST_ASSERT_TRUE(checkBinding(b, h) == Binding::WrongHash);
    // Default member initialisers: kind 0 matches no block (checked before the hash)
    Expect d;
    TEST_ASSERT_EQUAL_UINT32(0, d.target);
    TEST_ASSERT_EQUAL_UINT32(0, d.fwVersion);
    TEST_ASSERT_EQUAL_UINT32(0, d.payloadLength);
    TEST_ASSERT_NULL(d.payloadHash);
    TEST_ASSERT_TRUE(checkBinding(b, d) == Binding::WrongKind);
}

// ---------------------------------------------------------------- gateway ring

static const size_t CHUNKS[] = {0, 1, 2, 3, 7, 64, 127, 128, 129, 200, 4096};

static void test_ring_signed_all_chunkings(void)
{
    for (size_t len : {0, 1, 127, 128, 129, 255, 256, 1000})
    {
        Bytes p = payload(len);
        Bytes file = cat(p, blockFor(len));
        for (size_t chunk : CHUNKS)
        {
            Collect sink;
            Block b;
            Format f;
            TEST_ASSERT_TRUE(ring(file, chunk, sink, b, f) == Tail::Signed);
            TEST_ASSERT_TRUE(f == Format::Ok);
            TEST_ASSERT_TRUE(sink.got == p); // the sink saw exactly the payload
            TEST_ASSERT_EQUAL_UINT32(len, b.payloadLength);
        }
    }
}

static void test_ring_block_split_at_every_offset(void)
{
    // Two writes, split anywhere (inside the payload, at the block start,
    // inside the block, at its end)
    Bytes p = payload(300);
    Bytes file = cat(p, blockFor(300));
    for (size_t cut = 0; cut <= file.size(); cut++)
    {
        TailRing r;
        Collect sink;
        r.write(file.data(), cut, sink);
        r.write(file.data() + cut, file.size() - cut, sink);
        Block b;
        Format f;
        TEST_ASSERT_TRUE(r.finish(sink, b, f) == Tail::Signed);
        TEST_ASSERT_TRUE(sink.got == p);
    }
    // Three writes around the block start
    for (size_t a = 250; a < 300; a++)
    {
        for (size_t c = 300; c < 330; c++)
        {
            TailRing r;
            Collect sink;
            r.write(file.data(), a, sink);
            r.write(file.data() + a, c - a, sink);
            r.write(file.data() + c, file.size() - c, sink);
            Block b;
            Format f;
            TEST_ASSERT_TRUE(r.finish(sink, b, f) == Tail::Signed);
            TEST_ASSERT_TRUE(sink.got == p);
        }
    }
}

static void test_ring_unsigned_passthrough(void)
{
    // No block: the sink sees the whole file, whatever its length
    for (size_t len : {0, 1, 127, 128, 129, 1000})
    {
        Bytes file = payload(len, 9);
        for (size_t chunk : CHUNKS)
        {
            Collect sink;
            Block b;
            Format f;
            TEST_ASSERT_TRUE(ring(file, chunk, sink, b, f) == Tail::Unsigned);
            TEST_ASSERT_TRUE(f == Format::NoMagic);
            TEST_ASSERT_TRUE(sink.got == file);
        }
    }
}

static void test_ring_tails_of_127_128_129(void)
{
    Bytes p = payload(400);
    Bytes block = blockFor(400);
    // 127 bytes of the block after the payload (truncated): no block at the
    // end -> unsigned, everything hashed
    Bytes cut = cat(p, Bytes(block.begin(), block.end() - 1));
    Collect s1;
    Block b;
    Format f;
    TEST_ASSERT_TRUE(ring(cut, 64, s1, b, f) == Tail::Unsigned);
    TEST_ASSERT_TRUE(s1.got == cut);
    // 128: signed
    Collect s2;
    TEST_ASSERT_TRUE(ring(cat(p, block), 64, s2, b, f) == Tail::Signed);
    TEST_ASSERT_TRUE(s2.got == p);
    // 129 (one byte appended after the block): unsigned, everything hashed
    Bytes longer = cat(cat(p, block), Bytes{0x00});
    Collect s3;
    TEST_ASSERT_TRUE(ring(longer, 64, s3, b, f) == Tail::Unsigned);
    TEST_ASSERT_TRUE(s3.got == longer);
    // A 127-byte file that is the start of a block: unsigned
    Bytes shortFile(block.begin(), block.end() - 1);
    Collect s4;
    TEST_ASSERT_TRUE(ring(shortFile, 0, s4, b, f) == Tail::Unsigned);
    TEST_ASSERT_TRUE(s4.got == shortFile);
}

static void test_ring_bad_blocks(void)
{
    Bytes p = payload(200);
    Block b;
    Format f;
    // Length disagrees with the bytes before the block
    for (size_t claimed : {199, 201, 0})
    {
        Collect sink;
        TEST_ASSERT_TRUE(ring(cat(p, blockFor(claimed)), 50, sink, b, f) == Tail::BadBlock);
        TEST_ASSERT_TRUE(f == Format::Ok);
    }
    // Malformed block with the magic
    Bytes block = blockFor(200);
    block[125] = 1;
    Collect sink;
    TEST_ASSERT_TRUE(ring(cat(p, block), 50, sink, b, f) == Tail::BadBlock);
    TEST_ASSERT_TRUE(f == Format::BadReserved);
    block = blockFor(200);
    block[4] = 9;
    Collect sink2;
    TEST_ASSERT_TRUE(ring(cat(p, block), 50, sink2, b, f) == Tail::BadBlock);
    TEST_ASSERT_TRUE(f == Format::BadVersion);
}

// ---------------------------------------------------------------- sensor tail

static Tail sensor(const Bytes &file, uint32_t payloadLen, size_t chunk, Bytes &taken, Block &b, Format &f)
{
    SensorTail t(payloadLen);
    size_t step = chunk ? chunk : (file.empty() ? 1 : file.size());
    for (size_t off = 0; off < file.size(); off += step)
    {
        size_t n = file.size() - off < step ? file.size() - off : step;
        size_t take = t.write(file.data() + off, n);
        TEST_ASSERT_TRUE(take <= n);
        taken.insert(taken.end(), file.begin() + off, file.begin() + off + take);
    }
    return t.finish(b, f);
}

static void test_sensor_signed_and_unsigned(void)
{
    Bytes p = payload(256, 5);
    for (size_t chunk : CHUNKS)
    {
        Bytes taken;
        Block b;
        Format f;
        TEST_ASSERT_TRUE(sensor(cat(p, blockFor(256, Kind::Sensor)), 256, chunk, taken, b, f) == Tail::Signed);
        TEST_ASSERT_TRUE(taken == p); // the payload is a prefix, byte-identical
        taken.clear();
        TEST_ASSERT_TRUE(sensor(p, 256, chunk, taken, b, f) == Tail::Unsigned);
        TEST_ASSERT_TRUE(taken == p);
    }
    // Split anywhere
    Bytes file = cat(p, blockFor(256, Kind::Sensor));
    for (size_t cut = 0; cut <= file.size(); cut++)
    {
        SensorTail t(256);
        size_t a = t.write(file.data(), cut);
        size_t c = t.write(file.data() + cut, file.size() - cut);
        TEST_ASSERT_EQUAL_UINT32(256, a + c);
        TEST_ASSERT_EQUAL_UINT32(cut < 256 ? cut : 256, a);
        Block b;
        Format f;
        TEST_ASSERT_TRUE(t.finish(b, f) == Tail::Signed);
    }
}

static void test_sensor_refuses_odd_tails(void)
{
    Bytes p = payload(256, 5);
    Bytes block = blockFor(256, Kind::Sensor);
    Block b;
    Format f;
    Bytes taken;
    // 127 / 129 bytes after the payload
    TEST_ASSERT_TRUE(sensor(cat(p, Bytes(block.begin(), block.end() - 1)), 256, 64, taken, b, f) == Tail::BadBlock);
    taken.clear();
    TEST_ASSERT_TRUE(sensor(cat(cat(p, block), Bytes{0}), 256, 64, taken, b, f) == Tail::BadBlock);
    taken.clear();
    // Far more than a block (never buffered beyond 128 B)
    TEST_ASSERT_TRUE(sensor(cat(p, payload(5000)), 256, 300, taken, b, f) == Tail::BadBlock);
    TEST_ASSERT_TRUE(taken == p);
    taken.clear();
    // 128 bytes without the magic
    TEST_ASSERT_TRUE(sensor(cat(p, payload(128, 77)), 256, 64, taken, b, f) == Tail::BadBlock);
    TEST_ASSERT_TRUE(f == Format::NoMagic);
    taken.clear();
    // Block naming another payload length
    TEST_ASSERT_TRUE(sensor(cat(p, blockFor(255, Kind::Sensor)), 256, 64, taken, b, f) == Tail::BadBlock);
    taken.clear();
    // Malformed block
    block[121] = 1;
    TEST_ASSERT_TRUE(sensor(cat(p, block), 256, 64, taken, b, f) == Tail::BadBlock);
    TEST_ASSERT_TRUE(f == Format::BadReserved);
    taken.clear();
    // Truncated payload: unsigned by the tail, but payloadComplete() tells
    // the caller to refuse it by its length first
    TEST_ASSERT_TRUE(sensor(Bytes(p.begin(), p.end() - 1), 256, 64, taken, b, f) == Tail::Unsigned);
    SensorTail cut(256);
    cut.write(p.data(), 255);
    TEST_ASSERT_FALSE(cut.payloadComplete());
    cut.write(p.data() + 255, 1);
    TEST_ASSERT_TRUE(cut.payloadComplete());
}

// The payload length is fixed at construction: no default constructor, no
// setter that could change it in the middle of a stream
static_assert(!std::is_default_constructible<SensorTail>::value, "payload length is mandatory");
static_assert(!std::is_convertible<uint32_t, SensorTail>::value, "explicit");

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip);
    RUN_TEST(test_malformed_blocks);
    RUN_TEST(test_versions);
    RUN_TEST(test_binding);
    RUN_TEST(test_ring_signed_all_chunkings);
    RUN_TEST(test_ring_block_split_at_every_offset);
    RUN_TEST(test_ring_unsigned_passthrough);
    RUN_TEST(test_ring_tails_of_127_128_129);
    RUN_TEST(test_ring_bad_blocks);
    RUN_TEST(test_sensor_signed_and_unsigned);
    RUN_TEST(test_sensor_refuses_odd_tails);
    return UNITY_END();
}
