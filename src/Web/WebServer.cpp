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
#include "../Hardware/Liveness.h"
#include "HTMLGenerator.h"
#include "WebAssets.h"
#include "ApiRow.h"
#include "../Data/TimeZones.h"
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
#include <esp_app_format.h>
#include <NetworkClientSecure.h>
#include "../config.h"
#include "../Hardware/WatchdogPause.h"
#include "../Update/UpdateMode.h"
#include "../Hardware/HeapGuard.h"
#include "../Hardware/FlashPermit.h"
#include "../Hardware/RadioTask.h"
#include "../Storage/OtaStore.h"
#include "../Protocol/OtaImageSigned.h"
#include "../Update/SensorFetch.h"
#include "../Update/UpdateCheck.h"
#include "../Update/AutoUpdate.h"
#include "../Update/LayoutMigration.h"
#include "../Data/ForwardUrl.h"
#include <sys/time.h>
#include <atomic>

// Request attribute set on a request the patched parser must drop. The
// library keys attributes by pointer, so both sides use this one symbol
// (declared extern in scripts/patch_asyncwebserver.py)
extern const char EXPLORA_REJECT_ATTR[] = "explora_reject";

// Counted by the patched libraries (scripts/patch_asyncwebserver.py), shown
// in /diag/heap: connections reset at accept and requests dropped while
// being received because the 8-bit heap was short (or deadline / size limit).
// recvDeferred counts each delivery of incoming data that lwIP had to keep
// because the heap was short. Atomic: accept and the deferral run on the
// lwIP thread, the receive gate in the AsyncTCP task, /diag/heap reads them
// from the AsyncTCP task
std::atomic<uint32_t> exploraAcceptRefused{0};
std::atomic<uint32_t> exploraRecvDeferred{0};
// Events the AsyncTCP queue (64) could not take at once and kept in order
// behind it (lwIP thread)
std::atomic<uint32_t> exploraQueueOverflow{0};
// Longest the overflow list got; events lost (malloc failed and no reserve
// packet / queue gone: FIN, ERROR, SENT)
std::atomic<uint32_t> exploraQueueOverflowHigh{0};
std::atomic<uint32_t> exploraEventLost{0};
std::atomic<uint32_t> exploraReceiveDropped{0};

// Set on a request whose handler installs its own request->onDisconnect
// (the uploads). The library keeps one disconnect callback per request:
// rejectLowMemory() must not replace it, nor be replaced by it
static const char OWN_DISCONNECT_ATTR[] = "explora_own_disconnect";

// Requests whose body would be parsed into RAM are limited (the body is read
// before the login middleware runs); firmware uploads are streamed instead
static constexpr size_t MAX_REQUEST_BODY = 4096; // largest real form ~1 kB

// Receive timeout of a logged-in upload (the library sets 3 s per
// connection): longer than a heap shortage lasts under load (the Pro
// recovers within ~3 s after attacks.py "6 big held"); a longer silence
// closes the connection and the upload is aborted with an error
static constexpr uint32_t UPLOAD_RX_TIMEOUT_S = 30;

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
        bool upload = request->method() == HTTP_POST &&
                      (request->url() == "/firmware" || request->url() == "/ota/upload") &&
                      request->contentType().startsWith("multipart/form-data");
        if (upload && auth.hasPassword() && auth.isAuthorized(request))
        {
            // The patched AsyncTCP defers incoming data while the 8-bit heap
            // is short (scripts/patch_asyncwebserver.py, _tcp_recv); deferred
            // data does not refresh the client's rx timestamp, so the
            // library's 3 s rx timeout would reset an upload during a short
            // heap. A logged-in upload gets UPLOAD_RX_TIMEOUT_S instead (set
            // in the AsyncTCP task, which also runs the timeout check)
            request->client()->setRxTimeout(UPLOAD_RX_TIMEOUT_S);
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

// Constructor
WebPortal::WebPortal(SensorManager &sensors, Logger &log, String &ssid, String &password,
                     bool &config_mode, ConfigManager &config)
    : server(HTTP_PORT), sensorManager(sensors), logger(log), isAPMode(false),
      wifiSSID(ssid), wifiPassword(password), configMode(config_mode),
      configManager(config), auth(config, log), mqttManager(nullptr)
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
static unsigned heavyResponsesRunning(); // the heavy response slots, below
static bool rejectLowMemory(AsyncWebServerRequest *request, size_t peak, size_t block);
static size_t stringPagePeak(size_t pageSize);

bool WebPortal::init()
{
    logger.info("Initializing web portal");

    HTMLGenerator::setAdvanced(configManager.uiAdvanced);

    // Captive portal while the AP runs without local network access (see Uplink)
    if (uplink->isCaptive())
    {
        setAPMode(true);
    }

    // Setup routes (idempotent)
    setupRoutes();

    // Update mode: no session while an upload runs, running heavy pages
    // finish before its memory check (Update/UpdateMode.h)
    UpdateMode::setUploadBusy([this]()
                              { return otaUploadOwner != nullptr || otaUploadActive || imgUploadOwner != nullptr ||
                                       OtaStore::uploadRunning(); });
    UpdateMode::setHeavyActive(heavyResponsesRunning);
    UpdateMode::setRestartPending([this]()
                                  { return (bool)restartRequested; });

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

// Asset from flash (no copy), cached for a year: its URL changes with its
// content (WebAssets::TOKEN)
static void sendAsset(AsyncWebServerRequest *request, const char *type, const char *data, size_t len)
{
    AsyncWebServerResponse *response = request->beginResponse(200, type, (const uint8_t *)data, len);
    response->addHeader("Cache-Control", "public, max-age=31536000, immutable");
    request->send(response);
}

// One route to a handler of this class: a single lambda type for every
// route (std::bind per route cost ~100 B of code each)
void WebPortal::route(const char *uri, WebRequestMethodComposite method, Handler handler)
{
    server.on(uri, method, [this, handler](AsyncWebServerRequest *request) { (this->*handler)(request); });
}

// Setup routes for web server
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
    route("/login", HTTP_GET, &WebPortal::handleLogin);
    route("/login", HTTP_POST, &WebPortal::handleLoginPost);
    route("/logout", HTTP_GET, &WebPortal::handleLogout);
    route("/password", HTTP_GET, &WebPortal::handlePassword);
    route("/password", HTTP_POST, &WebPortal::handlePasswordPost);
    route("/system/mode", HTTP_POST, &WebPortal::handleSystemMode);

    route("/", HTTP_GET, &WebPortal::handleRoot);
    // Shared stylesheet and script (public, cached; the pages link them
    // with the version token)
    server.on("/a.css", HTTP_GET, [](AsyncWebServerRequest *request)
              { sendAsset(request, "text/css", WebAssets::CSS, sizeof(WebAssets::CSS) - 1); });
    server.on("/a.js", HTTP_GET, [](AsyncWebServerRequest *request)
              { sendAsset(request, "text/javascript", WebAssets::JS, sizeof(WebAssets::JS) - 1); });
    route("/config", HTTP_GET, &WebPortal::handleConfig);
    route("/config", HTTP_POST, &WebPortal::handleConfigPost);
    route("/timezone", HTTP_POST, &WebPortal::handleTimezonePost);

    // Sensor management
    route("/sensors/add", HTTP_GET, &WebPortal::handleSensorAdd);
    route("/sensors/add", HTTP_POST, &WebPortal::handleSensorAddPost);
    route("/sensors/edit", HTTP_GET, &WebPortal::handleSensorEdit);
    route("/sensors/update", HTTP_POST, &WebPortal::handleSensorEditPost);
    route("/sensors/delete", HTTP_GET, &WebPortal::handleSensorDelete);
    route("/sensors/add2", HTTP_GET, &WebPortal::handleGen2Add);
    route("/sensors/add2", HTTP_POST, &WebPortal::handleGen2AddPost);
    route("/sensors/delete2", HTTP_POST, &WebPortal::handleGen2Delete);
    route("/sensors/gen2/config", HTTP_POST, &WebPortal::handleGen2Config);
    route("/sensors/gen2/edit", HTTP_POST, &WebPortal::handleGen2Edit);
    route("/sensors/gen2/status", HTTP_POST, &WebPortal::handleGen2Status);
    route("/sensors/gen2/resetnonce", HTTP_POST, &WebPortal::handleGen2ResetNonce);
    route("/sensors/gen2/ota", HTTP_POST, &WebPortal::handleGen2Ota);
    route("/sensors/gen2", HTTP_GET, &WebPortal::handleGen2Detail);
    // OTA image store (sensor images, uploaded streamed to LittleFS)
    server.on(
        "/ota/upload", HTTP_POST, [this](AsyncWebServerRequest *request) { this->handleOtaImageComplete(request); },
        [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            this->handleOtaImageChunk(request, filename, index, data, len, final);
        });
    route("/ota/delete", HTTP_POST, &WebPortal::handleOtaImageDelete);
    // Before POST /ota/fetch (that handler also matches "/ota/fetch/...")
    route("/ota/fetch/status", HTTP_GET, &WebPortal::handleOtaFetchStatus);
    route("/ota/fetch", HTTP_POST, &WebPortal::handleOtaFetch);
    route("/sensors", HTTP_GET, &WebPortal::handleSensors);

    // Logs
    route("/logs/clear", HTTP_GET, &WebPortal::handleLogsClear);
    route("/logs/level", HTTP_POST, &WebPortal::handleLogLevel);
    route("/logs", HTTP_GET, &WebPortal::handleLogs);

    // Network (the AP setup page posts its WiFi form to /config)
    route("/network", HTTP_GET, &WebPortal::handleNetwork);
    route("/network", HTTP_POST, &WebPortal::handleNetworkPost);

    // MQTT
    route("/mqtt", HTTP_GET, &WebPortal::handleMqtt);
    route("/mqtt", HTTP_POST, &WebPortal::handleMqttPost);

    // API
    route("/api", HTTP_GET, &WebPortal::handleAPI);

    // Diagnostics
    route("/diag/heap", HTTP_GET, &WebPortal::handleDiagHeap);
    route("/diag/network", HTTP_GET, &WebPortal::handleDiagNetwork);

    // Firmware update endpoints
    route("/firmware/version", HTTP_GET, &WebPortal::handleFirmwareVersion);
    route("/firmware/check", HTTP_GET, &WebPortal::handleCheckStatus);
    route("/firmware/check", HTTP_POST, &WebPortal::handleCheckStart);
    route("/firmware/install", HTTP_POST, &WebPortal::handleFirmwareInstall);
    route("/firmware/auto", HTTP_POST, &WebPortal::handleAutoUpdatePost);
    route("/firmware/update", HTTP_POST, &WebPortal::handleFirmwareUpdate);
    route("/firmware", HTTP_GET, &WebPortal::handleFirmwarePage);
    route("/firmware/signed", HTTP_POST, &WebPortal::handleSignedOnlyPost);
    // Before POST /firmware (that handler also matches "/firmware/...")
    route("/firmware/confirm", HTTP_POST, &WebPortal::handleFirmwareConfirm);
    server.on("/firmware", HTTP_POST,
        [this](AsyncWebServerRequest *request) { this->handleFirmwareUploadComplete(request); },
        [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            this->handleFirmwareUploadChunk(request, filename, index, data, len, final);
        }
    );

    // Reboot
    route("/reboot", HTTP_GET, &WebPortal::handleReboot);

    // Unknown pages (404)
    server.onNotFound(std::bind(&WebPortal::handleNotFound, this, std::placeholders::_1));

    routesRegistered = true;
    logger.info("Routes registered");
}

// Paths reachable without a login: the login page itself, the read-only
// sensor data API, the version probe and the shared assets (the login,
// update progress and access point setup pages use them)
// (GET only: POST /firmware/version would otherwise reach the /firmware
// upload handler by prefix match)
static bool isPublicPath(AsyncWebServerRequest *request)
{
    const String &url = request->url();
    if (url == "/login")
    {
        return request->method() == HTTP_GET || request->method() == HTTP_POST;
    }
    return request->method() == HTTP_GET &&
           (url == "/api" || url == "/firmware/version" || url == "/a.css" || url == "/a.js");
}

// Light pages that stay available during an update-mode session: the
// login, the firmware page (a small status page then) and its status polls,
// the diagnostics. Everything else is answered 503 then: heavy pages would
// compete with the TLS session for the heap, and actions (sensor changes,
// OTA jobs, settings, uploads) wait for the session to end. /reboot too: the
// loop task that would restart is busy with the session, a restart after it
// would only discard its result, and the session ends by itself (hard cap)
static bool availableInUpdateMode(AsyncWebServerRequest *request)
{
    const String &url = request->url();
    if (request->method() == HTTP_POST)
    {
        return url == "/login" || url == "/password";
    }
    return request->method() == HTTP_GET &&
           (url == "/login" || url == "/logout" || url == "/password" || url == "/firmware" ||
            url == "/firmware/check" || url == "/firmware/version" || url == "/diag/heap" ||
            url == "/diag/network" || url == "/ota/fetch/status" || url == "/a.css" || url == "/a.js");
}

// 503 during an update-mode session. Logged-in clients learn why; the
// public paths (/api) get a generic text
static void sendUpdateInProgress(AsyncWebServerRequest *request, bool authorized)
{
    String text;
    if (!authorized)
    {
        text = "Update in progress - try again in a few minutes.";
    }
    else if (request->url() == "/reboot")
    {
        text = UpdateMode::refusalText() + " A restart now would only interrupt the update; the session ends by "
                                           "itself within " + String(UpdateMode::HARD_CAP_MS / 60000) + " minutes.";
    }
    else
    {
        text = UpdateMode::refusalText();
    }
    AsyncWebServerResponse *response = request->beginResponse(503, "text/plain", text);
    response->addHeader("Retry-After", "60");
    request->send(response);
}

void WebPortal::requireLogin(AsyncWebServerRequest *request, ArMiddlewareNext next)
{
    const String &url = request->url();
    const bool updateBlocked = UpdateMode::active() && !availableInUpdateMode(request);
    if (isPublicPath(request))
    {
        if (updateBlocked)
        {
            sendUpdateInProgress(request, false);
            return;
        }
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
    // Update mode: only after the login check (the reason is not for anyone)
    if (updateBlocked)
    {
        sendUpdateInProgress(request, true);
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

// System page: during an update-mode session only the password form (light)
static SystemView systemView(ConfigManager &config)
{
    SystemView v;
    v.full = !UpdateMode::active();
    std::lock_guard<std::recursive_mutex> guard(config.mutex());
    v.timezone = config.timezone;
    v.timezoneName = config.timezoneName;
    return v;
}

// The page with the time zone list (~5.5 kB)
static constexpr size_t SYSTEM_PAGE_SIZE = 6144;

void WebPortal::handlePassword(AsyncWebServerRequest *request)
{
    const SystemView view = systemView(configManager);
    // During an update-mode session only the light password form (~2 kB),
    // sent like the firmware page's status page: the admission for the full
    // page would refuse it on the session's TLS reserve
    if (view.full && rejectLowMemory(request, stringPagePeak(SYSTEM_PAGE_SIZE), SYSTEM_PAGE_SIZE)) return;
    request->send(200, "text/html", HTMLGenerator::generatePasswordPage(auth.hasPassword(), "", isAPMode, view));
}

// Interface mode (Simple / Advanced), the same for everyone
void WebPortal::handleSystemMode(AsyncWebServerRequest *request)
{
    const String mode = request->hasParam("mode", true) ? request->getParam("mode", true)->value() : String();
    if (mode != "simple" && mode != "advanced")
    {
        request->send(400, "text/plain", "Unknown mode");
        return;
    }
    const bool advanced = mode == "advanced";
    if (!configManager.setUiAdvanced(advanced))
    {
        request->send(500, "text/plain", "Failed to save configuration");
        return;
    }
    HTMLGenerator::setAdvanced(advanced);
    logger.info(String("Web interface: ") + (advanced ? "Advanced" : "Simple") + " mode");
    request->redirect("/password");
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
        request->send(400, "text/html",
                      HTMLGenerator::generatePasswordPage(auth.hasPassword(), error, isAPMode, systemView(configManager)));
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
    String otaUrl, fetchUrl;
    uint64_t otaSession = 0;
    Manifest::Expect otaExpect, fetchExpect;
    bool checkUpdate;
    {
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        otaUrl = pendingOtaUrl;
        otaSession = pendingOtaSession;
        otaExpect = pendingOtaExpect;
        pendingOtaExpect = Manifest::Expect();
        pendingOtaUrl = "";
        if (otaUrl.length() > 0)
        {
            urlOtaRunning = true; // under the lock: an upload start sees it
        }
        // Stays "queued" (busy for the uploads) until runSensorFetch() below
        // starts it: an update check queued meanwhile runs first
        if (fetchState == (uint8_t)SensorFetch::State::Queued)
        {
            fetchUrl = pendingFetchUrl;
            fetchExpect = pendingFetchExpect;
            pendingFetchExpect = Manifest::Expect();
            pendingFetchUrl = "";
            if (fetchUrl.length() == 0)
            {
                // The copy failed (out of memory): not stuck in "queued"
                fetchState = (uint8_t)SensorFetch::State::Failed;
                fetchResult = "out of memory";
            }
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
        runUrlOta(otaUrl, otaSession, otaExpect);
        urlOtaRunning = false;
    }
    if (fetchUrl.length() > 0)
    {
        runSensorFetch(fetchUrl, fetchExpect);
    }

    if (timezoneApplyRequested)
    {
        timezoneApplyRequested = false; // before reading: a newer change sets it again
        String tz;
        {
            std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
            tz = configManager.timezone;
        }
        sensorManager.applyTimezone(tz); // setenv/tzset, keeps today's rain totals
    }

    if (restartRequested && (long)(millis() - restartAt) >= 0 && LayoutMigration::restartRefusal())
    {
        restartRequested = false;
        logger.error(String("Deferred restart refused: ") + LayoutMigration::restartRefusal());
    }
    if (restartRequested && (long)(millis() - restartAt) >= 0)
    {
        logger.info("Deferred restart firing now");
        // A new firmware still waiting for its validation is marked valid
        // first: the restart was asked for in its web UI (an unwanted
        // rollback otherwise; Update/GatewayOta.h)
        GatewayOta::beforeRestart("web UI");
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

// Sensor changes are refused up front while sensors.json was loaded only
// in part (SensorManager::savesBlocked): nothing is changed in RAM, so a
// delete does not look done and come back after the restart, and the other
// failure messages (list full, serial in use) stay accurate
static bool rejectBlockedSave(SensorManager &sm, AsyncWebServerRequest *request)
{
    if (!sm.savesBlocked())
    {
        return false;
    }
    request->send(503, "text/plain",
                  "Not saved: the sensor configuration was loaded only in part at boot (not enough memory) - "
                  "saving would drop sensors. Restart the gateway.");
    return true;
}

// Process HTTP requests

// Heavy responses (pages, /api) running now, and the refusals. Only the
// AsyncTCP task touches them: handlers and the disconnect callback. A slot
// is freed by the request's onDisconnect - or after HEAVY_SLOT_EXPIRY_MS:
// every request is closed after 30 s (patch_asyncwebserver.py
// MAX_REQUEST_LIFETIME_MS), so an older slot belongs to a request whose
// disconnect never came (its connection's last event lost) and would hold
// the slot until a restart
static constexpr uint32_t HEAVY_SLOT_EXPIRY_MS = 45000;
struct HeavySlot
{
    AsyncWebServerRequest *request;
    uint32_t since;
};
static HeavySlot heavySlots[WEB_HEAVY_RESPONSES_MAX];
static std::atomic<unsigned> heavyActive{0};
static unsigned heavyResponsesRunning()
{
    return heavyActive.load();
}
static std::atomic<uint32_t> heavyRefusedBusy{0};
static std::atomic<uint32_t> heavyRefusedLow{0};
static std::atomic<uint32_t> heavyExpired{0};

static void heavyExpire()
{
    unsigned active = 0;
    for (auto &slot : heavySlots)
    {
        if (slot.request && millis() - slot.since >= HEAVY_SLOT_EXPIRY_MS)
        {
            slot.request = nullptr;
            heavyExpired++;
            log_e("heavy response slot expired (disconnect never came)");
        }
        active += slot.request != nullptr;
    }
    heavyActive = active;
}

// Pages and /api: refused with 503 while WEB_HEAVY_RESPONSES_MAX of them
// run or the usable (8-bit) heap is short (HeapGuard::admit), so pages never
// take the memory the rest of the system or an mbedTLS session (URL OTA
// download, update check) needs. peak = the request's transient heap use,
// block = its largest single allocation. An admitted request holds its slot
// until its connection closes (the request and its response are destroyed
// then) through request->onDisconnect: a handler that calls this must never
// set its own onDisconnect (it would replace this one and leak the slot),
// and a request that has one (OWN_DISCONNECT_ATTR, the uploads) is not
// counted here
static bool rejectLowMemory(AsyncWebServerRequest *request, size_t peak, size_t block)
{
    if (request->hasAttribute(OWN_DISCONNECT_ATTR))
    {
        log_e("rejectLowMemory on a request with its own onDisconnect - not counted");
        return false;
    }
    heavyExpire();
    HeapGuard::Admit admit =
        HeapGuard::admit(HeapGuard::usable(), peak, block, heavyActive.load(), WEB_HEAVY_RESPONSES_MAX);
    if (admit == HeapGuard::Admit::Busy)
    {
        heavyRefusedBusy++;
        request->send(503, "text/plain", "Busy, try again");
        return true;
    }
    if (admit == HeapGuard::Admit::LowMemory)
    {
        heavyRefusedLow++;
        request->send(503, "text/plain", "Low memory, try again");
        return true;
    }
    for (auto &slot : heavySlots)
    {
        if (!slot.request)
        {
            slot.request = request;
            slot.since = millis();
            break;
        }
    }
    heavyActive++;
    request->onDisconnect([request]() {
        for (auto &slot : heavySlots)
        {
            if (slot.request == request)
            {
                slot.request = nullptr;
                heavyActive--;
                break;
            }
        }
    });
    return false;
}

// Page built as one String and sent with send()
static size_t stringPagePeak(size_t pageSize)
{
    return HeapGuard::stringPagePeak(pageSize);
}

// /firmware (HTMLGenerator::generateFirmwarePage): measured 4.2 kB (Simple)
// and 4.95 kB (Advanced) on the Pro, 6.5 kB in Advanced with a manifest of
// 8 sensor packages and the update-mode card; ~0.6 kB more with the card of
// an image waiting for its confirmation and texts / versions at their
// longest; the automatic updates card (S6) up to ~1.3 kB with every state
// text - 9 kB leaves ~0.6 kB beyond that
static constexpr size_t FIRMWARE_PAGE_SIZE = 9216;

// Generation 2 detail: Advanced see HTMLGenerator::GEN2_ADV_PAGE_SIZE;
// Simple measured 1.9 kB, + the forward URL field (S7: ~0.4 kB, a
// 200-character URL full of HTML escapes up to ~1 kB more)
static size_t gen2PageSize()
{
    return HTMLGenerator::adv() ? HTMLGenerator::GEN2_ADV_PAGE_SIZE : 4096;
}

// Page streamed piece by piece (chunked): only the piece being sent is in
// RAM, never the whole page nor send()'s copy of it. piece(i) for i in
// 0 .. parts - 1 runs in the AsyncTCP task when the connection has room; it
// may return an empty piece. It must work on data copied beforehand (no
// lock held across pieces)
static void sendStreamed(AsyncWebServerRequest *request, size_t parts, std::function<String(size_t)> piece,
                         bool noStore = false, int code = 200)
{
    struct Stream
    {
        std::function<String(size_t)> piece;
        size_t parts;
        String pending;
        size_t offset = 0;
        size_t part = 0;
    };
    auto state = std::make_shared<Stream>();
    state->piece = std::move(piece);
    state->parts = parts;
    AsyncWebServerResponse *response = request->beginChunkedResponse(
        "text/html", [state](uint8_t *buffer, size_t maxLen, size_t) -> size_t
        {
            while (state->offset >= state->pending.length())
            {
                if (state->part >= state->parts)
                {
                    return 0;
                }
                state->offset = 0;
                state->pending = state->piece(state->part++);
            }
            size_t n = std::min(maxLen, state->pending.length() - state->offset);
            memcpy(buffer, state->pending.c_str() + state->offset, n);
            state->offset += n;
            return n; });
    response->setCode(code);
    if (noStore)
    {
        response->addHeader("Cache-Control", "no-store");
    }
    request->send(response);
}

// Root page
void WebPortal::handleRoot(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /");

    // First setup over the access point: WiFi page
    if (guardAPOnly(request))
    {
        return;
    }

    // Page: ~1.3 kB plus ~0.2 kB per sensor row (measured 1.25 kB with
    // three sensors); the readings copy ~0.4 kB per sensor plus its texts
    size_t rowsBytes = 0;
    const size_t n = sensorManager.readingCount(&rowsBytes);
    const size_t pageSize = 1536 + 256 * n;
    if (rejectLowMemory(request, stringPagePeak(pageSize) + rowsBytes, pageSize)) return;
    std::vector<SensorData> rows = sensorManager.getReadings();
    for (SensorData &s : rows)
    {
        s.deviceKey = 0;
    }
    request->send(200, "text/html",
                  HTMLGenerator::generateHomePage(rows, mqttManager ? mqttManager->statusText() : String("disabled")));
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

    // The access point setup page with the time zone list: ~5.2 kB
    if (rejectLowMemory(request, stringPagePeak(6144), 6144)) return;

    String currentIP = isAPMode ? uplink->getApIP().toString() : uplink->getLocalIP().toString();

    // Generate HTML (use isAPMode to drive UI state)
    String html = HTMLGenerator::generateConfigPage(wifiSSID, wifiPassword, isAPMode, currentIP, configManager);

    // Send response
    request->send(200, "text/html", html);
}

// WiFi credentials: SSID 1..32 characters (IEEE 802.11 limit); password
// empty (open network), an 8..63 character passphrase or a 64 hex digit
// pre-shared key (WPA2-PSK). "" or the error
static String wifiError(const String &ssid, const String &password)
{
    if (ssid.length() == 0)
    {
        return "SSID cannot be empty";
    }
    if (ssid.length() > 32)
    {
        return "SSID too long (max 32 characters)";
    }
    bool hexKey = password.length() == 64;
    for (size_t i = 0; hexKey && i < password.length(); i++)
    {
        hexKey = isHexadecimalDigit(password[i]);
    }
    if (password.length() > 0 && password.length() < 8)
    {
        return "Password must be at least 8 characters or left empty for open networks";
    }
    if (password.length() > 63 && !hexKey)
    {
        return "Password too long (max 63 characters, or a 64 hex digit key)";
    }
    return "";
}

// WiFi settings (and the zone, TimeZones::KEEP = unchanged) saved for the
// restart. Through ConfigManager: both stores (file and Preferences) and
// atomic, so nothing (the timezone) is overridden by a stale copy at boot.
// false: not saved, the running values are kept in both stores
bool WebPortal::saveWifi(const String &newSsid, const String &newPassword, bool enabled, int zone)
{
    FlashWriteGuard flashPermit; // lock order: flash permit before configMutex
    std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
    String oldSsid = configManager.wifiSSID, oldPassword = configManager.wifiPassword;
    bool oldConfigMode = configManager.configMode;
    bool oldWifiEnabled = configManager.wifiEnabled;
    unsigned long oldWifiAttempt = configManager.lastWifiAttempt;
    String oldZoneName = configManager.timezoneName;
    String oldZonePosix = configManager.timezone;
    String oldZoneNamePosix = configManager.timezoneNamePosix;
    if (zone >= 0)
    {
        configManager.setTimezoneFields(zone);
    }
    configManager.wifiEnabled = enabled;
    if (configManager.setWiFiConfig(newSsid, newPassword))
    {
        return true;
    }
    // One store may hold the new values: write the old ones to both, so
    // they agree (as ConfigManager::setTimezone does)
    configManager.wifiSSID = oldSsid;
    configManager.wifiPassword = oldPassword;
    configManager.configMode = oldConfigMode;
    configManager.wifiEnabled = oldWifiEnabled;
    configManager.lastWifiAttempt = oldWifiAttempt;
    configManager.timezoneName = oldZoneName;
    configManager.timezone = oldZonePosix;
    configManager.timezoneNamePosix = oldZoneNamePosix;
    configManager.save();
    return false;
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

    String error = wifiError(newSsid, newPassword);
    if (error.length() > 0)
    {
        request->send(400, "text/plain", error);
        return;
    }

    // Optional (the AP setup page has it): a zone name, or the POSIX string
    // older clients send; applied with the restart
    int zone = TimeZones::KEEP;
    if (request->hasParam("timezone", true))
    {
        zone = TimeZones::fromForm(request->getParam("timezone", true)->value().c_str(), configManager.timezone.c_str());
    }
    if (zone == -1)
    {
        request->send(400, "text/plain", "Unknown time zone");
        return;
    }

    logger.info("New WiFi configuration - SSID: " + newSsid);
    // New credentials are meant to be used: a switched-off WiFi would ignore
    // them after the restart
    if (!saveWifi(newSsid, newPassword, true, zone))
    {
        request->send(500, "text/plain", "Failed to save configuration");
        return;
    }

    if (const char *held = LayoutMigration::restartRefusal())
    {
        request->send(200, "text/html",
                      HTMLGenerator::generateNoticePage(
                          "Saved", String("The WiFi settings are saved but the gateway does not restart: ") + held + ".",
                          30, "/"));
        return;
    }
    request->send(200, "text/html",
                  HTMLGenerator::generateNoticePage("Saved", "The WiFi settings are saved - the gateway restarts.",
                                                    10, "/"));

    // Defer the restart so the async server can finish flushing the response.
    // handleClient() (called from the main loop) will trigger ESP.restart()
    // once restartAt has been reached.
    restartAt = millis() + 1500;
    restartRequested = true;
}

// Time zone from the System page: saved here, applied by the loop at once
// (no restart). setenv() may rewrite the TZ string in place while a
// localtime_r() reads it; the switch happens in the loop task (rain reset,
// most log stamps), but the clock is also read in this task (log stamps, the
// network page clock) and by Uplink, so a local time computed at that very
// moment may be wrong for that one call.
void WebPortal::handleTimezonePost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /timezone");
    String value = request->hasParam("timezone", true) ? request->getParam("timezone", true)->value() : String();
    int zone = TimeZones::fromForm(value.c_str(), configManager.timezone.c_str());
    if (zone == -1)
    {
        request->send(400, "text/plain", "Unknown time zone");
        return;
    }
    if (zone >= 0)
    {
        if (!configManager.setTimezone(zone))
        {
            request->send(500, "text/plain", "Failed to save configuration");
            return;
        }
        timezoneApplyRequested = true;
        logger.info("Time zone set to " + String(TimeZones::ZONES[zone].name));
    }
    request->redirect("/password");
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

    // Copied once (each list under its own lock), then streamed one row per
    // piece: no lock across the callbacks, rows of one moment
    auto view = std::make_shared<SensorsPageView>();
    view->entries = sensorManager.getActiveSensorEntries();
    view->gen2 = sensorManager.getGen2Sensors();
    view->waitingCount = sensorManager.getWaitingList(view->waiting, 8);
    view->nowMillis = millis();
    view->adv = HTMLGenerator::adv();
    // No key outlives this handler: the page shows only the first two bytes
    // of a label key and no generation 1 key
    for (Gen2Sensor &g : view->gen2)
    {
        uint8_t shown[2] = {g.label[0], g.label[1]};
        g.wipeKeys();
        g.label[0] = shown[0];
        g.label[1] = shown[1];
    }
    for (ActiveSensorEntry &e : view->entries)
    {
        e.data.deviceKey = 0;
    }

    // Peak on top of the copy: one piece (a row ~1.5 kB, header / footer /
    // waiting list < 4 kB) + the send buffer + the lwIP in-flight copy
    if (rejectLowMemory(request, HeapGuard::STREAMED_PAGE_PEAK, HeapGuard::STREAMED_PAGE_BLOCK)) return;

    // Key fingerprints: never cached
    sendStreamed(request, HTMLGenerator::sensorsPageParts(*view),
                 [view](size_t part) { return HTMLGenerator::generateSensorsPagePart(*view, part); }, true);
}

// Sensor add page
// Add and edit form: HTMLGenerator::SENSOR_FORM_SIZE
static constexpr size_t SENSOR_FORM_SIZE = HTMLGenerator::SENSOR_FORM_SIZE;

void WebPortal::handleSensorAdd(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /sensors/add");
    if (guardAPOnly(request)) return;
    if (rejectLowMemory(request, stringPagePeak(SENSOR_FORM_SIZE), SENSOR_FORM_SIZE)) return;
    request->send(200, "text/html", HTMLGenerator::generateSensorForm(nullptr, -1));
}

// Sensor form fields shared by add and edit
struct SensorForm
{
    String name;
    SensorType deviceType;
    uint32_t serialNumber;
    uint32_t deviceKey;
    String customUrl;
    SensorCalibration::Set cal;
};

// Correction fields of the form (generation 1 and 2): a field that is not
// posted keeps its value, an empty one is reset (no correction), one posted
// back unchanged is kept (SensorCalibration::parse); the error of the first
// bad field, empty if all are valid
// gen2: values rounded to 0.001 as sensors.json stores them (SensorCalibration::milli)
static String parseCorrections(AsyncWebServerRequest *request, SensorCalibration::Set &cal, bool gen2 = false)
{
    for (int i = 0; i < SensorCalibration::FIELDS; i++)
    {
        const SensorCalibration::Field f = (SensorCalibration::Field)i;
        const SensorCalibration::FieldInfo &fi = SensorCalibration::info(f);
        if (!request->hasParam(fi.form, true))
        {
            continue;
        }
        String text = request->getParam(fi.form, true)->value();
        text.trim();
        const SensorCalibration::Parsed r = SensorCalibration::parse(text.c_str(), f, cal.v[i], cal.v[i]);
        if (r != SensorCalibration::Parsed::Ok)
        {
            return String("Correction ") + fi.name +
                   (r == SensorCalibration::Parsed::NotNumber ? ": not a number" : ": out of range");
        }
        if (gen2)
        {
            cal.v[i] = SensorCalibration::milli(cal.v[i]);
        }
    }
    return "";
}

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

// The "Forward to URL" field of both generations (ForwardUrl::valid)
static const char FORWARD_URL_ERROR[] = "Custom URL must start with http:// or https:// (max 200 characters)";

// Parse and validate the sensor form; returns an error message, empty if valid
// keepKey: an empty key field keeps form.deviceKey (edit form)
// Parse and validate the sensor form into form (filled with the stored
// values, or the defaults for a new sensor, by the caller); returns an error
// message, empty if valid. A field that is not posted keeps its value (the
// Simple edit form has no type, serial number, key or corrections of other
// types); an empty key keeps the stored one, an empty correction is reset
static String parseSensorForm(AsyncWebServerRequest *request, SensorForm &form, bool adding)
{
    auto has = [request](const char *name) { return request->hasParam(name, true); };
    auto text = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    form.name = text("name");
    if (form.name.length() == 0 || form.name.length() > 32)
    {
        return "Name must have 1 to 32 characters";
    }
    if (adding || has("deviceType"))
    {
        long type = text("deviceType").toInt();
        if (type <= 0 || type > 255 || getSensorTypeInfo(static_cast<uint8_t>(type)).type == SensorType::UNKNOWN)
        {
            return "Unknown device type";
        }
        form.deviceType = static_cast<SensorType>(type);
    }
    if ((adding || has("serialNumber")) && !parseHexField(text("serialNumber"), 6, form.serialNumber))
    {
        return "Serial number must be 1 to 6 hex digits";
    }
    if ((adding || text("deviceKey").length() > 0) && !parseHexField(text("deviceKey"), 8, form.deviceKey))
    {
        return "Device key must be 1 to 8 hex digits";
    }
    if (has("customUrl"))
    {
        form.customUrl = text("customUrl");
        if (!ForwardUrl::valid(form.customUrl.c_str(), form.customUrl.length()))
        {
            return FORWARD_URL_ERROR;
        }
    }
    return parseCorrections(request, form.cal);
}

// The form filled with a sensor's stored values
static void sensorFormFrom(const SensorData &s, SensorForm &form)
{
    form.name = s.name;
    form.deviceType = s.deviceType;
    form.serialNumber = s.serialNumber;
    form.deviceKey = s.deviceKey;
    form.customUrl = s.customUrl;
    form.cal = s.cal;
}

static bool saveSensorForm(SensorManager &sensors, int index, const SensorForm &form)
{
    return sensors.updateSensorConfig(index, form.name, form.deviceType, form.serialNumber, form.deviceKey,
                                      form.customUrl, form.cal);
}

// Process sensor add form
void WebPortal::handleSensorAddPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/add");
    if (guardAPOnly(request)) return;
    if (rejectBlockedSave(sensorManager, request)) return;

    SensorForm form;
    sensorFormFrom(SensorData(), form); // defaults (no corrections)
    String error = parseSensorForm(request, form, true);
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
        request->send(500, "text/plain",
                      "Failed to add sensor (sensor list full, or the flash write failed - see Logs)");
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
            if (rejectLowMemory(request, stringPagePeak(SENSOR_FORM_SIZE), SENSOR_FORM_SIZE)) return;
            sensor.deviceKey = 0; // never in the page
            request->send(200, "text/html", HTMLGenerator::generateSensorForm(&sensor, index));
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
    if (rejectBlockedSave(sensorManager, request)) return;

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
    sensorFormFrom(before, form);
    String error = parseSensorForm(request, form, false);
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
    if (rejectBlockedSave(sensorManager, request)) return;

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
    if (rejectBlockedSave(sensorManager, request)) return;
    auto param = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    String name = param("name");
    String devIdText = param("devId");
    String keyText = param("key");
    const String url = param("customUrl");
    uint32_t devId = 0;
    uint8_t label[ep::kLabelKeyLen];
    String error;
    if (name.length() == 0 || name.length() > 32)
    {
        error = "Name must have 1 to 32 characters";
    }
    else if (!ForwardUrl::valid(url.c_str(), url.length()))
    {
        error = FORWARD_URL_ERROR;
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
        // The typed DevID and URL are kept (both bounded like the fields)
        sendNoStore(request, 400, "text/html",
                    HTMLGenerator::generateGen2AddPage(devIdText.substring(0, 8), error,
                                                       url.substring(0, ForwardUrl::MAX_LEN)));
        return;
    }
    int index = sensorManager.addGen2Sensor(devId, label, name, url);
    ep::wipe(label, sizeof(label));
    keyText = String();
    if (index == -2)
    {
        request->send(409, "text/plain", "A generation 2 sensor with this DevID already exists");
        return;
    }
    if (index == -4)
    {
        request->send(503, "text/plain", "Failed to add the sensor: not enough memory - try again later");
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
    if (rejectBlockedSave(sensorManager, request)) return;
    uint32_t devId = 0;
    if (gen2DevIdParam(request, true, devId))
    {
        if (!sensorManager.deleteGen2Sensor(devId))
        {
            logger.warning("Delete of a generation 2 sensor ignored or not saved");
        }
        if (mqttManager)
        {
            mqttManager->requestSensorRemoval(devId, true); // its Home Assistant entities
        }
    }
    request->redirect("/sensors");
}

void WebPortal::sendGen2Detail(AsyncWebServerRequest *request, uint32_t devId, const String &message, int code)
{
    // After a POST the action is done and its result goes out without the
    // admission check of GET /sensors/gen2. Built as one String (~9 kB) and
    // streamed from it: send() copied it into the response, and with a
    // fragmented heap (Pro after attacks.py, largest block ~15 kB) that copy
    // failed - the answer was the status code with an empty body. Without
    // room even for the String and the send buffers, the message alone
    const size_t pageSize = gen2PageSize();
    if (HeapGuard::admit(HeapGuard::usable(), pageSize + HeapGuard::STREAMED_PAGE_PEAK, pageSize, 0, 1) !=
        HeapGuard::Admit::Ok)
    {
        if (message.length() > 0)
        {
            request->send(code, "text/plain", message + " (low memory: reload the page for details)");
        }
        else
        {
            request->send(503, "text/plain", "Low memory, try again");
        }
        return;
    }
    // The page copy lives only for the generation (one sensor, ~600 bytes)
    std::unique_ptr<Gen2Sensor> g(new (std::nothrow) Gen2Sensor());
    if (!g || !sensorManager.getGen2Sensor(devId, *g))
    {
        request->send(404, "text/plain", "Unknown generation 2 sensor");
        return;
    }
    // OTA part (Advanced only): the job, the start checks, the image store
    // (spec 14.5)
    std::unique_ptr<Gen2OtaView> ota(HTMLGenerator::adv() ? new (std::nothrow) Gen2OtaView() : nullptr);
    if (ota)
    {
        ota->hasJob = sensorManager.otaJobCopy(ota->job);
        SensorManager::OtaCheck c = sensorManager.otaCheckStart(devId);
        ota->imageOk = c.imageOk;
        ota->imageFw = c.imageFw;
        ota->imageLen = c.imageLen;
        ota->blockers = c.blockers;
        ota->warnings = c.warnings;
        ota->confirm = c.confirm;
        ota->confirmToken = c.confirmToken;
        ota->estimateS = c.estimateS;
        ota->imageCount = OtaStore::list(ota->images, OtaImage::MAX_HW_TYPES);
        {
            std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
            ota->signedOnly = configManager.signedOnly;
        }
        ota->otaHourMs = RadioTask::stats().otaUsedMs;
        ota->lastEvent = sensorManager.otaLastEventText();
        {
            std::lock_guard<std::mutex> lock(otaUrlMutex);
            ota->fetchResult = fetchResult;
        }
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        ota->now.unixS = tv.tv_sec > 1700000000 ? (uint32_t)tv.tv_sec : 0;
        ota->now.us = esp_timer_get_time();
        ota->now.uptimeS = (uint32_t)(ota->now.us / 1000000); // uptime deadlines (Resume of a battery hold)
    }
    auto page = std::make_shared<String>(HTMLGenerator::generateGen2DetailPage(*g, message, ota.get()));
    g->wipeKeys();
    sendStreamed(request, 1, [page](size_t) { return std::move(*page); }, true, code);
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
    // Only the plain view: after a POST the action is done, its result page
    // is sent whatever the heap
    if (rejectLowMemory(request, stringPagePeak(gen2PageSize()), gen2PageSize())) return;
    sendGen2Detail(request, devId, "");
}

void WebPortal::handleGen2Config(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/gen2/config");
    if (guardAPOnly(request)) return;
    if (rejectBlockedSave(sensorManager, request)) return;
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
    if (result == -1)
    {
        request->send(404, "text/plain", "Unknown generation 2 sensor");
        return;
    }
    String msg = result == 1   ? String("Saved - waiting for the sensor (sent with its next uplink)")
                 : result == 0 ? String("Nothing changed")
                               : String("Not saved: flash write failed (see Logs)");
    sendGen2Detail(request, devId, msg.c_str(), result >= 0 ? 200 : 500);
}

// Name, corrections and (Simple page) report interval; a field that is
// not posted is kept
void WebPortal::handleGen2Edit(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/gen2/edit");
    if (guardAPOnly(request)) return;
    if (rejectBlockedSave(sensorManager, request)) return;
    uint32_t devId = 0;
    if (!gen2DevIdParam(request, true, devId))
    {
        request->send(400, "text/plain", "DevID missing");
        return;
    }
    Gen2Config::Values v;
    String name, url;
    SensorCalibration::Set cal;
    {
        // Heap copy: Gen2Sensor is too big for the AsyncTCP stack
        std::unique_ptr<Gen2Sensor> current(new (std::nothrow) Gen2Sensor());
        if (!current || !sensorManager.getGen2Sensor(devId, *current))
        {
            request->send(404, "text/plain", "Unknown generation 2 sensor");
            return;
        }
        v = current->config;
        name = current->name;
        url = current->customUrl;
        cal = current->cal;
        current->wipeKeys();
    }
    const String calError = parseCorrections(request, cal, true);
    if (calError.length())
    {
        sendGen2Detail(request, devId, "Not saved: " + calError, 400);
        return;
    }
    if (request->hasParam("name", true))
    {
        name = request->getParam("name", true)->value();
        name.trim();
        if (name.length() == 0 || name.length() > 32)
        {
            sendGen2Detail(request, devId, "Not saved: the name must have 1 to 32 characters", 400);
            return;
        }
    }
    if (request->hasParam("customUrl", true))
    {
        url = request->getParam("customUrl", true)->value();
        url.trim();
        if (!ForwardUrl::valid(url.c_str(), url.length()))
        {
            sendGen2Detail(request, devId, String("Not saved: ") + FORWARD_URL_ERROR, 400);
            return;
        }
    }
    bool intervalChanged = false;
    if (request->hasParam("interval", true))
    {
        String text = request->getParam("interval", true)->value();
        text.trim();
        char *end = nullptr;
        const long value = strtol(text.c_str(), &end, 10);
        if (text.length() == 0 || *end != '\0' || value < Gen2Config::MIN_INTERVAL_S ||
            value > Gen2Config::MAX_INTERVAL_S)
        {
            sendGen2Detail(request, devId, "Not saved: the report interval is out of range", 400);
            return;
        }
        intervalChanged = (uint16_t)value != v.intervalS;
        v.intervalS = (uint16_t)value;
        Gen2Config::clamp(v); // a slot offset beyond a shorter interval wraps into it
    }
    const int changed = sensorManager.setGen2Edit(devId, &name, &cal, intervalChanged ? &v : nullptr, &url);
    if (changed == -1) // deleted since the page was loaded
    {
        request->send(404, "text/plain", "Unknown generation 2 sensor");
        return;
    }
    if (changed < 0)
    {
        sendGen2Detail(request, devId, "Not saved: flash write failed (see Logs)", 500);
        return;
    }
    if ((changed & SensorManager::GEN2_EDIT_NAME) && mqttManager)
    {
        // The device name in Home Assistant (corrected values come with the
        // next uplink)
        mqttManager->requestGen2Publish(devId, true);
    }
    sendGen2Detail(request, devId,
                   (changed & SensorManager::GEN2_EDIT_CONFIG) ? "Saved - the new interval is sent with the next uplink"
                   : changed                                   ? "Saved"
                                                               : "Nothing changed");
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

// OTA actions on the generation 2 page: start / retry, abort, pause, resume, clear
void WebPortal::handleGen2Ota(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/gen2/ota");
    if (guardAPOnly(request)) return;
    uint32_t devId = 0;
    if (!gen2DevIdParam(request, true, devId))
    {
        request->send(400, "text/plain", "DevID missing");
        return;
    }
    String action = request->hasParam("action", true) ? request->getParam("action", true)->value() : String();
    String msg;
    bool ok = false;
    if (action == "start")
    {
        // The "I confirm" box carries the token of the reasons it was shown with
        uint32_t token = 0;
        if (request->hasParam("confirm", true))
        {
            token = (uint32_t)strtoul(request->getParam("confirm", true)->value().c_str(), nullptr, 10);
        }
        ok = sensorManager.otaRequestStart(devId, token, msg);
    }
    else if (action == "abort" || action == "pause" || action == "resume" || action == "clear")
    {
        ok = sensorManager.otaAdmin(devId, action[0] == 'r' ? 'r' : action[0], msg);
    }
    else
    {
        msg = "Unknown action";
    }
    sendGen2Detail(request, devId, msg, ok ? 200 : 409);
}

// Sensor image upload: streamed into the image store, validated at the end
void WebPortal::handleOtaImageChunk(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data,
                                    size_t len, bool final)
{
    // Upload data arrives before the middleware runs - the login is
    // checked when a request takes the upload (index 0), never again per
    // chunk: a failed check there (sessionToken() allocates) would skip a
    // chunk and leave a hole in the image. Data of a request that does not
    // own the upload is ignored (its completion answers 401 / 409)
    if (imgUploadOwner != request)
    {
        if (index > 0 || imgUploadOwner != nullptr)
        {
            return; // one upload at a time; the other request gets 409
        }
        if (!auth.hasPassword() || !auth.isAuthorized(request))
        {
            return;
        }
    }
    if (index == 0)
    {
        if (imgUploadOwner == request && imgUploadSawFile)
        {
            if (imgUploadError.length() == 0)
            {
                imgUploadError = "Only one image file per upload";
                OtaStore::uploadAbort();
            }
            return;
        }
        if (guardAPOnly(request))
        {
            return;
        }
        // Not during an update-mode session (the completion is answered 503
        // by the middleware) nor while a sensor image download is queued
        // (409); claimed under the locks a download is queued under
        // (otaUrlMutex), then under the gate the update mode checks uploads
        // under
        {
            std::lock_guard<std::mutex> lock(otaUrlMutex);
            if (fetchBusyLocked())
            {
                return;
            }
            std::lock_guard<std::mutex> gate(UpdateMode::uploadGate());
            if (UpdateMode::active())
            {
                return;
            }
            imgUploadOwner = request;
        }
        imgUploadSawFile = true;
        imgUploadLogged = false;
        imgUploadError = "";
        imgUploadMessage = "";
        // Own disconnect callback: this request must never pass through
        // rejectLowMemory(), which would replace it (one callback per request)
        request->setAttribute(OWN_DISCONNECT_ATTR, true);
        request->onDisconnect([this, request]() {
            if (imgUploadOwner == request)
            {
                if (OtaStore::uploadRunning())
                {
                    OtaStore::uploadAbort();
                }
                imgUploadOwner = nullptr;
                imgUploadSawFile = false;
            }
        });
        String error;
        if (!OtaStore::uploadBegin(error))
        {
            imgUploadError = error;
            return;
        }
        logger.info("Sensor image upload started: " + filename);
    }
    if (imgUploadError.length() > 0)
    {
        return;
    }
    String error;
    if (len > 0 && !OtaStore::uploadWrite(data, len, error))
    {
        imgUploadError = error;
        OtaStore::uploadAbort();
        return;
    }
    esp_task_wdt_reset();
    if (!final)
    {
        return;
    }
    // Refusals at the end of the file are logged in finishSensorImage(),
    // when they are known: a client that disconnects before the completion
    // must not hide them
    imgUploadLogged = true;
    String text;
    if (finishSensorImage(text))
    {
        imgUploadMessage = text;
    }
    else
    {
        imgUploadError = text;
    }
}

// The end of a sensor image, uploaded (web task) or downloaded (loop task):
// one set of rules for both. Validate and classify the signature
// (OtaStore::uploadValidate), "Signed images only", the job rule (an
// unfinished job or a queued start uses the image of this HW type), then
// commit. Logged here. true: stored, text = what was stored; false: text =
// why not (the upload is aborted: upload.tmp removed, the store as before)
bool WebPortal::finishSensorImage(String &text, const Manifest::Expect *expect, const uint8_t *digest)
{
    OtaStore::Meta meta;
    bool severe = false;
    String sigState;
    // A signature block that is there but wrong (tampered file, another HW
    // type or version, a bad signature) is an error, not a mistake
    auto refuse = [&](const String &why, bool asError) {
        text = why;
        if (asError)
        {
            logger.error("Image not stored: " + why);
        }
        else
        {
            logger.warning("Image not stored: " + why);
        }
        OtaStore::uploadAbort();
        return false;
    };
    String error;
    if (!OtaStore::uploadValidate(meta, error, severe, sigState))
    {
        return refuse(error, severe);
    }
    // From the manifest: its SHA-256, HW type and version, and verified (the
    // length was the download's exact size)
    if (expect)
    {
        const char *why =
            Manifest::sensorFetchRefusal(*expect, expect->size, digest, meta.isSigned(), meta.hwType, meta.fw);
        if (why)
        {
            return refuse(String(why) + " (the image is " + sigState + ")", true);
        }
        // Offered when the fetch was requested; checked again now, at the
        // commit: an image stored meanwhile (an upload) that is newer, or the
        // same version signed, is never replaced by the manifest's
        OtaStore::Meta cur;
        if (OtaStore::get(meta.hwType, cur) && !Manifest::sensorWanted(meta.fw, true, cur.fw, cur.isSigned()))
        {
            return refuse("an image at least as new is stored now - nothing replaced", false);
        }
    }
    // "Signed images only" (Firmware page): an unsigned image (incl. one
    // signed with an unknown key) is refused, the store stays as it was
    bool signedOnly;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        signedOnly = configManager.signedOnly;
    }
    switch (SensorFetch::verdict(OtaImageSigned::classOf(meta.signedKey, meta.unknownKey), signedOnly,
                                 sensorManager.otaImageBusy(meta.hwType)))
    {
    case SensorFetch::Verdict::SignedOnly:
        return refuse("the image is " + sigState + " and \"Signed images only\" is on (Firmware page, Advanced mode)",
                      false);
    case SensorFetch::Verdict::Busy:
        return refuse("An OTA job that is not finished uses the image of this HW type", false);
    case SensorFetch::Verdict::Store:
        break;
    }
    if (!OtaStore::uploadCommit(meta, error))
    {
        return refuse(error, true);
    }
    sensorManager.otaImageChanged(meta.hwType);
    char v[16];
    OtaImage::versionText(meta.fw, v, sizeof(v));
    char hw[8];
    snprintf(hw, sizeof(hw), "%04X", meta.hwType);
    text = String("Image v") + v + " for HW type 0x" + hw + " stored (" + String(meta.len) + " B), " + sigState +
           (meta.isSigned() ? "" : " - starting an update with it needs a confirmation");
    if (meta.isSigned())
    {
        logger.info(text);
    }
    else
    {
        logger.warning(text);
    }
    return true;
}

void WebPortal::handleOtaImageComplete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /ota/upload (complete)");
    if (guardAPOnly(request)) return;
    if (imgUploadOwner != request)
    {
        bool fetching;
        {
            std::lock_guard<std::mutex> lock(otaUrlMutex);
            fetching = fetchBusyLocked();
        }
        request->send(409, "text/plain",
                      fetching ? "Image not stored: a sensor image download is running"
                               : "No upload by this request (another one in progress?)");
        return;
    }
    String msg = imgUploadError.length() > 0 ? "Image not stored: " + imgUploadError
                 : imgUploadMessage.length() > 0 ? imgUploadMessage
                                                 : String("Image not stored: no file received");
    bool ok = imgUploadError.length() == 0 && imgUploadMessage.length() > 0;
    if (!ok)
    {
        if (OtaStore::uploadRunning())
        {
            OtaStore::uploadAbort();
        }
        if (!imgUploadLogged) // refusals at the end of the file were logged then
        {
            logger.warning(msg);
        }
    }
    imgUploadOwner = nullptr;
    imgUploadSawFile = false;
    imgUploadLogged = false;
    imgUploadError = "";
    imgUploadMessage = "";
    uint32_t devId = 0;
    if (gen2DevIdParam(request, false, devId))
    {
        sendGen2Detail(request, devId, msg, ok ? 200 : 400);
        return;
    }
    request->send(ok ? 200 : 400, "text/plain", msg);
}

void WebPortal::handleOtaImageDelete(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /ota/delete");
    if (guardAPOnly(request)) return;
    long hw = request->hasParam("hw", true) ? request->getParam("hw", true)->value().toInt() : -1;
    String msg;
    int code = 200;
    if (hw < 0 || hw > 0xFFFF)
    {
        msg = "HW type missing";
        code = 400;
    }
    else if (sensorManager.otaImageBusy((uint16_t)hw))
    {
        msg = "Not deleted: an OTA job that is not finished uses this image";
        code = 409;
    }
    else
    {
        bool had = OtaStore::remove((uint16_t)hw);
        sensorManager.otaImageChanged((uint16_t)hw);
        msg = had ? "Image deleted" : "No such image";
        logger.info("Sensor image of HW type " + String(hw) + (had ? " deleted" : " not found"));
    }
    uint32_t devId = 0;
    if (gen2DevIdParam(request, true, devId))
    {
        sendGen2Detail(request, devId, msg, code);
        return;
    }
    request->send(code, "text/plain", msg);
}

void WebPortal::handleGen2ResetNonce(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /sensors/gen2/resetnonce");
    if (guardAPOnly(request)) return;
    if (rejectBlockedSave(sensorManager, request)) return;
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

    // Streamed piece by piece: the whole page (the full log ring as HTML)
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

    HeapGuard::Heap heap = HeapGuard::usable();
    logger.info("Memory after log clear - 8-bit heap: free " + String(heap.free) + " B, largest block " +
                String(heap.largest) + " B");

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
        configManager.mqttHAPrefix,
        mqttManager ? mqttManager->statusText() : String("Disabled")
    );

    // Send response
    request->send(200, "text/html", html);
}

// MQTT Configuration Post Handler
// MQTT Configuration Post Handler. A field that is not posted keeps the
// stored value: with "Enable MQTT" off every setting is hidden (only the
// switch changes); the checkboxes count only when their part of the form
// was posted (marker f: the settings, adv: the Advanced ones)
void WebPortal::handleMqttPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /mqtt");
    if (guardAPOnly(request)) return;

    auto has = [request](const char *name) { return request->hasParam(name, true); };
    auto param = [request](const char *name)
    {
        String value = request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
        value.trim();
        return value;
    };
    MQTTManager::Settings settings;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        settings.host = configManager.mqttHost;
        settings.port = configManager.mqttPort;
        settings.user = configManager.mqttUser;
        settings.password = configManager.mqttPassword;
        settings.tls = configManager.mqttTls;
        settings.prefix = configManager.mqttPrefix;
        settings.haEnabled = configManager.mqttHAEnabled;
        settings.haPrefix = configManager.mqttHAPrefix;
    }
    settings.enabled = has("enabled");
    const bool fields = has("f"), advanced = has("adv");
    if (fields || has("host"))
    {
        settings.host = param("host");
        if (settings.host.length() == 0 || settings.host.length() > 64 || settings.host.indexOf(' ') >= 0)
        {
            request->send(400, "text/plain", "Broker host must have 1 to 64 characters without spaces");
            return;
        }
    }
    if (fields || has("port"))
    {
        settings.port = param("port").toInt();
        if (settings.port < 1 || settings.port > 65535)
        {
            request->send(400, "text/plain", "Port must be 1 to 65535");
            return;
        }
    }
    if (has("user"))
    {
        settings.user = param("user");
    }
    // Empty password = keep the stored one (it is not sent to the page); no
    // user means no password
    const String password = has("password") ? request->getParam("password", true)->value() : String();
    if (password.length() > 0)
    {
        settings.password = password;
    }
    else if (has("clearPassword") || settings.user.length() == 0)
    {
        settings.password = "";
    }
    if (settings.user.length() > 64 || settings.password.length() > 64)
    {
        request->send(400, "text/plain", "User name and password max 64 characters");
        return;
    }
    if (fields)
    {
        settings.haEnabled = has("haEnabled");
    }
    if (advanced)
    {
        settings.tls = has("tls");
    }
    if (has("prefix"))
    {
        String prefix = param("prefix");
        if (!ConfigManager::isValidMqttTopic(prefix) && prefix != settings.prefix)
        {
            request->send(400, "text/plain", "Invalid topic (1-32 characters, no #, + or /)");
            return;
        }
        settings.prefix = prefix;
    }
    if (has("haPrefix"))
    {
        String prefix = param("haPrefix");
        if (!ConfigManager::isValidMqttTopic(prefix) && prefix != settings.haPrefix)
        {
            request->send(400, "text/plain", "Invalid topic (1-32 characters, no #, + or /)");
            return;
        }
        settings.haPrefix = prefix;
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
// API for retrieving sensor data
void WebPortal::handleAPI(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /api");
    if (guardAPOnly(request)) return;
    String format = request->hasParam("format") ? request->getParam("format")->value() : "html";
    const bool json = format.equalsIgnoreCase("json"), csv = format.equalsIgnoreCase("csv");
    if (!json && !csv && !format.equalsIgnoreCase("html"))
    {
        request->send(400, "text/plain", "Invalid format parameter. Supported formats: json, csv, html");
        return;
    }
    // Public endpoint: peak = the readings copy (one row ~0.4 kB plus its
    // name and custom URL) + the response of the exact size (~0.3 kB per
    // sensor) + one row text + the send buffer (measured 3.6 kB in all with
    // 3 sensors)
    size_t rowsBytes = 0;
    const size_t n = sensorManager.readingCount(&rowsBytes);
    const size_t body = 512 + n * 320;
    if (rejectLowMemory(request, 4096 + rowsBytes + 2 * body, std::max(rowsBytes, body))) return;
    std::vector<SensorData> rows = sensorManager.getReadings();
    if (request->hasParam("sensor"))
    {
        // One sensor: serial number (generation 1) or DevID (generation 2),
        // never two rows (ApiRow::select: gen=1|2, else generation 1 first)
        const uint32_t id = strtoul(request->getParam("sensor")->value().c_str(), nullptr, 16);
        const int gen = request->hasParam("gen") ? request->getParam("gen")->value().toInt() : 0;
        const long k = ApiRow::select(rows, id, gen);
        long i = 0;
        rows.erase(std::remove_if(rows.begin(), rows.end(), [k, &i](const SensorData &) { return i++ != k; }),
                   rows.end());
    }
    for (SensorData &s : rows)
    {
        s.deviceKey = 0;
    }
    if (!json && !csv)
    {
        request->send(200, "text/html", HTMLGenerator::generateAPIPage(rows));
        return;
    }
    // One sensor at a time into a response of the exact size (measured in
    // a first pass): no document of every sensor, no growing buffer
    const unsigned long now = millis();
    const String head = json ? HTMLGenerator::apiJsonHead(uplink->isOnline())
                             : String("name,type,serialNumber,lastSeen,temperature,humidity,pressure,ppm,lux,"
                                      "batteryVoltage,rssi\r\n");
    auto row = [json, now](const SensorData &s) {
        return json ? HTMLGenerator::apiJsonRow(s, now) : HTMLGenerator::apiCsvRow(s, now);
    };
    size_t size = head.length() + 2 + rows.size();
    for (const SensorData &s : rows)
    {
        size += row(s).length();
    }
    AsyncResponseStream *response = request->beginResponseStream(json ? "application/json" : "text/csv", size);
    response->print(head);
    for (size_t i = 0; i < rows.size(); i++)
    {
        if (json && i > 0)
        {
            response->print(',');
        }
        response->print(row(rows[i]));
    }
    if (json)
    {
        response->print("]}");
    }
    response->addHeader(CORS_HEADER_NAME, CORS_HEADER_VALUE);
    request->send(response);
}

// Restart device
void WebPortal::handleReboot(AsyncWebServerRequest *request)
{
    logger.info("HTTP request: GET /reboot - Rebooting device");
    if (const char *held = LayoutMigration::restartRefusal())
    {
        logger.error(String("Restart from the web UI refused: ") + held);
        request->send(409, "text/html",
                      HTMLGenerator::generateNoticePage("Not restarting",
                                                        String("The gateway does not restart: ") + held + ".", 30,
                                                        "/firmware"));
        return;
    }

    request->send(200, "text/html",
                  HTMLGenerator::generateNoticePage("Restarting", "The gateway restarts - this page reloads in 10 s.",
                                                    10, "/"));

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
    heavyExpire(); // AsyncTCP task like the admissions: heavyActive up to date
    // internal = ESP.getFreeHeap() / getMaxAllocHeap(): MALLOC_CAP_INTERNAL,
    // on the Pro incl. ~33 kB of 32-bit-only IRAM malloc never gets
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
    // Every figure is labelled with its heap: *Internal = MALLOC_CAP_INTERNAL
    // (incl. 32-bit-only IRAM), *8bit = MALLOC_CAP_8BIT (what malloc, String
    // and lwIP use; the web guards), tls* = internal 8-bit (mbedTLS)
    res->print("\"freeHeapInternal\":"); res->print((unsigned long)freeHeap); res->print(",");
    res->print("\"maxAllocHeapInternal\":"); res->print((unsigned long)maxAlloc); res->print(",");
    res->print("\"free8bit\":"); res->print((unsigned long)free8); res->print(",");
    res->print("\"largest8bit\":"); res->print((unsigned long)largest8); res->print(",");
    res->print("\"fragmentation8bit\":"); res->print(frag, 4); res->print(",");
    // Lowest 8-bit free heap since boot (e.g. the peak of a TLS session)
    res->print("\"min8bit\":"); res->print((unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)); res->print(",");
    // TLS (HTTPS, MQTT TLS): the internal 8-bit heap against the thresholds
    HeapGuard::Heap tls = HeapGuard::tlsHeap();
    res->printf("\"tls\":{\"free\":%lu,\"largest\":%lu,\"needFree\":%lu,\"needBlock\":%lu,\"fits\":%s},",
                (unsigned long)tls.free, (unsigned long)tls.largest, (unsigned long)HeapGuard::TLS_MIN_FREE,
                (unsigned long)HeapGuard::TLS_MIN_BLOCK, HeapGuard::tlsFits(tls) ? "true" : "false");
    // Heavy responses (pages, /api): running now, limit, 503s since boot;
    // connections reset at accept / requests dropped while received (8-bit
    // heap short, deadline, too many large ones) by the patched libraries
    res->printf("\"web\":{\"heavyActive\":%u,\"heavyMax\":%u,\"refusedBusy\":%lu,\"refusedLow\":%lu,"
                "\"acceptRefused\":%lu,\"recvDeferred\":%lu,\"receiveDropped\":%lu,\"queueOverflow\":%lu,"
                "\"queueOverflowHigh\":%lu,\"eventLost\":%lu,\"heavyExpired\":%lu},",
                heavyActive.load(), (unsigned)WEB_HEAVY_RESPONSES_MAX, (unsigned long)heavyRefusedBusy.load(),
                (unsigned long)heavyRefusedLow.load(), (unsigned long)exploraAcceptRefused.load(),
                (unsigned long)exploraRecvDeferred.load(), (unsigned long)exploraReceiveDropped.load(),
                (unsigned long)exploraQueueOverflow.load(), (unsigned long)exploraQueueOverflowHigh.load(),
                (unsigned long)exploraEventLost.load(), (unsigned long)heavyExpired.load());
    // Heartbeats of the lwIP thread and the radio task (Hardware/Liveness):
    // time since they last advanced, the longest so far, refused probe posts
    Liveness::Status live = Liveness::status();
    res->printf("\"liveness\":{\"tcpipAgeMs\":%lu,\"tcpipMaxAgeMs\":%lu,\"tcpipBusyPosts\":%lu,"
                "\"radioAgeMs\":%lu,\"radioMaxAgeMs\":%lu},",
                (unsigned long)live.tcpipAgeMs, (unsigned long)live.tcpipMaxAgeMs, (unsigned long)live.tcpipBusyPosts,
                (unsigned long)live.radioAgeMs, (unsigned long)live.radioMaxAgeMs);
    // MQTT connection and why it is not connected
    if (mqttManager)
    {
        res->printf("\"mqtt\":{\"status\":\"%s\",\"tls\":%s},",
                    MQTTManager::statusName(mqttManager->status()), mqttManager->usesTls() ? "true" : "false");
    }
    // Update mode: the session now (reason, what is switched off, lowest
    // free heap since it started) and the last one
    res->print("\"updateMode\":");
    UpdateMode::printDiagJson(*res);
    // Gateway firmware: how the running image was installed, validation
    // (rollback) and an image waiting for a confirmation
    res->print(",\"update\":");
    GatewayOta::printDiagJson(*res, AutoUpdate::printDiagJson);
    // Sensor image download (POST /ota/fetch): state, bytes, last result
    res->print(",\"sensorFetch\":");
    {
        char fs[SensorFetch::STATUS_MAX];
        fetchStatus(fs, sizeof(fs));
        res->print(fs);
    }
    res->print(",\"radioPaused\":");
    res->print(RadioTask::paused() ? "true" : "false");
    res->print(",");
    // Restarts in a row because the heap stayed fragmented (main.cpp)
    extern uint32_t heapFragRestarts;
    res->print("\"fragRestarts\":"); res->print((unsigned long)heapFragRestarts); res->print(",");
    res->print("\"psramTotal\":"); res->print((unsigned long)psramSize); res->print(",");
    res->print("\"psramFree\":"); res->print((unsigned long)psramFree); res->print(",");
    res->print("\"uptimeMs\":"); res->print((unsigned long)millis()); res->print(",");
    // Unused stack of the loop task (bytes) - headroom check for TLS paths
    extern TaskHandle_t loopTaskHandle;
    res->print("\"loopStackFree\":");
    res->print((unsigned long)(loopTaskHandle ? uxTaskGetStackHighWaterMark(loopTaskHandle) : 0));
    // ... of the web server task (async_tcp, this one): its high-water mark
    // over all pages served so far
    res->print(",\"webStackFree\":");
    res->print((unsigned long)uxTaskGetStackHighWaterMark(nullptr));
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
    // OTA: the job, skipped slots by reason, radio OTA counters (spec 14.5)
    res->print("\"ota\":");
    res->print(sensorManager.otaDiagJson());
    res->printf(",\"otaRadio\":{\"sent\":%lu,\"usedMs\":%lu,\"budgetMs\":%lu,\"maxLateUs\":%ld},",
                (unsigned long)rs.otaSent, (unsigned long)rs.otaUsedMs, 172800UL, (long)rs.otaMaxLateUs);
    res->print("\"wifiStatus\":"); res->print((int)WiFi.status()); res->print(",");
    res->print("\"wifiDriver\":"); res->print(uplink->wifiDriverRunning() ? "true" : "false"); res->print(",");
    res->print("\"wifiOff\":"); res->print(uplink->wifiSwitchedOff() ? "true" : "false"); res->print(",");
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
#if defined(HAS_WIFI_SWITCH)
    wifi["enabled"] = configManager.wifiEnabled;
    wifi["switchedOff"] = uplink->wifiSwitchedOff();
#endif
    wifi["driver"] = uplink->wifiDriverRunning();
    wifi["ap"] = uplink->isApActive();
    wifi["connected"] = uplink->stationHasIP();
    wifi["ip"] = uplink->getStationIP().toString();
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
    logger.debug("HTTP request: GET /network");
    // Streamed: peak = one piece (<= ~4 kB) + the response's send buffer
    // + the lwIP in-flight copy (each <= TCP_SND_BUF, 5.7 kB) + the settings
    // copy, instead of ~35 kB for the page built as one String and copied
    // by send()
    if (rejectLowMemory(request, HeapGuard::STREAMED_PAGE_PEAK, HeapGuard::STREAMED_PAGE_BLOCK)) return;

    // Settings copied once under the mutex: the pieces are generated in later
    // callbacks, which must not hold the lock across them nor mix the values
    // of two saves
    auto view = std::make_shared<NetworkPageView>();
#if defined(HAS_WIFI_SWITCH)
    {
        // Requested over WiFi = not to the Ethernet address
        IPAddress eth = uplink->getEthernetIP();
        view->ethAddress = eth ? eth.toString() : String();
        view->viaWifi = !eth || request->client()->localIP() != eth;
    }
#endif
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        NetworkPageView &v = *view;
        v.apMode = isAPMode;
        v.ssid = wifiSSID;
        v.hasWifiPassword = wifiPassword.length() > 0;
        v.timezone = configManager.timezone;
        v.timezoneName = configManager.timezoneName;
        v.ethStatic = configManager.ethStatic;
        v.ethIp = configManager.ethIp;
        v.ethMask = configManager.ethMask;
        v.ethGateway = configManager.ethGateway;
        v.ethDns1 = configManager.ethDns1;
        v.ethDns2 = configManager.ethDns2;
        v.wifiEnabled = configManager.wifiEnabled;
        v.lteEnabled = configManager.lteEnabled;
        v.lteApn = configManager.lteApn;
        v.hasLtePin = configManager.ltePin.length() > 0;
    }
    sendStreamed(request, HTMLGenerator::NETWORK_PAGE_PARTS,
                 [view](size_t part) { return HTMLGenerator::generateNetworkPagePart(*view, part); });
}

// One form per section (hidden field "section"), all apply after a restart
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
    else if (section == "wifi")
    {
        // One card: "Use WiFi" (Pro) and the credentials; fields that are not
        // posted (hidden while WiFi is off) keep the stored values, an empty
        // password for the same network keeps the stored one
#if defined(HAS_WIFI_SWITCH)
        const bool enabled = request->hasParam("wifiEnabled", true);
#else
        const bool enabled = true;
#endif
        String ssid, password;
        {
            std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
            ssid = configManager.wifiSSID;
            password = configManager.wifiPassword;
        }
        if (request->hasParam("ssid", true))
        {
            const String stored = ssid;
            ssid = param("ssid");
            String typed = request->hasParam("password", true) ? request->getParam("password", true)->value() : String();
            if (typed.length() > 0 || ssid != stored)
            {
                password = typed; // another network without a password = an open one
            }
            String error = wifiError(ssid, password);
            if (error.length() > 0)
            {
                request->send(400, "text/plain", error);
                return;
            }
        }
        if (!saveWifi(ssid, password, enabled, TimeZones::KEEP))
        {
            request->send(500, "text/plain", "Failed to save configuration");
            return;
        }
        logger.info(String("WiFi settings saved (") + (enabled ? "on" : "off") + ") - restarting");
    }
#if defined(HAS_LTE)
    else if (section == "lte")
    {
        // APN and PIN are hidden (not posted) while LTE is off: kept
        bool enabled = request->hasParam("lteEnabled", true);
        String apn = request->hasParam("apn", true) ? param("apn") : configManager.lteApn;
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

    if (const char *held = LayoutMigration::restartRefusal())
    {
        request->send(200, "text/html",
                      HTMLGenerator::generateNoticePage(
                          "Saved", String("Saved, but the gateway does not restart (the change applies at the next start): ") +
                                       held + ".",
                          30, "/network"));
        return;
    }
    String html = "The gateway restarts in a few seconds.";
#if defined(HAS_ETHERNET)
    if (section == "ethernet" && configManager.ethStatic)
    {
        String ip = HTMLGenerator::htmlEscape(configManager.ethIp);
        html += " Ethernet address: <a href='http://" + ip + "/network'>" + ip + "</a>";
    }
    else if (section == "wifi" && !configManager.wifiEnabled && uplink->getEthernetIP())
    {
        String ip = uplink->getEthernetIP().toString();
        html += " WiFi will be off - reconnect over Ethernet: <a href='http://" + ip + "/network'>" + ip + "</a>";
    }
#endif
    request->send(200, "text/html", HTMLGenerator::generateNoticePage("Saved", html, 15, "/network"));
    // Deferred, see handleClient()
    restartAt = millis() + 1500;
    restartRequested = true;
}

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

// A URL as it may be logged: without "user[:pass]@", query and fragment
static String urlForLog(const String &url)
{
    // The host as HTTPClient connects to it, no user info, no query
    // (UpdateModePolicy::urlForLog)
    std::unique_ptr<char[]> buf(new (std::nothrow) char[url.length() + 16]);
    if (!buf)
    {
        return String("(URL)");
    }
    UpdateModePolicy::urlForLog(url.c_str(), buf.get(), url.length() + 16);
    return String(buf.get());
}

// What UpdateModePolicy::urlAccepted() wants (the 400 answers of the
// gateway firmware URL and the sensor image URL)
#define URL_RULE "must be https:// with a valid host, no spaces, at most 256 characters"

// A TLS client that reads as closed once its deadline passed: HTTPClient's
// own timeouts apply per step (connect 5 s, TLS 10 s, 10 s per header
// read, reset by every line), so a server trickling its headers could hold
// a request for as long as it likes. With this, a request ends at most one
// read timeout (10 s) after the deadline in every phase: a redirect hop is
// connected only while its connect + TLS time (SensorFetch::HOP_RESERVE_MS)
// fits before the deadline, and a read after it fails at once (with 1 ms
// pause: Stream::timedRead() loops on read() until its timeout)
class DeadlineClient : public NetworkClientSecure
{
public:
    using NetworkClientSecure::connect;
    // HTTPClient's connect (also for a redirect hop). The session of the
    // previous hop is closed first: HTTPClient stops it only while
    // connected(), which reads 0 after the deadline (a live TLS context
    // would be re-initialised without being freed)
    int connect(const char *host, uint16_t port, int32_t timeout) override
    {
        NetworkClientSecure::stop();
        if (over() || (hops_++ > 0 && ms_ != 0 && millis() - from_ + SensorFetch::HOP_RESERVE_MS > ms_))
        {
            late_ = true;
            return 0;
        }
        return NetworkClientSecure::connect(host, port, timeout);
    }
    // ms = 0: no deadline
    void limit(unsigned long fromMs, uint32_t ms)
    {
        from_ = fromMs;
        ms_ = ms;
    }
    bool over() const { return ms_ != 0 && millis() - from_ > ms_; }
    // Over, or a redirect hop was not started for lack of time
    bool late() const { return late_ || over(); }
    int available() override { return over() ? 0 : NetworkClientSecure::available(); }
    int read() override { return over() ? (delay(1), -1) : NetworkClientSecure::read(); }
    int read(uint8_t *buf, size_t size) override
    {
        return over() ? (delay(1), -1) : NetworkClientSecure::read(buf, size);
    }
    uint8_t connected() override { return over() ? 0 : NetworkClientSecure::connected(); }

private:
    unsigned long from_ = 0;
    uint32_t ms_ = 0;
    uint8_t hops_ = 0; // connects so far
    bool late_ = false;
};

// Redirects followed per request (strict: https stays https). Each hop can
// take connect + TLS + header time, so few
constexpr uint16_t HTTPS_REDIRECTS = 3;

// HTTPS only, server certificate checked against the built-in CA bundle
static bool beginHttps(HTTPClient &http, NetworkClientSecure &client, const String &url)
{
    if (!url.startsWith("https://"))
    {
        return false;
    }
    // Update mode (loop task): interfaces may just have been switched off,
    // which can clear lwIP's DNS list (a DHCP client torn down) while the
    // session blocks Uplink::process(). DNS servers and default route of the
    // uplink again, and the host resolved now: a failed lookup gets the
    // servers re-applied and one more try (the result is cached for GET)
    if (uplink)
    {
        extern Logger logger;
        uplink->refreshRoutes();
        // Host without "user[:pass]@" (never logged); IP literals need no lookup
        char host[128];
        bool literal = false;
        IPAddress ip;
        // hostByName() returns 1 on success and an lwIP error (negative) on a
        // failure - never 0
        if (UpdateModePolicy::urlHost(url.c_str(), host, sizeof(host), literal) && !literal &&
            Network.hostByName(host, ip) != 1)
        {
            logger.warning(String("DNS lookup of ") + host + " failed - DNS servers of the uplink re-applied, trying again");
            delay(1000);
            uplink->refreshRoutes();
            if (Network.hostByName(host, ip) != 1)
            {
                logger.error(String("DNS lookup of ") + host + " failed again");
            }
        }
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setRedirectLimit(HTTPS_REDIRECTS);
    client.useBuiltinCACertBundle();
    client.setHandshakeTimeout(SensorFetch::HANDSHAKE_MS / 1000);
    http.setConnectTimeout(SensorFetch::CONNECT_MS);
    http.setTimeout(SensorFetch::READ_TIMEOUT_MS);
    http.setReuse(false);
    return http.begin(client, url);
}

// POST /firmware/check ("Check now"): queued for the loop task
// (runUpdateCheck); the page polls GET /firmware/check
void WebPortal::handleCheckStart(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/check");
    if (guardAPOnly(request)) return;
    int status = 202;
    String body = "{\"status\":\"queued\"}";
    {
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        if (!updateCheckRunning)
        {
            // Fail now rather than queue a check the update mode refuses. Not
            // while a new firmware waits for its validation either (as
            // installs): a session's restore restart would roll it back. The
            // flag decides, the texts only explain
            const bool validating = GatewayOta::validationPending();
            const char *mode = validating ? nullptr : UpdateMode::wouldRefuse();
            if (validating || mode)
            {
                String refusal = validating ? GatewayOta::validationRefusal() : String(mode);
                if (refusal.length() == 0)
                {
                    // The validation ended meanwhile, or no memory for the text
                    refusal = "try again in a minute";
                }
                status = 503;
                body = "{\"error\":\"Update check not started: " + refusal + "\"}";
            }
            else
            {
                updateCheckRequested = true;
                updateCheckRunning = true;
            }
        }
    }
    request->send(status, "application/json", body);
}

// GET /firmware/check: the state of the check and the last result
void WebPortal::handleCheckStatus(AsyncWebServerRequest *request)
{
    const char *state;
    {
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        state = updateCheckRequested ? "queued" : updateCheckRunning ? "checking" : "idle";
    }
    AsyncResponseStream *res = request->beginResponseStream("application/json");
    UpdateCheck::printStatus(*res, state);
    request->send(res);
}

// Where a download goes: begin() once the Content-Length passed the check,
// then the body in chunks; false stops the download (error says why)
struct DownloadSink
{
    virtual bool begin(uint32_t total, String &error) = 0;
    virtual bool write(const uint8_t *data, size_t len, String &error) = 0;
    virtual void progress(uint32_t received, uint32_t total) = 0;
};

static bool httpsDownload(const String &url, UpdateMode::Session &session, uint32_t minLen, uint32_t maxLen,
                          DownloadSink &sink, String &error, uint32_t (*budgetMs)(uint32_t));

// The manifest: into a RAM buffer (at most Manifest::MAX_FILE, checked
// against the Content-Length before the body is read)
struct RamSink : DownloadSink
{
    char *buf;
    size_t len = 0;
    explicit RamSink(char *b) : buf(b) {}
    bool begin(uint32_t, String &) override { return true; }
    bool write(const uint8_t *data, size_t n, String &error) override
    {
        if (len + n > Manifest::MAX_FILE)
        {
            error = "manifest too large";
            return false;
        }
        memcpy(buf + len, data, n);
        len += n;
        return true;
    }
    void progress(uint32_t, uint32_t) override {}
};

// Loop task: "Check now" - the signed package manifest in an update-mode
// session (MQTT held, radio paused; interfaces off only if TLS does not
// fit), then UpdateCheck::evaluate() (signature, rules, replay floor)
void WebPortal::runUpdateCheck()
{
    String result;
    {
        UpdateMode::Session session(UpdateMode::Reason::UpdateCheck, UpdateMode::EXTRA_CHECK);
        if (!session.ok())
        {
            UpdateCheck::failed(result = "not started: " + session.error());
        }
        else
        {
            WatchdogPause pause;
            checkIn(session, result);
        }
    }
    std::lock_guard<std::mutex> lock(otaUrlMutex);
    updateCheckRunning = false;
}

// The check inside a session (manual or scheduled): true = a manifest was
// accepted; result as recorded (logged there)
bool WebPortal::checkIn(UpdateMode::Session &session, String &result)
{
    std::unique_ptr<char[]> buf(new (std::nothrow) char[Manifest::MAX_FILE]);
    String error;
    RamSink sink(buf.get());
    bool ok = false;
    if (!buf)
    {
        UpdateCheck::failed(result = "failed: no memory for the manifest");
    }
    else if (!httpsDownload(UPDATE_MANIFEST_URL, session, Manifest::MIN_FILE, Manifest::MAX_FILE, sink, error,
                            &Manifest::budgetMs))
    {
        UpdateCheck::failed(result = "failed: " + error);
    }
    else
    {
        ok = UpdateCheck::evaluate(buf.get(), sink.len, result); // logged and recorded there
    }
    session.setResult("update check " + result);
    return ok;
}

// Handle firmware update request: validated here, downloaded by the loop
// task (runUrlOta), so the response is sent and the async server stays free
void WebPortal::handleFirmwareUpdate(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/update");
    if (guardAPOnly(request)) return;

    String url = request->hasParam("url", true) ? request->getParam("url", true)->value() : String();
    url.trim();
    if (!UpdateModePolicy::urlAccepted(url.c_str()))
    {
        request->send(400, "application/json", "{\"error\":\"Firmware URL " URL_RULE "\"}");
        return;
    }
    if (queueFirmwareDownload(request, url, Manifest::Expect()))
    {
        logger.info("Firmware update requested from: " + urlForLog(url));
        request->send(202, "application/json",
                      "{\"status\":\"update-started\",\"message\":\"Downloading in update mode. A verified image is "
                      "installed and the gateway restarts; an unsigned one waits 10 minutes for your confirmation here. If "
                      "the page stops answering, reload it in a few minutes.\"}");
    }
}

// POST /firmware/install (version=x.y.z): the gateway firmware the last
// update check offers (newer than the running one) - downloaded like a URL
// install, but it must match the manifest (size, SHA-256) and be verified
// with its signed version equal to the manifest's; anything else is
// refused, there is no confirmation for it
void WebPortal::handleFirmwareInstall(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/install");
    if (guardAPOnly(request)) return;
    const String want = request->hasParam("version", true) ? request->getParam("version", true)->value() : String();
    uint32_t version = 0;
    bool offered = false;
    Manifest::Expect expect;
    String url;
    {
        std::lock_guard<std::mutex> lock(UpdateCheck::mutex());
        const Manifest::Doc *d = UpdateCheck::docLocked();
        offered = d && UpdateCheck::gatewayOfferLocked(d) == Manifest::Offer::Install &&
                  GatewayImage::parseVersion(want.c_str(), version) && version == d->gateway.version;
        if (offered)
        {
            expect = Manifest::expectOf(d->gateway);
            url = d->gateway.url;
        }
    }
    const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
    if (!offered || url.length() == 0 || !ota || expect.size > ota->size)
    {
        request->send(409, "application/json",
                      offered ? "{\"error\":\"The offered file does not fit the OTA slot\"}"
                              : "{\"error\":\"Not offered by the last update check - check again\"}");
        return;
    }
    if (queueFirmwareDownload(request, url, expect))
    {
        logger.info("Firmware v" + want + " from the update manifest requested: " + urlForLog(url));
        request->send(202, "application/json",
                      "{\"status\":\"update-started\",\"message\":\"Downloading v" + want +
                          " in update mode; it is installed only if it matches the manifest and its signature verifies, "
                          "then the gateway restarts. If the page stops answering, reload it in a few minutes.\"}");
    }
}

// Both firmware downloads: refused now (answered here, false) rather than
// queued and given up in the loop task (the memory check follows in the
// update mode, after MQTT and the radio stopped and possibly interfaces
// went off)
bool WebPortal::queueFirmwareDownload(AsyncWebServerRequest *request, const String &url, const Manifest::Expect &expect)
{
    // The flags decide (a text that could not be allocated never reads as
    // "allowed"); the texts only explain
    String refusal;
    const char *mode = UpdateMode::wouldRefuse();
    if (mode)
    {
        refusal = mode;
    }
    if (mode || GatewayOta::installRefused(refusal))
    {
        logger.warning("Firmware download not started: " + refusal);
        request->send(503, "application/json", "{\"error\":\"Firmware download not started: " + refusal + "\"}");
        return false;
    }
    std::lock_guard<std::mutex> lock(otaUrlMutex);
    if (restartRequested)
    {
        request->send(409, "application/json",
                      "{\"error\":\"The gateway is about to restart - install again after the restart\"}");
        return false;
    }
    if (otaUploadActive || otaUploadOwner != nullptr || urlOtaRunning || pendingOtaUrl.length() > 0 ||
        fetchBusyLocked())
    {
        request->send(409, "application/json",
                      "{\"error\":\"A firmware update or sensor image download is already running\"}");
        return false;
    }
    pendingOtaUrl = url;
    pendingOtaSession = auth.sessionTag(request);
    pendingOtaExpect = expect;
    return true;
}

// Loop task, inside an update-mode session: GET url over HTTPS (built-in CA
// bundle, strict redirects) and stream the body into sink. Only a 200 with
// a Content-Length within minLen..maxLen (checked before anything is
// written), then exactly that many bytes and nothing behind them. True =
// the whole body went to the sink; false = error says why ("HTTP 404",
// "bad size -1 ...", "connection closed", ...; the bool decides, so a text
// that could not be allocated still fails). budgetMs: the time limit for a
// Content-Length, counted from the request (SensorFetch::budgetMs; until
// the headers are in, the budget of maxLen); nullptr = 2 minutes until the
// headers, then 15 minutes for the body (gateway firmware). Every phase is
// bounded (DeadlineClient). Blocks the loop for the download
constexpr uint32_t GATEWAY_HEADER_MS = 120000;
constexpr uint32_t GATEWAY_BODY_MS = 900000;
// The HTTP status of the last httpsDownload() (loop task; 0 = no answer):
// 404 / 410 refuse the file for the automatic updates (AutoUpdatePolicy::refusedHttp)
static int dlHttpCode = 0;

static bool httpsDownload(const String &url, UpdateMode::Session &session, uint32_t minLen, uint32_t maxLen,
                          DownloadSink &sink, String &error, uint32_t (*budgetMs)(uint32_t))
{
    const unsigned long requested = millis();
    DeadlineClient client;
    HTTPClient http;
    const uint32_t headerMs = budgetMs ? budgetMs(maxLen) : GATEWAY_HEADER_MS;
    client.limit(requested, headerMs);
    if (!beginHttps(http, client, url))
    {
        error = "HTTP begin failed";
        return false;
    }
    http.addHeader("User-Agent", "expLORA-Gateway/" + String(FIRMWARE_VERSION));
    const int httpCode = http.GET();
    dlHttpCode = httpCode;
    if (httpCode != HTTP_CODE_OK)
    {
        const bool late = client.late();
        http.end();
        error = late ? "too slow: no answer within " + String(headerMs / 1000) + " s" : "HTTP " + String(httpCode);
        return false;
    }
    // A known size keeps the space check exact, avoids chunked framing in the
    // image (the raw stream is read below) and lets a cut download be
    // refused before its tail is classified (a cut signed file is no
    // unsigned file)
    const int contentLength = http.getSize();
    if (SensorFetch::lengthCheck(contentLength, minLen, maxLen) != SensorFetch::Length::Ok)
    {
        http.end();
        error = "bad size " + String(contentLength) + " (a Content-Length of " +
                (minLen == maxLen ? String(minLen) : String(minLen) + " to " + String(maxLen)) + " B is needed)";
        return false;
    }
    // The limit for this size: from the request (a slow connection or
    // header phase counts) or 15 minutes for the body
    const uint32_t limitMs = budgetMs ? budgetMs((uint32_t)contentLength) : GATEWAY_BODY_MS;
    // Download buffer only for the download (not kept in static RAM)
    constexpr size_t BUF_SIZE = 2048;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[BUF_SIZE]);
    if (!buf)
    {
        http.end();
        error = "no memory for the download buffer";
        return false;
    }
    if (!sink.begin((uint32_t)contentLength, error))
    {
        http.end();
        return false;
    }
    // nullptr when the connection is gone already (e.g. reset by the server
    // right after the headers)
    NetworkClient *stream = http.getStreamPtr();
    if (!stream)
    {
        http.end();
        error = "connection closed";
        return false;
    }
    uint32_t received = 0;
    const unsigned long started = millis();
    const unsigned long limitFrom = budgetMs ? requested : started;
    client.limit(limitFrom, limitMs);
    unsigned long lastData = started;
    bool ok = true;
    while (ok && received < (uint32_t)contentLength)
    {
        // Gateway: generous, PPP over the modem UART manages ~10 kB/s
        if (millis() - limitFrom > limitMs)
        {
            error = "too slow: not done within " + String(limitMs / 1000) + " s";
            ok = false;
            break;
        }
        if (session.expired())
        {
            error = "the update session reached its " + String(UpdateMode::HARD_CAP_MS / 60000) + " minute limit";
            ok = false;
            break;
        }
        const size_t avail = stream->available();
        if (avail == 0)
        {
            if (!http.connected() && stream->available() == 0)
            {
                error = "connection closed";
                ok = false;
                break;
            }
            if (millis() - lastData > 15000)
            {
                error = "no data for 15 s";
                ok = false;
                break;
            }
            delay(5);
            continue;
        }
        const size_t chunk = min(avail, min(BUF_SIZE, (size_t)(contentLength - received)));
        const int r = stream->readBytes(buf.get(), chunk);
        if (r <= 0)
        {
            continue;
        }
        if (!sink.write(buf.get(), r, error))
        {
            ok = false;
            break;
        }
        received += r;
        lastData = millis();
        sink.progress(received, contentLength);
    }
    if (ok)
    {
        switch (SensorFetch::endCheck(contentLength, received, stream->available() > 0))
        {
        case SensorFetch::End::Ok:
            break;
        case SensorFetch::End::Short:
            error = "short body";
            ok = false;
            break;
        case SensorFetch::End::Long:
            error = "more data than the Content-Length";
            ok = false;
            break;
        }
    }
    http.end();
    return ok;
}

// Gateway firmware download: into the OTA slot (GatewayOta::Writer)
struct FirmwareSink : DownloadSink
{
    GatewayOta::Writer &writer;
    Sha256 *whole; // an install from the manifest: SHA-256 of every byte (else nullptr)
    uint32_t total = 0; // the Content-Length
    int lastPct = -1;
    bool refusedStart = false; // Writer::begin refused (busy, validation)
    FirmwareSink(GatewayOta::Writer &w, Sha256 *h) : writer(w), whole(h) {}
    // The filesystem stays mounted: OTA writes only the other app slot, and
    // unmounting would race with the other task's file writes
    bool begin(uint32_t length, String &error) override
    {
        total = length;
        refusedStart = !writer.begin(error);
        return !refusedStart;
    }
    bool write(const uint8_t *data, size_t len, String &error) override
    {
        if (whole)
        {
            whole->update(data, len);
        }
        return writer.write(data, len, error);
    }
    void progress(uint32_t received, uint32_t total) override
    {
        const int pct = (int)(received * 100ULL / total);
        if (pct / 10 != lastPct / 10)
        {
            extern Logger logger;
            logger.info("OTA progress: " + String(pct) + "%");
            lastPct = pct;
        }
    }
};

// Loop task: download into the OTA slot; a verified image is activated and
// the device restarts, an unsigned one waits for the admin's confirmation
// (GatewayOta::conclude). Blocks the loop for the download.
void WebPortal::runUrlOta(const String &url, uint64_t requester, const Manifest::Expect &expect)
{
    // Update mode for the download (Update/UpdateMode.h); left again when
    // this returns (an activated install restarts instead)
    UpdateMode::Session session(UpdateMode::Reason::GatewayUrl, UpdateMode::EXTRA_GATEWAY_DOWNLOAD);
    if (!session.ok())
    {
        logger.error("Firmware download: " + session.error());
        return;
    }
    WatchdogPause pause;
    String result;
    installIn(session, url, requester, expect, result);
}

// The download and install inside a session (manual or scheduled). Returns
// only when nothing was activated (result says why; false = failed or
// refused, true = waiting for the admin's confirmation); an activated
// image restarts the gateway
bool WebPortal::installIn(UpdateMode::Session &session, const String &url, uint64_t requester,
                          const Manifest::Expect &expect, String &result, bool (*gate)(uint32_t),
                          AutoUpdatePolicy::GwEnd *end)
{
    using AutoUpdatePolicy::GwEnd;
    GwEnd ended = GwEnd::Busy;
    if (!end)
    {
        end = &ended;
    }
    *end = GwEnd::Busy;
    const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
    GatewayOta::Writer writer;
    std::unique_ptr<Sha256> whole(expect.set ? new (std::nothrow) Sha256() : nullptr);
    FirmwareSink sink(writer, whole.get());
    String error;
    dlHttpCode = 0;
    // From the manifest: exactly its size
    const bool downloaded =
        !ota                     ? (error = "no OTA slot", false)
        : expect.set && !whole   ? (error = "no memory for the hash", false)
        : expect.set             ? httpsDownload(url, session, expect.size, expect.size, sink, error, nullptr)
                                 : httpsDownload(url, session, GatewayImage::HEADER_SIZE + 1, ota->size, sink, error, nullptr);
    if (!downloaded)
    {
        // A cut download is never classified (and never activated)
        logger.error("Firmware download failed: " + error);
        *end = writer.headRefused() || AutoUpdatePolicy::refusedHttp(dlHttpCode) ? GwEnd::Refused
               : !ota || (expect.set && !whole) || sink.refusedStart ? GwEnd::Busy
                                                                     : GwEnd::Transport;
        writer.abort();
        session.setResult(result = "download failed: " + error);
        return false;
    }
    const GatewayImage::Outcome outcome = writer.finish((uint64_t)sink.total);
    if (expect.set && outcome.cls != GatewayImage::Class::Refused)
    {
        // From the manifest: its size and SHA-256, verified with the
        // manifest's version and newer than the running firmware - or
        // erased (no confirmation path)
        uint8_t digest[32];
        const char *why = !whole->finish(digest)
                              ? "the SHA-256 could not be computed"
                              : Manifest::gatewayInstallRefusal(expect, outcome.length, digest,
                                                                outcome.cls == GatewayImage::Class::Verified,
                                                                outcome.fwVersion,
                                                                GatewayImage::runningVersionOf(FIRMWARE_VERSION));
        if (why)
        {
            GatewayOta::discard(writer, why); // also the last install result
            logger.error(String("Firmware from the update manifest refused: ") + why + " (" + outcome.text +
                         ") - not activated, slot erased");
            session.setResult(result = String("install refused: ") + why);
            *end = GwEnd::Refused;
            return false;
        }
        if (gate && !gate(expect.version))
        {
            GatewayOta::discard(writer, "its attempt could not be saved");
            session.setResult(result = "install refused: its attempt could not be saved");
            return false;
        }
    }
    bool signedOnly;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        signedOnly = configManager.signedOnly;
    }
    GatewayOta::Result done = GatewayOta::conclude(writer, outcome, requester, signedOnly, "URL download");
    if (!done.activated)
    {
        // Confirm: unsigned / unknown key, or a verified older version
        const bool confirm = done.action == GatewayImage::Activation::Confirm;
        result = confirm ? String("downloaded, waiting for the admin's confirmation (") +
                               (outcome.cls == GatewayImage::Class::Verified ? "older version, " : "") + outcome.text +
                               ")"
                         : "download refused: " +
                               (outcome.cls == GatewayImage::Class::Refused ? String(outcome.text) : done.message);
        if (outcome.cls == GatewayImage::Class::Refused)
        {
            *end = GwEnd::Refused;
        }
        session.setResult(result);
        return confirm;
    }
    logger.info("OTA complete, restarting");
    delay(500);
    ESP.restart();
    return false;
}

// Sensor image download: into the image store, exactly like a web upload
struct ImageSink : DownloadSink
{
    std::atomic<uint32_t> &bytes, &length;
    Sha256 *whole; // a fetch from the manifest: SHA-256 of every byte (else nullptr)
    ImageSink(std::atomic<uint32_t> &b, std::atomic<uint32_t> &l, Sha256 *h) : bytes(b), length(l), whole(h) {}
    bool begin(uint32_t total, String &error) override
    {
        length = total;
        return OtaStore::uploadBegin(error);
    }
    bool write(const uint8_t *data, size_t len, String &error) override
    {
        if (whole)
        {
            whole->update(data, len);
        }
        return OtaStore::uploadWrite(data, len, error);
    }
    void progress(uint32_t received, uint32_t) override { bytes = received; }
};

bool WebPortal::fetchBusyLocked() const
{
    return fetchState == (uint8_t)SensorFetch::State::Queued || fetchState == (uint8_t)SensorFetch::State::Running;
}

// POST /ota/fetch (gen 2 page, Advanced): a sensor image from an https://
// URL. Refused (409 with the reason) while anything it may not overlap runs:
// another download, upload or update-mode session (503 by the middleware
// during a session), a pending restart, a sensor job that needs the radio
// (Transfer, Verified, Swapping, Confirming, a Waiting job with a start out
// or close to its deadline - UpdateModePolicy::jobAllows), offline or
// without a clock, a gateway firmware still being validated. Queued for the
// loop task (runSensorFetch); GET /ota/fetch/status follows it
void WebPortal::handleOtaFetch(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /ota/fetch");
    if (guardAPOnly(request)) return;
    if (request->url() != "/ota/fetch") // the route also matches /ota/fetch/...
    {
        handleNotFound(request);
        return;
    }
    String url;
    Manifest::Expect expect;
    if (request->hasParam("hw", true))
    {
        // From the manifest (Firmware page): the package the last update
        // check offers for this HW type, with its version named
        const String hwText = request->getParam("hw", true)->value();
        const String want = request->hasParam("version", true) ? request->getParam("version", true)->value() : String();
        uint16_t hw = 0;
        uint32_t version = 0;
        if (Manifest::parseHw(hwText.c_str(), hw) && GatewayImage::parseVersion(want.c_str(), version))
        {
            std::lock_guard<std::mutex> lock(UpdateCheck::mutex());
            const Manifest::Doc *d = UpdateCheck::docLocked();
            const Manifest::Package *p = d ? d->sensorFor(hw) : nullptr;
            if (p && p->version == version && UpdateCheck::sensorOfferLocked(d, hw) == Manifest::Offer::Install)
            {
                expect = Manifest::expectOf(*p, hw);
                url = p->url;
            }
        }
        if (!expect.set || url.length() == 0)
        {
            request->send(409, "text/plain", "Not offered by the last update check - check again");
            return;
        }
    }
    else
    {
        url = request->hasParam("url", true) ? request->getParam("url", true)->value() : String();
        url.trim();
        if (!UpdateModePolicy::urlAccepted(url.c_str()))
        {
            request->send(400, "text/plain", "The image URL " URL_RULE);
            return;
        }
    }
    // Not while a new gateway firmware waits for its validation (as the
    // update check: a restart by the session's connectivity watch would roll
    // it back). The flags decide; the texts only explain
    String refusal;
    bool refused = GatewayOta::validationPending();
    if (refused)
    {
        refusal = GatewayOta::validationRefusal();
    }
    else
    {
        const char *mode = UpdateMode::wouldRefuse(UpdateMode::Reason::SensorFetch);
        refused = mode != nullptr;
        if (refused)
        {
            refusal = mode;
        }
    }
    if (!refused)
    {
        // Under the same locks as the uploads claim theirs (otaUrlMutex,
        // then the update mode's gate): one of the two always proceeds
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        SensorFetch::Activity a{};
        a.fetch = fetchBusyLocked();
        a.restart = restartRequested;
        a.gatewayUpload = otaUploadOwner != nullptr || otaUploadActive;
        a.gatewayUrl = urlOtaRunning || pendingOtaUrl.length() > 0;
        a.updateCheck = updateCheckRunning || updateCheckRequested;
        std::lock_guard<std::mutex> gate(UpdateMode::uploadGate());
        a.imageUpload = imgUploadOwner != nullptr || OtaStore::uploadRunning();
        const SensorFetch::Busy b = SensorFetch::busy(a);
        refused = b != SensorFetch::Busy::None;
        if (refused)
        {
            refusal = SensorFetch::busyText(b);
        }
        else
        {
            pendingFetchUrl = url;
            pendingFetchExpect = expect;
            if (pendingFetchUrl.length() != url.length())
            {
                pendingFetchUrl = "";
                refused = true;
                refusal = "out of memory";
            }
            else
            {
                fetchState = (uint8_t)SensorFetch::State::Queued;
                fetchResult = "";
                fetchBytes = 0;
                fetchTotal = 0;
            }
        }
    }
    if (refused)
    {
        refusal = "Sensor image download not started: " + refusal;
        logger.warning(refusal);
        request->send(409, "text/plain", refusal);
        return;
    }
    logger.info("Sensor image download from " + urlForLog(url));
    request->send(202, "text/plain", "queued");
}

void WebPortal::fetchStatus(char *buf, size_t cap)
{
    std::lock_guard<std::mutex> lock(otaUrlMutex);
    SensorFetch::statusJson(buf, cap, (SensorFetch::State)fetchState, fetchBytes, fetchTotal, fetchResult.c_str());
}

void WebPortal::handleOtaFetchStatus(AsyncWebServerRequest *request)
{
    char buf[SensorFetch::STATUS_MAX];
    fetchStatus(buf, sizeof(buf));
    request->send(200, "application/json", buf);
}

// Loop task: the download of POST /ota/fetch in an update-mode session
// (MQTT held, radio paused, interfaces off only if TLS does not fit),
// streamed into the image store and ended by the same rules as a web
// upload (finishSensorImage). Any failure leaves the store as it was
// (upload.tmp removed). The state says done only after the session is left
void WebPortal::runSensorFetch(const String &url, const Manifest::Expect &expect)
{
    {
        std::lock_guard<std::mutex> lock(otaUrlMutex);
        fetchState = (uint8_t)SensorFetch::State::Running;
    }
    bool stored = false;
    String result;
    // Checked again here (the web handler's text could not decide this):
    // never while a new gateway firmware waits for its validation
    if (GatewayOta::validationPending())
    {
        result = GatewayOta::validationRefusal();
        logger.warning(result);
    }
    else
    {
        UpdateMode::Session session(UpdateMode::Reason::SensorFetch, UpdateMode::EXTRA_SENSOR_FETCH);
        if (!session.ok())
        {
            result = session.error();
        }
        else
        {
            WatchdogPause pause;
            stored = fetchIn(session, url, expect, result);
        }
    }
    std::lock_guard<std::mutex> lock(otaUrlMutex);
    fetchResult = result;
    fetchState = (uint8_t)(stored ? SensorFetch::State::Stored : SensorFetch::State::Failed);
}

// A sensor image download inside a session (manual or scheduled): true =
// stored; result as logged
bool WebPortal::fetchIn(UpdateMode::Session &session, const String &url, const Manifest::Expect &expect,
                        String &result, bool *refused)
{
    bool stored = false, notStored = false;
    dlHttpCode = 0;
    std::unique_ptr<Sha256> whole(expect.set ? new (std::nothrow) Sha256() : nullptr);
    ImageSink sink(fetchBytes, fetchTotal, whole.get());
    String error;
    uint8_t digest[32];
    // From the manifest: exactly its size, the SHA-256 of every byte
    if (expect.set && !whole)
    {
        result = "download failed: no memory for the hash";
        logger.error("Sensor image " + result);
    }
    else if (!httpsDownload(url, session, expect.set ? expect.size : SensorFetch::MIN_FILE,
                            expect.set ? expect.size : SensorFetch::MAX_FILE, sink, error, &SensorFetch::budgetMs) ||
             (expect.set && !whole->finish(digest) && (error = "the SHA-256 could not be computed", true)))
    {
        if (OtaStore::uploadRunning())
        {
            OtaStore::uploadAbort();
        }
        result = "download failed: " + error;
        logger.error("Sensor image " + result);
        notStored = AutoUpdatePolicy::refusedHttp(dlHttpCode);
    }
    else
    {
        stored = finishSensorImage(result, expect.set ? &expect : nullptr, digest); // logged there
        if (!stored)
        {
            result = "not stored: " + result;
            notStored = true;
        }
    }
    if (refused)
    {
        *refused = notStored;
    }
    session.setResult(result);
    return stored;
}

AutoUpdatePolicy::Activity WebPortal::autoActivity()
{
    AutoUpdatePolicy::Activity a;
    a.job = sensorManager.otaJobState(a.startPending);
    a.validation = GatewayOta::validationPending();
    a.session = UpdateMode::active();
    std::lock_guard<std::mutex> lock(otaUrlMutex);
    a.upload = otaUploadActive || otaUploadOwner != nullptr || imgUploadOwner != nullptr || OtaStore::uploadRunning();
    a.confirmWait = GatewayOta::waitingConfirm();
    a.download = urlOtaRunning || pendingOtaUrl.length() > 0 || fetchBusyLocked() || updateCheckRequested ||
                 updateCheckRunning;
    a.restart = restartRequested;
    return a;
}

// Loop task: the nightly run of the automatic updates in one update-mode
// session (Update/AutoUpdate.h). Verified images only, whatever "Signed
// images only" says (the manifest paths accept nothing else)
AutoUpdatePolicy::Outcome WebPortal::runScheduled(const AutoUpdatePolicy::Settings &s, String &result)
{
    using AutoUpdatePolicy::Outcome;
    UpdateMode::Session session(UpdateMode::Reason::Scheduled,
                                s.gateway ? UpdateMode::EXTRA_GATEWAY_DOWNLOAD : UpdateMode::EXTRA_SENSOR_FETCH);
    if (!session.ok())
    {
        result = session.error();
        return Outcome::Failed;
    }
    WatchdogPause pause;
    if (!checkIn(session, result))
    {
        result = "check " + result;
        return Outcome::Failed;
    }
    // The gateway first (a newer verified firmware restarts the gateway),
    // then every newer sensor image offered for a HW type whose image no
    // unfinished job uses. One package at a time: copied under the lock,
    // downloaded without it
    using AutoUpdatePolicy::GwEnd;
    uint8_t stored = 0, failed = 0;
    bool gwFailed = false;
    for (int i = s.gateway ? -1 : 0; i < (s.sensors ? (int)Manifest::MAX_PACKAGES : 0); i++)
    {
        Manifest::Expect expect;
        String url;
        {
            std::lock_guard<std::mutex> lock(UpdateCheck::mutex());
            const Manifest::Doc *d = UpdateCheck::docLocked();
            if (!d || i >= (int)d->sensorCount)
            {
                break;
            }
            if (i < 0)
            {
                if (UpdateCheck::gatewayOfferLocked(d) == Manifest::Offer::Install)
                {
                    const char *no = AutoUpdate::gatewayRefusal(d->gateway.version);
                    if (!no)
                    {
                        expect = Manifest::expectOf(d->gateway);
                        url = d->gateway.url;
                    }
                    else
                    {
                        result += no;
                    }
                }
            }
            else if (UpdateCheck::sensorOfferLocked(d, d->sensor[i].hw) == Manifest::Offer::Install)
            {
                expect = Manifest::expectOf(d->sensor[i].pkg, d->sensor[i].hw);
                url = d->sensor[i].pkg.url;
            }
        }
        String why;
        if (!expect.set || url.length() == 0)
        {
            continue;
        }
        if (i < 0)
        {
            // An image larger than the slot can never install: a strike
            const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
            GwEnd end = ota ? GwEnd::Refused : GwEnd::Busy;
            if (!ota || expect.size > ota->size)
            {
                why = "install refused: larger than the OTA slot";
                logger.error("Automatic update: gateway " + why);
            }
            else
            {
                AutoUpdate::beforeGatewayDownload(expect.version); // a restart during the download is a strike
                installIn(session, url, 0, expect, why, &AutoUpdate::beforeGatewayActivation, &end); // restarts when activated
            }
            AutoUpdate::gatewayInstallEnded(expect.version, end);
            result += ", gateway " + why;
            gwFailed = end != GwEnd::Refused; // a refused image is not retried tonight
            continue;
        }
        if (!AutoUpdate::sensorFetchWanted(expect.hw, expect.version))
        {
            Logger::logf(LogLevel::WARNING, "Automatic update: sensor image for HW %04X not fetched (failed on %u nights)",
                         expect.hw, AutoUpdatePolicy::FETCH_TRIES);
            continue;
        }
        if (session.expired() || sensorManager.otaImageBusy(expect.hw))
        {
            Logger::logf(LogLevel::WARNING, "Automatic update: sensor image for HW %04X not fetched (%s)", expect.hw,
                         session.expired() ? "session limit" : "an unfinished OTA job uses the stored one");
            failed += session.expired();
            continue;
        }
        bool refused = false;
        const bool ok = fetchIn(session, url, expect, why, &refused);
        ok ? stored++ : failed++;
        using AutoUpdatePolicy::FetchEnd;
        AutoUpdate::sensorFetched(expect.hw, expect.version,
                                  ok ? FetchEnd::Stored : refused ? FetchEnd::Refused : FetchEnd::Transport);
    }
    if (stored || failed)
    {
        char t[48];
        snprintf(t, sizeof(t), ", sensor images: %u stored, %u failed", stored, failed);
        result += t;
    }
    session.setResult("scheduled: " + result);
    return failed || gwFailed ? Outcome::Failed : Outcome::Ok;
}

void WebPortal::handleFirmwarePage(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: GET /firmware");
    if (guardAPOnly(request)) return;
    // During an update-mode session a small status page (no upload form,
    // ~1 kB; reloads itself until the session ends). The full page (a ~13 kB
    // String plus send()'s copy) would take its memory from the session's
    // TLS reserve
    if (UpdateMode::active())
    {
        request->send(200, "text/html", HTMLGenerator::generateUpdateProgressPage(UpdateMode::statusText()));
        return;
    }
    if (rejectLowMemory(request, stringPagePeak(FIRMWARE_PAGE_SIZE), FIRMWARE_PAGE_SIZE)) return;
    bool signedOnly;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        signedOnly = configManager.signedOnly;
    }
    UpdateCheckView check;
    {
        std::lock_guard<std::mutex> lock(UpdateCheck::mutex());
        const Manifest::Doc *d = UpdateCheck::docLocked();
        if (d)
        {
            check.have = true;
            check.keyId = d->keyId;
            check.hasGateway = d->hasGateway;
            check.gatewayVersion = d->gateway.version;
            check.gatewayOffer = UpdateCheck::gatewayOfferLocked(d);
            for (uint8_t i = 0; i < d->sensorCount; i++)
            {
                UpdateCheckView::Sensor &s = check.sensor[check.sensors++];
                s.hw = d->sensor[i].hw;
                s.offered = d->sensor[i].pkg.version;
                s.offer = UpdateCheck::sensorOfferLocked(d, s.hw, &s.storedFw, &s.stored);
            }
        }
    }
    String html = HTMLGenerator::generateFirmwarePage(UpdateMode::statusText(), signedOnly,
                                                      GatewayOta::view(auth.sessionTag(request)), check,
                                                      AutoUpdate::view());
    request->send(200, "text/html", html);
}

// "Automatic updates" card: the two switches and the window. Fields that
// are not posted keep their stored values (f = the switches were posted)
void WebPortal::handleAutoUpdatePost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/auto");
    if (guardAPOnly(request)) return;
    AutoUpdatePolicy::Settings s;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        s = AutoUpdatePolicy::unpack(configManager.autoUpdate);
    }
    auto has = [request](const char *n) { return request->hasParam(n, true); };
    if (has("f"))
    {
        s.gateway = has("gw");
        s.sensors = has("sn");
    }
    // An hour: 1 or 2 digits (anything else is refused, not read as 0)
    auto hour = [request](const char *n, int keep) {
        if (!request->hasParam(n, true))
        {
            return keep;
        }
        const String &v = request->getParam(n, true)->value();
        return v.length() >= 1 && v.length() <= 2 && isDigit(v[0]) && isDigit(v[v.length() - 1]) ? (int)v.toInt()
                                                                                                    : -1;
    };
    const int start = hour("ws", s.startH), end = hour("we", s.endH);
    if (!AutoUpdatePolicy::validWindow(start, end))
    {
        request->send(400, "text/plain", "The update window needs a start and an end hour (0-23) that differ");
        return;
    }
    s.startH = (uint8_t)start;
    s.endH = (uint8_t)end;
    if (!configManager.setAutoUpdate(AutoUpdatePolicy::pack(s)))
    {
        request->send(500, "text/plain", "Failed to save configuration");
        return;
    }
    AutoUpdate::settingsChanged(s);
    request->redirect("/firmware");
}

// "Signed images only" (shared by sensor images and the gateway firmware)
void WebPortal::handleSignedOnlyPost(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/signed");
    if (guardAPOnly(request)) return;
    const bool on = request->hasParam("signedOnly", true) && request->getParam("signedOnly", true)->value() == "1";
    if (!configManager.setSignedOnly(on))
    {
        request->send(500, "text/plain", "Failed to save configuration");
        return;
    }
    logger.warning(on ? "Signed images only: on - unsigned images are refused"
                      : "Signed images only: off - unsigned images are accepted with a confirmation");
    if (on)
    {
        // A queued start or an unfinished job with an unsigned image stops now
        sensorManager.otaEnforceSignedOnly("as \"Signed images only\" was switched on");
        // An unsigned gateway image waiting for its confirmation is dropped
        // and erased
        GatewayOta::signedOnlyOn();
    }
    request->redirect("/firmware");
}

// A text as a JSON string body (quotes, backslashes; control characters
// become spaces)
static String jsonText(const String &in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++)
    {
        const char c = in[i];
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else
        {
            out += (uint8_t)c < 0x20 ? ' ' : c;
        }
    }
    return out;
}

// Upload failed or was interrupted: release the OTA slot (nothing activated)
void WebPortal::abortUpload(const String &reason, bool log)
{
    if (otaWriter)
    {
        otaWriter->abort();
        otaWriter.reset();
    }
    otaUploadActive = false;
    otaUploadFinished = false;
    if (!otaUploadHasError)
    {
        otaUploadHasError = true;
        otaUploadErrorMsg = reason;
    }
    if (log)
    {
        logger.error("Firmware upload failed: " + reason);
    }
}

// The upload's file ended and was classified: activate a verified image,
// keep an unsigned one waiting for the confirmation, or refuse it
GatewayOta::Result WebPortal::concludeUpload()
{
    GatewayOta::Result result;
    if (!otaWriter || !otaUploadFinished)
    {
        result.message = "No firmware received";
        return result;
    }
    bool signedOnly;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        signedOnly = configManager.signedOnly;
    }
    result = GatewayOta::conclude(*otaWriter, otaUploadOutcome, otaUploadSession, signedOnly, "upload");
    otaWriter.reset();
    otaUploadActive = false;
    otaUploadFinished = false;
    if (result.activated)
    {
        // Deferred, see handleClient()
        restartAt = millis() + 1500;
        restartRequested = true;
    }
    return result;
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
        request->send(409, "application/json",
                      "{\"error\":\"No upload by this request (another update in progress?)\"}");
        return;
    }
    // The upload is over either way: a stale owner would keep refusing
    // update sessions and URL updates until the client disconnects. Cleared
    // only after the image is concluded or aborted (the owner is one of the
    // gates a URL install checks)
    auto releaseOwner = [this]()
    {
        std::lock_guard<std::mutex> gate(UpdateMode::uploadGate());
        otaUploadOwner = nullptr;
        otaUploadSawFile = false;
    };

    if (otaUploadHasError || !otaUploadFinished)
    {
        bool failed = otaUploadHasError;
        String reason = failed ? otaUploadErrorMsg : String("No firmware received");
        abortUpload(reason, !failed); // a failure was logged when it happened
        releaseOwner();
        request->send(failed ? 500 : 400, "application/json", "{\"error\":\"" + jsonText(reason) + "\"}");
        otaUploadHasError = false;
        otaUploadErrorMsg = "";
        otaUploadExpected = otaUploadWritten = 0;
        return;
    }

    const GatewayImage::Class cls = otaUploadOutcome.cls;
    GatewayOta::Result result = concludeUpload();
    releaseOwner();
    otaUploadExpected = otaUploadWritten = 0;
    const String msg = jsonText(result.message);
    switch (result.action)
    {
    case GatewayImage::Activation::Activate:
        if (result.activated)
        {
            request->send(200, "application/json",
                          String("{\"status\":\"activated\",\"sig\":\"") + GatewayImage::className(cls) +
                              "\",\"message\":\"" + msg + "\"}");
            return;
        }
        break;
    case GatewayImage::Activation::Confirm:
        request->send(200, "application/json",
                      String("{\"status\":\"confirm\",\"sig\":\"") + GatewayImage::className(cls) + "\",\"image\":\"" +
                          result.digestHex + "\",\"message\":\"" + msg + "\"}");
        return;
    default:
        break;
    }
    request->send(400, "application/json", "{\"error\":\"" + msg + "\"}");
}

// POST /firmware/confirm (image = SHA-256 hex of the waiting image): the
// admin activates an unsigned image that the same login session installed
// within the last 10 minutes
void WebPortal::handleFirmwareConfirm(AsyncWebServerRequest *request)
{
    logger.debug("HTTP request: POST /firmware/confirm");
    if (guardAPOnly(request)) return;
    const String image = request->hasParam("image", true) ? request->getParam("image", true)->value() : String();
    bool signedOnly;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager.mutex());
        signedOnly = configManager.signedOnly;
    }
    if (restartRequested)
    {
        request->send(409, "application/json", "{\"error\":\"The gateway is about to restart\"}");
        return;
    }
    const String error = GatewayOta::confirm(image.c_str(), auth.sessionTag(request), signedOnly);
    if (error.length() > 0)
    {
        request->send(400, "application/json", "{\"error\":\"" + jsonText(error) + "\"}");
        return;
    }
    request->send(200, "application/json",
                  "{\"status\":\"activated\",\"message\":\"Confirmed firmware activated - the gateway restarts.\"}");
    restartAt = millis() + 1500;
    restartRequested = true;
}

// Upload chunk callback
void WebPortal::handleFirmwareUploadChunk(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
    // Upload data arrives before the middleware runs - the login is
    // checked when a request takes the upload (index 0), never again per
    // chunk: a failed check there (sessionToken() allocates) would skip a
    // chunk and leave a hole in the image. One upload at a time: data of
    // any other request is ignored (its completion answers 401 / 409)
    if (otaUploadOwner != request)
    {
        if (index > 0 || otaUploadOwner != nullptr)
        {
            return;
        }
        if (!auth.hasPassword() || !auth.isAuthorized(request))
        {
            return;
        }
    }
    if (index == 0 && guardAPOnly(request))
    {
        return;
    }
    // A second file part in the same upload: never flash or report it, and
    // do not activate the first one either (it is concluded only at the
    // completion of a request with exactly one file)
    if (index == 0 && otaUploadOwner == request && otaUploadSawFile)
    {
        if (!otaUploadHasError)
        {
            if (otaWriter && otaUploadFinished)
            {
                // Classified, not activated: erased like any refused image
                GatewayOta::discard(*otaWriter);
                otaWriter.reset();
            }
            abortUpload("Only one firmware file per upload");
        }
        return;
    }
    if (index == 0)
    {
        // Upload start (first chunk). Not during an update-mode session:
        // the request's completion is then answered 503 by the middleware.
        // Not while a URL update is queued or runs either (it goes ahead;
        // this request's completion answers 409). Checked under otaUrlMutex
        // (handleFirmwareUpdate queues a URL under it and refuses while an
        // upload owner is set), then claimed under the gate the update mode
        // checks uploads under: one of the two always proceeds
        {
            std::lock_guard<std::mutex> lock(otaUrlMutex);
            if (urlOtaRunning || pendingOtaUrl.length() > 0 || fetchBusyLocked())
            {
                logger.warning("Firmware upload ignored: a download (firmware or sensor image) is queued or running");
                return;
            }
            std::lock_guard<std::mutex> gate(UpdateMode::uploadGate());
            if (UpdateMode::active())
            {
                return;
            }
            otaUploadOwner = request;
        }
        otaUploadHasError = false;
        otaUploadErrorMsg = "";
        otaUploadExpected = request->contentLength(); // HINT: may be inaccurate with multipart
        otaUploadWritten = 0;
        otaUploadFinished = false;
        otaUploadSession = auth.sessionTag(request);

        logger.info("Firmware upload started: " + filename + " size=" + String(otaUploadExpected));
        if (otaUploadActive || otaWriter)
        {
            // Leftover of a finished owner (no live request holds it)
            abortUpload("interrupted");
            otaUploadHasError = false;
            otaUploadErrorMsg = "";
        }
        // Client disconnecting mid-upload: no completion callback follows
        otaUploadSawFile = true;
        // Own disconnect callback: this request must never pass through
        // rejectLowMemory(), which would replace it (one callback per request)
        request->setAttribute(OWN_DISCONNECT_ATTR, true);
        request->onDisconnect([this, request]()
                              {
            // Only the request that owns the upload may abort it. A file
            // that ended completely is still concluded (a verified image is
            // installed, an unsigned one waits for the confirmation on the
            // Firmware page) - the client may be gone before the completion
            if (otaUploadOwner == request && otaWriter)
            {
                if (otaUploadFinished && !otaUploadHasError)
                {
                    concludeUpload();
                }
                else
                {
                    abortUpload("client disconnected");
                }
            }
            if (otaUploadOwner == request)
            {
                otaUploadOwner = nullptr;
                otaUploadSawFile = false;
            } });

        // If server/client specified expected size, at least verify it fits in slot
        // contentLength() includes the multipart framing (a few hundred bytes)
        const esp_partition_t *ota = esp_ota_get_next_update_partition(nullptr);
        if (!ota)
        {
            otaUploadHasError = true;
            otaUploadErrorMsg = "No OTA partition";
            return;
        }
        if (otaUploadExpected > ota->size + 1024)
        {
            otaUploadHasError = true;
            otaUploadErrorMsg = "Firmware too large for OTA slot";
            return;
        }

        if (restartRequested)
        {
            otaUploadHasError = true;
            otaUploadErrorMsg = "The gateway is about to restart - install again after the restart";
            return;
        }
        otaWriter.reset(new (std::nothrow) GatewayOta::Writer());
        String error = "not enough memory";
        if (!otaWriter || !otaWriter->begin(error))
        {
            otaWriter.reset();
            otaUploadHasError = true;
            otaUploadErrorMsg = error;
            logger.warning("Firmware upload refused: " + error);
            return;
        }
        otaUploadActive = true;
    }

    if (otaUploadHasError || !otaWriter || otaUploadFinished)
        return;

    // Write chunk (the writer classifies it and holds the image header
    // until it is checked against this chip)
    if (len)
    {
        String error;
        if (!otaWriter->write(data, len, error))
        {
            abortUpload(error);
            return;
        }
        otaUploadWritten += len;

        // Progress log
        if (otaUploadExpected > 0)
        {
            int pct = (int)((otaUploadWritten * 100ULL) / (size_t)otaUploadExpected);
            int before = (int)(((otaUploadWritten - len) * 100ULL) / (size_t)otaUploadExpected);
            if (pct / 10 != before / 10)
            {
                logger.info("OTA progress: " + String(pct) + "% (" + String(otaUploadWritten) + " / " + String(otaUploadExpected) + " bytes)");
            }
        }

        // Yield + WDT
        esp_task_wdt_reset();
        delay(0);
    }

    if (final)
    {
        logger.info("Firmware upload finished: total " + String(otaUploadWritten) + " bytes");
        // Length: a multipart upload ends at its boundary (no announced
        // length); a cut upload never gets here
        otaUploadOutcome = otaWriter->finish(0);
        if (otaUploadOutcome.cls == GatewayImage::Class::Refused)
        {
            // logged once, as an error (a wrong signature block or a damaged image)
            abortUpload(String("refused: ") + otaUploadOutcome.text);
            return;
        }
        otaUploadFinished = true;
        // concluded by handleFirmwareUploadComplete() (or at the disconnect)
    }
}
