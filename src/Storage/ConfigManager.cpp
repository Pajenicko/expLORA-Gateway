/**
 * expLORA Gateway
 *
 * Configuration manager implementation file
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

#include "ConfigManager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "../Hardware/FlashPermit.h"
#include "../Data/TimeZones.h"

// Constructor
ConfigManager::ConfigManager(Logger &log, const char *file)
    : logger(log), preferencesInitialized(false), configFile(file)
{

    // Default values
    resetToDefaults();
}

// Destructor
ConfigManager::~ConfigManager()
{
    // End Preferences if initialized
    if (preferencesInitialized)
    {
        preferences.end();
    }
}

// Initialization
bool ConfigManager::init()
{
    // Initialize Preferences (creates the namespace on the first boot)
    FlashWriteGuard flashGuard; // see Hardware/FlashPermit.h
    preferencesInitialized = preferences.begin("sverio", false);

    if (!preferencesInitialized)
    {
        logger.error("Failed to initialize Preferences");
    }
    else
    {
        logger.debug("Preferences initialized successfully");
    }

    // Load configuration
    return load();
}

// Load configuration from both sources
bool ConfigManager::load()
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    bool fsSuccess = loadFromFS();
    bool prefSuccess = loadFromPreferences();
    if (resolveTimezone() && (fsSuccess || prefSuccess))
    {
        logger.info("Time zone setting updated to " + (timezoneName.length() ? timezoneName : String("custom")) + ", saving");
        save();
    }

    if (fsSuccess)
    {
        logger.info("Configuration loaded from file system");
    }

    if (prefSuccess)
    {
        logger.info("Persistent configuration loaded from Preferences");
    }

    return fsSuccess || prefSuccess;
}

// Save configuration to both sources
bool ConfigManager::save()
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    bool fsSuccess = saveToFS();
    bool prefSuccess = saveToPreferences();

    if (fsSuccess)
    {
        logger.info("Configuration saved to file system");
    }
    else
    {
        logger.error("Failed to save configuration to file system");
    }

    if (prefSuccess)
    {
        logger.info("Persistent configuration saved to Preferences");
    }
    else
    {
        logger.error("Failed to save configuration to Preferences");
    }

    return fsSuccess && prefSuccess;
}

// Loading from LittleFS
bool ConfigManager::loadFromFS()
{
    FlashWriteGuard flashGuard; // see Hardware/FlashPermit.h
    // Power lost between removing the old file and renaming the new one
    String tmpFile = String(configFile) + ".tmp";
    if (!LittleFS.exists(configFile) && LittleFS.exists(tmpFile))
    {
        LittleFS.rename(tmpFile, configFile);
    }

    // Check if file exists
    if (!LittleFS.exists(configFile))
    {
        logger.warning("Config file not found: " + String(configFile));
        return false;
    }

    // Open file
    File file = LittleFS.open(configFile, "r");
    if (!file)
    {
        logger.error("Failed to open config file for reading: " + String(configFile));
        return false;
    }

    // (never logged: it holds the WiFi and MQTT passwords)
    // Create JSON document
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error)
    {
        logger.error("Failed to parse config file: " + String(error.c_str()));
        return false;
    }

    // Load values and log them
    wifiSSID = doc["ssid"].as<String>();
    wifiPassword = doc["password"].as<String>();
    configMode = doc["configMode"] | true;
    timezone = doc["timezone"] | DEFAULT_TIMEZONE;
    timezoneName = doc["tzName"] | "";
    timezoneNamePosix = doc["tzNamePosix"] | "";

    // MQTT configuration
    mqttHost = doc["mqttHost"] | MQTT_DEFAULT_HOST;
    mqttPort = doc["mqttPort"] | MQTT_DEFAULT_PORT;
    mqttUser = doc["mqttUser"] | MQTT_DEFAULT_USER;
    mqttPassword = doc["mqttPassword"] | MQTT_DEFAULT_PASS;
    mqttEnabled = doc["mqttEnabled"] | MQTT_DEFAULT_ENABLED;
    mqttTls = doc["mqttTls"] | MQTT_DEFAULT_TLS;
    mqttPrefix = doc["mqttPrefix"] | MQTT_DEFAULT_PREFIX;
    mqttHAPrefix = doc["mqttHAPrefix"] | HA_DISCOVERY_DEFAULT_PREFIX;
    mqttHAEnabled = doc["mqttHAEnabled"] | HA_DISCOVERY_DEFAULT_ENABLED;

    logger.debug("Loaded config - SSID: " + wifiSSID +
                 ", Password length: " + String(wifiPassword.length()) +
                 ", configMode: " + String(configMode ? "true" : "false"));

    // Set log level if available
    if (doc.containsKey("logLevel"))
    {
        String logLevelStr = doc["logLevel"].as<String>();
        logLevel = logger.levelFromString(logLevelStr);
    }

    return true;
}

// Saving to LittleFS
// Written to a temporary file and renamed, like sensors.json
bool ConfigManager::saveToFS()
{
    DynamicJsonDocument doc(1024);
    doc["ssid"] = wifiSSID;
    doc["password"] = wifiPassword;
    doc["configMode"] = configMode;
    doc["logLevel"] = logger.levelToString(logLevel);
    doc["timezone"] = timezone;
    doc["tzName"] = timezoneName;
    doc["tzNamePosix"] = timezoneNamePosix;
    doc["mqttHost"] = mqttHost;
    doc["mqttPort"] = mqttPort;
    doc["mqttUser"] = mqttUser;
    doc["mqttPassword"] = mqttPassword;
    doc["mqttEnabled"] = mqttEnabled;
    doc["mqttTls"] = mqttTls;
    doc["mqttPrefix"] = mqttPrefix;
    doc["mqttHAEnabled"] = mqttHAEnabled;
    doc["mqttHAPrefix"] = mqttHAPrefix;
    if (doc.overflowed())
    {
        logger.error("Configuration does not fit the JSON document - not saved");
        return false;
    }

    String tmpFile = String(configFile) + ".tmp";
    FlashWriteGuard flashGuard; // see Hardware/FlashPermit.h
    File file = LittleFS.open(tmpFile, "w");
    if (!file)
    {
        logger.error("Failed to open " + tmpFile + " for writing");
        return false;
    }
    size_t expected = measureJson(doc);
    size_t written = serializeJson(doc, file);
    file.close();
    if (written == 0 || written != expected)
    {
        logger.error("Failed to write config to file");
        LittleFS.remove(tmpFile);
        return false;
    }
    LittleFS.remove(configFile);
    if (!LittleFS.rename(tmpFile, configFile))
    {
        logger.error("Failed to replace " + String(configFile));
        return false;
    }
    return true;
}

// Loading from Preferences
bool ConfigManager::loadFromPreferences()
{
    if (!preferencesInitialized)
    {
        return false;
    }

    // Load basic configuration
    // Note: We let Preferences load only logLevel,
    // which we really need to preserve during firmware updates

    if (preferences.isKey("logLevel"))
    {
        String logLevelStr = preferences.getString("logLevel", "INFO");
        logLevel = logger.levelFromString(logLevelStr);
    }

    if (preferences.isKey("timezone"))
    {
        timezone = preferences.getString("timezone", DEFAULT_TIMEZONE);
    }
    if (preferences.isKey("tzName"))
    {
        timezoneName = preferences.getString("tzName", "");
    }
    if (preferences.isKey("tzNamePosix"))
    {
        timezoneNamePosix = preferences.getString("tzNamePosix", "");
    }

    // MQTT configuration from preferences
    if (preferences.isKey("mqttHost"))
    {
        mqttHost = preferences.getString("mqttHost", MQTT_DEFAULT_HOST);
    }
    if (preferences.isKey("mqttPort"))
    {
        mqttPort = preferences.getInt("mqttPort", MQTT_DEFAULT_PORT);
    }
    if (preferences.isKey("mqttUser"))
    {
        mqttUser = preferences.getString("mqttUser", MQTT_DEFAULT_USER);
    }
    if (preferences.isKey("mqttPassword"))
    {
        mqttPassword = preferences.getString("mqttPassword", MQTT_DEFAULT_PASS);
    }
    // Only stored keys override config.json (erased NVS must not reset them)
    if (preferences.isKey("mqttEnabled"))
    {
        mqttEnabled = preferences.getBool("mqttEnabled", MQTT_DEFAULT_ENABLED);
    }
    if (preferences.isKey("mqttTls"))
    {
        mqttTls = preferences.getBool("mqttTls", MQTT_DEFAULT_TLS);
    }
    if (preferences.isKey("mqttPrefix"))
    {
        mqttPrefix = preferences.getString("mqttPrefix", MQTT_DEFAULT_PREFIX);
    }
    if (preferences.isKey("mqttHAEnabled"))
    {
        mqttHAEnabled = preferences.getBool("mqttHAEnabled", HA_DISCOVERY_DEFAULT_ENABLED);
    }
    if (preferences.isKey("mqttHAPrefix"))
    {
        mqttHAPrefix = preferences.getString("mqttHAPrefix", HA_DISCOVERY_DEFAULT_PREFIX);
    }

#if defined(HAS_WIFI_SWITCH)
    wifiEnabled = preferences.getBool("wifiEnabled", WIFI_DEFAULT_ENABLED);
#endif
    lteEnabled = preferences.getBool("lteEnabled", LTE_DEFAULT_ENABLED);
    lteApn = preferences.getString("lteApn", LTE_DEFAULT_APN);
    ltePin = preferences.getString("ltePin", "");
    ltePinRejected = preferences.getBool("ltePinBad", false);

    adminPasswordHash = preferences.getString("adminPwd", "");
    signedOnly = preferences.getBool("signedOnly", false);

    ethStatic = preferences.getBool("ethStatic", false);
    ethIp = preferences.getString("ethIp", "");
    ethMask = preferences.getString("ethMask", "");
    ethGateway = preferences.getString("ethGateway", "");
    ethDns1 = preferences.getString("ethDns1", "");
    ethDns2 = preferences.getString("ethDns2", "");

    // Other values should be loaded from LittleFS, but if needed,
    // we can load them from Preferences as a backup solution
    if (wifiSSID.length() == 0 && preferences.isKey("ssid"))
    {
        wifiSSID = preferences.getString("ssid", "");
        wifiPassword = preferences.getString("password", "");
        configMode = preferences.getBool("configMode", true);
    }

    return true;
}

// Saving to Preferences
bool ConfigManager::saveToPreferences()
{
    FlashWriteGuard flashGuard; // see Hardware/FlashPermit.h
    if (!preferencesInitialized)
    {
        return false;
    }

    // Save logging level
    String logLevelStr = logger.levelToString(logLevel);
    preferences.putString("logLevel", logLevelStr);
    preferences.putString("timezone", timezone);
    preferences.putString("tzName", timezoneName);
    preferences.putString("tzNamePosix", timezoneNamePosix);

    // Save MQTT configuration
    preferences.putString("mqttHost", mqttHost);
    preferences.putInt("mqttPort", mqttPort);
    preferences.putString("mqttUser", mqttUser);
    preferences.putString("mqttPassword", mqttPassword);
    preferences.putBool("mqttEnabled", mqttEnabled);
    preferences.putBool("mqttTls", mqttTls);
    preferences.putString("mqttPrefix", mqttPrefix);
    preferences.putBool("mqttHAEnabled", mqttHAEnabled);
    preferences.putString("mqttHAPrefix", mqttHAPrefix);

#if defined(HAS_WIFI_SWITCH)
    preferences.putBool("wifiEnabled", wifiEnabled);
#endif
    preferences.putBool("signedOnly", signedOnly);
    preferences.putBool("lteEnabled", lteEnabled);
    preferences.putString("lteApn", lteApn);
    preferences.putString("ltePin", ltePin);

    preferences.putBool("ethStatic", ethStatic);
    preferences.putString("ethIp", ethIp);
    preferences.putString("ethMask", ethMask);
    preferences.putString("ethGateway", ethGateway);
    preferences.putString("ethDns1", ethDns1);
    preferences.putString("ethDns2", ethDns2);

    // Save WiFi configuration (as backup)
    preferences.putString("ssid", wifiSSID);
    preferences.putString("password", wifiPassword);
    preferences.putBool("configMode", configMode);

    return true;
}

// Add a new method to set MQTT configuration
bool ConfigManager::setWifiEnabled(bool enabled, bool saveConfig)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    wifiEnabled = enabled;
    if (saveConfig)
    {
        return save();
    }
    return true;
}

bool ConfigManager::setSignedOnly(bool on)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    // The option lives only in Preferences (not in config.json): one key is
    // written, read back, and RAM changes only when it is on flash - a failed
    // save never leaves RAM and flash disagreeing
    if (!preferencesInitialized)
    {
        return false;
    }
    if (preferences.putBool("signedOnly", on) == 0 || preferences.getBool("signedOnly", !on) != on)
    {
        logger.error("Signed images only: the setting could not be saved");
        return false;
    }
    signedOnly = on;
    return true;
}

bool ConfigManager::setLteConfig(bool enabled, const String &apn, const String &pin, bool saveConfig)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    // PIN: empty or 4-8 digits
    bool pinOk = pin.length() == 0 || (pin.length() >= 4 && pin.length() <= 8);
    for (size_t i = 0; pinOk && i < pin.length(); i++)
    {
        pinOk = isDigit(pin[i]);
    }
    if (!pinOk)
    {
        logger.error("Invalid SIM PIN - 4 to 8 digits");
        return false;
    }
    // APN: letters, digits, '.', '-' and '_' (it goes into an AT command)
    if (apn.length() > 63)
    {
        logger.error("APN too long");
        return false;
    }
    for (size_t i = 0; i < apn.length(); i++)
    {
        char c = apn[i];
        if (!isAlphaNumeric(c) && c != '.' && c != '-' && c != '_')
        {
            logger.error("Invalid APN character");
            return false;
        }
    }

    if (pin != ltePin)
    {
        setLtePinRejected(false);
    }
    lteEnabled = enabled;
    lteApn = apn;
    ltePin = pin;

    if (saveConfig)
    {
        return save();
    }
    return true;
}

// Contiguous netmask, at least /1 and at most /30
static bool isValidNetmask(const IPAddress &mask)
{
    uint32_t m = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) | ((uint32_t)mask[2] << 8) | mask[3];
    uint32_t inverted = ~m;
    return m != 0 && (inverted & (inverted + 1)) == 0 && inverted >= 3;
}

// Dotted IPv4 only (fromString() also accepts IPv6); empty input gives ""
static bool parseIPv4(const String &text, String &normalized)
{
    IPAddress addr;
    normalized = "";
    if (text.length() == 0)
    {
        return true;
    }
    if (!addr.fromString(text) || addr.type() != IPv4 || addr == IPAddress())
    {
        return false;
    }
    normalized = addr.toString();
    return true;
}

String ConfigManager::setEthernetConfig(bool useStatic, const String &ip, const String &mask, const String &gateway,
                                        const String &dns1, const String &dns2, bool saveConfig)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    String nIp, nMask, nGateway, nDns1, nDns2;
    bool ipOk = parseIPv4(ip, nIp);
    bool maskOk = parseIPv4(mask, nMask);
    bool gatewayOk = parseIPv4(gateway, nGateway);
    bool dns1Ok = parseIPv4(dns1, nDns1);
    bool dns2Ok = parseIPv4(dns2, nDns2);

    if (useStatic)
    {
        IPAddress a, m, g;
        if (!ipOk || nIp.length() == 0)
        {
            return "Invalid IP address";
        }
        m.fromString(nMask);
        if (!maskOk || nMask.length() == 0 || !isValidNetmask(m))
        {
            return "Invalid subnet mask";
        }
        if (!gatewayOk || nGateway.length() == 0)
        {
            return "Invalid gateway";
        }
        a.fromString(nIp);
        g.fromString(nGateway);
        for (int i = 0; i < 4; i++)
        {
            if ((a[i] & m[i]) != (g[i] & m[i]))
            {
                return "Gateway is not in the same subnet as the IP address";
            }
        }
        if (a == g)
        {
            return "IP address and gateway must differ";
        }
        if (!dns1Ok)
        {
            return "Invalid primary DNS";
        }
        if (!dns2Ok)
        {
            return "Invalid secondary DNS";
        }
    }

    ethStatic = useStatic;
    // With DHCP the form fields are disabled and not sent; keep the stored
    // addresses so switching back is easy, but never store invalid text
    if (useStatic || ip.length() > 0)
    {
        ethIp = ipOk ? nIp : "";
        ethMask = maskOk ? nMask : "";
        ethGateway = gatewayOk ? nGateway : "";
        ethDns1 = dns1Ok ? nDns1 : "";
        ethDns2 = dns2Ok ? nDns2 : "";
    }

    if (saveConfig && !save())
    {
        return "Saving configuration failed";
    }
    return "";
}

void ConfigManager::setLtePinRejected(bool rejected)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    ltePinRejected = rejected;
    if (preferencesInitialized)
    {
        preferences.putBool("ltePinBad", rejected);
    }
}

bool ConfigManager::setAdminPasswordHash(const String &hash)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    if (!preferencesInitialized)
    {
        return false;
    }
    if (hash.length() == 0)
    {
        if (preferences.isKey("adminPwd") && !preferences.remove("adminPwd"))
        {
            return false;
        }
        adminPasswordHash = "";
        return true;
    }
    // RAM follows only a successful write (a fresh device must not end up
    // with a password in RAM that is gone after a restart)
    if (preferences.putString("adminPwd", hash) != hash.length())
    {
        return false;
    }
    adminPasswordHash = hash;
    return true;
}

bool ConfigManager::setMqttConfig(const String &host, int port, const String &user,
                                  const String &password, bool enabled, bool tls,
                                  const String &rootPrefix, const String &haPrefix, bool haEnable,
                                  bool saveConfig)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    // Validate MQTT topic format (basic validation)
    // A prefix stored by an older version (e.g. with '/') may be kept as it is
    bool rootOk = isValidMqttTopic(rootPrefix) || rootPrefix == mqttPrefix;
    bool haOk = isValidMqttTopic(haPrefix) || haPrefix == mqttHAPrefix;
    if (!rootOk || !haOk)
    {
        logger.error("Invalid MQTT topic format");
        logger.verbose("Root topic: " + rootPrefix);
        logger.verbose("root check: " + String(isValidMqttTopic(rootPrefix)));
        logger.verbose("HA topic: " + haPrefix);
        logger.verbose("HA check: " + String(isValidMqttTopic(haPrefix)));
        return false;
    }

    // Kept to roll back: RAM must not show settings that are not saved
    String oldHost = mqttHost, oldUser = mqttUser, oldPassword = mqttPassword;
    String oldPrefix = mqttPrefix, oldHAPrefix = mqttHAPrefix;
    int oldPort = mqttPort;
    bool oldEnabled = mqttEnabled, oldTls = mqttTls, oldHAEnabled = mqttHAEnabled;

    mqttHost = host;
    mqttPort = port;
    mqttUser = user;
    mqttPassword = password;
    mqttEnabled = enabled;
    mqttTls = tls;
    mqttPrefix = rootPrefix;
    mqttHAPrefix = haPrefix;
    mqttHAEnabled = haEnable;

    if (saveConfig && !save())
    {
        mqttHost = oldHost;
        mqttPort = oldPort;
        mqttUser = oldUser;
        mqttPassword = oldPassword;
        mqttEnabled = oldEnabled;
        mqttTls = oldTls;
        mqttPrefix = oldPrefix;
        mqttHAPrefix = oldHAPrefix;
        mqttHAEnabled = oldHAEnabled;
        saveToPreferences(); // save() may have written the new values there
        return false;
    }
    return true;
}

// Helper function to validate MQTT topic format
bool ConfigManager::isValidMqttTopic(const String &topic)
{
    // Basic MQTT topic validation
    // Short: topics and discovery payloads must fit the 1024 B MQTT buffer
    if (topic.length() == 0 || topic.length() > 32)
        return false;
    
    // Check for invalid characters
    if (topic.indexOf('#') != -1 || topic.indexOf('+') != -1)
        return false;

    // One level only: the prefix is also part of discovery node and unique ids
    if (topic.indexOf('/') != -1)
        return false;

    return true;
}

// Reset configuration to default values
void ConfigManager::resetToDefaults()
{
    wifiSSID = "";
    wifiPassword = "";
    configMode = true;
    lastWifiAttempt = 0;
    logLevel = LogLevel::INFO;
    timezone = DEFAULT_TIMEZONE;
    timezoneName = TimeZones::DEFAULT_NAME;
    timezoneNamePosix = DEFAULT_TIMEZONE;
    mqttHost = MQTT_DEFAULT_HOST;
    mqttPort = MQTT_DEFAULT_PORT;
    mqttUser = MQTT_DEFAULT_USER;
    mqttPassword = MQTT_DEFAULT_PASS;
    mqttEnabled = MQTT_DEFAULT_ENABLED;
    mqttTls = MQTT_DEFAULT_TLS;
    mqttPrefix = MQTT_DEFAULT_PREFIX;
    mqttHAEnabled = HA_DISCOVERY_DEFAULT_ENABLED;
    wifiEnabled = WIFI_DEFAULT_ENABLED;
    signedOnly = false;
    lteEnabled = LTE_DEFAULT_ENABLED;
    lteApn = LTE_DEFAULT_APN;
    ltePin = "";
    ltePinRejected = false;
    ethStatic = false;
    ethIp = "";
    ethMask = "";
    ethGateway = "";
    ethDns1 = "";
    ethDns2 = "";
    mqttHAPrefix = HA_DISCOVERY_DEFAULT_PREFIX;
}

// Get firmware version
String ConfigManager::getFirmwareVersion() const
{
    return FIRMWARE_VERSION;
}

// Get device MAC address
String ConfigManager::getMacAddress() const
{
    uint8_t mac[6];
    getStationMac(mac);

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return String(macStr);
}

// Get device name for AP mode
String ConfigManager::getDeviceName() const
{
    uint8_t mac[6];
    getStationMac(mac);

    String deviceName = "expLORA-GW-";
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] < 16)
            deviceName += "0";
        deviceName += String(mac[i], HEX);
    }

    String result = deviceName;
    result.toUpperCase();
    return result;
}

// Set WiFi configuration
bool ConfigManager::setWiFiConfig(const String &ssid, const String &password, bool saveConfig)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    wifiSSID = ssid;
    wifiPassword = password;
    configMode = false;
    lastWifiAttempt = 0;

    if (saveConfig)
    {
        return save();
    }

    return true;
}

// Settings from before the time zone list hold only a POSIX string: it maps
// to the closest entry or stays custom. A known name takes the table's
// string, so corrected rules reach stored settings, unless an older firmware
// changed the string after the name (see TimeZones::resolve). true = the
// setting changed and should be saved.
bool ConfigManager::resolveTimezone()
{
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    String oldName = timezoneName, oldPosix = timezone, oldNamePosix = timezoneNamePosix;
    int index = TimeZones::resolve(timezoneName.c_str(), timezone.c_str(), timezoneNamePosix.c_str());
    setTimezoneFields(index);
    return timezoneName != oldName || timezone != oldPosix || timezoneNamePosix != oldNamePosix;
}

// Name, POSIX string and the string written with the name always together;
// -1 = keep the POSIX string as a custom zone
void ConfigManager::setTimezoneFields(int zoneIndex)
{
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    if (zoneIndex >= 0)
    {
        timezoneName = TimeZones::ZONES[zoneIndex].name;
        timezone = TimeZones::ZONES[zoneIndex].posix;
        timezoneNamePosix = timezone;
    }
    else
    {
        timezoneName = "";
        timezoneNamePosix = "";
    }
}

bool ConfigManager::setTimezone(int zoneIndex)
{
    if (zoneIndex < 0 || zoneIndex >= (int)TimeZones::COUNT)
    {
        return false;
    }
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    String oldName = timezoneName, oldPosix = timezone, oldNamePosix = timezoneNamePosix;
    setTimezoneFields(zoneIndex);
    if (!save())
    {
        // One store may hold the new zone: write the old one to both, so
        // they agree with what the page reports (the previous zone)
        timezoneName = oldName;
        timezone = oldPosix;
        timezoneNamePosix = oldNamePosix;
        save();
        return false;
    }
    return true;
}

// Switch to configuration mode (AP)
void ConfigManager::enableConfigMode(bool enable, bool saveConfig)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    // Called on every boot - only write flash when it changes
    if (configMode == enable)
    {
        return;
    }
    configMode = enable;

    if (saveConfig)
    {
        save();
    }
}

// Set logging level
void ConfigManager::setLogLevel(LogLevel level, bool saveConfig)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configMutex);
    logLevel = level;

    // Update level in logger
    logger.setLogLevel(level);

    if (saveConfig)
    {
        save();
    }
}
