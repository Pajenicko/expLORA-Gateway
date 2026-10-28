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
#include "../Update/GatewayOta.h"
#include "../Update/Manifest.h"
#include "../Update/AutoUpdate.h"

// The update check on the Firmware page (Update/UpdateCheck.h): the
// accepted manifest of this run, copied under its lock (the last result,
// time and seq come from GatewayOta::View)
struct UpdateCheckView
{
    bool have = false; // a manifest was accepted in this run
    uint32_t keyId = 0;
    bool hasGateway = false;
    uint32_t gatewayVersion = 0;
    Manifest::Offer gatewayOffer = Manifest::Offer::None;
    struct Sensor
    {
        uint16_t hw;
        uint32_t offered;
        bool stored;
        uint32_t storedFw;
        Manifest::Offer offer;
    } sensor[Manifest::MAX_PACKAGES];
    uint8_t sensors = 0;
};

// OTA part of the generation 2 detail page (protocol v2, spec 14.5)
struct Gen2OtaView
{
    bool hasJob = false;
    OtaJob::Job job;
    bool imageOk = false;
    uint32_t imageFw = 0, imageLen = 0;
    String blockers, warnings, confirm;
    uint32_t confirmToken = 0; // the "I confirm" box carries it (bound to these reasons and this image)
    uint32_t estimateS = 0;
    OtaStore::Meta images[OtaImage::MAX_HW_TYPES];
    size_t imageCount = 0;
    bool signedOnly = false; // "Signed images only" (Firmware page)
    uint32_t otaHourMs = 0; // OTA airtime of the sliding hour (radio task)
    String lastEvent;
    String fetchResult; // last sensor image download (POST /ota/fetch)
    OtaJob::Now now;
};

// Settings shown on the network page, copied under the config mutex before
// the page is streamed (its pieces are generated later, one per callback of
// the response, in the AsyncTCP task)
struct NetworkPageView
{
    bool apMode = false;
    String ssid;
    bool hasWifiPassword = false;
    String timezone, timezoneName;
    bool ethStatic = false;
    String ethIp, ethMask, ethGateway, ethDns1, ethDns2;
    bool wifiEnabled = true;
    bool viaWifi = false; // this page was requested over WiFi (station or AP)
    String ethAddress;    // current Ethernet address, empty without one
    bool lteEnabled = false;
    String lteApn;
    bool hasLtePin = false;
};

// System page: full = also the interface, time zone and restart cards
struct SystemView
{
    bool full = true;
    String timezone, timezoneName;
};

// Sensor list page data, copied before the page is streamed (see
// handleSensors): keys wiped, only what the page shows
struct SensorsPageView
{
    std::vector<ActiveSensorEntry> entries;
    std::vector<Gen2Sensor> gen2;
    WaitingEntry waiting[8];
    size_t waitingCount = 0;
    unsigned long nowMillis = 0; // one 'now' for every row
    bool adv = false;            // Advanced mode (one value for every part)
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

public:
    // Navigation entry a page belongs to (highlighted by route)
    enum class Page : uint8_t
    {
        None,
        Home,
        Sensors,
        Network,
        Mqtt,
        Firmware,
        Logs,
        Api,
        System,
        Config
    };

private:
    // Head, header and navigation; refreshS: reload after that many seconds
    static void addHtmlHeader(String &html, const String &title, Page page, bool isAPMode = false,
                              bool navigation = true, uint16_t refreshS = 0);
    // </main>, footer, the shared script
    static void addHtmlFooter(String &html);
    static void addNavigation(String &html, Page active, bool apMode);

public:
    // Web interface mode (ConfigManager::uiAdvanced), cached here so no
    // configuration lock is taken while a page is generated
    static void setAdvanced(bool advanced);
    static bool adv();

    // Escape text for HTML content and quoted attributes
    static String htmlEscape(const String &text);

    // Generate home page
    // sensors: SensorManager::getReadings() (generation 1 and 2)
    static String generateHomePage(const std::vector<SensorData> &sensors, const String &mqttStatus);

    // Generate configuration page
    // timeZone: also the time zone select (AP setup page), nullptr = without
    static void addWiFiForm(String &html, const String &ssid, bool hasPassword, const ConfigManager *timeZone);
    static void addTimezoneSelect(String &html, const String &zoneName, const String &zonePosix);
    static String generateConfigPage(const String &ssid, const String &password, bool configMode, const String &ip,
                                     const ConfigManager &config);

    // Generate MQTT settings page
    // status: connection state with the reason when not connected
    static String generateMqttPage(const String &host, int port, const String &user, const String &password, bool enabled, bool tls,
                                    const String &prefix, bool haEnabled, String &haPrefix, const String &status);

    // Login and admin password pages
    static String generateLoginPage(bool hasPassword, const String &error, const String &next);
    // System page (/password): interface mode, time zone, admin password,
    // restart. Without a password (first setup) or with full=false (update
    // mode: kept light) only the password form
    static String generatePasswordPage(bool hasPassword, const String &error, bool apMode,
                                       const SystemView &view = SystemView());

    // Network page (uplink status, time zone, WiFi, ethernet and LTE
    // settings) in pieces of at most ~4 kB, streamed by the web server (see
    // handleNetwork); part 0, 1, ... until NETWORK_PAGE_PARTS, a piece may be
    // empty (a card the board does not have)
    static constexpr size_t NETWORK_PAGE_PARTS = 6;

    // Generation 2 detail page in Advanced mode, reserved in one piece:
    // measured up to 9.5 kB (a finished job, two images, the last download
    // and the download form); four images add ~0.65 kB, longer job,
    // message and download texts some more, the forward URL field (S7)
    // ~0.4 kB plus up to ~1 kB for a URL full of HTML escapes; rounded up
    static constexpr size_t GEN2_ADV_PAGE_SIZE = 11776;
    // Generation 1 add / edit form: measured 3.57 kB (a METEO in Advanced
    // with a 230-character forward URL); a 200-character URL full of HTML
    // escapes and a 32-character name add up to ~1 kB more
    static constexpr size_t SENSOR_FORM_SIZE = 4608;
    static String generateNetworkPagePart(const NetworkPageView &view, size_t part);

    // Sensor list page in pieces (one per sensor row), streamed by the web
    // server (see handleSensors); part 0 .. sensorsPageParts() - 1
    static size_t sensorsPageParts(const SensorsPageView &view);
    static String generateSensorsPagePart(const SensorsPageView &view, size_t part);

    // Add a generation 2 sensor (DevID + label key)
    static String generateGen2AddPage(const String &devId, const String &error, const String &url = String());

    // Generation 2 sensor detail: status, configuration form, status request,
    // downlink counters, devNonce reset
    static String generateGen2DetailPage(const Gen2Sensor &sensor, const String &message,
                                         const Gen2OtaView *ota = nullptr);

    // Add (sensor == nullptr) or edit a generation 1 sensor
    static String generateSensorForm(const SensorData *sensor, int index);

    // Correction fields, shared by generation 1 and 2 (form names of
    // SensorCalibration::info); show: bit f = SensorCalibration::Field f
    static void addCorrections(String &html, uint16_t show, const SensorCalibration::Set &cal);

    // Generate logs page
    // Logs page in pieces, streamed by the web server (see handleLogs)
    static String generateLogsPageHead(LogLevel currentLevel);
    static String generateLogEntry(const LogEntry &entry);
    static String generateLogsPageTail();

    // Generate API page
    static String generateAPIPage(const std::vector<SensorData> &sensors);

    // /api pieces (streamed per sensor): the JSON head up to "sensors":[,
    // one sensor as JSON (ApiRow.h) or as a CSV line
    static String apiJsonHead(bool online);
    static String apiJsonRow(const SensorData &sensor, unsigned long nowMs);
    static String apiCsvRow(const SensorData &sensor, unsigned long nowMs);

    // Generate firmware update page
    static String generateFirmwarePage(const String &updateStatus, bool signedOnly, const GatewayOta::View &gw,
                                       const UpdateCheckView &check, const AutoUpdate::View &au);
    // Small page after an action (saved, restarting): text is HTML; reloads
    // after refreshS (to url when given)
    static String generateNoticePage(const char *title, const String &text, uint8_t refreshS, const char *url);
    // Small page served as /firmware while an update-mode session runs
    // (no styles, navigation or forms; reloads itself every 15 s)
    static String generateUpdateProgressPage(const String &updateStatus);
};
