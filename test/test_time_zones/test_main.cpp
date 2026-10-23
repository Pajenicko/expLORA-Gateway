/**
 * Native unit tests for TimeZones (the time zone list of the settings).
 *
 * Pins the lookup by name, the migration of stored settings from before the
 * list (only a POSIX string: the five old options, other known rules, custom
 * strings, nothing stored), and checks every POSIX string with the host's
 * tzset(): the standard offset written in the string must come out of the C
 * library, so a typo cannot silently fall back to UTC on the device. Every
 * entry is also compared with the host's zoneinfo (hourly 2026-2030 and at
 * every transition to the second), skipped without /usr/share/zoneinfo or
 * with a tzdata older than the table's snapshot.
 *
 * Run:   pio test -e native
 */

#include <unity.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>
#include "../../src/Data/TimeZones.h"

using TimeZones::COUNT;
using TimeZones::ZONES;

void setUp(void) {}
void tearDown(void) {}

static const char *nameOf(int index)
{
    return index >= 0 ? ZONES[index].name : "(custom)";
}

// Standard offset written in a POSIX string, seconds east of UTC
static long standardOffset(const char *posix)
{
    const char *p = posix;
    if (*p == '<')
    {
        p = strchr(p, '>') + 1;
    }
    else
    {
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
            p++;
    }
    int sign = 1;
    if (*p == '+' || *p == '-')
    {
        sign = *p == '-' ? -1 : 1;
        p++;
    }
    long seconds = strtol(p, (char **)&p, 10) * 3600;
    if (*p == ':')
    {
        seconds += strtol(p + 1, nullptr, 10) * 60;
    }
    return -sign * seconds; // POSIX counts west of UTC as positive
}

static struct tm localAt(const char *posix, time_t t)
{
    setenv("TZ", posix, 1);
    tzset();
    struct tm result;
    localtime_r(&t, &result);
    return result;
}

static const time_t JAN_15 = 1768478400; // 2026-01-15 12:00 UTC
static const time_t JUL_15 = 1784116800; // 2026-07-15 12:00 UTC

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

static void test_names_are_unique_and_sorted(void)
{
    TEST_ASSERT_EQUAL_STRING("UTC", ZONES[0].name);
    for (size_t i = 1; i < COUNT; i++)
    {
        TEST_ASSERT_TRUE_MESSAGE(i == 1 || strcmp(ZONES[i - 1].name, ZONES[i].name) < 0, ZONES[i].name);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)i, TimeZones::find(ZONES[i].name), ZONES[i].name);
    }
}

static void test_names_are_safe_in_html(void)
{
    for (size_t i = 0; i < COUNT; i++)
    {
        for (const char *c = ZONES[i].name; *c; c++)
        {
            bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '/' || *c == '_';
            TEST_ASSERT_TRUE_MESSAGE(ok, ZONES[i].name);
        }
    }
}

static void test_whole_of_europe_is_listed(void)
{
    const char *europe[] = {"Europe/Prague", "Europe/Bratislava", "Europe/Berlin", "Europe/Vienna", "Europe/Warsaw",
                            "Europe/London", "Europe/Dublin", "Europe/Lisbon", "Europe/Madrid", "Europe/Paris",
                            "Europe/Rome", "Europe/Athens", "Europe/Helsinki", "Europe/Kyiv", "Europe/Istanbul",
                            "Europe/Moscow", "Europe/Minsk", "Europe/Chisinau", "Asia/Nicosia", "Atlantic/Reykjavik"};
    for (const char *name : europe)
    {
        TEST_ASSERT_TRUE_MESSAGE(TimeZones::find(name) >= 0, name);
    }
    TEST_ASSERT_TRUE(COUNT >= 100);
}

static void test_default_is_prague_with_eu_rules(void)
{
    int index = TimeZones::find(TimeZones::DEFAULT_NAME);
    TEST_ASSERT_TRUE(index >= 0);
    TEST_ASSERT_EQUAL_STRING("Europe/Prague", ZONES[index].name);
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", ZONES[index].posix);
}

static void test_find_rejects_unknown_and_null(void)
{
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::find("Europe/Atlantis"));
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::find(""));
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::find(nullptr));
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::find("europe/prague")); // exact names only
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::find("CET-1CEST,M3.5.0,M10.5.0/3"));
}

// ---------------------------------------------------------------------------
// Every POSIX string is understood by a C library
// ---------------------------------------------------------------------------

static void test_every_rule_parses_with_its_standard_offset(void)
{
    for (size_t i = 0; i < COUNT; i++)
    {
        struct tm jan = localAt(ZONES[i].posix, JAN_15);
        struct tm jul = localAt(ZONES[i].posix, JUL_15);
        // Standard time in one of the two months (northern or southern DST)
        TEST_ASSERT_TRUE_MESSAGE(!jan.tm_isdst || !jul.tm_isdst, ZONES[i].name);
        const struct tm &standard = jan.tm_isdst ? jul : jan;
        TEST_ASSERT_EQUAL_INT_MESSAGE(standardOffset(ZONES[i].posix), standard.tm_gmtoff, ZONES[i].name);
        // A rule with DST must switch: one hour more in the other month
        if (strchr(ZONES[i].posix, ','))
        {
            const struct tm &summer = jan.tm_isdst ? jan : jul;
            TEST_ASSERT_TRUE_MESSAGE(summer.tm_isdst, ZONES[i].name);
            TEST_ASSERT_EQUAL_INT_MESSAGE(standard.tm_gmtoff + 3600, summer.tm_gmtoff, ZONES[i].name);
        }
    }
}

static void test_known_offsets_and_dst(void)
{
    struct
    {
        const char *name;
        long janOffset, julOffset;
        int janDst, julDst;
    } cases[] = {
        {"Europe/Prague", 3600, 7200, 0, 1},       {"Europe/London", 0, 3600, 0, 1},
        {"Europe/Dublin", 0, 3600, 0, 1},          {"Europe/Lisbon", 0, 3600, 0, 1},
        {"Europe/Helsinki", 7200, 10800, 0, 1},    {"Europe/Moscow", 10800, 10800, 0, 0},
        {"Europe/Istanbul", 10800, 10800, 0, 0},   {"Atlantic/Azores", -3600, 0, 0, 1},
        {"America/New_York", -18000, -14400, 0, 1}, {"America/St_Johns", -12600, -9000, 0, 1},
        {"America/Santiago", -10800, -14400, 1, 0}, {"Australia/Sydney", 39600, 36000, 1, 0},
        {"Australia/Adelaide", 37800, 34200, 1, 0}, {"Pacific/Auckland", 46800, 43200, 1, 0},
        {"Asia/Kathmandu", 20700, 20700, 0, 0},    {"Asia/Kolkata", 19800, 19800, 0, 0},
        {"Asia/Jerusalem", 7200, 10800, 0, 1},     {"Africa/Cairo", 7200, 10800, 0, 1},
        {"UTC", 0, 0, 0, 0},
    };
    for (const auto &c : cases)
    {
        int index = TimeZones::find(c.name);
        TEST_ASSERT_TRUE_MESSAGE(index >= 0, c.name);
        struct tm jan = localAt(ZONES[index].posix, JAN_15);
        struct tm jul = localAt(ZONES[index].posix, JUL_15);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c.janOffset, jan.tm_gmtoff, c.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c.julOffset, jul.tm_gmtoff, c.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c.janDst, jan.tm_isdst, c.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c.julDst, jul.tm_isdst, c.name);
    }
}

static void test_eu_switch_hours_in_prague(void)
{
    const char *cet = ZONES[TimeZones::find("Europe/Prague")].posix;
    // 2026-03-29 00:59:59 UTC = 01:59:59 CET, one second later 03:00:00 CEST
    struct tm before = localAt(cet, 1774745999);
    struct tm after = localAt(cet, 1774746000);
    TEST_ASSERT_EQUAL_INT(1, before.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, before.tm_isdst);
    TEST_ASSERT_EQUAL_INT(3, after.tm_hour);
    TEST_ASSERT_EQUAL_INT(1, after.tm_isdst);
    // 2026-10-25 00:59:59 UTC = 02:59:59 CEST, then 02:00:00 CET again
    before = localAt(cet, 1792889999);
    after = localAt(cet, 1792890000);
    TEST_ASSERT_EQUAL_INT(2, before.tm_hour);
    TEST_ASSERT_EQUAL_INT(1, before.tm_isdst);
    TEST_ASSERT_EQUAL_INT(2, after.tm_hour);
    TEST_ASSERT_EQUAL_INT(0, after.tm_isdst);
}

// ---------------------------------------------------------------------------
// Every entry against the host's zoneinfo (the IANA tz database)
// ---------------------------------------------------------------------------

static const char *const ZONEINFO = "/usr/share/zoneinfo";
static const time_t CHECK_FROM = 1767225600; // 2026-01-01 00:00 UTC
static const time_t CHECK_TO = 1924992000;   // 2031-01-01 00:00 UTC

// Zones whose rule changed during 2026: the table holds the new rule, so the
// comparison starts at the change
static time_t checkStart(const char *name)
{
    if (strcmp(name, "America/Vancouver") == 0)
    {
        return 1772964000; // 2026-03-08 10:00 UTC: PDT, then MST7 from 2026-11-01
    }
    if (strcmp(name, "Africa/Casablanca") == 0)
    {
        return 1789866000; // 2026-09-20 01:00 UTC: permanent +00
    }
    return CHECK_FROM;
}

// "2026c" from tzdata.zi, empty = unknown
static std::string hostTzdataVersion()
{
    std::string version;
    FILE *file = fopen((std::string(ZONEINFO) + "/tzdata.zi").c_str(), "r");
    if (file)
    {
        char line[64] = {0};
        if (fgets(line, sizeof(line), file) && strncmp(line, "# version ", 10) == 0)
        {
            version = line + 10;
            while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
                version.pop_back();
        }
        fclose(file);
    }
    return version;
}

static long offsetAt(time_t t)
{
    struct tm result;
    localtime_r(&t, &result);
    return result.tm_gmtoff;
}

static void setZone(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static void test_every_entry_matches_the_host_zoneinfo(void)
{
    if (access((std::string(ZONEINFO) + "/Europe/Prague").c_str(), R_OK) != 0)
    {
        TEST_IGNORE_MESSAGE("no zoneinfo on this host");
    }
    std::string host = hostTzdataVersion();
    if (!host.empty() && host < TimeZones::TZDATA_VERSION)
    {
        std::string message = "host tzdata " + host + " is older than the table's " + TimeZones::TZDATA_VERSION;
        TEST_IGNORE_MESSAGE(message.c_str());
    }
    std::vector<long> expected;
    for (size_t i = 0; i < COUNT; i++)
    {
        const char *name = ZONES[i].name;
        std::string where = std::string(name) + " (host tzdata " + (host.empty() ? "?" : host) + ")";
        if (strcmp(name, "UTC") != 0)
        {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, access((std::string(ZONEINFO) + "/" + name).c_str(), R_OK), where.c_str());
        }
        time_t from = checkStart(name);
        // Reference: hourly offsets, plus each transition to the second
        setZone((std::string(":") + name).c_str());
        expected.clear();
        std::vector<time_t> transitions;
        long previous = offsetAt(from);
        for (time_t t = from; t < CHECK_TO; t += 3600)
        {
            long offset = offsetAt(t);
            if (offset != previous)
            {
                time_t lo = t - 3600, hi = t; // offset changes in (lo, hi]
                while (hi - lo > 1)
                {
                    time_t mid = lo + (hi - lo) / 2;
                    (offsetAt(mid) == previous ? lo : hi) = mid;
                }
                transitions.push_back(hi);
                previous = offset;
            }
            expected.push_back(offset);
        }
        std::vector<long> atTransitions;
        for (time_t t : transitions)
        {
            atTransitions.push_back(offsetAt(t - 1));
            atTransitions.push_back(offsetAt(t));
        }
        // The table's POSIX string must give the same
        setZone(ZONES[i].posix);
        size_t hour = 0;
        for (time_t t = from; t < CHECK_TO; t += 3600, hour++)
        {
            if (offsetAt(t) != expected[hour])
            {
                char message[160];
                snprintf(message, sizeof(message), "%s: offset at %lld", where.c_str(), (long long)t);
                TEST_ASSERT_EQUAL_INT_MESSAGE(expected[hour], offsetAt(t), message);
            }
        }
        for (size_t k = 0; k < transitions.size(); k++)
        {
            char message[160];
            snprintf(message, sizeof(message), "%s: transition at %lld", where.c_str(), (long long)transitions[k]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(atTransitions[2 * k], offsetAt(transitions[k] - 1), message);
            TEST_ASSERT_EQUAL_INT_MESSAGE(atTransitions[2 * k + 1], offsetAt(transitions[k]), message);
        }
    }
}

// ---------------------------------------------------------------------------
// Stored settings -> zone (migration from the old POSIX-only setting)
// ---------------------------------------------------------------------------

static void test_resolve_prefers_a_known_name(void)
{
    // The name picks one of the zones sharing a rule
    TEST_ASSERT_EQUAL_STRING("Europe/Berlin", nameOf(TimeZones::resolve("Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3")));
    TEST_ASSERT_EQUAL_STRING("America/Toronto", nameOf(TimeZones::resolve("America/Toronto", "EST5EDT,M3.2.0,M11.1.0")));
    // An unknown string is an outdated rule of the name: the table's rule
    TEST_ASSERT_EQUAL_STRING("Europe/London", nameOf(TimeZones::resolve("Europe/London", "GMT0BST,M3.5.0/1,M10.4.0")));
    TEST_ASSERT_EQUAL_STRING("Asia/Tokyo", nameOf(TimeZones::resolve("Asia/Tokyo", "")));
}

static void test_resolve_follows_a_change_by_an_older_firmware(void)
{
    // 1.2.0 knows no names: it rewrites the string and leaves a stale name
    TEST_ASSERT_EQUAL_STRING("Europe/Lisbon", nameOf(TimeZones::resolve("Europe/Prague", "WET0WEST,M3.5.0/1,M10.5.0")));
    TEST_ASSERT_EQUAL_STRING("UTC", nameOf(TimeZones::resolve("Europe/Berlin", "GMT0")));
    TEST_ASSERT_EQUAL_STRING("Europe/Moscow", nameOf(TimeZones::resolve("Asia/Tokyo", "MSK-3")));
}

// 2.0.0 and newer store the POSIX string written with the name (namePosix)
static void test_resolve_corrected_rule_reaches_stored_settings(void)
{
    // Stored with the rule of an older table: the name wins, the caller takes
    // the table's current rule (also when the old string is another entry's)
    const char *oldVancouver = "PST8PDT,M3.2.0,M11.1.0"; // = America/Los_Angeles
    int index = TimeZones::resolve("America/Vancouver", oldVancouver, oldVancouver);
    TEST_ASSERT_EQUAL_STRING("America/Vancouver", nameOf(index));
    TEST_ASSERT_EQUAL_STRING("MST7", ZONES[index].posix);
    const char *oldChisinau = "EET-2EEST,M3.5.0,M10.5.0/3";
    index = TimeZones::resolve("Europe/Chisinau", oldChisinau, oldChisinau);
    TEST_ASSERT_EQUAL_STRING("Europe/Chisinau", nameOf(index));
    TEST_ASSERT_EQUAL_STRING("EET-2EEST,M3.5.0/3,M10.5.0/4", ZONES[index].posix);
    index = TimeZones::resolve("Africa/Casablanca", "<+01>-1", "<+01>-1");
    TEST_ASSERT_EQUAL_STRING("<+00>0", ZONES[index].posix);
}

static void test_resolve_shared_rule_keeps_the_name(void)
{
    const char *eet = "EET-2EEST,M3.5.0/3,M10.5.0/4";
    const char *cet = "CET-1CEST,M3.5.0,M10.5.0/3";
    TEST_ASSERT_EQUAL_STRING("Europe/Kyiv", nameOf(TimeZones::resolve("Europe/Kyiv", eet, eet)));
    TEST_ASSERT_EQUAL_STRING("Asia/Nicosia", nameOf(TimeZones::resolve("Asia/Nicosia", eet, eet)));
    TEST_ASSERT_EQUAL_STRING("Europe/Berlin", nameOf(TimeZones::resolve("Europe/Berlin", cet, cet)));
    // 1.2.0 saved its WiFi form with the same zone: the name stays
    TEST_ASSERT_EQUAL_STRING("America/Toronto", nameOf(TimeZones::resolve("America/Toronto", "EST5EDT,M3.2.0,M11.1.0",
                                                                          "EST5EDT,M3.2.0,M11.1.0")));
}

static void test_resolve_change_by_an_older_firmware_after_the_name_wins(void)
{
    // 1.2.0 (after a downgrade) rewrote only the POSIX string
    const char *cet = "CET-1CEST,M3.5.0,M10.5.0/3";
    TEST_ASSERT_EQUAL_STRING("Europe/Athens", nameOf(TimeZones::resolve("Europe/Berlin", "EET-2EEST,M3.5.0/3,M10.5.0/4", cet)));
    TEST_ASSERT_EQUAL_STRING("UTC", nameOf(TimeZones::resolve("Asia/Tokyo", "GMT0", "JST-9"))); // its form fell back to GMT
    TEST_ASSERT_EQUAL_STRING("Europe/Prague", nameOf(TimeZones::resolve("America/Vancouver", cet, "PST8PDT,M3.2.0,M11.1.0")));
    TEST_ASSERT_EQUAL_STRING("Europe/Lisbon", nameOf(TimeZones::resolve("Europe/Kyiv", "WET0WEST,M3.5.0/1,M10.5.0",
                                                                        "EET-2EEST,M3.5.0/3,M10.5.0/4")));
    // A string no entry has stays custom
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::resolve("Europe/Berlin", "XYZ-5", cet));
    // Its string is the name's current rule: nothing changed
    TEST_ASSERT_EQUAL_STRING("Europe/Berlin", nameOf(TimeZones::resolve("Europe/Berlin", cet, "CET-1CEST,M3.5.0,M10.4.0/3")));
}

static void test_resolve_maps_the_five_old_options(void)
{
    TEST_ASSERT_EQUAL_STRING("Europe/Prague", nameOf(TimeZones::resolve(nullptr, "CET-1CEST,M3.5.0,M10.5.0/3")));
    TEST_ASSERT_EQUAL_STRING("UTC", nameOf(TimeZones::resolve(nullptr, "GMT0")));
    TEST_ASSERT_EQUAL_STRING("Europe/Lisbon", nameOf(TimeZones::resolve(nullptr, "WET0WEST,M3.5.0/1,M10.5.0")));
    TEST_ASSERT_EQUAL_STRING("Europe/Athens", nameOf(TimeZones::resolve(nullptr, "EET-2EEST,M3.5.0/3,M10.5.0/4")));
    TEST_ASSERT_EQUAL_STRING("Europe/Moscow", nameOf(TimeZones::resolve("", "MSK-3")));
}

static void test_resolve_other_known_rule_takes_first_entry(void)
{
    TEST_ASSERT_EQUAL_STRING("Europe/London", nameOf(TimeZones::resolve(nullptr, "GMT0BST,M3.5.0/1,M10.5.0")));
    TEST_ASSERT_EQUAL_STRING("America/New_York", nameOf(TimeZones::resolve(nullptr, "EST5EDT,M3.2.0,M11.1.0")));
    TEST_ASSERT_EQUAL_STRING("UTC", nameOf(TimeZones::resolve("Mars/Olympus", "UTC0")));
}

static void test_resolve_keeps_unknown_strings_as_custom(void)
{
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::resolve(nullptr, "CET-1CEST"));
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::resolve("", "EST5EDT,M4.1.0,M10.5.0"));
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::resolve("Mars/Olympus", "<+01>-1<+02>,M3.5.0,M10.5.0/3"));
}

static void test_resolve_nothing_stored_is_the_default(void)
{
    TEST_ASSERT_EQUAL_STRING("Europe/Prague", nameOf(TimeZones::resolve(nullptr, nullptr)));
    TEST_ASSERT_EQUAL_STRING("Europe/Prague", nameOf(TimeZones::resolve("", "")));
    TEST_ASSERT_EQUAL_STRING("Europe/Prague", nameOf(TimeZones::resolve("Mars/Olympus", "")));
}

// ---------------------------------------------------------------------------
// Form values
// ---------------------------------------------------------------------------

static void test_form_takes_names_and_listed_rules(void)
{
    const char *cet = "CET-1CEST,M3.5.0,M10.5.0/3";
    TEST_ASSERT_EQUAL_STRING("Asia/Tokyo", nameOf(TimeZones::fromForm("Asia/Tokyo", cet)));
    // Older clients (integration tests, scripts) post the POSIX string
    TEST_ASSERT_EQUAL_STRING("Europe/London", nameOf(TimeZones::fromForm("GMT0BST,M3.5.0/1,M10.5.0", cet)));
    TEST_ASSERT_EQUAL_STRING("UTC", nameOf(TimeZones::fromForm("GMT0", cet)));
    // The stored string (also a custom one) or nothing keeps the zone
    TEST_ASSERT_EQUAL_INT(TimeZones::KEEP, TimeZones::fromForm(cet, cet));
    TEST_ASSERT_EQUAL_INT(TimeZones::KEEP, TimeZones::fromForm("XYZ-5", "XYZ-5"));
    TEST_ASSERT_EQUAL_INT(TimeZones::KEEP, TimeZones::fromForm("", "XYZ-5"));
    TEST_ASSERT_EQUAL_INT(TimeZones::KEEP, TimeZones::fromForm(nullptr, cet));
}

static void test_form_refuses_anything_else(void)
{
    const char *cet = "CET-1CEST,M3.5.0,M10.5.0/3";
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::fromForm("XYZ-5", cet)); // no new custom strings
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::fromForm("Europe/Atlantis", cet));
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::fromForm("Europe/Prague ", cet));
    TEST_ASSERT_EQUAL_INT(-1, TimeZones::fromForm("<script>", cet));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_names_are_unique_and_sorted);
    RUN_TEST(test_names_are_safe_in_html);
    RUN_TEST(test_whole_of_europe_is_listed);
    RUN_TEST(test_default_is_prague_with_eu_rules);
    RUN_TEST(test_find_rejects_unknown_and_null);
    RUN_TEST(test_every_rule_parses_with_its_standard_offset);
    RUN_TEST(test_known_offsets_and_dst);
    RUN_TEST(test_eu_switch_hours_in_prague);
    RUN_TEST(test_every_entry_matches_the_host_zoneinfo);
    RUN_TEST(test_resolve_prefers_a_known_name);
    RUN_TEST(test_resolve_follows_a_change_by_an_older_firmware);
    RUN_TEST(test_resolve_corrected_rule_reaches_stored_settings);
    RUN_TEST(test_resolve_shared_rule_keeps_the_name);
    RUN_TEST(test_resolve_change_by_an_older_firmware_after_the_name_wins);
    RUN_TEST(test_resolve_maps_the_five_old_options);
    RUN_TEST(test_resolve_other_known_rule_takes_first_entry);
    RUN_TEST(test_resolve_keeps_unknown_strings_as_custom);
    RUN_TEST(test_resolve_nothing_stored_is_the_default);
    RUN_TEST(test_form_takes_names_and_listed_rules);
    RUN_TEST(test_form_refuses_anything_else);
    return UNITY_END();
}
