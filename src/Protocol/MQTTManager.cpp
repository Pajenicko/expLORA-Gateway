/**
 * expLORA Gateway
 *
 * MQTT communication manager implementation file
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

#include "MQTTManager.h"
#include <new>
#include "../Hardware/WatchdogPause.h"
#include <ArduinoJson.h>
#include "config.h"
#include "../Network/Uplink.h"
#include "../Hardware/PowerSource.h"
#include "../Hardware/HeapGuard.h"

// Gateway state: checked for changes every 5 s, republished at least every 5 min
static constexpr unsigned long GATEWAY_STATE_CHECK = 5000;
static constexpr unsigned long GATEWAY_STATE_REFRESH = 300000;
// MQTT TLS refused for lack of memory: logged when it starts, then this often
static constexpr unsigned long TLS_LOW_LOG_INTERVAL = 1800000;

// Constructor
MQTTManager::MQTTManager(SensorManager &sensors, ConfigManager &config, Logger &log)
    : mqttClient(), sensorManager(sensors), configManager(config), logger(log),
      lastReconnectAttempt(0), lastDiscoveryUpdate(0)
{

    // Generate unique client ID from MAC address
    clientId = "explora-gw-";
    uint8_t mac[6];
    getStationMac(mac);
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] < 0x10)
            clientId += '0';
        clientId += String(mac[i], HEX);
    }
    gatewayId = makeApName();
}

// Initialization
bool MQTTManager::init()
{
    // Own copy: the web task may change the configuration at any time
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        cfg.host = configManager.mqttHost;
        cfg.port = configManager.mqttPort;
        cfg.user = configManager.mqttUser;
        cfg.password = configManager.mqttPassword;
        cfg.enabled = configManager.mqttEnabled;
        cfg.tls = configManager.mqttTls;
        cfg.prefix = configManager.mqttPrefix;
        cfg.haPrefix = configManager.mqttHAPrefix;
        cfg.haEnabled = configManager.mqttHAEnabled;
    }

    // The plain client until a TLS connect() sets the secure one; a TLS
    // client left from an earlier configuration goes (reinit disconnected it)
    mqttClient.setClient(wifiClient);
    if (!cfg.enabled || !cfg.tls)
    {
        wifiClientSecure.reset();
    }

    // Only initialize if MQTT is enabled in configuration
    if (!cfg.enabled)
    {
        statusCode = Status::Disabled;
        logger.info("MQTT integration disabled in configuration");
        return false;
    }
    statusCode = Status::Waiting;
    tlsLowLogged = false;

    if (!cfg.tls)
    {
        wifiClient.setConnectionTimeout(5000);
    }
    mqttClientReady = true;

    if (!mqttClient.setBufferSize(1024)) // discovery payloads need it
    {
        logger.error("MQTT: not enough memory for the 1 kB buffer - discovery will fail");
    }
    mqttClient.setCallback([this](char *topic, uint8_t *payload, unsigned int length)
                           {
        // Only subscription: <haPrefix>/status
        if (length == 6 && memcmp(payload, "online", 6) == 0)
        {
            haRestarted = true;
        } });

    // Configure MQTT client
    statusTopic = cfg.prefix + "/status";
    mqttClient.setServer(cfg.host.c_str(), cfg.port);
    mqttClient.setSocketTimeout(5);

    // Log initialization
    logger.info("MQTT initialized with broker: " + cfg.host + ":" + String(cfg.port));

    return true;
}

// Connect to MQTT broker
bool MQTTManager::connect()
{
    if (!mqttClientReady) {
        logger.error("MQTT client not initialized. Wait for init() first.");
        return false;
    }
    
    // A TLS session holds ~40 kB for as long as it is connected and needs
    // ~52 kB during the handshake; the rest of the system needs room next
    // to it (HeapGuard::MQTT_TLS_MIN_FREE). Without that much it would
    // fail or starve the web UI - do not start it; process() retries every
    // MQTT_RECONNECT_INTERVAL
    if (cfg.tls)
    {
        HeapGuard::Heap heap = HeapGuard::tlsHeap();
        if (!HeapGuard::mqttTlsFits(heap))
        {
            tlsLowFree = heap.free;
            tlsLowLargest = heap.largest;
            statusCode = Status::TlsLowMemory;
            if (!tlsLowLogged || millis() - tlsLowLoggedAt > TLS_LOW_LOG_INTERVAL)
            {
                tlsLowLogged = true;
                tlsLowLoggedAt = millis();
                logger.warning("MQTT TLS not connecting - not enough free memory: needs " +
                               String(HeapGuard::MQTT_TLS_MIN_FREE) + " B free with a block of " +
                               String(HeapGuard::TLS_MIN_BLOCK) + " B, has " + String(heap.free) +
                               " B free, largest block " + String(heap.largest) + " B");
            }
            return false;
        }
        if (tlsLowLogged)
        {
            tlsLowLogged = false;
            logger.info("MQTT TLS: enough free memory again, connecting");
        }
        if (!wifiClientSecure)
        {
            // Its constructor allocates the SSL context with a throwing new
            // (cannot fail after the check above unless the heap changes)
            try
            {
                wifiClientSecure.reset(new (std::nothrow) WiFiClientSecure());
            }
            catch (const std::bad_alloc &)
            {
                wifiClientSecure.reset();
            }
            if (!wifiClientSecure)
            {
                HeapGuard::Heap now = HeapGuard::tlsHeap();
                tlsLowFree = now.free;
                tlsLowLargest = now.largest;
                statusCode = Status::TlsLowMemory;
                logger.warning("MQTT TLS not connecting - the TLS client could not be allocated (" +
                               String(now.free) + " B free, largest block " + String(now.largest) + " B)");
                return false;
            }
            // Brokers on the LAN usually have self-signed certificates:
            // encrypted, but the server is not verified (README)
            wifiClientSecure->setInsecure();
            wifiClientSecure->setHandshakeTimeout(8);
            wifiClientSecure->setConnectionTimeout(5000);
        }
        mqttClient.setClient(*wifiClientSecure);
    }

    logger.debug("Attempting to connect to MQTT broker...");

    // DNS (e.g. homeassistant.local), TCP connect and TLS handshake can
    // together exceed the watchdog timeout
    WatchdogPause pause;

    // Last will: the broker publishes "offline" if the gateway disappears,
    // so Home Assistant marks its entities unavailable
    bool hasUser = cfg.user.length() > 0;
    bool connected = mqttClient.connect(clientId.c_str(),
                                        hasUser ? cfg.user.c_str() : nullptr,
                                        hasUser ? cfg.password.c_str() : nullptr,
                                        statusTopic.c_str(), 0, true, "offline");

    if (connected)
    {
        connectedWithTls = cfg.tls;
        statusCode = Status::Connected;
        logger.info("Connected to MQTT broker");

        // Home Assistant announces a restart here; discovery is resent then
        if (cfg.haEnabled)
        {
            mqttClient.subscribe((cfg.haPrefix + "/status").c_str());
        }

        // Publish discovery information after successful connection
        publishDiscovery();

        // TODO: Some better idea than magic number delay?
        // Give HA time to subscribe this topic
        delay(500);
        mqttClient.publish(statusTopic.c_str(), "online", true);
        publishGatewayState(true);
    }
    else
    {
        statusMqttState = mqttClient.state();
        statusCode = Status::Failed;
        logger.warning("Failed to connect to MQTT broker, error code: " + String(mqttClient.state()));
    }

    return connected;
}

// Process MQTT communication (call in main loop)
void MQTTManager::process()
{
    processPending();

    // Skip if MQTT is disabled in configuration
    if (!cfg.enabled || !mqttClientReady)
    {
        return;
    }

    // Update mode: off for the session (holdForUpdate)
    if (heldForUpdate)
    {
        disconnect();
        statusCode = Status::HeldForUpdate;
        return;
    }
    if (statusCode == Status::HeldForUpdate)
    {
        statusCode = Status::Waiting;
        lastReconnectAttempt = 0;
    }

    // The access point is the way in but failed to start for lack of
    // memory (Uplink::apStarved()): a TLS session holds ~40 kB - drop it and
    // hold it back while the AP retries, so the admin is not locked out
    if (cfg.tls && uplink->apStarved())
    {
        if (statusCode != Status::HeldForAp)
        {
            logger.warning("MQTT TLS held back: the access point needs the memory");
        }
        disconnect();
        statusCode = Status::HeldForAp;
        return;
    }
    if (statusCode == Status::HeldForAp)
    {
        statusCode = Status::Waiting;
        lastReconnectAttempt = 0; // the AP runs: connect right away
    }

    // Check network connection and MQTT broker
    if (uplink->isOnline())
    {
        if (!mqttClient.connected())
        {
            if (statusCode == Status::Connected)
            {
                statusCode = Status::Disconnected;
            }
            // Try to reconnect on interval
            unsigned long now = millis();
            if (now - lastReconnectAttempt > MQTT_RECONNECT_INTERVAL)
            {
                lastReconnectAttempt = now;
                if (connect())
                {
                    lastReconnectAttempt = 0;
                }
            }
        }
        else
        {
            // A write to a half-dead socket can block ~10 s (lwIP retries)
            WatchdogPause pause;
            // Process MQTT loop
            mqttClient.loop();
            publishGatewayState(false);

            // Discovery again when Home Assistant restarted, otherwise daily
            // (retained configs survive; hourly resends cost data on LTE)
            unsigned long now = millis();
            if (haRestarted || now - lastDiscoveryUpdate > 24UL * 3600 * 1000)
            {
                haRestarted = false;
                lastDiscoveryUpdate = now;
                publishDiscovery();
            }
        }
    }
}

// Publish discovery configuration for sensors
void MQTTManager::publishDiscovery()
{
    if (!cfg.haEnabled) {
        return; // Skip discovery if HA is disabled
    }

    // Skip if not connected
    if (!mqttClient.connected())
    {
        return;
    }

    logger.info("Publishing Home Assistant discovery information...");

    publishGatewayDiscovery();

    // Get all active sensors
    std::vector<SensorData> sensors = sensorManager.getActiveSensors();

    for (const auto &sensor : sensors)
    {
        publishSensorDiscovery(sensor);
    }

    logger.info("Home Assistant discovery completed for " + String(sensors.size()) + " sensors");
    lastDiscoveryUpdate = millis();
}

// Retained discovery message; a failure (payload over the buffer, socket
// error) is logged instead of silently missing entities in HA
bool MQTTManager::publishRetained(const String &topic, const String &payload)
{
    if (mqttClient.publish(topic.c_str(), payload.c_str(), true))
    {
        return true;
    }
    logger.warning("MQTT publish failed: " + topic + " (" + String(topic.length() + payload.length()) + " B)");
    return false;
}

// Discovery config of every value a sensor provides
void MQTTManager::publishSensorDiscovery(const SensorData &sensor)
{
    // Base state topic for this sensor
    String baseTopic = String(cfg.prefix) + "/" + formatSN(sensor.serialNumber);

    // Publish discovery for each supported value type based on sensor type
    if (sensor.hasTemperature())
    {
        String topic = buildDiscoveryTopic(sensor, "temperature");
        String payload = buildDiscoveryJson(sensor, "temperature", baseTopic + "/temperature");
        publishRetained(topic, payload); // Retained message
        logger.debug("Published temperature discovery for " + sensor.name);
    }

    if (sensor.hasHumidity())
    {
        String topic = buildDiscoveryTopic(sensor, "humidity");
        String payload = buildDiscoveryJson(sensor, "humidity", baseTopic + "/humidity");
        publishRetained(topic, payload);
        logger.debug("Published humidity discovery for " + sensor.name);
    }

    if (sensor.hasPressure())
    {
        String topic = buildDiscoveryTopic(sensor, "pressure");
        String payload = buildDiscoveryJson(sensor, "pressure", baseTopic + "/pressure");
        publishRetained(topic, payload);
        logger.debug("Published pressure discovery for " + sensor.name);
    }

    if (sensor.hasPPM())
    {
        String topic = buildDiscoveryTopic(sensor, "co2");
        String payload = buildDiscoveryJson(sensor, "co2", baseTopic + "/co2");
        publishRetained(topic, payload);
        logger.debug("Published CO2 discovery for " + sensor.name);
    }

    if (sensor.hasLux())
    {
        String topic = buildDiscoveryTopic(sensor, "illuminance");
        String payload = buildDiscoveryJson(sensor, "illuminance", baseTopic + "/illuminance");
        publishRetained(topic, payload);
        logger.debug("Published illuminance discovery for " + sensor.name);
    }

    if (sensor.hasWindSpeed())
    {
        String topic = buildDiscoveryTopic(sensor, "wind_speed");
        String payload = buildDiscoveryJson(sensor, "wind_speed", baseTopic + "/wind_speed");
        publishRetained(topic, payload);
        logger.debug("Published wind speed discovery for " + sensor.name);
    }

    if (sensor.hasWindDirection())
    {
        String topic = buildDiscoveryTopic(sensor, "wind_direction");
        String payload = buildDiscoveryJson(sensor, "wind_direction", baseTopic + "/wind_direction");
        publishRetained(topic, payload);
        logger.debug("Published wind direction discovery for " + sensor.name);
    }

    if (sensor.hasRainAmount())
    {
        String topic = buildDiscoveryTopic(sensor, "rain_amount");
        String payload = buildDiscoveryJson(sensor, "rain_amount", baseTopic + "/rain_amount");
        publishRetained(topic, payload);
        logger.debug("Published rain amount discovery for " + sensor.name);

        // Also for daily rain total
        topic = buildDiscoveryTopic(sensor, "daily_rain");
        payload = buildDiscoveryJson(sensor, "daily_rain", baseTopic + "/daily_rain");
        publishRetained(topic, payload);
        logger.debug("Published daily rain discovery for " + sensor.name);
    }

    if (sensor.hasRainRate())
    {
        String topic = buildDiscoveryTopic(sensor, "rain_rate");
        String payload = buildDiscoveryJson(sensor, "rain_rate", baseTopic + "/rain_rate");
        publishRetained(topic, payload);
        logger.debug("Published rain rate discovery for " + sensor.name);
    }

    // Battery voltage - available for all sensors
    String topic = buildDiscoveryTopic(sensor, "battery");
    String payload = buildDiscoveryJson(sensor, "battery", baseTopic + "/battery");
    publishRetained(topic, payload);
    logger.debug("Published battery discovery for " + sensor.name);

    // RSSI (signal strength) - available for all sensors
    topic = buildDiscoveryTopic(sensor, "rssi");
    payload = buildDiscoveryJson(sensor, "rssi", baseTopic + "/rssi");
    publishRetained(topic, payload);
    logger.debug("Published RSSI discovery for " + sensor.name);
}

String MQTTManager::gatewayTopic(const char *key) const
{
    return String(cfg.prefix) + "/gateway/" + key;
}

// One diagnostic sensor of the gateway device
void MQTTManager::publishGatewayEntity(const char *key, const char *name, const char *deviceClass,
                                       const char *unit, const char *icon)
{
    DynamicJsonDocument doc(768);
    doc["name"] = name;
    doc["state_topic"] = gatewayTopic(key);
    doc["unique_id"] = String(cfg.prefix) + "_" + gatewayId + "_" + key;
    doc["entity_category"] = "diagnostic";
    doc["availability_topic"] = String(cfg.prefix) + "/status";
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
    if (deviceClass)
    {
        doc["device_class"] = deviceClass;
    }
    if (unit)
    {
        doc["unit_of_measurement"] = unit;
    }
    if (icon)
    {
        doc["icon"] = icon;
    }

    JsonObject device = doc.createNestedObject("device");
    device["identifiers"] = gatewayId;
    device["name"] = gatewayId;
    device["model"] = BOARD_NAME;
    device["manufacturer"] = "expLORA";
    device["sw_version"] = FIRMWARE_VERSION;

    String payload;
    serializeJson(doc, payload);
    String topic = String(cfg.haPrefix) + "/sensor/" + String(cfg.prefix) + "_" +
                   gatewayId + "_" + key + "/config";
    publishRetained(topic, payload);
}

void MQTTManager::publishGatewayDiscovery()
{
    publishGatewayEntity("uplink", "Uplink", nullptr, nullptr, "mdi:lan-connect");
    publishGatewayEntity("ip", "IP address", nullptr, nullptr, "mdi:ip-network");
    publishGatewayEntity("time_sync", "Last time sync", "timestamp", nullptr, nullptr);
#if defined(HAS_PWR_SRC)
    publishGatewayEntity("power_source", "Power source", nullptr, nullptr, "mdi:power-plug");
#endif
#if defined(HAS_LTE)
    publishGatewayEntity("lte_signal", "LTE signal", "signal_strength", "dBm", nullptr);
    publishGatewayEntity("lte_state", "LTE state", nullptr, nullptr, "mdi:signal-cellular-3");
#endif
}

// Publish gateway state when it changed, or periodically
void MQTTManager::publishGatewayState(bool force)
{
    unsigned long now = millis();
    if (!force && now - lastGatewayCheck < GATEWAY_STATE_CHECK)
    {
        return;
    }
    lastGatewayCheck = now;

    String uplinkName = uplink->getActiveName();
    String ip = uplink->getLocalIP().toString();
    // ISO 8601 UTC for the HA timestamp class, "None" = unknown
    String timeSync = "None";
    time_t synced = uplink->getLastTimeSync();
    if (synced != 0)
    {
        struct tm utc;
        gmtime_r(&synced, &utc);
        char text[24];
        strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
        timeSync = text;
    }
    String state = uplinkName + "|" + ip + "|" + timeSync;
#if defined(HAS_PWR_SRC)
    String power = powerSource->getName();
    state += "|" + power;
#endif
#if defined(HAS_LTE)
    const LteModem &lte = uplink->getLte();
    int rssi = lte.getRssiDbm();
    // "None" is Home Assistant's payload for an unknown numeric state
    String lteSignal = rssi != 0 ? String(rssi) : String("None");
    String lteState = lte.getStateName();
    // Signal in 6 dB steps for change detection - CSQ jitters on every poll and
    // each publish costs data on a metered LTE link
    state += "|" + (rssi != 0 ? String(rssi / 6) : String("-")) + "|" + lteState;
#endif

    if (!force && state == lastGatewayState && now - lastGatewayPublish < GATEWAY_STATE_REFRESH)
    {
        return;
    }
    lastGatewayState = state;
    lastGatewayPublish = now;

    mqttClient.publish(gatewayTopic("uplink").c_str(), uplinkName.c_str(), true);
    mqttClient.publish(gatewayTopic("ip").c_str(), ip.c_str(), true);
    mqttClient.publish(gatewayTopic("time_sync").c_str(), timeSync.c_str(), true);
#if defined(HAS_PWR_SRC)
    mqttClient.publish(gatewayTopic("power_source").c_str(), power.c_str(), true);
#endif
#if defined(HAS_LTE)
    mqttClient.publish(gatewayTopic("lte_signal").c_str(), lteSignal.c_str(), true);
    mqttClient.publish(gatewayTopic("lte_state").c_str(), lteState.c_str(), true);
#endif
}

// Create discovery topic for sensor
String MQTTManager::buildDiscoveryTopic(const SensorData &sensor, const String &valueType)
{
    String deviceClass = valueType;

    // Map value type to Home Assistant device class
    // https://www.home-assistant.io/integrations/sensor/#device-class
    if (valueType == "co2")
        deviceClass = "carbon_dioxide";
    else if (valueType == "daily_rain")
        deviceClass = "precipitation";
    else if (valueType == "rain_amount")
        deviceClass = "precipitation";
    else if (valueType == "rain_rate")
        deviceClass = "precipitation_intensity";
    else if (valueType == "wind_speed")
        deviceClass = "wind_speed";
    else if (valueType == "wind_direction")
        deviceClass = "wind_direction";

    // Create discovery topic
    return String(cfg.haPrefix) + "/sensor/" +
           String(cfg.prefix) + "_" + formatSN(sensor.serialNumber) + "_" + valueType + "/config";
}

// Helper function to capitalize first letter
String MQTTManager::capitalizeFirst(const String &input)
{
    if (input.length() == 0)
        return input;
    String result = input;
    result.setCharAt(0, toupper(input.charAt(0)));
    return result;
}

// Create configuration JSON for Home Assistant discovery
String MQTTManager::buildDiscoveryJson(const SensorData &sensor, const String &valueType, const String &stateTopic)
{
    // Create JSON document for discovery payload
    DynamicJsonDocument doc(1024);

    // Entity name
    // doc["name"] = sensor.name + " " + capitalizeFirst(valueType);

    // Use the name from the sensor if it ends with the value type
    if (sensor.name.endsWith(capitalizeFirst(valueType)))
    {
        doc["name"] = sensor.name;
    }
    else
    {
        doc["name"] = capitalizeFirst(valueType); // Just the value type
    }

    // State topic
    doc["state_topic"] = stateTopic;

    // Value template - just use raw value
    doc["value_template"] = "{{ value }}";

    // Unique ID
    doc["unique_id"] = String(cfg.prefix) + "_" + formatSN(sensor.serialNumber) + "_" + valueType;

    // Availability topic - use LWT (Last Will and Testament)
    doc["availability_topic"] = String(cfg.prefix) + "/status";
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";

    // Set measurement units based on value type
    if (valueType == "temperature")
    {
        doc["device_class"] = "temperature";
        doc["unit_of_measurement"] = "°C";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "humidity")
    {
        doc["device_class"] = "humidity";
        doc["unit_of_measurement"] = "%";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "pressure")
    {
        doc["device_class"] = "pressure";
        doc["unit_of_measurement"] = "hPa";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "co2")
    {
        doc["device_class"] = "carbon_dioxide";
        doc["unit_of_measurement"] = "ppm";
    }
    else if (valueType == "illuminance")
    {
        doc["device_class"] = "illuminance";
        doc["unit_of_measurement"] = "lx";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "wind_speed")
    {
        doc["device_class"] = "wind_speed";
        doc["unit_of_measurement"] = "m/s";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "wind_direction")
    {
        doc["device_class"] = "wind_direction";
        doc["unit_of_measurement"] = "°";
    }
    else if (valueType == "rain_amount")
    {
        doc["device_class"] = "precipitation";
        doc["unit_of_measurement"] = "mm";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "daily_rain")
    {
        doc["device_class"] = "precipitation";
        doc["unit_of_measurement"] = "mm";
        doc["suggested_display_precision"] = 1;
        doc["name"] = sensor.name + " Daily Rain Total";
    }
    else if (valueType == "rain_rate")
    {
        doc["device_class"] = "precipitation_intensity";
        doc["unit_of_measurement"] = "mm/h";
        doc["suggested_display_precision"] = 1;
    }
    else if (valueType == "battery")
    {
        doc["device_class"] = "voltage";
        doc["unit_of_measurement"] = "V";
        doc["suggested_display_precision"] = 2;
    }
    else if (valueType == "rssi")
    {
        doc["device_class"] = "signal_strength";
        doc["unit_of_measurement"] = "dBm";
    }

    // Device information
    JsonObject device = doc.createNestedObject("device");
    device["identifiers"] = formatSN(sensor.serialNumber);
    device["name"] = sensor.name;
    device["model"] = sensor.getTypeInfo().name;
    device["manufacturer"] = "expLORA";

    // Serialize JSON to string
    String payload;
    serializeJson(doc, payload);
    return payload;
}

// Helper function to capitalize first letter
String capitalizeFirst(const String &input)
{
    if (input.length() == 0)
        return input;
    String result = input;
    result.setCharAt(0, toupper(input.charAt(0)));
    return result;
}

// Publish sensor data
void MQTTManager::publishSensorData(int sensorIndex)
{
    // Skip if not connected
    if (!mqttClient.connected() || !cfg.enabled)
    {
        return;
    }

    SensorData copy;
    if (!sensorManager.getSensorCopy(sensorIndex, copy))
    {
        return;
    }
    const SensorData *sensor = &copy;

    // Base topic for this sensor
    String baseTopic = String(cfg.prefix) + "/" + formatSN(sensor->serialNumber);

    // Publish each value based on sensor type
    if (sensor->hasTemperature())
    {
        mqttClient.publish((baseTopic + "/temperature").c_str(),
                           String(sensor->temperature, 2).c_str());
    }

    if (sensor->hasHumidity())
    {
        mqttClient.publish((baseTopic + "/humidity").c_str(),
                           String(sensor->humidity, 2).c_str());
    }

    if (sensor->hasPressure())
    {
        mqttClient.publish((baseTopic + "/pressure").c_str(),
                           String(sensor->pressure, 2).c_str());
    }

    if (sensor->hasPPM())
    {
        mqttClient.publish((baseTopic + "/co2").c_str(),
                           String(int(sensor->ppm)).c_str());
    }

    if (sensor->hasLux())
    {
        mqttClient.publish((baseTopic + "/illuminance").c_str(),
                           String(sensor->lux, 1).c_str());
    }

    if (sensor->hasWindSpeed())
    {
        mqttClient.publish((baseTopic + "/wind_speed").c_str(),
                           String(sensor->windSpeed, 1).c_str());
    }

    if (sensor->hasWindDirection())
    {
        mqttClient.publish((baseTopic + "/wind_direction").c_str(),
                           String(sensor->windDirection).c_str());
    }

    if (sensor->hasRainAmount())
    {
        mqttClient.publish((baseTopic + "/rain_amount").c_str(),
                           String(sensor->rainAmount, 1).c_str());
        mqttClient.publish((baseTopic + "/daily_rain").c_str(),
                           String(sensor->dailyRainTotal, 1).c_str());
    }

    if (sensor->hasRainRate())
    {
        mqttClient.publish((baseTopic + "/rain_rate").c_str(),
                           String(sensor->rainRate, 1).c_str());
    }

    // Battery voltage - available for all sensors
    mqttClient.publish((baseTopic + "/battery").c_str(),
                       String(sensor->batteryVoltage, 2).c_str());

    // RSSI - available for all sensors
    mqttClient.publish((baseTopic + "/rssi").c_str(),
                       String(sensor->rssi).c_str());

    logger.debug("Published MQTT data for sensor: " + sensor->name);
}

// Web handlers run in the async server task; PubSubClient is used only from
// the loop task, so they queue the work here and process() does it
static constexpr size_t MAX_PENDING_OPS = 32;

void MQTTManager::requestSensorDiscovery(uint32_t serialNumber)
{
    std::lock_guard<std::mutex> lock(pendingMutex);
    if (pending.size() < MAX_PENDING_OPS)
    {
        pending.push_back({PendingOp::Discovery, serialNumber});
    }
    else
    {
        logger.warning("MQTT queue full - discovery for " + formatSN(serialNumber) + " dropped");
    }
}

void MQTTManager::requestSensorRemoval(uint32_t serialNumber)
{
    std::lock_guard<std::mutex> lock(pendingMutex);
    if (pending.size() < MAX_PENDING_OPS)
    {
        pending.push_back({PendingOp::Removal, serialNumber});
    }
    else
    {
        logger.warning("MQTT queue full - removal for " + formatSN(serialNumber) + " dropped");
    }
}

void MQTTManager::requestReinit()
{
    std::lock_guard<std::mutex> lock(pendingMutex);
    reinitRequested = true;
}

void MQTTManager::processPending()
{
    bool reinit;
    std::vector<PendingOp> ops;
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        reinit = reinitRequested;
        reinitRequested = false;
        if (!cfg.enabled)
        {
            pending.clear(); // nothing to deliver to
        }
        else if (!reinit && mqttClient.connected())
        {
            ops.swap(pending); // kept queued until connected
        }
    }

    if (reinit)
    {
        logger.info("Reinitializing MQTT with new configuration...");
        disconnect();
        mqttClientReady = false;
        init();
        lastReconnectAttempt = 0;
        return; // full discovery follows the reconnect, queued ops after it
    }

    if (ops.empty())
    {
        return;
    }
    WatchdogPause pause; // publishes below can block on a stalled socket
    for (const PendingOp &op : ops)
    {
        if (op.type == PendingOp::Removal)
        {
            removeDiscoveryForSensor(op.serialNumber);
            continue;
        }
        if (!cfg.haEnabled)
        {
            continue;
        }
        for (const SensorData &sensor : sensorManager.getActiveSensors())
        {
            if (sensor.serialNumber == op.serialNumber)
            {
                logger.info("Publishing MQTT discovery for sensor: " + sensor.name);
                publishSensorDiscovery(sensor);
            }
        }
    }
}

void MQTTManager::holdForUpdate(bool hold)
{
    if (hold == heldForUpdate)
    {
        return;
    }
    heldForUpdate = hold;
    if (hold)
    {
        disconnect();
        if (cfg.enabled && mqttClientReady)
        {
            statusCode = Status::HeldForUpdate;
            logger.info("MQTT stopped for the update");
        }
    }
    else if (statusCode == Status::HeldForUpdate)
    {
        // Reconnect with the next process()
        statusCode = Status::Waiting;
        lastReconnectAttempt = 0;
    }
}

// Check connection
bool MQTTManager::isConnected()
{
    return mqttClient.connected() && cfg.enabled;
}

// Disconnect from MQTT broker
void MQTTManager::disconnect()
{
    if (mqttClient.connected())
    {
        WatchdogPause pause; // the socket may be half-dead (uplink just changed)
        logger.info("Disconnecting from MQTT broker");
        // A clean disconnect does not trigger the last will
        mqttClient.publish(statusTopic.c_str(), "offline", true);
        mqttClient.disconnect();
        if (statusCode == Status::Connected)
        {
            statusCode = Status::Disconnected;
        }
    }
}

const char *MQTTManager::statusName(Status s)
{
    switch (s)
    {
    case Status::Disabled:
        return "disabled";
    case Status::Waiting:
        return "waiting";
    case Status::Connected:
        return "connected";
    case Status::Failed:
        return "failed";
    case Status::Disconnected:
        return "disconnected";
    case Status::TlsLowMemory:
        return "tlsLowMemory";
    case Status::HeldForAp:
        return "heldForAp";
    case Status::HeldForUpdate:
        return "heldForUpdate";
    }
    return "unknown";
}

String MQTTManager::statusText() const
{
    String retry = " - retrying every " + String(MQTT_RECONNECT_INTERVAL / 1000) + " s";
    switch (statusCode.load())
    {
    case Status::Disabled:
        return "Disabled";
    case Status::Waiting:
        return "Not connected yet";
    case Status::Connected:
        return "Connected";
    case Status::Failed:
        return "Connection failed (error code " + String(statusMqttState.load()) + ")" + retry;
    case Status::Disconnected:
        return "Disconnected" + retry;
    case Status::TlsLowMemory:
    {
        String text = "Not connected: not enough free memory for TLS (needs " + String(HeapGuard::MQTT_TLS_MIN_FREE) +
                      " B free with a block of " + String(HeapGuard::TLS_MIN_BLOCK) + " B, had " +
                      String(tlsLowFree.load()) + " B free, largest block " + String(tlsLowLargest.load()) + " B)" + retry;
#if defined(HAS_WIFI_SWITCH)
        if (!uplink->wifiSwitchedOff())
        {
            text += ". Turn off WiFi (Network page) to free memory for TLS.";
        }
#endif
        return text;
    }
    case Status::HeldForUpdate:
        return "Not connected: stopped while the gateway updates (reconnects afterwards)";
    case Status::HeldForAp:
        return "Not connected: the access point could not start (" + String(uplink->apFailedStartFree()) +
               " B free memory) - the TLS session is held back to free memory for it (failed start " +
               String(uplink->apFailedStarts()) + " of at most " + String(Uplink::apStarvedMaxTries()) + ")";
    }
    return "Unknown";
}

// Remove discovery for deleted sensor
void MQTTManager::removeDiscoveryForSensor(uint32_t serialNumber)
{
    if (!cfg.haEnabled) {
        return; // Do not send discovery if HA is disabled
    }

    if (!mqttClient.connected())
    {
        return;
    }

    logger.info("Removing MQTT discovery for sensor with SN: " + formatSN(serialNumber));

    // Create discovery topics for all possible value types
    String baseDiscoveryTopic = String(cfg.haPrefix) + "/sensor/" + String(cfg.prefix) + "_" +
                                formatSN(serialNumber) + "_";

    // Remove discovery for all possible types
    String types[] = {"temperature", "humidity", "pressure", "co2", "illuminance",
                      "battery", "rssi", "wind_speed", "wind_direction",
                      "rain_amount", "daily_rain", "rain_rate"};

    for (const auto &type : types)
    {
        String topic = baseDiscoveryTopic + type + "/config";
        mqttClient.publish(topic.c_str(), "", true); // Empty payload deletes discovery
    }
}
