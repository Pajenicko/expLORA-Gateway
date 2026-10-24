#ifndef BOARD_CONFIG_H_
#define BOARD_CONFIG_H_

// expLORA Gateway Pro (EXPLORA GATEWAY PRO v1.0, ESP32-WROOM-32E-N16)
#define BOARD_NAME "expLORA Gateway Pro"

// OTA auto-update manifest
#define FIRMWARE_UPDATE_URL "https://pajenicko.cz/webflash/explora-gw-pro-autoupdate.json"
// Board id in a gateway signature block (Security/SigBlock.h: 1 Pro, 2 Lite)
#define BOARD_SIGN_TARGET 1

// Radio chip: E22-900M22S = SX1262, TCXO on DIO3, RF switch RXEN + DIO2 (TXEN)
#define RADIO_SX1262
#define LORA_TCXO_VOLTAGE 1.8
#define LORA_DOWNLINK_POWER_DBM 14  // up to 22 dBm allowed at 869.525 MHz (27 dBm ERP)
#define LORA_CHIP_OFFSET_US 6400    // RxDone -> response timing offset measured in M0

#define LORA_CS 15   // NSS (strap MTDO)
#define LORA_RST 32  // NRST
#define LORA_DIO1 35 // DIO1 (interrupt), input only
#define LORA_BUSY 34 // BUSY, input only
#define LORA_RXEN 4  // RXEN; TXEN is driven by DIO2
#define SPI_MISO_PIN 33
#define SPI_MOSI_PIN 14
#define SPI_SCK_PIN 13

// Ethernet LAN8720A (RMII), REF_CLK from the 50 MHz oscillator X1 on GPIO0
#define HAS_ETHERNET
#define ETH_OSC_EN_PIN 16 // X1 OE, 2k2 pull-down: oscillator off during boot (GPIO0 strap)
#define ETH_NRST_PIN 12   // LAN8720A nRST (strap MTDI, low at boot)
#define ETH_MDC_PIN 23
#define ETH_MDIO_PIN 18
#define ETH_PHY_ADDRESS 0 // PHYAD0 internal pull-down

// LTE carrier A7682E (UART2)
#define HAS_LTE
#define LTE_PWR_PIN 2  // TPS22965 ON (+5V_LTE) + LED D301, 100k pull-down
#define LTE_RX_PIN 17  // UART2 RX <- modem TX
#define LTE_TX_PIN 5   // UART2 TX -> modem RX (keep as input while LTE is off)
#define LTE_NET_PIN 39 // NETLIGHT, input only

// Power source: TPS2116 ST, H = PoE (IN1), L = USB (IN2)
#define HAS_PWR_SRC
#define PWR_SRC_PIN 36 // input only, read by polling (ESP32 errata GPIO-3.14)

// Memory: no PSRAM, and WiFi + Ethernet + LTE share ~300 kB of internal RAM
// Log ring: ~95 typical entries (an entry takes ~86 B)
#define LOG_RING_BYTES 8192
// ~99 kB of 8-bit heap at idle with WiFi, LTE and Ethernet up (~56 kB
// with an MQTT TLS session): three pages at once (a page peaks at 16-26 kB;
// 60 parallel loads: 36 x 200, 24 x 503 Busy, none Low memory, 8-bit low
// 35.7 kB; the heap check still answers 503 when it is short)
#define WEB_HEAVY_RESPONSES_MAX 3
#define LOG_MESSAGE_CAPACITY 160

#endif
