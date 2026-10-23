/**
 * expLORA Gateway
 *
 * Main configuration file for the expLORA Gateway
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
#include <WiFi.h>
#include <esp_mac.h>

// Firmware version
#define FIRMWARE_VERSION "2.0.0"

// OTA Configuration: FIRMWARE_UPDATE_URL is per board (board_config.h)
#include "board_config.h"

// LoRa radio parameters - must match the sensors (receive only)
#define LORA_FREQUENCY_MHZ 868.0
#define LORA_BANDWIDTH_KHZ 125.0
#define LORA_SPREADING_FACTOR 9
#define LORA_CODING_RATE 5      // 4/5
#define LORA_SYNC_WORD 0x12     // private network (SX126x: 0x1424)
#define LORA_PREAMBLE_LENGTH 16
// Protocol v2 downlink (docs/protocol-v2.md section 1): 869.525 MHz, same
// modulation; power per board in board_config.h (LORA_DOWNLINK_POWER_DBM)
#define LORA_DOWNLINK_MHZ 869.525

// Application configuration
#define MAX_SENSORS 20                // Maximum number of sensors
#ifndef LOG_BUFFER_SIZE
#define LOG_BUFFER_SIZE 150           // Size of log buffer (entries)
#endif
#define AP_TIMEOUT 300000             // AP mode timeout (5 minutes in milliseconds)
#define WIFI_RECONNECT_INTERVAL 60000 // WiFi reconnect attempt interval (1 minute in milliseconds)
#define WDT_TIMEOUT 10                // Watchdog timeout in seconds

// File system configuration
#define CONFIG_FILE "/config.json"   // Configuration file
#define SENSORS_FILE "/sensors.json" // Sensors file
#define SENSORS_BACKUP_FILE "/sensors.bak" // last good copy of SENSORS_FILE

// NTP configuration
// Format uint32_t as zero-padded uppercase hex string
// width=6 for serial numbers (3 bytes), width=8 for device keys (4 bytes)
inline String formatHex(uint32_t value, uint8_t width)
{
    String hex = String(value, HEX);
    hex.toUpperCase();
    while (hex.length() < width)
        hex = "0" + hex;
    return hex;
}

#define formatSN(val) formatHex(val, 6)
#define formatKey(val) formatHex(val, 8)

// WiFi station MAC from eFuse. Unlike WiFi.macAddress() it is valid before
// the WiFi driver starts (Arduino core 3.x returns zeros until then).
inline void getStationMac(uint8_t mac[6])
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

// Generate the unique AP SSID for this device from its MAC address.
// Format: "expLORA-GW-XXXXXX" where XXXXXX is the last 3 bytes of the MAC in uppercase hex.
inline String makeApName()
{
    uint8_t mac[6];
    getStationMac(mac);
    char suffix[7];
    snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    return "expLORA-GW-" + String(suffix);
}

// NTP: up to three servers; a server announced by DHCP takes the first slot.
// SNTP re-syncs every 3 h (CONFIG_LWIP_SNTP_UPDATE_DELAY); between syncs and
// when a sync fails the clock keeps running.
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.cloudflare.com"
#define NTP_SERVER_3 "time.google.com"
#define TIME_STALE_AFTER_S (24 * 3600) // no successful sync for this long = stale
// Time zone: the list of names is in Data/TimeZones.h (default Europe/Prague)
#define DEFAULT_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3" // Central European Time with auto DST

// MQTT Configuration
#define MQTT_DEFAULT_HOST "homeassistant.local"
#define MQTT_DEFAULT_PORT 1883
#define MQTT_DEFAULT_USER ""
#define MQTT_DEFAULT_PASS ""
#define MQTT_DEFAULT_ENABLED false
#define MQTT_DEFAULT_TLS false
#ifndef MQTT_RECONNECT_INTERVAL
#define MQTT_RECONNECT_INTERVAL 30000 // 30 seconds in milliseconds
#endif
#define MQTT_DEFAULT_PREFIX "explora"
#define HA_DISCOVERY_DEFAULT_ENABLED true
#define HA_DISCOVERY_DEFAULT_PREFIX "homeassistant"

// LTE defaults (boards with HAS_LTE)
#define LTE_DEFAULT_ENABLED false
#define LTE_DEFAULT_APN "" // empty = APN assigned by the network

// CORS headers for API
#define CORS_HEADER_NAME "Access-Control-Allow-Origin"
#define CORS_HEADER_VALUE "*"

// Webserver ports
#define HTTP_PORT 80 // HTTP server port
#define DNS_PORT 53  // DNS server port

// Sensor type definitions - easily extendable for additional types
#define SENSOR_TYPE_UNKNOWN  0x00
#define SENSOR_TYPE_BME280   0x01  // Temperature, humidity, pressure
#define SENSOR_TYPE_SCD40    0x02  // Temperature, humidity, CO2
#define SENSOR_TYPE_METEO    0x03  // Meteorological station
#define SENSOR_TYPE_VEML7700 0x04  // Light sensor (LUX)
#define SENSOR_TYPE_SHT40    0x05  // Temperature, humidity
#define SENSOR_TYPE_DIY_TEMP 0x51  // Temperature
// Add additional sensor types here...

// PSRAM configuration for web server

// OTA update settings
#define OTA_PORT 3232        // OTA update port