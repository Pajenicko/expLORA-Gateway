/**
 * expLORA Gateway
 *
 * SensorTypeIds - the generation 1 sensor types this firmware supports
 * (pure, for the native tests; SensorTypes.h holds their table)
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

namespace SensorTypeIds
{
constexpr uint8_t CLIMA = 0x01;  // BME280: temperature, humidity, pressure
constexpr uint8_t CARBON = 0x02; // SCD40: temperature, humidity, CO2
constexpr uint8_t METEO = 0x03;  // weather station

// Types of the table in SensorTypes.h. 0x04 (VEML7700), 0x05 (SHT40) and
// 0x51 (DIY_TEMP) were removed in 2.0.0: a sensors.json entry of another
// type is dropped at load (SensorManager::parseSensorsLocked)
constexpr bool supportedType(uint8_t type)
{
    return type == CLIMA || type == CARBON || type == METEO;
}
} // namespace SensorTypeIds
