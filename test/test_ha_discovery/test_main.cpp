/**
 * Native unit tests for HaDiscovery: topics, unique_id and the discovery
 * payload of a generation 1 sensor exactly as 1.x built them (Home
 * Assistant keeps the entities), the generation 2 device block (model
 * "Gen 2 HW<n>", sw_version), entity names, state values, payload size
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <string>
#include "../../src/Protocol/HaDiscovery.h"

void setUp(void) {}
void tearDown(void) {}

static HaDiscovery::Device v1()
{
    HaDiscovery::Device d;
    d.prefix = "explora";
    d.haPrefix = "homeassistant";
    d.id = "00ABCD";
    d.name = "Living room";
    d.model = "CLIMA";
    return d;
}

static std::string payload(const HaDiscovery::Device &d, Qty::Id q)
{
    char buf[900];
    TEST_ASSERT_TRUE(HaDiscovery::payload(buf, sizeof(buf), d, q) > 0);
    return buf;
}

static void test_topics(void)
{
    char t[128];
    HaDiscovery::topic(t, sizeof(t), v1(), Qty::Temperature);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/explora_00ABCD_temperature/config", t);
    HaDiscovery::topic(t, sizeof(t), v1(), Qty::Co2);
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/explora_00ABCD_co2/config", t);
    HaDiscovery::stateTopic(t, sizeof(t), v1(), Qty::DailyRain);
    TEST_ASSERT_EQUAL_STRING("explora/00ABCD/daily_rain", t);
    HaDiscovery::Device g = v1();
    g.id = "4776B882";
    HaDiscovery::stateTopic(t, sizeof(t), g, Qty::Battery);
    TEST_ASSERT_EQUAL_STRING("explora/4776B882/battery", t);
}

// The 1.x payload of a CLIMA temperature, byte for byte
static void test_gen1_payload_as_before(void)
{
    TEST_ASSERT_EQUAL_STRING(
        "{\"name\":\"Temperature\",\"state_topic\":\"explora/00ABCD/temperature\",\"value_template\":\"{{ value }}\","
        "\"unique_id\":\"explora_00ABCD_temperature\",\"availability_topic\":\"explora/status\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"device_class\":\"temperature\","
        "\"unit_of_measurement\":\"\xC2\xB0" "C\",\"suggested_display_precision\":1,\"device\":{\"identifiers\":"
        "\"00ABCD\",\"name\":\"Living room\",\"model\":\"CLIMA\",\"manufacturer\":\"expLORA\"}}",
        payload(v1(), Qty::Temperature).c_str());
    // CO2 and RSSI without a display precision, battery in V
    const std::string co2 = payload(v1(), Qty::Co2);
    TEST_ASSERT_TRUE(co2.find("\"device_class\":\"carbon_dioxide\",\"unit_of_measurement\":\"ppm\",\"device\"") !=
                     std::string::npos);
    const std::string bat = payload(v1(), Qty::Battery);
    TEST_ASSERT_TRUE(bat.find("\"name\":\"Battery\"") != std::string::npos);
    TEST_ASSERT_TRUE(bat.find("\"device_class\":\"voltage\",\"unit_of_measurement\":\"V\","
                              "\"suggested_display_precision\":2") != std::string::npos);
    TEST_ASSERT_TRUE(payload(v1(), Qty::WindSpeed).find("\"name\":\"Wind_speed\"") != std::string::npos);
}

static void test_entity_names(void)
{
    HaDiscovery::Device d = v1();
    d.name = "Garden";
    TEST_ASSERT_TRUE(payload(d, Qty::DailyRain).find("\"name\":\"Garden Daily Rain Total\"") != std::string::npos);
    d.name = "Cellar Humidity"; // ends with the quantity: used as it is
    TEST_ASSERT_TRUE(payload(d, Qty::Humidity).find("\"name\":\"Cellar Humidity\"") != std::string::npos);
    TEST_ASSERT_TRUE(payload(d, Qty::Temperature).find("\"name\":\"Temperature\"") != std::string::npos);
}

static void test_gen2_device_block(void)
{
    HaDiscovery::Device d = v1();
    d.id = "4776B882";
    d.name = "Bedroom";
    d.model = "Gen 2 HW3";
    d.sw = "0.2.12";
    const std::string p = payload(d, Qty::Humidity);
    TEST_ASSERT_TRUE(p.find("\"unique_id\":\"explora_4776B882_humidity\"") != std::string::npos);
    TEST_ASSERT_TRUE(p.find("\"device\":{\"identifiers\":\"4776B882\",\"name\":\"Bedroom\",\"model\":\"Gen 2 HW3\","
                            "\"manufacturer\":\"expLORA\",\"sw_version\":\"0.2.12\"}}") != std::string::npos);
}

static void test_state_values(void)
{
    char v[24];
    HaDiscovery::stateValue(v, sizeof(v), Qty::Temperature, 21.456f);
    TEST_ASSERT_EQUAL_STRING("21.46", v);
    HaDiscovery::stateValue(v, sizeof(v), Qty::Co2, 1149.9f); // truncated like 1.x int()
    TEST_ASSERT_EQUAL_STRING("1149", v);
    HaDiscovery::stateValue(v, sizeof(v), Qty::Lux, 312.25f);
    TEST_ASSERT_EQUAL_STRING("312.2", v);
    HaDiscovery::stateValue(v, sizeof(v), Qty::Battery, 2.93f);
    TEST_ASSERT_EQUAL_STRING("2.93", v);
    HaDiscovery::stateValue(v, sizeof(v), Qty::Rssi, -71);
    TEST_ASSERT_EQUAL_STRING("-71", v);
}

// Longest names and topics fit the payload limit of MQTTManager (900 B,
// PubSubClient buffer 1 kB with the topic); too small a buffer gives 0
static void test_payload_size(void)
{
    HaDiscovery::Device d;
    d.prefix = "abcdefghijklmnopqrstuvwxyz012345";
    d.haPrefix = "abcdefghijklmnopqrstuvwxyz012345";
    d.id = "FFFFFFFF";
    d.name = "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\""; // 32 quotes, escaped
    d.model = "Gen 2 HW65535";
    d.sw = "255.255.255";
    char buf[900];
    for (int q = 0; q < Qty::COUNT; q++)
    {
        TEST_ASSERT_TRUE(HaDiscovery::payload(buf, sizeof(buf), d, (Qty::Id)q) > 0);
    }
    char small[64];
    TEST_ASSERT_EQUAL_UINT32(0, HaDiscovery::payload(small, sizeof(small), d, Qty::Temperature));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_topics);
    RUN_TEST(test_gen1_payload_as_before);
    RUN_TEST(test_entity_names);
    RUN_TEST(test_gen2_device_block);
    RUN_TEST(test_state_values);
    RUN_TEST(test_payload_size);
    return UNITY_END();
}
