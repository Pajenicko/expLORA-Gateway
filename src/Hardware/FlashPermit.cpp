/**
 * expLORA Gateway
 *
 * Flash write permit
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "FlashPermit.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace FlashPermit
{

volatile uint32_t flashWriteSections = 0;
volatile uint32_t flashSectionsClosed = 0;

static StaticSemaphore_t permitBuffer;
static SemaphoreHandle_t permit = nullptr;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED; // everything below
static Logger *log_ = nullptr;
static uint32_t waits = 0;
static uint32_t timeouts = 0;
static int64_t sectionStartUs = INT64_MIN / 2; // last outermost section
static int64_t sectionEndUs = INT64_MIN / 2;

// Tasks currently writing without the permit (after a timeout) and their
// nesting depth: nested guards must not wait another WRITER_TIMEOUT_MS
struct Unpermitted
{
    TaskHandle_t task;
    uint32_t depth;
};
static Unpermitted unpermitted[4] = {};

// Created on first use: writers run before setup() finishes (file system
// recovery, settings load)
static SemaphoreHandle_t handle()
{
    if (permit == nullptr)
    {
        portENTER_CRITICAL(&lock);
        if (permit == nullptr)
        {
            permit = xSemaphoreCreateRecursiveMutexStatic(&permitBuffer);
        }
        portEXIT_CRITICAL(&lock);
    }
    return permit;
}

void setLogger(Logger *logger)
{
    log_ = logger;
}

bool tryHoldForRadio()
{
    if (xSemaphoreTakeRecursive(handle(), 0) != pdTRUE)
    {
        return false;
    }
    if (flashWriteSections != 0) // a timed-out writer writes without the mutex
    {
        xSemaphoreGiveRecursive(handle());
        return false;
    }
    return true;
}

void releaseFromRadio()
{
    xSemaphoreGiveRecursive(handle());
}

bool timestampReliable(int64_t rxTimeUs, bool writeActiveAtIsr, uint32_t closedAtIsr)
{
    portENTER_CRITICAL(&lock);
    bool activeNow = flashWriteSections > 0;
    uint32_t closedNow = flashSectionsClosed;
    int64_t start = sectionStartUs;
    int64_t end = sectionEndUs;
    portEXIT_CRITICAL(&lock);
    return FlashTiming::rxTimeReliable(rxTimeUs, writeActiveAtIsr, closedAtIsr, start, end, activeNow, closedNow);
}

// Caller holds lock
static void openSection()
{
    uint32_t n = flashWriteSections;
    flashWriteSections = n + 1;
    if (n == 0)
    {
        sectionStartUs = esp_timer_get_time();
    }
}

static void closeSection()
{
    uint32_t n = flashWriteSections;
    if (n == 0)
    {
        return;
    }
    flashWriteSections = n - 1;
    flashSectionsClosed = flashSectionsClosed + 1;
    if (n == 1)
    {
        sectionEndUs = esp_timer_get_time();
    }
}

// Caller holds lock: entry of this task in the unpermitted table, or nullptr
static Unpermitted *findUnpermitted(TaskHandle_t self)
{
    for (Unpermitted &u : unpermitted)
    {
        if (u.task == self && u.depth > 0)
        {
            return &u;
        }
    }
    return nullptr;
}

bool acquireForWrite(uint32_t timeoutMs)
{
    SemaphoreHandle_t h = handle();
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    // Already writing without the permit in this task: no second wait
    portENTER_CRITICAL(&lock);
    Unpermitted *u = findUnpermitted(self);
    if (u != nullptr)
    {
        u->depth++;
        openSection();
        portEXIT_CRITICAL(&lock);
        return false;
    }
    portEXIT_CRITICAL(&lock);

    bool got = xSemaphoreTakeRecursive(h, 0) == pdTRUE;
    portENTER_CRITICAL(&lock);
    // Open the section before any wait: from here tryHoldForRadio() fails, so
    // there is no gap between a timeout decision and the write in which the
    // radio could take a hold
    openSection();
    if (!got)
    {
        waits++;
    }
    portEXIT_CRITICAL(&lock);
    if (!got)
    {
        got = xSemaphoreTakeRecursive(h, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }

    portENTER_CRITICAL(&lock);
    if (!got)
    {
        timeouts++;
        for (Unpermitted &slot : unpermitted)
        {
            if (slot.depth == 0)
            {
                slot.task = self;
                slot.depth = 1;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&lock);

    if (!got && log_)
    {
        log_->warning("Flash write permit not granted within " + String(timeoutMs) + " ms - writing anyway");
    }
    return got;
}

void releaseAfterWrite(bool permitHeld)
{
    portENTER_CRITICAL(&lock);
    closeSection();
    if (!permitHeld)
    {
        Unpermitted *u = findUnpermitted(xTaskGetCurrentTaskHandle());
        if (u != nullptr)
        {
            u->depth--;
        }
    }
    portEXIT_CRITICAL(&lock);
    if (permitHeld)
    {
        xSemaphoreGiveRecursive(handle());
    }
}

uint32_t writerWaits()
{
    portENTER_CRITICAL(&lock);
    uint32_t v = waits;
    portEXIT_CRITICAL(&lock);
    return v;
}

uint32_t writerTimeouts()
{
    portENTER_CRITICAL(&lock);
    uint32_t v = timeouts;
    portEXIT_CRITICAL(&lock);
    return v;
}

} // namespace FlashPermit
