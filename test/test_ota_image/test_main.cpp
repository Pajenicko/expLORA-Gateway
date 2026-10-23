/**
 * Native tests for the gateway's OTA image validation (src/Protocol/OtaImage.h,
 * docs/protocol-v2.md 14.5 / 10.1): CRC-32, the streaming pass in arbitrary
 * chunks, every rejection reason including the trailer flags rule.
 * Run:   pio test -e native -f test_ota_image
 */

#include <unity.h>
#include <vector>
#include "../../src/Protocol/OtaImage.h"

using namespace OtaImage;

void setUp(void) {}
void tearDown(void) {}

static void put32(std::vector<uint8_t> &d, size_t at, uint32_t v)
{
    d[at] = (uint8_t)v;
    d[at + 1] = (uint8_t)(v >> 8);
    d[at + 2] = (uint8_t)(v >> 16);
    d[at + 3] = (uint8_t)(v >> 24);
}

// A trailered image as scripts/image_trailer.py writes it
static std::vector<uint8_t> makeImage(uint32_t size, uint32_t flags, uint32_t load = LOAD_ADDRESS, uint16_t hw = 3,
                                      uint32_t fw = 0x000300)
{
    std::vector<uint8_t> d(size + TRAILER_SIZE, 0);
    for (uint32_t i = 0; i < size; i++)
    {
        d[i] = (uint8_t)(i * 7 + 3);
    }
    put32(d, SIZE_WORD_OFFSET, size);
    put32(d, size, TRAILER_MAGIC);
    d[size + 4] = 1;
    d[size + 5] = 0;
    d[size + 6] = (uint8_t)hw;
    d[size + 7] = (uint8_t)(hw >> 8);
    put32(d, size + 8, fw);
    put32(d, size + 12, size);
    put32(d, size + 16, load);
    put32(d, size + 20, flags);
    put32(d, size + 24, 0);
    put32(d, size + 28, crc32Update(0, d.data(), size + 28));
    return d;
}

static Error check(const std::vector<uint8_t> &d, Trailer &t, size_t chunk = 1000)
{
    Streamer s;
    for (size_t i = 0; i < d.size(); i += chunk)
    {
        size_t n = d.size() - i < chunk ? d.size() - i : chunk;
        s.feed(d.data() + i, n);
    }
    return validate(s, t);
}

static void test_crc32(void)
{
    const uint8_t v[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, crc32Update(0, v, sizeof(v)));
    // streaming in pieces
    uint32_t c = crc32Update(0, v, 4);
    c = crc32Update(c, v + 4, 5);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, c);
}

static void test_valid_image_any_chunking(void)
{
    std::vector<uint8_t> d = makeImage(92016, 0x101);
    const size_t chunks[] = {1, 3, 31, 32, 33, 1436, 4096, 200000};
    for (size_t c : chunks)
    {
        Trailer t;
        TEST_ASSERT_EQUAL((int)Error::None, (int)check(d, t, c));
        TEST_ASSERT_EQUAL(3, t.hwType);
        TEST_ASSERT_EQUAL_HEX32(0x000300, t.fw);
        TEST_ASSERT_EQUAL_HEX32(0x101, t.flags);
    }
}

static void test_rejections(void)
{
    Trailer t;
    // no OTA receiver (every image before M4b: flags 0)
    TEST_ASSERT_EQUAL((int)Error::NoOta, (int)check(makeImage(4096, 0), t));
    // unknown transfer version
    TEST_ASSERT_EQUAL((int)Error::OtaVersion, (int)check(makeImage(4096, 0x201), t));
    TEST_ASSERT_EQUAL((int)Error::OtaVersion, (int)check(makeImage(4096, 0x001), t));
    // linked for another address
    TEST_ASSERT_EQUAL((int)Error::Load, (int)check(makeImage(4096, 0x101, 0x0801E000), t));
    // damaged byte
    std::vector<uint8_t> d = makeImage(4096, 0x101);
    d[1000] ^= 0x40;
    TEST_ASSERT_EQUAL((int)Error::Crc, (int)check(d, t));
    // not a multiple of 8 (hand-made image): the sensor would refuse it
    TEST_ASSERT_EQUAL((int)Error::Align, (int)check(makeImage(4100, 0x101), t));
    // truncated / extra byte
    std::vector<uint8_t> e = makeImage(4096, 0x101);
    e.push_back(0);
    TEST_ASSERT_EQUAL((int)Error::Length, (int)check(e, t));
    // no trailer magic
    std::vector<uint8_t> m = makeImage(4096, 0x101);
    m[4096] = 0;
    TEST_ASSERT_EQUAL((int)Error::Magic, (int)check(m, t));
    // too short / too large
    std::vector<uint8_t> s(100, 0);
    TEST_ASSERT_EQUAL((int)Error::TooShort, (int)check(s, t));
    TEST_ASSERT_EQUAL((int)Error::TooLarge, (int)check(makeImage(114688, 0x101), t));
    TEST_ASSERT_EQUAL((int)Error::None, (int)check(makeImage(114688 - 32, 0x101), t));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32);
    RUN_TEST(test_valid_image_any_chunking);
    RUN_TEST(test_rejections);
    return UNITY_END();
}
