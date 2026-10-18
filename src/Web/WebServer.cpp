/**
 * expLORA Gateway
 *
 * Web portal implementation file
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

#include "WebServer.h"
#include "HTMLGenerator.h"
#include <WiFi.h>
#include "../Network/Uplink.h"
#include "../Hardware/PowerSource.h"
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_app_format.h>
#include <NetworkClientSecure.h>
#include "../config.h"
#include "../Hardware/WatchdogPause.h"
#include "../Hardware/FlashPermit.h"
#include "../Hardware/RadioTask.h"

// OTA flash access under the flash write permit (a pending radio response
// is not delayed by erasing / programming the app partition)
static bool otaBegin(size_t size)
{
    FlashWriteGuard guard;
    return Update.begin(size);
}

static size_t otaWrite(uint8_t *data, size_t len)
{
    FlashWriteGuard guard;
    return Update.write(data, len);
}

static bool otaEnd(bool evenIfRemaining = false)
{
    FlashWriteGuard guard;
    return Update.end(evenIfRemaining);
}

// Request attribute set on a request the patched parser must drop. The
// library keys attributes by pointer, so both sides use this one symbol
// (declared extern in scripts/patch_asyncwebserver.py)
extern const char EXPLORA_REJECT_ATTR[] = "explora_reject";

// Requests whose body would be parsed into RAM are limited (the body is read
// before the login middleware runs); firmware uploads are streamed instead
static constexpr size_t MAX_REQUEST_BODY = 4096; // largest real form ~1 kB

class OversizeRequestHandler : public AsyncWebHandler
{
public:
    explicit OversizeRequestHandler(WebAuth &webAuth) : auth(webAuth) {}

    // Only a logged-in multipart firmware upload may be large: any other
    // body (also a form-encoded POST to /firmware) would be buffered in RAM.
    // Headers, including the session cookie, are parsed at this point.
    bool canHandle(AsyncWebServerRequest *request) const override
    {
        if (request->contentLength() <= MAX_REQUEST_BODY)
        {
            return false;
        }
        bool upload = request->method() == HTTP_POST && request->url() == "/firmware" &&
                      request->contentType().startsWith("multipart/form-data");
        if (upload && auth.hasPassword() && auth.isAuthorized(request))
        {
            return false;
        }
        // The library would still receive the whole body before answering:
        // the patched parser drops a marked request at its next data
        request->setAttribute(EXPLORA_REJECT_ATTR, true);
        return true;
    }
    void handleRequest(AsyncWebServerRequest *request) override
    {
        request->send(413, "text/plain", "Request too large");
    }

private:
    WebAuth &auth;
};

// Reject images built for another chip (Lite = ESP32-S3, Pro = ESP32) before
// anything is written; the image header starts every application binary
static bool firmwareImageMatches(const uint8_t *data, size_t len, String &error)
{
    if (len < sizeof(esp_image_header_t))
    {
        error = "Firmware image too short";
        return false;
    }
    const esp_image_header_t *header = (const esp_image_header_t *)data;
    if (header->magic != ESP_IMAGE_HEADER_MAGIC)
    {
        error = "Not an ESP32 firmware image";
        return false;
    }
    if (header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
    {
        error = "Firmware is for a different board (" BOARD_NAME " expected)";
        return false;
    }
    return true;
}

// Constructor
WebPortal::WebPortal(SensorManager &sensors, Logger &log, String &ssid, String &password,
                     bool &config_mode, ConfigManager &config, String &tz)
    : server(HTTP_PORT), sensorManager(sensors), logger(log), isAPMode(false),
      wifiSSID(ssid), wifiPassword(password), configMode(config_mode),
      timezone(tz), configManager(config), auth(config, log), mqttManager(nullptr)
{
}

// Add this static task function to WebPortal in WebServer.cpp
// void WebPortal::webServerTask(void *parameter) {
//     WebPortal *server = (WebPortal*)parameter;

//     for (;;) {
//         if (server->isAPMode) {
//             // Process DNS requests (captive portal)
//             server->dnsServer.processNextRequest();
//         }

//         // Give other tasks time to run
//         vTaskDelay(1);
//     }
// }

// Modify the init function to create the task
bool WebPortal::init()
{
    logger.info("Initializing web portal");

    // Captive portal while the AP runs without local network access (see Uplink)
    if (uplink->isCaptive())
    {
        setAPMode(true);
    }

    // Setup routes (idempotent)
    setupRoutes();

    // Start server once
    if (!serverStarted)
    {
        server.begin();
        serverStarted = true;
        logger.info("Web server started on port " + String(HTTP_PORT));
    }


    // Create task on core 0 for DNS and other processing
    // xTaskCreatePinnedToCore(
    //     webServerTask,        // Task function
    //     "WebServerTask",      // Task name
    //     4096,                 // Stack size (bytes)
    //     this,                 // Parameter to pass
    //     1,                    // Task priority (1 is low)
    //     &webServerTaskHandle, // Task handle
    //     0                     // Run on core 0
    // );

    // logger.info("Web server task created on core 0");

    return true;
}

// Setup routes for web server
// Modify WebPortal::setupRoutes() in WebServer.cpp
void WebPortal::setupRoutes()
{
    if (routesRegistered)
    {
        return;
    }

    logger.info("Registering web server routes (once)");

    // First handler: claims oversized requests so their body is not parsed
    server.addHandler(new OversizeRequestHandler(auth));
    server.addMiddleware([this](AsyncWebServerRequest *request, ArMiddlewareNext next)
                         { requireLogin(request, next); });
    server.on("/login", HTTP_GET, std::bind(&WebPortal::handleLogin, this, std::placeholders::_1));
    server.on("/login", HTTP_POST, std::bind(&WebPortal::handleLoginPost, this, std::placeholders::_1));
    server.on("/logout", HTTP_GET, std::bind(&WebPortal::handleLogout, this, std::placeholders::_1));
    server.on("/password", HTTP_GET, std::bind(&WebPortal::handlePassword, this, std::placeholders::_1));
    server.on("/password", HTTP_POST, std::bind(&WebPortal::handlePasswordPost, this, std::placeholders::_1));

    server.on("/", HTTP_GET, std::bind(&WebPortal::handleRoot, this, std::placeholders::_1));
    server.on("/config", HTTP_GET, std::bind(&WebPortal::handleConfig, this, std::placeholders::_1));
    server.on("/config", HTTP_POST, std::bind(&WebPortal::handleConfigPost, this, std::placeholders::_1));

    // Sensor management
    server.on("/sensors/add", HTTP_GET, std::bind(&WebPortal::handleSensorAdd, this, std::placeholders::_1));
    server.on("/sensors/add", HTTP_POST, std::bind(&WebPortal::handleSensorAddPost, this, std::placeholders::_1));
    server.on("/sensors/edit", HTTP_GET, std::bind(&WebPortal::handleSensorEdit, this, std::placeholders::_1));
    server.on("/sensors/update", HTTP_POST, std::bind(&WebPortal::handleSensorEditPost, this, std::placeholders::_1));
    server.on("/sensors/delete", HTTP_GET, std::bind(&WebPortal::handleSensorDelete, this, std::placeholders::_1));
    server.on("/sensors/add2", HTTP_GET, std::bind(&WebPortal::handleGen2Add, this, std::placeholders::_1));
    server.on("/sensors/add2", HTTP_POST, std::bind(&WebPortal::handleGen2AddPost, this, std::placeholders::_1));
    server.on("/sensors/delete2", HTTP_POST, std::bind(&WebPortal::handleGen2Delete, this, std::placeholders::_1));
    server.on("/sensors/gen2/config", HTTP_POST, std::bind(&WebPortal::handleGen2Config, this, std::placeholders::_1));
    server.on("/sensors/gen2/status", HTTP_POST, std::bind(&WebPortal::handleGen2Status, this, std::placeholders::_1));
    server.on("/sensors/gen2/resetnonce", HTTP_POST,
              std::bind(&WebPortal::handleGen2ResetNonce, this, std::placeholders::_1));
    server.on("/sensors/gen2", HTTP_GET, std::bind(&WebPortal::handleGen2Detail, this, std::placeholders::_1));
    server.on("/sensors", HTTP_GET, std::bind(&WebPortal::handleSensors, this, std::placeholders::_1));

    // Logs
    server.on("/logs/clear", HTTP_GET, std::bind(&WebPortal::handleLogsClear, this, std::placeholders::_1));
    server.on("/logs/level", HTTP_POST, std::bind(&WebPortal::handleLogLevel, this, std::placeholders::_1));
    server.on("/logs", HTTP_GET, std::bind(&WebPortal::handleLogs, this, std::placeholders::_1));

    // Network (WiFi settings post to /config, shared with the AP setup page)
    server.on("/network", HTTP_GET, std::bind(&WebPortal::handleNetwork, this, std::placeholders::_1));
#if defined(HAS_ETHERNET) || defined(HAS_LTE)
    server.on("/network", HTTP_POST, std::bind(&WebPortal::handleNetworkPost, this, std::placeholders::_1));
#endif

    // MQTT
    server.on("/mqtt", HTTP_GET, std::bind(&WebPortal::handleMqtt, this, std::placeholders::_1));
    server.on("/mqtt", HTTP_POST, std::bind(&WebPortal::handleMqttPost, this, std::placeholders::_1));

    // API
    server.on("/api", HTTP_GET, std::bind(&WebPortal::handleAPI, this, std::placeholders::_1));

    // Diagnostics
    server.on("/diag/heap", HTTP_GET, std::bind(&WebPortal::handleDiagHeap, this, std::placeholders::_1));
    server.on("/diag/network", HTTP_GET, std::bind(&WebPortal::handleDiagNetwork, this, std::placeholders::_1));

    // Firmware update endpoints
    server.on("/firmware/version", HTTP_GET, std::bind(&WebPortal::handleFirmwareVersion, this, std::placeholders::_1));
    server.on("/firmware/check", HTTP_GET, std::bind(&WebPortal::handleFirmwareCheck, this, std::placeholders::_1));
    server.on("/firmware/update", HTTP_POST, std::bind(&WebPortal::handleFirmwareUpdate, this, std::placeholders::_1));
    server.on("/firmware", HTTP_GET, std::bind(&WebPortal::handleFirmwarePage, this, std::placeholders::_1));
    server.on("/firmware", HTTP_POST,
        [this](AsyncWebServerRequest *request) { this->handleFirmwareUploadComplete(request); },
        [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            this->handleFirmwareUploadChunk(request, filename, index, data, len, final);
        }
    );

    // Reboot
    server.on("/reboot", HTTP_GET, std::bind(&WebPortal::handleReboot, this, std::placeholders::_1));

    // Unknown pages (404)
    server.onNotFound(std::bind(&WebPortal::handleNotFound, this, std::placeholders::_1));

    routesRegistered = true;
    logger.info("Routes registered");
}

// Paths reachable without a login: the login page itself, the read-only
// sensor data API and the version probe
// (GET only: POST /firmware/version would otherwise reach the /firmware
// upload handler by prefix match)
static bool isPublicPath(AsyncWebServerRequest *request)
{
    const String &url = request->url();
    if (url == "/login")
    {
        return request->method() == HTTP_GET || request->method() == HTTP_POST;
    }
    return request->method() == HTTP_GET && (url == "/api" || url == "/firmware/version");
}

void WebPortal::requireLogin(AsyncWebServerRequest *request, ArMiddlewareNext next)
{
    const String &url = request->url();
    if (isPublicPath(request))
    {
        next();
        return;
    }
    if (!auth.isAuthorized(request))
    {
        if (request->method() != HTTP_GET)
        {
            request->send(401, "text/plain", "Login required");
            return;
        }
        // Absolute in AP mode: captive portal probes come with foreign host names
        String target = isAPMode ? "http://" + uplink->getApIP().toString() + "/login" : String("/login");
        if (!isAPMode && url != "/" && url != "/logout")
        {
            // url() is percent-decoded: re-encode, a raw CR/LF would split the header
            target += "?next=";
            for (size_t i = 0; i < url.length(); i++)
            {
                char c = url[i];
                if (isAlphaNumeric(c) || c == '/' || c == '-' || c == '_' || c == '.')
                {
                    target += c;
                }
                else
                {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
                    target += hex;
                }
            }
        }
        request->redirect(target);
        return;
    }
    // Fresh device or update from a version without login: set a password first
    if (!auth.hasPassword() && url != "/password" && url != "/logout")
    {
        request->redirect("/password");
        return;
    }
    next();
}

// Local path to return to after the login
// Page to return to after the login: one of the pages only - never an action
// such as /reboot or /logs/clear (a crafted link would run it on login), and
// never another host
static String safeNextPath(const String &next)
{
    static const char *const pages[] = {"/", "/network", "/sensors", "/sensors/add", "/mqtt", "/firmware",
                                        "/logs", "/api", "/password", "/config"};
    for (const char *page : pages)
    {
        if (next == page)
        {
            return next;
        }
    }
    return "/";
}

void WebPortal::handleLogin(AsyncWebServerRequest *request)
{
    String next = request->hasParam("next") ? request->getParam("next")->value() : String();
    request->send(200, "text/html", HTMLGenerator::generateLoginPage(auth.hasPassword(), "", safeNextPath(next)));
}

void WebPortal::handleLoginPost(AsyncWebServerRequest *request)
{
    String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : String();
    String next = request->hasParam("next", true) ? request->getParam("next", true)->value() : String();
    unsigned long retryAfter = 0;
    String token = auth.login(request, password, retryAfter);
    if (token.length() == 0)
    {
        String error = retryAfter > 0 ? "Too many failed attempts - try again in " + String(retryAfter) + " s"
                                      : String("Wrong password");
        request->send(401, "text/html", HTMLGenerator::generateLoginPage(auth.hasPassword(), error, safeNextPath(next)));
        return;
    }
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", auth.hasPassword() ? safeNextPath(next) : String("/password"));
    response->addHeader("Set-Cookie", WebAuth::cookieFor(token));
    request->send(response);
}

void WebPortal::handleLogout(AsyncWebServerRequest *request)
{
    auth.logout(request);
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", "/login");
    response->addHeader("Set-Cookie", WebAuth::cookieFor(""));
    request->send(response);
}

void WebPortal::handlePassword(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", HTMLGenerator::generatePasswordPage(auth.hasPassword(), "", isAPMode));
}

void WebPortal::handlePasswordPost(AsyncWebServerRequest *request)
{
    auto param = [request](const char *name)
    {
        return request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
    };
    String error = auth.changePassword(request, param("current"), param("password"), param("confirm"));
    if (error.length() > 0)
    {
        request->send(400, "text/html", HTMLGenerator::generatePasswordPage(auth.hasPassword(), error, isAPMode));
        return;
    }
    // All sessions ended with the change - continue with a new one
    String token = auth.sessionAfterPasswordChange();
    AsyncWebServerResponse *response = request->beginResponse(302);
    response->addHeader("Location", isAPMode ? "/config" : "/");
    response->addHeader("Set-Cookie", WebAuth::cookieFor(token));
    request->send(response);
}

// Called from the main loop. Drives deferred restarts so handlers can safely
// schedule ESP.restart() without blocking the async response flush.
void WebPortal::handleClient()
{
    String otaUrl;
    bool checkUpdate;
    {
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        otaUrl = pendingOtaUrl;
        pendingOtaUrl = "";
        if (otaUrl.length() > 0)
        {
            urlOtaRunning = true; // under the lock: an upload start sees it
        }
        checkUpdate = updateCheckRequested;
        updateCheckRequested = false;
    }
    if (checkUpdate)
    {
        runUpdateCheck();
    }
    if (otaUrl.length() > 0)
    {
        runUrlOta(otaUrl);
        urlOtaRunning = false;
    }

    if (restartRequested && (long)(millis() - restartAt) >= 0)
    {
        logger.info("Deferred restart firing now");
        delay(50); // tiny window for any in-flight TCP to drain
        ESP.restart();
    }
}

// Ensure proper cleanup in the destructor
WebPortal::~WebPortal()
{
    // Stop the task if it's running
    // if (webServerTaskHandle != NULL) {
    //    vTaskDelete(webServerTaskHandle);
    //}

    // Stop DNS server
    dnsServer.stop();
}

// Process DNS requests
void WebPortal::processDNS()
{
    dnsServer.processNextRequest();
    // called automatically in handleClient()
}

// Toggle the captive-portal UI/DNS layer. The AP radio itself is managed by Uplink.
void WebPortal::setAPMode(bool enable)
{
    if (enable == isAPMode)
    {
        return; // No change
    }

    if (enable)
    {
        IPAddress apIP = uplink->getApIP();
        if (apIP != IPAddress(0, 0, 0, 0))
        {
            dnsServer.setTTL(30);
            dnsServer.start(DNS_PORT, "*", apIP);
            logger.info("DNS server (re)started on port " + String(DNS_PORT));
        }
        else
        {
            logger.warning("setAPMode(true): AP IP is 0.0.0.0, DNS not started");
        }
        isAPMode = true;
    }
    else
    {
        // Stop captive portal DNS; the AP radio is handled by Uplink
        dnsServer.stop();
        isAPMode = false;
    }
}

// Get current mode
bool WebPortal::isInAPMode() const
{
    return isAPMode;
}

// Restart web server
void WebPortal::restart()
{
    server.end();
    serverStarted = false;

    // Reset DNS server if it was running
    if (isAPMode)
    {
        dnsServer.stop();
    }

    // Re-initialize
    init();
}

// Process HTTP requests

// Root page
void WebPortal::handleRoot(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /");

    // First setup over the access point: WiFi page
    if (guardAPOnly(request))
    {
        return;
    }

    // Get list of sensors
    std::vector<SensorData> sensorsList = sensorManager.getActiveSensors();

    // Generate HTML
    String html = HTMLGenerator::generateHomePage(sensorsList);

    // Send response
    request->send(200, "text/html", html);
}

// Only the very first setup (no WiFi configured and no other uplink) is
// limited to the WiFi page; an LTE-only gateway, or one whose network is
// down, is managed over its access point like any other
bool WebPortal::guardAPOnly(AsyncWebServerRequest *request)
{
    if (isAPMode && wifiSSID.length() == 0 && !uplink->isOnline())
    {
        request->redirect("/config");
        return true;
    }
    return false;
}

// WiFi configuration page
void WebPortal::handleConfig(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /config");

    // Outside AP mode the WiFi settings live on the network page
    if (!isAPMode)
    {
        request->redirect("/network");
        return;
    }

    String currentIP = isAPMode ? uplink->getApIP().toString() : uplink->getLocalIP().toString();

    // Generate HTML (use isAPMode to drive UI state)
    String html = HTMLGenerator::generateConfigPage(wifiSSID, wifiPassword, isAPMode, currentIP, timezone);

    // Send response
    request->send(200, "text/html", html);
}

// Process WiFi configuration form
void WebPortal::handleConfigPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /config");

    if (!request->hasParam("ssid", true) || !request->hasParam("password", true))
    {
        request->send(400, "text/plain", "Missing parameters");
        return;
    }

    String newSsid = request->getParam("ssid", true)->value();
    newSsid.trim();
    String newPassword = request->getParam("password", true)->value();
    // The stored password is not sent to the page: empty for the same network
    // keeps it, empty for another network means an open one
    if (newPassword.length() == 0 && newSsid == wifiSSID)
    {
        newPassword = wifiPassword;
    }

    // Validate SSID: 1..32 chars (IEEE 802.11 limit)
    if (newSsid.length() == 0)
    {
        request->send(400, "text/plain", "SSID cannot be empty");
        return;
    }
    if (newSsid.length() > 32)
    {
        request->send(400, "text/plain", "SSID too long (max 32 characters)");
        return;
    }
    // Validate password: empty (open network), an 8..63 character passphrase
    // or a 64 hex digit pre-shared key (WPA2-PSK)
    bool hexKey = newPassword.length() == 64;
    for (size_t i = 0; hexKey && i < newPassword.length(); i++)
    {
        hexKey = isHexadecimalDigit(newPassword[i]);
    }
    if (newPassword.length() > 0 && newPassword.length() < 8)
    {
        request->send(400, "text/plain", "Password must be at least 8 characters or left empty for open networks");
        return;
    }
    if (newPassword.length() > 63 && !hexKey)
    {
        request->send(400, "text/plain", "Password too long (max 63 characters, or a 64 hex digit key)");
        return;
    }

    String newTimezone = request->hasParam("timezone", true) ? request->getParam("timezone", true)->value() : timezone;
    static const char *const zones[][2] = TIMEZONE_OPTIONS;
    bool knownZone = false;
    for (const auto &zone : zones)
    {
        knownZone = knownZone || newTimezone == zone[0];
    }
    if (!knownZone)
    {
        request->send(400, "text/plain", "Unknown timezone");
        return;
    }

    // Through ConfigManager: both stores (file and Preferences) and atomic,
    // so nothing (the timezone) is overridden by a stale copy at boot
    logger.info("New WiFi configuration - SSID: " + newSsid);
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        timezone = newTimezone;
    }
    if (!configManager.setWiFiConfig(newSsid, newPassword))
    {
        request->send(500, "text/plain", "Failed to save configuration");
        return;
    }

    // Build confirmation page
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='10;url=/'>";
    html += "<title>Configuration Saved</title></head>";
    html += "<body><h1>Configuration Saved</h1>";
    html += "<p>New WiFi settings have been saved. The device will restart in a few seconds.</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);

    // Defer the restart so the async server can finish flushing the response.
    // handleClient() (called from the main loop) will trigger ESP.restart()
    // once restartAt has been reached.
    restartAt = millis() + 1500;
    restartRequested = true;
}

// Response that browsers and proxies must not store (pages with key material)
static void sendNoStore(AsyncWebServerRequest *request, int code, const char *type, const String &body)
{
    AsyncWebServerResponse *response = request->beginResponse(code, type, body);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

// Sensors list page
void WebPortal::handleSensors(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors");
    if (guardAPOnly(request)) return;

    // Get list of sensors
    std::vector<ActiveSensorEntry> sensorsList = sensorManager.getActiveSensorEntries();
    std::vector<Gen2Sensor> gen2List = sensorManager.getGen2Sensors();
    WaitingEntry waiting[8];
    size_t waitingCount = sensorManager.getWaitingList(waiting, 8);

    // Generate HTML
    String html = HTMLGenerator::generateSensorsPage(sensorsList, gen2List, waiting, waitingCount);
    for (Gen2Sensor &g : gen2List)
    {
        g.wipeKeys();
    }

    // Send response (key fingerprints: never cached)
    sendNoStore(request, 200, "text/html", html);
}

// Sensor add page
void WebPortal::handleSensorAdd(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/add");
    if (guardAPOnly(request)) return;

    // Generate HTML
    String html = HTMLGenerator::generateSensorAddPage();

    // Send response
    request->send(200, "text/html", html);
}

// Sensor form fields shared by add and edit
struct SensorForm
{
    String name;
    SensorType deviceType;
    uint32_t serialNumber;
    uint32_t deviceKey;
    String customUrl;
    int altitude;
    float tempCorr, humCorr, pressCorr, ppmCorr, luxCorr;
    float windSpeedCorr;
    int windDirCorr;
    float rainAmountCorr, rainRateCorr;
};

// Hex number of 1..maxDigits digits, non-zero
static bool parseHexField(const String &text, uint8_t maxDigits, uint32_t &out)
{
    if (text.length() == 0 || text.length() > maxDigits)
    {
        return false;
    }
    for (size_t i = 0; i < text.length(); i++)
    {
        if (!isHexadecimalDigit(text[i]))
        {
            return false;
        }
    }
    out = strtoul(text.c_str(), nullptr, 16);
    return out != 0;
}

// Parse and validate the sensor form; returns an error message, empty if valid
// keepKey: an empty key field keeps form.deviceKey (edit form)
static String parseSensorForm(AsyncWebServerRequest *request, SensorForm &form, bool keepKey = false)
{
    auto text = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    auto number = [&text](const char *name, float fallback, float minValue, float maxValue, bool &ok)
    {
        String value = text(name);
        if (value.length() == 0)
        {
            return fallback;
        }
        float f = value.toFloat();
        if (!isfinite(f) || f < minValue || f > maxValue)
        {
            ok = false;
        }
        return f;
    };

    form.name = text("name");
    if (form.name.length() == 0 || form.name.length() > 32)
    {
        return "Name must have 1 to 32 characters";
    }
    long type = text("deviceType").toInt();
    if (type <= 0 || type > 255 ||
        getSensorTypeInfo(static_cast<uint8_t>(type)).type == SensorType::UNKNOWN)
    {
        return "Unknown device type";
    }
    form.deviceType = static_cast<SensorType>(type);
    if (!parseHexField(text("serialNumber"), 6, form.serialNumber))
    {
        return "Serial number must be 1 to 6 hex digits";
    }
    if (!(keepKey && text("deviceKey").length() == 0) && !parseHexField(text("deviceKey"), 8, form.deviceKey))
    {
        return "Device key must be 1 to 8 hex digits";
    }
    form.customUrl = text("customUrl");
    if (form.customUrl.length() > 200 ||
        (form.customUrl.length() > 0 && !form.customUrl.startsWith("http://") && !form.customUrl.startsWith("https://")))
    {
        return "Custom URL must start with http:// or https:// (max 200 characters)";
    }

    bool ok = true;
    form.altitude = (int)number("altitude", 0, 0, 9000, ok);
    form.tempCorr = number("tempCorr", 0, -50, 50, ok);
    form.humCorr = number("humCorr", 0, -50, 50, ok);
    form.pressCorr = number("pressCorr", 0, -200, 200, ok);
    form.ppmCorr = number("ppmCorr", 0, -5000, 5000, ok);
    form.luxCorr = number("luxCorr", 0, -100000, 100000, ok);
    form.windSpeedCorr = number("windSpeedCorr", 1, 0, 10, ok);
    form.windDirCorr = (int)number("windDirCorr", 0, -360, 360, ok);
    form.rainAmountCorr = number("rainAmountCorr", 1, 0, 10, ok);
    form.rainRateCorr = number("rainRateCorr", 1, 0, 10, ok);
    if (!ok)
    {
        return "A correction value or the altitude is out of range";
    }
    return "";
}

static bool saveSensorForm(SensorManager &sensors, int index, const SensorForm &form)
{
    return sensors.updateSensorConfig(index, form.name, form.deviceType, form.serialNumber, form.deviceKey,
                                      form.customUrl, form.altitude, form.tempCorr, form.humCorr,
                                      form.pressCorr, form.ppmCorr, form.luxCorr, form.windSpeedCorr,
                                      form.windDirCorr, form.rainAmountCorr, form.rainRateCorr);
}

// Process sensor add form
void WebPortal::handleSensorAddPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/add");
    if (guardAPOnly(request)) return;

    SensorForm form;
    String error = parseSensorForm(request, form);
    if (error.length() > 0)
    {
        request->send(400, "text/plain", error);
        return;
    }

    int index = sensorManager.addSensor(form.deviceType, form.serialNumber, form.deviceKey, form.name);
    if (index == -2)
    {
        request->send(409, "text/plain", "A sensor with this serial number already exists - edit it instead");
        return;
    }
    if (index >= 0 && !saveSensorForm(sensorManager, index, form))
    {
        // Not saved: take it back out, so a retry does not hit "already exists"
        sensorManager.deleteSensor(index);
        index = -1;
    }
    if (index < 0)
    {
        request->send(500, "text/plain", "Failed to add sensor (sensor list full, or the flash write failed - see Logs)");
        return;
    }

    if (mqttManager)
    {
        mqttManager->requestSensorDiscovery(form.serialNumber);
    }
    request->redirect("/sensors");
}

// Sensor edit page
void WebPortal::handleSensorEdit(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/edit");
    if (guardAPOnly(request)) return;

    // Check index parameter
    if (request->hasParam("index"))
    {
        int index = request->getParam("index")->value().toInt();

        SensorData sensor;
        if (sensorManager.getSensorCopy(index, sensor))
        {
            // Generate HTML
            String html = HTMLGenerator::generateSensorEditPage(sensor, index);

            // Send response
            request->send(200, "text/html", html);
            return;
        }
    }

    // Sensor not found, redirect to sensors list
    request->redirect("/sensors");
}

// Process sensor edit form
void WebPortal::handleSensorEditPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/update");
    if (guardAPOnly(request)) return;

    int index = request->hasParam("index", true) ? request->getParam("index", true)->value().toInt() : -1;
    SensorData before;
    if (!sensorManager.getSensorCopy(index, before))
    {
        request->send(404, "text/plain", "Sensor not found");
        return;
    }
    if (!request->hasParam("sn", true) || request->getParam("sn", true)->value() != formatSN(before.serialNumber))
    {
        request->send(409, "text/plain", "The sensor list changed in the meantime - reload the page");
        return;
    }

    SensorForm form;
    form.deviceKey = before.deviceKey;
    String error = parseSensorForm(request, form, true);
    if (error.length() > 0)
    {
        request->send(400, "text/plain", error);
        return;
    }
    if (!saveSensorForm(sensorManager, index, form))
    {
        request->send(409, "text/plain",
                      "Failed to update sensor (serial number used by another sensor, or the flash write failed - see Logs)");
        return;
    }

    if (mqttManager)
    {
        // Entities of the old serial number / old value set would stay in HA
        if (before.serialNumber != form.serialNumber || before.deviceType != form.deviceType)
        {
            mqttManager->requestSensorRemoval(before.serialNumber);
        }
        mqttManager->requestSensorDiscovery(form.serialNumber);
    }
    request->redirect("/sensors");
}

// Delete sensor
void WebPortal::handleSensorDelete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/delete");
    if (guardAPOnly(request)) return;

    // Check index parameter
    if (request->hasParam("index"))
    {
        int index = request->getParam("index")->value().toInt();

        SensorData sensor;
        String sn = request->hasParam("sn") ? request->getParam("sn")->value() : String();
        if (sensorManager.getSensorCopy(index, sensor) && sn == formatSN(sensor.serialNumber))
        {
            String name = sensor.name;
            uint32_t serialNumber = sensor.serialNumber;

            // Gone from RAM either way; false only means the flash write failed
            if (!sensorManager.deleteSensor(index))
            {
                logger.warning("Deleted sensor " + name + ", but saving the list failed - it returns after a restart");
            }
            if (mqttManager)
            {
                mqttManager->requestSensorRemoval(serialNumber);
            }
        }
        else
        {
            logger.warning("Delete ignored: no sensor " + sn + " at index " + String(index));
        }
    }

    // Redirect to sensors list
    request->redirect("/sensors");
}

// Generation 2 sensors (protocol v2): add by DevID + label key, delete by DevID
void WebPortal::handleGen2Add(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/add2");
    if (guardAPOnly(request)) return;
    // Prefill from the waiting list; only a valid DevID is echoed back
    String devId;
    uint32_t id = 0;
    if (request->hasParam("devId") && ep::parseDevId(request->getParam("devId")->value().c_str(), id))
    {
        char buf[9];
        snprintf(buf, sizeof(buf), "%08lX", (unsigned long)id);
        devId = buf;
    }
    sendNoStore(request, 200, "text/html", HTMLGenerator::generateGen2AddPage(devId, ""));
}

void WebPortal::handleGen2AddPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/add2");
    if (guardAPOnly(request)) return;
    auto param = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    String name = param("name");
    String devIdText = param("devId");
    String keyText = param("key");
    uint32_t devId = 0;
    uint8_t label[ep::kLabelKeyLen];
    String error;
    if (name.length() == 0 || name.length() > 32)
    {
        error = "Name must have 1 to 32 characters";
    }
    else if (!ep::parseDevId(devIdText.c_str(), devId))
    {
        error = "DevID must be exactly 8 hex digits";
    }
    else if (!ep::parseLabelKey(keyText.c_str(), label))
    {
        error = "Key must be exactly 16 hex digits (dashes and spaces are ignored)";
    }
    if (error.length() > 0)
    {
        sendNoStore(request, 400, "text/html", HTMLGenerator::generateGen2AddPage(devIdText.substring(0, 8), error));
        return;
    }
    int index = sensorManager.addGen2Sensor(devId, label, name);
    ep::wipe(label, sizeof(label));
    keyText = String();
    if (index == -2)
    {
        request->send(409, "text/plain", "A generation 2 sensor with this DevID already exists");
        return;
    }
    if (index < 0)
    {
        request->send(500, "text/plain",
                      "Failed to add the sensor (list full, or the flash write failed - see Logs)");
        return;
    }
    request->redirect("/sensors");
}

// DevID from a query (GET) or form (POST) parameter
static bool gen2DevIdParam(AsyncWebServerRequest *request, bool post, uint32_t &devId)
{
    return request->hasParam("devId", post) &&
           ep::parseDevId(request->getParam("devId", post)->value().c_str(), devId);
}

void WebPortal::handleGen2Delete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/delete2");
    if (guardAPOnly(request)) return;
    uint32_t devId = 0;
    if (gen2DevIdParam(request, true, devId))
    {
        if (!sensorManager.deleteGen2Sensor(devId))
        {
            logger.warning("Delete of a generation 2 sensor ignored or not saved");
        }
    }
    request->redirect("/sensors");
}

void WebPortal::sendGen2Detail(AsyncWebServerRequest *request, uint32_t devId, const String &message, int code)
{
    // The page copy lives only for the generation (one sensor, ~600 bytes)
    std::unique_ptr<Gen2Sensor> g(new (std::nothrow) Gen2Sensor());
    if (!g || !sensorManager.getGen2Sensor(devId, *g))
    {
        request->send(404, "text/plain", "Unknown generation 2 sensor");
        return;
    }
    String html = HTMLGenerator::generateGen2DetailPage(*g, message);
    g->wipeKeys();
    sendNoStore(request, code, "text/html", html);
}

void WebPortal::handleGen2Detail(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/gen2");
    if (guardAPOnly(request)) return;
    uint32_t devId = 0;
    if (!gen2DevIdParam(request, false, devId))
    {
        request->send(400, "text/plain", "DevID missing");
        return;
    }
    sendGen2Detail(request, devId, "");
}

void WebPortal::handleGen2Config(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/gen2/config");
    if (guardAPOnly(request)) return;
    uint32_t devId = 0;
    if (!gen2DevIdParam(request, true, devId))
    {
        request->send(400, "text/plain", "DevID missing");
        return;
    }
    Gen2Config::Values v;
    {
        // Heap copy: Gen2Sensor is too big for the AsyncTCP stack
        std::unique_ptr<Gen2Sensor> current(new (std::nothrow) Gen2Sensor());
        if (!current || !sensorManager.getGen2Sensor(devId, *current))
        {
            request->send(404, "text/plain", "Unknown generation 2 sensor");
            return;
        }
        v = current->config;
        current->wipeKeys();
    }
    bool ok = true;
    auto number = [request, &ok](const char *name, long min, long max, long fallback)
    {
        if (!request->hasParam(name, true))
        {
            return fallback;
        }
        String text = request->getParam(name, true)->value();
        text.trim();
        char *end = nullptr;
        long value = strtol(text.c_str(), &end, 10);
        if (text.length() == 0 || end == nullptr || *end != '\0' || value < min || value > max)
        {
            ok = false;
            return fallback;
        }
        return value;
    };
    v.intervalS = (uint16_t)number("interval", Gen2Config::MIN_INTERVAL_S, Gen2Config::MAX_INTERVAL_S, v.intervalS);
    v.slotS = (uint16_t)number("slot", 0, Gen2Config::MAX_INTERVAL_S - 1, v.slotS);
    v.txPowerDbm = (int8_t)number("txPower", Gen2Config::MIN_TX_POWER_DBM, Gen2Config::MAX_TX_POWER_DBM, v.txPowerDbm);
    v.ackEvery = (uint8_t)number("ackEvery", 0, 255, v.ackEvery);
    v.ackRetries = (uint8_t)number("ackRetries", 0, Gen2Config::MAX_RETRIES, v.ackRetries);
    v.ackBackoffS = (uint8_t)number("ackBackoff", Gen2Config::MIN_BACKOFF_S, Gen2Config::MAX_BACKOFF_S, v.ackBackoffS);
    if (!ok)
    {
        sendGen2Detail(request, devId, "Not saved: a value is out of range", 400);
        return;
    }
    if (v.slotS >= v.intervalS)
    {
        sendGen2Detail(request, devId, "Not saved: the slot offset must be smaller than the interval", 400);
        return;
    }
    int result = sensorManager.setGen2Config(devId, v);
    const char *msg = result == 1   ? "Saved - waiting for the sensor (sent with its next uplink)"
                      : result == 0 ? "Nothing changed"
                                    : "Not saved: flash write failed (see Logs)";
    sendGen2Detail(request, devId, msg, result >= 0 ? 200 : 500);
}

void WebPortal::handleGen2Status(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/gen2/status");
    if (guardAPOnly(request)) return;
    uint32_t devId = 0;
    if (!gen2DevIdParam(request, true, devId))
    {
        request->send(400, "text/plain", "DevID missing");
        return;
    }
    if (!sensorManager.requestGen2Status(devId))
    {
        request->send(404, "text/plain", "Unknown generation 2 sensor");
        return;
    }
    sendGen2Detail(request, devId, "Status requested - the sensor answers after its next uplink");
}

void WebPortal::handleGen2ResetNonce(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/gen2/resetnonce");
    if (guardAPOnly(request)) return;
    uint32_t devId = 0;
    if (!gen2DevIdParam(request, true, devId))
    {
        request->send(400, "text/plain", "DevID missing");
        return;
    }
    bool ok = sensorManager.resetGen2DevNonce(devId);
    sendGen2Detail(request, devId, ok ? "devNonce reset - restart the sensor now" : "devNonce reset not saved (see Logs)",
                   ok ? 200 : 500);
}

// Logs page
void WebPortal::handleLogs(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /logs");
    if (guardAPOnly(request)) return;

    // Streamed piece by piece: the whole page (up to ~50 kB with 150 entries)
    // never sits in RAM at once, nor twice (send() would copy it)
    struct Stream
    {
        String pending;
        size_t offset = 0;
        size_t entry = 0;
        uint8_t phase = 0; // 0 head, 1 entries, 2 tail, 3 done
        LogLevel level;
    };
    auto state = std::make_shared<Stream>();
    state->level = logger.getLogLevel();
    request->sendChunked("text/html", [state](uint8_t *buffer, size_t maxLen, size_t) -> size_t
                         {
        while (state->offset >= state->pending.length())
        {
            state->offset = 0;
            if (state->phase == 0)
            {
                state->pending = HTMLGenerator::generateLogsPageHead(state->level);
                state->phase = 1;
            }
            else if (state->phase == 1)
            {
                LogEntry entry;
                if (Logger::copyNewest(state->entry, entry))
                {
                    state->pending = HTMLGenerator::generateLogEntry(entry);
                    state->entry++;
                }
                else
                {
                    state->pending = state->entry == 0 ? "<div class='log-entry'>No logs to display</div>" : "";
                    state->phase = 2;
                }
            }
            else if (state->phase == 2)
            {
                state->pending = HTMLGenerator::generateLogsPageTail();
                state->phase = 3;
            }
            else
            {
                return 0;
            }
        }
        size_t n = std::min(maxLen, state->pending.length() - state->offset);
        memcpy(buffer, state->pending.c_str() + state->offset, n);
        state->offset += n;
        return n; });
}

// Clear logs
void WebPortal::handleLogsClear(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /logs/clear");
    if (guardAPOnly(request)) return;

    // Clear logs
    logger.clearLogs();

    logger.info("Memory after log clear - Free heap: " + String(ESP.getFreeHeap()) +
                " bytes, Largest block: " + String(ESP.getMaxAllocHeap()) + " bytes");

    // Redirect back to logs page
    request->redirect("/logs");
}

// Set logging level
void WebPortal::handleLogLevel(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /logs/level");
    if (guardAPOnly(request)) return;

    // Check level parameter
    if (request->hasParam("level", true))
    {
        String levelStr = request->getParam("level", true)->value();
        LogLevel level = logger.levelFromString(levelStr);

        // Set new level
        configManager.setLogLevel(level);
    }

    // Redirect back to logs page
    request->redirect("/logs");
}

// MQTT Configuration Page
void WebPortal::handleMqtt(AsyncWebServerRequest *request)
{
    // logger.debug("HTTP request: GET /mqtt");
    if (guardAPOnly(request)) return;

    // Get MQTT configuration from ConfigManager
    // ConfigManager* configManager = (ConfigManager*)request->getParam("configManager")->value().toInt();
    logger.debug("HTTP request: GET /mqtt");

    // Generate HTML
    String html = HTMLGenerator::generateMqttPage(
        configManager.mqttHost,
        configManager.mqttPort,
        configManager.mqttUser,
        configManager.mqttPassword,
        configManager.mqttEnabled,
        configManager.mqttTls,
        configManager.mqttPrefix,
        configManager.mqttHAEnabled,
        configManager.mqttHAPrefix
    );

    // Send response
    request->send(200, "text/html", html);
}

// MQTT Configuration Post Handler
void WebPortal::handleMqttPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /mqtt");
    if (guardAPOnly(request)) return;

    auto param = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    MQTTManager::Settings settings;
    settings.host = param("host");
    settings.port = param("port").toInt();
    settings.user = param("user");
    settings.password = request->hasParam("password", true) ? request->getParam("password", true)->value() : String();
    // Empty password = keep the stored one (it is not sent to the page);
    // no user means no password
    if (settings.password.length() == 0 && settings.user.length() > 0 && !request->hasParam("clearPassword", true))
    {
        settings.password = configManager.mqttPassword;
    }
    settings.enabled = request->hasParam("enabled", true);
    settings.tls = request->hasParam("tls", true);
    settings.prefix = param("prefix");
    settings.haEnabled = request->hasParam("haEnabled", true);
    settings.haPrefix = param("haPrefix");

    if (settings.host.length() == 0 || settings.host.length() > 64 || settings.host.indexOf(' ') >= 0)
    {
        request->send(400, "text/plain", "Broker host must have 1 to 64 characters without spaces");
        return;
    }
    if (settings.port < 1 || settings.port > 65535)
    {
        request->send(400, "text/plain", "Port must be 1 to 65535");
        return;
    }
    if (settings.user.length() > 64 || settings.password.length() > 64)
    {
        request->send(400, "text/plain", "User name and password max 64 characters");
        return;
    }
    bool prefixOk = ConfigManager::isValidMqttTopic(settings.prefix) || settings.prefix == configManager.mqttPrefix;
    bool haPrefixOk = ConfigManager::isValidMqttTopic(settings.haPrefix) || settings.haPrefix == configManager.mqttHAPrefix;
    if (!prefixOk || !haPrefixOk)
    {
        request->send(400, "text/plain", "Invalid topic (1-32 characters, no #, + or /)");
        return;
    }

    if (!configManager.setMqttConfig(settings.host, settings.port, settings.user, settings.password,
                                     settings.enabled, settings.tls, settings.prefix, settings.haPrefix,
                                     settings.haEnabled))
    {
        request->send(500, "text/plain", "Saving the MQTT configuration failed");
        return;
    }
    logger.info("MQTT configuration updated - " + settings.host + ":" + String(settings.port) +
                (settings.enabled ? ", enabled" : ", disabled"));
    // The loop task reconnects with a fresh copy of the settings
    if (mqttManager)
    {
        mqttManager->requestReinit();
    }
    request->redirect("/mqtt");
}

// API for retrieving sensor data
void WebPortal::handleAPI(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /api");
    if (guardAPOnly(request)) return;
    // Public endpoint: keep its peak (~20 kB with 20 sensors: list copy, JSON
    // document, response buffer) from eating the heap others need; 40 kB
    // still lets it run next to an MQTT TLS session on the Pro
    if (ESP.getFreeHeap() < 40000 || ESP.getMaxAllocHeap() < 12000)
    {
        request->send(503, "text/plain", "Low memory, try again");
        return;
    }

    // Check format parameter
    String format = request->hasParam("format") ? request->getParam("format")->value() : "html";

    // Check sensor parameter
    String sensorParam = request->hasParam("sensor") ? request->getParam("sensor")->value() : "";

    // Get list of sensors
    std::vector<SensorData> sensorsList;

    if (sensorParam.length() > 0)
    {
        // Filter by specific sensor
        uint32_t serialNumber = strtoul(sensorParam.c_str(), NULL, 16);
        for (const SensorData &sensor : sensorManager.getActiveSensors())
        {
            if (sensor.serialNumber == serialNumber)
            {
                sensorsList.push_back(sensor);
            }
        }
    }
    else
    {
        // All sensors
        sensorsList = sensorManager.getActiveSensors();
    }

    if (format.equalsIgnoreCase("json"))
    {
        // JSON format
        // One reserve of the measured size: the stream grows byte by byte
        // otherwise (a realloc per character of a public response)
        DynamicJsonDocument doc = HTMLGenerator::buildAPIJson(sensorsList);
        AsyncResponseStream *response = request->beginResponseStream("application/json", measureJson(doc) + 16);
        serializeJson(doc, *response);
        response->addHeader(CORS_HEADER_NAME, CORS_HEADER_VALUE);
        request->send(response);
    }
    else if (format.equalsIgnoreCase("csv"))
    {
        // CSV format
        AsyncResponseStream *response = request->beginResponseStream("text/csv");
        response->print("name,type,serialNumber,lastSeen,temperature,humidity,pressure,ppm,lux,batteryVoltage,rssi\r\n");

        for (const auto &sensor : sensorsList)
        {
            // RFC 4180 quoting; a leading = + - @ would be a spreadsheet formula
            String name = sensor.name;
            name.replace("\"", "\"\"");
            if (name.length() > 0 && strchr("=+-@", name[0]))
            {
                name = "'" + name;
            }
            response->print("\"" + name + "\"");
            response->print(",");
            response->print(static_cast<uint8_t>(sensor.deviceType));
            response->print(",");
            response->print(formatSN(sensor.serialNumber));
            response->print(",");
            response->print(sensor.lastSeen > 0 ? String((millis() - sensor.lastSeen) / 1000) : "-1");
            response->print(",");

            response->print(sensor.hasTemperature() ? String(sensor.temperature, 2) : "");
            response->print(",");
            response->print(sensor.hasHumidity() ? String(sensor.humidity, 2) : "");
            response->print(",");
            response->print(sensor.hasPressure() ? String(sensor.pressure, 2) : "");
            response->print(",");
            response->print(sensor.hasPPM() ? String(sensor.ppm, 0) : "");
            response->print(",");
            response->print(sensor.hasLux() ? String(sensor.lux, 1) : "");
            response->print(",");
            response->print(String(sensor.batteryVoltage, 2));
            response->print(",");
            response->print(String(sensor.rssi));
            response->print("\r\n");
        }

        response->addHeader(CORS_HEADER_NAME, CORS_HEADER_VALUE);
        request->send(response);
    }
    else if (format.equalsIgnoreCase("html"))
    {
        // HTML format (API documentation)
        String html = HTMLGenerator::generateAPIPage(sensorsList);
        request->send(200, "text/html", html);
    }
    else
    {
        // Unknown format
        request->send(400, "text/plain", "Invalid format parameter. Supported formats: json, csv, html");
    }
}

// Restart device
void WebPortal::handleReboot(AsyncWebServerRequest *request)
{
    logger.info("HTTP request: GET /reboot - Rebooting device");

    request->send(200, "text/html",
                  "<html><head><meta http-equiv='refresh' content='10;url=/'></head>"
                  "<body><h1>Rebooting</h1>"
                  "<p>The device is rebooting. You will be redirected in 10 seconds...</p></body></html>");

    // Defer the restart so the async server can flush the response cleanly
    // (same pattern as handleConfigPost). The handleClient() main-loop hook
    // fires ESP.restart() once restartAt has been reached. The previous
    // inline `delay(500); ESP.restart()` raced with the async-server task
    // and intermittently lost the restart entirely when the LoRa loop was
    // busy at the same moment.
    restartAt = millis() + 1500;
    restartRequested = true;
}

// Handle unknown pages
// In WebServer.cpp, modify the handleNotFound method
// Update WebPortal::handleNotFound in WebServer.cpp
void WebPortal::handleNotFound(AsyncWebServerRequest *request)
{
    String requestUrl = request->url();
    logger.debug("HTTP 404: " + requestUrl);

    if (isAPMode)
    {
        // In AP mode - captive portal - redirect all requests to config page

        // Skip redirecting for certain files that might be requested by browsers
        if (requestUrl.endsWith(".ico") || requestUrl.endsWith(".jpg") ||
            requestUrl.endsWith(".png") || requestUrl.endsWith(".gif") ||
            requestUrl.endsWith(".css") || requestUrl.endsWith(".js"))
        {
            request->send(404, "text/plain", "Not found");
            return;
        }

        // For Apple devices
        if (requestUrl == "/hotspot-detect.html" || requestUrl.indexOf("captive.apple.com") >= 0)
        {
            request->redirect("http://" + uplink->getApIP().toString() + "/config");
            return;
        }

        // For Android and Windows devices
        if (requestUrl == "/generate_204" || requestUrl == "/ncsi.txt" ||
            requestUrl.indexOf("detectportal.firefox.com") >= 0)
        {
            request->redirect("http://" + uplink->getApIP().toString() + "/config");
            return;
        }

        // General case - redirect to config
        request->redirect("/config");
    }
    else
    {
        // In normal mode - 404
        request->send(404, "text/plain", "404: Not Found");
    }
}

// Diagnostics endpoint: heap and system info 
void WebPortal::handleDiagHeap(AsyncWebServerRequest *request)
{
    size_t freeHeap = ESP.getFreeHeap();
    size_t maxAlloc = ESP.getMaxAllocHeap();
#ifdef BOARD_HAS_PSRAM
    size_t psramSize = ESP.getPsramSize();
    size_t psramFree = ESP.getFreePsram();
#else
    size_t psramSize = 0;
    size_t psramFree = 0;
#endif
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    float frag = (free8 > 0) ? (1.0f - (float)largest8 / (float)free8) : 0.0f;

    AsyncResponseStream *res = request->beginResponseStream("application/json");
    res->print("{");
    res->print("\"freeHeap\":"); res->print((unsigned long)freeHeap); res->print(",");
    res->print("\"maxAllocHeap\":"); res->print((unsigned long)maxAlloc); res->print(",");
    res->print("\"free8bit\":"); res->print((unsigned long)free8); res->print(",");
    res->print("\"largest8bit\":"); res->print((unsigned long)largest8); res->print(",");
    res->print("\"fragmentation\":"); res->print(frag, 4); res->print(",");
    res->print("\"psramTotal\":"); res->print((unsigned long)psramSize); res->print(",");
    res->print("\"psramFree\":"); res->print((unsigned long)psramFree); res->print(",");
    res->print("\"uptimeMs\":"); res->print((unsigned long)millis()); res->print(",");
    // Unused stack of the loop task (bytes) - headroom check for TLS paths
    extern TaskHandle_t loopTaskHandle;
    res->print("\"loopStackFree\":");
    res->print((unsigned long)(loopTaskHandle ? uxTaskGetStackHighWaterMark(loopTaskHandle) : 0));
    res->print(",");
    // Radio task: queue, latency to loop(), stack, flash write permit
    RadioTask::Stats rs = RadioTask::stats();
    res->printf("\"radio\":{\"received\":%lu,\"crcErrors\":%lu,\"readErrors\":%lu,\"queueDrops\":%lu,"
                "\"queueHigh\":%lu,\"maxLatencyMs\":%lu,\"resetsOk\":%lu,\"resetsFailed\":%lu,\"selfHeals\":%lu,"
                "\"unreliableTime\":%lu,\"stackFree\":%lu,\"flashWaits\":%lu,\"flashTimeouts\":%lu},",
                (unsigned long)rs.received, (unsigned long)rs.crcErrors, (unsigned long)rs.readErrors,
                (unsigned long)rs.queueDrops, (unsigned long)rs.queueHigh, (unsigned long)rs.maxLatencyMs,
                (unsigned long)rs.resetsOk, (unsigned long)rs.resetsFailed, (unsigned long)rs.selfHeals,
                (unsigned long)rs.unreliableTime, (unsigned long)rs.stackFree,
                (unsigned long)FlashPermit::writerWaits(), (unsigned long)FlashPermit::writerTimeouts());
    // Protocol v2 downlink: responses per window, reasons for not sending,
    // duty cycle used in the last hour (869.525 MHz, 10 % = 360 s)
    res->printf("\"downlink\":{\"rx1\":%lu,\"rx2\":%lu,\"busy\":%lu,\"budget\":%lu,\"pacing\":%lu,"
                "\"late\":%lu,\"lateRequest\":%lu,\"noHold\":%lu,\"failed\":%lu,\"txDoneTimeouts\":%lu,"
                "\"holdFails\":%lu,\"slotsBusy\":%lu,\"dutyUsedMs\":%lu,\"joinUsedMs\":%lu,\"dutyBudgetMs\":%lu},",
                (unsigned long)rs.txRx1, (unsigned long)rs.txRx2, (unsigned long)rs.txBusy,
                (unsigned long)rs.txBudget, (unsigned long)rs.txPacing, (unsigned long)rs.txLate,
                (unsigned long)rs.txLateRequest, (unsigned long)rs.txNoHold, (unsigned long)rs.txFailed,
                (unsigned long)rs.txDoneTimeouts, (unsigned long)rs.holdFails, (unsigned long)rs.slotsBusy,
                (unsigned long)rs.dutyUsedMs, (unsigned long)rs.joinUsedMs, 360000UL);
    res->print("\"wifiStatus\":"); res->print((int)WiFi.status()); res->print(",");
    res->print("\"apMode\":"); res->print(isAPMode ? "true" : "false"); res->print(",");
    res->print("\"uplink\":\""); res->print(uplink->getActiveName()); res->print("\",");
    res->print("\"ip\":\""); res->print(uplink->getLocalIP().toString()); res->print("\"");
    res->print("}");
    request->send(res);
}

// Network state as JSON
void WebPortal::handleDiagNetwork(AsyncWebServerRequest *request)
{
    DynamicJsonDocument doc(1536);
    doc["uplink"] = uplink->getActiveName();
    doc["ip"] = uplink->getLocalIP().toString();
    doc["ap"] = uplink->isApActive();
    doc["captive"] = uplink->isCaptive();
    JsonObject timeObj = doc.createNestedObject("time");
    timeObj["valid"] = uplink->isTimeValid();
    timeObj["lastSync"] = (long)uplink->getLastTimeSync();
    timeObj["stale"] = uplink->isTimeStale();
    timeObj["status"] = uplink->getTimeStatus();
#if defined(HAS_PWR_SRC)
    doc["power"] = powerSource->getName();
#endif
    JsonObject wifi = doc.createNestedObject("wifi");
    wifi["connected"] = WiFi.STA.hasIP();
    wifi["ip"] = WiFi.STA.localIP().toString();
#if defined(HAS_ETHERNET)
    JsonObject eth = doc.createNestedObject("ethernet");
    eth["link"] = uplink->ethernetLinkUp();
    eth["ip"] = uplink->getEthernetIP().toString();
    eth["mode"] = configManager.ethStatic ? "static" : "dhcp";
#endif
#if defined(HAS_LTE)
    const LteModem &lte = uplink->getLte();
    JsonObject l = doc.createNestedObject("lte");
    l["state"] = lte.getStateName();
    l["ip"] = lte.localIP().toString();
    l["csq"] = lte.getSignalQuality();
    l["rssi"] = lte.getRssiDbm();
    l["operator"] = lte.getOperator();
    l["apn"] = lte.getApn();
    l["module"] = lte.getModuleName();
    l["error"] = lte.getLastError();
#endif
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

// Also served in AP mode, so ethernet / LTE settings stay reachable from the
// access point (e.g. LTE-only installs or a wrong static address)
void WebPortal::handleNetwork(AsyncWebServerRequest *request)
{
    request->send(200, "text/html", HTMLGenerator::generateNetworkPage(configManager, wifiSSID, wifiPassword, timezone, isAPMode));
}

#if defined(HAS_ETHERNET) || defined(HAS_LTE)
// One form per section (hidden field "section"), both apply after a restart
void WebPortal::handleNetworkPost(AsyncWebServerRequest *request)
{
    auto param = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    String section = param("section");

    if (false)
    {
    }
#if defined(HAS_ETHERNET)
    else if (section == "ethernet")
    {
        bool useStatic = param("ethMode") == "static";
        String error = configManager.setEthernetConfig(useStatic, param("ip"), param("mask"), param("gateway"),
                                                       param("dns1"), param("dns2"));
        if (error.length() > 0)
        {
            request->send(400, "text/plain", error);
            return;
        }
        logger.info("Ethernet configuration updated - " +
                    String(useStatic ? "static " + configManager.ethIp : String("DHCP")));
    }
#endif
#if defined(HAS_LTE)
    else if (section == "lte")
    {
        bool enabled = request->hasParam("lteEnabled", true);
        String apn = param("apn");
        String pin = param("pin");
        bool pinTyped = pin.length() > 0;
        // Empty PIN = keep the stored one, unless removal was asked for
        if (pin.length() == 0 && !request->hasParam("clearPin", true))
        {
            pin = configManager.ltePin;
        }
        if (!configManager.setLteConfig(enabled, apn, pin))
        {
            request->send(400, "text/plain", "Invalid LTE settings (PIN 4-8 digits, APN max 63 characters: letters, digits, . - _)");
            return;
        }
        if (pinTyped)
        {
            configManager.setLtePinRejected(false); // re-entered: try it once more
        }
        logger.info("LTE configuration updated - enabled: " + String(enabled ? "yes" : "no") +
                    ", APN: " + (apn.length() > 0 ? apn : String("automatic")));
    }
#endif
    else
    {
        request->send(400, "text/plain", "Unknown settings section");
        return;
    }

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta http-equiv='refresh' content='15;url=/network'>";
    html += "<title>Settings Saved</title></head>";
    html += "<body><h1>Settings Saved</h1>";
    html += "<p>The device will restart in a few seconds.";
#if defined(HAS_ETHERNET)
    if (section == "ethernet" && configManager.ethStatic)
    {
        String ip = HTMLGenerator::htmlEscape(configManager.ethIp);
        html += " Ethernet address: <a href='http://" + ip + "/network'>" + ip + "</a>";
    }
#endif
    html += "</p></body></html>";
    request->send(200, "text/html", html);
    // Deferred, see handleClient()
    restartAt = millis() + 1500;
    restartRequested = true;
}
#endif

// Handle firmware version request
void WebPortal::handleFirmwareVersion(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /firmware/version");
    if (guardAPOnly(request)) return;
    
    String json = "{\"version\":\"" + String(FIRMWARE_VERSION) + "\"}";
    request->send(200, "application/json", json);
}

// Write-only stream that keeps at most `limit` bytes (write() fails beyond
// it, which ends HTTPClient::writeToStream)
class BoundedText : public Stream
{
public:
    explicit BoundedText(size_t limit) : limit(limit) {}
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    size_t write(uint8_t c) override
    {
        if (content.length() >= limit)
        {
            return 0;
        }
        content += (char)c;
        return 1;
    }
    size_t write(const uint8_t *buffer, size_t size) override
    {
        if (content.length() + size > limit)
        {
            return 0;
        }
        content.concat((const char *)buffer, size);
        return size;
    }
    String content;

private:
    size_t limit;
};

// One TLS session needs ~35-40 kB in total (16 kB record buffers each way);
// its largest single allocation is ~17 kB
static constexpr uint32_t TLS_MIN_FREE_HEAP = 48000;
static constexpr uint32_t TLS_MIN_FREE_BLOCK = 24000;

// Why an HTTPS request cannot start, empty if it can
static String httpsBlocker()
{
    if (time(nullptr) < 1700000000)
    {
        return "The clock is not synchronized yet (needed to check certificates)";
    }
    if (ESP.getFreeHeap() < TLS_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < TLS_MIN_FREE_BLOCK)
    {
        return "Not enough free memory for HTTPS (" + String(ESP.getFreeHeap()) + " B free, largest block " +
               String(ESP.getMaxAllocHeap()) + " B)";
    }
    return "";
}

// HTTPS only, server certificate checked against the built-in CA bundle
static bool beginHttps(HTTPClient &http, NetworkClientSecure &client, const String &url)
{
    if (!url.startsWith("https://"))
    {
        return false;
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    client.useBuiltinCACertBundle();
    client.setHandshakeTimeout(10);
    http.setConnectTimeout(5000);
    http.setTimeout(10000);
    http.setReuse(false);
    return http.begin(client, url);
}

// Handle firmware check request: the loop task fetches the manifest
// (runUpdateCheck); this answers 202 until the result is there
void WebPortal::handleFirmwareCheck(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /firmware/check");
    if (guardAPOnly(request)) return;

    int status = 202;
    String body = "{\"status\":\"checking\"}";
    {
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        if (updateCheckDone && millis() - updateCheckAt < 60000)
        {
            status = updateCheckStatus;
            body = updateCheckResult;
            updateCheckDone = false;
        }
        else if (!updateCheckRunning)
        {
            // Polls while the check runs must not queue another one
            updateCheckRequested = true;
            updateCheckRunning = true;
        }
    }
    request->send(status, "application/json", body);
}

void WebPortal::runUpdateCheck()
{
    int status = 500;
    String result;
    String blocker = httpsBlocker();
    bool clockSet = time(nullptr) >= 1700000000;
    if (blocker.length() > 0 && clockSet && mqttManager && mqttManager->isConnected() && mqttManager->usesTls())
    {
        // Short of memory with an MQTT TLS session up: free it for the check
        // (MQTT reconnects by itself)
        mqttManager->disconnect();
        blocker = httpsBlocker();
    }
    if (blocker.length() > 0)
    {
        result = "{\"error\":\"" + blocker + "\"}";
    }
    else
    {
        WatchdogPause pause;
        NetworkClientSecure client;
        HTTPClient http;
        if (!beginHttps(http, client, FIRMWARE_UPDATE_URL))
        {
            result = "{\"error\":\"HTTP begin failed\"}";
        }
        else
        {
            http.addHeader("User-Agent", "expLORA-Gateway/" + String(FIRMWARE_VERSION));
            int httpCode = http.GET();
            // The manifest is a few hundred bytes (size -1 = chunked, from our server)
            if (httpCode == HTTP_CODE_OK && http.getSize() < 2048)
            {
                status = 200;
                // Bounded read; writeToStream also decodes a chunked reply
                BoundedText text(2048);
                if (http.writeToStream(&text) < 0)
                {
                    status = 500;
                    result = "{\"error\":\"Update manifest too large or unreadable\"}";
                }
                else
                {
                    result = text.content;
                    logger.info("Firmware check successful");
                }
            }
            else
            {
                logger.error("Firmware check failed: HTTP " + String(httpCode));
                result = "{\"error\":\"Failed to check remote version\",\"code\":" + String(httpCode) + "}";
            }
            http.end();
        }
    }
    std::lock_guard<std::mutex> lock(otaUrlMutex);
    updateCheckStatus = status;
    updateCheckResult = result;
    updateCheckDone = true;
    updateCheckRunning = false;
    updateCheckAt = millis();
}

// Handle firmware update request: validated here, downloaded by the loop
// task (runUrlOta), so the response is sent and the async server stays free
void WebPortal::handleFirmwareUpdate(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/update");
    if (guardAPOnly(request)) return;

    String url = request->hasParam("url", true) ? request->getParam("url", true)->value() : String();
    url.trim();
    if (!url.startsWith("https://") || url.length() > 256)
    {
        request->send(400, "application/json", "{\"error\":\"Firmware URL must be https://\"}");
        return;
    }
    // Fail now rather than answer "started" and give up in the loop task
    String blocker = httpsBlocker();
    if (blocker.length() > 0 && !(mqttManager && mqttManager->isConnected() && mqttManager->usesTls()))
    {
        request->send(503, "application/json", "{\"error\":\"" + blocker + "\"}");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        if (otaUploadActive || urlOtaRunning || pendingOtaUrl.length() > 0 || restartRequested)
        {
            request->send(409, "application/json", "{\"error\":\"A firmware update is already running\"}");
            return;
        }
        pendingOtaUrl = url;
    }
    logger.info("Firmware update requested from: " + (url.indexOf('?') >= 0 ? url.substring(0, url.indexOf('?')) : url));
    request->send(202, "application/json",
                  "{\"status\":\"update-started\",\"message\":\"Downloading firmware. The device restarts when done.\"}");
}

// Loop task: download into the OTA slot and restart. Blocks the loop for the
// download; the device restarts afterwards either way.
void WebPortal::runUrlOta(const String &url)
{
    // Free the heap of an MQTT TLS session for this one
    if (mqttManager)
    {
        mqttManager->disconnect();
    }
    String blocker = httpsBlocker();
    if (blocker.length() > 0)
    {
        logger.error("Firmware download: " + blocker);
        return;
    }
    WatchdogPause pause;
    NetworkClientSecure client;
    HTTPClient http;
    if (!beginHttps(http, client, url))
    {
        logger.error("Firmware download: HTTP begin failed");
        return;
    }
    http.addHeader("User-Agent", "expLORA-Gateway/" + String(FIRMWARE_VERSION));
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        logger.error("Firmware download failed: HTTP " + String(httpCode));
        http.end();
        return;
    }

    // A known size keeps the slot check exact and avoids chunked framing in
    // the image (the raw stream is read below)
    const int contentLength = http.getSize();
    const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
    if (!ota || contentLength <= (int)sizeof(esp_image_header_t) || (size_t)contentLength > ota->size)
    {
        logger.error("Firmware download: bad size " + String(contentLength) + " (server must send Content-Length)");
        http.end();
        return;
    }

    // The filesystem stays mounted: OTA writes only the other app slot, and
    // unmounting would race with the other task's file writes
    if (!otaBegin((size_t)contentLength))
    {
        logger.error("Update.begin failed: " + String(Update.getError()));
        http.end();
        return;
    }

    NetworkClient *stream = http.getStreamPtr();
    static uint8_t buf[2048];
    size_t written = 0;
    int lastPct = -1;
    unsigned long started = millis();
    unsigned long lastData = started;
    String error;
    while (written < (size_t)contentLength)
    {
        // Generous: PPP over the modem UART manages ~10 kB/s
        if (millis() - started > 900000)
        {
            error = "download took over 15 minutes";
            break;
        }
        size_t avail = stream->available();
        // The image header must arrive in one piece for the chip check
        if (avail == 0 || (written == 0 && avail < sizeof(esp_image_header_t)))
        {
            if (!http.connected() && stream->available() == 0)
            {
                error = "connection closed";
                break;
            }
            if (millis() - lastData > 15000)
            {
                error = "no data for 15 s";
                break;
            }
            delay(5);
            continue;
        }
        size_t chunk = min(avail, min(sizeof(buf), (size_t)contentLength - written));
        int r = stream->readBytes(buf, chunk);
        if (r <= 0)
        {
            continue;
        }
        if (written == 0 && !firmwareImageMatches(buf, r, error))
        {
            break;
        }
        if (otaWrite(buf, r) != (size_t)r)
        {
            error = "flash write failed";
            break;
        }
        written += r;
        lastData = millis();
        int pct = (int)(written * 100ULL / (size_t)contentLength);
        if (pct / 10 != lastPct / 10)
        {
            logger.info("OTA progress: " + String(pct) + "%");
            lastPct = pct;
        }
    }
    http.end();

    if (error.length() == 0 && !otaEnd())
    {
        error = "Update.end error " + String(Update.getError());
    }
    if (error.length() > 0)
    {
        logger.error("Firmware update aborted: " + error);
        Update.abort();
        return;
    }
    logger.info("OTA complete, restarting");
    delay(500);
    ESP.restart();
}

void WebPortal::handleFirmwarePage(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /firmware");
    if (guardAPOnly(request)) return;
    String html = HTMLGenerator::generateFirmwarePage();
    request->send(200, "text/html", html);
}

// Upload failed or was interrupted: release the OTA slot
void WebPortal::abortUpload(const String &reason)
{
    if (otaUploadActive)
    {
        Update.abort();
        otaUploadActive = false;
    }
    if (!otaUploadHasError)
    {
        otaUploadHasError = true;
        otaUploadErrorMsg = reason;
    }
    logger.error("Firmware upload failed: " + reason);
}

// POST /firmware - completion callback (sending response)
void WebPortal::handleFirmwareUploadComplete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware (complete)");
    if (guardAPOnly(request)) return;
    // Only the request that ran the upload gets its result (a duplicate
    // POST whose data was ignored must not report success)
    if (otaUploadOwner != request)
    {
        request->send(409, "application/json", "{\"error\":\"No upload by this request (another one in progress?)\"}");
        return;
    }

    bool ok = !otaUploadHasError && otaUploadWritten > 0 && Update.isFinished();
    if (!ok)
    {
        bool failed = otaUploadHasError;
        String reason = failed ? otaUploadErrorMsg : String("No firmware received");
        abortUpload(reason);
        request->send(failed ? 500 : 400, "application/json",
                      "{\"error\":\"" + HTMLGenerator::htmlEscape(reason) + "\"}");
        otaUploadHasError = false;
        otaUploadErrorMsg = "";
        otaUploadExpected = otaUploadWritten = 0;
        return;
    }

    request->send(200, "application/json",
                  "{\"status\":\"update-started\",\"message\":\"Firmware uploaded. Installing… device will reboot automatically.\",\"bytes\":" +
                      String(otaUploadWritten) + "}");
    // Deferred, see handleClient()
    restartAt = millis() + 1500;
    restartRequested = true;
}

// Upload chunk callback
void WebPortal::handleFirmwareUploadChunk(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
    // Upload data arrives before the middleware runs - check the login here
    if (!auth.isAuthorized(request) || !auth.hasPassword())
    {
        return;
    }
    // One upload at a time: data of any other request is ignored (its
    // completion callback answers 409)
    if (otaUploadOwner != nullptr && otaUploadOwner != request)
    {
        return;
    }
    if (index > 0 && otaUploadOwner == nullptr)
    {
        return;
    }
    if (index == 0 && guardAPOnly(request))
    {
        return;
    }
    // A second file part in the same upload: never flash or report it, and
    // do not boot into the first one either
    if (index == 0 && otaUploadOwner == request && otaUploadSawFile)
    {
        if (!otaUploadHasError)
        {
            otaUploadActive = otaUploadActive || Update.isRunning();
            abortUpload("Only one firmware file per upload");
            {
                FlashWriteGuard guard; // writes the otadata partition
                esp_ota_set_boot_partition(esp_ota_get_running_partition());
            }
            restartRequested = false;
        }
        return;
    }
    if (index == 0)
    {
        // Upload start (first chunk)
        otaUploadHasError = false;
        otaUploadErrorMsg = "";
        otaUploadExpected = request->contentLength(); // HINT: may be inaccurate with multipart
        otaUploadWritten = 0;
        otaHeaderLen = 0;

        logger.info("Firmware upload started: " + filename + " size=" + String(otaUploadExpected));
        if (otaUploadActive)
        {
            // Leftover of a finished owner (no live request holds it)
            abortUpload("interrupted");
            otaUploadHasError = false;
            otaUploadErrorMsg = "";
        }
        // Client disconnecting mid-upload: no completion callback follows
        otaUploadOwner = request;
        otaUploadSawFile = true;
        request->onDisconnect([this, request]()
                              {
            // Only the request that owns the upload may abort it
            if (otaUploadActive && otaUploadOwner == request && !Update.isFinished())
            {
                abortUpload("client disconnected");
            }
            if (otaUploadOwner == request)
            {
                otaUploadOwner = nullptr;
                otaUploadSawFile = false;
            } });

        // Target OTA slot
        const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
        if (!ota)
        {
            otaUploadHasError = true;
            otaUploadErrorMsg = "No OTA partition";
            return;
        }

        // If server/client specified expected size, at least verify it fits in slot
        // contentLength() includes the multipart framing (a few hundred bytes)
        if (otaUploadExpected > ota->size + 1024)
        {
            otaUploadHasError = true;
            otaUploadErrorMsg = "Firmware too large for OTA slot";
            return;
        }

        bool urlBusy;
        {
            std::lock_guard<std::mutex> lock(otaUrlMutex);
            urlBusy = urlOtaRunning || pendingOtaUrl.length() > 0;
        }
        if (urlBusy || restartRequested)
        {
            otaUploadHasError = true;
            otaUploadErrorMsg = "Another firmware update is running";
            return;
        }
        // IMPORTANT: use size UNKNOWN for multipart - otherwise risk "premature end"
        if (!otaBegin((size_t)UPDATE_SIZE_UNKNOWN))
        {
            otaUploadHasError = true;
            otaUploadErrorMsg = "Update.begin failed: " + String(Update.getError());
            return;
        }
        otaUploadActive = true;
    }

    if (otaUploadHasError)
        return;

    // Write chunk
    if (len)
    {
        // Safety: never overwrite OTA slot size
        const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
        if (ota && (otaUploadWritten + len) > ota->size)
        {
            abortUpload("Data exceed OTA slot size");
            return;
        }

        // Hold back the first bytes until the whole image header is there
        if (otaUploadWritten == 0)
        {
            size_t need = sizeof(esp_image_header_t) - otaHeaderLen;
            size_t take = std::min(need, len);
            memcpy(otaHeader + otaHeaderLen, data, take);
            otaHeaderLen += take;
            data += take;
            len -= take;
            if (otaHeaderLen < sizeof(esp_image_header_t))
            {
                return;
            }
            String imageError;
            if (!firmwareImageMatches(otaHeader, otaHeaderLen, imageError))
            {
                abortUpload(imageError);
                return;
            }
            if (otaWrite(otaHeader, otaHeaderLen) != otaHeaderLen)
            {
                abortUpload("Short write of the image header");
                return;
            }
            otaUploadWritten = otaHeaderLen;
            if (len == 0)
            {
                return;
            }
        }

        size_t w = otaWrite(data, len);
        if (w != len)
        {
            abortUpload("Short write: " + String(w) + "/" + String(len));
            return;
        }
        otaUploadWritten += w;

        // Progress log
        if (otaUploadExpected > 0)
        {
            int pct = (int)((otaUploadWritten * 100ULL) / (size_t)otaUploadExpected);
            if (pct == 1 || pct % 5 == 0 || pct >= 99)
            {
                logger.info("OTA progress: " + String(pct) + "% (" + String(otaUploadWritten) + " / " + String(otaUploadExpected) + " bytes)");
            }
        }
        else if ((otaUploadWritten % (128 * 1024)) < len)
        {
            logger.info("OTA written: " + String(otaUploadWritten) + " bytes (chunked)");
        }

        // Yield + WDT
        esp_task_wdt_reset();
        delay(0);
    }

    if (final)
    {
        logger.info("Firmware upload finished: total " + String(otaUploadWritten) + " bytes");

        if (otaUploadExpected > 0 && otaUploadWritten != otaUploadExpected)
        {
            logger.warning("Upload size hint mismatch: written=" + String(otaUploadWritten) + " expected=" + String(otaUploadExpected));
        }

        // IMPORTANT: finish multipart upload with evenIfRemaining=true
        if (!otaEnd(true) || !Update.isFinished())
        {
            abortUpload("Update.end error: " + String(Update.getError()));
            return;
        }
        otaUploadActive = false;
        // Restart even if the client is gone before the completion callback
        restartAt = millis() + 1500;
        restartRequested = true;
        // response will be sent by handleFirmwareUploadComplete()
    }
}
