/**
 * expLORA protocol generation 2 - crypto primitives
 *
 * Everything is built on one AES-128 block encryption supplied by the
 * platform (mbedtls on ESP32, HAL CRYP on STM32WL) or the portable software
 * implementation below. CMAC follows NIST SP 800-38B, CTR NIST SP 800-38A
 * (128-bit big-endian counter increment).
 *
 * Shared verbatim between expLORA-Gateway and expLORA-Sensors-gen2
 * (lib/explora-protocol, scripts/sync_protocol.sh).
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ep
{

constexpr size_t kAesBlock = 16;
constexpr size_t kKeyLen = 16;

// Platform AES-128: encrypt one 16-byte block with a 16-byte key.
// in and out may be the same buffer.
using AesEncryptFn = void (*)(const uint8_t key[kKeyLen], const uint8_t in[kAesBlock], uint8_t out[kAesBlock]);

// Portable software AES-128 (encryption only, FIPS-197)
void aesSoftEncrypt(const uint8_t key[kKeyLen], const uint8_t in[kAesBlock], uint8_t out[kAesBlock]);

// Incremental AES-CMAC
class Cmac
{
public:
    Cmac(AesEncryptFn aes, const uint8_t key[kKeyLen]);
    void update(const uint8_t *data, size_t len);
    void finish(uint8_t mac[kAesBlock]);

private:
    AesEncryptFn aes_;
    uint8_t key_[kKeyLen];
    uint8_t x_[kAesBlock];   // chaining value
    uint8_t buf_[kAesBlock]; // pending (possibly last) block
    size_t used_;
};

// One-shot AES-CMAC
void cmac(AesEncryptFn aes, const uint8_t key[kKeyLen], const uint8_t *msg, size_t len, uint8_t mac[kAesBlock]);

// AES-CTR: XOR data with the key stream starting at counterBlock (copied,
// incremented as a 128-bit big-endian number after every block)
void ctrXor(AesEncryptFn aes, const uint8_t key[kKeyLen], const uint8_t counterBlock[kAesBlock], uint8_t *data,
            size_t len);

// Comparison without early exit (MIC checks)
bool equalConstTime(const uint8_t *a, const uint8_t *b, size_t len);

// Overwrite key material
void wipe(void *p, size_t len);

} // namespace ep
