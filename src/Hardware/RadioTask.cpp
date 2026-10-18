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
#include "../Protocol/Gen2Downlink.h"

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

// Timed responses
constexpr UBaseType_t TX_REQUEST_QUEUE_LENGTH = 2;
constexpr UBaseType_t TX_RESULT_QUEUE_LENGTH = 4;
static StaticQueue_t requestQueueBuffer, resultQueueBuffer;
static uint8_t requestQueueStorage[TX_REQUEST_QUEUE_LENGTH * sizeof(TxRequest)];
static uint8_t resultQueueStorage[TX_RESULT_QUEUE_LENGTH * sizeof(TxResult)];
static QueueHandle_t requestQueue = nullptr;
static QueueHandle_t resultQueue = nullptr;
static TxRequest requestIn; // task-private

// The one response in progress (task-private)
static struct
{
    bool active;
    uint32_t slot;
    int64_t rxUs;
    bool holding;     // flash permit held (RxDone -> RX1 launch, T2 - 20 ms -> RX2 launch)
    bool rx1Decided;  // RX1 sent or given up
    bool haveRequest; // response frames from loop()
    TxOutcome reason; // why RX1 was not used
    TxRequest request;
} pending = {};
static uint32_t nextSlot = 1;
static Gen2Downlink::Budget budget; // task-private
static uint32_t lastDutyPublishMs = 0;

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

static bool isGen2Marker(const uint8_t *data, uint8_t length)
{
    return length > 0 && (data[0] & 0xE0) == 0xA0;
}

static void releaseHold()
{
    if (pending.holding)
    {
        pending.holding = false;
        FlashPermit::releaseFromRadio();
    }
}

static void finishPending(TxOutcome outcome, uint16_t airtimeMs)
{
    releaseHold();
    if (pending.haveRequest)
    {
        TxResult r = {pending.slot, pending.request.devId, outcome, pending.request.join, airtimeMs};
        xQueueSend(resultQueue, &r, 0); // loop() stalled: the outcome is only counted
    }
    portENTER_CRITICAL(&statsLock);
    switch (outcome)
    {
    case TxOutcome::Rx1:
        counters.txRx1++;
        break;
    case TxOutcome::Rx2:
        counters.txRx2++;
        break;
    case TxOutcome::Busy:
        counters.txBusy++;
        break;
    case TxOutcome::Budget:
    case TxOutcome::JoinBudget:
        counters.txBudget++;
        break;
    case TxOutcome::Pacing:
        counters.txPacing++;
        break;
    case TxOutcome::NoHold:
        counters.txNoHold++;
        break;
    case TxOutcome::Late:
        counters.txLate++;
        break;
    case TxOutcome::Failed:
        counters.txFailed++;
        break;
    case TxOutcome::LateRequest:
        counters.txLateRequest++;
        break;
    }
    portEXIT_CRITICAL(&statsLock);
    pending.active = false;
    pending.haveRequest = false;
}

// A response slot for a generation 2 frame: only with a reliable RxDone time,
// no other response pending and the flash permit held from now on
static uint32_t claimSlot(const RadioPacket &p)
{
    if (!p.timeReliable || !isGen2Marker(p.data, p.length) || !radioOk)
    {
        return 0;
    }
    if (pending.active)
    {
        COUNT(slotsBusy);
        return 0;
    }
    if (!FlashPermit::tryHoldForRadio())
    {
        COUNT(holdFails);
        return 0;
    }
    pending = {};
    pending.active = true;
    pending.holding = true;
    pending.rxUs = p.rxTimeUs;
    pending.slot = nextSlot++;
    if (nextSlot == 0)
    {
        nextSlot = 1;
    }
    return pending.slot;
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

    rxPacket.responseSlot = claimSlot(rxPacket);
    bool queued = xQueueSend(queue, &rxPacket, 0) == pdTRUE;
    if (!queued && rxPacket.responseSlot != 0)
    {
        releaseHold(); // nobody will answer this slot
        pending.active = false;
    }
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

// Wait for TxDone (DIO line) until the deadline
static bool waitTxDone(int64_t deadlineUs)
{
    for (;;)
    {
        if (radio->irqPinActive())
        {
            return true;
        }
        int64_t left = deadlineUs - esp_timer_get_time();
        if (left <= 0)
        {
            return false;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left / 1000 + 1));
    }
}

// Stage and launch one frame at launchUs. The hold (if any) is released
// right after the launch: from then on the chip transmits on its own.
static TxOutcome transmit(int64_t launchUs, const uint8_t *frame, uint8_t len, bool rx2, uint16_t &airtimeMs)
{
    airtimeMs = 0;
    // A packet that just completed is read before the radio leaves receive
    if (radio->irqPinActive())
    {
        readNotified();
    }
    if (!pending.request.priority && radio->receivingNow())
    {
        return TxOutcome::Busy;
    }
    // Already hopelessly late (task held off): do not stop the reception for nothing
    if (esp_timer_get_time() - launchUs > Gen2Downlink::MAX_LAUNCH_LATE_US)
    {
        return TxOutcome::Late;
    }
    uint32_t airUs = radio->timeOnAirUs(len);
    uint16_t airMs = (uint16_t)((airUs + 999) / 1000);
    uint32_t launchMs = millis() + (uint32_t)((launchUs - esp_timer_get_time()) / 1000);
    switch (budget.check(launchMs, airMs, pending.request.join))
    {
    case Gen2Downlink::Verdict::Ok:
        break;
    case Gen2Downlink::Verdict::Budget:
        return TxOutcome::Budget;
    case Gen2Downlink::Verdict::JoinBudget:
        return TxOutcome::JoinBudget;
    case Gen2Downlink::Verdict::Pacing:
        return TxOutcome::Pacing;
    }
    if (radio->prepareTx(frame, len) != 0)
    {
        recordError();
        radio->finishTx();
        return TxOutcome::Failed;
    }
    int64_t left = launchUs - esp_timer_get_time();
    if (left > 3000)
    {
        vTaskDelay(pdMS_TO_TICKS((left - 2000) / 1000));
    }
    while (esp_timer_get_time() < launchUs)
    {
    }
    if (esp_timer_get_time() - launchUs > Gen2Downlink::MAX_LAUNCH_LATE_US)
    {
        radio->finishTx(); // flash stall or preemption: the sensor's window is gone
        return TxOutcome::Late;
    }
    ulTaskNotifyTake(pdTRUE, 0); // the TxDone notification must not be an old one
    if (radio->launchTx() != 0)
    {
        recordError();
        radio->finishTx();
        return TxOutcome::Failed;
    }
    int64_t startUs = esp_timer_get_time();
    releaseHold();
    if (!waitTxDone(startUs + airUs + 100000))
    {
        COUNT(txDoneTimeouts);
    }
    radio->finishTx();
    if (!radio->receiving())
    {
        recordError();
    }
    budget.record(millis(), airMs, pending.request.join);
    airtimeMs = airMs;
    return rx2 ? TxOutcome::Rx2 : TxOutcome::Rx1;
}

// Requests from loop(): attach to the pending slot or report them late
static void takeRequests()
{
    while (xQueueReceive(requestQueue, &requestIn, 0) == pdTRUE)
    {
        bool cancel = requestIn.len1 == 0 && requestIn.len2 == 0;
        if (pending.active && requestIn.slot == pending.slot && !pending.haveRequest)
        {
            if (cancel)
            {
                releaseHold();
                pending.active = false;
                continue;
            }
            pending.request = requestIn;
            pending.haveRequest = true;
            continue;
        }
        if (!cancel)
        {
            TxResult r = {requestIn.slot, requestIn.devId, TxOutcome::LateRequest, requestIn.join, 0};
            xQueueSend(resultQueue, &r, 0);
            COUNT(txLateRequest);
        }
    }
}

// Drive the pending response; returns the ms until it needs the task again
static uint32_t runPending()
{
    if (!pending.active)
    {
        return UINT32_MAX;
    }
    const int64_t offset = radio->chipOffsetUs();
    const int64_t t1 = Gen2Downlink::launchTime(pending.rxUs, Gen2Downlink::Window::Rx1, offset);
    const int64_t t2 = Gen2Downlink::launchTime(pending.rxUs, Gen2Downlink::Window::Rx2, offset);
    const int64_t d1 = Gen2Downlink::decisionTime(pending.rxUs, Gen2Downlink::Window::Rx1, offset);
    const int64_t d2 = Gen2Downlink::decisionTime(pending.rxUs, Gen2Downlink::Window::Rx2, offset);
    int64_t now = esp_timer_get_time();
    if (!pending.rx1Decided)
    {
        if (now < d1)
        {
            return (uint32_t)((d1 - now + 999) / 1000);
        }
        pending.rx1Decided = true;
        pending.reason = TxOutcome::LateRequest;
#ifdef GEN2_TEST_SKIP_RX1
        // Bench test of the RX2 path (build flag only, never in a release)
        if (pending.haveRequest)
        {
            pending.reason = TxOutcome::Busy;
        }
        else
#endif
        if (pending.haveRequest && pending.request.len1 > 0)
        {
            uint16_t air = 0;
            TxOutcome o = transmit(t1, pending.request.frame1, pending.request.len1, false, air);
            if (o == TxOutcome::Rx1 || o == TxOutcome::Budget || o == TxOutcome::JoinBudget)
            {
                finishPending(o, air); // sent, or RX2 one second later would not pass either
                return UINT32_MAX;
            }
            pending.reason = o;
        }
        releaseHold(); // the RX1 hold ends at the RX1 launch
        now = esp_timer_get_time();
    }
    if (now < d2)
    {
        return (uint32_t)((d2 - now + 999) / 1000);
    }
    if (!pending.haveRequest)
    {
        // Nothing from loop() yet: a request that comes later is reported
        // late in takeRequests(), a cancel is simply dropped
        releaseHold();
        pending.active = false;
        return UINT32_MAX;
    }
    if (pending.request.len2 == 0)
    {
        finishPending(pending.reason, 0);
        return UINT32_MAX;
    }
    if (!FlashPermit::tryHoldForRadio())
    {
        finishPending(TxOutcome::NoHold, 0);
        return UINT32_MAX;
    }
    pending.holding = true;
    uint16_t air = 0;
    TxOutcome o = transmit(t2, pending.request.frame2, pending.request.len2, true, air);
    finishPending(o, air);
    return UINT32_MAX;
}

static void publishDuty()
{
    uint32_t nowMs = millis();
    if (nowMs - lastDutyPublishMs < 1000)
    {
        return;
    }
    lastDutyPublishMs = nowMs;
    uint32_t used = budget.usedMs(nowMs);
    uint32_t joins = budget.joinUsedMs(nowMs);
    portENTER_CRITICAL(&statsLock);
    counters.dutyUsedMs = used;
    counters.joinUsedMs = joins;
    portEXIT_CRITICAL(&statsLock);
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
        takeRequests();
        uint32_t waitMs = runPending();
        if (waitMs > 1000)
        {
            waitMs = 1000;
        }
        publishDuty();
        if (waitMs == 0)
        {
            continue; // a deadline is due (runPending acted on it)
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs)) > 0)
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
    requestQueue = xQueueCreateStatic(TX_REQUEST_QUEUE_LENGTH, sizeof(TxRequest), requestQueueStorage,
                                      &requestQueueBuffer);
    resultQueue =
        xQueueCreateStatic(TX_RESULT_QUEUE_LENGTH, sizeof(TxResult), resultQueueStorage, &resultQueueBuffer);
    task = xTaskCreateStaticPinnedToCore(taskMain, "radio", RADIO_TASK_STACK, nullptr, RADIO_TASK_PRIORITY, taskStack,
                                         &taskBuffer, RADIO_TASK_CORE);
    // From now on the DIO interrupt wakes the task (a packet that arrived
    // during setup is handed over by setNotifyTask)
    LoRaRadio::setNotifyTask(task);
    return queue != nullptr && task != nullptr;
}

bool receive(RadioPacket &packet)
{
    static int64_t loopStartUs = 0;
    if (loopStartUs == 0)
    {
        loopStartUs = esp_timer_get_time();
    }
    if (queue == nullptr || xQueueReceive(queue, &packet, 0) != pdTRUE)
    {
        return false;
    }
    uint32_t latencyMs = (uint32_t)((esp_timer_get_time() - packet.rxTimeUs) / 1000);
    portENTER_CRITICAL(&statsLock);
    if (packet.rxTimeUs >= loopStartUs && latencyMs > counters.maxLatencyMs)
    {
        counters.maxLatencyMs = latencyMs;
    }
    portEXIT_CRITICAL(&statsLock);
    return true;
}

bool respond(const TxRequest &request)
{
    if (requestQueue == nullptr || request.len1 > RADIO_TX_MAX || request.len2 > RADIO_TX_MAX)
    {
        return false;
    }
    bool ok = xQueueSend(requestQueue, &request, 0) == pdTRUE;
    xTaskNotifyGive(task);
    return ok;
}

void cancel(uint32_t slot)
{
    if (requestQueue == nullptr || slot == 0)
    {
        return;
    }
    static TxRequest c; // loop() only; len1 = len2 = 0 means cancel
    c.slot = slot;
    c.len1 = 0;
    c.len2 = 0;
    xQueueSend(requestQueue, &c, 0);
    xTaskNotifyGive(task);
}

bool takeResult(TxResult &result)
{
    return resultQueue != nullptr && xQueueReceive(resultQueue, &result, 0) == pdTRUE;
}

const char *txOutcomeName(TxOutcome outcome)
{
    switch (outcome)
    {
    case TxOutcome::Rx1:
        return "RX1";
    case TxOutcome::Rx2:
        return "RX2";
    case TxOutcome::LateRequest:
        return "response too late";
    case TxOutcome::Busy:
        return "reception in progress";
    case TxOutcome::Budget:
        return "duty cycle exhausted";
    case TxOutcome::JoinBudget:
        return "JOIN_ACC duty share exhausted";
    case TxOutcome::Pacing:
        return "duty cycle pacing";
    case TxOutcome::NoHold:
        return "flash write in progress";
    case TxOutcome::Late:
        return "launch late";
    case TxOutcome::Failed:
        return "radio error";
    }
    return "?";
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
    auto err = [&now]() { return String(now.lastError.what) + " (RadioLib " + String(now.lastError.code) + ")"; };
    // Duty cycle warning above 80 % of the hourly downlink budget (once per crossing)
    static bool dutyWarned = false;
    bool dutyHigh = (uint64_t)now.dutyUsedMs * 100 >= (uint64_t)Gen2Downlink::BUDGET_MS_PER_HOUR * Gen2Downlink::WARN_PERCENT;
    if (dutyHigh != dutyWarned)
    {
        dutyWarned = dutyHigh;
        if (dutyHigh)
        {
            logger.warning("LoRa downlink duty cycle above 80 %: " + String(now.dutyUsedMs / 1000) +
                           " s of 360 s in the last hour - only JOIN_ACC once the data share is used up");
        }
    }
    if (now.crcErrors == last.crcErrors && now.readErrors == last.readErrors && now.queueDrops == last.queueDrops &&
        now.resetsOk == last.resetsOk && now.resetsFailed == last.resetsFailed && now.selfHeals == last.selfHeals &&
        now.txFailed == last.txFailed && now.txDoneTimeouts == last.txDoneTimeouts)
    {
        return; // nothing new (called on every loop() pass: no String work)
    }
    if (now.txFailed != last.txFailed)
    {
        logger.warning("LoRa downlink failed: " + err() + " (" + String(now.txFailed - last.txFailed) + "x)");
    }
    if (now.txDoneTimeouts != last.txDoneTimeouts)
    {
        logger.warning("LoRa downlink: no TxDone (" + String(now.txDoneTimeouts - last.txDoneTimeouts) + "x)");
    }
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
