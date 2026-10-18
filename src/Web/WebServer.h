/**
 * expLORA Gateway
 *
 * Web portal manager header file
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
#include <mutex>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include "../Data/SensorManager.h"
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"
#include "../Protocol/MQTTManager.h"
#include "WebAuth.h"

/**
 * Class for web portal management
 *
 * Provides a web interface for device configuration and monitoring.
 * Offers API for accessing sensor data.
 */
class WebPortal
{
private:
    // Task handle for web server
    // TaskHandle_t webServerTaskHandle = NULL;

    MQTTManager *mqttManager; // Reference to MQTT manager

    // Static task function for the second core
    static void webServerTask(void *parameter);

    // Processing flag to avoid race conditions
    volatile bool isProcessing = false;

    AsyncWebServer server;        // Asynchronous web server
    DNSServer dnsServer;          // DNS server for captive portal
    SensorManager &sensorManager; // Reference to sensor manager
    Logger &logger;               // Reference to logger

    bool isAPMode; // AP mode (true) or client mode (false)
    bool routesRegistered = false; // Ensure routes are registered only once
    bool serverStarted = false;    // Track server begin/end state

    // Device configuration - reference to external configuration
    String &wifiSSID;
    String &wifiPassword;
    bool &configMode;
    String &timezone;


    // Firmware upload state
    bool otaUploadActive = false; // Update.begin() succeeded, not finished/aborted yet
    AsyncWebServerRequest *otaUploadOwner = nullptr; // request that runs the upload
    bool otaUploadSawFile = false;                   // the owner already sent one file part
    bool otaUploadHasError = false;
    String otaUploadErrorMsg = "";
    size_t otaUploadExpected = 0;
    size_t otaUploadWritten  = 0;
    // Image header collected across chunks for the chip check (a TCP
    // segment boundary can split it)
    uint8_t otaHeader[32];
    size_t otaHeaderLen = 0;

    // Deferred restart: handlers set these and handleClient() executes the reboot
    // once the response has had time to flush. Avoids ESP.restart() inside async handlers.
    volatile bool restartRequested = false; // set by handlers (async task), read by the loop

    // Firmware download requested by /firmware/update, done by the loop task
    String pendingOtaUrl;
    volatile bool urlOtaRunning = false; // loop task is downloading firmware
    std::mutex otaUrlMutex;
    void runUrlOta(const String &url);

    // Update check, also done by the loop task; the page polls for the result
    bool updateCheckRequested = false;
    bool updateCheckRunning = false;
    bool updateCheckDone = false;
    int updateCheckStatus = 0;
    String updateCheckResult;
    unsigned long updateCheckAt = 0;
    void runUpdateCheck();
    void abortUpload(const String &reason);
    volatile unsigned long restartAt = 0; // always written before restartRequested

    ConfigManager &configManager; // Reference to configuration
    WebAuth auth;                 // Admin password and login sessions

    // Every route except a few public ones needs a login (see setupRoutes)
    void requireLogin(AsyncWebServerRequest *request, ArMiddlewareNext next);
    void handleLogin(AsyncWebServerRequest *request);
    void handleLoginPost(AsyncWebServerRequest *request);
    void handleLogout(AsyncWebServerRequest *request);
    void handlePassword(AsyncWebServerRequest *request);
    void handlePasswordPost(AsyncWebServerRequest *request);

    // Setup routes for web server
    void setupRoutes();
    // First setup (AP, no WiFi configured, no uplink): redirect to /config, true
    bool guardAPOnly(AsyncWebServerRequest *request);

    // Create content for individual pages
    String createHomePage();
    String createConfigPage();
    String createSensorsPage();
    String createSensorAddPage();
    String createSensorEditPage(int index);
    String createLogsPage();
    String createAPIPage();

    // Process HTTP requests
    void handleRoot(AsyncWebServerRequest *request);
    void handleConfig(AsyncWebServerRequest *request);
    void handleConfigPost(AsyncWebServerRequest *request);
    void handleSensors(AsyncWebServerRequest *request);
    void handleSensorAdd(AsyncWebServerRequest *request);
    void handleSensorAddPost(AsyncWebServerRequest *request);
    void handleSensorEdit(AsyncWebServerRequest *request);
    void handleSensorEditPost(AsyncWebServerRequest *request);
    void handleSensorDelete(AsyncWebServerRequest *request);
    void handleGen2Add(AsyncWebServerRequest *request);
    void handleGen2AddPost(AsyncWebServerRequest *request);
    void handleGen2Delete(AsyncWebServerRequest *request);
    void handleGen2Detail(AsyncWebServerRequest *request);
    void handleGen2Config(AsyncWebServerRequest *request);
    void handleGen2Status(AsyncWebServerRequest *request);
    void handleGen2ResetNonce(AsyncWebServerRequest *request);
    void sendGen2Detail(AsyncWebServerRequest *request, uint32_t devId, const String &message, int code = 200);
    void handleLogs(AsyncWebServerRequest *request);
    void handleLogsClear(AsyncWebServerRequest *request);
    void handleLogLevel(AsyncWebServerRequest *request);
    void handleAPI(AsyncWebServerRequest *request);
    void handleNetwork(AsyncWebServerRequest *request);
#if defined(HAS_ETHERNET) || defined(HAS_LTE)
    void handleNetworkPost(AsyncWebServerRequest *request);
#endif
    void handleDiagNetwork(AsyncWebServerRequest *request);
    void handleMqtt(AsyncWebServerRequest *request);
    void handleMqttPost(AsyncWebServerRequest *request);
    void handleFirmwareVersion(AsyncWebServerRequest *request);
    void handleFirmwareCheck(AsyncWebServerRequest *request);
    void handleFirmwareUpdate(AsyncWebServerRequest *request);
    void handleReboot(AsyncWebServerRequest *request);
    void handleNotFound(AsyncWebServerRequest *request);
    void handleDiagHeap(AsyncWebServerRequest *request);

    // Firmware update handling
    void handleFirmwarePage(AsyncWebServerRequest *request);
    void handleFirmwareUploadComplete(AsyncWebServerRequest *request);
    void handleFirmwareUploadChunk(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);


    // Receive WebSocket messages
    void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                          AwsEventType type, void *arg, uint8_t *data, size_t len);

    // Send updates via WebSocket
    void sendSensorUpdates();
    void sendLogUpdates();

public:
    // Constructor
    WebPortal(SensorManager &sensors, Logger &log, String &ssid, String &password,
              bool &configMode, ConfigManager &config, String &timezone);
    // Destructor
    ~WebPortal();

    // Web server initialization
    bool init();

    // Handle requests
    void handleClient();

    // Process DNS requests (for captive portal)
    void processDNS();

    // Switch between AP and client modes
    void setAPMode(bool enable);

    // Get current mode
    bool isInAPMode() const;

    // Restart web server
    void restart();

    // Set MQTTManager reference
    void setMqttManager(MQTTManager *manager) { mqttManager = manager; }

    // Forgotten admin password: cleared from the serial console
    void resetAdminPassword() { auth.resetPassword(); }
};
