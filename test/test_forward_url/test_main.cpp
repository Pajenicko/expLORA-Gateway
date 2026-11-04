/**
 * Native unit tests for ForwardUrl: the "Forward to URL" placeholders of
 * both sensor generations - values with the generation 1 decimals, a
 * quantity without a current value leaves its placeholder (never a stale
 * number), *SN* / *TYPE*, unknown or broken placeholders copied, every
 * occurrence replaced, the output bound, the field validation
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <string>
#include <utility>
#include <initializer_list>
#include "../../src/Data/ForwardUrl.h"
#include "../../src/Protocol/Gen2Values.h"

void setUp(void) {}
void tearDown(void) {}

static std::string run(const char *tmpl, const ForwardUrl::Values &v, size_t cap = 512)
{
    char out[1024];
    const size_t n = ForwardUrl::expand(tmpl, v, out, cap);
    return n ? std::string(out, n) : std::string("<none>");
}

static ForwardUrl::Values gen1Clima()
{
    ForwardUrl::Values v;
    v.valid = Qty::mask(Qty::Temperature) | Qty::mask(Qty::Humidity) | Qty::mask(Qty::Pressure) |
              Qty::mask(Qty::Battery) | Qty::mask(Qty::Rssi);
    v.v[Qty::Temperature] = 21.456f;
    v.v[Qty::Humidity] = 48.2f;
    v.v[Qty::Pressure] = 1013.25f;
    v.v[Qty::Battery] = 3.012f;
    v.v[Qty::Rssi] = -87;
    v.sn = "12AB34";
    v.type = 1;
    return v;
}

void test_generation1_values_and_decimals()
{
    const ForwardUrl::Values v = gen1Clima();
    TEST_ASSERT_EQUAL_STRING("http://tmep.cz/?t=21.46&h=48.20&p=1013.25&b=3.01&r=-87&sn=12AB34&ty=1",
                             run("http://tmep.cz/?t=*TEMP*&h=*HUM*&p=*PRESS*&b=*BAT*&r=*RSSI*&sn=*SN*&ty=*TYPE*", v)
                                 .c_str());
    // Not provided by the type: left as it is (generation 1 behaviour)
    TEST_ASSERT_EQUAL_STRING("http://x/?c=*PPM*&w=*WIND_SPEED*", run("http://x/?c=*PPM*&w=*WIND_SPEED*", v).c_str());
    // Every occurrence
    TEST_ASSERT_EQUAL_STRING("http://x/21.46/21.46", run("http://x/*TEMP*/*TEMP*", v).c_str());
}

void test_meteo_and_co2()
{
    ForwardUrl::Values v;
    v.valid = Qty::mask(Qty::WindSpeed) | Qty::mask(Qty::WindDir) | Qty::mask(Qty::Rain) |
              Qty::mask(Qty::DailyRain) | Qty::mask(Qty::RainRate) | Qty::mask(Qty::Co2) | Qty::mask(Qty::Lux);
    v.v[Qty::WindSpeed] = 3.25f;
    v.v[Qty::WindDir] = 270;
    v.v[Qty::Rain] = 0.3f;
    v.v[Qty::DailyRain] = 12.75f;
    v.v[Qty::RainRate] = 1.04f;
    v.v[Qty::Co2] = 612.6f;
    v.v[Qty::Lux] = 120.06f;
    TEST_ASSERT_EQUAL_STRING("https://h/?w=3.2&d=270&r=0.3&dr=12.8&rr=1.0&c=613&l=120.1",
                             run("https://h/?w=*WIND_SPEED*&d=*WIND_DIR*&r=*RAIN*&dr=*DAILY_RAIN*&rr=*RAIN_RATE*&c=*PPM*"
                                 "&l=*LUX*",
                                 v)
                                 .c_str());
}

// Generation 2: only the quantities with a current value (qNow); a failed
// or never reported one keeps its placeholder; *SN* is the DevID, *TYPE* 0
void test_generation2_missing_values()
{
    ForwardUrl::Values v;
    v.valid = Qty::mask(Qty::Temperature) | Qty::mask(Qty::Rssi); // humidity failed, battery never reported
    v.v[Qty::Temperature] = -4.5f;
    v.v[Qty::Humidity] = 55.0f; // stale: must not be sent
    v.v[Qty::Battery] = 2.9f;   // stale
    v.v[Qty::Rssi] = -101;
    v.sn = "4776B882";
    v.type = 0;
    TEST_ASSERT_EQUAL_STRING("http://s/?id=4776B882&t=-4.50&h=*HUM*&b=*BAT*&r=-101&ty=0",
                             run("http://s/?id=*SN*&t=*TEMP*&h=*HUM*&b=*BAT*&r=*RSSI*&ty=*TYPE*", v).c_str());
}

// A generation 2 sensor's readings (the Gen2Sensor fields Gen2Values uses)
struct FakeGen2
{
    float value[Qty::COUNT] = {};
    uint16_t qNow = 0, qMask = 0, qBad = 0;
    SensorCalibration::Set cal;
};

// One verified UP: its channel-0 TLVs applied as Gen2Response::decodeUp
// does; returns the quantities it carried with a valid value
static uint16_t up(FakeGen2 &g, std::initializer_list<std::pair<uint8_t, uint16_t>> tlvs, int rssi = -90)
{
    uint16_t fresh = 0;
    g.value[Qty::Rssi] = (float)rssi;
    g.qNow |= Qty::mask(Qty::Rssi);
    for (const auto &p : tlvs)
    {
        uint8_t b[2] = {(uint8_t)(p.second >> 8), (uint8_t)p.second};
        ep::Tlv t{p.first, 0, 2, b};
        Gen2Values::apply(g, t, &fresh);
    }
    return fresh;
}

// The URL forwardGen2 would send for this UP, "<no send>" when not due
static std::string forwardOf(const FakeGen2 &g, uint16_t fresh)
{
    if (!ForwardUrl::gen2Due(fresh))
    {
        return "<no send>";
    }
    ForwardUrl::Values v;
    v.valid = ForwardUrl::gen2Valid(fresh, g.qNow);
    for (int q = 0; q < Qty::COUNT; q++)
    {
        v.v[q] = g.value[q];
    }
    v.sn = "4776B882";
    return run("http://s/?t=*TEMP*&h=*HUM*&b=*BAT*&r=*RSSI*", v);
}

// The measuring chip fails after a good report: the sensor leaves the
// temperature / humidity out and sends the battery only - no forward, and
// the older values are never sent as current ones
void test_generation2_chip_failure_no_stale_values()
{
    using namespace ep::up;
    FakeGen2 g;
    uint16_t f = up(g, {{kTemperature, 2114}, {kHumidity, 4820}, {kBattery, 2950}});
    TEST_ASSERT_EQUAL_STRING("http://s/?t=21.14&h=48.20&b=2.95&r=-90", forwardOf(g, f).c_str());
    f = up(g, {{kBattery, 2940}}, -95);
    TEST_ASSERT_EQUAL_STRING("<no send>", forwardOf(g, f).c_str());
    // The older readings are still current for the pages (qNow) ...
    TEST_ASSERT_TRUE(g.qNow & Qty::mask(Qty::Temperature));
    // ... but an UP with the temperature only does not send the humidity
    f = up(g, {{kTemperature, 2001}, {kBattery, 2940}}, -91);
    TEST_ASSERT_EQUAL_STRING("http://s/?t=20.01&h=*HUM*&b=2.94&r=-91", forwardOf(g, f).c_str());
    // Without the battery in the UP: *BAT* stays as well
    f = up(g, {{kHumidity, 5000}}, -92);
    TEST_ASSERT_EQUAL_STRING("http://s/?t=*TEMP*&h=50.00&b=*BAT*&r=-92", forwardOf(g, f).c_str());
}

// An invalid-value sentinel: that quantity stays a placeholder
void test_generation2_invalid_sentinel()
{
    using namespace ep::up;
    FakeGen2 g;
    up(g, {{kTemperature, 2114}, {kHumidity, 4820}, {kBattery, 2950}});
    const uint16_t f = up(g, {{kTemperature, 2200}, {kHumidity, 0xFFFF}, {kBattery, 2950}});
    TEST_ASSERT_EQUAL_STRING("http://s/?t=22.00&h=*HUM*&b=2.95&r=-90", forwardOf(g, f).c_str());
}

// Only invalid readings (and a valid battery), or nothing at all: no send
void test_generation2_all_invalid_no_send()
{
    using namespace ep::up;
    FakeGen2 g;
    up(g, {{kTemperature, 2114}, {kHumidity, 4820}, {kBattery, 2950}});
    uint16_t f = up(g, {{kTemperature, 0x8000}, {kHumidity, 0xFFFF}, {kBattery, 2950}});
    TEST_ASSERT_EQUAL_STRING("<no send>", forwardOf(g, f).c_str());
    f = up(g, {{kTemperature, 0x8000}, {kBattery, 0xFFFF}});
    TEST_ASSERT_EQUAL_STRING("<no send>", forwardOf(g, f).c_str());
    f = up(g, {});
    TEST_ASSERT_EQUAL_STRING("<no send>", forwardOf(g, f).c_str());
    // Any other reading counts (CO2 / pressure / light)
    f = up(g, {{kCo2, 800}});
    TEST_ASSERT_TRUE(ForwardUrl::gen2Due(f));
    TEST_ASSERT_EQUAL_STRING("http://s/?t=*TEMP*&h=*HUM*&b=*BAT*&r=-90", forwardOf(g, f).c_str());
}

// A sensor without an answer is skipped for 5 minutes; an answer clears
// it; other sensors are not affected; millis() wrap; full table
void test_backoff()
{
    ForwardUrl::Backoff<3> b;
    const uint32_t T = ForwardUrl::BACKOFF_MS;
    TEST_ASSERT_FALSE(b.skip(0x4776B882, 1000));
    b.result(0x4776B882, false, 1000);
    TEST_ASSERT_TRUE(b.skip(0x4776B882, 1000));
    TEST_ASSERT_TRUE(b.skip(0x4776B882, 1000 + T - 1));
    TEST_ASSERT_FALSE(b.skip(0x4776B882, 1000 + T));
    TEST_ASSERT_FALSE(b.skip(0x12AB34, 1000));
    // Fails again after the pause, then answers
    b.result(0x4776B882, false, 1000 + T);
    TEST_ASSERT_TRUE(b.skip(0x4776B882, 1000 + T + 5));
    b.result(0x4776B882, true, 1000 + 2 * T);
    TEST_ASSERT_FALSE(b.skip(0x4776B882, 1000 + 2 * T));
    // An answer of a sensor without an entry changes nothing
    b.result(0x1, true, 5);
    TEST_ASSERT_FALSE(b.skip(0x1, 5));
    // Across the millis() wrap
    b.result(0x2, false, 0xFFFFFF00u);
    TEST_ASSERT_TRUE(b.skip(0x2, 0x100));
    TEST_ASSERT_FALSE(b.skip(0x2, 0xFFFFFF00u + T));
    // Table full: the oldest goes, the others stay
    ForwardUrl::Backoff<2> f;
    f.result(0xA, false, 100);
    f.result(0xB, false, 200);
    f.result(0xC, false, 300);
    TEST_ASSERT_FALSE(f.skip(0xA, 300));
    TEST_ASSERT_TRUE(f.skip(0xB, 300));
    TEST_ASSERT_TRUE(f.skip(0xC, 300));
    // An expired entry is reused before the oldest live one
    f.result(0xD, false, 200 + T);
    TEST_ASSERT_TRUE(f.skip(0xC, 200 + T));
    TEST_ASSERT_TRUE(f.skip(0xD, 200 + T));
}

void test_unknown_and_broken_placeholders()
{
    const ForwardUrl::Values v = gen1Clima();
    TEST_ASSERT_EQUAL_STRING("http://x/*FOO*21.46", run("http://x/*FOO**TEMP*", v).c_str());
    TEST_ASSERT_EQUAL_STRING("http://x/*TEMP", run("http://x/*TEMP", v).c_str());
    TEST_ASSERT_EQUAL_STRING("http://x/**21.46*", run("http://x/***TEMP**", v).c_str());
    TEST_ASSERT_EQUAL_STRING("http://x/*temp*", run("http://x/*temp*", v).c_str()); // case matters
    TEST_ASSERT_EQUAL_STRING("http://x/?a=*&b=21.46", run("http://x/?a=*&b=*TEMP*", v).c_str());
    TEST_ASSERT_EQUAL_STRING("http://x/48.20RAIN*", run("http://x/*HUM*RAIN*", v).c_str());
}

void test_bounds()
{
    const ForwardUrl::Values v = gen1Clima();
    // Fits exactly (cap includes the terminator), one byte less does not
    const char *t = "http://x/*TEMP*"; // "http://x/21.46" = 14
    TEST_ASSERT_EQUAL_STRING("http://x/21.46", run(t, v, 15).c_str());
    TEST_ASSERT_EQUAL_STRING("<none>", run(t, v, 14).c_str());
    // A huge (corrected) value cannot overflow
    ForwardUrl::Values big = v;
    big.v[Qty::Temperature] = 3.0e38f;
    std::string many = "http://x/";
    for (int i = 0; i < 40; i++)
    {
        many += "*TEMP*";
    }
    TEST_ASSERT_EQUAL_STRING("<none>", run(many.c_str(), big, 512).c_str());
    char out[64];
    TEST_ASSERT_EQUAL(0, ForwardUrl::expand("", v, out, sizeof(out)));
}

void test_field_validation()
{
    TEST_ASSERT_TRUE(ForwardUrl::valid("", 0));
    TEST_ASSERT_TRUE(ForwardUrl::valid("http://a", 8));
    TEST_ASSERT_TRUE(ForwardUrl::valid("https://a", 9));
    TEST_ASSERT_FALSE(ForwardUrl::valid("ftp://a", 7));
    TEST_ASSERT_FALSE(ForwardUrl::valid("HTTP://a", 8));
    std::string u = "https://";
    u.append(ForwardUrl::MAX_LEN - u.size(), 'a');
    TEST_ASSERT_TRUE(ForwardUrl::valid(u.c_str(), u.size()));
    u += 'a';
    TEST_ASSERT_FALSE(ForwardUrl::valid(u.c_str(), u.size()));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_generation1_values_and_decimals);
    RUN_TEST(test_meteo_and_co2);
    RUN_TEST(test_generation2_missing_values);
    RUN_TEST(test_generation2_chip_failure_no_stale_values);
    RUN_TEST(test_generation2_invalid_sentinel);
    RUN_TEST(test_generation2_all_invalid_no_send);
    RUN_TEST(test_backoff);
    RUN_TEST(test_unknown_and_broken_placeholders);
    RUN_TEST(test_bounds);
    RUN_TEST(test_field_validation);
    return UNITY_END();
}
