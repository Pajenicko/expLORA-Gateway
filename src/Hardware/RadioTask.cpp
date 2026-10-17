/**
 * expLORA Gateway
 *
 * Radio task
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "RadioTask.h"
#include "FlashPermit.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace RadioTask
{

// Everything static: allocated at boot, invisible to the heap fragmentation watchdog
static StaticTask_t taskBuffer;
static StackType_t taskStack[RADIO_TASK_STACK];
static StaticQueue_t queueBuffer;
static uint8_t queueStorage[RADIO_QUEUE_LENGTH * sizeof(RadioPacket)];
static QueueHandle_t queue = nullptr;
static TaskHandle_t task = nullptr;

static LoRaRadio *radio = nullptr;
static bool radioOk = false;
static Stats counters = {};
static portMUX_TYPE statsLock = portMUX_INITIALIZER_UNLOCKED;
static RadioPacket rxPacket; // task-private work buffer (keeps the stack small)

#define COUNT(field)                    \
    do                                  \
    {                                   \
        portENTER_CRITICAL(&statsLock); \
        counters.field++;               \
        portEXIT_CRITICAL(&statsLock);  \
    } while (0)

static void recordError()
{
    RadioError e = radio->lastError();
    portENTER_CRITICAL(&statsLock);
    counters.lastError = e;
    portEXIT_CRITICAL(&statsLock);
}

// No logging, no heap, no locks other than the stats spinlock and the queue
static bool readPacket(int64_t rxTimeUs, bool timeKnown, bool writeAtIsr, uint32_t closedAtIsr)
{
    uint8_t length = 0;
    if (!radio->receivePacket(rxPacket.data, &length))
    {
        recordError();
        if (strcmp(radio->lastError().what, "CRC") == 0)
        {
            COUNT(crcErrors);
        }
        else
        {
            COUNT(readErrors);
        }
        return false;
    }
    rxPacket.length = length;
    rxPacket.rssi = (int16_t)radio->getRSSI();
    rxPacket.snr = radio->getSNR();
    rxPacket.freqError = radio->getFrequencyError();
    rxPacket.rxTimeUs = rxTimeUs;
    rxPacket.timeReliable = timeKnown && FlashPermit::timestampReliable(rxTimeUs, writeAtIsr, closedAtIsr);
    if (!rxPacket.timeReliable)
    {
        COUNT(unreliableTime); // M3: no timed response to this packet
    }

    bool queued = xQueueSend(queue, &rxPacket, 0) == pdTRUE;
    UBaseType_t waiting = uxQueueMessagesWaiting(queue);
    portENTER_CRITICAL(&statsLock);
    if (queued)
    {
        counters.received++;
    }
    else
    {
        counters.queueDrops++;
    }
    if (waiting > counters.queueHigh)
    {
        counters.queueHigh = waiting;
    }
    portEXIT_CRITICAL(&statsLock);
    return true;
}

static void resetRadio()
{
    radioOk = radio->reset();
    if (radioOk)
    {
        COUNT(resetsOk);
    }
    else
    {
        COUNT(resetsFailed);
        recordError();
    }
}

static void readNotified()
{
    bool writeAtIsr = false;
    uint32_t closedAtIsr = 0;
    int64_t t = LoRaRadio::lastInterruptUs(&writeAtIsr, &closedAtIsr);
    readPacket(t, true, writeAtIsr, closedAtIsr);
}

static void taskMain(void *)
{
    uint32_t lastHealthCheck = millis();
    uint32_t failedPolls = 0;
    for (;;)
    {
        // A packet is read only while the packet-ready line is high: a
        // notification that raced with the polling below then finds the line
        // low and cannot read the same packet twice
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0)
        {
            if (radio->irqPinActive())
            {
                readNotified();
            }
        }
        else if (radioOk && radio->irqPinActive())
        {
            // An interrupt may have arrived between the timeout and here
            if (ulTaskNotifyTake(pdTRUE, 0) > 0)
            {
                readNotified();
            }
            else
            {
                // The line is high but no interrupt came (lost edge): without
                // this the radio would stay deaf until the next reset
                COUNT(selfHeals);
                if (readPacket(esp_timer_get_time(), false, false, 0))
                {
                    failedPolls = 0;
                }
                else if (++failedPolls >= 3)
                {
                    // Line stuck high, reads keep failing: re-initialize now
                    failedPolls = 0;
                    resetRadio();
                }
            }
        }

        // Receive could not be restarted after a read: deaf until reset
        if (radioOk && !radio->receiving())
        {
            resetRadio();
        }

        // A chip that stopped answering (brown-out, ESD) or failed to start
        // is re-initialized instead of silently receiving nothing
        uint32_t interval = radioOk ? RADIO_HEALTH_CHECK_MS : RADIO_RETRY_MS;
        if (millis() - lastHealthCheck > interval)
        {
            lastHealthCheck = millis();
            if (!radio->isConnected())
            {
                resetRadio();
            }
        }
    }
}

bool start(LoRaRadio *r, Logger &logger)
{
    if (task != nullptr || r == nullptr)
    {
        return false;
    }
    radio = r;
    radioOk = r->isConnected(); // still setup(): the task does not run yet
    FlashPermit::setLogger(&logger);
    queue = xQueueCreateStatic(RADIO_QUEUE_LENGTH, sizeof(RadioPacket), queueStorage, &queueBuffer);
    task = xTaskCreateStaticPinnedToCore(taskMain, "radio", RADIO_TASK_STACK, nullptr, RADIO_TASK_PRIORITY, taskStack,
                                         &taskBuffer, RADIO_TASK_CORE);
    // From now on the DIO interrupt wakes the task (a packet that arrived
    // during setup is handed over by setNotifyTask)
    LoRaRadio::setNotifyTask(task);
    return queue != nullptr && task != nullptr;
}

bool receive(RadioPacket &packet)
{
    if (queue == nullptr || xQueueReceive(queue, &packet, 0) != pdTRUE)
    {
        return false;
    }
    uint32_t latencyMs = (uint32_t)((esp_timer_get_time() - packet.rxTimeUs) / 1000);
    portENTER_CRITICAL(&statsLock);
    if (latencyMs > counters.maxLatencyMs)
    {
        counters.maxLatencyMs = latencyMs;
    }
    portEXIT_CRITICAL(&statsLock);
    return true;
}

Stats stats()
{
    portENTER_CRITICAL(&statsLock);
    Stats s = counters;
    portEXIT_CRITICAL(&statsLock);
    s.stackFree = task ? (uint32_t)uxTaskGetStackHighWaterMark(task) : 0;
    return s;
}

void logEvents(Logger &logger)
{
    static Stats last = {};
    Stats now = stats();
    if (now.crcErrors == last.crcErrors && now.readErrors == last.readErrors && now.queueDrops == last.queueDrops &&
        now.resetsOk == last.resetsOk && now.resetsFailed == last.resetsFailed && now.selfHeals == last.selfHeals)
    {
        return; // nothing new (called on every loop() pass: no String work)
    }
    auto err = [&now]() { return String(now.lastError.what) + " (RadioLib " + String(now.lastError.code) + ")"; };
    if (now.crcErrors != last.crcErrors)
    {
        logger.warning("LoRa packet CRC error, dropped (" + String(now.crcErrors - last.crcErrors) + "x)");
    }
    if (now.readErrors != last.readErrors)
    {
        logger.warning("LoRa packet read failed: " + err() + " (" + String(now.readErrors - last.readErrors) + "x)");
    }
    if (now.queueDrops != last.queueDrops)
    {
        logger.warning("LoRa receive queue full: " + String(now.queueDrops - last.queueDrops) + " packet(s) dropped");
    }
    if (now.resetsOk != last.resetsOk)
    {
        logger.warning("LoRa radio re-initialized (" + err() + ")");
    }
    if (now.resetsFailed != last.resetsFailed)
    {
        logger.error("LoRa radio re-initialization failed: " + err());
    }
    if (now.selfHeals != last.selfHeals)
    {
        logger.warning("LoRa interrupt missed, packet picked up by polling (" +
                       String(now.selfHeals - last.selfHeals) + "x)");
    }
    last = now;
}

} // namespace RadioTask
