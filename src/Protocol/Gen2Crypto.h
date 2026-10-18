/**
 * expLORA Gateway
 *
 * Platform AES-128 for lib/explora-protocol (mbedtls, hardware AES on the
 * ESP32) and key derivation for generation 2 sensors
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "../Data/Gen2Sensor.h"

namespace Gen2Crypto
{
// ep::AesEncryptFn backed by mbedtls
void aes(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

// K from the label key, then the join keys; K is wiped
void deriveJoinKeys(Gen2Sensor &sensor);
} // namespace Gen2Crypto
