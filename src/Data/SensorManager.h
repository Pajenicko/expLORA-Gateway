/**
 * expLORA Gateway
 *
 * Sensor manager header file
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
#include <mutex>
#include <vector>
#include "SensorData.h"
#include "Logging.h"

/**
 * Class for managing a collection of sensors
 *
 * Handles adding, modifying, and deleting sensors, searching for them,
 * and managing their data. Stores sensor configuration in the file system.
 */

class SensorManager
{
private:
    SensorData sensors[MAX_SENSORS]; // Array of sensors - limited to MAX_SENSORS
    size_t sensorCount;              // Current number of sensors
    mutable std::mutex sensorMutex;  // Mutex for safe multi-threaded access
    Logger &logger;                  // Reference to logger

    // Filename for storing sensor configuration
    const char *sensorsFile;

    // Daily rain totals changed since the last save (saved from process())
    bool rainDirty = false;
    unsigned long lastRainSave = 0;

    uint32_t snapshotSeq = 0; // under sensorMutex
    uint32_t writtenSeq = 0;  // under fileMutex
    std::mutex fileMutex;     // sensors.json writes (taken after the flash permit)
    bool snapshotLocked(String &json);
    bool saveAndUnlock(std::unique_lock<std::mutex> &lock);
    bool writeSnapshot(const String &json, uint32_t seq);

public:
    // Constructor
    SensorManager(Logger &log, const char *file = SENSORS_FILE);

    // Destructor
    ~SensorManager();

    // Sensor manager initialization
    bool init();

    // Add a new sensor; returns its index, -1 when full, -2 when the serial
    // number is already used
    int addSensor(SensorType deviceType, uint32_t serialNumber, uint32_t deviceKey, const String &name);

    // Copy of a configured sensor, taken under the lock; false if none
    bool getSensorCopy(int index, SensorData &out) const;

    // Deferred work (rain total saves), call from loop()
    void process(unsigned long now);

    // Find sensor by serial number
    int findSensorBySN(uint32_t serialNumber);

    // Update sensor data
    bool updateSensor(int index, const SensorData &data);

    // Update sensor data by type (overloaded method for easier use)
    bool updateSensorData(int index, float temperature, float humidity, float pressure,
                          float ppm, float lux, float batteryVoltage, int rssi,
                          float windSpeed = 0.0f, uint16_t windDirection = 0,
                          float rainAmount = 0.0f, float rainRate = 0.0f);

    // Update sensor configuration
    bool updateSensorConfig(int index, const String &name, SensorType deviceType,
                            uint32_t serialNumber, uint32_t deviceKey,
                            const String &customUrl, int altitude,
                            float tempCorr, float humCorr, float pressCorr, float ppmCorr, float luxCorr,
                            float windSpeedCorr, int windDirCorr, float rainAmountCorr, float rainRateCorr);

    // Delete sensor
    bool deleteSensor(int index);

    // Get number of sensors
    size_t getSensorCount() const;

    // Get sensor by index
    const SensorData *getSensor(int index) const;
    SensorData *getSensor(int index);

    // Get list of all sensors
    std::vector<SensorData> getAllSensors() const;

    // Get list of all active sensors (configured)
    std::vector<SensorData> getActiveSensors() const;

    // Get list of active sensors (configured) with real index
    std::vector<ActiveSensorEntry> getActiveSensorEntries() const;

    // Save sensor configuration to file
    bool saveSensors(); // caller must not hold sensorMutex

    // Load sensor configuration from file
    bool loadSensors();

    // Forward sensor data to HTTP (blocking - never call with the lock held)
    bool forwardSensorData(const SensorData &sensor);

    // Convert relative pressure to absolute pressure
    double relativeToAbsolutePressure(double p0_hpa, int altitude_m, double temp_c);

    // -----------------------------------------------------------------------
    // Per-sensor health tracking. Calls delegate to SensorHealth (header-only,
    // pure C++) which lives on each sensor's `health` member. `nowMillis` is
    // the value of millis() at the event; tests use deterministic values.
    //
    // Use recordSensorSuccess/recordSensorRejection from packet processing,
    // and tickSensorHealth from the main loop so the 24h ring buffer rotates
    // even during quiet periods.
    // -----------------------------------------------------------------------
    void recordSensorSuccess(int index, unsigned long nowMillis);
    void recordSensorRejection(int index, unsigned long nowMillis, const char *reason);
    void tickSensorHealth(unsigned long nowMillis);
};