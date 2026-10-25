/**
 * expLORA Gateway
 *
 * Gen2Values - measured values in a generation 2 uplink (channel 0 TLVs) as
 * quantities, corrected with the sensor's calibration (pure, native tests)
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

#include <cmath>
#include "ExploraProtocol.h"
#include "../Data/Quantity.h"

namespace Gen2Values
{
// What a channel-0 TLV holds
enum class Decoded : uint8_t
{
    None,    // not a quantity this firmware maps (rain / wind come with a
             // generation 2 METEO), or a malformed one
    Value,   // a measured value
    Invalid, // a known quantity with the sensor's invalid-value sentinel
             // (sensor present, reading failed; spec 4: i16 0x8000, u16 0xFFFF)
};

// A channel-0 TLV as a quantity in its unit (value: only with Decoded::Value)
inline Decoded decode(const ep::Tlv &t, Qty::Id &q, float &value)
{
    if (t.len != 2)
    {
        return Decoded::None;
    }
    const uint16_t raw = ep::tlvU16(t);
    bool invalid = raw == ep::kInvalidU16;
    switch (t.type)
    {
    case ep::up::kTemperature:
        q = Qty::Temperature;
        value = (int16_t)raw / 100.0f;
        invalid = (int16_t)raw == ep::kInvalidI16; // 0xFFFF is -0.01 degC
        break;
    case ep::up::kHumidity:
        q = Qty::Humidity;
        value = raw / 100.0f;
        break;
    case ep::up::kPressure:
        q = Qty::Pressure;
        value = raw / 10.0f;
        break;
    case ep::up::kCo2:
        q = Qty::Co2;
        value = raw;
        break;
    case ep::up::kIlluminance:
        q = Qty::Lux; // round(2048 * log2(lux + 1))
        value = exp2f(raw / 2048.0f) - 1.0f;
        break;
    case ep::up::kBattery:
        q = Qty::Battery;
        value = raw / 1000.0f;
        break;
    default:
        return Decoded::None;
    }
    return invalid ? Decoded::Invalid : Decoded::Value;
}

// The corrected value (offset / multiplier of the quantity's field; the
// altitude conversion of the pressure: SensorCalibration::pressureAtAltitude)
inline float corrected(const SensorCalibration::Set &cal, Qty::Id q, float value)
{
    const SensorCalibration::Field f = Qty::info(q).cal;
    return f == SensorCalibration::FIELDS ? value : SensorCalibration::apply(cal, f, value);
}

// One channel-0 TLV applied to a sensor's readings (G: Gen2Sensor - value[],
// qNow, qMask, qBad, cal). A value is stored corrected and marked current;
// an invalid one clears the quantity's current bit (the pages, /api and MQTT
// show it as missing, never the last good value) and marks it bad (MQTT
// sends "None"); both count as a reported quantity (qMask)
template <class G> Decoded apply(G &g, const ep::Tlv &t)
{
    Qty::Id q;
    float value = 0;
    const Decoded d = decode(t, q, value);
    if (d == Decoded::None)
    {
        return d;
    }
    const uint16_t m = Qty::mask(q);
    g.qMask |= m;
    if (d == Decoded::Value)
    {
        g.value[q] = corrected(g.cal, q, value);
        g.qNow |= m;
        g.qBad &= (uint16_t)~m;
    }
    else
    {
        g.qNow &= (uint16_t)~m;
        g.qBad |= m;
    }
    return d;
}

// After the TLVs, only when this UP carried a valid pressure (the stored
// value is already converted otherwise): the corrected station pressure to
// sea level with this UP's (or the last valid) temperature, 15 degC when the
// temperature is not current (never reported, or invalid in this UP)
template <class G> void pressureToAltitude(G &g)
{
    const float altitude = g.cal[SensorCalibration::Altitude];
    if (altitude > 0 && (g.qNow & Qty::mask(Qty::Pressure)))
    {
        const float t = (g.qNow & Qty::mask(Qty::Temperature)) ? g.value[Qty::Temperature] : 15.0f;
        g.value[Qty::Pressure] = SensorCalibration::pressureAtAltitude(g.value[Qty::Pressure], (int)altitude, t);
    }
}
} // namespace Gen2Values
