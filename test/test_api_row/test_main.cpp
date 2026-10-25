/**
 * Native unit tests for ApiRow: /api?format=json rows of generation 1 and 2
 * sensors have the same keys in the same order; a generation 2 row adds
 * "gen":2 and carries its DevID; values are rounded like 1.x; a quantity
 * without a value (never reported) is null; the keys follow the quantities
 * the sensor provides
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <string>
#include <vector>
#include "../../src/Web/ApiRow.h"

void setUp(void) {}
void tearDown(void) {}

static std::string json(const ApiRow::Row &r)
{
    StaticJsonDocument<768> doc;
    ApiRow::toJson(doc.to<JsonObject>(), r);
    TEST_ASSERT_FALSE(doc.overflowed());
    std::string out;
    serializeJson(doc, out);
    return out;
}

static ApiRow::Row clima()
{
    ApiRow::Row r;
    r.name = "Living room";
    r.deviceType = 1;
    r.typeName = "CLIMA";
    r.id = "00ABCD";
    r.q = Qty::mask(Qty::Temperature) | Qty::mask(Qty::Humidity) | Qty::mask(Qty::Pressure) |
          Qty::mask(Qty::Battery) | Qty::mask(Qty::Rssi);
    r.qValid = r.q;
    r.v[Qty::Temperature] = 22.456f;
    r.v[Qty::Humidity] = 45.1f;
    r.v[Qty::Pressure] = 1013.249f;
    r.v[Qty::Battery] = 3.0249f;
    r.v[Qty::Rssi] = -71;
    r.lastSeenS = 120;
    return r;
}

static void test_gen1_row(void)
{
    TEST_ASSERT_EQUAL_STRING("{\"deviceType\":1,\"typeName\":\"CLIMA\",\"serialNumber\":\"00ABCD\",\"name\":\"Living "
                             "room\",\"temperature\":22.46,\"humidity\":45.1,\"pressure\":1013.25,"
                             "\"batteryVoltage\":3.02,\"rssi\":-71,\"lastSeen\":120}",
                             json(clima()).c_str());
}

static void test_gen2_row_same_keys_plus_gen(void)
{
    ApiRow::Row r = clima();
    r.name = "Bedroom";
    r.deviceType = 0;
    r.typeName = "GEN2";
    r.id = "4776B882";
    r.gen2 = true;
    TEST_ASSERT_EQUAL_STRING("{\"deviceType\":0,\"typeName\":\"GEN2\",\"serialNumber\":\"4776B882\",\"name\":"
                             "\"Bedroom\",\"gen\":2,\"temperature\":22.46,\"humidity\":45.1,\"pressure\":1013.25,"
                             "\"batteryVoltage\":3.02,\"rssi\":-71,\"lastSeen\":120}",
                             json(r).c_str());
}

static void test_never_reported_is_null(void)
{
    ApiRow::Row r = clima();
    r.qValid = 0;
    r.lastSeenS = -1;
    TEST_ASSERT_EQUAL_STRING("{\"deviceType\":1,\"typeName\":\"CLIMA\",\"serialNumber\":\"00ABCD\",\"name\":\"Living "
                             "room\",\"temperature\":null,\"humidity\":null,\"pressure\":null,"
                             "\"batteryVoltage\":null,\"rssi\":null,\"lastSeen\":-1}",
                             json(r).c_str());
}

static void test_keys_follow_the_quantities(void)
{
    // Gen 2 that reported T, CO2 and lux: no pressure / humidity keys;
    // ppm whole, lux one decimal
    ApiRow::Row r;
    r.name = "Office";
    r.typeName = "GEN2";
    r.id = "E686F9A7";
    r.gen2 = true;
    r.q = Qty::mask(Qty::Temperature) | Qty::mask(Qty::Co2) | Qty::mask(Qty::Lux) | Qty::mask(Qty::Rssi);
    r.qValid = r.q;
    r.v[Qty::Temperature] = -5.004f;
    r.v[Qty::Co2] = 1149.6f;
    r.v[Qty::Lux] = 312.25f;
    r.v[Qty::Rssi] = -38;
    r.lastSeenS = 3;
    TEST_ASSERT_EQUAL_STRING("{\"deviceType\":0,\"typeName\":\"GEN2\",\"serialNumber\":\"E686F9A7\",\"name\":"
                             "\"Office\",\"gen\":2,\"temperature\":-5,\"ppm\":1150,\"lux\":312.3,\"rssi\":-38,"
                             "\"lastSeen\":3}",
                             json(r).c_str());
}

static void test_meteo_keys_in_the_old_order(void)
{
    ApiRow::Row r;
    r.name = "Garden";
    r.deviceType = 3;
    r.typeName = "METEO";
    r.id = "475481";
    r.q = 0x0FFF & ~(Qty::mask(Qty::Co2) | Qty::mask(Qty::Lux));
    r.qValid = r.q;
    r.v[Qty::WindDir] = 225;
    r.v[Qty::Rain] = 0.4f;
    r.v[Qty::DailyRain] = 6.2f;
    r.lastSeenS = 40;
    const std::string s = json(r);
    const char *order[] = {"temperature", "humidity", "pressure", "windSpeed", "windDirection", "rainAmount",
                           "dailyRainTotal", "rainRate", "batteryVoltage", "rssi", "lastSeen"};
    size_t at = 0;
    for (const char *k : order)
    {
        size_t p = s.find(std::string("\"") + k + "\"");
        TEST_ASSERT_TRUE_MESSAGE(p != std::string::npos && p > at, k);
        at = p;
    }
    TEST_ASSERT_TRUE(s.find("\"windDirection\":225,") != std::string::npos);
}

static std::string csv(const ApiRow::Row &r)
{
    std::string out;
    ApiRow::toCsv(out, r);
    return out;
}

// CSV: name quoted, type, ID, lastSeen, then T,H,P,ppm,lux,battery,rssi in
// the 1.x decimals; a column without a value (or not provided) is empty
static void test_csv_rows(void)
{
    TEST_ASSERT_EQUAL_STRING("\"Living room\",1,00ABCD,120,22.46,45.10,1013.25,,,3.02,-71\r\n",
                             csv(clima()).c_str());
    ApiRow::Row r = clima();
    r.qValid = 0;
    r.lastSeenS = -1;
    TEST_ASSERT_EQUAL_STRING("\"Living room\",1,00ABCD,-1,,,,,,,\r\n", csv(r).c_str());
    // Gen 2: type 0, DevID; a failed humidity (not in qValid) is empty
    r = clima();
    r.deviceType = 0;
    r.id = "4776B882";
    r.gen2 = true;
    r.qValid &= (uint16_t)~Qty::mask(Qty::Humidity);
    TEST_ASSERT_EQUAL_STRING("\"Living room\",0,4776B882,120,22.46,,1013.25,,,3.02,-71\r\n", csv(r).c_str());
    // Quotes doubled, a formula start defused
    r.name = "=1+\"x\"";
    TEST_ASSERT_EQUAL_STRING("\"'=1+\"\"x\"\"\",0,4776B882,120,22.46,,1013.25,,,3.02,-71\r\n", csv(r).c_str());
}

struct IdRow
{
    uint32_t serialNumber;
    bool gen2;
};

// /api?sensor=ID never gives two rows: gen 1 first, gen=2 / gen=1 choose
static void test_select_one_sensor(void)
{
    const std::vector<IdRow> rows = {{0x123456, true}, {0xABCDEF, false}, {0x123456, false}, {0x4776B882, true}};
    TEST_ASSERT_EQUAL(2, ApiRow::select(rows, 0x123456, 0));
    TEST_ASSERT_EQUAL(2, ApiRow::select(rows, 0x123456, 1));
    TEST_ASSERT_EQUAL(0, ApiRow::select(rows, 0x123456, 2));
    TEST_ASSERT_EQUAL(3, ApiRow::select(rows, 0x4776B882, 0));
    TEST_ASSERT_EQUAL(-1, ApiRow::select(rows, 0x4776B882, 1));
    TEST_ASSERT_EQUAL(1, ApiRow::select(rows, 0xABCDEF, 0));
    TEST_ASSERT_EQUAL(-1, ApiRow::select(rows, 0xABCDEF, 2));
    TEST_ASSERT_EQUAL(-1, ApiRow::select(rows, 0x1, 0));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_gen1_row);
    RUN_TEST(test_gen2_row_same_keys_plus_gen);
    RUN_TEST(test_never_reported_is_null);
    RUN_TEST(test_keys_follow_the_quantities);
    RUN_TEST(test_meteo_keys_in_the_old_order);
    RUN_TEST(test_csv_rows);
    RUN_TEST(test_select_one_sensor);
    return UNITY_END();
}
