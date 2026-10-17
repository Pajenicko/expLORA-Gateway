/**
 * expLORA protocol generation 2 - frame counters and devNonce
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ep_counter.h"

namespace ep
{

bool expandFcnt(bool hasLast, uint32_t last, uint16_t fcnt16, uint32_t &fcnt32)
{
    if (!hasLast)
    {
        if (fcnt16 >= kMaxFcntGap)
        {
            return false;
        }
        fcnt32 = fcnt16;
        return true;
    }
    uint64_t candidate = ((uint64_t)last & 0xFFFF0000u) | fcnt16;
    if (candidate <= last)
    {
        candidate += 0x10000;
    }
    if (candidate > 0xFFFFFFFFu || candidate - last > kMaxFcntGap)
    {
        return false;
    }
    fcnt32 = (uint32_t)candidate;
    return true;
}

} // namespace ep
