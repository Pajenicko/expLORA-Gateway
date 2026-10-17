/**
 * expLORA Gateway
 *
 * SX1262 (E22-900M22S) LoRa radio driver header file
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

#include "board_config.h"

#if defined(RADIO_SX1262)

#include <Arduino.h>
#include <RadioLib.h>
#include "../config.h"
#include "../Data/Logging.h"
#include "SPI_Manager.h"
#include "LoRaRadio.h"

/**
 * SX1262 LoRa radio driver (RadioLib)
 *
 * Receive only: the driver never calls transmit. DIO1 signals RxDone
 * (and CRC / header errors, which receivePacket() rejects).
 */
class RadioSX1262 : public LoRaRadio
{
private:
    Logger &logger;
    SPIManager *spiManager;
    Module *module;
    SX1262 *radio;
    bool initialized;
    int lastRssi;         // metadata of the last packet, read before
    float lastSnr;        // receive is restarted
    float lastFreqError;

    // Configure the chip and start continuous receive
    bool configure();

    RadioError lastErr = {"none", 0};
    bool rxRunning = false;

    // Create the RadioLib objects (no logging)
    bool create();

    // Remember a RadioLib error code
    void recordError(const char *what, int16_t state);

public:
    RadioSX1262(Logger &log, SPIManager *spiMgr);
    ~RadioSX1262() override;

    bool init() override;
    bool reset() override;
    bool receivePacket(uint8_t *buffer, uint8_t *length) override;
    int getRSSI() override;
    float getSNR() override;
    float getFrequencyError() override;
    bool isConnected() override;
    bool irqPinActive() override;
    RadioError lastError() const override { return lastErr; }
    bool receiving() const override { return rxRunning; }
    const char *getChipName() const override { return "SX1262"; }
};

#endif // RADIO_SX1262
