/**
 * expLORA Gateway
 *
 * Fixed number of slots, each allocated on first use
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

// Pure C++ (no Arduino): native tests in test/test_lazy_slots.
//
// The sensor tables (SensorManager) keep room for MAX_SENSORS and
// MAX_GEN2_SENSORS entries, but a gateway usually has a few sensors. A slot
// is allocated (default-constructed) when it is first needed and is kept
// until the table goes: a deleted sensor's slot is reused by the next one,
// pointers to a slot stay valid, and the heap sees no churn. With every
// slot in use the memory is the same as a plain array plus one pointer and
// one heap block header per slot.

#include <stddef.h>
#include <memory>
#include <new>

template <typename T, size_t N>
class LazySlots
{
public:
    static constexpr size_t capacity() { return N; }

    bool allocated(size_t i) const { return i < N && slots[i] != nullptr; }

    // Slot i allocated (default-constructed the first time); false when i
    // is out of range or the heap is short
    bool ensure(size_t i)
    {
        if (i >= N)
        {
            return false;
        }
        if (!slots[i])
        {
            slots[i].reset(new (std::nothrow) T());
        }
        return slots[i] != nullptr;
    }

    size_t allocatedCount() const
    {
        size_t n = 0;
        for (size_t i = 0; i < N; i++)
        {
            n += slots[i] ? 1 : 0;
        }
        return n;
    }

    // Slot i, which must be allocated (allocated() / ensure())
    T &operator[](size_t i) { return *slots[i]; }
    const T &operator[](size_t i) const { return *slots[i]; }

private:
    std::unique_ptr<T> slots[N];
};
