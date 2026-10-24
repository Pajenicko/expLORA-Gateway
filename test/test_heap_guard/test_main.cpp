/**
 * Native unit tests for HeapGuard (memory guards on the usable 8-bit heap).
 *
 * Pins the calibration against the heap measured on the bench (Pro idle
 * 43,752 / 36,852 B, Lite idle ~109 kB): no guard fires at idle for any
 * page, the concurrency limit and the growth of running responses refuse
 * the third / an oversized request, the measured failure point (5,684 /
 * 4,340 B) is refused, and the TLS check follows the measured session
 * peak (Lite admitted at idle, refused under load; Pro refused at idle).
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Hardware/HeapGuard.h"

using HeapGuard::Admit;
using HeapGuard::admit;
using HeapGuard::Heap;
using HeapGuard::stringPagePeak;
using HeapGuard::STREAMED_PAGE_BLOCK;
using HeapGuard::STREAMED_PAGE_PEAK;

static const Heap PRO_IDLE = {43752, 36852};
static const Heap LITE_IDLE = {109188, 61428};
static const unsigned PRO_MAX = 2;
static const unsigned LITE_MAX = 4;

// Page sizes as the handlers estimate them
static const size_t HOME_20_SENSORS = 4096 + 250 * 20;
static const size_t GEN2_DETAIL = 9000;
static const size_t CONFIG_PAGE = 8192;

void setUp(void) {}
void tearDown(void) {}

static void test_string_page_peak(void)
{
    TEST_ASSERT_EQUAL_UINT32(8000, stringPagePeak(0));
    TEST_ASSERT_EQUAL_UINT32(2 * 9096 + 8000, stringPagePeak(HOME_20_SENSORS));
}

static void test_idle_never_refused(void)
{
    const Heap boards[] = {PRO_IDLE, LITE_IDLE};
    for (const Heap &h : boards)
    {
        TEST_ASSERT_EQUAL(Admit::Ok, admit(h, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 0, PRO_MAX));
        TEST_ASSERT_EQUAL(Admit::Ok, admit(h, stringPagePeak(HOME_20_SENSORS), HOME_20_SENSORS, 0, PRO_MAX));
        TEST_ASSERT_EQUAL(Admit::Ok, admit(h, stringPagePeak(GEN2_DETAIL), GEN2_DETAIL, 0, PRO_MAX));
        TEST_ASSERT_EQUAL(Admit::Ok, admit(h, stringPagePeak(CONFIG_PAGE), CONFIG_PAGE, 0, PRO_MAX));
        TEST_ASSERT_EQUAL(Admit::Ok, admit(h, 20000, 12000, 0, PRO_MAX)); // /api
    }
    // Pro: the largest page still has > 9 kB of slack for idle drift
    TEST_ASSERT_TRUE(PRO_IDLE.free >= stringPagePeak(HOME_20_SENSORS) + HeapGuard::BASE_RESERVE + 9000);
}

static void test_failure_point_refused(void)
{
    // 8-bit heap measured while connections were reset on the Pro
    const Heap failing = {5684, 4340};
    TEST_ASSERT_EQUAL(Admit::LowMemory, admit(failing, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 0, PRO_MAX));
    TEST_ASSERT_EQUAL(Admit::LowMemory, admit(failing, stringPagePeak(1000), 1000, 0, PRO_MAX));
    // The guard view before the fix (internal heap incl. 32-bit-only IRAM)
    // would pass; the 8-bit view of the same moment does not
    TEST_ASSERT_EQUAL(Admit::Ok, admit({38876, 32756}, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 0, PRO_MAX));
}

static void test_concurrency_limit(void)
{
    TEST_ASSERT_EQUAL(Admit::Busy, admit(LITE_IDLE, 1000, 100, PRO_MAX, PRO_MAX));
    TEST_ASSERT_EQUAL(Admit::Busy, admit(LITE_IDLE, 1000, 100, LITE_MAX, LITE_MAX));
    TEST_ASSERT_EQUAL(Admit::Ok, admit(LITE_IDLE, 1000, 100, LITE_MAX - 1, LITE_MAX));
    // Busy wins over low memory (the counter, not the heap, is the reason)
    TEST_ASSERT_EQUAL(Admit::Busy, admit({0, 0}, 1000, 100, PRO_MAX, PRO_MAX));
}

static void test_running_responses_count(void)
{
    // Pro: one streamed page running (~7.5 kB measured per page) - a second
    // streamed page fits next to it, the largest string page does not
    Heap oneRunning = {PRO_IDLE.free - 7500, PRO_IDLE.largest};
    TEST_ASSERT_EQUAL(Admit::Ok, admit(oneRunning, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 1, PRO_MAX));
    TEST_ASSERT_EQUAL(Admit::LowMemory,
                      admit(oneRunning, stringPagePeak(HOME_20_SENSORS), HOME_20_SENSORS, 1, PRO_MAX));
    // The growth reserve of the running ones is exact at the boundary
    size_t need = STREAMED_PAGE_PEAK + HeapGuard::BASE_RESERVE + 3 * HeapGuard::ACTIVE_GROWTH;
    TEST_ASSERT_EQUAL(Admit::Ok, admit({need, 65535}, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 3, LITE_MAX));
    TEST_ASSERT_EQUAL(Admit::LowMemory,
                      admit({need - 1, 65535}, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 3, LITE_MAX));
    // Worst case of two admitted streamed pages on the Pro: both at their
    // peak still leave the base reserve
    TEST_ASSERT_TRUE(PRO_IDLE.free >= 2 * STREAMED_PAGE_PEAK + HeapGuard::BASE_RESERVE);
}

static void test_largest_block(void)
{
    TEST_ASSERT_EQUAL(Admit::LowMemory, admit({100000, 6143}, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 0, 4));
    TEST_ASSERT_EQUAL(Admit::Ok, admit({100000, 6144}, STREAMED_PAGE_PEAK, STREAMED_PAGE_BLOCK, 0, 4));
}

static void test_tls(void)
{
    // Measured peak of a whole HTTPS session (Lite, bench 2) + base reserve;
    // the two record buffers are separate allocations: one must fit a block
    TEST_ASSERT_TRUE(HeapGuard::TLS_PEAK >= 109588 - 58448);
    TEST_ASSERT_EQUAL_UINT32(HeapGuard::TLS_PEAK + HeapGuard::BASE_RESERVE, HeapGuard::TLS_MIN_FREE);
    TEST_ASSERT_TRUE(HeapGuard::TLS_MIN_BLOCK >= HeapGuard::TLS_RECORD_BUFFER);
    TEST_ASSERT_TRUE(HeapGuard::TLS_MIN_BLOCK < 2 * HeapGuard::TLS_RECORD_BUFFER);
    TEST_ASSERT_TRUE(HeapGuard::tlsFits({HeapGuard::TLS_MIN_FREE, HeapGuard::TLS_MIN_BLOCK}));
    TEST_ASSERT_FALSE(HeapGuard::tlsFits({HeapGuard::TLS_MIN_FREE - 1, HeapGuard::TLS_MIN_BLOCK}));
    TEST_ASSERT_FALSE(HeapGuard::tlsFits({HeapGuard::TLS_MIN_FREE, HeapGuard::TLS_MIN_BLOCK - 1}));
    // Lite at idle: admitted, before the first session (109,588 / 65,524)
    // and after it (the largest block stays fragmented at 33,780; the
    // second check on the bench succeeded too)
    TEST_ASSERT_TRUE(HeapGuard::tlsFits(LITE_IDLE));
    TEST_ASSERT_TRUE(HeapGuard::tlsFits({109588, 65524}));
    TEST_ASSERT_TRUE(HeapGuard::tlsFits({109132, 33780}));
    // Lite under load (5 parallel /sensors: 52,196 / 31,732): refused, the
    // session would leave less than the base reserve
    TEST_ASSERT_FALSE(HeapGuard::tlsFits({52196, 31732}));
    // Pro at idle: refused (no handshake completed on the bench from
    // 43,024 B: low-water 1,164 B), also at the best idle sample
    TEST_ASSERT_FALSE(HeapGuard::tlsFits(PRO_IDLE));
    TEST_ASSERT_FALSE(HeapGuard::tlsFits({43024, 40948}));
    TEST_ASSERT_FALSE(HeapGuard::tlsFits({49196, 40948}));
}

// Persistent MQTT TLS session: handshake peak plus room next to the session
static void test_mqtt_tls(void)
{
    TEST_ASSERT_TRUE(HeapGuard::MQTT_TLS_MIN_FREE > HeapGuard::TLS_MIN_FREE);
    TEST_ASSERT_TRUE(HeapGuard::mqttTlsFits({HeapGuard::MQTT_TLS_MIN_FREE, HeapGuard::TLS_MIN_BLOCK}));
    TEST_ASSERT_FALSE(HeapGuard::mqttTlsFits({HeapGuard::MQTT_TLS_MIN_FREE - 1, 60000}));
    TEST_ASSERT_FALSE(HeapGuard::mqttTlsFits({100000, HeapGuard::TLS_MIN_BLOCK - 1}));
    // Pro bench (aa462a9): WiFi on, LTE off 67,840 / 61,428 left the web UI
    // unusable with the session - refused; WiFi off 81,648 / 77,812 (LTE on)
    // and 102,560 / 94,196 worked
    TEST_ASSERT_FALSE(HeapGuard::mqttTlsFits({67840, 61428}));
    TEST_ASSERT_FALSE(HeapGuard::mqttTlsFits({65616, 59380}));
    TEST_ASSERT_TRUE(HeapGuard::mqttTlsFits({81648, 77812}));
    TEST_ASSERT_TRUE(HeapGuard::mqttTlsFits({102560, 94196}));
    // Lite idle fits
    TEST_ASSERT_TRUE(HeapGuard::mqttTlsFits({109556, 65524}));
    // the transient check still admits the Pro with WiFi on and LTE off
    TEST_ASSERT_TRUE(HeapGuard::tlsFits({67840, 61428}));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_string_page_peak);
    RUN_TEST(test_idle_never_refused);
    RUN_TEST(test_failure_point_refused);
    RUN_TEST(test_concurrency_limit);
    RUN_TEST(test_running_responses_count);
    RUN_TEST(test_largest_block);
    RUN_TEST(test_tls);
    RUN_TEST(test_mqtt_tls);
    return UNITY_END();
}
