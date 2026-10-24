#ifndef BOARD_CONFIG_H_
#define BOARD_CONFIG_H_

// expLORA Gateway Lite
#define BOARD_NAME "expLORA Gateway Lite"

// OTA auto-update manifest
#define FIRMWARE_UPDATE_URL "https://pajenicko.cz/webflash/explora-gw-lite-autoupdate.json"
// Board id in a gateway signature block (Security/SigBlock.h: 1 Pro, 2 Lite)
#define BOARD_SIGN_TARGET 2

// Radio chip: RFM95W = SX1276
#define RADIO_SX1276
#define LORA_DOWNLINK_POWER_DBM 14  // max 17 dBm on PA_BOOST (the +20 dBm mode allows only 1 %)
#define LORA_CHIP_OFFSET_US 5900    // RxDone -> response timing offset measured in M0

// RFM95W pin definitions
#define LORA_CS 5       // NSS pin
#define LORA_RST 14     // RESET pin
#define LORA_DIO0 36    // DIO0 pin (interrupt)
#define SPI_MISO_PIN 35 // MISO pin
#define SPI_MOSI_PIN 37 // MOSI pin
#define SPI_SCK_PIN 18  // SCK pin

#endif
