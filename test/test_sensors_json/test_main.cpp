/**
 * Native unit tests for SensorsJsonBudget: a full sensors.json (20
 * generation 1 + 10 generation 2 sensors, every optional member, longest
 * strings) fits the write budget and loads with the read budget; a file
 * with extra members per sensor (another firmware version) loads; a much
 * larger one loads through the 2x retry or is NoMemory, never Damage; a
 * cut file is Damage. The members mirror SensorManager::snapshotLocked -
 * keep them in step. Keys and string values are std::string, which
 * ArduinoJson copies into the document like the firmware's String values
 * (a const char * would be stored by pointer and not count). Every string
 * value is unique per entry: ArduinoJson deduplicates equal strings, so
 * repeated values would hide the text budget.
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <cstdio>
#include <string>
#include "../../src/Data/SensorsJsonBudget.h"
#include "../../src/Data/SensorTypeIds.h"

using SensorsJson::MAX_GEN1;
using SensorsJson::MAX_GEN2;

void setUp(void) {}
void tearDown(void) {}

// A string of exactly len characters, unique per (fill, i): the index is
// written over the end
static std::string uniq(char fill, size_t len, size_t i)
{
    std::string s(len, fill);
    std::string n = std::to_string(i);
    s.replace(len - n.size(), n.size(), n);
    return s;
}

// Builds the document like snapshotLocked; extra > 0 adds that many unknown
// members per entry and arrays of arrayLen (a newer firmware's file)
static std::string buildFull(size_t extra, size_t arrayLen, bool &overflowed)
{
    // Longest values: name 32, URL 200, devId 8, label 16, fw "255.255.255"
    size_t text = 0;
    for (size_t i = 0; i < MAX_GEN1; i++)
    {
        text += 32 + 200 + 2;
    }
    for (size_t i = 0; i < MAX_GEN2; i++)
    {
        text += 32 + 32 + 12;
    }
    size_t cap = SensorsJson::writeCapacity(MAX_GEN1, MAX_GEN2, text);
    if (extra)
    {
        cap *= 8; // the newer writer's own budget (keys copied too), not under test
    }
    DynamicJsonDocument doc(cap);
    JsonArray s = doc.createNestedArray(std::string("sensors"));
    for (size_t i = 0; i < MAX_GEN1; i++)
    {
        JsonObject o = s.createNestedObject();
        o[std::string("deviceType")] = 7;
        o[std::string("serialNumber")] = 4294967295u;
        o[std::string("deviceKey")] = 4294967295u;
        o[std::string("name")] = uniq('N', 32, i);
        o[std::string("customUrl")] = "https://" + uniq('u', 192, i);
        o[std::string("altitude")] = -123.5f;
        o[std::string("dailyRainTotal")] = 123.25f;
        o[std::string("lastRainReset")] = 4294967295u;
        const char *corr[] = {"temperatureCorrection", "humidityCorrection", "pressureCorrection",
                              "ppmCorrection", "luxCorrection", "windSpeedCorrection",
                              "windDirectionCorrection", "rainAmountCorrection", "rainRateCorrection"};
        for (const char *c : corr)
        {
            o[std::string(c)] = -1.5f;
        }
        for (size_t x = 0; x < extra; x++)
        {
            o[std::string("future") + std::to_string(x)] = 1;
        }
    }
    JsonArray g = doc.createNestedArray(std::string("gen2"));
    for (size_t i = 0; i < MAX_GEN2; i++)
    {
        JsonObject o = g.createNestedObject();
        char devId[9];
        snprintf(devId, sizeof(devId), "%08X", (unsigned)(0x4776B800u + i));
        o[std::string("devId")] = std::string(devId);
        o[std::string("name")] = uniq('M', 32, i);
        o[std::string("label")] = uniq('L', 16, i);
        o[std::string("devNonce")] = 4294967295u;
        o[std::string("interval")] = 86400;
        o[std::string("slot")] = 86399;
        o[std::string("txPower")] = -9;
        o[std::string("ackEvery")] = 255;
        o[std::string("ackRetries")] = 15;
        o[std::string("ackBackoff")] = 15;
        o[std::string("cfgVersion")] = 65535;
        o[std::string("q")] = 65535;
        const char *cal[] = {"temperatureCorrection", "humidityCorrection", "pressureCorrection", "ppmCorrection",
                             "luxCorrection", "windSpeedCorrection", "windDirectionCorrection",
                             "rainAmountCorrection", "rainRateCorrection", "altitude"};
        for (const char *c : cal)
        {
            o[std::string(c)] = -1.5f; // worst case: every correction set
        }
        o[std::string("fw")] = "255.255." + std::to_string(200 + i);
        o[std::string("hw")] = 65535;
        o[std::string("bootState")] = 7;
        o[std::string("revFw")] = 4294967295u;
        o[std::string("revCrc")] = 4294967295u;
        JsonArray w = o.createNestedArray(std::string("otaWatch"));
        for (size_t k = 0; k < (extra ? arrayLen : 5); k++)
        {
            w.add(4294967295u);
        }
        JsonArray a = o.createNestedArray(std::string("otaAbort"));
        for (size_t k = 0; k < (extra ? arrayLen : 3); k++)
        {
            a.add(4294967295u);
        }
        for (size_t x = 0; x < extra; x++)
        {
            o[std::string("future") + std::to_string(x)] = 1;
        }
    }
    overflowed = doc.overflowed();
    std::string out;
    serializeJson(doc, out);
    return out;
}

static SensorsJson::Load load(const std::string &file, size_t &factor)
{
    std::unique_ptr<DynamicJsonDocument> doc;
    return SensorsJson::parse(file.data(), file.size(), doc, factor);
}

static void test_full_configuration_fits_write_and_read(void)
{
    bool overflowed = true;
    std::string file = buildFull(0, 0, overflowed);
    TEST_ASSERT_FALSE(overflowed); // the write budget holds, strings included
    TEST_ASSERT_TRUE(file.size() < SensorsJson::MAX_FILE_BYTES);
    size_t factor = 0;
    TEST_ASSERT_TRUE(load(file, factor) == SensorsJson::Load::Ok);
    TEST_ASSERT_EQUAL_UINT32(1, factor);
}

static void test_file_with_extra_members_loads(void)
{
    bool overflowed = true;
    std::string file = buildFull(4, 8, overflowed); // 21 / 33 members, arrays of 8
    TEST_ASSERT_FALSE(overflowed);
    size_t factor = 0;
    TEST_ASSERT_TRUE(load(file, factor) == SensorsJson::Load::Ok);
    TEST_ASSERT_EQUAL_UINT32(1, factor); // within the slack
}

static void test_larger_file_takes_the_retry(void)
{
    bool overflowed = true;
    std::string file = buildFull(60, 8, overflowed); // ~77 members per entry
    TEST_ASSERT_FALSE(overflowed);
    size_t factor = 0;
    TEST_ASSERT_TRUE(load(file, factor) == SensorsJson::Load::Ok);
    TEST_ASSERT_EQUAL_UINT32(2, factor);
}

static void test_much_larger_file_is_no_memory_not_damage(void)
{
    // Valid JSON whose values need far more document than text: 20,000 tiny
    // array elements (one slot each) - beyond twice the budget
    std::string file = "{\"sensors\":[],\"gen2\":[{\"otaWatch\":[1";
    for (int i = 0; i < 20000; i++)
    {
        file += ",1";
    }
    file += "]}]}";
    size_t factor = 0;
    TEST_ASSERT_TRUE(load(file, factor) == SensorsJson::Load::NoMemory);
    TEST_ASSERT_EQUAL_UINT32(2, factor);
}

static void test_cut_file_is_damage(void)
{
    bool overflowed = true;
    std::string file = buildFull(0, 0, overflowed);
    file.resize(file.size() / 2);
    size_t factor = 0;
    TEST_ASSERT_TRUE(load(file, factor) == SensorsJson::Load::Damage);
}

// A file with entries of removed types (SHT40 0x05, DIY_TEMP 0x51) and an
// out-of-range type loads; the loader keeps only the supported ones, read
// the way SensorManager::parseSensorsLocked reads them (as<uint8_t>(): 0
// when out of range, never truncated to another type)
static void test_removed_types_are_dropped(void)
{
    const std::string file = "{\"sensors\":[{\"deviceType\":1,\"serialNumber\":1},{\"deviceType\":5,"
                             "\"serialNumber\":535578},{\"deviceType\":81,\"serialNumber\":3},{\"deviceType\":3,"
                             "\"serialNumber\":4},{\"deviceType\":257,\"serialNumber\":5}],\"gen2\":[]}";
    std::unique_ptr<DynamicJsonDocument> doc;
    size_t factor = 0;
    TEST_ASSERT_TRUE(SensorsJson::parse(file.data(), file.size(), doc, factor) == SensorsJson::Load::Ok);
    unsigned kept = 0;
    uint32_t sum = 0;
    for (JsonObject o : (*doc)["sensors"].as<JsonArray>())
    {
        if (SensorTypeIds::supportedType(o["deviceType"].as<uint8_t>()))
        {
            kept++;
            sum += o["serialNumber"].as<uint32_t>();
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2, kept);
    TEST_ASSERT_EQUAL_UINT32(1 + 4, sum);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_full_configuration_fits_write_and_read);
    RUN_TEST(test_file_with_extra_members_loads);
    RUN_TEST(test_larger_file_takes_the_retry);
    RUN_TEST(test_much_larger_file_is_no_memory_not_damage);
    RUN_TEST(test_cut_file_is_damage);
    RUN_TEST(test_removed_types_are_dropped);
    return UNITY_END();
}
