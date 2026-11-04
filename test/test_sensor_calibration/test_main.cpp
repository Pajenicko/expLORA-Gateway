/**
 * Native unit tests for SensorCalibration.
 *
 * Most interesting case: applyWindDirectionCorrection — the previous
 * inline implementation in SensorManager wrapped negative offsets to
 * ~65k° due to (uint16_t + int) % 360 promotion mismatch. These tests
 * pin the corrected behaviour.
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <cstring>
#include "../../src/Data/SensorCalibration.h"
#include "../../src/Data/CalibrationJson.h"
#include "../../src/Protocol/Gen2Values.h"
#include <string>

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// applyOffset — additive correction (temp / humidity / pressure / ppm / lux)
// ---------------------------------------------------------------------------

static void test_offset_zero_is_identity(void)
{
    TEST_ASSERT_EQUAL_FLOAT(21.5f, SensorCalibration::applyOffset(21.5f, 0.0f));
}

static void test_offset_positive(void)
{
    TEST_ASSERT_EQUAL_FLOAT(23.0f, SensorCalibration::applyOffset(21.5f, 1.5f));
}

static void test_offset_negative(void)
{
    TEST_ASSERT_EQUAL_FLOAT(20.0f, SensorCalibration::applyOffset(21.5f, -1.5f));
}

static void test_offset_preserves_zero_input(void)
{
    // 0 °C + offset behaves like any other reading; no special-casing.
    TEST_ASSERT_EQUAL_FLOAT(0.5f, SensorCalibration::applyOffset(0.0f, 0.5f));
}

// ---------------------------------------------------------------------------
// applyMultiplier — multiplicative correction (wind speed, rain)
// ---------------------------------------------------------------------------

static void test_multiplier_one_is_identity(void)
{
    TEST_ASSERT_EQUAL_FLOAT(4.2f, SensorCalibration::applyMultiplier(4.2f, 1.0f));
}

static void test_multiplier_scales(void)
{
    // 10 % uplift
    TEST_ASSERT_EQUAL_FLOAT(11.0f, SensorCalibration::applyMultiplier(10.0f, 1.10f));
}

static void test_multiplier_zero_zeros_value(void)
{
    // A multiplier of 0 silences the sensor — caller's responsibility.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, SensorCalibration::applyMultiplier(123.45f, 0.0f));
}

static void test_multiplier_negative_is_passed_through(void)
{
    // Wind/rain shouldn't be negative, but the helper is dumb arithmetic;
    // it doesn't clamp. Test documents that intentionally.
    TEST_ASSERT_EQUAL_FLOAT(-5.0f, SensorCalibration::applyMultiplier(10.0f, -0.5f));
}

// ---------------------------------------------------------------------------
// applyWindDirectionCorrection — modular wrap with proper signed handling
// ---------------------------------------------------------------------------

static void test_wind_zero_correction_is_identity_for_normal_values(void)
{
    TEST_ASSERT_EQUAL_UINT16(180, SensorCalibration::applyWindDirectionCorrection(180, 0));
    TEST_ASSERT_EQUAL_UINT16(45,  SensorCalibration::applyWindDirectionCorrection(45,  0));
    TEST_ASSERT_EQUAL_UINT16(359, SensorCalibration::applyWindDirectionCorrection(359, 0));
}

static void test_wind_north_convention_returns_360(void)
{
    // True north (0°) is reported as 360° to preserve historical UI behaviour.
    TEST_ASSERT_EQUAL_UINT16(360, SensorCalibration::applyWindDirectionCorrection(0, 0));
    // 360 raw also stays 360 (360 % 360 == 0 → mapped back to 360).
    TEST_ASSERT_EQUAL_UINT16(360, SensorCalibration::applyWindDirectionCorrection(360, 0));
}

static void test_wind_positive_correction_no_wrap(void)
{
    TEST_ASSERT_EQUAL_UINT16(190, SensorCalibration::applyWindDirectionCorrection(180, 10));
}

static void test_wind_positive_correction_wraps_through_north(void)
{
    // 350° + 20° = 370° → 10°
    TEST_ASSERT_EQUAL_UINT16(10, SensorCalibration::applyWindDirectionCorrection(350, 20));
}

static void test_wind_positive_correction_lands_on_north(void)
{
    // 350° + 10° = 360° → mapped to 360 by north convention.
    TEST_ASSERT_EQUAL_UINT16(360, SensorCalibration::applyWindDirectionCorrection(350, 10));
}

static void test_wind_negative_correction_wraps_through_north(void)
{
    // 10° - 20° = -10° → 350°. This is the bug-fix case: the old inline
    // implementation produced 65526 here due to signed→unsigned promotion.
    TEST_ASSERT_EQUAL_UINT16(350, SensorCalibration::applyWindDirectionCorrection(10, -20));
}

static void test_wind_negative_correction_no_wrap(void)
{
    TEST_ASSERT_EQUAL_UINT16(170, SensorCalibration::applyWindDirectionCorrection(180, -10));
}

static void test_wind_large_positive_correction(void)
{
    // 0° + 720° = two full rotations → 0° → 360° (north convention)
    TEST_ASSERT_EQUAL_UINT16(360, SensorCalibration::applyWindDirectionCorrection(0, 720));
    // 180° + 720° → 180°
    TEST_ASSERT_EQUAL_UINT16(180, SensorCalibration::applyWindDirectionCorrection(180, 720));
}

static void test_wind_large_negative_correction(void)
{
    // 180° - 720° = -540° → -540 % 360 = -180 → +360 → 180°
    TEST_ASSERT_EQUAL_UINT16(180, SensorCalibration::applyWindDirectionCorrection(180, -720));
    // 90° - 720° → 90°
    TEST_ASSERT_EQUAL_UINT16(90, SensorCalibration::applyWindDirectionCorrection(90, -720));
}

static void test_wind_out_of_range_raw_is_normalised(void)
{
    // Sensor delivering out-of-spec value (400°) gets brought back into range.
    TEST_ASSERT_EQUAL_UINT16(40, SensorCalibration::applyWindDirectionCorrection(400, 0));
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Set / apply / JSON - shared by generation 1 and 2
// ---------------------------------------------------------------------------

static void test_set_defaults_are_no_op(void)
{
    using namespace SensorCalibration;
    Set s;
    for (int f = 0; f < FIELDS; f++)
    {
        TEST_ASSERT_TRUE(s.isNone((Field)f));
    }
    TEST_ASSERT_EQUAL_FLOAT(21.37f, apply(s, Temperature, 21.37f));
    TEST_ASSERT_EQUAL_FLOAT(48.5f, apply(s, Humidity, 48.5f));
    TEST_ASSERT_EQUAL_FLOAT(1013.2f, apply(s, Pressure, 1013.2f));
    TEST_ASSERT_EQUAL_FLOAT(850.0f, apply(s, Co2, 850.0f));
    TEST_ASSERT_EQUAL_FLOAT(3.4f, apply(s, WindSpeed, 3.4f));
    TEST_ASSERT_EQUAL_FLOAT(225.0f, apply(s, WindDirection, 225.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.6f, apply(s, RainAmount, 0.6f));
    TEST_ASSERT_EQUAL_FLOAT(1.2f, apply(s, RainRate, 1.2f));
    TEST_ASSERT_EQUAL_FLOAT(1013.2f, pressureAtAltitude(1013.2f, (int)s[Altitude], 20.0f));
}

static void test_set_applies_each_field(void)
{
    using namespace SensorCalibration;
    Set s;
    s[Temperature] = -0.3f;
    s[Humidity] = 2.0f;
    s[Pressure] = 1.5f;
    s[Co2] = -40.0f;
    s[Lux] = 10.0f;
    s[WindSpeed] = 1.1f;
    s[WindDirection] = -20.0f;
    s[RainAmount] = 0.5f;
    s[RainRate] = 2.0f;
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 20.84f, apply(s, Temperature, 21.14f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -10.3f, apply(s, Temperature, -10.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 51.6f, apply(s, Humidity, 49.6f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1014.7f, apply(s, Pressure, 1013.2f));
    TEST_ASSERT_EQUAL_FLOAT(810.0f, apply(s, Co2, 850.0f));
    TEST_ASSERT_EQUAL_FLOAT(110.0f, apply(s, Lux, 100.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.2f, apply(s, WindSpeed, 2.0f));
    TEST_ASSERT_EQUAL_FLOAT(350.0f, apply(s, WindDirection, 10.0f)); // wraps through north
    TEST_ASSERT_EQUAL_FLOAT(0.3f, apply(s, RainAmount, 0.6f));
    TEST_ASSERT_EQUAL_FLOAT(2.4f, apply(s, RainRate, 1.2f));
}

static void test_pressure_at_altitude(void)
{
    // 350 m, 15 degC: about +41 hPa (barometric formula, as before)
    const float p = SensorCalibration::pressureAtAltitude(972.0f, 350, 15.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 1013.0f, p);
    TEST_ASSERT_TRUE(p > 972.0f);
}

// Generation 1: every correction written, in the order and with the types
// of the earlier firmware (sensors.json unchanged)
static void test_json_gen1_writes_every_key_as_before(void)
{
    using namespace SensorCalibration;
    Set s;
    s[Temperature] = -0.5f;
    s[WindDirection] = 15.0f;
    s[Altitude] = 350.0f;
    DynamicJsonDocument a(1024), b(1024);
    JsonObject o = a.to<JsonObject>();
    o["altitude"] = (int)s[Altitude];
    writeJson(o, s, false, Temperature, Altitude);
    JsonObject old = b.to<JsonObject>();
    old["altitude"] = 350;
    old["temperatureCorrection"] = -0.5f;
    old["humidityCorrection"] = 0.0f;
    old["pressureCorrection"] = 0.0f;
    old["ppmCorrection"] = 0.0f;
    old["luxCorrection"] = 0.0f;
    old["windSpeedCorrection"] = 1.0f;
    old["windDirectionCorrection"] = 15;
    old["rainAmountCorrection"] = 1.0f;
    old["rainRateCorrection"] = 1.0f;
    std::string x, y;
    serializeJson(a, x);
    serializeJson(b, y);
    TEST_ASSERT_EQUAL_STRING(y.c_str(), x.c_str());
}

// Generation 2: only what corrects something; read back the same
static void test_json_gen2_only_corrections_round_trip(void)
{
    using namespace SensorCalibration;
    Set s;
    s[Temperature] = -0.3f;
    s[Humidity] = 2.0f;
    s[Altitude] = 350.0f;
    DynamicJsonDocument d(512);
    JsonObject o = d.to<JsonObject>();
    writeJson(o, s, true);
    std::string out;
    serializeJson(d, out);
    TEST_ASSERT_EQUAL_STRING("{\"temperatureCorrection\":-0.3,\"humidityCorrection\":2,\"altitude\":350}", out.c_str());
    DynamicJsonDocument r(512);
    TEST_ASSERT_FALSE(deserializeJson(r, out));
    Set back;
    readJson(r.as<JsonObjectConst>(), back);
    TEST_ASSERT_EQUAL_MEMORY(s.v, back.v, sizeof(s.v));
    // An entry without corrections reads as none
    Set none;
    DynamicJsonDocument e(64);
    deserializeJson(e, "{}");
    readJson(e.as<JsonObjectConst>(), none);
    TEST_ASSERT_TRUE(none.isNone(Temperature) && none.isNone(WindSpeed) && none.isNone(Altitude));
}

// Whole fields: a huge number is out of range (no cast of a value beyond long)
static void test_form_parse_huge_whole(void)
{
    using namespace SensorCalibration;
    float out = 0;
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("1e30", Altitude, 0, out));
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("-1e30", WindDirection, 0, out));
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("3000000000", Altitude, 0, out));
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("1e9", Altitude, 0, out));
    TEST_ASSERT_TRUE(Parsed::Ok == parse("-12.9", WindDirection, 0, out));
    TEST_ASSERT_EQUAL_FLOAT(-12.0f, out);
    TEST_ASSERT_TRUE(Parsed::NotNumber == parse("1e400", Altitude, 0, out)); // not finite
}

// Gen 2: an entered correction is rounded to 0.001 as sensors.json stores
// it, so RAM and the file agree after a restart
static void test_gen2_milli_matches_stored(void)
{
    using namespace SensorCalibration;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, milli(0.0004f));
    TEST_ASSERT_EQUAL_FLOAT(0.013f, milli(0.0125f));
    TEST_ASSERT_EQUAL_FLOAT(-0.3f, milli(-0.3f));
    // -0.0 becomes +0.0 (bitwise: a stored 0 compares equal, no "-0" shown)
    const float zero = 0.0f;
    for (float x : {-0.0004f, -0.0f, -0.0005f + 0.0002f})
    {
        const float m = milli(x);
        TEST_ASSERT_EQUAL_MEMORY(&zero, &m, sizeof(float));
    }
    const float xs[] = {0.0004f, 0.0125f, -0.0005f, 1.23456f, -49.9994f, 0.125f, 3.14159f, -0.001f, 99.9995f};
    for (float x : xs)
    {
        Set s;
        s[Temperature] = milli(x);
        s[WindSpeed] = milli(x + 2.0f); // a multiplier field (none = 1)
        DynamicJsonDocument d(512);
        writeJson(d.to<JsonObject>(), s, true);
        std::string out;
        serializeJson(d, out);
        DynamicJsonDocument r(512);
        TEST_ASSERT_FALSE(deserializeJson(r, out));
        Set back;
        readJson(r.as<JsonObjectConst>(), back);
        TEST_ASSERT_EQUAL_MEMORY(s.v, back.v, sizeof(s.v));
    }
}

// Gen 2 uplink values: decoded, invalid sentinels skipped, corrected
static void test_gen2_values_decode_and_correct(void)
{
    using namespace SensorCalibration;
    uint8_t t[2] = {0x08, 0x42}; // 2114 = 21.14 degC
    ep::Tlv tlv{ep::up::kTemperature, 0, 2, t};
    Qty::Id q;
    float v;
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(tlv, q, v));
    TEST_ASSERT_EQUAL(Qty::Temperature, q);
    Set s;
    s[Temperature] = -0.3f;
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 20.84f, Gen2Values::corrected(s, q, v));
    uint8_t neg[2] = {0xFC, 0x18}; // -1000 = -10.00 degC
    tlv.value = neg;
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(tlv, q, v));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -10.3f, Gen2Values::corrected(s, q, v));
    uint8_t invalid[2] = {0x80, 0x00};
    tlv.value = invalid;
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Invalid == Gen2Values::decode(tlv, q, v));
    TEST_ASSERT_EQUAL(Qty::Temperature, q);
    uint8_t mv[2] = {0x0B, 0x72}; // 2930 mV
    ep::Tlv bat{ep::up::kBattery, 0, 2, mv};
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(bat, q, v));
    TEST_ASSERT_EQUAL(Qty::Battery, q);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.93f, Gen2Values::corrected(s, q, v)); // no correction field
    uint8_t ff[2] = {0xFF, 0xFF};
    ep::Tlv hum{ep::up::kHumidity, 0, 2, ff};
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Invalid == Gen2Values::decode(hum, q, v));
    TEST_ASSERT_EQUAL(Qty::Humidity, q);
    uint8_t p[2] = {0x27, 0x94}; // 10132 = 1013.2 hPa
    ep::Tlv pr{ep::up::kPressure, 0, 2, p};
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(pr, q, v));
    TEST_ASSERT_EQUAL(Qty::Pressure, q);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1013.2f, v);
    uint8_t c[2] = {0x04, 0x7E}; // 1150 ppm
    ep::Tlv co2{ep::up::kCo2, 0, 2, c};
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(co2, q, v));
    TEST_ASSERT_EQUAL(Qty::Co2, q);
    TEST_ASSERT_EQUAL_FLOAT(1150.0f, v);
    uint8_t l[2] = {0x35, 0x44}; // 13636 = round(2048 * log2(100 + 1))
    ep::Tlv lux{ep::up::kIlluminance, 0, 2, l};
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(lux, q, v));
    TEST_ASSERT_EQUAL(Qty::Lux, q);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 100.0f, v);
    uint8_t w[2] = {0x00, 0x10};
    ep::Tlv wind{ep::up::kWindAvg, 0, 2, w}; // mapped with a gen 2 METEO later
    TEST_ASSERT_TRUE(Gen2Values::Decoded::None == Gen2Values::decode(wind, q, v));
}

// Readings of a generation 2 sensor (the Gen2Sensor fields Gen2Values uses)
struct FakeGen2
{
    float value[Qty::COUNT] = {};
    uint16_t qNow = 0, qMask = 0, qBad = 0;
    SensorCalibration::Set cal;
};

static ep::Tlv tlv16(uint8_t type, uint8_t *buf, uint16_t raw)
{
    buf[0] = (uint8_t)(raw >> 8);
    buf[1] = (uint8_t)raw;
    return ep::Tlv{type, 0, 2, buf};
}

// Every sentinel of a mapped quantity: Invalid (i16 0x8000 for the
// temperature, u16 0xFFFF for the rest); 0xFFFF is a valid -0.01 degC
static void test_gen2_every_sentinel(void)
{
    const uint8_t u16types[] = {ep::up::kHumidity, ep::up::kPressure, ep::up::kCo2, ep::up::kIlluminance,
                                ep::up::kBattery};
    const Qty::Id u16q[] = {Qty::Humidity, Qty::Pressure, Qty::Co2, Qty::Lux, Qty::Battery};
    uint8_t b[2];
    Qty::Id q;
    float v;
    for (size_t i = 0; i < sizeof(u16types); i++)
    {
        ep::Tlv t = tlv16(u16types[i], b, 0xFFFF);
        TEST_ASSERT_TRUE(Gen2Values::Decoded::Invalid == Gen2Values::decode(t, q, v));
        TEST_ASSERT_EQUAL(u16q[i], q);
        t = tlv16(u16types[i], b, 0xFFFE);
        TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(t, q, v));
    }
    ep::Tlv t = tlv16(ep::up::kTemperature, b, 0x8000);
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Invalid == Gen2Values::decode(t, q, v));
    t = tlv16(ep::up::kTemperature, b, 0xFFFF);
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::decode(t, q, v));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -0.01f, v);
}

// An invalid value clears the quantity's current bit (keeps it reported),
// marks it bad; the next value makes it current again
static void test_gen2_invalid_value_is_missing_not_stale(void)
{
    FakeGen2 g;
    uint8_t b[2];
    ep::Tlv t = tlv16(ep::up::kTemperature, b, 2114);
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::apply(g, t));
    const uint16_t mt = Qty::mask(Qty::Temperature);
    TEST_ASSERT_EQUAL_HEX16(mt, g.qNow);
    TEST_ASSERT_EQUAL_HEX16(mt, g.qMask);
    TEST_ASSERT_EQUAL_HEX16(0, g.qBad);
    t = tlv16(ep::up::kTemperature, b, 0x8000);
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Invalid == Gen2Values::apply(g, t));
    TEST_ASSERT_EQUAL_HEX16(0, g.qNow);
    TEST_ASSERT_EQUAL_HEX16(mt, g.qMask);
    TEST_ASSERT_EQUAL_HEX16(mt, g.qBad);
    // A quantity first reported as invalid is known (Home Assistant gets
    // the entity) but has no value
    t = tlv16(ep::up::kCo2, b, 0xFFFF);
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Invalid == Gen2Values::apply(g, t));
    TEST_ASSERT_EQUAL_HEX16(mt | Qty::mask(Qty::Co2), g.qMask);
    TEST_ASSERT_EQUAL_HEX16(0, g.qNow);
    t = tlv16(ep::up::kTemperature, b, 1999);
    TEST_ASSERT_TRUE(Gen2Values::Decoded::Value == Gen2Values::apply(g, t));
    TEST_ASSERT_EQUAL_HEX16(mt, g.qNow);
    TEST_ASSERT_EQUAL_HEX16(Qty::mask(Qty::Co2), g.qBad);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 19.99f, g.value[Qty::Temperature]);
    // Not a quantity: nothing changes
    t = tlv16(ep::up::kWindAvg, b, 0xFFFF);
    TEST_ASSERT_TRUE(Gen2Values::Decoded::None == Gen2Values::apply(g, t));
    TEST_ASSERT_EQUAL_HEX16(mt | Qty::mask(Qty::Co2), g.qMask);
}

// The altitude conversion uses the temperature only while it is current:
// after an invalid temperature it falls back to 15 degC as without one
static void test_gen2_altitude_without_current_temperature(void)
{
    using namespace SensorCalibration;
    FakeGen2 g, ref;
    g.cal[Altitude] = 500;
    ref.cal[Altitude] = 500;
    uint8_t b[2];
    ep::Tlv t = tlv16(ep::up::kTemperature, b, 3000); // 30 degC
    Gen2Values::apply(g, t);
    t = tlv16(ep::up::kTemperature, b, 0x8000);
    Gen2Values::apply(g, t);
    t = tlv16(ep::up::kPressure, b, 9550);
    Gen2Values::apply(g, t);
    Gen2Values::apply(ref, t);
    Gen2Values::pressureToAltitude(g);
    Gen2Values::pressureToAltitude(ref); // never had a temperature
    TEST_ASSERT_EQUAL_FLOAT(pressureAtAltitude(955.0f, 500, 15.0f), g.value[Qty::Pressure]);
    TEST_ASSERT_EQUAL_FLOAT(ref.value[Qty::Pressure], g.value[Qty::Pressure]);
    // With a current temperature it is used
    FakeGen2 w;
    w.cal[Altitude] = 500;
    t = tlv16(ep::up::kTemperature, b, 3000);
    Gen2Values::apply(w, t);
    t = tlv16(ep::up::kPressure, b, 9550);
    Gen2Values::apply(w, t);
    Gen2Values::pressureToAltitude(w);
    TEST_ASSERT_EQUAL_FLOAT(pressureAtAltitude(955.0f, 500, 30.0f), w.value[Qty::Pressure]);
}

// The edit form prints a stored correction so that posting it back gives
// the same float (1.x values like 0.125 or 1.005 included)
static void test_form_text_round_trip(void)
{
    using namespace SensorCalibration;
    const float values[] = {0.0f, 0.125f, 1.005f, -0.3f, 0.1f, 2.5f, -49.99f, 1234.567f, 100000.0f, 1e-7f, 3.14159265f};
    char t[24];
    for (float v : values)
    {
        formText(t, sizeof(t), Temperature, v);
        float back = -999;
        TEST_ASSERT_TRUE(Parsed::Ok == parse(t, Temperature, v, back));
        TEST_ASSERT_EQUAL_MESSAGE(0, memcmp(&v, &back, sizeof(v)), t);
    }
    formText(t, sizeof(t), Temperature, 0.125f);
    TEST_ASSERT_EQUAL_STRING("0.125", t);
    formText(t, sizeof(t), RainRate, 1.0f);
    TEST_ASSERT_EQUAL_STRING("1", t);
    formText(t, sizeof(t), Altitude, 350.0f);
    TEST_ASSERT_EQUAL_STRING("350", t);
}

// Posted values: empty = none, a number within the limits, a stored
// out-of-range value posted back unchanged is kept (it must not block the
// other fields), a new out-of-range value or text that is not a number is
// refused
static void test_form_parse(void)
{
    using namespace SensorCalibration;
    float out = 7;
    TEST_ASSERT_TRUE(Parsed::Ok == parse("", WindSpeed, 1.2f, out));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out);
    TEST_ASSERT_TRUE(Parsed::Ok == parse("-1.5", Temperature, 0, out));
    TEST_ASSERT_EQUAL_FLOAT(-1.5f, out);
    TEST_ASSERT_TRUE(Parsed::Ok == parse("6000", Co2, 6000.0f, out)); // legacy, unchanged
    TEST_ASSERT_EQUAL_FLOAT(6000.0f, out);
    out = 7;
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("6001", Co2, 6000.0f, out));
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("5001", Co2, 0, out));
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("-0.1", WindSpeed, 1, out));
    TEST_ASSERT_TRUE(Parsed::NotNumber == parse("abc", Temperature, 0, out));
    TEST_ASSERT_TRUE(Parsed::NotNumber == parse("1.5x", Temperature, 0, out));
    TEST_ASSERT_TRUE(Parsed::NotNumber == parse("nan", Temperature, 0, out));
    TEST_ASSERT_TRUE(Parsed::NotNumber == parse("inf", Temperature, 0, out));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, out); // untouched on a refusal
    TEST_ASSERT_TRUE(Parsed::Ok == parse("12.7", WindDirection, 0, out)); // whole field
    TEST_ASSERT_EQUAL_FLOAT(12.0f, out);
    TEST_ASSERT_TRUE(Parsed::Ok == parse("400", WindDirection, 400.0f, out)); // legacy, unchanged
    TEST_ASSERT_TRUE(Parsed::OutOfRange == parse("9001", Altitude, 0, out));
}

int main(int, char **)
{
    UNITY_BEGIN();

    RUN_TEST(test_offset_zero_is_identity);
    RUN_TEST(test_offset_positive);
    RUN_TEST(test_offset_negative);
    RUN_TEST(test_offset_preserves_zero_input);

    RUN_TEST(test_multiplier_one_is_identity);
    RUN_TEST(test_multiplier_scales);
    RUN_TEST(test_multiplier_zero_zeros_value);
    RUN_TEST(test_multiplier_negative_is_passed_through);

    RUN_TEST(test_wind_zero_correction_is_identity_for_normal_values);
    RUN_TEST(test_wind_north_convention_returns_360);
    RUN_TEST(test_wind_positive_correction_no_wrap);
    RUN_TEST(test_wind_positive_correction_wraps_through_north);
    RUN_TEST(test_wind_positive_correction_lands_on_north);
    RUN_TEST(test_wind_negative_correction_wraps_through_north);
    RUN_TEST(test_wind_negative_correction_no_wrap);
    RUN_TEST(test_wind_large_positive_correction);
    RUN_TEST(test_wind_large_negative_correction);
    RUN_TEST(test_wind_out_of_range_raw_is_normalised);

    RUN_TEST(test_set_defaults_are_no_op);
    RUN_TEST(test_set_applies_each_field);
    RUN_TEST(test_pressure_at_altitude);
    RUN_TEST(test_json_gen1_writes_every_key_as_before);
    RUN_TEST(test_json_gen2_only_corrections_round_trip);
    RUN_TEST(test_gen2_values_decode_and_correct);
    RUN_TEST(test_form_text_round_trip);
    RUN_TEST(test_form_parse);
    RUN_TEST(test_form_parse_huge_whole);
    RUN_TEST(test_gen2_milli_matches_stored);
    RUN_TEST(test_gen2_every_sentinel);
    RUN_TEST(test_gen2_invalid_value_is_missing_not_stale);
    RUN_TEST(test_gen2_altitude_without_current_temperature);

    return UNITY_END();
}
