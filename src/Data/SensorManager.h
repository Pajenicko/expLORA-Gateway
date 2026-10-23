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
#include <memory>
#include <mutex>
#include <vector>
#include "SensorData.h"
#include "Gen2Sensor.h"
#include "../Protocol/WaitingList.h"
#include "../Hardware/RadioTask.h"
#include "../Protocol/OtaJob.h"
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

    // Generation 2 sensors (protocol v2) and unknown DevIDs seen in JOIN_REQs
    Gen2Sensor gen2[MAX_GEN2_SENSORS];
    size_t gen2Count = 0;
    bool devNonceSaveDue = false;      // devNonce / firmware / boot state changed: write sensors.json after the response
    uint32_t devNonceSaveAtMs = 0;     // next attempt (DueTime::reached)
    void scheduleGen2SaveLocked(unsigned long nowMs);
    WaitingList<8> waiting;
    bool waitingLogged = false;
    unsigned long lastWaitingLogMs = 0;
    int findGen2Locked(uint32_t devId) const;
    void removeGen2Locked(uint32_t devId);
    void clearLocked();
    int readSensorsFile(const char *path, String &content);
    bool parseSensorsLocked(const String &fileContent);

    uint32_t snapshotSeq = 0; // under sensorMutex
    uint32_t writtenSeq = 0;  // under fileMutex
    std::mutex fileMutex;     // sensors.json writes (taken after the flash permit)
    bool snapshotLocked(String &json);
    bool saveAndUnlock(std::unique_lock<std::mutex> &lock);
    bool writeSnapshot(const String &json, uint32_t seq);

    // OTA (protocol v2, docs/protocol-v2.md 14.5; SensorManagerOta.cpp).
    // The job lives under sensorMutex; job.json is written from loop().
    std::unique_ptr<OtaJob::Job> otaJobPtr; // heap, only while a job (or its revert watch) exists
    OtaJob::Job &otaJobLocked();            // the job, created when there is none
    void otaDropIfGoneLocked();             // free a job that no longer exists
    bool otaSaveDue = false;
    uint32_t otaSaveAtMs = 0;
    uint32_t otaSeq = 0;            // OTA frame sequence numbers
    bool otaFrameOut = false;       // a frame is with the radio task
    uint32_t otaFrameSeq = 0;
    uint32_t otaFrameSlot = 0;
    uint16_t otaFrameIndex = 0;
    bool otaFrameEnd = false;
    uint32_t otaLastSlot = UINT32_MAX; // the slot a frame was built (or skipped) for
    int64_t otaLastAnchorUs = 0;       // slot numbers restart with every accepting UP
    bool otaStartPending = false;   // a start requested by the admin, run by loop()
    uint32_t otaStartDevId = 0;
    uint32_t otaSkipUplink = 0, otaSkipLoop = 0, otaSkipSession = 0, otaSkipRead = 0;
    uint32_t otaSkipRadio[10] = {}; // per OtaOutcome
    uint32_t otaSaveMaxUs = 0;      // job.json write (diagnostics)
    String otaLastEvent;            // last state change in words (page / diag)
    OtaJob::Now otaNow() const;
    void otaScheduleSaveLocked(unsigned long nowMs);
    void otaApplyLocked(Gen2Sensor &g, const OtaJob::Result &r, const OtaJob::Up &u, unsigned long nowMs);
    OtaJob::Down otaOnUpLocked(Gen2Sensor &g, OtaJob::Up &u, unsigned long nowMs);
    bool otaSaveJob();
    void otaRunStart();
    void otaBuildFrame(unsigned long nowMs);

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

    // Switches the time zone (setenv/tzset, call from loop()). The daily rain
    // reset keeps the old zone's view of the last reset: a total started
    // today stays today's (no extra reset when the last reset falls on
    // yesterday in the new zone), one from an earlier day is still reset by
    // the next packet (not skipped when the new zone puts it on today).
    void applyTimezone(const String &posix);

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

    // Load sensor configuration from file (sensors.bak when it is damaged)
    bool loadSensors();

    // Generation 2 sensors. add: index, -1 full, -2 DevID exists, -3 save failed
    int addGen2Sensor(uint32_t devId, const uint8_t label[ep::kLabelKeyLen], const String &name);
    bool deleteGen2Sensor(uint32_t devId);
    bool isKnownGen2(uint32_t devId) const;
    std::vector<Gen2Sensor> getGen2Sensors() const; // copies without key material except the label
    size_t getWaitingList(WaitingEntry *out, size_t max) const;
    void recordWaitingJoin(const ep::JoinRequest &req, int16_t rssi, unsigned long nowMs);
    // A frame of a configured gen2 sensor. true: req holds the response for
    // packet.responseSlot (never with slot 0)
    bool handleGen2Frame(const RadioPacket &packet, const ep::FrameInfo &info, unsigned long nowMs, TxRequest &req);
    // Sensor detail page (copy without key material)
    bool getGen2Sensor(uint32_t devId, Gen2Sensor &out) const;
    // New desired configuration (clamped; version raised when it changed).
    // 1 changed and saved, 0 unchanged, -1 unknown DevID, -2 save failed
    int setGen2Config(uint32_t devId, const Gen2Config::Values &values);
    // Ask for a status report with the next DOWN
    bool requestGen2Status(uint32_t devId);
    // Forget the stored devNonce (sensor lost its configuration store, spec 12a)
    bool resetGen2DevNonce(uint32_t devId);
    // Outcome of a response reported by the radio task
    void recordTxResult(const TxResult &result);

    // --- OTA (SensorManagerOta.cpp) ---
    // setup(): job.json restored (Transfer -> Paused, uptime deadlines restart)
    void otaBegin();
    // loop(): queued start, deadlines, block frames for the radio task, job.json
    void otaProcess(unsigned long nowMs);
    // loop(): outcome of an OTA frame
    void otaResult(const OtaResult &result);
    // Start checks (gen2 page): blockers stop it, warnings and confirm
    // (downgrade, reinstall, the reverted image) are shown; estimate in s
    struct OtaCheck
    {
        bool imageOk = false;
        uint32_t imageFw = 0, imageLen = 0;
        String blockers, warnings, confirm;
        uint32_t estimateS = 0;
    };
    // requestStatus: ask the sensor for a status report when none is known
    // (the start path; a page view only shows the blocker)
    OtaCheck otaCheckStart(uint32_t devId, bool requestStatus = false);
    // Queue a start (loop() re-verifies the image and creates the job);
    // false: blocked (msg says why)
    bool otaRequestStart(uint32_t devId, bool confirmed, String &msg);
    // Admin actions: 'a' abort, 'p' pause, 'r' resume, 'c' clear
    bool otaAdmin(uint32_t devId, char action, String &msg);
    // Copy of the job (page, diag); false when there is none
    bool otaJobCopy(OtaJob::Job &out) const;
    String otaLastEventText() const;
    // A non-terminal job uses an image of this HW type (no upload / delete then)
    bool otaImageBusy(uint16_t hwType) const;
    // An image was replaced or deleted: a terminal job of this HW type goes
    void otaImageChanged(uint16_t hwType);
    // /diag/heap "ota" object (JSON text)
    String otaDiagJson() const;

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