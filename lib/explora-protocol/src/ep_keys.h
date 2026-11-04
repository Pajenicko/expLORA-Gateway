/**
 * expLORA protocol generation 2 - identity and key derivation
 *
 *   label key L   64 bit, printed on the sensor as 16 hex digits (dashes optional)
 *   K             = AES-CMAC(L || 0^64, DevID)                  root key
 *   K_join_enc    = AES-CMAC_K(0x02 || DevID)                   JOIN_ACC encryption
 *   K_join_mic    = AES-CMAC_K(0x04 || DevID)                   JOIN_REQ / JOIN_ACC MIC
 *   Ks_enc        = AES-CMAC_K(0x01 || DevID || devNonce || gwNonce)
 *   Ks_mic        = AES-CMAC_K(0x03 || DevID || devNonce || gwNonce)
 *
 * All integers big endian. See docs/protocol-v2.md.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "ep_crypto.h"

namespace ep
{

constexpr size_t kLabelKeyLen = 8;

struct KeyPair
{
    uint8_t enc[kKeyLen];
    uint8_t mic[kKeyLen];
};

// "A1B2-C3D4-E5F6-0718" / "a1b2c3d4e5f60718": exactly 16 hex digits,
// '-' and ' ' ignored
bool parseLabelKey(const char *text, uint8_t out[kLabelKeyLen]);

// "3EA4191D": exactly 8 hex digits
bool parseDevId(const char *text, uint32_t &out);

void deriveRootKey(AesEncryptFn aes, const uint8_t label[kLabelKeyLen], uint32_t devId, uint8_t rootKey[kKeyLen]);

void deriveJoinKeys(AesEncryptFn aes, const uint8_t rootKey[kKeyLen], uint32_t devId, KeyPair &out);

void deriveSessionKeys(AesEncryptFn aes, const uint8_t rootKey[kKeyLen], uint32_t devId, uint32_t devNonce,
                       uint32_t gwNonce, KeyPair &out);

} // namespace ep
