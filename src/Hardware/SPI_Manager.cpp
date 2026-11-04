/**
 * expLORA Gateway
 *
 * SPI communication manager implementation file
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

#include "SPI_Manager.h"

// Constructor
SPIManager::SPIManager(Logger &log, int sck, int miso, int mosi)
    : spi(nullptr), logger(log), sckPin(sck), misoPin(miso), mosiPin(mosi), initialized(false)
{
}

// Destructor
SPIManager::~SPIManager()
{
    // We don't free the SPI instance, as it may be shared with other components
}

// SPI initialization
bool SPIManager::init(bool quiet)
{
    if (!quiet)
    {
        logger.debug("Initializing SPI interface: SCK=" + String(sckPin) + ", MISO=" + String(misoPin) +
                     ", MOSI=" + String(mosiPin));
    }

    spi = new SPIClass(HSPI);

    if (spi == nullptr)
    {
        if (!quiet)
        {
            logger.error("Failed to create SPI instance");
        }
        return false;
    }

    // Initialize SPI
    spi->begin(sckPin, misoPin, mosiPin);

    initialized = true;
    if (!quiet)
    {
        logger.info("SPI interface initialized successfully");
    }
    return true;
}

// Reset SPI
bool SPIManager::reset()
{
    if (!initialized)
    {
        return init();
    }

    logger.debug("Resetting SPI interface");

    // End and reinitialize SPI
    spi->end();
    delay(100);
    spi->begin(sckPin, misoPin, mosiPin);

    logger.info("SPI interface reset successfully");
    return true;
}

// Get SPI instance
SPIClass *SPIManager::getSPI()
{
    if (!initialized)
    {
        init();
    }

    return spi;
}

// Begin transaction
void SPIManager::beginTransaction(SPISettings settings)
{
    if (!initialized)
    {
        init();
    }

    spi->beginTransaction(settings);
}

// End transaction
void SPIManager::endTransaction()
{
    if (initialized)
    {
        spi->endTransaction();
    }
}

// Data transfer
uint8_t SPIManager::transfer(uint8_t data)
{
    if (!initialized)
    {
        init();
    }

    return spi->transfer(data);
}

// Data transfer - array
void SPIManager::transfer(uint8_t *data, size_t length)
{
    if (!initialized)
    {
        init();
    }

    for (size_t i = 0; i < length; i++)
    {
        data[i] = spi->transfer(data[i]);
    }
}

// Check if SPI is initialized
bool SPIManager::isInitialized() const
{
    return initialized;
}
