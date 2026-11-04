/**
 * expLORA Gateway
 *
 * MQTT communication manager header file
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

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <vector>
#include "Data/SensorManager.h"
#include "Data/Logging.h"
#include "Storage/ConfigManager.h"

/**
 * Class for managing MQTT communication with Home Assistant
 *
 * Handles automatic sensor detection in Home Assistant using MQTT discovery
 * and regular publishing of sensor data.
 */
class MQTTManager
{
private:
    WiFiClient wifiClient;              // WiFi client for MQTT
    // MQTTS client: its TLS context (~2 kB) exists only while TLS is configured,
    // created by the first TLS connect()
    std::unique_ptr<WiFiClientSecure> wifiClientSecure;
    PubSubClient mqttClient;            // MQTT client
    SensorManager &sensorManager;       // Reference to sensor manager
    ConfigManager &configManager;       // Reference to configuration
    Logger &logger;                     // Reference to logger

    String clientId;                    // MQTT Client ID
    unsigned long lastReconnectAttempt; // Time of last connection attempt
    unsigned long lastDiscoveryUpdate;  // Time of last discovery update
    
    // MQTT flag representing if client is ready
    bool mqttClientReady = false;

    // Gateway's own entities (uplink, power source, LTE signal)
    String gatewayId;                   // Device identifier, same as the AP name
    String lastGatewayState;            // Last published state, to publish on change
    unsigned long lastGatewayCheck = 0;
    unsigned long lastGatewayPublish = 0;

    // Connect to MQTT broker
    bool connect();

    // Discovery config of one sensor (generation 1 or 2), its current values
    static constexpr size_t HA_PAYLOAD_MAX = 900; // < the 1 kB PubSubClient buffer with the topic
    void publishSensorDiscovery(const SensorData &sensor);
    void publishState(const SensorData &sensor);
    bool publishRetained(const String &topic, const String &payload);
    volatile bool haRestarted = false; // "online" on <haPrefix>/status

    // Work queued by other tasks (web handlers), done in process()
    struct PendingOp
    {
        enum Type : uint8_t
        {
            Discovery,   // generation 1 sensor (serial number)
            Removal,     // its entities (serial number or DevID)
            Gen2Publish, // readings of a generation 2 sensor, discovery first when 'discover'
        } type;
        bool gen2;
        bool discover;
        uint32_t id; // serial number or DevID
    };
    void queue(const PendingOp &op);
    std::mutex pendingMutex;
    std::vector<PendingOp> pending;
    bool reinitRequested = false;
    void processPending();

    String statusTopic;

    // Gateway entities: discovery and state
    String gatewayTopic(const char *key) const;
    void publishGatewayEntity(const char *key, const char *name, const char *deviceClass,
                              const char *unit, const char *icon);
    void publishGatewayDiscovery();
    void publishGatewayState(bool force);

public:
    // Constructor
    MQTTManager(SensorManager &sensors, ConfigManager &config, Logger &log);

    // Initialization
    bool init();

    // Process MQTT communication (call in main loop)
    void process();

    // Publish discovery configuration for sensors
    void publishDiscovery();

    // Publish sensor data
    void publishSensorData(int sensorIndex);

    // Remove the entities of a deleted sensor (its ID: 6 or 8 hex digits)
    void removeDiscoveryForSensor(const String &id);

    // Thread-safe requests (any task), carried out by process()
    void requestSensorDiscovery(uint32_t serialNumber);
    void requestSensorRemoval(uint32_t id, bool gen2 = false);
    // A generation 2 sensor: publish its readings; discover: send its
    // discovery first (new quantities, renamed)
    void requestGen2Publish(uint32_t devId, bool discover);
    // Copy of the MQTT configuration taken by init() (loop task), so the
    // strings in use - PubSubClient keeps the host pointer - never change
    // under it when the web task saves new settings
    struct Settings
    {
        String host;
        int port;
        String user;
        String password;
        bool enabled;
        bool tls;
        String prefix;
        String haPrefix;
        bool haEnabled;
    };

    // Reconnect with the saved configuration (called after the web form saved it)
    void requestReinit();

private:
    Settings cfg;

public:

    // Check connection
    bool isConnected();
    bool usesTls() const { return cfg.tls; }

    // Connection state for the MQTT page and /diag/heap (any task)
    enum class Status : uint8_t
    {
        Disabled,
        Waiting,      // not tried yet (uplink offline, first attempt pending)
        Connected,
        Failed,       // the broker refused / was unreachable, statusState() has the code
        Disconnected, // was connected, reconnects
        TlsLowMemory, // TLS needs more free memory than there is; not tried
        HeldForAp,    // TLS dropped / held back while the access point cannot start
        HeldForUpdate // disconnected for an update-mode session (Update/UpdateMode.h)
    };
    // Update mode (loop task): true disconnects and keeps MQTT off, false
    // reconnects at once
    void holdForUpdate(bool hold);
    Status status() const { return statusCode.load(); }
    // A TLS session is up (any task: atomics only - the TLS client itself
    // belongs to the loop task, which may replace it)
    bool tlsSessionUp() const { return statusCode.load() == Status::Connected && connectedWithTls.load(); }
    int statusState() const { return statusMqttState.load(); }
    // Human readable, with the reason when not connected
    String statusText() const;
    // Short machine name of status() (diagnostics JSON)
    static const char *statusName(Status s);

    // Disconnect from MQTT broker
    void disconnect();

private:
    std::atomic<Status> statusCode{Status::Disabled};
    std::atomic<int> statusMqttState{0};  // PubSubClient state of the last failed attempt
    std::atomic<bool> connectedWithTls{false}; // the last connect used TLS
    std::atomic<uint32_t> tlsLowFree{0};  // internal heap at the last TLS refusal
    std::atomic<uint32_t> tlsLowLargest{0};
    bool heldForUpdate = false;           // loop task only
    bool tlsLowLogged = false;            // the refusal is logged once per episode,
    unsigned long tlsLowLoggedAt = 0;     // then at most every TLS_LOW_LOG_INTERVAL
};
