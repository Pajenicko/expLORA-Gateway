/**
 * expLORA Gateway
 *
 * SigVerify - checks a parsed SigBlock: the binding to the received file
 * (kind, target, version, payload length, payload hash), the key id against
 * a key table (ReleaseKeys::KEYS by default) and the ECDSA P-256 signature
 * r || s over SHA-256(block bytes 0..55) through mbedtls. Transient heap
 * only (mbedtls big numbers, a few kB during one verify), no static buffers.
 * Built for the gateway and, against the host mbedtls 3.6.6, for the native
 * tests (test/test_sigverify).
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

#include "ReleaseKeys.h"
#include "SigBlock.h"

namespace SigVerify
{
// Error is 0 and Ok is not: a value-initialised Result never reads as Ok
enum class Result : uint8_t
{
    Error = 0,    // mbedtls failure (no memory, bad key in the table), missing hash
    Ok = 1,       // signed by a key of the table, bound to this file
    NoKey,        // the block's key id is not in the table (or the table is empty)
    BadSig,       // the signature does not verify with that key
    BadBlock,     // malformed block (magic, version, kind, reserved)
    WrongKind,    // gateway image offered as sensor image or the other way round
    WrongTarget,  // other board / other sensor HW type
    WrongVersion, // block version differs from the image's own version
    WrongLength,  // block payload length differs from the received payload
    WrongHash,    // payload changed after signing
};

const char *resultText(Result r);

// The 4-byte key id of a 65-byte uncompressed point (big-endian hash prefix)
bool keyId(const uint8_t point[ReleaseKeys::POINT_SIZE], uint32_t &out);

// Raw ECDSA P-256 check: sig = r || s over a 32-byte digest. Ok / BadSig /
// Error (point not on the curve, no memory)
Result verifyDigest(const uint8_t point[ReleaseKeys::POINT_SIZE], const uint8_t digest[32], const uint8_t sig[64]);

// The whole check of a parsed block against the file it came with, in the
// order binding -> key -> signature. Only block.raw is used: it is parsed
// again here, so a Block whose fields were changed after parse() can never
// pass with values the signature does not cover. expect.payloadHash must
// not be null (Error)
Result verify(const SigBlock::Block &block, const SigBlock::Expect &expect,
              const ReleaseKeys::Key *keys = ReleaseKeys::KEYS, size_t keyCount = ReleaseKeys::SLOTS);

// Same, from the 128 raw bytes (BadBlock when they do not parse)
Result verifyRaw(const uint8_t raw[SigBlock::SIZE], const SigBlock::Expect &expect,
                 const ReleaseKeys::Key *keys = ReleaseKeys::KEYS, size_t keyCount = ReleaseKeys::SLOTS);
} // namespace SigVerify
