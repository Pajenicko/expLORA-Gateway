/**
 * expLORA Gateway
 *
 * Quantity - the measured quantities of generation 1 and 2 sensors (pure, for
 * the native tests): ids, their calibration field, how they are named in the
 * API, MQTT and Home Assistant
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

#include <cstdint>
#include "SensorCalibration.h"

namespace Qty
{
enum Id : uint8_t
{
    Temperature, // degC
    Humidity,    // %RH
    Pressure,    // hPa
    Co2,         // ppm
    Lux,         // lx
    WindSpeed,   // m/s
    WindDir,     // deg
    Rain,        // mm (last report)
    DailyRain,   // mm since midnight
    RainRate,    // mm/h
    Battery,     // V
    Rssi,        // dBm
    COUNT
};

constexpr uint16_t mask(Id q)
{
    return (uint16_t)(1u << q);
}

struct Info
{
    const char *api;     // /api JSON key
    uint8_t apiDecimals; // JSON / CSV rounding
    const char *mqtt;    // MQTT topic and HA unique_id suffix
    uint8_t mqttDecimals;
    const char *haClass; // Home Assistant device_class
    const char *unit;    // unit_of_measurement (UTF-8)
    int8_t haPrecision;  // suggested_display_precision, -1 none
    SensorCalibration::Field cal; // its correction (FIELDS = none)
};

inline const Info &info(Id q)
{
    using namespace SensorCalibration;
    static const Info table[COUNT] = {
        {"temperature", 2, "temperature", 2, "temperature", "\xC2\xB0" "C", 1, Field::Temperature},
        {"humidity", 2, "humidity", 2, "humidity", "%", 1, Field::Humidity},
        {"pressure", 2, "pressure", 2, "pressure", "hPa", 1, Field::Pressure},
        {"ppm", 0, "co2", 0, "carbon_dioxide", "ppm", -1, Field::Co2},
        {"lux", 1, "illuminance", 1, "illuminance", "lx", 1, Field::Lux},
        {"windSpeed", 1, "wind_speed", 1, "wind_speed", "m/s", 1, Field::WindSpeed},
        {"windDirection", 0, "wind_direction", 0, "wind_direction", "\xC2\xB0", -1, Field::WindDirection},
        {"rainAmount", 2, "rain_amount", 1, "precipitation", "mm", 1, Field::RainAmount},
        {"dailyRainTotal", 2, "daily_rain", 1, "precipitation", "mm", 1, Field::FIELDS},
        {"rainRate", 2, "rain_rate", 1, "precipitation_intensity", "mm/h", 1, Field::RainRate},
        {"batteryVoltage", 2, "battery", 2, "voltage", "V", 2, Field::FIELDS},
        {"rssi", 0, "rssi", 0, "signal_strength", "dBm", -1, Field::FIELDS},
    };
    return table[q];
}
} // namespace Qty
