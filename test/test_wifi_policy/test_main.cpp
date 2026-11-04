/**
 * Native unit tests for WifiPolicy (the "Enable WiFi" switch and the AP
 * recovery rules of the connectivity policy, see Network/Uplink.h).
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include "../../src/Network/WifiPolicy.h"

using namespace WifiPolicy;

static const uint32_t AP_TIMEOUT_MS = 300000;
static const uint32_t RESTORE_MS = 10000;

void setUp(void) {}
void tearDown(void) {}

static ApInput base()
{
    ApInput in{};
    in.apTimeout = AP_TIMEOUT_MS;
    in.restoreDelay = RESTORE_MS;
    return in;
}

static void test_switch_only_on_boards_with_switch(void)
{
    TEST_ASSERT_TRUE(switchedOff(true, false));
    TEST_ASSERT_FALSE(switchedOff(false, false)); // Lite: setting does not exist
    TEST_ASSERT_FALSE(switchedOff(true, true));
}

static void test_station(void)
{
    TEST_ASSERT_TRUE(stationWanted(false, false, true));
    TEST_ASSERT_FALSE(stationWanted(true, false, true));
    TEST_ASSERT_FALSE(stationWanted(true, true, false)); // switched off without credentials
    TEST_ASSERT_FALSE(stationWanted(false, true, true));
    TEST_ASSERT_FALSE(stationWanted(false, false, false));
}

// WiFi off, ethernet with an address at boot (possibly a wrong static one):
// the AP runs for the boot grace, then stops and the driver with it
static void test_off_boot_grace_then_off(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.localAccess = true;
    in.localAccessSince = 0; // static address: at once
    in.bootGrace = true;
    for (uint32_t t = 0; t < AP_TIMEOUT_MS; t += 1000)
    {
        in.now = t;
        in.apActive = t > 0;
        TEST_ASSERT_TRUE(apWanted(in));
    }
    in.bootGrace = false; // latched by the caller after AP_TIMEOUT
    in.apActive = true;
    for (uint32_t t = AP_TIMEOUT_MS; t < 3 * AP_TIMEOUT_MS; t += 1000)
    {
        in.now = t;
        TEST_ASSERT_FALSE(apWanted(in));
        in.apActive = false;
    }
    TEST_ASSERT_FALSE(driverWanted(false, false));
}

// Boot grace also without credentials / in configuration mode (the switch
// applies there too) and when the first AP start failed
static void test_off_boot_grace_retries(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.stationWanted = false;
    in.localAccess = true;
    in.localAccessSince = 3000;
    in.bootGrace = true;
    in.apActive = false; // start failed, retried
    in.now = 4000;
    TEST_ASSERT_TRUE(apWanted(in));
    TEST_ASSERT_TRUE(driverWanted(false, true));
}

// DHCP a few seconds after boot: the AP stays until the later of the two
static void test_off_ethernet_late_at_boot(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.apActive = true;
    in.localAccess = true;
    in.localAccessSince = 4000;
    in.bootGrace = false;
    in.now = AP_TIMEOUT_MS + 3999;
    TEST_ASSERT_TRUE(apWanted(in));
    in.now = AP_TIMEOUT_MS + 4000;
    TEST_ASSERT_FALSE(apWanted(in));
}

// WiFi off, no ethernet address at boot: the AP from the boot, and it stays
static void test_off_without_ethernet_keeps_ap(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.bootGrace = true;
    in.localAccessLostAt = 0;
    in.now = 0;
    TEST_ASSERT_TRUE(apWanted(in));
    in.apActive = true;
    in.bootGrace = false;
    in.now = AP_TIMEOUT_MS + 3600000u;
    TEST_ASSERT_TRUE(apWanted(in));
    // AP failed for good (no memory): still wanted, retried
    in.apActive = false;
    TEST_ASSERT_TRUE(apWanted(in));
}

// WiFi off, ethernet lost later: AP after the restore delay, stays
static void test_off_without_ethernet_restores_ap(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.localAccessLostAt = 1000000;
    in.now = 1000000 + RESTORE_MS;
    TEST_ASSERT_FALSE(apWanted(in)); // short outages do not count
    in.now = 1000001 + RESTORE_MS;
    TEST_ASSERT_TRUE(apWanted(in));
    TEST_ASSERT_TRUE(driverWanted(false, true));
    in.apActive = true;
    in.now += 3600000u; // stays up while there is no local access
    TEST_ASSERT_TRUE(apWanted(in));
}

// WiFi off, ethernet comes back: the AP times out, then the driver stops
static void test_off_ethernet_back_ap_times_out(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.apActive = true;
    in.localAccess = true;
    in.localAccessSince = 500000;
    in.now = 500000 + AP_TIMEOUT_MS - 1;
    TEST_ASSERT_TRUE(apWanted(in));
    in.now = 500000 + AP_TIMEOUT_MS;
    TEST_ASSERT_FALSE(apWanted(in));
    TEST_ASSERT_FALSE(driverWanted(false, false));
    // never started again while ethernet stays
    in.apActive = false;
    in.now += 1000;
    TEST_ASSERT_FALSE(apWanted(in));
}

// WiFi off, a short ethernet drop does not bring the AP up
static void test_off_short_outage(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.localAccessLostAt = 1000000;
    in.now = 1000000 + RESTORE_MS / 2;
    TEST_ASSERT_FALSE(apWanted(in));
}

// Unchanged behaviour with WiFi on (the boot grace does not apply)
static void test_on_unchanged(void)
{
    ApInput in = base();
    in.bootGrace = true;
    // no credentials: AP immediately without local access
    in.localAccessLostAt = 1000;
    in.now = 1001;
    TEST_ASSERT_TRUE(apWanted(in));
    // credentials, station retrying: after the restore delay
    in.stationWanted = true;
    TEST_ASSERT_FALSE(apWanted(in));
    in.now = 1001 + RESTORE_MS;
    TEST_ASSERT_TRUE(apWanted(in));
    // local access: AP kept only for AP_TIMEOUT, never started
    in.localAccess = true;
    in.localAccessSince = 2000;
    in.apActive = false;
    TEST_ASSERT_FALSE(apWanted(in));
    in.apActive = true;
    in.bootGrace = false;
    in.now = 2000 + AP_TIMEOUT_MS;
    TEST_ASSERT_FALSE(apWanted(in));
    TEST_ASSERT_TRUE(driverWanted(true, false));
}

// millis() wrap
static void test_wrap(void)
{
    ApInput in = base();
    in.switchedOff = true;
    in.localAccessLostAt = 0xFFFFFFF0u;
    in.now = 0x10;
    TEST_ASSERT_FALSE(apWanted(in));
    in.now = RESTORE_MS + 0x10;
    TEST_ASSERT_TRUE(apWanted(in));
    // AP timeout across the wrap
    in.localAccess = true;
    in.apActive = true;
    in.localAccessSince = 0xFFFFFF00u;
    in.now = AP_TIMEOUT_MS - 0x101;
    TEST_ASSERT_TRUE(apWanted(in));
    in.now = AP_TIMEOUT_MS;
    TEST_ASSERT_FALSE(apWanted(in));
}

// The boot grace runs from the first AP start, not from boot: an AP that
// could not start for minutes still gets its full time
static void test_boot_grace_counts_from_ap_up(void)
{
    TEST_ASSERT_FALSE(bootGraceOver(false, 10 * AP_TIMEOUT_MS, 0, AP_TIMEOUT_MS)); // never up
    TEST_ASSERT_FALSE(bootGraceOver(true, 400000, 200000, AP_TIMEOUT_MS));          // up at 200 s
    TEST_ASSERT_TRUE(bootGraceOver(true, 500000, 200000, AP_TIMEOUT_MS));
    TEST_ASSERT_TRUE(bootGraceOver(true, 5000, 0xFFFFFFFFu - AP_TIMEOUT_MS + 5000, AP_TIMEOUT_MS)); // wrap
    TEST_ASSERT_FALSE(bootGraceOver(true, 4000, 0xFFFFFFFFu - AP_TIMEOUT_MS + 5000, AP_TIMEOUT_MS));
}

static void test_ap_starved(void)
{
    TEST_ASSERT_FALSE(apStarved(true, 0, true, 3));  // no failure
    TEST_ASSERT_TRUE(apStarved(true, 1, true, 3));
    TEST_ASSERT_TRUE(apStarved(true, 3, true, 3));
    TEST_ASSERT_FALSE(apStarved(true, 4, true, 3));  // holding did not help: released
    TEST_ASSERT_FALSE(apStarved(true, 1, false, 3)); // failed with memory to spare
    TEST_ASSERT_FALSE(apStarved(false, 1, true, 3)); // other local access exists
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_switch_only_on_boards_with_switch);
    RUN_TEST(test_station);
    RUN_TEST(test_off_boot_grace_then_off);
    RUN_TEST(test_off_boot_grace_retries);
    RUN_TEST(test_off_ethernet_late_at_boot);
    RUN_TEST(test_off_without_ethernet_keeps_ap);
    RUN_TEST(test_off_without_ethernet_restores_ap);
    RUN_TEST(test_off_ethernet_back_ap_times_out);
    RUN_TEST(test_off_short_outage);
    RUN_TEST(test_on_unchanged);
    RUN_TEST(test_wrap);
    RUN_TEST(test_boot_grace_counts_from_ap_up);
    RUN_TEST(test_ap_starved);
    return UNITY_END();
}
