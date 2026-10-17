/**
 * expLORA Gateway
 *
 * SPI communication manager header file
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
#include <SPI.h>
#include "../Data/Logging.h"
#include "../config.h"
#include "board_config.h"

/**
 * Class for SPI communication management
 *
 * Ensures initialization and management of SPI interface for communication with LoRa module and other peripherals.
 * After setup() the bus is private to the radio task (RadioTask.h): no other
 * code may use it.
 */
class SPIManager
{
private:
    SPIClass *spi;  // SPI instance
    Logger &logger; // Reference to logger

    int sckPin;       // SCK pin
    int misoPin;      // MISO pin
    int mosiPin;      // MOSI pin
    bool initialized; // Initialization flag

public:
    // Constructor
    SPIManager(Logger &log, int sck = SPI_SCK_PIN, int miso = SPI_MISO_PIN, int mosi = SPI_MOSI_PIN);

    // Destructor
    ~SPIManager();

    // SPI initialization
    bool init(bool quiet = false); // quiet: no logging (radio task)

    // SPI reset
    bool reset();

    // Get SPI instance
    SPIClass *getSPI();

    // Begin transaction
    void beginTransaction(SPISettings settings = SPISettings(1000000, MSBFIRST, SPI_MODE0));

    // End transaction
    void endTransaction();

    // Data transfer
    uint8_t transfer(uint8_t data);
    void transfer(uint8_t *data, size_t length);

    // Check if SPI is initialized
    bool isInitialized() const;
};
