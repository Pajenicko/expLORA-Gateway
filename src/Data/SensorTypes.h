/**
 * expLORA Gateway
 *
 * Sensor types definitions header file
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

#include <Arduino.h>
#include "../config.h"
#include "SensorTypeIds.h"
#include "Quantity.h"

/**
 * Definition of sensor types and their properties
 *
 * This file contains definitions for all supported sensor types including their
 * specific properties and capabilities. When adding a new sensor type, simply
 * define its properties here and add implementation for its processing.
 */

// Enumeration for sensor types - matching the definitions in config.h
enum class SensorType : uint8_t
{
    UNKNOWN = SENSOR_TYPE_UNKNOWN,
    BME280 = SENSOR_TYPE_BME280,       // Temperature, humidity, pressure
    SCD40 = SENSOR_TYPE_SCD40,         // Temperature, humidity, CO2
    METEO = SENSOR_TYPE_METEO,         // Meteorological station
    // Add more sensor types here...
};

// Structure for sensor type information
struct SensorTypeInfo
{
    SensorType type;            // Sensor type (enum)
    const char *name;           // Sensor type name
    uint8_t expectedDataLength; // Expected data length in packet (excluding header and checksum)
    uint8_t packetDataOffset;   // Offset where sensor data begins in the packet
    uint16_t q;                 // Quantities it provides (Qty::mask), battery and signal included
};

// Quantities of every generation 1 sensor, and of the types
constexpr uint16_t Q_ANY = Qty::mask(Qty::Battery) | Qty::mask(Qty::Rssi);
constexpr uint16_t Q_CLIMA = Q_ANY | Qty::mask(Qty::Temperature) | Qty::mask(Qty::Humidity) | Qty::mask(Qty::Pressure);
constexpr uint16_t Q_CARBON = Q_ANY | Qty::mask(Qty::Temperature) | Qty::mask(Qty::Humidity) | Qty::mask(Qty::Co2);
constexpr uint16_t Q_METEO = Q_CLIMA | Qty::mask(Qty::WindSpeed) | Qty::mask(Qty::WindDir) | Qty::mask(Qty::Rain) |
                             Qty::mask(Qty::DailyRain) | Qty::mask(Qty::RainRate);

// Table of sensor type definitions - for easy lookup and adding new types
const SensorTypeInfo SENSOR_TYPE_DEFINITIONS[] = {
    {SensorType::UNKNOWN, "Unknown", 0, 7, 0},
    {SensorType::BME280, "CLIMA", 6, 7, Q_CLIMA},
    {SensorType::SCD40, "CARBON", 6, 7, Q_CARBON},
    {SensorType::METEO, "METEO", 14, 7, Q_METEO},
};

static_assert(SENSOR_TYPE_BME280 == SensorTypeIds::CLIMA && SENSOR_TYPE_SCD40 == SensorTypeIds::CARBON &&
                  SENSOR_TYPE_METEO == SensorTypeIds::METEO,
              "SensorTypeIds.h: the supported types are the table's");

// Helper function to get information about sensor type by enum value
inline const SensorTypeInfo &getSensorTypeInfo(SensorType type)
{
    for (const auto &info : SENSOR_TYPE_DEFINITIONS)
    {
        if (info.type == type)
        {
            return info;
        }
    }
    return SENSOR_TYPE_DEFINITIONS[0]; // Returns UNKNOWN if type is not found
}

// Helper function to get information about sensor type by uint8_t value
inline const SensorTypeInfo &getSensorTypeInfo(uint8_t typeValue)
{
    return getSensorTypeInfo(static_cast<SensorType>(typeValue));
}

// Helper function to get enum SensorType from uint8_t value
inline SensorType sensorTypeFromValue(uint8_t value)
{
    return static_cast<SensorType>(value);
}

// Convert SensorType to string for debugging and logging
inline String sensorTypeToString(SensorType type)
{
    const SensorTypeInfo &info = getSensorTypeInfo(type);
    return String(info.name);
}