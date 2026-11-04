/**
 * expLORA Gateway
 *
 * Generation 2 / generation 1 dispatch decision (pure, unit-tested natively
 * in test/test_gen2_gateway). docs/protocol-v2.md section 6:
 *
 *  1. The frame has the gen2 marker, protocol version 1 and a length valid
 *     for its frame type, and its DevID belongs to a configured gen2 sensor
 *     -> gen2 path (verify / REJOIN later), never the gen1 decoder.
 *  2. A structurally valid JOIN_REQ (fixed length + magic) from an unknown
 *     DevID -> "waiting to be added" list.
 *  3. Everything else -> unchanged generation 1 decoding.
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

namespace Gen2Dispatch
{
enum class Route : uint8_t
{
    Gen1,        // unchanged generation 1 path
    Gen2Known,   // configured gen2 DevID
    JoinUnknown, // JOIN_REQ of a DevID nobody added yet
};

// isKnownGen2(devId) -> bool; info is filled for the gen2 routes
template <typename KnownFn>
Route classify(const uint8_t *frame, size_t len, KnownFn isKnownGen2, ep::FrameInfo &info)
{
    if (!ep::peekFrame(frame, len, info))
    {
        return Route::Gen1;
    }
    if (isKnownGen2(info.devId))
    {
        return Route::Gen2Known;
    }
    if (info.type == ep::FrameType::JoinReq)
    {
        return Route::JoinUnknown;
    }
    return Route::Gen1;
}
} // namespace Gen2Dispatch
