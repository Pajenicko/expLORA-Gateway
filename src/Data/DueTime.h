/**
 * expLORA Gateway
 *
 * Due-time check for millis()-based deadlines: correct across the 49.7-day
 * wrap and for deadlines in the future (pure, native-tested)
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

namespace DueTime
{
// True once nowMs has reached dueMs (deadlines less than ~24.8 days apart)
inline bool reached(uint32_t nowMs, uint32_t dueMs)
{
    return (int32_t)(nowMs - dueMs) >= 0;
}
} // namespace DueTime
