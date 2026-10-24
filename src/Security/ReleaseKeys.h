/**
 * expLORA Gateway
 *
 * ReleaseKeys - the public keys (ECDSA P-256) that release images are
 * signed with. Two slots, the current key and the next one, so a key can be
 * rotated without a flag day: new firmware carries both, later releases are
 * signed with the next key.
 *
 * keyId = the first 4 bytes of SHA-256 over the 65-byte uncompressed point
 * (04 || X || Y), read big-endian (printed as 8 hex digits it is the hash
 * prefix). test/test_sigverify checks every filled slot against its point.
 *
 * NO PRODUCTION KEY YET: both slots are empty (keyId 0). The key pair is
 * created by the owner only (tools/release/explora_keygen.py, the private
 * key stays on the signing machine only, never in a
 * repository); the tool prints the slot to paste here. Until then every
 * image verifies as SigVerify::Result::NoKey, which callers must treat as
 * "unsigned - not verified", never as verified. Test keys live under test/
 * only.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
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

#include <cstddef>
#include <cstdint>

namespace ReleaseKeys
{
constexpr size_t POINT_SIZE = 65; // 0x04 || X (32 B) || Y (32 B)

struct Key
{
    uint32_t keyId; // 0 = empty slot (never matches)
    uint8_t point[POINT_SIZE];
};

constexpr size_t SLOTS = 2;

// [0] current release key, [1] next key (rotation). Empty: no production
// key has been generated yet
constexpr Key KEYS[SLOTS] = {
    {0, {}},
    {0, {}},
};
} // namespace ReleaseKeys
