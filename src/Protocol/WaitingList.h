/**
 * expLORA Gateway
 *
 * Generation 2 sensors "waiting to be added" (pure, unit-tested natively in
 * test/test_gen2_gateway)
 *
 * JOIN_REQ frames of unknown DevIDs cannot be authenticated, so the list is
 * a hint for the user, built defensively:
 *  - an entry is shown only after two consistent JOIN_REQs (same HW type,
 *    firmware version and capabilities, devNonce increasing); an
 *    inconsistent frame restarts the entry,
 *  - the list is capped; a new DevID replaces the least recently seen entry
 *    that is not listed yet, a listed one only when all are listed (forged
 *    JOIN_REQs with fresh DevIDs cannot flush genuine listed sensors),
 *  - it lives in RAM only.
 * JOIN_REQ frames of unknown DevIDs are unauthenticated: a forger can still
 * list fake entries with two frames, flush genuine ones with enough
 * two-frame forgeries, or de-list a genuine entry with one frame carrying its
 * DevID and different fields. The unlisted-first eviction only raises the
 * cost; the user confirms by comparing the DevID with the label.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "ExploraProtocol.h"

struct WaitingEntry
{
    bool used = false;
    uint32_t devId = 0;
    uint16_t hwType = 0;
    uint8_t fwMajor = 0, fwMinor = 0, fwPatch = 0;
    uint8_t caps = 0;
    uint32_t lastDevNonce = 0;
    int16_t rssi = 0;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    uint16_t frames = 0; // consistent JOIN_REQs in a row

    bool listed() const { return used && frames >= 2; }
};

template <size_t N>
class WaitingList
{
public:
    // Record a structurally valid JOIN_REQ; true when the entry has just
    // become listed (second consistent frame)
    bool record(const ep::JoinRequest &req, int16_t rssi, uint32_t nowMs)
    {
        WaitingEntry *e = find(req.devId);
        if (e != nullptr)
        {
            bool consistent = e->hwType == req.hwType && e->fwMajor == req.fwMajor && e->fwMinor == req.fwMinor &&
                              e->fwPatch == req.fwPatch && e->caps == req.caps && req.devNonce > e->lastDevNonce;
            if (!consistent)
            {
                fill(*e, req, rssi, nowMs); // restart
                return false;
            }
            e->lastDevNonce = req.devNonce;
            e->rssi = rssi;
            e->lastSeenMs = nowMs;
            if (e->frames < UINT16_MAX)
            {
                e->frames++;
            }
            return e->frames == 2;
        }
        fill(*slotFor(), req, rssi, nowMs);
        return false;
    }

    void remove(uint32_t devId)
    {
        WaitingEntry *e = find(devId);
        if (e != nullptr)
        {
            *e = WaitingEntry();
        }
    }

    // Listed entries, most recently seen first; returns the count
    size_t listed(WaitingEntry *out, size_t max) const
    {
        size_t n = 0;
        for (const WaitingEntry &e : entries)
        {
            if (e.listed() && n < max)
            {
                out[n++] = e;
            }
        }
        for (size_t i = 1; i < n; i++) // insertion sort, N is small
        {
            WaitingEntry key = out[i];
            size_t j = i;
            while (j > 0 && (int32_t)(out[j - 1].lastSeenMs - key.lastSeenMs) < 0)
            {
                out[j] = out[j - 1];
                j--;
            }
            out[j] = key;
        }
        return n;
    }

    size_t capacity() const { return N; }

private:
    WaitingEntry entries[N];

    WaitingEntry *find(uint32_t devId)
    {
        for (WaitingEntry &e : entries)
        {
            if (e.used && e.devId == devId)
            {
                return &e;
            }
        }
        return nullptr;
    }

    // Free slot, else the least recently seen unlisted entry, else the least
    // recently seen listed one
    WaitingEntry *slotFor()
    {
        WaitingEntry *oldestUnlisted = nullptr, *oldest = &entries[0];
        for (WaitingEntry &e : entries)
        {
            if (!e.used)
            {
                return &e;
            }
            if (!e.listed() && (oldestUnlisted == nullptr || (int32_t)(e.lastSeenMs - oldestUnlisted->lastSeenMs) < 0))
            {
                oldestUnlisted = &e;
            }
            if ((int32_t)(e.lastSeenMs - oldest->lastSeenMs) < 0)
            {
                oldest = &e;
            }
        }
        return oldestUnlisted ? oldestUnlisted : oldest;
    }

    static void fill(WaitingEntry &e, const ep::JoinRequest &req, int16_t rssi, uint32_t nowMs)
    {
        e.used = true;
        e.devId = req.devId;
        e.hwType = req.hwType;
        e.fwMajor = req.fwMajor;
        e.fwMinor = req.fwMinor;
        e.fwPatch = req.fwPatch;
        e.caps = req.caps;
        e.lastDevNonce = req.devNonce;
        e.rssi = rssi;
        e.firstSeenMs = nowMs;
        e.lastSeenMs = nowMs;
        e.frames = 1;
    }
};
