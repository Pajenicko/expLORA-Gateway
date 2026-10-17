/**
 * expLORA Gateway
 *
 * Decision whether an RxDone timestamp can be trusted for a timed response
 * (pure function, unit-tested natively in test/test_flash_timing)
 *
 * A flash erase/program defers the DIO interrupt until the operation ends,
 * so the interrupt's timestamp is late. The DIO interrupt samples whether a
 * flash write section was open; FlashPermit keeps [start, end] of the last
 * completed outermost section. The timestamp is unreliable when:
 *   - a section was open when the interrupt ran, or
 *   - it falls inside the last section, up to marginUs after its end
 *     (deferred into a gap between two flash operations of one section, or
 *     the writer on the other core closed the section only after the
 *     deferred interrupt), or
 *   - a section is open while the packet is evaluated, or
 *   - any section closed between the interrupt and the evaluation.
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

namespace FlashTiming
{
constexpr int64_t MARGIN_US = 2000; // after the end of the last write section

// sectionStartUs/sectionEndUs: last completed outermost write section
// (start > end while the first section is still open; INT64_MIN/2 = none).
// closedAtIsr / closedNow: count of closed write sections sampled by the
// interrupt and at evaluation - a change means a section closed after the
// interrupt (e.g. a short section that hides the one that deferred it).
inline bool rxTimeReliable(int64_t rxUs, bool activeAtIsr, uint32_t closedAtIsr, int64_t sectionStartUs,
                           int64_t sectionEndUs, bool activeNow, uint32_t closedNow, int64_t marginUs = MARGIN_US)
{
    if (activeAtIsr || activeNow || closedAtIsr != closedNow)
    {
        return false;
    }
    return !(sectionStartUs <= rxUs && rxUs <= sectionEndUs + marginUs);
}
} // namespace FlashTiming
