/**
 * expLORA Gateway
 *
 * Main program file for the expLORA Gateway
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

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>

// Configuration headers
#include "config.h"

// Data
#include "Data/SensorTypes.h"
#include "Data/SensorData.h"
#include "Data/Logging.h"
#include "Data/SensorManager.h"

// Hardware
#include "Hardware/LoRaRadio.h"
#include "Hardware/RadioTask.h"
#include "Hardware/Liveness.h"
#include "Network/Uplink.h"
#include "Hardware/PSRAM_Manager.h"
#include "Hardware/PowerSource.h"
#include "Hardware/SPI_Manager.h"
#include "Hardware/WatchdogPause.h"
#include "Hardware/HeapGuard.h"
#include "Security/VersionMarker.h"
#include "Update/UpdateMode.h"
#include "Update/GatewayOta.h"

// Storage
#include "Storage/ConfigManager.h"

// Protocol
#include "Protocol/LoRaProtocol.h"
// MQTT
#include "Protocol/MQTTManager.h"

// Web interface
#include "Web/WebServer.h"
#include "Web/HTMLGenerator.h"

// Global object instances
Logger logger;                // Logging system
ConfigManager *configManager; // Configuration manager
SPIManager *spiManager;       // SPI communication manager
Uplink *uplink;               // Connectivity policy
LoRaRadio *loraModule;        // LoRa radio (chip selected in board_config.h)
SensorManager *sensorManager; // Sensor manager
LoRaProtocol *loraProtocol;   // LoRa protocol
MQTTManager *mqttManager;     // MQTT Manager
WebPortal *webPortal;         // Web interface
PowerSource *powerSource;     // Supply source (PoE / USB) on boards that detect it

// Restarts because the heap stayed fragmented (loop()), counted in RTC memory:
// kept across ESP.restart() and panics, cleared by a power cycle and after an
// hour with a healthy heap. Logged at boot and shown in /diag/heap
static constexpr uint32_t FRAG_RESTART_MAGIC = 0x46524147; // "FRAG"
static constexpr uint32_t FRAG_RESTARTS_MAX = 3;
RTC_NOINIT_ATTR static uint32_t fragRestartMagic;
RTC_NOINIT_ATTR uint32_t heapFragRestarts;

// Timer for disabling AP mode

// Loop task stack: TLS handshakes (update check, URL OTA, HTTPS forwarding,
// MQTT TLS) run in loop(); 8 kB left under 4 kB headroom (see /diag/heap)
SET_LOOP_TASK_STACK_SIZE(10 * 1024);

// File system initialization
bool initFileSystem()
{
    // Format only when mounting keeps failing (a blank partition on the first
    // boot) - not on one transient error, which would wipe all settings
    bool mounted = false;
    for (int attempt = 0; attempt < 3 && !mounted; attempt++)
    {
        if (attempt > 0)
        {
            delay(500);
        }
        mounted = LittleFS.begin(false);
    }
    if (!mounted)
    {
        Serial.println("LittleFS mount failed 3 times - formatting (sensors and settings files are lost)");
        mounted = LittleFS.begin(true);
    }
    if (!mounted)
    {
        Serial.println("ERROR: Failed to mount LittleFS");
        return false;
    }
    Serial.println("LittleFS mounted successfully");
    return true;
}

// Setup - device initialization
// Rollback (Update/GatewayOta.h): the core leaves a new firmware in the
// bootloader's PENDING_VERIFY state; GatewayOta::process() marks it valid
// after 120 s of normal operation, a reset before that rolls back
extern "C" bool verifyRollbackLater()
{
    return true;
}

void setup()
{
    Liveness::captureLastRestart(); // before anything can restart again
    // Task watchdog first: the core starts it with 5 s and the CPU0 idle task
    // watched. Change the timeout, keep panic, and stop watching the CPU0
    // idle task: the lwIP thread (priority 18, pinned to CPU0) has CPU0 to
    // itself for as long as a LAN host floods the gateway with connections
    // (~500 a second on the bench), so the idle task starved and the
    // watchdog rebooted the gateway (at a normal heap too, and during boot
    // into a flood before this ran). Watched: the loop task (added at the
    // end of setup()) and the AsyncTCP task; a hung lwIP thread and a hung
    // radio task are caught by Hardware/Liveness. Accepted losses: a task on
    // CPU0 spinning without blocking the others is no longer caught (the
    // interrupt watchdog is unaffected), and setup() itself is not watched
    // (the loop task is added at its end; it can block for seconds on WiFi /
    // modem start)
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    if (esp_task_wdt_reconfigure(&wdtConfig) == ESP_ERR_INVALID_STATE)
    {
        esp_task_wdt_init(&wdtConfig);
    }

    // Serial port initialization
    Serial.begin(115200);
    delay(1000); // Wait for stabilization

    Serial.println("\n\n" BOARD_NAME);
    Serial.println("------------------------------------------------------------");

// Explicit PSRAM initialization
#ifdef BOARD_HAS_PSRAM
    if (psramInit())
    {
        Serial.println("PSRAM initialized manually!");
    }
#endif

    // Memory diagnostics
    Serial.printf("Total heap: %d bytes\n", ESP.getHeapSize());
    Serial.printf("Free heap: %d bytes (8-bit: %u bytes)\n", ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

// PSRAM initialization
#ifdef BOARD_HAS_PSRAM
    bool psramInitialized = false;

    // Try different PSRAM detection methods
    if (psramFound())
    {
        psramInitialized = true;
        Serial.println("PSRAM initialized via psramFound()");
    }
    else if (ESP.getPsramSize() > 0)
    {
        psramInitialized = true;
        Serial.println("PSRAM detected via ESP.getPsramSize()");
    }

    if (psramInitialized)
    {
        Serial.printf("Total PSRAM: %d bytes\n", ESP.getPsramSize());
        Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
    }
    else
    {
        Serial.println("PSRAM initialization failed or not available");
    }
#endif

    // File system initialization
    if (!initFileSystem())
    {
        Serial.println("ERROR: File system initialization failed");
        return;
    }

    // Logging system initialization
    if (!logger.init(LOG_RING_BYTES))
    {
        Serial.println("ERROR: Logger initialization failed");
        return;
    }

    // Basic log after initialization
    // The version from the image's version marker (read by the release tool)
    logger.info(String(BOARD_NAME) + " starting up - Firmware v" + String(VersionMarker::version()));
    if (fragRestartMagic != FRAG_RESTART_MAGIC || esp_reset_reason() == ESP_RST_POWERON ||
        heapFragRestarts > FRAG_RESTARTS_MAX)
    {
        fragRestartMagic = FRAG_RESTART_MAGIC;
        heapFragRestarts = 0;
    }
    else if (heapFragRestarts > 0)
    {
        logger.warning("Restarted because the heap stayed fragmented (" + String(heapFragRestarts) + " of at most " +
                       String(FRAG_RESTARTS_MAX) + " such restarts in a row)");
    }
    Liveness::logLastRestart(logger);
    UpdateMode::logLastRestart(logger);
    // Gateway firmware install record: rollback, validation of a new firmware
    GatewayOta::begin(logger);

    // Configuration manager initialization
    configManager = new ConfigManager(logger);
    if (!configManager->init())
    {
        logger.error("Failed to initialize configuration manager");
        return;
    }

    // Setting logging level according to configuration
    logger.setLogLevel(configManager->logLevel);

    powerSource = new PowerSource(logger);
    powerSource->begin();

    // SPI manager initialization
    spiManager = new SPIManager(logger);
    if (!spiManager->init())
    {
        logger.error("Failed to initialize SPI manager");
        return;
    }

    // Sensor manager initialization
    sensorManager = new SensorManager(logger);
    if (!sensorManager->init())
    {
        logger.error("Failed to initialize sensor manager");
    }
    else
    {
        logger.info("Sensor manager initialized with " +
                    String(sensorManager->getSensorCount()) + " sensors");
    }

    sensorManager->otaBegin(); // OTA image store and job (protocol v2, spec 14.5)

    // Network: ethernet, AP and/or WiFi station, time sync once online
    uplink = new Uplink(*configManager, logger);
    uplink->begin();

    webPortal = new WebPortal(*sensorManager, logger,
                              configManager->wifiSSID, configManager->wifiPassword,
                              configManager->configMode, *configManager);

    // LoRa module initialization
    // The radio and its SPI bus belong to the radio task from here on
    loraModule = createLoRaRadio(logger, spiManager);
    if (!loraModule->init())
    {
        logger.error("Failed to initialize LoRa module");
    }
    else
    {
        logger.info(String("LoRa module initialized successfully (") + loraModule->getChipName() + ")");
    }
    // Started even after a failed init: the health check re-initializes the chip
    if (!RadioTask::start(loraModule, logger))
    {
        logger.error("Failed to start the radio task");
    }
    // lwIP thread and radio task heartbeats (Hardware/Liveness.h)
    Liveness::start();

    // LoRa protocol initialization
    loraProtocol = new LoRaProtocol(*sensorManager, logger);

    // Initialize MQTT Manager
    mqttManager = new MQTTManager(*sensorManager, *configManager, logger);
    if (!mqttManager->init())
    {
        logger.debug("MQTT Manager initialization skipped (disabled in config)");
    }

    if (!webPortal->init())
    {
        logger.error("Failed to initialize web portal");
    }
    else
    {
        logger.info("Web portal initialized successfully");
    }

    webPortal->setMqttManager(mqttManager);

    // Captive portal follows local network access
    uplink->setCaptiveCallback([](bool captive)
                               { webPortal->setAPMode(captive); });

    // MQTT session stays on the old interface's address - reconnect over the new one
    uplink->setActiveChangeCallback([](Uplink::Type)
                                    { mqttManager->disconnect(); });

    // Task watchdog (configured at the start of setup()): the loop task from here on
    esp_task_wdt_add(NULL);
    // The AsyncTCP task subscribes itself when it starts; make sure a failed
    // self-subscription (e.g. no memory, booting into a flood) does not
    // leave it unwatched
    TaskHandle_t asyncTask = xTaskGetHandle("async_tcp");
    if (asyncTask && esp_task_wdt_status(asyncTask) == ESP_ERR_NOT_FOUND)
    {
        esp_err_t added = esp_task_wdt_add(asyncTask);
        logger.warning(String("AsyncTCP task was not on the task watchdog - ") +
                       (added == ESP_OK ? "added" : "could not be added"));
    }
    logger.info("Task watchdog initialized with timeout of " + String(WDT_TIMEOUT) + " seconds");

    logger.info("System initialization complete");
}

// Loop - main loop
void loop()
{
    esp_task_wdt_reset();

    // setup() stopped early (filesystem / configuration failure, logged)
    if (!uplink || !powerSource)
    {
        delay(1000);
        return;
    }

    // Connectivity: station reconnect, AP / captive portal, time sync
    uplink->process();
    powerSource->process();

    // If in AP mode, process DNS captive portal:
    if (webPortal && webPortal->isInAPMode())
    {
        webPortal->processDNS(); // This method internally calls dnsServer.processNextRequest();
    }

    // Handle web interface
    if (webPortal)
    {
        webPortal->handleClient();
    }

    // Update mode: hard cap of a session, connectivity after one
    UpdateMode::process();

    // A new firmware is validated after 120 s with the web server up and the
    // lwIP thread / radio task heartbeats fresh (Hardware/Liveness); an image
    // waiting for its confirmation expires after 10 minutes
    {
        bool healthy = false;
        if (GatewayOta::validationPending())
        {
            const Liveness::Status live = Liveness::status();
            healthy = live.tcpipAgeMs < Liveness::TCPIP_HANG_MS && live.radioAgeMs < Liveness::RADIO_HANG_MS;
        }
        GatewayOta::process(webPortal && webPortal->started(), healthy);
    }

    // Process MQTT communication (settings changes are applied even offline)
    if (mqttManager)
    {
        mqttManager->process(); // pauses the watchdog itself around socket work
    }

    // What the radio task counted (it never logs itself)
    RadioTask::logEvents(logger);

    // LoRa packets queued by the radio task (a few per pass, the rest wait
    // in the queue), published to MQTT when decoded
    static RadioPacket packet; // 280 bytes, kept off the loop stack
    for (int i = 0; i < 4 && loraProtocol && RadioTask::receive(packet); i++)
    {
        if (loraProtocol->processPacket(packet) && mqttManager && uplink->isOnline())
        {
            WatchdogPause pause;
            mqttManager->publishSensorData(loraProtocol->getLastProcessedSensorIndex());
        }
    }

    // Outcomes of generation 2 responses (per-sensor downlink counters)
    TxResult txResult;
    while (sensorManager && RadioTask::takeResult(txResult))
    {
        sensorManager->recordTxResult(txResult);
    }

    // OTA: launch outcomes, then the job (deadlines, the next block frame, job.json)
    OtaResult otaResult;
    while (sensorManager && RadioTask::otaTakeResult(otaResult))
    {
        sensorManager->otaResult(otaResult);
    }
    if (sensorManager)
    {
        sensorManager->otaProcess(millis());
    }

    // Short delay for stability
    delay(5);

    // The web server refuses pages while the largest usable (8-bit) block
    // is below their largest allocation (6-9 kB); if fragmentation keeps it
    // under 8 kB for 15 minutes, restart rather than stay unmanageable until
    // someone power-cycles the gateway. Idle: Pro ~36 kB, Lite ~60 kB.
    // Not counted while an MQTT TLS session is up: its record buffers (2 x
    // 16.7 kB) stay allocated as long as it is connected, and a restart would
    // only connect it again into the same heap - a restart every 15 minutes.
    // HTTPS sessions (update check, URL OTA, forwarding) run inside this loop
    // and are closed again before this check. At most FRAG_RESTARTS_MAX
    // restarts in a row; the counter clears after an hour with a healthy heap
    static unsigned long lowBlockSince = 0;
    static unsigned long healthySince = 0;
    static bool fragGaveUp = false;
    size_t largest8 = HeapGuard::usable().largest;
    bool tlsSessionUp = mqttManager && mqttManager->isConnected() && mqttManager->usesTls();
    if (largest8 < 8192)
    {
        healthySince = 0;
        if (tlsSessionUp)
        {
            lowBlockSince = 0;
        }
        else if (lowBlockSince == 0)
        {
            lowBlockSince = millis() | 1;
            logger.warning("Heap fragmented: largest free block " + String(largest8) + " B");
        }
        else if (millis() - lowBlockSince > 900000 && heapFragRestarts < FRAG_RESTARTS_MAX)
        {
            heapFragRestarts++;
            logger.error("Heap fragmented for 15 minutes - restarting (" + String(heapFragRestarts) + " of at most " +
                         String(FRAG_RESTARTS_MAX) + " such restarts in a row)");
            if (sensorManager)
            {
                sensorManager->saveSensors(); // keep the rain totals
            }
            delay(200);
            ESP.restart();
        }
        else if (millis() - lowBlockSince > 900000 && !fragGaveUp)
        {
            fragGaveUp = true;
            logger.error("Heap fragmented for 15 minutes after " + String(FRAG_RESTARTS_MAX) +
                         " restarts in a row - not restarting again until the heap recovers for an hour");
        }
    }
    else
    {
        lowBlockSince = 0;
        if (healthySince == 0)
        {
            healthySince = millis() | 1;
        }
        else if (heapFragRestarts > 0 && millis() - healthySince > 3600000)
        {
            heapFragRestarts = 0;
            fragGaveUp = false;
            logger.info("Heap healthy for an hour - fragmentation restart counter cleared");
        }
    }

    if (sensorManager)
    {
        sensorManager->process(millis());
    }

    // Serial console: "reset-password" clears a forgotten web admin password
    static String serialLine;
    while (Serial.available())
    {
        char c = Serial.read();
        if (c == '\n' || c == '\r')
        {
            serialLine.trim();
            if (serialLine == "reset-password" && webPortal)
            {
                webPortal->resetAdminPassword();
            }

            serialLine = "";
        }
        else if (serialLine.length() < 32)
        {
            serialLine += c;
        }
    }

    // Periodically rotate per-sensor health buckets so a dead-silent sensor
    // (no packets at all) eventually drops out of the 24h "OK" tally instead
    // of clinging to whatever was in the last bucket at boot.
    static unsigned long lastHealthTick = 0;
    if (millis() - lastHealthTick > 60000) // once a minute is plenty
    {
        lastHealthTick = millis();
        if (sensorManager)
        {
            sensorManager->tickSensorHealth(millis());
        }
    }

    // Memory diagnostics every 10 minutes
    static unsigned long lastMemCheck = 0;
    if (millis() - lastMemCheck > 600000)
    { // 10 minutes
        lastMemCheck = millis();

        // 8-bit = what malloc / String / lwIP / mbedTLS can use; internal
        // also counts the Pro's ~33 kB of 32-bit-only IRAM
        HeapGuard::Heap heap = HeapGuard::usable();
        logger.info("Memory status - 8-bit heap: free " + String(heap.free) + " B, largest block " +
                    String(heap.largest) + " B, low " + String(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)) +
                    " B (internal incl. 32-bit-only: free " + String(ESP.getFreeHeap()) + " B)");

#ifdef BOARD_HAS_PSRAM
        if (psramFound())
        {
            logger.debug("PSRAM status - Free: " + String(ESP.getFreePsram()) +
                         " bytes, Largest block: " + String(ESP.getMaxAllocPsram()) + " bytes");
        }
#endif
    }
}
