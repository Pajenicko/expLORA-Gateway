/**
 * expLORA Gateway - native tests for Update/PartitionTable.h: the partition
 * table parser / classifier (Lite v1, Lite v2, Pro, unknown, damaged), the
 * v2 table builder against gen_esp32part.py's output (fixtures.h, made by
 * gen_fixtures.py from the committed CSV files), the migration marker and
 * the migration step decisions.
 */

#include <unity.h>
#include <cstring>
#include <vector>

#include "../../src/Update/PartitionTable.h"
#include "fixtures.h"

using namespace PartitionTable;

void setUp(void) {}
void tearDown(void) {}

// --- MD5 (RFC 1321), reference implementation for the host ---------------------------

static uint32_t rol(uint32_t x, int c)
{
    return (x << c) | (x >> (32 - c));
}

static void md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::vector<uint8_t> m(data, data + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56)
    {
        m.push_back(0);
    }
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
    {
        m.push_back((uint8_t)(bits >> (8 * i)));
    }
    for (size_t off = 0; off < m.size(); off += 64)
    {
        uint32_t w[16];
        for (int i = 0; i < 16; i++)
        {
            w[i] = le32(&m[off + 4 * i]);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++)
        {
            uint32_t f;
            int g;
            if (i < 16)
            {
                f = (b & c) | (~b & d);
                g = i;
            }
            else if (i < 32)
            {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            }
            else if (i < 48)
            {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            }
            else
            {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const uint32_t t = d;
            d = c;
            c = b;
            b = b + rol(a + f + K[i] + w[g], R[i]);
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    for (int i = 0; i < 4; i++)
    {
        put32(out + 4 * i, h[i]);
    }
}

static std::vector<uint8_t> copyOf(const uint8_t *t)
{
    return std::vector<uint8_t>(t, t + MAX_LEN);
}

// The MD5 entry after n entries is recomputed (a valid table with other contents)
static void reseal(std::vector<uint8_t> &t, size_t n)
{
    md5(t.data(), n * ENTRY_LEN, t.data() + n * ENTRY_LEN + 16);
}

// --- MD5 itself ------------------------------------------------------------------------

static void test_md5_reference()
{
    uint8_t d[16];
    md5((const uint8_t *)"abc", 3, d);
    const uint8_t want[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
                              0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, d, 16);
}

// --- classify --------------------------------------------------------------------------

static void test_known_layouts()
{
    TEST_ASSERT_EQUAL((int)Layout::LiteV1, (int)classify(LITE_V1_BIN, MAX_LEN, md5));
    TEST_ASSERT_EQUAL((int)Layout::LiteV2, (int)classify(LITE_V2_BIN, MAX_LEN, md5));
    TEST_ASSERT_EQUAL((int)Layout::Pro, (int)classify(PRO_BIN, MAX_LEN, md5));
}

static void test_whole_sector_accepted()
{
    // The firmware reads the whole 4 kB sector: bytes after MAX_LEN are not looked at
    std::vector<uint8_t> s(SECTOR, 0xFF);
    memcpy(s.data(), LITE_V1_BIN, MAX_LEN);
    s[MAX_LEN + 5] = 0x00;
    TEST_ASSERT_EQUAL((int)Layout::LiteV1, (int)classify(s.data(), s.size(), md5));
}

static void test_build_matches_gen_esp32part()
{
    uint8_t out[MAX_LEN];
    build(LITE_V2, LITE_V2_N, md5, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(LITE_V2_BIN, out, MAX_LEN);
    build(LITE_V1, LITE_V1_N, md5, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(LITE_V1_BIN, out, MAX_LEN);
    build(PRO_16MB, PRO_16MB_N, md5, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(PRO_BIN, out, MAX_LEN);
}

static void test_v2_layout_rules()
{
    // nvs / otadata / coredump / spiffs as in v1; slots equal; app1 ends at 8 MB
    TEST_ASSERT_EQUAL_UINT32(LITE_V1[0].offset, LITE_V2[0].offset);
    TEST_ASSERT_EQUAL_UINT32(LITE_V1[0].size, LITE_V2[0].size);
    TEST_ASSERT_EQUAL_UINT32(LITE_V1[1].offset, LITE_V2[1].offset);
    TEST_ASSERT_EQUAL_UINT32(LITE_V1[4].offset, LITE_V2[3].offset);
    TEST_ASSERT_EQUAL_UINT32(LITE_V1[5].offset, LITE_V2[4].offset);
    TEST_ASSERT_EQUAL_UINT32(LITE_V1[5].size, LITE_V2[4].size);
    TEST_ASSERT_EQUAL_UINT32(LITE_V2[2].size, LITE_V2[5].size);
    TEST_ASSERT_EQUAL_UINT32(FLASH_MIN, LITE_V2[5].offset + LITE_V2[5].size);
    // The new app0 starts where v1 app0 does (the running image keeps its place)
    TEST_ASSERT_EQUAL_UINT32(LITE_V1[2].offset, LITE_V2[2].offset);
    TEST_ASSERT_EQUAL_STRING("v2 (2752K slots)", layoutText(Layout::LiteV2));
    TEST_ASSERT_EQUAL_UINT32(2752u * 1024, LITE_V2[2].size);
    TEST_ASSERT_EQUAL_UINT32(1984u * 1024, LITE_V1[2].size);
}

static void test_corrupted_md5()
{
    std::vector<uint8_t> t = copyOf(LITE_V1_BIN);
    t[LITE_V1_N * ENTRY_LEN + 20] ^= 0x01; // a bit of the stored MD5
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(t.data(), MAX_LEN, md5));
    t = copyOf(LITE_V1_BIN);
    t[3 * ENTRY_LEN + 9] ^= 0x10; // app1 size: the MD5 no longer matches
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(t.data(), MAX_LEN, md5));
    t = copyOf(LITE_V2_BIN);
    t[LITE_V2_N * ENTRY_LEN + 5] = 0x00; // MD5 entry padding
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(t.data(), MAX_LEN, md5));
}

static void test_bytes_after_end()
{
    std::vector<uint8_t> t = copyOf(LITE_V2_BIN);
    t[MAX_LEN - 1] = 0x00;
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(t.data(), MAX_LEN, md5));
    t = copyOf(LITE_V2_BIN);
    t[(LITE_V2_N + 1) * ENTRY_LEN] = 0xAA; // an entry after the MD5 entry
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(t.data(), MAX_LEN, md5));
}

static void test_bad_magic()
{
    std::vector<uint8_t> t = copyOf(LITE_V1_BIN);
    t[2 * ENTRY_LEN + 1] = 0x51;
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(t.data(), MAX_LEN, md5));
}

static void test_unknown_valid_tables()
{
    // Valid MD5, one size changed
    std::vector<uint8_t> t = copyOf(LITE_V1_BIN);
    put32(t.data() + 6 * ENTRY_LEN + 8, 0x0A0000);
    reseal(t, LITE_V1_N);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(t.data(), MAX_LEN, md5));
    // A label differs
    t = copyOf(LITE_V1_BIN);
    t[6 * ENTRY_LEN + 12] = 'L';
    reseal(t, LITE_V1_N);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(t.data(), MAX_LEN, md5));
    // Non-NUL after the label text
    t = copyOf(LITE_V2_BIN);
    t[2 * ENTRY_LEN + 12 + 15] = 'x';
    reseal(t, LITE_V2_N);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(t.data(), MAX_LEN, md5));
    // A flag set (encrypted)
    t = copyOf(LITE_V1_BIN);
    t[5 * ENTRY_LEN + 28] = 0x01;
    reseal(t, LITE_V1_N);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(t.data(), MAX_LEN, md5));
    // v1 without its last entry (6 entries)
    uint8_t six[MAX_LEN];
    build(LITE_V1, LITE_V1_N - 1, md5, six);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(six, MAX_LEN, md5));
    // v2 entries in another order
    Entry swapped[LITE_V2_N];
    memcpy(swapped, LITE_V2, sizeof(swapped));
    std::swap(swapped[3], swapped[4]);
    uint8_t other[MAX_LEN];
    build(swapped, LITE_V2_N, md5, other);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(other, MAX_LEN, md5));
}

static void test_no_md5_entry()
{
    // Entries only (an old generator without MD5): never acted on
    std::vector<uint8_t> t = copyOf(LITE_V1_BIN);
    memset(t.data() + LITE_V1_N * ENTRY_LEN, 0xFF, ENTRY_LEN);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(t.data(), MAX_LEN, md5));
    // Erased sector
    std::vector<uint8_t> e(MAX_LEN, 0xFF);
    TEST_ASSERT_EQUAL((int)Layout::Unknown, (int)classify(e.data(), MAX_LEN, md5));
    // Zeroed sector
    std::vector<uint8_t> z(MAX_LEN, 0x00);
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(z.data(), MAX_LEN, md5));
}

static void test_short_buffer()
{
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(LITE_V1_BIN, MAX_LEN - 1, md5));
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(nullptr, MAX_LEN, md5));
    TEST_ASSERT_EQUAL((int)Layout::Damaged, (int)classify(LITE_V1_BIN, MAX_LEN, nullptr));
}

// --- Marker ----------------------------------------------------------------------------

static void test_marker_round_trip()
{
    char t[32];
    Marker m, back;
    m.stage = Stage::Copy;
    m.tries = 2;
    size_t n = formatMarker(m, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("copy 2\n", t);
    TEST_ASSERT_TRUE(parseMarker(t, n, back));
    TEST_ASSERT_EQUAL((int)Stage::Copy, (int)back.stage);
    TEST_ASSERT_EQUAL_UINT8(2, back.tries);
    m.stage = Stage::Table;
    m.tries = 1;
    n = formatMarker(m, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("table 1\n", t);
    TEST_ASSERT_TRUE(parseMarker(t, n, back));
    TEST_ASSERT_EQUAL((int)Stage::Table, (int)back.stage);
    Marker f;
    f.stage = Stage::Failed;
    strcpy(f.version, "2.0.1");
    n = formatMarker(f, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("fail 2.0.1\n", t);
    TEST_ASSERT_TRUE(parseMarker(t, n, back));
    TEST_ASSERT_EQUAL((int)Stage::Failed, (int)back.stage);
    TEST_ASSERT_EQUAL_STRING("2.0.1", back.version);
}

static void test_marker_rejects()
{
    Marker m;
    const char *bad[] = {"", "copy", "copy ", "copy 0", "copy 12", "copy x", "table 0", "fail ", "fail 2.0.1a",
                         "fail 12345678901234567", "COPY 1", "copy  1", "erase 1"};
    for (const char *s : bad)
    {
        TEST_ASSERT_FALSE_MESSAGE(parseMarker(s, strlen(s), m), s);
        TEST_ASSERT_EQUAL((int)Stage::None, (int)m.stage);
    }
    TEST_ASSERT_TRUE(parseMarker("table 3\r\n", 9, m));
    TEST_ASSERT_EQUAL_UINT8(3, m.tries);
    char t[8];
    TEST_ASSERT_EQUAL(0u, formatMarker(m, t, sizeof(t))); // too small a buffer
}

static void test_next_marker()
{
    Marker none;
    Marker a = nextMarker(none, Stage::Copy);
    TEST_ASSERT_EQUAL((int)Stage::Copy, (int)a.stage);
    TEST_ASSERT_EQUAL_UINT8(1, a.tries);
    a = nextMarker(a, Stage::Copy);
    TEST_ASSERT_EQUAL_UINT8(2, a.tries);
    Marker b = nextMarker(a, Stage::Table);
    TEST_ASSERT_EQUAL((int)Stage::Table, (int)b.stage);
    TEST_ASSERT_EQUAL_UINT8(1, b.tries);
    Marker f;
    f.stage = Stage::Failed;
    TEST_ASSERT_EQUAL_UINT8(1, nextMarker(f, Stage::Table).tries);
}

// --- Decisions -------------------------------------------------------------------------

static BootIn liteV1(Slot running, RunState st)
{
    BootIn in;
    in.board = true;
    in.flashOk = true;
    in.layout = Layout::LiteV1;
    in.running = running;
    in.state = st;
    in.bootIsRunning = true;
    return in;
}

static void test_decide_not_lite()
{
    BootIn in = liteV1(Slot::App1, RunState::Valid);
    in.board = false; // the Pro build
    TEST_ASSERT_EQUAL((int)Action::None, (int)decide(in));
    in.board = true;
    in.layout = Layout::Pro; // a Lite build on a Pro table: never acted on
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
}

static void test_decide_unknown_damaged_small()
{
    BootIn in = liteV1(Slot::App0, RunState::Valid);
    in.layout = Layout::Unknown;
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
    in.layout = Layout::Damaged;
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
    in = liteV1(Slot::App0, RunState::Valid);
    in.flashOk = false;
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
}

static void test_decide_v2()
{
    BootIn in = liteV1(Slot::App0, RunState::Valid);
    in.layout = Layout::LiteV2;
    TEST_ASSERT_EQUAL((int)Action::None, (int)decide(in));
    in.marker.stage = Stage::Table;
    in.marker.tries = 1;
    TEST_ASSERT_EQUAL((int)Action::Finished, (int)decide(in));
    in.marker.stage = Stage::Copy; // a leftover: removed with the log line
    TEST_ASSERT_EQUAL((int)Action::Finished, (int)decide(in));
    in.marker.stage = Stage::Failed;
    TEST_ASSERT_EQUAL((int)Action::None, (int)decide(in));
    // Pending or on app1, v2 never acts
    in = liteV1(Slot::App1, RunState::Pending);
    in.layout = Layout::LiteV2;
    TEST_ASSERT_EQUAL((int)Action::None, (int)decide(in));
}

static void test_decide_steps()
{
    // Validated in app1: copy
    TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(liteV1(Slot::App1, RunState::Valid)));
    // Validated in app0, otadata selects it: table
    TEST_ASSERT_EQUAL((int)Action::Table, (int)decide(liteV1(Slot::App0, RunState::Valid)));
    // Pending (new firmware): wait for the validation, in either slot
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)decide(liteV1(Slot::App0, RunState::Pending)));
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)decide(liteV1(Slot::App1, RunState::Pending)));
    // The copy started from app0 (pending, marker copy): accept
    BootIn in = liteV1(Slot::App0, RunState::Pending);
    in.marker.stage = Stage::Copy;
    in.marker.tries = 1;
    TEST_ASSERT_EQUAL((int)Action::Accept, (int)decide(in));
    // ... but a pending image in app1 with a copy marker waits
    in.running = Slot::App1;
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)decide(in));
    // Pending with a table marker (a new install after a failed table step): waits
    in = liteV1(Slot::App0, RunState::Pending);
    in.marker.stage = Stage::Table;
    in.marker.tries = 1;
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)decide(in));
    // app0 runs but otadata selects another slot: nothing
    in = liteV1(Slot::App0, RunState::Valid);
    in.bootIsRunning = false;
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
    // Running from neither slot / odd state
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(liteV1(Slot::Other, RunState::Valid)));
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(liteV1(Slot::App0, RunState::Other)));
}

static void test_decide_bounded()
{
    BootIn in = liteV1(Slot::App1, RunState::Valid);
    for (uint8_t t = 1; t < MAX_TRIES; t++)
    {
        in.marker.stage = Stage::Copy;
        in.marker.tries = t;
        TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(in));
    }
    in.marker.tries = MAX_TRIES;
    TEST_ASSERT_EQUAL((int)Action::GiveUp, (int)decide(in));
    // A table marker does not count against the copy
    in.marker.stage = Stage::Table;
    TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(in));
    in = liteV1(Slot::App0, RunState::Valid);
    in.marker.stage = Stage::Table;
    in.marker.tries = MAX_TRIES - 1;
    TEST_ASSERT_EQUAL((int)Action::Table, (int)decide(in));
    in.marker.tries = MAX_TRIES;
    TEST_ASSERT_EQUAL((int)Action::GiveUp, (int)decide(in));
    // Copy marker at its limit, now in app0 validated: the table step starts at 1
    in.marker.stage = Stage::Copy;
    TEST_ASSERT_EQUAL((int)Action::Table, (int)decide(in));
}

static void test_decide_failed_version()
{
    BootIn in = liteV1(Slot::App0, RunState::Valid);
    in.marker.stage = Stage::Failed;
    strcpy(in.marker.version, "2.0.1");
    in.markerThisVersion = true;
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
    in.markerThisVersion = false; // another firmware gave up: this one tries again
    TEST_ASSERT_EQUAL((int)Action::Table, (int)decide(in));
    in.running = Slot::App1;
    TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(in));
}

// An activation recorded for another slot is reported at this boot (a
// rollback, an activation that did not start) before Copy / Table touch
// that slot: wait (process() restarts once the record is settled)
static void test_decide_ota_record_other()
{
    BootIn in = liteV1(Slot::App1, RunState::Valid);
    in.otaRecordOther = true;
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)decide(in));
    in.marker.stage = Stage::Copy;
    in.marker.tries = MAX_TRIES; // waits before giving up too (no step is started)
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)decide(in));
    in = liteV1(Slot::App0, RunState::Valid);
    in.otaRecordOther = true;
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)decide(in));
    // Settled at the next boot: the steps run
    in.otaRecordOther = false;
    TEST_ASSERT_EQUAL((int)Action::Table, (int)decide(in));
    in.running = Slot::App1;
    TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(in));
    // Nothing else changes: the copy is still accepted, v2 / notes stay
    in = liteV1(Slot::App0, RunState::Pending);
    in.otaRecordOther = true;
    in.marker.stage = Stage::Copy;
    in.marker.tries = 1;
    TEST_ASSERT_EQUAL((int)Action::Accept, (int)decide(in));
    in = liteV1(Slot::App0, RunState::Valid);
    in.otaRecordOther = true;
    in.bootIsRunning = false;
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
    in.layout = Layout::LiteV2;
    TEST_ASSERT_EQUAL((int)Action::None, (int)decide(in));
    in = liteV1(Slot::App0, RunState::Valid);
    in.otaRecordOther = true;
    in.marker.stage = Stage::Failed;
    in.markerThisVersion = true;
    TEST_ASSERT_EQUAL((int)Action::Note, (int)decide(in));
}

// Every reset between the steps leads on: a simulation of the boots with
// the flash state of each step (no table write in it - the only window that
// is not covered)
static void test_resets_between_steps()
{
    // Start: v1, validated in app1, no marker
    BootIn in = liteV1(Slot::App1, RunState::Valid);
    TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(in));
    in.marker = nextMarker(in.marker, Stage::Copy); // marker written, then a reset during the copy
    TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(in)); // copied again
    in.marker = nextMarker(in.marker, Stage::Copy);
    // Copy done, boot set to app0 (NEW) -> bootloader starts app0 PENDING_VERIFY
    BootIn a0 = in;
    a0.running = Slot::App0;
    a0.state = RunState::Pending;
    TEST_ASSERT_EQUAL((int)Action::Accept, (int)decide(a0));
    // A reset before the accept: bootloader aborts app0, app1 runs again
    TEST_ASSERT_EQUAL((int)Action::Copy, (int)decide(in));
    in.marker = nextMarker(in.marker, Stage::Copy);
    TEST_ASSERT_EQUAL_UINT8(MAX_TRIES, in.marker.tries);
    // Accepted: valid in app0
    a0.marker = in.marker;
    a0.state = RunState::Valid;
    TEST_ASSERT_EQUAL((int)Action::Table, (int)decide(a0));
    a0.marker = nextMarker(a0.marker, Stage::Table);
    // A reset after the marker, before the table write: again
    TEST_ASSERT_EQUAL((int)Action::Table, (int)decide(a0));
    a0.marker = nextMarker(a0.marker, Stage::Table);
    // Table written: v2, marker table -> finished once
    a0.layout = Layout::LiteV2;
    TEST_ASSERT_EQUAL((int)Action::Finished, (int)decide(a0));
    a0.marker = Marker();
    TEST_ASSERT_EQUAL((int)Action::None, (int)decide(a0));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_md5_reference);
    RUN_TEST(test_known_layouts);
    RUN_TEST(test_whole_sector_accepted);
    RUN_TEST(test_build_matches_gen_esp32part);
    RUN_TEST(test_v2_layout_rules);
    RUN_TEST(test_corrupted_md5);
    RUN_TEST(test_bytes_after_end);
    RUN_TEST(test_bad_magic);
    RUN_TEST(test_unknown_valid_tables);
    RUN_TEST(test_no_md5_entry);
    RUN_TEST(test_short_buffer);
    RUN_TEST(test_marker_round_trip);
    RUN_TEST(test_marker_rejects);
    RUN_TEST(test_next_marker);
    RUN_TEST(test_decide_not_lite);
    RUN_TEST(test_decide_unknown_damaged_small);
    RUN_TEST(test_decide_v2);
    RUN_TEST(test_decide_steps);
    RUN_TEST(test_decide_bounded);
    RUN_TEST(test_decide_failed_version);
    RUN_TEST(test_decide_ota_record_other);
    RUN_TEST(test_resets_between_steps);
    return UNITY_END();
}
