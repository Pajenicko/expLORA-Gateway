/**
 * expLORA Gateway
 *
 * SX1276 (RFM95W) LoRa radio driver implementation file
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

#include "Radio_SX1276.h"

#if defined(RADIO_SX1276)

// Constructor
RadioSX1276::RadioSX1276(Logger &log, SPIManager *spiMgr)
    : logger(log), spiManager(spiMgr), module(nullptr), radio(nullptr), initialized(false), lastRssi(0), lastSnr(0), lastFreqError(0)
{
    if (spiManager == nullptr)
    {
        spiManager = new SPIManager(logger);
    }
}

// Destructor
RadioSX1276::~RadioSX1276()
{
    delete radio;
    delete module;
}

// Remember a RadioLib error code (no logging: also called from the radio task)
void RadioSX1276::recordError(const char *what, int16_t state)
{
    lastErr.what = what;
    lastErr.code = state;
}

// Configure the chip and start continuous receive
bool RadioSX1276::configure()
{
    rxRunning = false;
    // Downlink power is set per response in prepareTx(); gain 0 = AGC
    int16_t state = radio->begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                                 LORA_SYNC_WORD, 10, LORA_PREAMBLE_LENGTH, 0);
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("begin", state);
        return false;
    }

    state = radio->setCRC(true);
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("setCRC", state);
        return false;
    }

    // Downlink on PA_BOOST: the default 100 mA over-current limit is too low
    state = radio->setCurrentLimit(140);
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("setCurrentLimit", state);
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
bool RadioSX1276::create()
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
    module = new Module(LORA_CS, LORA_DIO0, LORA_RST, RADIOLIB_NC, *spiManager->getSPI(),
                SPISettings(2000000, MSBFIRST, SPI_MODE0));
    radio = new SX1276(module);
    return true;
}

// Initialize LoRa module (setup(), logs the result)
bool RadioSX1276::init()
{
    logger.info("Initializing LoRa module (SX1276)...");
    initialized = create() && configure();
    if (initialized)
    {
        logger.info("LoRa module initialized and in receive mode");
    }
    else
    {
        logger.error(String("SX1276 ") + lastErr.what + " failed, RadioLib error " + String(lastErr.code));
    }
    return initialized;
}

// Reset module (radio task: no logging, the result is reported by the task)
bool RadioSX1276::reset()
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
bool RadioSX1276::irqPinActive()
{
    return digitalRead(LORA_DIO0) == HIGH;
}

// Receive packet
bool RadioSX1276::receivePacket(uint8_t *buffer, uint8_t *length)
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

// Get RSSI of last packet (RadioLib applies the HF port offset and the SNR
// correction below the noise floor, SX1276 datasheet 5.5.5)
int RadioSX1276::getRSSI()
{
    return lastRssi;
}

// Get SNR of last packet
float RadioSX1276::getSNR()
{
    return lastSnr;
}

// Frequency error of the last packet in Hz
float RadioSX1276::getFrequencyError()
{
    return lastFreqError;
}

// Modem status: signal synchronized or header info valid = receiving
bool RadioSX1276::receivingNow()
{
    if (!initialized)
    {
        return false;
    }
    // RegModemStat: bit 0 signal detected (from the preamble on), bit 1
    // signal synchronized, bit 3 header info valid - live, not latched
    return (module->SPIreadRegister(RADIOLIB_SX127X_REG_MODEM_STAT) & 0x0B) != 0;
}

int16_t RadioSX1276::prepareTx(const uint8_t *frame, size_t len)
{
    if (!initialized)
    {
        return RADIOLIB_ERR_CHIP_NOT_FOUND;
    }
    rxRunning = false;
    int16_t state = radio->standby();
    if (state == RADIOLIB_ERR_NONE)
        state = radio->setFrequency(LORA_DOWNLINK_MHZ);
    if (state == RADIOLIB_ERR_NONE)
        state = radio->setOutputPower(LORA_DOWNLINK_POWER_DBM > 17 ? 17 : LORA_DOWNLINK_POWER_DBM);
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

int16_t RadioSX1276::launchTx()
{
    int16_t state = radio->launchMode();
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("launchTx", state);
    }
    return state;
}

void RadioSX1276::finishTx()
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

bool RadioSX1276::standby()
{
    rxRunning = false;
    if (!initialized)
    {
        return false;
    }
    int16_t state = radio->standby();
    if (state != RADIOLIB_ERR_NONE)
    {
        recordError("standby", state);
        return false;
    }
    return true;
}

uint32_t RadioSX1276::timeOnAirUs(size_t len)
{
    return (uint32_t)radio->getTimeOnAir(len);
}

// Check if module is connected (silicon version register reads 0x12)
bool RadioSX1276::isConnected()
{
    if (!initialized)
    {
        return false;
    }
    return module->SPIreadRegister(RADIOLIB_SX127X_REG_VERSION) == 0x12;
}

#endif // RADIO_SX1276
