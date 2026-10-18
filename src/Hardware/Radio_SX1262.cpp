/**
 * expLORA Gateway
 *
 * SX1262 (E22-900M22S) LoRa radio driver implementation file
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

#include "Radio_SX1262.h"

#if defined(RADIO_SX1262)

// Constructor
RadioSX1262::RadioSX1262(Logger &log, SPIManager *spiMgr)
    : logger(log), spiManager(spiMgr), module(nullptr), radio(nullptr), initialized(false), lastRssi(0), lastSnr(0), lastFreqError(0)
{
    if (spiManager == nullptr)
    {
        spiManager = new SPIManager(logger);
    }
}

// Destructor
RadioSX1262::~RadioSX1262()
{
    delete radio;
    delete module;
}

// Remember a RadioLib error code (no logging: also called from the radio task)
void RadioSX1262::recordError(const char *what, int16_t state)
{
    lastErr.what = what;
    lastErr.code = state;
}

// Configure the chip and start continuous receive
bool RadioSX1262::configure()
{
    rxRunning = false;
    // Downlink power is set per response in prepareTx(); RadioLib needs a value here
    int16_t state = radio->begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                                 LORA_SYNC_WORD, 10, LORA_PREAMBLE_LENGTH, LORA_TCXO_VOLTAGE);
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("begin", state);
        return false;
    }

    // RXEN from the MCU, TXEN from DIO2
    radio->setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
    state = radio->setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("setDio2AsRfSwitch", state);
        return false;
    }

    state = radio->setCRC(2);
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("setCRC", state);
        return false;
    }

    radio->setPacketReceivedAction(handleInterrupt);

    state = radio->startReceive();
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("startReceive", state);
        return false;
    }

    rxRunning = true;
    return true;
}

// Create the RadioLib objects (no logging)
bool RadioSX1262::create()
{
    if (radio != nullptr)
    {
        return true;
    }
    if (!spiManager->isInitialized() && !spiManager->init(true))
    {
        recordError("SPI init", 0);
        return false;
    }
    module = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, *spiManager->getSPI(),
                SPISettings(2000000, MSBFIRST, SPI_MODE0));
    radio = new SX1262(module);
    return true;
}

// Initialize LoRa module (setup(), logs the result)
bool RadioSX1262::init()
{
    logger.info("Initializing LoRa module (SX1262)...");
    initialized = create() && configure();
    if (initialized)
    {
        logger.info("LoRa module initialized and in receive mode");
    }
    else
    {
        logger.error(String("SX1262 ") + lastErr.what + " failed, RadioLib error " + String(lastErr.code));
    }
    return initialized;
}

// Reset module (radio task: no logging, the result is reported by the task)
bool RadioSX1262::reset()
{
    if (!create())
    {
        initialized = false;
        return false;
    }
    radio->clearPacketReceivedAction();
    radio->reset();
    initialized = configure();
    return initialized;
}

// DIO line still high: an RxDone that was not picked up
bool RadioSX1262::irqPinActive()
{
    return digitalRead(LORA_DIO1) == HIGH;
}

// Receive packet
bool RadioSX1262::receivePacket(uint8_t *buffer, uint8_t *length)
{
    if (!initialized)
    {
        recordError("not initialized", 0);
        return false;
    }

    size_t packetLength = radio->getPacketLength();
    int16_t state = radio->readData(buffer, packetLength);
    lastRssi = (int)radio->getRSSI();
    lastSnr = radio->getSNR();
    lastFreqError = radio->getFrequencyError();

    // Back to continuous receive; if that fails the chip is deaf until reset
    int16_t rxState = radio->startReceive();
    rxRunning = rxState == RADIOLIB_ERR_NONE;
    if (!rxRunning)
    {
        recordError("startReceive", rxState);
    }

    // No logging here (radio task): the task counts the error and loop() logs it
    if (state != RADIOLIB_ERR_NONE)
    {
        if (rxRunning) // keep "startReceive" as the more important error
        {
            recordError(state == RADIOLIB_ERR_CRC_MISMATCH ? "CRC" : "readData", state);
        }
        return false;
    }
    if (packetLength == 0 || packetLength > 255)
    {
        if (rxRunning)
        {
            recordError("length", (int16_t)packetLength);
        }
        return false;
    }

    *length = (uint8_t)packetLength;
    return true;
}

// Get RSSI of last packet
int RadioSX1262::getRSSI()
{
    return lastRssi;
}

// Get SNR of last packet
float RadioSX1262::getSNR()
{
    return lastSnr;
}

// Frequency error of the last packet in Hz
float RadioSX1262::getFrequencyError()
{
    return lastFreqError;
}

// Header received, RxDone not yet (HEADER_VALID is enabled in the RX IRQ flags)
bool RadioSX1262::receivingNow()
{
    if (!initialized)
    {
        return false;
    }
    uint32_t flags = radio->getIrqFlags();
    return (flags & RADIOLIB_SX126X_IRQ_HEADER_VALID) && !(flags & RADIOLIB_SX126X_IRQ_RX_DONE);
}

int16_t RadioSX1262::prepareTx(const uint8_t *frame, size_t len)
{
    if (!initialized)
    {
        return RADIOLIB_ERR_CHIP_NOT_FOUND;
    }
    rxRunning = false;
    // XOSC standby: the TCXO keeps running, no 5 ms restart at the launch
    int16_t state = radio->standby(RADIOLIB_SX126X_STANDBY_XOSC);
    if (state == RADIOLIB_ERR_NONE)
        state = radio->setFrequency(LORA_DOWNLINK_MHZ); // 1.5 MHz: no image recalibration
    if (state == RADIOLIB_ERR_NONE)
        state = radio->setOutputPower(LORA_DOWNLINK_POWER_DBM);
    if (state == RADIOLIB_ERR_NONE)
    {
        RadioModeConfig_t cfg;
        cfg.transmit.data = frame;
        cfg.transmit.len = len;
        cfg.transmit.addr = 0;
        state = radio->stageMode(RADIOLIB_RADIO_MODE_TX, &cfg);
    }
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("prepareTx", state);
    }
    return state;
}

int16_t RadioSX1262::launchTx()
{
    int16_t state = radio->launchMode();
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("launchTx", state);
    }
    return state;
}

void RadioSX1262::finishTx()
{
    radio->finishTransmit();
    radio->setFrequency(LORA_FREQUENCY_MHZ);
    int16_t state = radio->startReceive();
    rxRunning = state == RADIOLIB_ERR_NONE;
    if (!rxRunning)
    {
        recordError("startReceive", state);
    }
}

uint32_t RadioSX1262::timeOnAirUs(size_t len)
{
    return (uint32_t)radio->getTimeOnAir(len);
}

// Check if module is connected (LoRa sync word register reads back 0x1424)
bool RadioSX1262::isConnected()
{
    if (!initialized)
    {
        return false;
    }
    uint8_t sync[2] = {0, 0};
    module->SPIreadRegisterBurst(RADIOLIB_SX126X_REG_LORA_SYNC_WORD_MSB, 2, sync);
    return sync[0] == 0x14 && sync[1] == 0x24;
}

#endif // RADIO_SX1262
