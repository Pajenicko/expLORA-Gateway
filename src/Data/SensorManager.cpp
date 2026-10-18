/**
 * expLORA Gateway
 *
 * Sensor manager implementation file
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

#include "SensorManager.h"
#include "SensorCalibration.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "../Hardware/FlashPermit.h"
#include "../Protocol/Gen2Crypto.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <memory>
#include "../Network/Uplink.h"
#include "../Hardware/WatchdogPause.h"

// Constructor
SensorManager::SensorManager(Logger &log, const char *file)
    : sensorCount(0), logger(log), sensorsFile(file)
{
}

// Destructor
SensorManager::~SensorManager()
{
    // Nothing special needs to be released, all objects will be released automatically
}

// Sensor manager initialization
bool SensorManager::init()
{
    logger.info("Initializing sensor manager");
    return loadSensors();
}

// Constants for pressure conversion
#define G 9.80665   // gravitational acceleration [m/s^2]
#define M 0.0289644 // molar mass of air [kg/mol]
#define R 8.3144598 // universal gas constant [J/(mol·K)]
#define L 0.0065    // temperature gradient [K/m]

// Convert relative pressure to absolute pressure
double SensorManager::relativeToAbsolutePressure(double p_rel_hpa, int altitude_m, double temp_c)
{
    if (altitude_m == 0)
    {
        return p_rel_hpa;
    }
    double T = temp_c + 273.15;
    double exponent = (G * M) / (R * L);
    return p_rel_hpa / pow(1 - (L * altitude_m) / T, exponent);
}

// Add a new sensor
int SensorManager::addSensor(SensorType deviceType, uint32_t serialNumber, uint32_t deviceKey, const String &name)
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    // A second sensor with the same serial number would never receive data
    // (and silently replacing the key of the first one is worse)
    if (findSensorBySN(serialNumber) >= 0)
    {
        logger.warning("Failed to add sensor: SN " + formatSN(serialNumber) + " already exists");
        return -2;
    }

    // First free slot (deleted sensors leave free slots below sensorCount)
    int newIndex = -1;
    for (size_t i = 0; i < MAX_SENSORS; i++)
    {
        if (!sensors[i].configured)
        {
            newIndex = i;
            break;
        }
    }
    if (newIndex < 0)
    {
        logger.error("Failed to add sensor: maximum number of sensors reached");
        return -1;
    }
    if ((size_t)newIndex >= sensorCount)
    {
        sensorCount = newIndex + 1;
    }

    // Fresh state - a reused slot must not inherit the deleted sensor's
    // health counters, rain totals or corrections
    sensors[newIndex] = SensorData();
    sensors[newIndex].deviceType = deviceType;
    sensors[newIndex].serialNumber = serialNumber;
    sensors[newIndex].deviceKey = deviceKey;
    sensors[newIndex].name = name;
    sensors[newIndex].configured = true;

    // Not saved yet: the web form's updateSensorConfig() saves the complete
    // sensor once (no half-configured sensor on flash if that fails)
    logger.info("Added new sensor: " + name + " (SN: " + formatSN(serialNumber) + ")");
    return newIndex;
}

// Find sensor by serial number
int SensorManager::findSensorBySN(uint32_t serialNumber)
{
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured && sensors[i].serialNumber == serialNumber)
        {
            return i;
        }
    }
    return -1;
}

// Update sensor data
bool SensorManager::updateSensor(int index, const SensorData &data)
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to update non-existent sensor at index " + String(index));
        return false;
    }

    // Update data while preserving configuration
    sensors[index].deviceType = data.deviceType;
    sensors[index].temperature = data.temperature;
    sensors[index].humidity = data.humidity;
    sensors[index].pressure = data.pressure;
    sensors[index].ppm = data.ppm;
    sensors[index].lux = data.lux;
    sensors[index].batteryVoltage = data.batteryVoltage;
    sensors[index].rssi = data.rssi;
    sensors[index].lastSeen = millis();

    return true;
}

// Update sensor data by type
bool SensorManager::updateSensorData(int index, float temperature, float humidity, float pressure,
                                     float ppm, float lux, float batteryVoltage, int rssi,
                                     float windSpeed, uint16_t windDirection,
                                     float rainAmount, float rainRate)
{
    std::unique_lock<std::mutex> lock(sensorMutex);

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to update non-existent sensor at index " + String(index));
        return false;
    }

    // Store original values for logging
    float originalTemp = temperature;
    float originalHum = humidity;
    float originalPress = pressure;
    float originalPPM = ppm;
    float originalLux = lux;
    float originalWindSpeed = windSpeed;
    uint16_t originalWindDir = windDirection;
    float originalRainAmount = rainAmount;
    float originalRainRate = rainRate;

    // Apply corrections to input values before updating.
    // Delegated to SensorCalibration (covered by native unit tests).
    // Note: applyWindDirectionCorrection fixes a latent C++ integer-promotion
    // bug in the previous inline (uint16_t + int) % 360 path that wrapped
    // negative offsets to ~65k° instead of e.g. 350°.
    temperature = SensorCalibration::applyOffset(temperature, sensors[index].temperatureCorrection);
    humidity    = SensorCalibration::applyOffset(humidity,    sensors[index].humidityCorrection);
    pressure    = SensorCalibration::applyOffset(pressure,    sensors[index].pressureCorrection);
    ppm         = SensorCalibration::applyOffset(ppm,         sensors[index].ppmCorrection);
    lux         = SensorCalibration::applyOffset(lux,         sensors[index].luxCorrection);
    windSpeed   = SensorCalibration::applyMultiplier(windSpeed, sensors[index].windSpeedCorrection);
    windDirection = SensorCalibration::applyWindDirectionCorrection(
        windDirection, sensors[index].windDirectionCorrection);
    rainAmount  = SensorCalibration::applyMultiplier(rainAmount, sensors[index].rainAmountCorrection);
    rainRate    = SensorCalibration::applyMultiplier(rainRate,   sensors[index].rainRateCorrection);

    // Log if corrections were applied (only build string if debug enabled)
    if (logger.getLogLevel() >= LogLevel::DEBUG)
    {
        bool correctionsApplied = false;
        String correctionLog;
        correctionLog.reserve(128);
        correctionLog += "Corrections applied to ";
        correctionLog += sensors[index].name;
        correctionLog += ": ";

        if (sensors[index].hasTemperature() && sensors[index].temperatureCorrection != 0)
        {
            correctionLog += "Temp ";
            correctionLog += String(originalTemp, 2);
            correctionLog += "→";
            correctionLog += String(temperature, 2);
            correctionLog += "°C, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasHumidity() && sensors[index].humidityCorrection != 0)
        {
            correctionLog += "Hum ";
            correctionLog += String(originalHum, 2);
            correctionLog += "→";
            correctionLog += String(humidity, 2);
            correctionLog += "%, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasPressure() && sensors[index].pressureCorrection != 0)
        {
            correctionLog += "Press ";
            correctionLog += String(originalPress, 2);
            correctionLog += "→";
            correctionLog += String(pressure, 2);
            correctionLog += "hPa, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasPPM() && sensors[index].ppmCorrection != 0)
        {
            correctionLog += "CO2 ";
            correctionLog += String(originalPPM, 0);
            correctionLog += "→";
            correctionLog += String(ppm, 0);
            correctionLog += "ppm, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasLux() && sensors[index].luxCorrection != 0)
        {
            correctionLog += "Lux ";
            correctionLog += String(originalLux, 1);
            correctionLog += "→";
            correctionLog += String(lux, 1);
            correctionLog += "lx, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasWindSpeed() && sensors[index].windSpeedCorrection != 1.0f)
        {
            correctionLog += "Wind ";
            correctionLog += String(originalWindSpeed, 1);
            correctionLog += "→";
            correctionLog += String(windSpeed, 1);
            correctionLog += "m/s, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasWindDirection() && sensors[index].windDirectionCorrection != 0)
        {
            correctionLog += "Dir ";
            correctionLog += String(originalWindDir);
            correctionLog += "→";
            correctionLog += String(windDirection);
            correctionLog += "°, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasRainAmount() && sensors[index].rainAmountCorrection != 1.0f)
        {
            correctionLog += "Rain ";
            correctionLog += String(originalRainAmount, 1);
            correctionLog += "→";
            correctionLog += String(rainAmount, 1);
            correctionLog += "mm, ";
            correctionsApplied = true;
        }

        if (sensors[index].hasRainRate() && sensors[index].rainRateCorrection != 1.0f)
        {
            correctionLog += "Rate ";
            correctionLog += String(originalRainRate, 1);
            correctionLog += "→";
            correctionLog += String(rainRate, 1);
            correctionLog += "mm/h, ";
            correctionsApplied = true;
        }

        if (correctionsApplied)
        {
            // Remove trailing ", " if present
            if (correctionLog.endsWith(", "))
            {
                correctionLog.remove(correctionLog.length() - 2);
            }
            logger.debug(correctionLog);
        }
    }

    // Adjust pressure for altitude
    // Only adjust if the sensor has pressure capability and altitude is set
    if (sensors[index].hasPressure() && sensors[index].altitude > 0)
    {
        float adjustedPressure = relativeToAbsolutePressure(pressure, sensors[index].altitude, temperature);
        logger.debug("Adjusted pressure from " + String(pressure, 2) + " hPa to " +
                     String(adjustedPressure, 2) + " hPa at altitude " +
                     String(sensors[index].altitude) + " m");
        pressure = adjustedPressure;
    }

    // Update only relevant values according to sensor type
    if (sensors[index].hasTemperature())
    {
        sensors[index].temperature = temperature;
    }

    if (sensors[index].hasHumidity())
    {
        sensors[index].humidity = humidity;
    }

    if (sensors[index].hasPressure())
    {
        sensors[index].pressure = pressure;
    }

    if (sensors[index].hasPPM())
    {
        sensors[index].ppm = ppm;
    }

    if (sensors[index].hasLux())
    {
        sensors[index].lux = lux;
    }

    // Meteorological data
    if (sensors[index].hasWindSpeed())
    {
        sensors[index].windSpeed = windSpeed;
    }

    if (sensors[index].hasWindDirection())
    {
        sensors[index].windDirection = windDirection;
    }

    if (sensors[index].hasRainAmount())
    {
        sensors[index].rainAmount = rainAmount;

        // Check if we need to reset daily total (new day)
        if (Logger::isTimeInitialized())
        { // Using method from Logger
            // Get current time
            struct tm timeinfo;
            time_t now;
            time(&now);

            if (getLocalTime(&timeinfo, 0))
            {
                // Create timestamps for comparing dates
                // Date of last reset
                struct tm lastResetTime;
                // time_t is 64-bit, the stored field 32-bit: convert, never cast the pointer
                time_t lastReset = (time_t)sensors[index].lastRainReset;
                localtime_r(&lastReset, &lastResetTime);

                // If last reset was on a different day than today, reset the counter
                if (sensors[index].lastRainReset == 0 ||
                    lastResetTime.tm_mday != timeinfo.tm_mday ||
                    lastResetTime.tm_mon != timeinfo.tm_mon ||
                    lastResetTime.tm_year != timeinfo.tm_year)
                {

                    logger.info("Resetting daily rain total for sensor: " + sensors[index].name);
                    sensors[index].dailyRainTotal = 0.0f;
                    sensors[index].lastRainReset = now;
                }
            }
        }

        // Add current rain amount to daily total
        sensors[index].dailyRainTotal += rainAmount;

        // Log the updated daily total if it has changed
        if (rainAmount > 0)
        {
            rainDirty = true; // saved from process(), not on every packet
        }
    }

    if (sensors[index].hasRainRate())
    {
        sensors[index].rainRate = rainRate;
    }

    // Always update general data
    sensors[index].batteryVoltage = batteryVoltage;
    sensors[index].rssi = rssi;
    sensors[index].lastSeen = millis();

    if (sensors[index].customUrl.length() == 0)
    {
        return true;
    }
    if (!uplink->isOnline())
    {
        logger.debug("Not forwarding data - offline");
        return true;
    }
    // The HTTP request blocks for up to seconds; web handlers wait on the lock
    SensorData copy = sensors[index];
    lock.unlock();
    forwardSensorData(copy);
    return true;
}

bool SensorManager::forwardSensorData(const SensorData &sensor)
{
    // Check if the sensor has a custom endpoint configured
    if (sensor.customUrl.length() == 0)
    {
        // No endpoint configured, nothing to do
        return true;
    }

    // DNS has no timeout of its own and a TLS handshake can be slow; the
    // loop task stays off the watchdog for this request
    WatchdogPause pause;

    // Clients first: ~HTTPClient() may still touch its client (stop()), so
    // they must outlive it
    WiFiClient plainClient;
    std::unique_ptr<WiFiClientSecure> secureClient; // only for HTTPS
    HTTPClient http;
    http.setTimeout(3000);
    http.setConnectTimeout(3000);
    http.setReuse(false);

    // Format the custom URL by replacing placeholders
    String url = sensor.customUrl;

    // Skip if no URL is configured
    if (url.length() == 0)
    {
        return true;
    }

    // Check if HTTP or HTTPS
    bool isHttps = url.startsWith("https://");

    // Process the URL - replace placeholders with actual values
    if (sensor.hasTemperature())
    {
        url.replace("*TEMP*", String(sensor.temperature, 2));
    }
    if (sensor.hasHumidity())
    {
        url.replace("*HUM*", String(sensor.humidity, 2));
    }
    if (sensor.hasPressure())
    {
        url.replace("*PRESS*", String(sensor.pressure, 2));
    }
    if (sensor.hasPPM())
    {
        url.replace("*PPM*", String(sensor.ppm, 0));
    }
    if (sensor.hasLux())
    {
        url.replace("*LUX*", String(sensor.lux, 1));
    }

    // Added placeholders for METEO data
    if (sensor.hasWindSpeed())
    {
        url.replace("*WIND_SPEED*", String(sensor.windSpeed, 1));
    }
    if (sensor.hasWindDirection())
    {
        url.replace("*WIND_DIR*", String(sensor.windDirection));
    }
    if (sensor.hasRainAmount())
    {
        url.replace("*RAIN*", String(sensor.rainAmount, 1));
        url.replace("*DAILY_RAIN*", String(sensor.dailyRainTotal, 1)); // New placeholder
    }
    if (sensor.hasRainRate())
    {
        url.replace("*RAIN_RATE*", String(sensor.rainRate, 1));
    }

    // Replace other common placeholders
    url.replace("*BAT*", String(sensor.batteryVoltage, 2));
    url.replace("*RSSI*", String(sensor.rssi));
    url.replace("*SN*", formatSN(sensor.serialNumber));
    url.replace("*TYPE*", String(static_cast<uint8_t>(sensor.deviceType)));

    // Without the query: it may carry tokens (logs are shown on the web page)
    String urlForLog = url.indexOf('?') >= 0 ? url.substring(0, url.indexOf('?')) : url;
    logger.debug("Forwarding data for sensor " + sensor.name + " to " + urlForLog);

    // For HTTPS, use a secure client, but ensure we delete it after the request
    if (isHttps)
    {
        // A TLS session needs ~40 kB (Pro: next to an MQTT TLS session)
        if (ESP.getFreeHeap() < 48000 || ESP.getMaxAllocHeap() < 24000)
        {
            logger.warning("Not forwarding to " + urlForLog + " - not enough memory for HTTPS");
            return false;
        }
        secureClient.reset(new WiFiClientSecure());
        secureClient->setInsecure(); // custom endpoints may use self-signed certificates
        secureClient->setHandshakeTimeout(5);
        http.begin(*secureClient, url);
    }
    else
    {
        http.begin(plainClient, url);
    }

    // Send the request
    int httpCode = http.GET();

    // Check result
    if (httpCode > 0)
    {
        logger.debug("HTTP request sent, response code: " + String(httpCode));
        // The body is not read: only the status matters, and an endpoint
        // could send an unbounded (chunked) reply
    }
    else
    {
        logger.warning("HTTP request failed: " + http.errorToString(httpCode));
    }

    http.end();

    return (httpCode == HTTP_CODE_OK);
}

// Update sensor configuration
bool SensorManager::updateSensorConfig(int index, const String &name, SensorType deviceType,
                                       uint32_t serialNumber, uint32_t deviceKey,
                                       const String &customUrl, int altitude,
                                       float tempCorr, float humCorr, float pressCorr,
                                       float ppmCorr, float luxCorr,
                                       float windSpeedCorr, int windDirCorr,
                                       float rainAmountCorr, float rainRateCorr)
{
    std::unique_lock<std::mutex> lock(sensorMutex);

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to update non-existent sensor at index " + String(index));
        return false;
    }

    // Check if serial number is already used by another sensor
    int existingIndex = findSensorBySN(serialNumber);
    if (existingIndex >= 0 && existingIndex != index)
    {
        logger.warning("Cannot update sensor config: Serial number " +
                       formatSN(serialNumber) + " already used by sensor " +
                       sensors[existingIndex].name);
        return false;
    }

    // Another sensor now (type or serial number changed): its readings, rain
    // totals and health counters do not belong to it
    if (sensors[index].deviceType != deviceType || sensors[index].serialNumber != serialNumber)
    {
        SensorData fresh;
        fresh.configured = true;
        sensors[index] = fresh;
    }

    // Update basic configuration
    sensors[index].name = name;
    sensors[index].deviceType = deviceType;
    sensors[index].serialNumber = serialNumber;
    sensors[index].deviceKey = deviceKey;
    sensors[index].customUrl = customUrl;
    sensors[index].altitude = altitude;

    // Update correction values
    sensors[index].temperatureCorrection = tempCorr;
    sensors[index].humidityCorrection = humCorr;
    sensors[index].pressureCorrection = pressCorr;
    sensors[index].ppmCorrection = ppmCorr;
    sensors[index].luxCorrection = luxCorr;
    sensors[index].windSpeedCorrection = windSpeedCorr;
    sensors[index].windDirectionCorrection = windDirCorr;
    sensors[index].rainAmountCorrection = rainAmountCorr;
    sensors[index].rainRateCorrection = rainRateCorr;

    logger.info("Updated configuration for sensor: " + name + " (SN: " + formatSN(serialNumber) + ")");
    return saveAndUnlock(lock);
}

// Delete sensor
bool SensorManager::deleteSensor(int index)
{
    std::unique_lock<std::mutex> lock(sensorMutex);

    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        logger.warning("Attempt to delete non-existent sensor at index " + String(index));
        return false;
    }

    String name = sensors[index].name;
    uint32_t serialNumber = sensors[index].serialNumber;

    // Mark as unconfigured instead of physically removing
    sensors[index].configured = false;

    logger.info("Deleted sensor: " + name + " (SN: " + formatSN(serialNumber) + ")");
    return saveAndUnlock(lock);
}

// Get number of sensors
size_t SensorManager::getSensorCount() const
{
    return sensorCount;
}

// Get sensor by index (const version)
const SensorData *SensorManager::getSensor(int index) const
{
    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        return nullptr;
    }
    return &sensors[index];
}

// Get sensor by index (non-const version)
SensorData *SensorManager::getSensor(int index)
{
    if (index < 0 || index >= sensorCount || !sensors[index].configured)
    {
        return nullptr;
    }
    return &sensors[index];
}

// Get list of all sensors
std::vector<SensorData> SensorManager::getAllSensors() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    std::vector<SensorData> result;
    result.reserve(sensorCount);
    for (size_t i = 0; i < sensorCount; i++)
    {
        result.push_back(sensors[i]);
    }
    return result;
}

std::vector<ActiveSensorEntry> SensorManager::getActiveSensorEntries() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    std::vector<ActiveSensorEntry> result;
    result.reserve(sensorCount);
    for (size_t i = 0; i < sensorCount; ++i)
    {
        if (sensors[i].configured)
        {
            result.push_back({ i, sensors[i] });
        }
    }
    return result;
}

// Get list of all active sensors (configured)
std::vector<SensorData> SensorManager::getActiveSensors() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);

    std::vector<SensorData> result;
    result.reserve(sensorCount);
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured)
        {
            result.push_back(sensors[i]);
        }
    }
    return result;
}

bool SensorManager::getSensorCopy(int index, SensorData &out) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || index >= (int)sensorCount || !sensors[index].configured)
    {
        return false;
    }
    out = sensors[index];
    return true;
}

// Rain totals: at most one flash write per interval
static constexpr unsigned long RAIN_SAVE_INTERVAL = 600000;

void SensorManager::process(unsigned long now)
{
    std::unique_lock<std::mutex> lock(sensorMutex);
    if (rainDirty && now - lastRainSave > RAIN_SAVE_INTERVAL)
    {
        // Next attempt after a full interval even when this one fails
        lastRainSave = now;
        saveAndUnlock(lock);
    }
}

// JSON members per stored sensor (see saveSensorsLocked)
static constexpr size_t SENSOR_JSON_FIELDS = 20;

// Save sensor configuration to file (caller must not hold sensorMutex)
bool SensorManager::saveSensors()
{
    std::unique_lock<std::mutex> lock(sensorMutex);
    return saveAndUnlock(lock);
}

// Snapshot under the held lock, release it, then write: the flash write
// (and the wait for the flash write permit, Hardware/FlashPermit.h) never
// blocks other users of sensorMutex, and the permit is never requested
// while sensorMutex is held
bool SensorManager::saveAndUnlock(std::unique_lock<std::mutex> &lock)
{
    // Always returns with the lock released
    String json;
    bool hasRain = rainDirty;
    if (!snapshotLocked(json))
    {
        lock.unlock();
        return false;
    }
    uint32_t seq = ++snapshotSeq;
    rainDirty = false;
    lock.unlock();

    bool ok = writeSnapshot(json, seq);
    if (!ok && hasRain)
    {
        std::lock_guard<std::mutex> relock(sensorMutex);
        rainDirty = true; // retried after the next interval
    }
    return ok;
}

// Caller holds sensorMutex: serialize the configured sensors
bool SensorManager::snapshotLocked(String &json)
{
    size_t textSize = 0;
    for (size_t i = 0; i < sensorCount; i++)
    {
        textSize += sensors[i].name.length() + sensors[i].customUrl.length() + 2;
    }
    for (size_t i = 0; i < gen2Count; i++)
    {
        textSize += gen2[i].name.length() + 32; // + DevID and label hex strings
    }
    // Sized for the sensors present (one smaller block on a fragmented heap)
    DynamicJsonDocument doc(JSON_OBJECT_SIZE(2) + JSON_ARRAY_SIZE(sensorCount) +
                            sensorCount * JSON_OBJECT_SIZE(SENSOR_JSON_FIELDS) + JSON_ARRAY_SIZE(gen2Count) +
                            gen2Count * JSON_OBJECT_SIZE(4) + textSize + 256);
    if (doc.capacity() == 0)
    {
        logger.error("Not enough memory to save the sensor configuration");
        return false;
    }
    JsonArray sensorArray = doc.createNestedArray("sensors");

    // Add sensors to JSON
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured)
        {
            JsonObject sensor = sensorArray.createNestedObject();
            sensor["deviceType"] = static_cast<uint8_t>(sensors[i].deviceType);
            sensor["serialNumber"] = sensors[i].serialNumber;
            sensor["deviceKey"] = sensors[i].deviceKey;
            sensor["name"] = sensors[i].name;
            sensor["customUrl"] = sensors[i].customUrl;
            sensor["altitude"] = sensors[i].altitude;

            // Also save daily rain total and time of last reset
            if (sensors[i].hasRainAmount())
            {
                sensor["dailyRainTotal"] = sensors[i].dailyRainTotal;
                sensor["lastRainReset"] = sensors[i].lastRainReset;
            }

            sensor["temperatureCorrection"] = sensors[i].temperatureCorrection;
            sensor["humidityCorrection"] = sensors[i].humidityCorrection;
            sensor["pressureCorrection"] = sensors[i].pressureCorrection;
            sensor["ppmCorrection"] = sensors[i].ppmCorrection;
            sensor["luxCorrection"] = sensors[i].luxCorrection;
            sensor["windSpeedCorrection"] = sensors[i].windSpeedCorrection;
            sensor["windDirectionCorrection"] = sensors[i].windDirectionCorrection;
            sensor["rainAmountCorrection"] = sensors[i].rainAmountCorrection;
            sensor["rainRateCorrection"] = sensors[i].rainRateCorrection;
        }
    }
    // Generation 2 sensors in their own array: firmware without protocol v2
    // ignores it, so a downgrade keeps the generation 1 sensors working
    if (gen2Count > 0)
    {
        JsonArray gen2Array = doc.createNestedArray("gen2");
        for (size_t i = 0; i < gen2Count; i++)
        {
            const Gen2Sensor &g = gen2[i];
            char devId[9], label[17];
            snprintf(devId, sizeof(devId), "%08lX", (unsigned long)g.devId);
            for (size_t b = 0; b < ep::kLabelKeyLen; b++)
            {
                snprintf(label + 2 * b, 3, "%02X", g.label[b]);
            }
            JsonObject obj = gen2Array.createNestedObject();
            obj["devId"] = devId; // copied (char array)
            obj["name"] = g.name;
            obj["label"] = label;
            if (g.hasDevNonce)
            {
                obj["devNonce"] = g.lastDevNonce;
            }
            ep::wipe(label, sizeof(label));
        }
    }
    if (doc.overflowed())
    {
        logger.error("Sensor configuration does not fit the JSON document - not saved");
        return false;
    }
    if (serializeJson(doc, json) == 0)
    {
        logger.error("Not enough memory to serialize the sensor configuration");
        return false;
    }
    return true;
}

// Write a snapshot without sensorMutex. Writers are serialized by the flash
// write permit; a snapshot older than the last one written is skipped
bool SensorManager::writeSnapshot(const String &json, uint32_t seq)
{
    FlashWriteGuard flashGuard;
    // Writers that timed out on the permit run concurrently: the file itself
    // (tmp file, remove, rename, writtenSeq) is still written by one at a time
    std::lock_guard<std::mutex> fileLock(fileMutex);
    if ((int32_t)(seq - writtenSeq) <= 0)
    {
        return true; // a newer snapshot is already on flash
    }
    String tmpFile = String(sensorsFile) + ".tmp";
    File file = LittleFS.open(tmpFile, "w");
    if (!file)
    {
        logger.error("Failed to open " + tmpFile + " for writing");
        return false;
    }
    size_t bytesWritten = file.print(json);
    file.close();
    if (json.length() == 0 || bytesWritten != json.length())
    {
        logger.error("Failed to write sensor configuration");
        LittleFS.remove(tmpFile);
        return false;
    }
    // The current file becomes the last good copy, then the new one replaces it
    if (LittleFS.exists(sensorsFile))
    {
        LittleFS.remove(SENSORS_BACKUP_FILE);
        if (!LittleFS.rename(sensorsFile, SENSORS_BACKUP_FILE))
        {
            LittleFS.remove(sensorsFile);
        }
    }
    if (!LittleFS.rename(tmpFile, sensorsFile))
    {
        logger.error("Failed to replace " + String(sensorsFile));
        return false;
    }

    writtenSeq = seq;
    logger.info("Saved the sensor configuration to " + String(sensorsFile));
    return true;
}

// Load sensor configuration from file
bool SensorManager::loadSensors()
{
    FlashWriteGuard flashGuard; // lock order: flash permit before sensorMutex
    std::lock_guard<std::mutex> lock(sensorMutex);

    clearLocked();

    // Power lost between removing the old file and renaming the new one
    String tmpFile = String(sensorsFile) + ".tmp";
    if (!LittleFS.exists(sensorsFile) && LittleFS.exists(tmpFile))
    {
        logger.warning("Recovering sensor configuration from " + tmpFile);
        LittleFS.rename(tmpFile, sensorsFile);
    }

    String content;
    int primary = readSensorsFile(sensorsFile, content);
    bool present = primary > 0;
    bool ok = present && parseSensorsLocked(content);
    if (primary < 0 || (present && !ok))
    {
        // Unreadable or unparsable: move it aside first (kept for recovery),
        // so restoring and saving below never overwrite the good backup with it
        String corruptFile = String(sensorsFile) + ".corrupt";
        LittleFS.remove(corruptFile);
        if (!LittleFS.rename(sensorsFile, corruptFile))
        {
            // Never let the broken file rotate over sensors.bak in the restore
            LittleFS.remove(sensorsFile);
        }
        logger.error(String(primary < 0 ? "Failed to read " : "Failed to parse ") + String(sensorsFile) +
                     ", moved to " + corruptFile);
    }
    if (!ok)
    {
        // Last good copy (written before every replacement of sensors.json)
        clearLocked();
        String backup;
        if (readSensorsFile(SENSORS_BACKUP_FILE, backup) > 0 && parseSensorsLocked(backup))
        {
            // A restore undoes exactly one save. Save the restored state as the
            // current file at once (the same backup is never applied twice) and
            // drop the broken copy, so keys of deleted sensors do not linger
            logger.warning("Sensor configuration restored from " + String(SENSORS_BACKUP_FILE) +
                           " (the last change before the damage is lost)");
            ok = true;
            String json;
            if (snapshotLocked(json) && writeSnapshot(json, ++snapshotSeq))
            {
                LittleFS.remove(String(sensorsFile) + ".corrupt");
            }
        }
        else
        {
            clearLocked();
        }
    }
    if (!ok && primary == 0)
    {
        logger.info("Sensors file not found: " + String(sensorsFile) + ", starting with empty configuration");
    }
    return ok || primary == 0; // false: the file exists but could not be read or restored
}

// Read a sensors file: 1 content, 0 missing or empty, -1 could not be opened
int SensorManager::readSensorsFile(const char *path, String &content)
{
    if (!LittleFS.exists(path))
    {
        return 0;
    }
    File file = LittleFS.open(path, "r");
    if (!file)
    {
        logger.error("Failed to open " + String(path) + " for reading");
        return -1;
    }
    content = file.readString();
    file.close();
    content.trim();
    return content.length() > 0 ? 1 : 0;
}

// Caller holds sensorMutex: generation 1 and generation 2 sensors from JSON
bool SensorManager::parseSensorsLocked(const String &fileContent)
{
    // Strings are copied into the document, so its size follows the file
    DynamicJsonDocument doc(fileContent.length() + JSON_OBJECT_SIZE(2) + JSON_ARRAY_SIZE(MAX_SENSORS) +
                            MAX_SENSORS * JSON_OBJECT_SIZE(SENSOR_JSON_FIELDS) + JSON_ARRAY_SIZE(MAX_GEN2_SENSORS) +
                            MAX_GEN2_SENSORS * JSON_OBJECT_SIZE(4) + 256);
    if (doc.capacity() == 0 || deserializeJson(doc, fileContent))
    {
        return false;
    }

    // Load sensors from JSON
    JsonArray sensorArray = doc["sensors"].as<JsonArray>();
    for (JsonObject sensorObj : sensorArray)
    {
        if (sensorCount < MAX_SENSORS)
        {
            uint8_t typeValue = sensorObj["deviceType"];
            SensorType deviceType = static_cast<SensorType>(typeValue);
            uint32_t serialNumber = sensorObj["serialNumber"];
            uint32_t deviceKey = sensorObj["deviceKey"];
            String name = sensorObj["name"].as<String>();
            String customUrl = sensorObj["customUrl"].as<String>();

            // Create sensor
            sensors[sensorCount].deviceType = deviceType;
            sensors[sensorCount].serialNumber = serialNumber;
            sensors[sensorCount].deviceKey = deviceKey;
            sensors[sensorCount].name = name;
            sensors[sensorCount].customUrl = customUrl;
            sensors[sensorCount].lastSeen = 0;
            sensors[sensorCount].temperature = 0.0f;
            sensors[sensorCount].humidity = 0.0f;
            sensors[sensorCount].pressure = 0.0f;
            sensors[sensorCount].ppm = 0.0f;
            sensors[sensorCount].lux = 0.0f;
            sensors[sensorCount].batteryVoltage = 0.0f;
            sensors[sensorCount].rssi = 0;
            sensors[sensorCount].configured = true;

            // Load daily rain total if it exists
            if (sensorObj.containsKey("dailyRainTotal"))
            {
                sensors[sensorCount].dailyRainTotal = sensorObj["dailyRainTotal"].as<float>();
            }
            else
            {
                sensors[sensorCount].dailyRainTotal = 0.0f;
            }

            // Load time of last reset if it exists
            if (sensorObj.containsKey("lastRainReset"))
            {
                sensors[sensorCount].lastRainReset = sensorObj["lastRainReset"].as<unsigned long>();
            }
            else
            {
                sensors[sensorCount].lastRainReset = 0;
            }

            if (sensorObj.containsKey("altitude"))
            {
                sensors[sensorCount].altitude = sensorObj["altitude"].as<int>();
            }
            else
            {
                sensors[sensorCount].altitude = 0;
            }

            if (sensorObj.containsKey("temperatureCorrection"))
            {
                sensors[sensorCount].temperatureCorrection = sensorObj["temperatureCorrection"].as<float>();
            }
            if (sensorObj.containsKey("humidityCorrection"))
            {
                sensors[sensorCount].humidityCorrection = sensorObj["humidityCorrection"].as<float>();
            }
            if (sensorObj.containsKey("pressureCorrection"))
            {
                sensors[sensorCount].pressureCorrection = sensorObj["pressureCorrection"].as<float>();
            }
            if (sensorObj.containsKey("ppmCorrection"))
            {
                sensors[sensorCount].ppmCorrection = sensorObj["ppmCorrection"].as<float>();
            }
            if (sensorObj.containsKey("luxCorrection"))
            {
                sensors[sensorCount].luxCorrection = sensorObj["luxCorrection"].as<float>();
            }
            if (sensorObj.containsKey("windSpeedCorrection"))
            {
                sensors[sensorCount].windSpeedCorrection = sensorObj["windSpeedCorrection"].as<float>();
            }
            if (sensorObj.containsKey("windDirectionCorrection"))
            {
                sensors[sensorCount].windDirectionCorrection = sensorObj["windDirectionCorrection"].as<int>();
            }
            if (sensorObj.containsKey("rainAmountCorrection"))
            {
                sensors[sensorCount].rainAmountCorrection = sensorObj["rainAmountCorrection"].as<float>();
            }
            if (sensorObj.containsKey("rainRateCorrection"))
            {
                sensors[sensorCount].rainRateCorrection = sensorObj["rainRateCorrection"].as<float>();
            }
            sensorCount++;
        }
        else
        {
            logger.warning("Too many sensors in configuration file, ignoring some");
            break;
        }
    }

    // Generation 2 sensors (absent in files written before protocol v2;
    // older firmware ignores the array)
    JsonArray gen2Array = doc["gen2"].as<JsonArray>();
    for (JsonObject obj : gen2Array)
    {
        uint32_t devId = 0;
        uint8_t label[ep::kLabelKeyLen];
        if (gen2Count >= MAX_GEN2_SENSORS || !ep::parseDevId(obj["devId"] | "", devId) ||
            !ep::parseLabelKey(obj["label"] | "", label) || findGen2Locked(devId) >= 0)
        {
            logger.warning("Ignoring an invalid or surplus generation 2 sensor entry");
            continue;
        }
        Gen2Sensor &g = gen2[gen2Count++];
        g = Gen2Sensor();
        g.configured = true;
        g.devId = devId;
        g.name = obj["name"] | "";
        memcpy(g.label, label, sizeof(label));
        if (obj.containsKey("devNonce"))
        {
            g.hasDevNonce = true;
            g.lastDevNonce = obj["devNonce"].as<uint32_t>();
        }
        Gen2Crypto::deriveJoinKeys(g); // K and the join keys live in RAM only
        ep::wipe(label, sizeof(label));
    }

    logger.info("Loaded " + String(sensorCount) + " sensors and " + String(gen2Count) +
                " generation 2 sensors from configuration");
    return true;
}

// Caller holds sensorMutex
void SensorManager::clearLocked()
{
    sensorCount = 0;
    for (size_t i = 0; i < MAX_SENSORS; i++)
    {
        sensors[i].configured = false;
    }
    gen2Count = 0;
    for (size_t i = 0; i < MAX_GEN2_SENSORS; i++)
    {
        gen2[i].wipeKeys();
        gen2[i] = Gen2Sensor();
    }
}

// ---------------------------------------------------------------------------
// Per-sensor health bookkeeping. Out-of-range indices and unconfigured slots
// are silently ignored — caller doesn't need to know whether a sensor was
// recently deleted between packet receipt and bookkeeping.
// ---------------------------------------------------------------------------

void SensorManager::recordSensorSuccess(int index, unsigned long nowMillis)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || static_cast<size_t>(index) >= sensorCount || !sensors[index].configured)
    {
        return;
    }
    SensorHealth::recordSuccess(sensors[index].health, nowMillis);
}

void SensorManager::recordSensorRejection(int index, unsigned long nowMillis, const char *reason)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (index < 0 || static_cast<size_t>(index) >= sensorCount || !sensors[index].configured)
    {
        return;
    }
    SensorHealth::recordRejection(sensors[index].health, nowMillis, reason);
}

void SensorManager::tickSensorHealth(unsigned long nowMillis)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    for (size_t i = 0; i < sensorCount; i++)
    {
        if (sensors[i].configured)
        {
            SensorHealth::advanceBucketsIfNeeded(sensors[i].health, nowMillis);
            SensorHealth::latchStale(sensors[i].health, nowMillis);
        }
    }
}

// ---------------------------------------------------------------------------
// Generation 2 sensors (protocol v2)

int SensorManager::findGen2Locked(uint32_t devId) const
{
    for (size_t i = 0; i < gen2Count; i++)
    {
        if (gen2[i].devId == devId)
        {
            return (int)i;
        }
    }
    return -1;
}

bool SensorManager::isKnownGen2(uint32_t devId) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    return findGen2Locked(devId) >= 0;
}

int SensorManager::addGen2Sensor(uint32_t devId, const uint8_t label[ep::kLabelKeyLen], const String &name)
{
    std::unique_lock<std::mutex> lock(sensorMutex);
    if (findGen2Locked(devId) >= 0)
    {
        return -2;
    }
    if (gen2Count >= MAX_GEN2_SENSORS)
    {
        return -1;
    }
    size_t index = gen2Count++;
    Gen2Sensor &g = gen2[index];
    g = Gen2Sensor();
    g.configured = true;
    g.devId = devId;
    g.name = name;
    memcpy(g.label, label, ep::kLabelKeyLen);
    Gen2Crypto::deriveJoinKeys(g);
    waiting.remove(devId);
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)devId);
    // Snapshot under the same lock: if it fails, the entry is taken back out
    // before any other writer can see it
    String json;
    if (!snapshotLocked(json))
    {
        removeGen2Locked(devId);
        return -3;
    }
    uint32_t seq = ++snapshotSeq;
    lock.unlock();
    if (!writeSnapshot(json, seq))
    {
        // Not on flash: take it back out so a retry does not hit "exists". A
        // save by another writer in between may already contain the entry;
        // then it returns after a restart (logged)
        lock.lock();
        removeGen2Locked(devId);
        logger.warning("Generation 2 sensor " + String(id) + " not saved (flash write failed)");
        return -3;
    }
    logger.info("Added generation 2 sensor " + name + " (DevID " + String(id) + ")");
    return (int)index;
}

// Caller holds sensorMutex
void SensorManager::removeGen2Locked(uint32_t devId)
{
    int i = findGen2Locked(devId);
    if (i < 0)
    {
        return;
    }
    gen2[i].wipeKeys();
    gen2[i] = gen2[gen2Count - 1];
    gen2[gen2Count - 1].wipeKeys();
    gen2[gen2Count - 1] = Gen2Sensor();
    gen2Count--;
}

bool SensorManager::deleteGen2Sensor(uint32_t devId)
{
    std::unique_lock<std::mutex> lock(sensorMutex);
    if (findGen2Locked(devId) < 0)
    {
        return false;
    }
    removeGen2Locked(devId);
    logger.info("Deleted a generation 2 sensor");
    return saveAndUnlock(lock);
}

std::vector<Gen2Sensor> SensorManager::getGen2Sensors() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    std::vector<Gen2Sensor> out(gen2, gen2 + gen2Count);
    for (Gen2Sensor &g : out)
    {
        ep::wipe(&g.joinKeys, sizeof(g.joinKeys)); // callers only display
    }
    return out;
}

size_t SensorManager::getWaitingList(WaitingEntry *out, size_t max) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    return waiting.listed(out, max);
}

void SensorManager::recordWaitingJoin(const ep::JoinRequest &req, int16_t rssi, unsigned long nowMs)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (findGen2Locked(req.devId) >= 0)
    {
        return;
    }
    if (waiting.record(req, rssi, (uint32_t)nowMs) &&
        (!waitingLogged || nowMs - lastWaitingLogMs >= 60000)) // global limit: 1 line per minute
    {
        waitingLogged = true;
        lastWaitingLogMs = nowMs;
        char id[9];
        snprintf(id, sizeof(id), "%08lX", (unsigned long)req.devId);
        logger.info("Generation 2 sensor waiting to be added: DevID " + String(id) + ", HW " + String(req.hwType) +
                    ", firmware " + String(req.fwMajor) + "." + String(req.fwMinor) + "." + String(req.fwPatch));
    }
}

// A frame of a configured gen2 sensor. Phase A (M2b): JOIN_REQ is verified
// and its content shown; nothing is answered or decrypted yet. Counters
// always; at most one log line per DevID and kind per GEN2_LOG_INTERVAL_MS.
static constexpr unsigned long GEN2_LOG_INTERVAL_MS = 600000;

static bool logDue(bool &done, unsigned long &lastMs, unsigned long nowMs)
{
    if (done && nowMs - lastMs < GEN2_LOG_INTERVAL_MS)
    {
        return false;
    }
    done = true;
    lastMs = nowMs;
    return true;
}

void SensorManager::handleGen2Frame(const uint8_t *frame, size_t len, const ep::FrameInfo &info, int16_t rssi,
                                    float snr, float freqError, unsigned long nowMs)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(info.devId);
    if (i < 0)
    {
        return;
    }
    Gen2Sensor &g = gen2[i];
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)info.devId);

    switch (info.type)
    {
    case ep::FrameType::JoinReq:
    {
        ep::JoinRequest req;
        if (!ep::parseJoinRequest(frame, len, req) ||
            !ep::verifyJoinRequest(Gen2Crypto::aes, g.joinKeys, frame, len))
        {
            g.micFailures++;
            if (logDue(g.warned, g.lastWarnMs, nowMs))
            {
                logger.warning("Generation 2 JOIN_REQ from " + String(id) + " failed the MIC check (" +
                               String(g.micFailures) +
                               " so far) - wrong label key, or the sensor is paired with another gateway?");
            }
            return;
        }
        if (g.seen && req.devNonce <= g.lastJoinDevNonce)
        {
            g.replayedJoins++; // old devNonce: a replay, the status stays as it is
            return;
        }
        g.seen = true;
        g.lastSeenMs = nowMs;
        g.rssi = rssi;
        g.snr = snr;
        g.freqError = freqError;
        g.hwType = req.hwType;
        g.fwMajor = req.fwMajor;
        g.fwMinor = req.fwMinor;
        g.fwPatch = req.fwPatch;
        g.caps = req.caps;
        g.configVersion = req.configVersion;
        g.lastJoinDevNonce = req.devNonce;
        g.joinRequests++;
        if (logDue(g.logged, g.lastLogMs, nowMs))
        {
            logger.info("Generation 2 JOIN_REQ from " + g.name + " (" + String(id) + "), devNonce " +
                        String(req.devNonce) + ", RSSI " + String(rssi) + " dBm - join answers come with M3");
        }
        return;
    }
    case ep::FrameType::Up:
        // No session yet (sessions are never persisted): REJOIN in M3
        g.upWithoutSession++;
        if (logDue(g.logged, g.lastLogMs, nowMs))
        {
            logger.info("Generation 2 UP from " + String(id) + " without a session (REJOIN comes with M3)");
        }
        return;
    default:
        // A downlink frame carrying this DevID (another gateway?) - ignored
        return;
    }
}
