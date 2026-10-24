/**
 * Native unit tests for LogRing (the logger's byte ring, Data/LogRing.h).
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <deque>
#include <string>
#include <random>
#include "../../src/Data/LogRing.h"

void setUp(void) {}
void tearDown(void) {}

struct Ref
{
    uint8_t level;
    std::string time;
    std::string msg;
};

static LogRing::Time at(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s)
{
    LogRing::Time t;
    t.valid = true;
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = h;
    t.minute = mi;
    t.second = s;
    return t;
}

static Ref get(const LogRing &r, size_t n)
{
    uint8_t level = 0xEE;
    char time[LogRing::TIME_TEXT];
    char msg[400];
    size_t len = 0;
    TEST_ASSERT_TRUE(r.get(n, level, time, msg, sizeof(msg), len));
    TEST_ASSERT_EQUAL_UINT32(strlen(msg), len);
    return {level, time, msg};
}

static void test_empty(void)
{
    uint8_t buf[64];
    LogRing r;
    r.init(buf, sizeof(buf));
    uint8_t level;
    char time[LogRing::TIME_TEXT];
    char msg[8];
    size_t len;
    TEST_ASSERT_EQUAL_UINT32(0, r.count());
    TEST_ASSERT_FALSE(r.get(0, level, time, msg, sizeof(msg), len));
}

static void test_time_format_and_order(void)
{
    uint8_t buf[256];
    LogRing r;
    r.init(buf, sizeof(buf));
    TEST_ASSERT_TRUE(r.push(2, LogRing::Time(), "boot", 4));
    TEST_ASSERT_TRUE(r.push(1, at(2026, 10, 3, 7, 5, 9), "second", 6));
    TEST_ASSERT_TRUE(r.push(0, at(2255, 12, 31, 23, 59, 59), "", 0));
    TEST_ASSERT_EQUAL_UINT32(3, r.count());
    Ref a = get(r, 0), b = get(r, 1), c = get(r, 2);
    TEST_ASSERT_EQUAL_STRING("2255-12-31 23:59:59", a.time.c_str());
    TEST_ASSERT_EQUAL_STRING("", a.msg.c_str());
    TEST_ASSERT_EQUAL_UINT8(0, a.level);
    TEST_ASSERT_EQUAL_STRING("2026-10-03 07:05:09", b.time.c_str());
    TEST_ASSERT_EQUAL_STRING("second", b.msg.c_str());
    TEST_ASSERT_EQUAL_UINT8(1, b.level);
    TEST_ASSERT_EQUAL_STRING("[Time not set]", c.time.c_str());
    TEST_ASSERT_EQUAL_STRING("boot", c.msg.c_str());
    TEST_ASSERT_EQUAL_UINT8(2, c.level);
    uint8_t level;
    char time[LogRing::TIME_TEXT];
    char msg[8];
    size_t len;
    TEST_ASSERT_FALSE(r.get(3, level, time, msg, sizeof(msg), len));
}

static void test_sizes(void)
{
    uint8_t buf[256];
    LogRing r;
    r.init(buf, sizeof(buf));
    r.push(2, LogRing::Time(), "abc", 3);
    TEST_ASSERT_EQUAL_UINT32(4 + 3 + 2, r.bytesUsed());
    r.push(2, at(2030, 1, 1, 0, 0, 0), "abc", 3);
    TEST_ASSERT_EQUAL_UINT32(9 + 4 + 6 + 3 + 2, r.bytesUsed());
}

static void test_drops_oldest_and_wraps(void)
{
    uint8_t buf[40];
    LogRing r;
    r.init(buf, sizeof(buf));
    // 4 + 10 + 2 = 16 B each: two fit, the third drops the first
    TEST_ASSERT_TRUE(r.push(1, LogRing::Time(), "0123456789", 10));
    TEST_ASSERT_TRUE(r.push(2, LogRing::Time(), "abcdefghij", 10));
    TEST_ASSERT_EQUAL_UINT32(2, r.count());
    TEST_ASSERT_TRUE(r.push(3, LogRing::Time(), "ABCDEFGHIJ", 10)); // wraps around the end
    TEST_ASSERT_EQUAL_UINT32(2, r.count());
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", get(r, 0).msg.c_str());
    TEST_ASSERT_EQUAL_STRING("abcdefghij", get(r, 1).msg.c_str());
    // A record that fills the whole ring leaves only itself
    char big[34];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_TRUE(r.push(4, LogRing::Time(), big, sizeof(big)));
    TEST_ASSERT_EQUAL_UINT32(1, r.count());
    TEST_ASSERT_EQUAL_UINT32(40, r.bytesUsed());
    TEST_ASSERT_EQUAL_STRING(std::string(34, 'x').c_str(), get(r, 0).msg.c_str());
}

static void test_refuses_what_never_fits(void)
{
    uint8_t buf[32];
    LogRing r;
    r.init(buf, sizeof(buf));
    r.push(1, LogRing::Time(), "keep", 4);
    char big[27];
    memset(big, 'y', sizeof(big));
    TEST_ASSERT_FALSE(r.push(1, LogRing::Time(), big, sizeof(big))); // 33 > 32
    TEST_ASSERT_FALSE(r.push(1, at(1999, 1, 1, 0, 0, 0), "a", 1));
    TEST_ASSERT_FALSE(r.push(1, at(2256, 1, 1, 0, 0, 0), "a", 1));
    TEST_ASSERT_EQUAL_UINT32(1, r.count());
    TEST_ASSERT_EQUAL_STRING("keep", get(r, 0).msg.c_str());
    LogRing none;
    TEST_ASSERT_FALSE(none.push(1, LogRing::Time(), "a", 1));
}

static void test_message_cut_to_caller_buffer(void)
{
    uint8_t buf[64];
    LogRing r;
    r.init(buf, sizeof(buf));
    r.push(2, LogRing::Time(), "abcdefgh", 8);
    uint8_t level;
    char time[LogRing::TIME_TEXT];
    char msg[5];
    size_t len;
    TEST_ASSERT_TRUE(r.get(0, level, time, msg, sizeof(msg), len));
    TEST_ASSERT_EQUAL_UINT32(8, len);
    TEST_ASSERT_EQUAL_STRING("abcd", msg);
}

static void test_clear(void)
{
    uint8_t buf[64];
    LogRing r;
    r.init(buf, sizeof(buf));
    r.push(2, LogRing::Time(), "abc", 3);
    r.clear();
    TEST_ASSERT_EQUAL_UINT32(0, r.count());
    TEST_ASSERT_EQUAL_UINT32(0, r.bytesUsed());
    r.push(2, LogRing::Time(), "def", 3);
    TEST_ASSERT_EQUAL_STRING("def", get(r, 0).msg.c_str());
}

// Random lengths and times against a model that keeps the newest records
// whose sizes fit into the ring
static void test_random_against_model(void)
{
    for (size_t cap : {64u, 97u, 256u, 1000u})
    {
        std::vector<uint8_t> buf(cap);
        LogRing r;
        r.init(buf.data(), cap);
        std::deque<Ref> model;
        std::deque<size_t> sizes;
        size_t used = 0;
        std::mt19937 rng(cap);
        for (int i = 0; i < 3000; i++)
        {
            size_t len = rng() % 70;
            std::string m;
            for (size_t k = 0; k < len; k++)
            {
                m += (char)('a' + rng() % 26);
            }
            bool tv = rng() % 3 != 0;
            LogRing::Time t = tv ? at(2000 + rng() % 256, 1 + rng() % 12, 1 + rng() % 28, rng() % 24, rng() % 60,
                                      rng() % 60)
                                 : LogRing::Time();
            uint8_t level = rng() % 5;
            size_t size = LogRing::recordSize(len, tv);
            bool ok = r.push(level, t, m.data(), m.size());
            TEST_ASSERT_EQUAL(size <= cap, ok);
            if (!ok)
            {
                continue;
            }
            while (cap - used < size)
            {
                used -= sizes.front();
                sizes.pop_front();
                model.pop_front();
            }
            char ts[24];
            if (tv)
            {
                snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u", (unsigned)t.year, (unsigned)t.month,
                         (unsigned)t.day, (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
            }
            else
            {
                snprintf(ts, sizeof(ts), "[Time not set]");
            }
            model.push_back({level, ts, m});
            sizes.push_back(size);
            used += size;
            TEST_ASSERT_EQUAL_UINT32(model.size(), r.count());
            TEST_ASSERT_EQUAL_UINT32(used, r.bytesUsed());
            if (i % 7 == 0)
            {
                for (size_t n = 0; n < model.size(); n++)
                {
                    const Ref &e = model[model.size() - 1 - n];
                    Ref g = get(r, n);
                    TEST_ASSERT_EQUAL_UINT8(e.level, g.level);
                    TEST_ASSERT_EQUAL_STRING(e.time.c_str(), g.time.c_str());
                    TEST_ASSERT_EQUAL_STRING(e.msg.c_str(), g.msg.c_str());
                }
            }
        }
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_time_format_and_order);
    RUN_TEST(test_sizes);
    RUN_TEST(test_drops_oldest_and_wraps);
    RUN_TEST(test_refuses_what_never_fits);
    RUN_TEST(test_message_cut_to_caller_buffer);
    RUN_TEST(test_clear);
    RUN_TEST(test_random_against_model);
    return UNITY_END();
}
