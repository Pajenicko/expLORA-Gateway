/**
 * Native unit tests for gateway 2.0.2: the revert record ends once its
 * version runs confirmed (OtaJob::confirmedRunning) and is persisted that
 * way (Data/Gen2Json.h, sensors.json "revFw" / "revCrc"); the automatic
 * slot offset from the DevID (Gen2Config::autoSlot / resolveSlot) and its
 * persistence ("slot" / "slotAuto", files written before 2.0.2); the image
 * identity rule (OtaImage::needsProvisionedIdentity) and the automatic
 * update skip of a sensor with the development identity.
 *
 * Run:   pio test -e native -f test_gen2_persist
 */

#include <ArduinoJson.h>
#include <string>
#include <unity.h>

#include "../../src/Data/Gen2Json.h"
#include "../../src/Protocol/OtaImage.h"
#include "../../src/Protocol/OtaJob.h"
#include "../../src/Update/AutoUpdatePolicy.h"

void setUp(void) {}
void tearDown(void) {}

static constexpr uint32_t V100 = 0x010000, V101 = 0x010001, V0212 = 0x00020C;

// A sensor record as SensorManager keeps it (the parts these rules touch)
struct Rec
{
    bool hasRev = false;
    uint32_t revFw = 0, revCrc = 0;
    uint32_t fw = 0;      // from the last JOIN_REQ
    uint8_t boot = 0xFF;  // from the last status report
};

// What SensorManager does per UP / JOIN_REQ (handleGen2Frame + otaOnUpLocked)
static void join(Rec &r, uint32_t fw)
{
    if (r.fw != fw)
    {
        r.boot = 0xFF; // another firmware: its boot state is unknown
    }
    r.fw = fw;
    OtaJob::Up u;
    u.joinReq = true;
    u.hasFw = true;
    u.fw = fw;
    if (r.hasRev && OtaJob::confirmedRunning(r.revFw, u, r.boot))
    {
        r.hasRev = false;
        r.revFw = r.revCrc = 0;
    }
}

static void status(Rec &r, uint32_t fw, uint8_t boot, uint8_t flags = 0)
{
    r.boot = boot;
    OtaJob::Up u;
    u.hasStatus = true;
    u.hasFw = true;
    u.fw = fw;
    u.boot = boot;
    u.flags = flags;
    if (r.hasRev && OtaJob::confirmedRunning(r.revFw, u, r.boot))
    {
        r.hasRev = false;
        r.revFw = r.revCrc = 0;
    }
}

static std::string save(const Rec &r)
{
    DynamicJsonDocument doc(512);
    JsonObject o = doc.to<JsonObject>();
    o["devId"] = "4776B882";
    Gen2Json::writeRevert(o, r.hasRev, r.revFw, r.revCrc);
    std::string out;
    serializeJson(doc, out);
    return out;
}

static Rec load(const std::string &json)
{
    DynamicJsonDocument doc(512);
    TEST_ASSERT_TRUE(deserializeJson(doc, json) == DeserializationError::Ok);
    Rec r;
    Gen2Json::readRevert(doc.as<JsonObjectConst>(), r.hasRev, r.revFw, r.revCrc);
    return r;
}

// The night of 2026-10-29: 1.0.0 reverted on bench sensors (no identity
// record), then they were provisioned and 1.0.0 was installed by hand
static void test_revert_record_ends_when_the_version_runs_confirmed(void)
{
    Rec r;
    r.fw = V0212;
    r.boot = 2;
    // the revert: the old firmware runs again, boot state 3 (flagged)
    status(r, V0212, 3, 0x08);
    r.hasRev = true;
    r.revFw = V100;
    r.revCrc = 0x9A3C5E7Fu; // >= 2^31: must read back (| 0u)
    TEST_ASSERT_EQUAL_STRING("{\"devId\":\"4776B882\",\"revFw\":65536,\"revCrc\":2587647615}", save(r).c_str());
    // other version joins / reports: the record stays
    join(r, V0212);
    status(r, V0212, 2);
    TEST_ASSERT_TRUE(r.hasRev);
    // the manual reinstall of 1.0.0: swap, BOOT (1), KEPT (6) - not confirmed
    join(r, V100);
    TEST_ASSERT_TRUE(r.hasRev);
    status(r, V100, 1, 0x08);
    TEST_ASSERT_TRUE(r.hasRev);
    status(r, V100, 6);
    TEST_ASSERT_TRUE(r.hasRev);
    // CONFIRMED (the job's Done comes with this report): the record ends
    status(r, V100, 2);
    TEST_ASSERT_FALSE(r.hasRev);
    const std::string json = save(r);
    TEST_ASSERT_EQUAL_STRING("{\"devId\":\"4776B882\"}", json.c_str()); // persisted as absent
    TEST_ASSERT_FALSE(load(json).hasRev);
}

// A record left by firmware 2.0.1 while 1.0.0 already runs confirmed (the
// bench state before 2.0.2): the first JOIN_REQ after the update clears it
static void test_stale_record_cleared_by_the_join(void)
{
    Rec r = load("{\"devId\":\"4776B882\",\"revFw\":65536,\"revCrc\":27353424}");
    TEST_ASSERT_TRUE(r.hasRev);
    TEST_ASSERT_EQUAL_HEX32(V100, r.revFw);
    TEST_ASSERT_EQUAL_HEX32(27353424, r.revCrc);
    r.fw = V100;
    r.boot = 2; // persisted with the last status report
    join(r, V100);
    TEST_ASSERT_FALSE(r.hasRev);
    // the same JOIN_REQ with an unconfirmed or unknown boot state keeps it
    for (uint8_t boot : {(uint8_t)0xFF, (uint8_t)1, (uint8_t)3, (uint8_t)6})
    {
        Rec k = load("{\"revFw\":65536,\"revCrc\":1}");
        k.fw = V100;
        k.boot = boot;
        join(k, V100);
        TEST_ASSERT_TRUE(k.hasRev);
    }
    // a JOIN_REQ that brings the version clears the old boot state first:
    // a CONFIRMED of the previous firmware never counts for the new one
    Rec n = load("{\"revFw\":65537,\"revCrc\":1}");
    n.fw = V100;
    n.boot = 2;
    join(n, V101);
    TEST_ASSERT_TRUE(n.hasRev);
    status(n, V101, 2);
    TEST_ASSERT_FALSE(n.hasRev);
}

static void test_confirmed_running_rule(void)
{
    OtaJob::Up u;
    TEST_ASSERT_FALSE(OtaJob::confirmedRunning(V100, u, 2)); // no version
    u.hasFw = true;
    u.fw = V100;
    TEST_ASSERT_FALSE(OtaJob::confirmedRunning(V100, u, 2)); // an UP without status / join
    u.hasStatus = true;
    u.boot = 2;
    TEST_ASSERT_TRUE(OtaJob::confirmedRunning(V100, u, 0xFF)); // the report decides, not the stored state
    TEST_ASSERT_FALSE(OtaJob::confirmedRunning(V101, u, 2));
    u.boot = 3;
    TEST_ASSERT_FALSE(OtaJob::confirmedRunning(V100, u, 2));
    OtaJob::Up j;
    j.joinReq = j.hasFw = true;
    j.fw = V100;
    TEST_ASSERT_TRUE(OtaJob::confirmedRunning(V100, j, 2));
    TEST_ASSERT_FALSE(OtaJob::confirmedRunning(V100, j, 6));
}

// --- slot offset ----------------------------------------------------------------------

static void test_auto_slot(void)
{
    // stable, inside the interval, different for the two bench sensors
    TEST_ASSERT_EQUAL_UINT16(92, Gen2Config::autoSlot(0x4776B882, 600));
    TEST_ASSERT_EQUAL_UINT16(513, Gen2Config::autoSlot(0xE686F9A7, 600));
    TEST_ASSERT_EQUAL_UINT16(0, Gen2Config::autoSlot(0x4776B882, 0));
    for (uint32_t id = 0; id < 200000; id += 7)
    {
        const uint16_t iv = (uint16_t)(60 + id % (43200 - 60 + 1));
        TEST_ASSERT_TRUE(Gen2Config::autoSlot(id * 2654435761u, iv) < iv);
    }
    // spread: 600 s, 1000 DevIDs - every second used at most a handful of times
    uint16_t hits[600] = {};
    uint16_t worst = 0;
    for (uint32_t i = 0; i < 1000; i++)
    {
        uint16_t s = Gen2Config::autoSlot(0x4776B800u + i, 600);
        if (++hits[s] > worst)
        {
            worst = hits[s];
        }
    }
    TEST_ASSERT_TRUE(worst <= 7);
    // an automatic slot follows the interval, an explicit one stays
    Gen2Config::Values v;
    TEST_ASSERT_TRUE(v.slotAuto); // new sensors
    Gen2Config::resolveSlot(v, 0x4776B882);
    TEST_ASSERT_EQUAL_UINT16(92, v.slotS);
    v.intervalS = 900;
    Gen2Config::resolveSlot(v, 0x4776B882);
    TEST_ASSERT_EQUAL_UINT16(Gen2Config::autoSlot(0x4776B882, 900), v.slotS);
    v.slotAuto = false;
    v.slotS = 0;
    Gen2Config::resolveSlot(v, 0x4776B882);
    TEST_ASSERT_EQUAL_UINT16(0, v.slotS);
    // switching between automatic and explicit is a change (saved, new version)
    Gen2Config::Values a, b;
    b.slotAuto = false;
    TEST_ASSERT_TRUE(a != b);
}

static Gen2Config::Values loadSlot(const char *json)
{
    DynamicJsonDocument doc(256);
    TEST_ASSERT_TRUE(deserializeJson(doc, json) == DeserializationError::Ok);
    Gen2Config::Values v;
    Gen2Json::readSlot(doc.as<JsonObjectConst>(), v);
    return v;
}

static void test_slot_persistence(void)
{
    // written before 2.0.2: 0 = the old default (automatic), anything else = set by the admin
    Gen2Config::Values v = loadSlot("{\"interval\":600,\"slot\":0}");
    TEST_ASSERT_TRUE(v.slotAuto);
    v = loadSlot("{\"interval\":600,\"slot\":45}");
    TEST_ASSERT_FALSE(v.slotAuto);
    TEST_ASSERT_EQUAL_UINT16(45, v.slotS);
    v = loadSlot("{}"); // before M3: defaults
    TEST_ASSERT_TRUE(v.slotAuto);
    // 2.0.2: the flag decides, also for an explicit 0
    v = loadSlot("{\"slot\":0,\"slotAuto\":false}");
    TEST_ASSERT_FALSE(v.slotAuto);
    v = loadSlot("{\"slot\":92,\"slotAuto\":true}");
    TEST_ASSERT_TRUE(v.slotAuto);
    // round trip
    Gen2Config::Values w;
    w.slotS = 513;
    w.slotAuto = true;
    DynamicJsonDocument doc(256);
    JsonObject o = doc.to<JsonObject>();
    Gen2Json::writeSlot(o, w);
    std::string out;
    serializeJson(doc, out);
    TEST_ASSERT_EQUAL_STRING("{\"slot\":513,\"slotAuto\":true}", out.c_str());
    Gen2Config::Values r = loadSlot(out.c_str());
    TEST_ASSERT_TRUE(r.slotAuto);
    TEST_ASSERT_EQUAL_UINT16(513, r.slotS);
}

// SensorManager's load: cfgVersion, "cfgRun", slot -> the automatic slot and a new version
struct Loaded
{
    Gen2Config::Values v;
    uint8_t cfgVersion = 1;
    uint8_t lastRun = 0;
    bool changed = false;
};

static Loaded loadEntry(const char *json, uint32_t devId)
{
    DynamicJsonDocument doc(256);
    TEST_ASSERT_TRUE(deserializeJson(doc, json) == DeserializationError::Ok);
    JsonObjectConst o = doc.as<JsonObjectConst>();
    Loaded l;
    l.v.intervalS = o["interval"] | l.v.intervalS;
    Gen2Json::readSlot(o, l.v);
    l.cfgVersion = o["cfgVersion"] | l.cfgVersion;
    l.lastRun = Gen2Json::readCfgRun(o, l.cfgVersion);
    l.changed = Gen2Config::resolveSlotAtLoad(l.v, devId, l.cfgVersion, l.lastRun);
    return l;
}

static void test_config_version_at_load(void)
{
    // in sync (no "cfgRun"): the new version differs from the stored one
    Loaded l = loadEntry("{\"interval\":600,\"slot\":0,\"cfgVersion\":5}", 0x4776B882);
    TEST_ASSERT_TRUE(l.changed);
    TEST_ASSERT_EQUAL_UINT16(92, l.v.slotS);
    TEST_ASSERT_EQUAL_UINT8(6, l.cfgVersion);
    // the sensor last reported 6 (an earlier desired version): 6 is skipped
    l = loadEntry("{\"interval\":600,\"slot\":0,\"cfgVersion\":5,\"cfgRun\":6}", 0x4776B882);
    TEST_ASSERT_EQUAL_UINT8(6, l.lastRun);
    TEST_ASSERT_EQUAL_UINT8(7, l.cfgVersion);
    // wrap: 15 -> 1, unless the sensor runs 1
    l = loadEntry("{\"interval\":600,\"slot\":0,\"cfgVersion\":15,\"cfgRun\":1}", 0xE686F9A7);
    TEST_ASSERT_EQUAL_UINT16(513, l.v.slotS);
    TEST_ASSERT_EQUAL_UINT8(2, l.cfgVersion);
    // every stored / reported pair: the new version is neither
    for (unsigned stored = 1; stored <= 15; stored++)
    {
        for (unsigned run = 0; run <= 15; run++)
        {
            char json[96];
            snprintf(json, sizeof(json), "{\"interval\":600,\"slot\":0,\"cfgVersion\":%u,\"cfgRun\":%u}", stored, run);
            l = loadEntry(json, 0x4776B882);
            TEST_ASSERT_TRUE(l.changed);
            TEST_ASSERT_TRUE(Gen2Config::validVersion(l.cfgVersion));
            TEST_ASSERT_NOT_EQUAL(stored, l.cfgVersion);
            TEST_ASSERT_NOT_EQUAL(run == 0 ? stored : run, l.cfgVersion);
        }
    }
    // the slot already resolved (2.0.2 file): no new version
    l = loadEntry("{\"interval\":600,\"slot\":92,\"slotAuto\":true,\"cfgVersion\":6}", 0x4776B882);
    TEST_ASSERT_FALSE(l.changed);
    TEST_ASSERT_EQUAL_UINT8(6, l.cfgVersion);
    // explicit slot: stays, no new version
    l = loadEntry("{\"interval\":600,\"slot\":45,\"cfgVersion\":3}", 0x4776B882);
    TEST_ASSERT_FALSE(l.changed);
    TEST_ASSERT_EQUAL_UINT16(45, l.v.slotS);
    // "cfgRun" is written only while the sensor is not in sync, never 0 / invalid
    const struct
    {
        uint8_t desired, run;
        const char *json;
    } w[] = {{5, 5, "{}"}, {5, 0, "{}"}, {5, 16, "{}"}, {5, 4, "{\"cfgRun\":4}"}, {1, 15, "{\"cfgRun\":15}"}};
    for (const auto &c : w)
    {
        DynamicJsonDocument doc(128);
        JsonObject o = doc.to<JsonObject>();
        Gen2Json::writeCfgRun(o, c.desired, c.run);
        std::string out;
        serializeJson(doc, out);
        TEST_ASSERT_EQUAL_STRING(c.json, out.c_str());
    }
}

// --- image identity -------------------------------------------------------------------

static void test_image_needs_provisioned_identity(void)
{
    const uint32_t ota = 0x101;
    // declared in the trailer (sensor 1.0.1+): the field decides, signed or not
    TEST_ASSERT_TRUE(OtaImage::needsProvisionedIdentity(ota | 0x20, false));
    TEST_ASSERT_TRUE(OtaImage::needsProvisionedIdentity(ota | 0x20, true));
    TEST_ASSERT_FALSE(OtaImage::needsProvisionedIdentity(ota | 0x10, false));
    TEST_ASSERT_FALSE(OtaImage::needsProvisionedIdentity(ota | 0x10, true)); // a bench build stays a bench build
    TEST_ASSERT_TRUE(OtaImage::needsProvisionedIdentity(ota | 0x30, false));  // reserved = provisioned
    // undeclared (before 1.0.1): a release-signed image is a production build (1.0.0)
    TEST_ASSERT_TRUE(OtaImage::needsProvisionedIdentity(ota, true));
    TEST_ASSERT_FALSE(OtaImage::needsProvisionedIdentity(ota, false));
}

static void test_auto_update_skips_development_identity(void)
{
    using namespace AutoUpdatePolicy;
    SensorInfo s;
    s.hasFw = true;
    s.fw = V100;
    ImageInfo img;
    img.stored = img.verified = true;
    img.fw = V101;
    img.needsIdentity = true;
    TEST_ASSERT_EQUAL((int)Skip::None, (int)sensorSkip(s, img)); // provisioned / older firmware (bit 0)
    s.devIdentity = true;
    TEST_ASSERT_EQUAL((int)Skip::DevIdentity, (int)sensorSkip(s, img));
    TEST_ASSERT_TRUE(strstr(skipText(Skip::DevIdentity), "development identity") != nullptr);
    img.needsIdentity = false; // a bench image: allowed
    TEST_ASSERT_EQUAL((int)Skip::None, (int)sensorSkip(s, img));
    img.needsIdentity = true;
    img.fw = V100; // the older rules still come first
    TEST_ASSERT_EQUAL((int)Skip::NotNewer, (int)sensorSkip(s, img));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_revert_record_ends_when_the_version_runs_confirmed);
    RUN_TEST(test_stale_record_cleared_by_the_join);
    RUN_TEST(test_confirmed_running_rule);
    RUN_TEST(test_auto_slot);
    RUN_TEST(test_slot_persistence);
    RUN_TEST(test_config_version_at_load);
    RUN_TEST(test_image_needs_provisioned_identity);
    RUN_TEST(test_auto_update_skips_development_identity);
    return UNITY_END();
}
