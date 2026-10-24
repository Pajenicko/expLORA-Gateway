/**
 * expLORA Gateway
 *
 * Time zones offered in the settings: IANA name -> POSIX TZ string (what
 * newlib's tzset() understands, DST rules included). The name is what the
 * page shows and what is stored next to the POSIX string; the table's string
 * is used on load, so a corrected rule in a newer firmware reaches stored
 * settings. Settings from before the list (only a POSIX string) map to the
 * closest entry; an unknown string stays as a custom zone. Pure C++ (native
 * tests).
 *
 * The rules are frozen at build time: derived from the IANA tz database
 * (tzdata) version TZDATA_VERSION, checked entry by entry against the host's
 * zoneinfo by the native tests. A government changing its rules later needs
 * a firmware update.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstring>

namespace TimeZones
{

struct Zone
{
    const char *name;
    const char *posix;
};

// Shared rules (identical literals are merged, so every rule is stored once;
// the tables are inline: one copy for all translation units)
#define TZ_CET "CET-1CEST,M3.5.0,M10.5.0/3"
#define TZ_EET "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define TZ_WET "WET0WEST,M3.5.0/1,M10.5.0"
#define TZ_US(std, off, dst) std #off dst ",M3.2.0,M11.1.0"
#define TZ_AU_EAST "AEST-10AEDT,M10.1.0,M4.1.0/3"

// tzdata version the table was derived from (and checked against)
static constexpr const char *TZDATA_VERSION = "2026c";

// UTC first, then sorted by name (the page lists them in this order).
// Ireland uses the equivalent GMT/IST form (tzdata's negative DST would
// show summer time in winter). Rule changes taken from 2026c: British
// Columbia stays on UTC-7 from 2026-11-01, Morocco on +00 from 2026-09-20.
inline constexpr Zone ZONES[] = {
    {"UTC", "UTC0"},
    {"Africa/Accra", "GMT0"},
    {"Africa/Algiers", "CET-1"},
    {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Casablanca", "<+00>0"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Lagos", "WAT-1"},
    {"Africa/Nairobi", "EAT-3"},
    {"Africa/Tunis", "CET-1"},
    {"America/Anchorage", TZ_US("AKST", 9, "AKDT")},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"America/Bogota", "<-05>5"},
    {"America/Caracas", "<-04>4"},
    {"America/Chicago", TZ_US("CST", 6, "CDT")},
    {"America/Denver", TZ_US("MST", 7, "MDT")},
    {"America/Halifax", TZ_US("AST", 4, "ADT")},
    {"America/Lima", "<-05>5"},
    {"America/Los_Angeles", TZ_US("PST", 8, "PDT")},
    {"America/Mexico_City", "CST6"},
    {"America/New_York", TZ_US("EST", 5, "EDT")},
    {"America/Phoenix", "MST7"},
    {"America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"America/Sao_Paulo", "<-03>3"},
    {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Toronto", TZ_US("EST", 5, "EDT")},
    {"America/Vancouver", "MST7"},
    {"Asia/Almaty", "<+05>-5"},
    {"Asia/Baku", "<+04>-4"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Dhaka", "<+06>-6"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Kathmandu", "<+0545>-5:45"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Manila", "PST-8"},
    {"Asia/Nicosia", TZ_EET},
    {"Asia/Novosibirsk", "<+07>-7"},
    {"Asia/Riyadh", "<+03>-3"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Taipei", "CST-8"},
    {"Asia/Tashkent", "<+05>-5"},
    {"Asia/Tbilisi", "<+04>-4"},
    {"Asia/Tehran", "<+0330>-3:30"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Vladivostok", "<+10>-10"},
    {"Asia/Yekaterinburg", "<+05>-5"},
    {"Asia/Yerevan", "<+04>-4"},
    {"Atlantic/Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"Atlantic/Canary", TZ_WET},
    {"Atlantic/Reykjavik", "GMT0"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Darwin", "ACST-9:30"},
    {"Australia/Hobart", TZ_AU_EAST},
    {"Australia/Melbourne", TZ_AU_EAST},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Sydney", TZ_AU_EAST},
    {"Europe/Amsterdam", TZ_CET},
    {"Europe/Andorra", TZ_CET},
    {"Europe/Athens", TZ_EET},
    {"Europe/Belgrade", TZ_CET},
    {"Europe/Berlin", TZ_CET},
    {"Europe/Bratislava", TZ_CET},
    {"Europe/Brussels", TZ_CET},
    {"Europe/Bucharest", TZ_EET},
    {"Europe/Budapest", TZ_CET},
    {"Europe/Chisinau", TZ_EET},
    {"Europe/Copenhagen", TZ_CET},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Gibraltar", TZ_CET},
    {"Europe/Helsinki", TZ_EET},
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Kaliningrad", "EET-2"},
    {"Europe/Kyiv", TZ_EET},
    {"Europe/Lisbon", TZ_WET},
    {"Europe/Ljubljana", TZ_CET},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Luxembourg", TZ_CET},
    {"Europe/Madrid", TZ_CET},
    {"Europe/Malta", TZ_CET},
    {"Europe/Minsk", "<+03>-3"},
    {"Europe/Monaco", TZ_CET},
    {"Europe/Moscow", "MSK-3"},
    {"Europe/Oslo", TZ_CET},
    {"Europe/Paris", TZ_CET},
    {"Europe/Podgorica", TZ_CET},
    {"Europe/Prague", TZ_CET},
    {"Europe/Riga", TZ_EET},
    {"Europe/Rome", TZ_CET},
    {"Europe/Samara", "<+04>-4"},
    {"Europe/San_Marino", TZ_CET},
    {"Europe/Sarajevo", TZ_CET},
    {"Europe/Skopje", TZ_CET},
    {"Europe/Sofia", TZ_EET},
    {"Europe/Stockholm", TZ_CET},
    {"Europe/Tallinn", TZ_EET},
    {"Europe/Tirane", TZ_CET},
    {"Europe/Vaduz", TZ_CET},
    {"Europe/Vienna", TZ_CET},
    {"Europe/Vilnius", TZ_EET},
    {"Europe/Volgograd", "MSK-3"},
    {"Europe/Warsaw", TZ_CET},
    {"Europe/Zagreb", TZ_CET},
    {"Europe/Zurich", TZ_CET},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Honolulu", "HST10"},
};

static constexpr size_t COUNT = sizeof(ZONES) / sizeof(ZONES[0]);
static constexpr const char *DEFAULT_NAME = "Europe/Prague";

// The five POSIX strings of the old list (before 2.0.0) -> the entry that
// matches its label best; any other stored string takes the first entry
// with the same rule
inline constexpr Zone LEGACY[] = {
    {"Europe/Prague", TZ_CET},
    {"UTC", "GMT0"},
    {"Europe/Lisbon", TZ_WET},
    {"Europe/Athens", TZ_EET},
    {"Europe/Moscow", "MSK-3"},
};

#undef TZ_CET
#undef TZ_EET
#undef TZ_WET
#undef TZ_US
#undef TZ_AU_EAST

// Index of the zone with this name, -1 = none
inline int find(const char *name)
{
    for (size_t i = 0; name && i < COUNT; i++)
    {
        if (strcmp(ZONES[i].name, name) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

// Zone for a stored POSIX string without a name, -1 = custom
inline int findByPosix(const char *posix)
{
    if (!posix)
    {
        return -1;
    }
    for (const Zone &legacy : LEGACY)
    {
        if (strcmp(legacy.posix, posix) == 0)
        {
            return find(legacy.name);
        }
    }
    for (size_t i = 0; i < COUNT; i++)
    {
        if (strcmp(ZONES[i].posix, posix) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

// Stored settings -> zone; nothing stored = the default, -1 = keep the stored
// POSIX string as a custom zone. namePosix is the POSIX string that was
// stored together with the name (2.0.0 and newer write both, an older
// firmware rewrites only the POSIX string):
// - the stored string is the one written with the name: the name wins and
//   the caller takes the table's current string (a corrected rule reaches
//   stored settings, a zone sharing its rule with others keeps its name);
// - it differs: an older firmware (1.2.0 after a downgrade) changed the zone
//   after the name was written; its string wins (an entry, or custom);
// - no namePosix (settings of 2.0.0 before it existed): a string of another
//   entry is taken as a change by an older firmware, any other string is an
//   outdated rule of the name. Known case, accepted: America/Vancouver
//   saved by a pre-release 2.0.0 build with its old rule PST8PDT (= the
//   America/Los_Angeles entry) becomes America/Los_Angeles; such settings
//   were never shipped, and the offsets agree until 2026-11-01.
inline int resolve(const char *name, const char *posix, const char *namePosix = nullptr)
{
    int index = find(name);
    if (!posix || !*posix)
    {
        return index >= 0 ? index : find(DEFAULT_NAME);
    }
    if (index >= 0 && strcmp(ZONES[index].posix, posix) == 0)
    {
        return index;
    }
    if (index >= 0 && namePosix && *namePosix)
    {
        return strcmp(namePosix, posix) == 0 ? index : findByPosix(posix);
    }
    int byPosix = findByPosix(posix);
    return byPosix >= 0 ? byPosix : index;
}

// Value posted by a settings form: a name of the list, or (older clients) a
// POSIX string of the list; empty or the stored string keeps the zone
static constexpr int KEEP = -2;
inline int fromForm(const char *value, const char *storedPosix)
{
    if (!value || !*value || (storedPosix && strcmp(value, storedPosix) == 0))
    {
        return KEEP;
    }
    int index = find(value);
    return index >= 0 ? index : findByPosix(value);
}

} // namespace TimeZones
