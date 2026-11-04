/**
 * expLORA Gateway
 *
 * Sha256 - streaming SHA-256 over mbedtls (the ESP32 hardware engine on
 * the gateway, the same mbedtls version on the host for the native tests).
 * Usable as the sink of SigBlock::TailRing.
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

#include <mbedtls/sha256.h>

class Sha256
{
public:
    Sha256()
    {
        mbedtls_sha256_init(&ctx_);
        ok_ = mbedtls_sha256_starts(&ctx_, 0) == 0;
    }
    ~Sha256() { mbedtls_sha256_free(&ctx_); }
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;

    void update(const uint8_t *data, size_t len)
    {
        if (ok_ && len)
        {
            ok_ = mbedtls_sha256_update(&ctx_, data, len) == 0;
        }
    }

    // false: the hash engine failed (out must not be used)
    bool finish(uint8_t out[32])
    {
        if (ok_)
        {
            ok_ = mbedtls_sha256_finish(&ctx_, out) == 0;
        }
        bool r = ok_;
        ok_ = false; // finished: further updates are ignored
        return r;
    }

private:
    mbedtls_sha256_context ctx_;
    bool ok_ = false;
};
