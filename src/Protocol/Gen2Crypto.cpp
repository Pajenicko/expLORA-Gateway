/**
 * expLORA Gateway
 *
 * Platform AES-128 for lib/explora-protocol and gen2 key derivation
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "Gen2Crypto.h"

#include <mbedtls/aes.h>

namespace Gen2Crypto
{

void aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

void deriveJoinKeys(Gen2Sensor &sensor)
{
    uint8_t rootKey[ep::kKeyLen];
    ep::deriveRootKey(aes, sensor.label, sensor.devId, rootKey);
    ep::deriveJoinKeys(aes, rootKey, sensor.devId, sensor.joinKeys);
    ep::wipe(rootKey, sizeof(rootKey));
}

void deriveSessionKeys(const Gen2Sensor &sensor, uint32_t devNonce, uint32_t gwNonce, ep::KeyPair &out)
{
    uint8_t rootKey[ep::kKeyLen];
    ep::deriveRootKey(aes, sensor.label, sensor.devId, rootKey);
    ep::deriveSessionKeys(aes, rootKey, sensor.devId, devNonce, gwNonce, out);
    ep::wipe(rootKey, sizeof(rootKey));
}

} // namespace Gen2Crypto
