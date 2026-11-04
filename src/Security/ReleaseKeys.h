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
 * Slot 0 holds the production release key (key id 76091636). The key pair
 * is created by the owner only (tools/release/explora_keygen.py, the
 * private key stays on the signing machine only, never in a
 * repository); the tool prints the slot to paste here. An image signed with
 * any other key verifies as SigVerify::Result::NoKey, which callers must
 * treat as "unsigned - not verified", never as verified. Test keys live
 * under test/ only.
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

// [0] current release key, [1] next key (rotation; empty until a key
// rotation is prepared)
constexpr Key KEYS[SLOTS] = {
    // production release key (owner, 2026-10-25), key id 76091636
    {0x76091636u,
        {
         0x04, 0x3B, 0xE8, 0xFB, 0x13, 0x19, 0x5B, 0x1F, 0x03, 0xFA, 0x20, 0x1E, 0x14,
         0x87, 0x7E, 0x49, 0x36, 0xF1, 0x2A, 0x73, 0xAE, 0x9C, 0x60, 0x1B, 0x5B, 0x21,
         0xC6, 0x10, 0xF0, 0x97, 0x02, 0x39, 0x1B, 0xD3, 0x42, 0x0C, 0x73, 0x9C, 0x51,
         0x75, 0x1A, 0xF5, 0x7B, 0x5B, 0xB2, 0x40, 0xA9, 0x2C, 0x11, 0x81, 0x41, 0xC3,
         0xEE, 0x15, 0x03, 0xD3, 0x0B, 0x2A, 0x19, 0xC3, 0x43, 0x54, 0x94, 0x67, 0x37,
        }},
    {0, {}},
};
} // namespace ReleaseKeys
