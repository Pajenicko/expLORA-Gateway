/**
 * expLORA protocol generation 2 - identity and key derivation
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ep_keys.h"
#include "ep_bytes.h"

#include <cstring>

namespace ep
{

static int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parseHex(const char *text, uint8_t *out, size_t bytes, bool allowSeparators)
{
    if (text == nullptr)
    {
        return false;
    }
    size_t digits = 0;
    for (const char *p = text; *p; p++)
    {
        if (allowSeparators && (*p == '-' || *p == ' '))
        {
            continue;
        }
        int v = hexValue(*p);
        if (v < 0 || digits >= 2 * bytes)
        {
            return false;
        }
        if (digits % 2 == 0)
        {
            out[digits / 2] = (uint8_t)(v << 4);
        }
        else
        {
            out[digits / 2] |= (uint8_t)v;
        }
        digits++;
    }
    return digits == 2 * bytes;
}

bool parseLabelKey(const char *text, uint8_t out[kLabelKeyLen])
{
    uint8_t tmp[kLabelKeyLen];
    if (!parseHex(text, tmp, sizeof(tmp), true))
    {
        return false;
    }
    memcpy(out, tmp, sizeof(tmp));
    wipe(tmp, sizeof(tmp));
    return true;
}

bool parseDevId(const char *text, uint32_t &out)
{
    uint8_t b[4];
    if (!parseHex(text, b, sizeof(b), false))
    {
        return false;
    }
    out = getU32(b);
    return true;
}

void deriveRootKey(AesEncryptFn aes, const uint8_t label[kLabelKeyLen], uint32_t devId, uint8_t rootKey[kKeyLen])
{
    uint8_t key[kKeyLen] = {0};
    memcpy(key, label, kLabelKeyLen);
    uint8_t msg[4];
    putU32(msg, devId);
    cmac(aes, key, msg, sizeof(msg), rootKey);
    wipe(key, sizeof(key));
}

static void derive(AesEncryptFn aes, const uint8_t rootKey[kKeyLen], uint8_t label, uint32_t devId, const uint8_t *extra,
                   size_t extraLen, uint8_t out[kKeyLen])
{
    uint8_t msg[1 + 4 + 8];
    msg[0] = label;
    putU32(msg + 1, devId);
    memcpy(msg + 5, extra, extraLen);
    cmac(aes, rootKey, msg, 5 + extraLen, out);
}

void deriveJoinKeys(AesEncryptFn aes, const uint8_t rootKey[kKeyLen], uint32_t devId, KeyPair &out)
{
    derive(aes, rootKey, 0x02, devId, nullptr, 0, out.enc);
    derive(aes, rootKey, 0x04, devId, nullptr, 0, out.mic);
}

void deriveSessionKeys(AesEncryptFn aes, const uint8_t rootKey[kKeyLen], uint32_t devId, uint32_t devNonce,
                       uint32_t gwNonce, KeyPair &out)
{
    uint8_t nonces[8];
    putU32(nonces, devNonce);
    putU32(nonces + 4, gwNonce);
    derive(aes, rootKey, 0x01, devId, nonces, sizeof(nonces), out.enc);
    derive(aes, rootKey, 0x03, devId, nonces, sizeof(nonces), out.mic);
}

} // namespace ep
