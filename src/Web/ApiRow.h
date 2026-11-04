/**
 * expLORA Gateway
 *
 * ApiRow - one sensor of /api?format=json, the same keys for generation 1
 * and 2 (pure, native tests)
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
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

#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../Data/Quantity.h"

namespace ApiRow
{
struct Row
{
    const char *name = "";
    uint8_t deviceType = 0;    // generation 1 type, 0 for generation 2
    const char *typeName = ""; // CLIMA / CARBON / METEO / GEN2
    const char *id = "";       // 6 hex digits (serial number) or 8 (DevID)
    bool gen2 = false;
    uint16_t q = 0;      // quantities it provides: their keys are present
    uint16_t qValid = 0; // ... with a value (null otherwise, e.g. never reported)
    float v[Qty::COUNT] = {};
    long lastSeenS = -1; // seconds since the last report, -1 never
};

inline double rounded(float v, uint8_t decimals)
{
    const double p = decimals == 0 ? 1 : decimals == 1 ? 10 : decimals == 2 ? 100 : 1000;
    return std::round(v * p) / p;
}

// deviceType, typeName, serialNumber, name, ("gen":2), the quantities in
// Qty order (batteryVoltage and rssi last), lastSeen
inline void toJson(JsonObject o, const Row &r)
{
    o["deviceType"] = r.deviceType;
    o["typeName"] = r.typeName;
    o["serialNumber"] = r.id;
    o["name"] = r.name;
    if (r.gen2)
    {
        o["gen"] = 2;
    }
    for (int i = 0; i < Qty::COUNT; i++)
    {
        const Qty::Id q = (Qty::Id)i;
        if (!(r.q & Qty::mask(q)))
        {
            continue;
        }
        const Qty::Info &info = Qty::info(q);
        if (r.qValid & Qty::mask(q))
        {
            o[info.api] = rounded(r.v[q], info.apiDecimals);
        }
        else
        {
            o[info.api] = nullptr;
        }
    }
    o["lastSeen"] = r.lastSeenS;
}

// Columns of /api?format=csv after name,type,serialNumber,lastSeen
constexpr Qty::Id CSV_COLUMNS[] = {Qty::Temperature, Qty::Humidity, Qty::Pressure, Qty::Co2,
                                   Qty::Lux,         Qty::Battery,  Qty::Rssi};

// One CSV line (S: String or std::string): name,type,serialNumber,lastSeen,
// then CSV_COLUMNS, empty without a value; CRLF. The name is RFC 4180
// quoted, a leading = + - @ gets a ' (no spreadsheet formula)
template <class S> void toCsv(S &out, const Row &r)
{
    out += '"';
    if (r.name[0] && strchr("=+-@", r.name[0]))
    {
        out += '\'';
    }
    for (const char *c = r.name; *c; c++)
    {
        if (*c == '"')
        {
            out += '"';
        }
        out += *c;
    }
    char num[24];
    snprintf(num, sizeof(num), "\",%u,", (unsigned)r.deviceType);
    out += num;
    out += r.id;
    snprintf(num, sizeof(num), ",%ld", r.lastSeenS);
    out += num;
    const uint16_t valid = r.qValid & r.q;
    for (Qty::Id q : CSV_COLUMNS)
    {
        if (valid & Qty::mask(q))
        {
            snprintf(num, sizeof(num), ",%.*f", (int)Qty::info(q).apiDecimals, (double)r.v[q]);
            out += num;
        }
        else
        {
            out += ',';
        }
    }
    out += "\r\n";
}

// /api?sensor=ID: the index of that sensor's row, -1 none (R: rows with
// serialNumber and gen2). A gen 2 DevID and a gen 1 serial number are both
// hex and can be equal: gen 1 or 2 picks the generation; without it (0) the
// generation 1 sensor wins, so 1.x URLs keep their meaning
template <class R> long select(const R &rows, uint32_t id, int gen)
{
    long gen2 = -1;
    for (size_t i = 0; i < rows.size(); i++)
    {
        if (rows[i].serialNumber != id)
        {
            continue;
        }
        if (!rows[i].gen2 && gen != 2)
        {
            return (long)i;
        }
        if (rows[i].gen2 && gen != 1 && gen2 < 0)
        {
            gen2 = (long)i;
        }
    }
    return gen2;
}
} // namespace ApiRow
