/**
 * expLORA Gateway
 *
 * Sensor data structure header file
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
#include <ArduinoJson.h>
#include "SensorTypes.h"
#include "SensorHealth.h"
#include "SensorCalibration.h"

/**
 * Structure for storing sensor data
 *
 * This structure stores both configuration data and current measurement values
 * from sensors. Thanks to integration with SensorTypes, it allows flexible
 * handling of different sensor types and their specific data.
 */
struct SensorData
{
    // Basic sensor identification
    SensorType deviceType; // Device type
    uint32_t serialNumber; // Serial number
    uint32_t deviceKey;    // Key for data decryption
    String name;           // User-defined sensor name

    // Extended configuration
    String customUrl; // Complete URL with placeholders

    // Sensor status
    unsigned long lastSeen; // Time of last seen packet
    bool configured;        // Whether the sensor is configured

    // General sensor data - values are valid according to device type
    float temperature; // Temperature (°C) - BME280, SCD40
    float humidity;    // Humidity (%) - BME280, SCD40
    float pressure;    // Pressure (hPa) - BME280
    float ppm;         // CO2 concentration (ppm) - SCD40
    float lux;         // Light intensity (lux) - VEML7700

    // New variables for METEO sensor
    float windSpeed;             // Wind speed (m/s)
    uint16_t windDirection;      // Wind direction (degrees)
    float rainAmount;            // Rain amount (mm)
    float rainRate;              // Rain intensity (mm/h)
    float dailyRainTotal;        // Daily precipitation total (mm) - resets at midnight
    unsigned long lastRainReset; // Timestamp of last daily total reset

    // Corrections and the altitude for the pressure (SensorCalibration.h)
    SensorCalibration::Set cal;

    // General device data
    float batteryVoltage; // Battery voltage (V)
    int rssi;             // Signal strength (dBm)

    // Per-sensor health tracking (rejection counters, streaks, 24h window).
    // Updated by SensorManager::recordSensor{Success,Rejection}/tickSensorHealth.
    SensorHealth::Stats health;

    // Constructor for initialization with default values
    SensorData() : deviceType(SensorType::UNKNOWN),
                   serialNumber(0),
                   deviceKey(0),
                   name(""),
                   customUrl(""),
                   lastSeen(0),
                   configured(false),
                   temperature(0.0f),
                   humidity(0.0f),
                   pressure(0.0f),
                   ppm(0.0f),
                   lux(0.0f),
                   batteryVoltage(0.0f),
                   rssi(0),
                   windSpeed(0.0f),
                   windDirection(0),
                   rainAmount(0.0f),
                   rainRate(0.0f),
                   dailyRainTotal(0.0f),
                   lastRainReset(0)
    {
    }

    // Generation 2 readings (SensorManager::getReadings): serialNumber holds
    // the DevID, the quantities come from the sensor, not from a type
    bool gen2 = false;
    uint16_t gen2Q = 0;    // quantities it has reported (Gen2Sensor::qMask)
    uint16_t gen2QNow = 0; // ... with a value since this start (Gen2Sensor::qNow)
    uint16_t gen2QBad = 0; // ... whose last report was invalid (Gen2Sensor::qBad)
    uint16_t hwType = 0;   // HW type and firmware (Home Assistant model / sw_version)
    uint32_t fwVersion = 0;

    // Quantities it provides, and those with a current value
    uint16_t qMask() const { return gen2 ? gen2Q : getSensorTypeInfo(deviceType).q; }
    uint16_t qValid() const { return gen2 ? gen2QNow : lastSeen > 0 ? qMask() : 0; }
    bool has(Qty::Id q) const { return (qMask() & Qty::mask(q)) != 0; }
    bool hasTemperature() const { return has(Qty::Temperature); }
    bool hasHumidity() const { return has(Qty::Humidity); }
    bool hasPressure() const { return has(Qty::Pressure); }
    bool hasPPM() const { return has(Qty::Co2); }
    bool hasLux() const { return has(Qty::Lux); }
    bool hasWindSpeed() const { return has(Qty::WindSpeed); }
    bool hasWindDirection() const { return has(Qty::WindDir); }
    bool hasRainAmount() const { return has(Qty::Rain); }
    bool hasRainRate() const { return has(Qty::RainRate); }

    float value(Qty::Id q) const
    {
        switch (q)
        {
        case Qty::Temperature:
            return temperature;
        case Qty::Humidity:
            return humidity;
        case Qty::Pressure:
            return pressure;
        case Qty::Co2:
            return ppm;
        case Qty::Lux:
            return lux;
        case Qty::WindSpeed:
            return windSpeed;
        case Qty::WindDir:
            return windDirection;
        case Qty::Rain:
            return rainAmount;
        case Qty::DailyRain:
            return dailyRainTotal;
        case Qty::RainRate:
            return rainRate;
        case Qty::Battery:
            return batteryVoltage;
        default:
            return rssi;
        }
    }

    // Get information about sensor type
    const SensorTypeInfo &getTypeInfo() const
    {
        return getSensorTypeInfo(deviceType);
    }

    // Type name for the pages, the API and Home Assistant
    const char *typeName() const { return gen2 ? "GEN2" : getTypeInfo().name; }

    // The ID in the API and the MQTT topics: 6 hex digits of the serial
    // number, 8 of a generation 2 DevID
    String idString() const { return gen2 ? formatHex(serialNumber, 8) : formatSN(serialNumber); }
};

struct ActiveSensorEntry {
    size_t       index;   // real index in the sensors array
    SensorData   data;
};
