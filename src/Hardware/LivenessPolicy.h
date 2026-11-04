/**
 * expLORA Gateway
 *
 * LivenessPolicy - pure age tracking of a heartbeat counter (Liveness.cpp),
 * kept free of Arduino for the native tests (test/test_liveness)
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

#pragma once

#include <cstdint>

namespace LivenessPolicy
{
    // A counter that its owner advances while it is alive; sampled with the
    // current time (uint32_t milliseconds: the arithmetic wraps)
    struct Beat
    {
        bool seen = false;    // sampled at least once
        uint32_t last = 0;    // counter value at the last advance
        uint32_t since = 0;   // time of the last advance (or of the first sample)
        uint32_t maxAge = 0;  // longest time without an advance so far

        // Sample the counter; returns the time since it last advanced
        uint32_t sample(uint32_t counter, uint32_t now)
        {
            if (!seen || counter != last)
            {
                seen = true;
                last = counter;
                since = now;
                return 0;
            }
            uint32_t age = now - since;
            if (age > maxAge)
            {
                maxAge = age;
            }
            return age;
        }
    };

    // Hung: no advance for longer than the limit
    inline bool hung(uint32_t age, uint32_t limit)
    {
        return age > limit;
    }

    // The lwIP thread probe: with a probe pending (accepted by the mailbox)
    // hung once it has not run for runLimit (the thread takes nothing);
    // without one (every post refused) hung once the counter has not
    // advanced for busyLimit (a mailbox that stays full that long is not
    // merely busy)
    inline bool tcpipHung(bool pending, uint32_t pendingAge, uint32_t beatAge, uint32_t runLimit, uint32_t busyLimit)
    {
        return pending ? pendingAge > runLimit : beatAge > busyLimit;
    }
}
