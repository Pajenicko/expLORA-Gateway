/**
 * expLORA Gateway
 *
 * PartitionTable - the flash partition table at 0x8000 (pure, free of
 * Arduino for the native tests in test/test_partition_table) and the
 * decisions of the Lite layout migration (Update/LayoutMigration.h).
 *
 * Table format (ESP-IDF): 32-byte entries (magic AA 50, type, subtype,
 * offset, size, 16-byte label, flags), then the MD5 entry (EB EB, 14 x FF,
 * MD5 of all entries before it), then 0xFF up to MAX_LEN. A table is one
 * of the known layouts only if its MD5 entry is there and right, every byte
 * after it is 0xFF and its entries are exactly the layout's (count, order,
 * types, offsets, sizes, labels, flags).
 *
 * Lite layouts (8 MB flash; nvs / otadata / coredump / spiffs identical):
 *   v1 (firmware up to 2.0.0, partitions/huge_app_littlefs.csv): app0
 *      0x10000 and app1 0x200000, 1984K each; littlefs 0x550000 (unused)
 *   v2 (2.0.1, partitions/lite_8MB_v2.csv): app0 0x10000 and app1
 *      0x550000, 2752K each; 0x2C0000-0x3F0000 undeclared
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace PartitionTable
{
constexpr uint32_t OFFSET = 0x8000;  // CONFIG_PARTITION_TABLE_OFFSET
constexpr size_t MAX_LEN = 0xC00;    // ESP_PARTITION_TABLE_MAX_LEN
constexpr size_t SECTOR = 0x1000;    // the table's flash sector
constexpr size_t ENTRY_LEN = 32;
constexpr uint32_t FLASH_MIN = 0x800000; // v2 ends at 8 MB (v1 at 6 MB): needed to migrate

struct Entry
{
    uint8_t type, subtype;
    uint32_t offset, size;
    const char *label;
    uint32_t flags;
};

// Types / subtypes used here
constexpr uint8_t APP = 0x00, DATA = 0x01;
constexpr uint8_t OTA_0 = 0x10, OTA_1 = 0x11;
constexpr uint8_t D_OTA = 0x00, D_NVS = 0x02, D_COREDUMP = 0x03, D_SPIFFS = 0x82;

constexpr Entry LITE_V1[] = {
    {DATA, D_NVS, 0x9000, 0x5000, "nvs", 0},
    {DATA, D_OTA, 0xE000, 0x2000, "otadata", 0},
    {APP, OTA_0, 0x10000, 0x1F0000, "app0", 0},
    {APP, OTA_1, 0x200000, 0x1F0000, "app1", 0},
    {DATA, D_COREDUMP, 0x3F0000, 0x10000, "coredump", 0},
    {DATA, D_SPIFFS, 0x400000, 0x150000, "spiffs", 0},
    {DATA, D_SPIFFS, 0x550000, 0x0B0000, "littlefs", 0},
};
constexpr Entry LITE_V2[] = {
    {DATA, D_NVS, 0x9000, 0x5000, "nvs", 0},
    {DATA, D_OTA, 0xE000, 0x2000, "otadata", 0},
    {APP, OTA_0, 0x10000, 0x2B0000, "app0", 0},
    {DATA, D_COREDUMP, 0x3F0000, 0x10000, "coredump", 0},
    {DATA, D_SPIFFS, 0x400000, 0x150000, "spiffs", 0},
    {APP, OTA_1, 0x550000, 0x2B0000, "app1", 0},
};
constexpr Entry PRO_16MB[] = {
    {DATA, D_NVS, 0x9000, 0x5000, "nvs", 0},
    {DATA, D_OTA, 0xE000, 0x2000, "otadata", 0},
    {APP, OTA_0, 0x10000, 0x400000, "app0", 0},
    {APP, OTA_1, 0x410000, 0x400000, "app1", 0},
    {DATA, D_COREDUMP, 0x810000, 0x10000, "coredump", 0},
    {DATA, D_SPIFFS, 0x820000, 0x7E0000, "spiffs", 0},
};
constexpr size_t LITE_V1_N = sizeof(LITE_V1) / sizeof(LITE_V1[0]);
constexpr size_t LITE_V2_N = sizeof(LITE_V2) / sizeof(LITE_V2[0]);
constexpr size_t PRO_16MB_N = sizeof(PRO_16MB) / sizeof(PRO_16MB[0]);

// Error is 0 (a value-initialised result is never a known layout)
enum class Layout : uint8_t
{
    Damaged = 0, // no valid structure: bad magic, bad MD5, bytes after the end
    Unknown,     // a valid table that is none of the below (or one without an MD5 entry)
    LiteV1,
    LiteV2,
    Pro,
};

inline const char *layoutText(Layout l)
{
    switch (l)
    {
    case Layout::LiteV1:
        return "v1 (1984K slots)";
    case Layout::LiteV2:
        return "v2 (2752K slots)";
    case Layout::Pro:
        return "Pro (4096K slots)";
    case Layout::Unknown:
        return "unknown";
    default:
        return "damaged";
    }
}

// MD5 of data (the firmware passes the ROM's, the tests their own)
using Md5Fn = void (*)(const uint8_t *data, size_t len, uint8_t out[16]);

inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

inline bool entryIs(const uint8_t *e, const Entry &x)
{
    if (e[0] != 0xAA || e[1] != 0x50 || e[2] != x.type || e[3] != x.subtype || le32(e + 4) != x.offset ||
        le32(e + 8) != x.size || le32(e + 28) != x.flags)
    {
        return false;
    }
    // Label: the text, then NUL up to 16 bytes
    const size_t n = strlen(x.label);
    if (n > 16 || memcmp(e + 12, x.label, n) != 0)
    {
        return false;
    }
    for (size_t i = n; i < 16; i++)
    {
        if (e[12 + i] != 0)
        {
            return false;
        }
    }
    return true;
}

inline bool tableIs(const uint8_t *buf, size_t entries, const Entry *x, size_t n)
{
    if (entries != n)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        if (!entryIs(buf + i * ENTRY_LEN, x[i]))
        {
            return false;
        }
    }
    return true;
}

// buf: the table's first len bytes (len >= MAX_LEN; only MAX_LEN are read)
inline Layout classify(const uint8_t *buf, size_t len, Md5Fn md5)
{
    if (!buf || len < MAX_LEN || !md5)
    {
        return Layout::Damaged;
    }
    size_t entries = 0;
    size_t at = 0;
    bool haveMd5 = false;
    for (; at + ENTRY_LEN <= MAX_LEN; at += ENTRY_LEN)
    {
        const uint8_t *e = buf + at;
        if (e[0] == 0xAA && e[1] == 0x50)
        {
            entries++;
            continue;
        }
        if (e[0] == 0xEB && e[1] == 0xEB)
        {
            for (size_t i = 2; i < 16; i++)
            {
                if (e[i] != 0xFF)
                {
                    return Layout::Damaged;
                }
            }
            uint8_t sum[16];
            md5(buf, at, sum);
            if (memcmp(sum, e + 16, 16) != 0)
            {
                return Layout::Damaged;
            }
            haveMd5 = true;
            at += ENTRY_LEN;
            break;
        }
        bool blank = true;
        for (size_t i = 0; i < ENTRY_LEN; i++)
        {
            blank = blank && e[i] == 0xFF;
        }
        if (!blank)
        {
            return Layout::Damaged;
        }
        break; // the end without an MD5 entry
    }
    for (; at < MAX_LEN; at++)
    {
        if (buf[at] != 0xFF)
        {
            return Layout::Damaged;
        }
    }
    if (!haveMd5)
    {
        return Layout::Unknown; // never acted on: not proven intact
    }
    if (tableIs(buf, entries, LITE_V1, LITE_V1_N))
    {
        return Layout::LiteV1;
    }
    if (tableIs(buf, entries, LITE_V2, LITE_V2_N))
    {
        return Layout::LiteV2;
    }
    if (tableIs(buf, entries, PRO_16MB, PRO_16MB_N))
    {
        return Layout::Pro;
    }
    return Layout::Unknown;
}

// The table of a layout as gen_esp32part.py writes it: MAX_LEN bytes
// (entries, MD5 entry, 0xFF)
inline void build(const Entry *x, size_t n, Md5Fn md5, uint8_t *out)
{
    memset(out, 0xFF, MAX_LEN);
    for (size_t i = 0; i < n; i++)
    {
        uint8_t *e = out + i * ENTRY_LEN;
        e[0] = 0xAA;
        e[1] = 0x50;
        e[2] = x[i].type;
        e[3] = x[i].subtype;
        put32(e + 4, x[i].offset);
        put32(e + 8, x[i].size);
        memset(e + 12, 0, 16);
        memcpy(e + 12, x[i].label, strlen(x[i].label));
        put32(e + 28, x[i].flags);
    }
    uint8_t *m = out + n * ENTRY_LEN;
    m[0] = 0xEB;
    m[1] = 0xEB;
    md5(out, n * ENTRY_LEN, m + 16);
}

// --- Migration v1 -> v2 (Lite) ---------------------------------------------------------
//
// Runs at boot (setup(), before the network, the radio and the web server:
// no update session, no sensor job, no upload can write the flash then),
// only with a validated running image:
//   Copy:  running from v1 app1 -> the running image is copied into v1 app0
//          (whose space the new app0 starts with), verified, made the boot
//          partition (otadata: state NEW), restart. The bootloader starts
//          the copy as PENDING_VERIFY.
//   Accept: running the copy from app0 as PENDING_VERIFY, the marker says
//          a copy was made and app1 (the validated source) holds the same
//          image: marked valid at once (it is the image that was
//          validated), then Table in the same boot. A reset before that
//          makes the bootloader abort app0 and start app1 again, which
//          copies again (bounded).
//   Table: running from app0, the boot partition is app0: the v2 table is
//          written at 0x8000 (read back byte for byte, retried once, else
//          the v1 table is written back and kept), restart. otadata stays:
//          it selects app0 (ota_0 in both tables) - the validated state
//          and the install record carry over.
// The marker file (/update/layout.txt: "<stage> <attempts>" or
// "fail <version>") is written before each step; a step started MAX_TRIES
// times without success gives up for this firmware version (a newer
// firmware tries again). Without a marker that could be saved no step runs.
enum class Stage : uint8_t
{
    None = 0,
    Copy,
    Table,
    Failed, // gave up (for the version in the marker)
};

struct Marker
{
    Stage stage = Stage::None;
    uint8_t tries = 0;      // Copy / Table: steps started
    char version[16] = {}; // Failed: the firmware that gave up
};

constexpr uint8_t MAX_TRIES = 3;

// "copy 2", "table 1", "fail 2.0.1" (one line, optional '\n'). false: not a
// marker (then treated as none)
inline bool parseMarker(const char *s, size_t len, Marker &m)
{
    m = Marker();
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
    {
        len--;
    }
    auto starts = [&](const char *w) {
        const size_t n = strlen(w);
        return len > n && memcmp(s, w, n) == 0 && s[n] == ' ';
    };
    const char *rest;
    size_t restLen;
    if (starts("copy") || starts("table"))
    {
        const bool copy = s[0] == 'c';
        rest = s + (copy ? 5 : 6);
        restLen = len - (copy ? 5 : 6);
        if (restLen != 1 || rest[0] < '1' || rest[0] > '9')
        {
            return false;
        }
        m.stage = copy ? Stage::Copy : Stage::Table;
        m.tries = (uint8_t)(rest[0] - '0');
        return true;
    }
    if (starts("fail"))
    {
        rest = s + 5;
        restLen = len - 5;
        if (restLen == 0 || restLen >= sizeof(m.version))
        {
            return false;
        }
        for (size_t i = 0; i < restLen; i++)
        {
            if (!((rest[i] >= '0' && rest[i] <= '9') || rest[i] == '.'))
            {
                return false;
            }
        }
        m.stage = Stage::Failed;
        memcpy(m.version, rest, restLen);
        m.version[restLen] = 0;
        return true;
    }
    return false;
}

// The marker's text (cap >= 24)
inline size_t formatMarker(const Marker &m, char *out, size_t cap)
{
    if (cap < 24)
    {
        return 0;
    }
    switch (m.stage)
    {
    case Stage::Copy:
    case Stage::Table:
        if (m.tries < 1 || m.tries > 9)
        {
            return 0;
        }
        memcpy(out, m.stage == Stage::Copy ? "copy " : "table ", m.stage == Stage::Copy ? 5 : 6);
        {
            size_t n = m.stage == Stage::Copy ? 5 : 6;
            out[n++] = (char)('0' + m.tries);
            out[n++] = '\n';
            out[n] = 0;
            return n;
        }
    case Stage::Failed:
    {
        const size_t v = strnlen(m.version, sizeof(m.version) - 1);
        memcpy(out, "fail ", 5);
        memcpy(out + 5, m.version, v);
        out[5 + v] = '\n';
        out[6 + v] = 0;
        return 6 + v;
    }
    default:
        return 0;
    }
}

enum class Slot : uint8_t
{
    App0,  // the ota_0 partition (0x10000)
    App1,  // ota_1
    Other, // anything else (factory, test, not found)
};

// The running image's otadata state
enum class RunState : uint8_t
{
    Valid,   // VALID, or no rollback state (UNDEFINED / no otadata entry: blank otadata, older firmware)
    Pending, // PENDING_VERIFY: being validated
    Other,   // NEW / INVALID / ABORTED (never expected for the running image)
};

struct BootIn
{
    bool board = false;     // compiled for the Lite
    bool flashOk = false;   // physical and configured flash >= FLASH_MIN
    Layout layout = Layout::Damaged;
    Slot running = Slot::Other;
    RunState state = RunState::Other;
    bool bootIsRunning = false; // otadata selects the running partition
    Marker marker;
    bool markerThisVersion = false; // marker.stage == Failed for this firmware's version
    // The install record names an activated image in another slot: this
    // boot reports it (a rollback, or an activation that did not start) -
    // Copy / Table would overwrite or drop that slot before it is recorded
    bool otaRecordOther = false;
};

enum class Action : uint8_t
{
    None = 0,  // nothing (not a Lite, v2 without a marker, Pro)
    Note,      // nothing, with a note (unknown / damaged table, small flash, gave up, other slot)
    Finished,  // v2 with a marker: the migration finished - log once, remove the marker
    Wait,      // v1, running image pending (or otaRecordOther): migrate after its validation / the
               // record (restart from loop())
    Copy,
    Accept,    // mark the copy valid (if it equals app1), then Table
    Table,
    GiveUp,    // the step was started MAX_TRIES times: marker "fail <version>"
};

// The step for this boot (the caller re-reads its inputs after Accept and
// asks again). Stage counting: the caller writes the marker with
// tries + 1 (same stage) or 1 (new stage) before the step
inline Action decide(const BootIn &in)
{
    if (!in.board)
    {
        return Action::None;
    }
    if (in.layout == Layout::LiteV2)
    {
        return in.marker.stage == Stage::Copy || in.marker.stage == Stage::Table ? Action::Finished : Action::None;
    }
    if (in.layout != Layout::LiteV1 || !in.flashOk)
    {
        return Action::Note;
    }
    if (in.marker.stage == Stage::Failed && in.markerThisVersion)
    {
        return Action::Note;
    }
    if (in.state == RunState::Pending)
    {
        // The copy of a validated image: accepted at once (the marker says
        // a copy was made; the caller compares it with app1). Anything else
        // pending is validated first
        return in.running == Slot::App0 && in.marker.stage == Stage::Copy ? Action::Accept : Action::Wait;
    }
    if (in.state != RunState::Valid)
    {
        return Action::Note;
    }
    if (in.otaRecordOther && (in.running == Slot::App1 || (in.running == Slot::App0 && in.bootIsRunning)))
    {
        return Action::Wait;
    }
    if (in.running == Slot::App1)
    {
        return in.marker.stage == Stage::Copy && in.marker.tries >= MAX_TRIES ? Action::GiveUp : Action::Copy;
    }
    if (in.running == Slot::App0 && in.bootIsRunning)
    {
        return in.marker.stage == Stage::Table && in.marker.tries >= MAX_TRIES ? Action::GiveUp : Action::Table;
    }
    return Action::Note;
}

// The marker to write before a step (Copy / Table)
inline Marker nextMarker(const Marker &m, Stage step)
{
    Marker n;
    n.stage = step;
    n.tries = (uint8_t)(m.stage == step ? m.tries + 1 : 1);
    return n;
}
} // namespace PartitionTable
