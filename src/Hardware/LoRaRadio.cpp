/**
 * expLORA Gateway
 *
 * LoRa radio interface implementation file
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

#include "LoRaRadio.h"
#include "board_config.h"
#include <esp_timer.h>
#include "FlashPermit.h"

#if defined(RADIO_SX1276)
#include "Radio_SX1276.h"
#elif defined(RADIO_SX1262)
#include "Radio_SX1262.h"
#else
#error "board_config.h must define RADIO_SX1276 or RADIO_SX1262"
#endif

// Shared between the DIO interrupt and the tasks, always under isrLock
static portMUX_TYPE isrLock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t notifyTask = nullptr;
static int64_t interruptTimeUs = 0;
static bool interruptDuringWrite = false; // a flash write section was open at the interrupt
static uint32_t interruptClosedSections = 0; // closed write sections at the interrupt
static bool interruptPending = false; // came before notifyTask was set

// DIO interrupt: timestamp first, then wake the radio task
void IRAM_ATTR LoRaRadio::handleInterrupt()
{
    int64_t now = esp_timer_get_time();
    bool writing = FlashPermit::flashWriteSections != 0;
    uint32_t closed = FlashPermit::flashSectionsClosed;
    portENTER_CRITICAL_ISR(&isrLock);
    interruptTimeUs = now;
    interruptDuringWrite = writing;
    interruptClosedSections = closed;
    TaskHandle_t task = notifyTask;
    if (task == nullptr)
    {
        interruptPending = true;
    }
    portEXIT_CRITICAL_ISR(&isrLock);
    if (task == nullptr)
    {
        return;
    }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    if (woken)
    {
        portYIELD_FROM_ISR();
    }
}

void LoRaRadio::setNotifyTask(TaskHandle_t task)
{
    portENTER_CRITICAL(&isrLock);
    notifyTask = task;
    bool pending = interruptPending && task != nullptr;
    if (pending)
    {
        interruptPending = false;
    }
    portEXIT_CRITICAL(&isrLock);
    if (pending)
    {
        xTaskNotifyGive(task);
    }
}

int64_t LoRaRadio::lastInterruptUs(bool *duringWrite, uint32_t *closedSections)
{
    portENTER_CRITICAL(&isrLock); // 64-bit value, no torn read
    int64_t t = interruptTimeUs;
    bool w = interruptDuringWrite;
    uint32_t c = interruptClosedSections;
    portEXIT_CRITICAL(&isrLock);
    if (duringWrite)
    {
        *duringWrite = w;
    }
    if (closedSections)
    {
        *closedSections = c;
    }
    return t;
}

// Create the radio driver selected in board_config.h
LoRaRadio *createLoRaRadio(Logger &logger, SPIManager *spiManager)
{
#if defined(RADIO_SX1276)
    return new RadioSX1276(logger, spiManager);
#else
    return new RadioSX1262(logger, spiManager);
#endif
}
