/**
 * expLORA Gateway
 *
 * LoRa radio interface - common API for the supported radio chips
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
#include "../Data/Logging.h"
#include "SPI_Manager.h"

// Last driver error: what failed and the RadioLib code (what = "none" if none)
struct RadioError
{
    const char *what;
    int16_t code;
};

/**
 * Abstract LoRa radio
 *
 * Each driver configures the chip with the LORA_* parameters from config.h
 * and stays in continuous receive mode on the uplink channel; for a protocol
 * v2 response it is switched to the downlink channel, the frame is staged
 * (RadioLib stageMode) and launched at the exact time (launchMode), then it
 * returns to receive.
 * The chip's "packet ready" pin timestamps the event and notifies the radio
 * task (RadioTask.h), the only user of the driver after setup().
 */
class LoRaRadio
{
protected:
    static void IRAM_ATTR handleInterrupt(); // DIO interrupt handler

public:
    virtual ~LoRaRadio() = default;

    // Initialize the chip and start continuous receive
    virtual bool init() = 0;

    // Reset the chip and restart receive
    virtual bool reset() = 0;

    // Read a received packet; false if none is ready or it is invalid
    virtual bool receivePacket(uint8_t *buffer, uint8_t *length) = 0;

    // RSSI of the last packet in dBm
    virtual int getRSSI() = 0;

    // SNR of the last packet in dB
    virtual float getSNR() = 0;

    // Frequency error of the last packet in Hz
    virtual float getFrequencyError() = 0;

    // Check if the chip responds
    virtual bool isConnected() = 0;

    // The packet-ready line is still high (an RxDone nobody picked up)
    virtual bool irqPinActive() = 0;

    // False after receive could not be restarted (chip out of continuous
    // RX, deaf until reset): the radio task resets it at once
    virtual bool receiving() const = 0;

    // Reason of the last failed receivePacket()/reset(); drivers never log
    // from paths the radio task uses (receivePacket, isConnected, reset)
    virtual RadioError lastError() const = 0;

    // --- Downlink (radio task only; no logging) ---------------------------

    // RxDone -> response launch offset of this chip (M0 measurement)
    virtual int64_t chipOffsetUs() const = 0;

    // A packet is being received right now (header seen, RxDone not yet)
    virtual bool receivingNow() = 0;

    // Standby (XOSC on SX1262), retune to the downlink channel, set the
    // power and stage the frame; the launch follows with launchTx()
    virtual int16_t prepareTx(const uint8_t *frame, size_t len) = 0;
    virtual int16_t launchTx() = 0;

    // After TxDone (or a failed launch): back to receive on the uplink channel
    virtual void finishTx() = 0;

    // Update mode (radio task only): receive off, chip in standby. Back
    // with reset(), which re-initializes it and restarts receive
    virtual bool standby() = 0;

    // Time on air of a frame on the downlink channel
    virtual uint32_t timeOnAirUs(size_t len) = 0;

    // Chip name for logs and the web UI
    virtual const char *getChipName() const = 0;

    // Task to notify from the DIO interrupt; an interrupt that came before
    // the task was set is handed over at once
    static void setNotifyTask(TaskHandle_t task);

    // esp_timer time of the last DIO interrupt (microseconds), whether a
    // flash write section was open and how many had closed when it ran
    static int64_t lastInterruptUs(bool *duringWrite = nullptr, uint32_t *closedSections = nullptr);
};

// Create the radio driver selected in board_config.h
LoRaRadio *createLoRaRadio(Logger &logger, SPIManager *spiManager);
