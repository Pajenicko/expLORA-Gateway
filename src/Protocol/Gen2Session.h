/**
 * expLORA Gateway
 *
 * Generation 2 sessions of one sensor (pure, unit-tested natively in
 * test/test_gen2_downlink). Sessions live in RAM only (docs/protocol-v2.md
 * section 2). After a JOIN_ACC the new session is "pending" next to the
 * current one until the first UP under the new keys verifies (spec 5.1).
 * Reception order per spec 5.3: peek (type UP) -> expand the counter of
 * the session tried -> openData(UP) -> store the counter only on success.
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

struct Gen2Session
{
    bool valid = false;
    ep::KeyPair keys = {};
    uint32_t devNonce = 0;
    bool hasUp = false;
    uint32_t fcntUp = 0;   // last accepted uplink counter
    uint32_t fcntDown = 0; // next downlink counter

    void clear()
    {
        ep::wipe(this, sizeof(*this));
    }
};

enum class SessionUse : uint8_t
{
    None,     // no session verified this frame
    Current,  // current session
    Promoted, // pending session verified: it is the current one now
};

struct Gen2SessionPair
{
    Gen2Session current;
    Gen2Session pending;

    bool any() const { return current.valid || pending.valid; }

    // New session from a JOIN_ACC; the current one stays until the first UP
    // under the new keys
    void startPending(const ep::KeyPair &keys, uint32_t devNonce)
    {
        pending.clear();
        pending.valid = true;
        pending.keys = keys;
        pending.devNonce = devNonce;
    }

    // Verify and decrypt an UP. payload must hold len - kDataOverhead bytes.
    SessionUse openUp(ep::AesEncryptFn aes, const uint8_t *frame, size_t len, uint8_t *payload, size_t &payloadLen,
                      uint32_t &fcnt32)
    {
        ep::FrameInfo info;
        if (!ep::peekFrame(frame, len, info) || info.type != ep::FrameType::Up)
        {
            return SessionUse::None;
        }
        if (tryOpen(aes, pending, frame, len, info.fcnt16, payload, payloadLen, fcnt32))
        {
            current = pending;
            pending.clear();
            return SessionUse::Promoted;
        }
        if (tryOpen(aes, current, frame, len, info.fcnt16, payload, payloadLen, fcnt32))
        {
            return SessionUse::Current;
        }
        return SessionUse::None;
    }

    void clear()
    {
        current.clear();
        pending.clear();
    }

private:
    static bool tryOpen(ep::AesEncryptFn aes, Gen2Session &s, const uint8_t *frame, size_t len, uint16_t fcnt16,
                        uint8_t *payload, size_t &payloadLen, uint32_t &fcnt32)
    {
        uint32_t expanded = 0;
        if (!s.valid || !ep::expandFcnt(s.hasUp, s.fcntUp, fcnt16, expanded))
        {
            return false;
        }
        if (!ep::openData(aes, s.keys, ep::FrameType::Up, frame, len, expanded, 0, payload, payloadLen))
        {
            return false;
        }
        s.hasUp = true; // store the counter only after the MIC verified
        s.fcntUp = expanded;
        fcnt32 = expanded;
        return true;
    }
};
