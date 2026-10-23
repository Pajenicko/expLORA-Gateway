/**
 * expLORA Gateway
 *
 * HTML content generator header file
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
#include <vector>
#include "../Data/SensorData.h"
#include "../Data/Gen2Sensor.h"
#include "../Protocol/WaitingList.h"
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"
#include "../Storage/OtaStore.h"
#include "../Protocol/OtaJob.h"

// OTA part of the generation 2 detail page (protocol v2, spec 14.5)
struct Gen2OtaView
{
    bool hasJob = false;
    OtaJob::Job job;
    bool imageOk = false;
    uint32_t imageFw = 0, imageLen = 0;
    String blockers, warnings, confirm;
    uint32_t estimateS = 0;
    OtaStore::Meta images[OtaImage::MAX_HW_TYPES];
    size_t imageCount = 0;
    uint32_t otaHourMs = 0; // OTA airtime of the sliding hour (radio task)
    String lastEvent;
    OtaJob::Now now;
};

/**
 * Class for generating HTML content
 *
 * Handles creation of HTML pages for web interface, optimized
 * for use with PSRAM (if available).
 */
class HTMLGenerator
{
private:

    // Add HTML opening code
    static void addHtmlHeader(String &html, const String &title, bool isAPMode = false, bool navigation = true);

    // Add HTML closing code
    static void addHtmlFooter(String &html, bool isAPMode = false);

    // Add CSS for pages
    static void addStyles(String &html);

    // Add JavaScript for interactive elements
    static void addJavaScript(String &html);

    // Add navigation
    static void addNavigation(String &html, const String &activePage);

public:
    // Escape text for HTML content and quoted attributes
    static String htmlEscape(const String &text);

    // Generate home page
    static String generateHomePage(const std::vector<SensorData> &sensors);

    // Generate configuration page
    static void addWiFiForm(String &html, const String &ssid, const String &password, const String &timezone);
    static String generateConfigPage(const String &ssid, const String &password, bool configMode, const String &ip, const String &timezone);

    // Generate MQTT settings page
    static String generateMqttPage(const String &host, int port, const String &user, const String &password, bool enabled, bool tls,
                                    const String &prefix, bool haEnabled, String &haPrefix);

    // Login and admin password pages
    static String generateLoginPage(bool hasPassword, const String &error, const String &next);
    static String generatePasswordPage(bool hasPassword, const String &error, bool apMode);

    // Generating network page (uplink status, WiFi, ethernet and LTE settings)
    static String generateNetworkPage(const ConfigManager &config, const String &ssid, const String &password,
                                      const String &timezone, bool apMode);

    // Generate sensor list page
    static String generateSensorsPage(const std::vector<ActiveSensorEntry>& sensors,
                                      const std::vector<Gen2Sensor> &gen2 = {},
                                      const WaitingEntry *waiting = nullptr, size_t waitingCount = 0);

    // Add a generation 2 sensor (DevID + label key)
    static String generateGen2AddPage(const String &devId, const String &error);

    // Generation 2 sensor detail: status, configuration form, status request,
    // downlink counters, devNonce reset
    static String generateGen2DetailPage(const Gen2Sensor &sensor, const String &message,
                                         const Gen2OtaView *ota = nullptr);

    // Generate sensor add page
    static String generateSensorAddPage();

    // Generate sensor edit page
    static String generateSensorEditPage(const SensorData &sensor, int index);

    // Generate logs page
    // Logs page in pieces, streamed by the web server (see handleLogs)
    static String generateLogsPageHead(LogLevel currentLevel);
    static String generateLogEntry(const LogEntry &entry);
    static String generateLogsPageTail();

    // Generate API page
    static String generateAPIPage(const std::vector<SensorData> &sensors);

    // Generate JSON for API
    // JSON document for /api (serialized by the caller into a sized buffer)
    static DynamicJsonDocument buildAPIJson(const std::vector<SensorData> &sensors);

    // Generate firmware update page
    static String generateFirmwarePage();

    // Optimized versions using buffer

    // Additional helper methods
    static String getWifiNetworkOptions(const String &currentSSID);
    static String getSensorTypeOptions(SensorType currentType);
    static String getLogLevelOptions(LogLevel currentLevel);
};
