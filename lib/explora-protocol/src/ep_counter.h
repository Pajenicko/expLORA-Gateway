/**
 * expLORA protocol generation 2 - frame counters and devNonce
 *
 * Frames carry the low 16 bits of a 32-bit counter (separate counters per
 * direction, both start at 0 with every session). The receiver expands it
 * against the last accepted value and accepts only strictly increasing
 * counters within kMaxFcntGap. The counter is updated only after the MIC
 * verified. devNonce is a 32-bit counter stored in the sensor's config page;
 * the gateway accepts a JOIN_REQ only with devNonce greater than the last one
 * it accepted for that DevID.
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

namespace ep
{

constexpr uint32_t kMaxFcntGap = 16384;

// hasLast = false: first frame of a session, valid for fcnt16 < kMaxFcntGap.
// Otherwise the smallest value > last whose low 16 bits are fcnt16, if it is
// at most kMaxFcntGap ahead and does not wrap the 32-bit counter.
bool expandFcnt(bool hasLast, uint32_t last, uint16_t fcnt16, uint32_t &fcnt32);

inline bool devNonceAcceptable(bool hasLast, uint32_t last, uint32_t devNonce)
{
    return !hasLast || devNonce > last;
}

} // namespace ep
