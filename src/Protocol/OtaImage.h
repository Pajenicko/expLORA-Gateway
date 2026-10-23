/**
 * expLORA Gateway
 *
 * Sensor image validation for the OTA image store (protocol v2,
 * docs/protocol-v2.md 14.5, trailer format 10.1): a streaming pass computes
 * the CRC-32 over the image and the first 28 trailer bytes while keeping the
 * size word (vector table offset 0x20) and the last 32 bytes (the trailer),
 * so an upload is never buffered in RAM. Pure C++, unit-tested natively in
 * test/test_ota_image.
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
#include <cstdio>
#include <cstring>

#include "ExploraProtocol.h"

namespace OtaImage
{
constexpr uint32_t TRAILER_MAGIC = 0x544C5845; // "EXLT"
constexpr uint16_t TRAILER_VERSION = 1;
constexpr uint32_t TRAILER_SIZE = 32;
constexpr uint32_t SIZE_WORD_OFFSET = 0x20;
constexpr uint32_t LOAD_ADDRESS = 0x08002000; // slot A of the sensor
constexpr uint32_t MAX_LEN = ep::ota::kSlotSize; // image + trailer
constexpr uint8_t MAX_HW_TYPES = 4;

// CRC-32 (zlib / IEEE 802.3, reflected 0xEDB88320), streaming: crc starts at 0
inline uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

struct Trailer
{
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t hwType = 0;
    uint32_t fw = 0;
    uint32_t size = 0;
    uint32_t load = 0;
    uint32_t flags = 0;
    uint32_t crc = 0;
};

inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

inline Trailer parseTrailer(const uint8_t t[TRAILER_SIZE])
{
    Trailer r;
    r.magic = le32(t);
    r.version = (uint16_t)(t[4] | t[5] << 8);
    r.hwType = (uint16_t)(t[6] | t[7] << 8);
    r.fw = le32(t + 8);
    r.size = le32(t + 12);
    r.load = le32(t + 16);
    r.flags = le32(t + 20);
    r.crc = le32(t + 28);
    return r;
}

// Streaming pass over a whole image file
class Streamer
{
public:
    void feed(const uint8_t *data, size_t len)
    {
        for (size_t i = 0; i < len; i++)
        {
            const uint8_t b = data[i];
            if (total_ >= SIZE_WORD_OFFSET && total_ < SIZE_WORD_OFFSET + 4)
            {
                size_[total_ - SIZE_WORD_OFFSET] = b;
            }
            // The CRC covers everything but the last 4 bytes: lag by 4
            if (total_ >= 4)
            {
                const uint8_t out = lag_[total_ & 3];
                crc_ = crc32Update(crc_, &out, 1);
            }
            lag_[total_ & 3] = b;
            tail_[total_ % TRAILER_SIZE] = b;
            total_++;
        }
    }
    uint32_t total() const { return total_; }
    uint32_t crc() const { return crc_; } // over the first total - 4 bytes
    uint32_t sizeWord() const { return le32(size_); }
    // The last 32 bytes in order (valid when total >= 32)
    void trailer(uint8_t out[TRAILER_SIZE]) const
    {
        for (uint32_t i = 0; i < TRAILER_SIZE; i++)
        {
            out[i] = tail_[(total_ + i) % TRAILER_SIZE];
        }
    }

private:
    uint32_t total_ = 0;
    uint32_t crc_ = 0;
    uint8_t lag_[4] = {};
    uint8_t size_[4] = {};
    uint8_t tail_[TRAILER_SIZE] = {};
};

enum class Error : uint8_t
{
    None,
    TooShort,
    TooLarge,
    Length,      // file length != size word + 32
    Align,       // length not a multiple of 8 (the sensor refuses it)
    Magic,
    Version,
    Size,        // trailer size != size word
    Load,        // not linked for slot A (0x08002000)
    Crc,
    NoOta,       // trailer flags bit 0 clear: no OTA receiver in the image
    OtaVersion,  // transfer version outside 1..1
};

inline const char *errorText(Error e)
{
    switch (e)
    {
    case Error::None:
        return "ok";
    case Error::TooShort:
        return "file too short for a sensor image";
    case Error::TooLarge:
        return "image larger than the sensor's slot (114,688 bytes)";
    case Error::Length:
        return "file length does not match the image size in the vector table (not a trailered sensor image?)";
    case Error::Align:
        return "image length is not a multiple of 8 bytes (the sensor would refuse it)";
    case Error::Magic:
        return "no image trailer (magic)";
    case Error::Version:
        return "unknown trailer version";
    case Error::Size:
        return "trailer size differs from the vector table";
    case Error::Load:
        return "image not linked for slot A (0x08002000)";
    case Error::Crc:
        return "CRC mismatch - damaged file";
    case Error::NoOta:
        return "the image has no OTA receiver - a sensor running it could not be updated over the air again";
    case Error::OtaVersion:
        return "the image's OTA transfer version is not supported by this gateway";
    }
    return "?";
}

// Validate a finished stream (the trailer flags rule of 14.5 included)
inline Error validate(const Streamer &s, Trailer &t)
{
    if (s.total() < ep::ota::kMinImageLen)
    {
        return Error::TooShort;
    }
    if (s.total() > MAX_LEN)
    {
        return Error::TooLarge;
    }
    if ((uint64_t)s.sizeWord() + TRAILER_SIZE != s.total())
    {
        return Error::Length;
    }
    if (s.total() % 8 != 0)
    {
        return Error::Align;
    }
    uint8_t raw[TRAILER_SIZE];
    s.trailer(raw);
    t = parseTrailer(raw);
    if (t.magic != TRAILER_MAGIC)
    {
        return Error::Magic;
    }
    if (t.version != TRAILER_VERSION)
    {
        return Error::Version;
    }
    if (t.size != s.sizeWord())
    {
        return Error::Size;
    }
    if (t.load != LOAD_ADDRESS)
    {
        return Error::Load;
    }
    if (t.crc != s.crc())
    {
        return Error::Crc;
    }
    if (!(t.flags & 1))
    {
        return Error::NoOta;
    }
    const uint32_t v = (t.flags >> 8) & 0x0F;
    if (v < 1 || v > ep::ota::kTransferVersion)
    {
        return Error::OtaVersion;
    }
    return Error::None;
}

inline void versionText(uint32_t fw, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u", (unsigned)(fw >> 16 & 0xFF), (unsigned)(fw >> 8 & 0xFF), (unsigned)(fw & 0xFF));
}
} // namespace OtaImage
