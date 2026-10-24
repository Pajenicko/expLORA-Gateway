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
#include "SensorsJsonBudget.h"
static_assert(MAX_SENSORS == SensorsJson::MAX_GEN1, "SensorsJsonBudget.h: MAX_GEN1");
static_assert(MAX_GEN2_SENSORS == SensorsJson::MAX_GEN2, "SensorsJsonBudget.h: MAX_GEN2");
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "../Hardware/FlashPermit.h"
#include "../Protocol/Gen2Crypto.h"
#include "../Protocol/Gen2Response.h"
#include "DueTime.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <memory>
#include "../Network/Uplink.h"
#include "../Hardware/WatchdogPause.h"
#include "../Hardware/HeapGuard.h"

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
        if (i >= sensorCount || !sensors[i].configured)
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
    if (!sensors.ensure(newIndex))
    {
        logger.error("Failed to add sensor: not enough memory");
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
        // A TLS session needs ~52 kB of the internal heap (HeapGuard.h)
        if (!HeapGuard::tlsFits(HeapGuard::tlsHeap()))
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

// devNonce is written after the response (the radio task holds the flash
// permit until the launch, RX2 ends ~2 s after RxDone)
static constexpr unsigned long DEVNONCE_SAVE_DELAY_MS = 2500;
static constexpr unsigned long DEVNONCE_RETRY_MS = 60000;

// Rain totals: at most one flash write per interval
static constexpr unsigned long RAIN_SAVE_INTERVAL = 600000;

void SensorManager::process(unsigned long now)
{
    std::unique_lock<std::mutex> lock(sensorMutex);
    if (devNonceSaveDue && DueTime::reached((uint32_t)now, devNonceSaveAtMs))
    {
        devNonceSaveDue = false;
        if (!saveAndUnlock(lock))
        {
            logger.error("devNonce of a generation 2 sensor not saved, retrying in 60 s");
            lock.lock();
            devNonceSaveDue = true;
            devNonceSaveAtMs = (uint32_t)now + DEVNONCE_RETRY_MS;
        }
        return; // a successful save also wrote the rain totals
    }
    if (rainDirty && now - lastRainSave > RAIN_SAVE_INTERVAL)
    {
        // Next attempt after a full interval even when this one fails
        lastRainSave = now;
        saveAndUnlock(lock);
    }
}

static bool sameLocalDay(time_t a, time_t b)
{
    struct tm ta, tb;
    localtime_r(&a, &ta);
    localtime_r(&b, &tb);
    return ta.tm_mday == tb.tm_mday && ta.tm_mon == tb.tm_mon && ta.tm_year == tb.tm_year;
}

void SensorManager::applyTimezone(const String &posix)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    bool timeValid = Logger::isTimeInitialized();
    time_t now = time(nullptr);
    bool startedToday[MAX_SENSORS] = {false};
    for (size_t i = 0; timeValid && i < sensorCount; i++)
    {
        startedToday[i] = sameLocalDay((time_t)sensors[i].lastRainReset, now);
    }
    setenv("TZ", posix.c_str(), 1);
    tzset();
    for (size_t i = 0; timeValid && i < sensorCount; i++)
    {
        // updateSensorData() compares the last reset by its local date, now
        // in the new zone: keep the old zone's answer
        if (!sensors[i].configured || !sensors[i].hasRainAmount() || sensors[i].lastRainReset == 0 ||
            startedToday[i] == sameLocalDay((time_t)sensors[i].lastRainReset, now))
        {
            continue;
        }
        sensors[i].lastRainReset = startedToday[i] ? (unsigned long)now : 0; // 0 = reset by the next packet
        rainDirty = true;
    }
}


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
    if (partialLoad)
    {
        logger.error("Not saving the sensor configuration: it was loaded only in part (not enough memory), saving "
                     "would drop the rest of " + String(sensorsFile) + " - restart the gateway");
        return false;
    }
    size_t textSize = 0;
    for (size_t i = 0; i < sensorCount; i++)
    {
        textSize += sensors[i].name.length() + sensors[i].customUrl.length() + 2;
    }
    for (size_t i = 0; i < gen2Count; i++)
    {
        textSize += gen2[i].name.length() + 32 + 12; // + DevID, label and firmware version strings
    }
    // Sized for the sensors present (one smaller block on a fragmented heap)
    DynamicJsonDocument doc(SensorsJson::writeCapacity(sensorCount, gen2Count, textSize));
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
            obj["interval"] = g.config.intervalS;
            obj["slot"] = g.config.slotS;
            obj["txPower"] = g.config.txPowerDbm;
            obj["ackEvery"] = g.config.ackEvery;
            obj["ackRetries"] = g.config.ackRetries;
            obj["ackBackoff"] = g.config.ackBackoffS;
            obj["cfgVersion"] = g.cfgVersion;
            if (g.hasFw)
            {
                char fw[12];
                snprintf(fw, sizeof(fw), "%u.%u.%u", g.fwMajor, g.fwMinor, g.fwPatch);
                obj["fw"] = fw; // copied (char array)
                obj["hw"] = g.hwType;
            }
            if (g.bootState != 0xFF)
            {
                obj["bootState"] = g.bootState;
            }
            if (g.hasReverted)
            {
                obj["revFw"] = g.revertedFw;
                obj["revCrc"] = g.revertedCrc;
            }
            if (g.otaWatch.armed())
            {
                JsonArray w = obj.createNestedArray("otaWatch");
                w.add(g.otaWatchFw);
                w.add(g.otaWatchCrc);
                w.add(g.otaWatch.kind);
                w.add(g.otaWatch.at);
                w.add(g.otaWatch.durS);
            }
            if (g.otaAbort.armed())
            {
                JsonArray a = obj.createNestedArray("otaAbort");
                a.add(g.otaAbort.kind);
                a.add(g.otaAbort.at);
                a.add(g.otaAbort.durS);
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
    partialLoad = false;

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
    int parsed = present ? parseSensorsLocked(content, sensorsFile) : 0;
    if (primary == -2 || parsed < 0)
    {
        // Out of memory while reading or parsing: the file is fine. Not moved
        // aside, no restore from the backup; the tables stay as far as they
        // got and saving is blocked until the next load (restart)
        partialLoad = true;
        logger.error("Could not load " + String(sensorsFile) + " (not enough memory, or larger than this firmware "
                     "expects) - sensors not (fully) loaded, the file is kept, saving is blocked until a restart");
        return true;
    }
    bool ok = parsed > 0;
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
        int backupRead = readSensorsFile(SENSORS_BACKUP_FILE, backup);
        int backupParsed = backupRead > 0 ? parseSensorsLocked(backup, SENSORS_BACKUP_FILE) : 0;
        if (backupRead == -2 || backupParsed < 0)
        {
            partialLoad = true; // no memory for the backup either: never save over it
            logger.error("Not enough memory to load " + String(SENSORS_BACKUP_FILE) + " - saving is blocked until a restart");
        }
        if (backupParsed > 0)
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

// Read a sensors file: 1 content, 0 missing or empty, -1 could not be opened,
// read short or larger than SensorsJson::MAX_FILE_BYTES (damage), -2 no
// memory for its content (the file is fine)
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
    size_t size = file.size();
    content = String();
    if (size > SensorsJson::MAX_FILE_BYTES)
    {
        file.close();
        logger.error(String(path) + " is " + String(size) + " B - more than any configuration");
        return -1; // damage, not a short heap
    }
    if (!content.reserve(size))
    {
        file.close();
        return -2;
    }
    uint8_t buf[128];
    while (content.length() < size)
    {
        int n = file.read(buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        content.concat((const char *)buf, (unsigned int)n);
    }
    file.close();
    if (content.length() < size)
    {
        logger.error("Read only " + String(content.length()) + " of " + String(size) + " B of " + String(path));
        return -1; // the memory was there: the file system could not read it
    }
    content.trim();
    return content.length() > 0 ? 1 : 0;
}

// Caller holds sensorMutex: generation 1 and generation 2 sensors from JSON
int SensorManager::parseSensorsLocked(const String &fileContent, const char *path)
{
    // Read budget with slack (SensorsJsonBudget.h). A file that still needs
    // more (another firmware version: more members or sensors) gets one
    // retry with twice the document; if that fails too it cannot be loaded
    // here - saves are blocked (-1), the file and sensors.bak stay as they
    // are. Only a syntax error is damage
    std::unique_ptr<DynamicJsonDocument> owned;
    size_t factor = 1;
    SensorsJson::Load load = SensorsJson::parse(fileContent.c_str(), fileContent.length(), owned, factor);
    if (load == SensorsJson::Load::NoMemory)
    {
        logger.error(String(path) + " could not be parsed (no memory for a JSON document of up to twice the "
                     "expected size) - not loaded");
        return -1;
    }
    if (load == SensorsJson::Load::Damage)
    {
        return 0;
    }
    DynamicJsonDocument &doc = *owned;

    // Load sensors from JSON
    JsonArray sensorArray = doc["sensors"].as<JsonArray>();
    for (JsonObject sensorObj : sensorArray)
    {
        if (sensorCount < MAX_SENSORS && !sensors.ensure(sensorCount))
        {
            partialLoad = true;
            logger.error("Not enough memory for all sensors - the rest is not loaded, saving is blocked until a restart");
            break;
        }
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
        if (!gen2.ensure(gen2Count))
        {
            partialLoad = true;
            logger.error("Not enough memory for all generation 2 sensors - the rest is not loaded, saving is blocked "
                         "until a restart");
            break;
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
        // Configuration (defaults for entries written before M3)
        g.config.intervalS = obj["interval"] | g.config.intervalS;
        g.config.slotS = obj["slot"] | g.config.slotS;
        g.config.txPowerDbm = obj["txPower"] | g.config.txPowerDbm;
        g.config.ackEvery = obj["ackEvery"] | g.config.ackEvery;
        g.config.ackRetries = obj["ackRetries"] | g.config.ackRetries;
        g.config.ackBackoffS = obj["ackBackoff"] | g.config.ackBackoffS;
        g.cfgVersion = obj["cfgVersion"] | g.cfgVersion;
        if (!Gen2Config::validVersion(g.cfgVersion))
        {
            g.cfgVersion = 1;
        }
        Gen2Config::clamp(g.config);
        unsigned maj = 0, min = 0, pat = 0;
        if (sscanf(obj["fw"] | "", "%u.%u.%u", &maj, &min, &pat) == 3)
        {
            g.hasFw = true;
            g.fwMajor = (uint8_t)maj;
            g.fwMinor = (uint8_t)min;
            g.fwPatch = (uint8_t)pat;
            g.hwType = obj["hw"] | 0u;
        }
        g.bootState = obj["bootState"] | 0xFF;
        g.hasReverted = obj.containsKey("revFw");
        g.revertedFw = obj["revFw"] | 0u;
        g.revertedCrc = obj["revCrc"] | 0u;
        JsonArray w = obj["otaWatch"];
        if (!w.isNull() && w.size() == 5)
        {
            g.otaWatchFw = w[0] | 0u;
            g.otaWatchCrc = w[1] | 0u;
            g.otaWatch.kind = w[2] | 0u;
            g.otaWatch.at = w[3] | 0u;
            g.otaWatch.durS = w[4] | 0u;
        }
        JsonArray a = obj["otaAbort"];
        if (!a.isNull() && a.size() == 3)
        {
            g.otaAbort.kind = a[0] | 0u;
            g.otaAbort.at = a[1] | 0u;
            g.otaAbort.durS = a[2] | 0u;
        }
        // uptime deadlines restart after the gateway restart (spec 14.5)
        OtaJob::Now bootNow;
        bootNow.uptimeS = (uint32_t)(esp_timer_get_time() / 1000000);
        g.otaWatch.afterReboot(bootNow);
        g.otaAbort.afterReboot(bootNow);
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
        if (sensors.allocated(i))
        {
            sensors[i].configured = false;
        }
    }
    gen2Count = 0;
    for (size_t i = 0; i < MAX_GEN2_SENSORS; i++)
    {
        if (gen2.allocated(i))
        {
            gen2[i].wipeKeys();
            gen2[i] = Gen2Sensor();
        }
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
    if (!gen2.ensure(gen2Count))
    {
        char hex[9];
        snprintf(hex, sizeof(hex), "%08lX", (unsigned long)devId);
        logger.error("Not enough memory to add generation 2 sensor " + String(hex));
        return -4;
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
    if (otaJobPtr && otaJobPtr->devId == devId)
    {
        // A deleted sensor takes its OTA job with it
        if (otaFrameOut)
        {
            RadioTask::otaCancel(otaFrameSeq);
        }
        otaJobPtr.reset();
        otaFrameOut = false;
        otaScheduleSaveLocked(millis());
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
    std::vector<Gen2Sensor> out;
    out.reserve(gen2Count);
    for (size_t i = 0; i < gen2Count; i++)
    {
        out.push_back(gen2[i]);
    }
    for (Gen2Sensor &g : out)
    {
        ep::wipe(&g.joinKeys, sizeof(g.joinKeys)); // callers only display
        g.sessions.clear();
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

// A frame of a configured gen2 sensor: JOIN_REQ -> JOIN_ACC, UP -> verify
// (dual session), ACK when requested, REJOIN when there is no session.
// Counters always; at most one log line per DevID and kind per
// GEN2_LOG_INTERVAL_MS.
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

static bool joinAccAllowed(bool sent, unsigned long lastMs, unsigned long nowMs)
{
    return !sent || nowMs - lastMs >= Gen2Response::JOIN_ACC_INTERVAL_MS;
}

// Caller holds sensorMutex: write sensors.json shortly after the response
// (the radio task holds the flash permit until the launch)
void SensorManager::scheduleGen2SaveLocked(unsigned long nowMs)
{
    devNonceSaveDue = true;
    devNonceSaveAtMs = (uint32_t)nowMs + DEVNONCE_SAVE_DELAY_MS;
}

bool SensorManager::handleGen2Frame(const RadioPacket &packet, const ep::FrameInfo &info, unsigned long nowMs,
                                    TxRequest &req)
{
    const uint8_t *frame = packet.data;
    const size_t len = packet.length;
    const uint32_t slot = packet.responseSlot;
    std::lock_guard<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(info.devId);
    if (i < 0)
    {
        return false;
    }
    Gen2Sensor &g = gen2[i];
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)info.devId);
    req.slot = slot;

    switch (info.type)
    {
    case ep::FrameType::JoinReq:
    {
        ep::JoinRequest jr;
        if (!ep::parseJoinRequest(frame, len, jr) ||
            !ep::verifyJoinRequest(Gen2Crypto::aes, g.joinKeys, frame, len))
        {
            g.micFailures++;
            if (logDue(g.warned, g.lastWarnMs, nowMs))
            {
                logger.warning("Generation 2 JOIN_REQ from " + String(id) + " failed the MIC check (" +
                               String(g.micFailures) +
                               " so far) - wrong label key, or the sensor is paired with another gateway?");
            }
            return false;
        }
        if ((g.hasDevNonce && jr.devNonce <= g.lastDevNonce) || (g.seen && jr.devNonce <= g.lastJoinDevNonce))
        {
            // Old devNonce: a replay - or the sensor lost its configuration
            // store (spec 12a: admin "reset devNonce")
            g.replayedJoins++;
            g.staleJoinStreak++;
            if (g.staleJoinStreak >= 3 && logDue(g.warned, g.lastWarnMs, nowMs))
            {
                logger.warning("Generation 2 sensor " + g.name + " (" + String(id) + ") keeps sending JOIN_REQ with devNonce " +
                               String(jr.devNonce) + " <= " + String(g.lastDevNonce) +
                               " - lost its configuration? Reset its devNonce on the sensor page");
            }
            return false;
        }
        g.seen = true;
        g.lastSeenMs = nowMs;
        g.rssi = packet.rssi;
        g.snr = packet.snr;
        g.freqError = packet.freqError;
        if (!g.hasFw || g.hwType != jr.hwType || g.fwMajor != jr.fwMajor || g.fwMinor != jr.fwMinor ||
            g.fwPatch != jr.fwPatch)
        {
            if (g.hasFw)
            {
                // another firmware: the boot state of the old one no longer applies
                g.bootState = 0xFF;
                g.blVersion = 0;
            }
            g.hasFw = true;
            scheduleGen2SaveLocked(nowMs); // the firmware version survives a gateway restart
        }
        g.hwType = jr.hwType;
        g.fwMajor = jr.fwMajor;
        g.fwMinor = jr.fwMinor;
        g.fwPatch = jr.fwPatch;
        g.caps = jr.caps;
        g.configVersion = jr.configVersion;
        g.lastJoinDevNonce = jr.devNonce;
        g.joinRequests++;
        {
            // OTA: a new session (and maybe the new firmware after a swap)
            OtaJob::Up ou;
            ou.joinReq = true;
            ou.hasFw = true;
            ou.fw = g.fwVersion();
            ou.rxUs = packet.rxTimeUs;
            otaOnUpLocked(g, ou, nowMs);
        }
        if (slot == 0)
        {
            g.noSlot++; // the sensor retries with a new devNonce
            return false;
        }
        if (!joinAccAllowed(g.acceptSent, g.lastAcceptMs, nowMs))
        {
            g.joinLimited++;
            return false;
        }
        if (!Gen2Response::buildJoinAccept(g, jr.devNonce, packet.rxTimeUs, req))
        {
            return false;
        }
        g.staleJoinStreak = 0;
        g.lastDevNonce = jr.devNonce;
        g.hasDevNonce = true;
        scheduleGen2SaveLocked(nowMs);
        g.acceptSent = true;
        g.lastAcceptMs = nowMs;
        g.joinAccepts++;
        if (g.reportedTx != INT8_MAX)
        {
            g.reportedTx = INT8_MAX; // a new session: a fresh status report tells the power again
            g.statusRequested = true;
        }
        if (logDue(g.logged, g.lastLogMs, nowMs))
        {
            logger.info("Generation 2 JOIN_REQ from " + g.name + " (" + String(id) + ") accepted, devNonce " +
                        String(jr.devNonce) + ", RSSI " + String(packet.rssi) + " dBm");
        }
        return true;
    }
    case ep::FrameType::Up:
    {
        if (len < ep::kDataOverhead)
        {
            return false;
        }
        uint8_t payload[ep::kMaxFrame]; // >= len - kDataOverhead
        size_t payloadLen = 0;
        uint32_t fcnt32 = 0;
        SessionUse use = g.sessions.openUp(Gen2Crypto::aes, frame, len, payload, payloadLen, fcnt32);
        if (use == SessionUse::None)
        {
            if (g.sessions.current.valid)
            {
                g.micFailures++; // forged, replayed or another session
                return false;
            }
            // No session (gateway restarted), or only a pending one the
            // sensor never got (JOIN_ACC lost): REJOIN bound to this uplink
            // (spec 5.4)
            g.upWithoutSession++;
            if (slot == 0)
            {
                g.noSlot++;
                return false;
            }
            if (!joinAccAllowed(g.rejoinSent, g.lastRejoinMs, nowMs))
            {
                g.joinLimited++;
                return false;
            }
            if (!Gen2Response::buildRejoin(g, frame, len, info.fcnt16, req))
            {
                return false;
            }
            g.rejoinSent = true;
            g.lastRejoinMs = nowMs;
            g.rejoins++;
            if (logDue(g.logged, g.lastLogMs, nowMs))
            {
                logger.info("Generation 2 UP from " + g.name + " (" + String(id) + ") without a session - REJOIN");
            }
            return true;
        }
        g.seen = true;
        g.lastSeenMs = nowMs;
        g.rssi = packet.rssi;
        g.snr = packet.snr;
        g.freqError = packet.freqError;
        g.upFrames++;
        g.linkLostPending +=
            Gen2Link::lostBetween(use != SessionUse::Promoted && g.upFrames > 1, g.lastFcnt, fcnt32);
        g.lastFcnt = fcnt32;
        g.configVersion = info.fctrl & ep::kFctrlCfgMask; // version the sensor runs
        const uint8_t bootStateBefore = g.bootState;
        g.upDownlinkRssi = Gen2Link::NONE;
        g.upDownlinkSnr = Gen2Link::NONE;
        OtaJob::Up ou;
        const bool confirmed = Gen2Response::decodeUp(g, payload, payloadLen, nowMs, &ou);
        // Link history (spec 4.3): only while the sensor runs the desired
        // configuration, so the TX power of the sample is known
        if (Gen2Config::inSync(g.configVersion, g.cfgVersion))
        {
            g.link.add(gen2LinkNowS(), packet.rssi, (int)floorf(packet.snr), g.upDownlinkRssi, g.upDownlinkSnr,
                       g.effectiveTxPower(), g.linkLostPending);
            g.linkLostPending = 0;
        }
        if (g.bootState != bootStateBefore)
        {
            scheduleGen2SaveLocked(nowMs);
            logger.info("Generation 2 sensor " + g.name + ": boot state " + String(g.bootState));
        }
        ep::wipe(payload, payloadLen);
        if (use == SessionUse::Promoted)
        {
            g.sessionUp = true;
            logger.info("Generation 2 sensor " + g.name + " (" + String(id) + "): session established");
        }
        // OTA (spec 14.5): TLVs applied above, then the job's transitions,
        // then the DOWN (a start, an OTA end, or nothing)
        ou.bit4 = info.fctrl & ep::kFctrlOtaActive;
        ou.fcnt32 = fcnt32;
        ou.rxUs = packet.rxTimeUs;
        ou.timeReliable = packet.timeReliable;
        ou.airUs = ep::ota::airtimeUs(len, 9);
        ou.sessionNonce = g.sessions.current.devNonce;
        ou.canStart = g.sessions.current.valid && Gen2Config::inSync(g.configVersion, g.cfgVersion);
#ifdef OTA_TEST_DROP_ACCEPT
        // Bench build only: the first accepting UP after a boot is treated as
        // lost (no OTA facts, no DOWN) - the "lost accepting UP" recovery paths
        // OTA_TEST_DROP_ACCEPT = n: the n-th accepting UP since boot (1 = the first)
        static int acceptsSeen = 0;
        if (ou.accepted && ++acceptsSeen == OTA_TEST_DROP_ACCEPT)
        {
            logger.warning("TEST: accepting UP of " + g.name + " dropped (OTA_TEST_DROP_ACCEPT)");
            return false;
        }
#endif
        const OtaJob::Down otaDown = otaOnUpLocked(g, ou, nowMs);
        const bool ackRequested = info.fctrl & ep::kFctrlAck;
        if (!Gen2Response::needsDown(g, ackRequested) && otaDown == OtaJob::Down::None)
        {
            return false;
        }
        if (slot == 0)
        {
            g.noSlot++;
            return false;
        }
        uint8_t otaTlv[2 + ep::ota::kStartLen];
        uint8_t otaLen = 0;
        if (otaDown == OtaJob::Down::Start)
        {
            uint8_t v[ep::ota::kStartLen];
            ep::ota::encodeStart(otaJobPtr->start(), v);
            ep::TlvWriter w(otaTlv, sizeof(otaTlv));
            w.add(ep::down::kOtaStart, 0, v, sizeof(v));
            otaLen = (uint8_t)w.size();
        }
        else if (otaDown != OtaJob::Down::None)
        {
            const uint8_t code = otaDown == OtaJob::Down::EndAbort ? (uint8_t)ep::ota::EndCode::Abort
                                                                   : (uint8_t)ep::ota::EndCode::Pause;
            ep::TlvWriter w(otaTlv, sizeof(otaTlv));
            w.add(ep::down::kOtaEnd, 0, &code, 1);
            otaLen = (uint8_t)w.size();
        }
        bool otaPlaced = false;
        const bool built = Gen2Response::buildDown(g, fcnt32, ackRequested, confirmed, packet.rxTimeUs, req,
                                                   otaLen ? otaTlv : nullptr, otaLen,
                                                   otaDown == OtaJob::Down::Start, &otaPlaced);
        if (built && otaDown != OtaJob::Down::None && !otaPlaced)
        {
            // The configuration filled the DOWN: the OTA TLV goes with a later
            // DOWN (the job asks again: start / abortPending / orphan)
            logger.info("OTA TLV to " + g.name + " deferred (the DOWN is full with the configuration)");
        }
        else if (built && otaDown == OtaJob::Down::Start)
        {
            OtaJob::startSent(*otaJobPtr, fcnt32, g.sessions.current.devNonce, otaNow());
            const uint32_t fw = otaJobPtr->fw;
            logger.info("OTA start to " + g.name + " (v" + String(fw >> 16) + "." + String((fw >> 8) & 0xFF) + "." +
                        String(fw & 0xFF) + ") in the DOWN to FCnt " +
                        String(fcnt32));
        }
        else if (built && otaDown != OtaJob::Down::None)
        {
            logger.info(String("OTA end ") + (otaDown == OtaJob::Down::EndAbort ? "(abort)" : "(pause)") + " to " +
                        g.name);
        }
        return built;
    }
    default:
        // A downlink frame carrying this DevID (another gateway?) - ignored
        return false;
    }
}

bool SensorManager::getGen2Sensor(uint32_t devId, Gen2Sensor &out) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(devId);
    if (i < 0)
    {
        return false;
    }
    out = gen2[i];
    ep::wipe(&out.joinKeys, sizeof(out.joinKeys));
    out.sessions.clear();
    return true;
}

int SensorManager::setGen2Config(uint32_t devId, const Gen2Config::Values &values)
{
    std::unique_lock<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(devId);
    if (i < 0)
    {
        return -1;
    }
    Gen2Sensor &g = gen2[i];
    Gen2Config::Values v = values;
    Gen2Config::clamp(v);
    if (v == g.config)
    {
        return 0;
    }
    Gen2Config::Values old = g.config;
    g.config = v;
    g.cfgVersion = Gen2Config::nextVersion(g.cfgVersion, g.configVersion);
    // The reported power belongs to the old configuration (spec 4.3); after a
    // power change, or when a reported power is forgotten, the next UP brings
    // a fresh status report
    if (v.txPowerDbm != old.txPowerDbm || g.reportedTx != INT8_MAX)
    {
        g.statusRequested = true;
    }
    g.reportedTx = INT8_MAX;
    logger.info("Generation 2 sensor " + g.name + ": configuration v" + String(g.cfgVersion) + " (interval " +
                String(v.intervalS) + " s, TX " + String(v.txPowerDbm) + " dBm) - sent with the next uplink");
    if (saveAndUnlock(lock))
    {
        return 1;
    }
    // Not on flash: take the values back so the UI does not show a state
    // that a restart would lose. loop() may already have sent them under
    // the raised version, so the old values get yet another version: a
    // number is never reused for different content (spec 4.2).
    lock.lock();
    i = findGen2Locked(devId);
    if (i >= 0)
    {
        gen2[i].config = old;
        gen2[i].cfgVersion = Gen2Config::nextVersion(gen2[i].cfgVersion, gen2[i].configVersion);
        if (gen2[i].reportedTx != INT8_MAX)
        {
            gen2[i].statusRequested = true;
            gen2[i].reportedTx = INT8_MAX;
        }
    }
    return -2;
}

bool SensorManager::requestGen2Status(uint32_t devId)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(devId);
    if (i < 0)
    {
        return false;
    }
    gen2[i].statusRequested = true;
    return true;
}

bool SensorManager::resetGen2DevNonce(uint32_t devId)
{
    std::unique_lock<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(devId);
    if (i < 0)
    {
        return false;
    }
    Gen2Sensor &g = gen2[i];
    g.hasDevNonce = false;
    g.lastDevNonce = 0;
    g.lastJoinDevNonce = 0;
    g.staleJoinStreak = 0;
    logger.warning("Generation 2 sensor " + g.name +
                   ": devNonce reset by the admin - the next valid JOIN_REQ is accepted with any devNonce");
    return saveAndUnlock(lock);
}

void SensorManager::recordTxResult(const TxResult &result)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(result.devId);
    if (i < 0)
    {
        return;
    }
    Gen2Sensor &g = gen2[i];
    g.lastDownOutcome = (uint8_t)result.outcome;
    if (result.outcome == TxOutcome::Rx1)
    {
        g.downRx1++;
    }
    else if (result.outcome == TxOutcome::Rx2)
    {
        g.downRx2++;
    }
    else
    {
        g.downNotSent++;
    }
}
