/**
 * expLORA Gateway
 *
 * SigVerify - see SigVerify.h
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

#include "SigVerify.h"

#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

namespace SigVerify
{
const char *resultText(Result r)
{
    switch (r)
    {
    case Result::Ok:
        return "signature verified";
    case Result::NoKey:
        return "signed with an unknown key";
    case Result::BadSig:
        return "invalid signature";
    case Result::BadBlock:
        return "malformed signature block";
    case Result::WrongKind:
        return "signature is for another kind of image";
    case Result::WrongTarget:
        return "signature is for another board or sensor type";
    case Result::WrongVersion:
        return "signature is for another firmware version";
    case Result::WrongLength:
        return "signature is for a file of another length";
    case Result::WrongHash:
        return "file content does not match the signature";
    case Result::Error:
        return "signature check failed (no memory or bad key)";
    }
    return "?";
}

bool keyId(const uint8_t point[ReleaseKeys::POINT_SIZE], uint32_t &out)
{
    uint8_t h[32];
    if (mbedtls_sha256(point, ReleaseKeys::POINT_SIZE, h, 0) != 0)
    {
        return false;
    }
    out = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    return true;
}

Result verifyDigest(const uint8_t point[ReleaseKeys::POINT_SIZE], const uint8_t digest[32], const uint8_t sig[64])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    Result result = Result::Error;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_point_read_binary(&grp, &q, point, ReleaseKeys::POINT_SIZE) == 0 &&
        mbedtls_ecp_check_pubkey(&grp, &q) == 0 && mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
        mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0)
    {
        int rc = mbedtls_ecdsa_verify(&grp, digest, 32, &q, &r, &s);
        // r or s out of range is a verify failure too
        result = rc == 0 ? Result::Ok : rc == MBEDTLS_ERR_ECP_VERIFY_FAILED ? Result::BadSig : Result::Error;
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return result;
}

Result verify(const SigBlock::Block &given, const SigBlock::Expect &expect, const ReleaseKeys::Key *keys,
              size_t keyCount)
{
    return verifyRaw(given.raw, expect, keys, keyCount);
}

Result verifyRaw(const uint8_t raw[SigBlock::SIZE], const SigBlock::Expect &expect, const ReleaseKeys::Key *keys,
                 size_t keyCount)
{
    if (!raw || !expect.payloadHash || (keyCount && !keys))
    {
        return Result::Error;
    }
    // Every field below comes from the bytes the signature covers
    SigBlock::Block block;
    if (SigBlock::parse(raw, block) != SigBlock::Format::Ok)
    {
        return Result::BadBlock;
    }
    switch (SigBlock::checkBinding(block, expect))
    {
    case SigBlock::Binding::Ok:
        break;
    case SigBlock::Binding::WrongKind:
        return Result::WrongKind;
    case SigBlock::Binding::WrongTarget:
        return Result::WrongTarget;
    case SigBlock::Binding::WrongVersion:
        return Result::WrongVersion;
    case SigBlock::Binding::WrongLength:
        return Result::WrongLength;
    case SigBlock::Binding::WrongHash:
        return Result::WrongHash;
    }
    const ReleaseKeys::Key *key = nullptr;
    for (size_t i = 0; block.keyId != 0 && i < keyCount; i++)
    {
        if (keys[i].keyId == block.keyId)
        {
            key = &keys[i];
            break;
        }
    }
    if (!key)
    {
        return Result::NoKey;
    }
    uint8_t digest[32];
    if (mbedtls_sha256(block.raw, SigBlock::SIGNED_BYTES, digest, 0) != 0)
    {
        return Result::Error;
    }
    return verifyDigest(key->point, digest, block.signature);
}

} // namespace SigVerify
