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
#include "Network/Uplink.h"
#include "Hardware/PSRAM_Manager.h"
#include "Hardware/PowerSource.h"
#include "Hardware/SPI_Manager.h"
#include "Hardware/WatchdogPause.h"

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
void setup()
{
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
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

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
    if (!logger.init(LOG_BUFFER_SIZE))
    {
        Serial.println("ERROR: Logger initialization failed");
        return;
    }

    // Basic log after initialization
    logger.info(String(BOARD_NAME) + " starting up - Firmware v" + String(FIRMWARE_VERSION));

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

    // Network: ethernet, AP and/or WiFi station, time sync once online
    uplink = new Uplink(*configManager, logger);
    uplink->begin();

    webPortal = new WebPortal(*sensorManager, logger,
                              configManager->wifiSSID, configManager->wifiPassword,
                              configManager->configMode, *configManager, configManager->timezone);

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

    // Initialize task watchdog
    // The core already starts the TWDT (5 s, idle task on CPU0) - only change timeout, keep panic
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = 1 << 0,
        .trigger_panic = true,
    };
    if (esp_task_wdt_reconfigure(&wdtConfig) == ESP_ERR_INVALID_STATE)
    {
        esp_task_wdt_init(&wdtConfig);
    }
    esp_task_wdt_add(NULL); // Add current task to watchdog
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

    // Short delay for stability
    delay(5);

    // The web server refuses requests while the largest free block is below
    // 8 kB; if fragmentation keeps it there for 15 minutes, restart rather than stay
    // unmanageable until someone power-cycles the gateway
    static unsigned long lowBlockSince = 0;
    if (ESP.getMaxAllocHeap() < 8192)
    {
        if (lowBlockSince == 0)
        {
            lowBlockSince = millis() | 1;
            logger.warning("Heap fragmented: largest free block " + String(ESP.getMaxAllocHeap()) + " B");
        }
        else if (millis() - lowBlockSince > 900000)
        {
            logger.error("Heap fragmented for 15 minutes - restarting");
            if (sensorManager)
            {
                sensorManager->saveSensors(); // keep the rain totals
            }
            delay(200);
            ESP.restart();
        }
    }
    else
    {
        lowBlockSince = 0;
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

        logger.info("Memory status - Free heap: " + String(ESP.getFreeHeap()) +
                    " bytes, Largest block: " + String(ESP.getMaxAllocHeap()) + " bytes");

#ifdef BOARD_HAS_PSRAM
        if (psramFound())
        {
            logger.debug("PSRAM status - Free: " + String(ESP.getFreePsram()) +
                         " bytes, Largest block: " + String(ESP.getMaxAllocPsram()) + " bytes");
        }
#endif
    }
}
