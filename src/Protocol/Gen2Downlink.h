/**
 * expLORA Gateway
 *
 * Generation 2 downlink rules (pure, unit-tested natively in
 * test/test_gen2_downlink): window choice for a response, duty-cycle
 * accounting over a sliding hour, pacing. docs/protocol-v2.md sections 7, 9.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>

namespace Gen2Downlink
{
constexpr int64_t RX1_DELAY_US = 1000000;
constexpr int64_t RX2_DELAY_US = 2000000;
constexpr int64_t PREPARE_LEAD_US = 20000; // standby + retune + stage before the launch
constexpr int64_t MAX_LAUNCH_LATE_US = 40000; // abort a launch later than this (the sensor's window ends ~T + 64 ms)
constexpr uint32_t BUDGET_MS_PER_HOUR = 360000;     // 10 % of 869.4-869.65 MHz
constexpr uint32_t JOIN_BUDGET_MS_PER_HOUR = 72000;  // JOIN_ACC + REJOIN: 20 % of it
constexpr uint32_t DATA_BUDGET_MS_PER_HOUR = BUDGET_MS_PER_HOUR - JOIN_BUDGET_MS_PER_HOUR; // everything else: 80 %
constexpr uint32_t PACING_FACTOR = 9;               // off-time >= 9 x airtime
constexpr uint32_t WARN_PERCENT = 80;

enum class Window : uint8_t
{
    Rx1,
    Rx2,
};

// Launch times relative to the RxDone interrupt, compensated by the chip's
// fixed offset (M0: SX1262 6.4 ms, SX1276 5.9 ms)
inline int64_t launchTime(int64_t rxDoneUs, Window w, int64_t chipOffsetUs)
{
    return rxDoneUs + (w == Window::Rx1 ? RX1_DELAY_US : RX2_DELAY_US) - chipOffsetUs;
}

// The radio task decides about a window at launch - PREPARE_LEAD_US: a
// response handed over by loop() until then (the task takes requests before
// it decides) goes out in that window; a later one waits for the RX2
// decision, after the RX2 decision it is reported late.
inline int64_t decisionTime(int64_t rxDoneUs, Window w, int64_t chipOffsetUs)
{
    return launchTime(rxDoneUs, w, chipOffsetUs) - PREPARE_LEAD_US;
}

// Airtime over a sliding hour in 60 one-minute buckets
class AirtimeWindow
{
public:
    void record(uint32_t nowMs, uint32_t airtimeMs)
    {
        advance(nowMs);
        buckets[current] += airtimeMs;
    }

    uint32_t usedMs(uint32_t nowMs)
    {
        advance(nowMs);
        uint32_t sum = 0;
        for (uint32_t b : buckets)
        {
            sum += b;
        }
        return sum;
    }

private:
    uint32_t buckets[60] = {};
    uint32_t current = 0;
    uint32_t bucketStartMs = 0; // start of buckets[current]
    bool started = false;

    // Elapsed time with a signed difference: a call with an earlier time
    // (check() looks ~20 ms ahead of later calls) never clears the window,
    // and the 49.7-day millis() wrap is just another minute
    void advance(uint32_t nowMs)
    {
        if (!started)
        {
            started = true;
            bucketStartMs = nowMs - nowMs % 60000;
            return;
        }
        int32_t elapsed = (int32_t)(nowMs - bucketStartMs);
        if (elapsed < 60000)
        {
            return;
        }
        uint32_t steps = (uint32_t)elapsed / 60000;
        if (steps >= 60)
        {
            for (uint32_t &b : buckets)
            {
                b = 0;
            }
        }
        else
        {
            for (uint32_t i = 0; i < steps; i++)
            {
                current = (current + 1) % 60;
                buckets[current] = 0;
            }
        }
        bucketStartMs += steps * 60000;
    }
};

enum class Verdict : uint8_t
{
    Ok,
    Budget,     // data share (80 % of the hourly 10 %) exhausted
    JoinBudget, // JOIN_ACC / REJOIN share exhausted
    Pacing,     // off-time after the previous transmission too short
};

// Duty-cycle gate for the gateway's downlink channel
class Budget
{
public:
    Verdict check(uint32_t launchMs, uint32_t airtimeMs, bool join)
    {
        if (hasLast && (int32_t)(launchMs - (lastEndMs + PACING_FACTOR * lastAirtimeMs)) < 0)
        {
            return Verdict::Pacing;
        }
        if (join)
        {
            return joins.usedMs(launchMs) + airtimeMs > JOIN_BUDGET_MS_PER_HOUR ? Verdict::JoinBudget : Verdict::Ok;
        }
        return data.usedMs(launchMs) + airtimeMs > DATA_BUDGET_MS_PER_HOUR ? Verdict::Budget : Verdict::Ok;
    }

    void record(uint32_t endMs, uint32_t airtimeMs, bool join)
    {
        (join ? joins : data).record(endMs, airtimeMs);
        hasLast = true;
        lastEndMs = endMs;
        lastAirtimeMs = airtimeMs;
    }

    uint32_t usedMs(uint32_t nowMs) { return data.usedMs(nowMs) + joins.usedMs(nowMs); }
    uint32_t joinUsedMs(uint32_t nowMs) { return joins.usedMs(nowMs); }
    bool warn(uint32_t nowMs) { return usedMs(nowMs) * 100 >= (uint64_t)BUDGET_MS_PER_HOUR * WARN_PERCENT; }

private:
    AirtimeWindow data, joins; // the two shares never exceed 10 % together
    bool hasLast = false;
    uint32_t lastEndMs = 0;
    uint32_t lastAirtimeMs = 0;
};
} // namespace Gen2Downlink
