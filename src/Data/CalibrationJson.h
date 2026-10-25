/**
 * expLORA Gateway
 *
 * CalibrationJson - a sensor's corrections in sensors.json, the same keys
 * for generation 1 (every field written) and generation 2 (only the ones
 * that correct something)
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
#include "SensorCalibration.h"

namespace SensorCalibration
{
// Fields first..last-1 into o. Generation 1 (onlyCorrecting false): every
// field, as floats - the file format of earlier versions. Generation 2
// (onlyCorrecting): only the fields that correct something, rounded to
// 0.001 (no float noise like -0.300000012 in the file). Integer fields as
// integers
inline void writeJson(JsonObject o, const Set &s, bool onlyCorrecting, Field first = Temperature,
                      Field last = FIELDS)
{
    for (int i = first; i < last; i++)
    {
        const Field f = (Field)i;
        if (onlyCorrecting && s.isNone(f))
        {
            continue;
        }
        if (info(f).whole)
        {
            o[info(f).key] = (int)s[f];
        }
        else if (onlyCorrecting)
        {
            o[info(f).key] = std::round(s[f] * 1000.0) / 1000.0;
        }
        else
        {
            o[info(f).key] = s[f];
        }
    }
}

// The fields present in o (absent ones keep their value)
inline void readJson(JsonObjectConst o, Set &s)
{
    for (int i = 0; i < FIELDS; i++)
    {
        const Field f = (Field)i;
        JsonVariantConst v = o[info(f).key];
        if (!v.isNull())
        {
            const float x = v.as<float>();
            if (std::isfinite(x))
            {
                s[f] = info(f).whole ? (float)(int)x : x;
            }
        }
    }
}
} // namespace SensorCalibration
