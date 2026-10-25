/**
 * Native unit tests for SensorTypeIds::supportedType: the generation 1
 * types of 2.0.0 (CLIMA, CARBON, METEO); the removed VEML7700 (0x04),
 * SHT40 (0x05) and DIY_TEMP (0x51) and anything else are not supported, so
 * a sensors.json entry of such a type is dropped at load
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Data/SensorTypeIds.h"

void setUp(void) {}
void tearDown(void) {}

static void test_supported_types(void)
{
    TEST_ASSERT_TRUE(SensorTypeIds::supportedType(0x01));
    TEST_ASSERT_TRUE(SensorTypeIds::supportedType(0x02));
    TEST_ASSERT_TRUE(SensorTypeIds::supportedType(0x03));
}

static void test_removed_and_unknown_types(void)
{
    TEST_ASSERT_FALSE(SensorTypeIds::supportedType(0x00));
    TEST_ASSERT_FALSE(SensorTypeIds::supportedType(0x04)); // VEML7700
    TEST_ASSERT_FALSE(SensorTypeIds::supportedType(0x05)); // SHT40
    TEST_ASSERT_FALSE(SensorTypeIds::supportedType(0x51)); // DIY_TEMP
    int supported = 0;
    for (int t = 0; t < 256; t++)
    {
        supported += SensorTypeIds::supportedType((uint8_t)t);
    }
    TEST_ASSERT_EQUAL_INT(3, supported);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_supported_types);
    RUN_TEST(test_removed_and_unknown_types);
    return UNITY_END();
}
