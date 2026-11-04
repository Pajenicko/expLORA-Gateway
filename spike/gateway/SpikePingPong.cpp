/**
 * expLORA Gateway - protocol v2 milestone M0 timing spike (throw-away code)
 *
 * A dedicated FreeRTOS task owns the radio. On every RxDone it records the
 * esp_timer time in the DIO interrupt. A spike "ping" from the test sensor is
 * answered with a "pong" exactly RX1_DELAY_US after RxDone on 869.525 MHz:
 * at T-10 ms the radio goes to standby, is retuned and the frame is staged
 * (RadioLib stageMode), the task busy-waits on esp_timer and calls
 * launchMode() at T. Gen1 packets are passed to loop() through a queue.
 *
 * "Beacon" mode chains transmissions without a sensor: each TxDone interrupt
 * is the reference for the next frame BEACON_DELAY_US later, which gives
 * hundreds of samples per load scenario (2 s spacing keeps the 869.4-869.65
 * MHz band under its 10 % duty cycle).
 *
 * Measured per transmission (all esp_timer microseconds):
 *   err    = launchMode() return - T          (scheduling error)
 *   launch = duration of launchMode()
 *   prep   = standby + retune + stageMode
 *   notify = task wake-up - interrupt time
 *   lag    = TxDone interrupt - (launch end + time on air): interrupt
 *            latency incl. the chip's own TxDone delay
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "SpikePingPong.h"

#include <RadioLib.h>
#include <esp_timer.h>
#include "board_config.h"
#include "../../src/config.h"
#include "../../src/Data/SensorManager.h"

#if defined(RADIO_SX1262)
using Chip = SX1262;
static constexpr uint8_t GATEWAY_ID = 0; // ping target byte
#else
using Chip = SX1276;
static constexpr uint8_t GATEWAY_ID = 1;
#endif

static constexpr float DOWNLINK_MHZ = 869.525f;
static constexpr int64_t RX1_DELAY_US = 1000000;
static constexpr int64_t BEACON_DELAY_US = 2000000;
static constexpr int64_t PREPARE_US = 10000;
static constexpr int8_t TX_POWER_DBM = 10;

// Spike frames (unencrypted): ping A5 5A <target> <seq16>, pong 5A A5 <target> <seq16> <notify us 16>
static constexpr uint8_t PING_MAGIC0 = 0xA5, PING_MAGIC1 = 0x5A;
static constexpr uint8_t BEACON_TARGET = 0xFF;

struct RxItem
{
    uint8_t len;
    uint8_t data[255];
    int16_t rssi;
    float snr;
};

static Logger *spikeLogger;
static SPIManager *spikeSpi;
static Module *mod;
static Chip *radio;
static TaskHandle_t task;
static QueueHandle_t rxQueue;
static volatile int64_t isrTime;
static volatile bool beaconMode;
static volatile bool saveLoad;
static volatile uint32_t saveIntervalMs = 300;
static volatile uint32_t saveCount;

static void spikeSignalLoop();

// ---------------------------------------------------------------------------
// Statistics

struct Hist
{
    static constexpr int EDGES = 15;
    static constexpr int32_t edge[EDGES] = {-1000, -100, 0, 20, 50, 100, 200, 500, 1000,
                                           2000, 5000, 10000, 20000, 50000, 100000};
    uint32_t bins[EDGES + 1];
    uint32_t n;
    int64_t sum, min, max;

    void reset()
    {
        memset(bins, 0, sizeof(bins));
        n = 0;
        sum = 0;
        min = INT64_MAX;
        max = INT64_MIN;
    }
    void add(int64_t v)
    {
        int i = 0;
        while (i < EDGES && v >= edge[i])
        {
            i++;
        }
        bins[i]++;
        n++;
        sum += v;
        if (v < min)
            min = v;
        if (v > max)
            max = v;
    }
    void print(const char *name) const
    {
        if (n == 0)
        {
            Serial.printf("SPK hist %s n=0\n", name);
            return;
        }
        String line = String("SPK hist ") + name + " n=" + n + " min=" + (long)min + " avg=" + (long)(sum / n) +
                      " max=" + (long)max + " |";
        for (int i = 0; i <= EDGES; i++)
        {
            if (bins[i] == 0)
                continue;
            if (i == 0)
                line += String(" <") + edge[0];
            else if (i == EDGES)
                line += String(" >=") + edge[EDGES - 1];
            else
                line += String(" ") + edge[i - 1] + ".." + edge[i];
            line += String(":") + bins[i];
        }
        Serial.println(line);
    }
};
constexpr int32_t Hist::edge[Hist::EDGES];

static Hist histErr, histLaunch, histPrep, histNotify, histLag, histOff;
static uint32_t txCount, lateStaged, busyAtSwitch, txTimeouts, rxQueued, rxDropped, pingIgnored;
static portMUX_TYPE statsLock = portMUX_INITIALIZER_UNLOCKED;

static void resetStats()
{
    portENTER_CRITICAL(&statsLock);
    histErr.reset();
    histLaunch.reset();
    histPrep.reset();
    histNotify.reset();
    histLag.reset();
    histOff.reset();
    txCount = lateStaged = busyAtSwitch = txTimeouts = rxQueued = rxDropped = pingIgnored = 0;
    portEXIT_CRITICAL(&statsLock);
}

static void printStats()
{
    Serial.printf("SPK stats gw=%u tx=%lu lateStaged=%lu busyAtSwitch=%lu txTimeout=%lu rxQueued=%lu rxDropped=%lu "
                  "pingIgnored=%lu saves=%lu prio=%u heap=%lu\n",
                  GATEWAY_ID, (unsigned long)txCount, (unsigned long)lateStaged, (unsigned long)busyAtSwitch,
                  (unsigned long)txTimeouts, (unsigned long)rxQueued, (unsigned long)rxDropped,
                  (unsigned long)pingIgnored, (unsigned long)saveCount, (unsigned)uxTaskPriorityGet(task),
                  (unsigned long)ESP.getFreeHeap());
    histErr.print("err");
    histLaunch.print("launch");
    histPrep.print("prep");
    histNotify.print("notify");
    histLag.print("lag");
    histOff.print("offchan");
}

// ---------------------------------------------------------------------------
// Radio

static void IRAM_ATTR spikeIsr()
{
    isrTime = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    if (task)
    {
        vTaskNotifyGiveFromISR(task, &woken);
    }
    if (woken)
    {
        portYIELD_FROM_ISR();
    }
}

static int16_t startRx()
{
#if defined(RADIO_SX1262)
    return radio->startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF,
                               RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED),
                               RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
#else
    return radio->startReceive();
#endif
}

// A packet is being received right now (header seen, RxDone not yet)
static bool receiving()
{
#if defined(RADIO_SX1262)
    uint32_t flags = radio->getIrqFlags();
    return (flags & RADIOLIB_SX126X_IRQ_HEADER_VALID) && !(flags & RADIOLIB_SX126X_IRQ_RX_DONE);
#else
    uint8_t stat = mod->SPIreadRegister(RADIOLIB_SX127X_REG_MODEM_STAT);
    return stat & 0x0A; // signal synchronized | header info valid
#endif
}

static bool configureRadio()
{
#if defined(RADIO_SX1262)
    int16_t state = radio->begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                                 LORA_SYNC_WORD, TX_POWER_DBM, LORA_PREAMBLE_LENGTH, LORA_TCXO_VOLTAGE);
    if (state == RADIOLIB_ERR_NONE)
    {
        radio->setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
        state = radio->setDio2AsRfSwitch(true);
    }
    if (state == RADIOLIB_ERR_NONE)
        state = radio->setCRC(2);
#else
    int16_t state = radio->begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ, LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                                 LORA_SYNC_WORD, TX_POWER_DBM, LORA_PREAMBLE_LENGTH, 0);
    if (state == RADIOLIB_ERR_NONE)
        state = radio->setCRC(true);
    if (state == RADIOLIB_ERR_NONE)
        state = radio->setCurrentLimit(140);
#endif
    if (state == RADIOLIB_ERR_NONE)
    {
        radio->setPacketReceivedAction(spikeIsr);
        state = startRx();
    }
    if (state != RADIOLIB_ERR_NONE)
    {
        spikeLogger->error("SPIKE radio setup failed, RadioLib error " + String(state));
        return false;
    }
    return true;
}

// Read the packet behind the last RxDone; true + target/seq for a spike ping
static bool readPacket(uint8_t &target, uint16_t &seq)
{
    RxItem item;
    size_t len = radio->getPacketLength();
    int16_t state = radio->readData(item.data, len);
    item.rssi = (int16_t)radio->getRSSI();
    item.snr = radio->getSNR();
    startRx();
    if (state != RADIOLIB_ERR_NONE || len == 0 || len > 255)
    {
        Serial.printf("SPK rx error %d len %u\n", state, (unsigned)len);
        return false;
    }
    item.len = (uint8_t)len;
    if (len == 5 && item.data[0] == PING_MAGIC0 && item.data[1] == PING_MAGIC1)
    {
        target = item.data[2];
        seq = (item.data[3] << 8) | item.data[4];
        Serial.printf("SPK ping target=%u seq=%u rssi=%d snr=%.1f\n", target, seq, item.rssi, item.snr);
        return true;
    }
    if (xQueueSend(rxQueue, &item, 0) == pdTRUE)
    {
        rxQueued++;
        spikeSignalLoop();
    }
    else
    {
        rxDropped++;
    }
    return false;
}

// Send one frame at time T; returns the TxDone interrupt time (0 on timeout)
static int64_t transmitAt(int64_t T, uint8_t target, uint16_t seq, int64_t notifyUs)
{
    uint8_t frame[7] = {PING_MAGIC1, PING_MAGIC0, target, (uint8_t)(seq >> 8), (uint8_t)seq,
                        (uint8_t)(notifyUs > 0xFFFF ? 0xFF : notifyUs >> 8),
                        (uint8_t)(notifyUs > 0xFFFF ? 0xFF : notifyUs)};
    const size_t len = (target == BEACON_TARGET) ? 5 : sizeof(frame);

    bool busy = receiving();
    int64_t tPrep = esp_timer_get_time();
#if defined(RADIO_SX1262)
    radio->standby(RADIOLIB_SX126X_STANDBY_XOSC);
#else
    radio->standby();
#endif
    radio->setFrequency(DOWNLINK_MHZ);
    RadioModeConfig_t cfg;
    cfg.transmit.data = frame;
    cfg.transmit.len = len;
    cfg.transmit.addr = 0;
    int16_t state = radio->stageMode(RADIOLIB_RADIO_MODE_TX, &cfg);
    int64_t tStaged = esp_timer_get_time();
    ulTaskNotifyTake(pdTRUE, 0); // receive was aborted by standby

    while (esp_timer_get_time() < T)
    {
    }
    int64_t t0 = esp_timer_get_time();
    if (state == RADIOLIB_ERR_NONE)
    {
        state = radio->launchMode();
    }
    int64_t t1 = esp_timer_get_time();

    int64_t txDone = 0;
    if (state == RADIOLIB_ERR_NONE && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0)
    {
        txDone = isrTime;
    }
    radio->finishTransmit();
    radio->setFrequency(LORA_FREQUENCY_MHZ);
    startRx();
    int64_t tBack = esp_timer_get_time();

    int64_t toa = (int64_t)radio->getTimeOnAir(len);
    int64_t lag = txDone ? txDone - (t1 + toa) : 0;

    portENTER_CRITICAL(&statsLock);
    txCount++;
    if (busy)
        busyAtSwitch++;
    if (tStaged > T)
        lateStaged++;
    if (!txDone)
        txTimeouts++;
    histErr.add(t1 - T);
    histLaunch.add(t1 - t0);
    histPrep.add(tStaged - tPrep);
    if (txDone)
        histLag.add(lag);
    histOff.add(tBack - tPrep);
    portEXIT_CRITICAL(&statsLock);

    Serial.printf("SPK tx %s seq=%u state=%d err=%lld start=%lld launch=%lld prep=%lld notify=%lld lag=%lld toa=%lld "
                  "off=%lld busy=%d\n",
                  target == BEACON_TARGET ? "beacon" : "pong", seq, state, t1 - T, t0 - T, t1 - t0, tStaged - tPrep,
                  notifyUs, lag, toa, tBack - tPrep, busy);
    if (txCount % 100 == 0)
    {
        printStats();
    }
    return txDone;
}

static void taskMain(void *)
{
    bool pending = false;
    int64_t pendingT = 0, pendingNotify = 0;
    uint8_t pendingTarget = 0;
    uint16_t pendingSeq = 0, beaconSeq = 0;

    for (;;)
    {
        if (!pending && beaconMode)
        {
            pending = true;
            pendingT = esp_timer_get_time() + BEACON_DELAY_US;
            pendingTarget = BEACON_TARGET;
            pendingSeq = beaconSeq++;
            pendingNotify = 0;
        }

        TickType_t wait = pdMS_TO_TICKS(100); // idle: re-check beacon mode
        if (pending)
        {
            int64_t until = pendingT - PREPARE_US - esp_timer_get_time();
            wait = until > 2000 ? pdMS_TO_TICKS((until - 1000) / 1000) : 0;
        }
        if (wait > 0 || !pending)
        {
            uint32_t notified = ulTaskNotifyTake(pdTRUE, wait);
            int64_t woke = esp_timer_get_time();
            if (notified)
            {
                int64_t rxDone = isrTime;
                histNotify.add(woke - rxDone);
                uint8_t target;
                uint16_t seq;
                if (readPacket(target, seq) && target == GATEWAY_ID)
                {
                    if (pending)
                    {
                        pingIgnored++;
                        Serial.printf("SPK ping seq=%u ignored (transmission pending)\n", seq);
                    }
                    else
                    {
                        pending = true;
                        pendingT = rxDone + RX1_DELAY_US;
                        pendingTarget = target;
                        pendingSeq = seq;
                        pendingNotify = woke - rxDone;
                    }
                }
            }
            continue;
        }

        // A packet finished right before the switch: read it first
        if (ulTaskNotifyTake(pdTRUE, 0) > 0)
        {
            uint8_t target;
            uint16_t seq;
            if (readPacket(target, seq) && target == GATEWAY_ID)
            {
                pingIgnored++;
            }
        }

        int64_t txDone = transmitAt(pendingT, pendingTarget, pendingSeq, pendingNotify);
        pending = false;
        if (pendingTarget == BEACON_TARGET && beaconMode && txDone)
        {
            // Chain: the TxDone interrupt is the reference for the next beacon
            pending = true;
            pendingT = txDone + BEACON_DELAY_US;
            pendingSeq = beaconSeq++;
            pendingNotify = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// LoRaRadio facade for loop()

class SpikeRadio : public LoRaRadio
{
    int lastRssi = 0;
    float lastSnr = 0;

public:
    static void signal() { interruptOccurred = true; }

    bool init() override
    {
        spikeLogger->info("Initializing LoRa module (M0 timing spike, radio task)...");
        if (!spikeSpi->isInitialized() && !spikeSpi->init())
        {
            return false;
        }
#if defined(RADIO_SX1262)
        mod = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, *spikeSpi->getSPI(),
                         SPISettings(2000000, MSBFIRST, SPI_MODE0));
#else
        mod = new Module(LORA_CS, LORA_DIO0, LORA_RST, RADIOLIB_NC, *spikeSpi->getSPI(),
                         SPISettings(2000000, MSBFIRST, SPI_MODE0));
#endif
        radio = new Chip(mod);
        rxQueue = xQueueCreate(8, sizeof(RxItem));
        resetStats();
        if (!configureRadio())
        {
            return false;
        }
        xTaskCreatePinnedToCore(taskMain, "spike_radio", 6144, nullptr, 14, &task, 1);
        Serial.printf("SPK started gw=%u chip=%s\n", GATEWAY_ID, getChipName());
        return true;
    }
    bool reset() override { return true; }
    bool receivePacket(uint8_t *buffer, uint8_t *length) override
    {
        RxItem item;
        if (xQueueReceive(rxQueue, &item, 0) != pdTRUE)
        {
            return false;
        }
        memcpy(buffer, item.data, item.len);
        *length = item.len;
        lastRssi = item.rssi;
        lastSnr = item.snr;
        if (uxQueueMessagesWaiting(rxQueue) > 0)
        {
            interruptOccurred = true;
        }
        return true;
    }
    int getRSSI() override { return lastRssi; }
    float getSNR() override { return lastSnr; }
    bool isConnected() override { return true; }
    const char *getChipName() const override
    {
#if defined(RADIO_SX1262)
        return "SX1262 (spike)";
#else
        return "SX1276 (spike)";
#endif
    }
};

static void spikeSignalLoop()
{
    SpikeRadio::signal();
}

LoRaRadio *spikeCreateRadio(Logger &logger, SPIManager *spiManager)
{
    spikeLogger = &logger;
    spikeSpi = spiManager;
    return new SpikeRadio();
}

// ---------------------------------------------------------------------------
// Console and load

void spikeCommand(const String &line, SensorManager *)
{
    if (line == "spike-stats")
    {
        printStats();
    }
    else if (line.startsWith("spike-reset"))
    {
        printStats();
        resetStats();
        Serial.printf("SPK reset tag=%s\n", line.substring(11).c_str());
    }
    else if (line == "spike-beacon on" || line == "spike-beacon off")
    {
        beaconMode = line.endsWith("on");
        Serial.printf("SPK beacon %d\n", beaconMode);
    }
    else if (line.startsWith("spike-save "))
    {
        String arg = line.substring(11);
        if (arg == "off")
        {
            saveLoad = false;
        }
        else
        {
            saveLoad = true;
            if (arg.toInt() > 0)
                saveIntervalMs = arg.toInt();
        }
        Serial.printf("SPK save %d every %lu ms\n", saveLoad, (unsigned long)saveIntervalMs);
    }
    else if (line.startsWith("spike-prio "))
    {
        vTaskPrioritySet(task, line.substring(11).toInt());
        Serial.printf("SPK prio %u\n", (unsigned)uxTaskPriorityGet(task));
    }
}

void spikeLoop(SensorManager *sensors)
{
    static unsigned long last = 0;
    if (saveLoad && sensors && millis() - last >= saveIntervalMs)
    {
        last = millis();
        sensors->saveSensors(true);
        saveCount = saveCount + 1;
    }
}
