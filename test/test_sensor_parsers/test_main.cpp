/**
 * Native unit tests for SensorParsers.
 *
 * Each fixture is a hand-computed wire packet (decrypted, sans checksum).
 * Byte indices: 0 flags, 1 type, 2..4 serial (24-bit BE), 5..6 battery mV
 * (BE), 7 numValues, 8+ payload. See SensorParsers.h for layout details.
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Protocol/SensorParsers.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// parseCommonHeader
// ---------------------------------------------------------------------------

static void test_common_header_too_short_returns_false(void)
{
    const uint8_t buf[6] = {0};
    SensorParsers::CommonHeader h{};
    TEST_ASSERT_FALSE(SensorParsers::parseCommonHeader(buf, sizeof(buf), h));
}

static void test_common_header_extracts_serial_and_voltage(void)
{
    // SN = 0x123456, battery raw = 3820 mV (0x0EEC) → 3.820 V
    const uint8_t buf[] = {0x00, 0x01, 0x12, 0x34, 0x56, 0x0E, 0xEC};
    SensorParsers::CommonHeader h{};
    TEST_ASSERT_TRUE(SensorParsers::parseCommonHeader(buf, sizeof(buf), h));
    TEST_ASSERT_EQUAL_UINT32(0x123456u, h.serialNumber);
    TEST_ASSERT_EQUAL_FLOAT(3.820f, h.batteryVoltage);
}

static void test_common_header_max_serial_24bit(void)
{
    // SN = 0xFFFFFF (max 24-bit value, byte 0 must stay zero)
    const uint8_t buf[] = {0xAA, 0xBB, 0xFF, 0xFF, 0xFF, 0x00, 0x00};
    SensorParsers::CommonHeader h{};
    TEST_ASSERT_TRUE(SensorParsers::parseCommonHeader(buf, sizeof(buf), h));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFu, h.serialNumber);
}

// ---------------------------------------------------------------------------
// parseBME280
// ---------------------------------------------------------------------------

static void test_bme280_too_short_returns_false(void)
{
    const uint8_t buf[13] = {0};
    SensorParsers::BME280Data d{};
    TEST_ASSERT_FALSE(SensorParsers::parseBME280(buf, sizeof(buf), d));
}

static void test_bme280_decodes_typical_values(void)
{
    // temp = 25.50 °C (2550 = 0x09F6)
    // press = 1013.2 hPa (10132 = 0x2794)
    // hum = 45.20 % (4520 = 0x11A8)
    const uint8_t buf[] = {
        0x00, 0x01, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x03,
        0x09, 0xF6, 0x27, 0x94, 0x11, 0xA8,
    };
    SensorParsers::BME280Data d{};
    TEST_ASSERT_TRUE(SensorParsers::parseBME280(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_FLOAT(25.50f, d.temperature);
    TEST_ASSERT_EQUAL_FLOAT(1013.2f, d.pressure);
    TEST_ASSERT_EQUAL_FLOAT(45.20f, d.humidity);
}

static void test_bme280_decodes_negative_temperature(void)
{
    // temp = -12.34 °C (-1234 = 0xFB2E as int16)
    const uint8_t buf[] = {
        0x00, 0x01, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x03,
        0xFB, 0x2E, 0x27, 0x94, 0x11, 0xA8,
    };
    SensorParsers::BME280Data d{};
    TEST_ASSERT_TRUE(SensorParsers::parseBME280(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_FLOAT(-12.34f, d.temperature);
}

static void test_bme280_decodes_extreme_negative_temperature(void)
{
    // temp = -327.68 °C (INT16_MIN = -32768 = 0x8000) — algorithmic edge
    const uint8_t buf[] = {
        0x00, 0x01, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x03,
        0x80, 0x00, 0x27, 0x94, 0x11, 0xA8,
    };
    SensorParsers::BME280Data d{};
    TEST_ASSERT_TRUE(SensorParsers::parseBME280(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_FLOAT(-327.68f, d.temperature);
}

// ---------------------------------------------------------------------------
// parseSCD40
// ---------------------------------------------------------------------------

static void test_scd40_too_short_returns_false(void)
{
    const uint8_t buf[13] = {0};
    SensorParsers::SCD40Data d{};
    TEST_ASSERT_FALSE(SensorParsers::parseSCD40(buf, sizeof(buf), d));
}

static void test_scd40_decodes_typical_values(void)
{
    // temp = 22.50 (2250 = 0x08CA)
    // ppm = 450 (0x01C2)  — note: SCD40 ppm has no scaling, raw uint16
    // hum = 55.00 (5500 = 0x157C)
    const uint8_t buf[] = {
        0x00, 0x02, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x03,
        0x08, 0xCA, 0x01, 0xC2, 0x15, 0x7C,
    };
    SensorParsers::SCD40Data d{};
    TEST_ASSERT_TRUE(SensorParsers::parseSCD40(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_FLOAT(22.50f, d.temperature);
    TEST_ASSERT_EQUAL_FLOAT(450.0f, d.ppm);
    TEST_ASSERT_EQUAL_FLOAT(55.00f, d.humidity);
}

static void test_scd40_max_ppm_uint16(void)
{
    // CO2 sensors can hit > 5000 ppm in indoor scenarios; uint16 caps at 65535.
    const uint8_t buf[] = {
        0x00, 0x02, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x03,
        0x08, 0xCA, 0xFF, 0xFF, 0x15, 0x7C,
    };
    SensorParsers::SCD40Data d{};
    TEST_ASSERT_TRUE(SensorParsers::parseSCD40(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_FLOAT(65535.0f, d.ppm);
}

// ---------------------------------------------------------------------------
// parseMETEO — basic (20 bytes) vs extended (22 bytes with rain rate)
// ---------------------------------------------------------------------------

static void test_meteo_too_short_returns_false(void)
{
    const uint8_t buf[19] = {0};
    SensorParsers::METEOData d{};
    TEST_ASSERT_FALSE(SensorParsers::parseMETEO(buf, sizeof(buf), d));
}

static void test_meteo_basic_packet_decodes_and_lacks_rain_rate(void)
{
    // temp = 18.75 (1875 = 0x0753)
    // press = 998.4 (9984 = 0x2700)
    // hum = 72.5 (7250 = 0x1C52)
    // windSpeedRaw = 45 (0x002D) → 45 * 0.111111 ≈ 4.999995
    // windDir = 180 (0x00B4)
    // rainAmountRaw = 62500 (0xF424) → 62500 / 5000 = 12.5 mm
    const uint8_t buf[] = {
        0x00, 0x03, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x06,
        0x07, 0x53,   // temp
        0x27, 0x00,   // press
        0x1C, 0x52,   // hum
        0x00, 0x2D,   // wind speed raw
        0x00, 0xB4,   // wind direction
        0xF4, 0x24,   // rain amount raw
    };
    SensorParsers::METEOData d{};
    TEST_ASSERT_TRUE(SensorParsers::parseMETEO(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_FLOAT(18.75f, d.temperature);
    TEST_ASSERT_EQUAL_FLOAT(998.4f, d.pressure);
    TEST_ASSERT_EQUAL_FLOAT(72.5f,  d.humidity);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 5.0f, d.windSpeed);
    TEST_ASSERT_EQUAL_UINT16(180, d.windDirection);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, d.rainAmount);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.rainRate);
    TEST_ASSERT_FALSE(d.hasRainRate);
}

static void test_meteo_extended_packet_includes_rain_rate(void)
{
    // Same as basic packet plus rainRateRaw = 1000 (0x03E8) → 1000 / 500 = 2.0 mm/h
    const uint8_t buf[] = {
        0x00, 0x03, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x07,
        0x07, 0x53, 0x27, 0x00, 0x1C, 0x52,
        0x00, 0x2D, 0x00, 0xB4, 0xF4, 0x24,
        0x03, 0xE8,   // rain rate raw
    };
    SensorParsers::METEOData d{};
    TEST_ASSERT_TRUE(SensorParsers::parseMETEO(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_FLOAT(2.0f, d.rainRate);
    TEST_ASSERT_TRUE(d.hasRainRate);
}

static void test_meteo_north_wind_direction(void)
{
    // Raw wind direction = 0 — this is the value the sensor emits for north.
    // The parser returns it as-is; the "0 → 360" remapping is calibration's
    // job, not the parser's.
    const uint8_t buf[] = {
        0x00, 0x03, 0x12, 0x34, 0x56, 0x0E, 0xEC, 0x06,
        0x07, 0x53, 0x27, 0x00, 0x1C, 0x52,
        0x00, 0x2D, 0x00, 0x00, 0xF4, 0x24,
    };
    SensorParsers::METEOData d{};
    TEST_ASSERT_TRUE(SensorParsers::parseMETEO(buf, sizeof(buf), d));
    TEST_ASSERT_EQUAL_UINT16(0, d.windDirection);
}

// ---------------------------------------------------------------------------

int main(int, char **)
{
    UNITY_BEGIN();

    RUN_TEST(test_common_header_too_short_returns_false);
    RUN_TEST(test_common_header_extracts_serial_and_voltage);
    RUN_TEST(test_common_header_max_serial_24bit);

    RUN_TEST(test_bme280_too_short_returns_false);
    RUN_TEST(test_bme280_decodes_typical_values);
    RUN_TEST(test_bme280_decodes_negative_temperature);
    RUN_TEST(test_bme280_decodes_extreme_negative_temperature);

    RUN_TEST(test_scd40_too_short_returns_false);
    RUN_TEST(test_scd40_decodes_typical_values);
    RUN_TEST(test_scd40_max_ppm_uint16);

    RUN_TEST(test_meteo_too_short_returns_false);
    RUN_TEST(test_meteo_basic_packet_decodes_and_lacks_rain_rate);
    RUN_TEST(test_meteo_extended_packet_includes_rain_rate);
    RUN_TEST(test_meteo_north_wind_direction);

    return UNITY_END();
}
