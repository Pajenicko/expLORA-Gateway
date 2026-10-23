/**
 * expLORA protocol generation 2 - OTA transfer, pure parts (docs/protocol-v2.md 14)
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ep_ota.h"
#include <cstring>
#include "ep_bytes.h"
#include "ep_tlv.h"

namespace ep
{
namespace ota
{

size_t encodeStart(const Start &s, uint8_t out[kStartLen])
{
    out[0] = s.version;
    putU32(out + 1, s.imageLen);
    putU32(out + 5, s.imageCrc);
    putU32(out + 9, s.fwVersion);
    putU16(out + 13, s.hwType);
    out[15] = s.blockSize;
    out[16] = s.k;
    putU16(out + 17, s.cadence100ms);
    out[19] = s.sf;
    putU16(out + 20, s.startDelayMs);
    return kStartLen;
}

bool decodeStart(const uint8_t *v, size_t len, Start &s)
{
    if (v == nullptr || len < kStartLen)
    {
        return false;
    }
    s.version = v[0];
    s.imageLen = getU32(v + 1);
    s.imageCrc = getU32(v + 5);
    s.fwVersion = getU32(v + 9);
    s.hwType = getU16(v + 13);
    s.blockSize = v[15];
    s.k = v[16];
    s.cadence100ms = getU16(v + 17);
    s.sf = v[19];
    s.startDelayMs = getU16(v + 20);
    return true;
}

uint8_t checkStart(const Start &s)
{
    if (s.version != kTransferVersion)
    {
        return kRefuseVersion;
    }
    const bool imageOk = s.imageLen >= kMinImageLen && s.imageLen <= kSlotSize && (s.imageLen & 7) == 0;
    const bool blockOk = s.blockSize == kBlockSize;
    const bool roundOk = s.k >= kMinK && s.k <= kMaxK;
    const uint32_t frameUs = airtimeUs(kBlockFrameLen, s.sf);
    const bool cadenceOk = s.cadence100ms >= kMinCadence100ms &&
                           (uint32_t)s.cadence100ms * 100000 >= frameUs + 2 * (kRxLeadMs + kRxTimeoutMs) * 1000 &&
                           ((uint32_t)s.k + 1) * s.cadence100ms >= kMinRoundS * 10; // <= 5 statuses per hour
    const bool sfOk = s.sf == 9; // transfer version 1 receivers: SF9 (the field keeps SF7 possible later)
    if (!imageOk || !blockOk || !roundOk || !cadenceOk || !sfOk || s.startDelayMs < kMinStartDelayMs)
    {
        return kRefuseImage;
    }
    return 0;
}

size_t encodeBlock(uint16_t index, const uint8_t *data, size_t len, uint8_t *out, size_t cap)
{
    if (len > kBlockSize || cap < len + 2 || (len > 0 && data == nullptr))
    {
        return 0;
    }
    putU16(out, index);
    memcpy(out + 2, data, len);
    return len + 2;
}

bool decodeBlock(const uint8_t *v, size_t len, uint16_t &index, const uint8_t *&data, size_t &dataLen)
{
    if (v == nullptr || len < 2 + 8 || len > 2 + kBlockSize || ((len - 2) & 7))
    {
        return false;
    }
    index = getU16(v);
    data = v + 2;
    dataLen = len - 2;
    return true;
}

bool sessionTlv(const uint8_t *payload, size_t len, uint8_t &type, const uint8_t *&value, size_t &valueLen)
{
    TlvReader r(payload, len);
    Tlv t;
    if (r.next(t) != 1 || t.channel != 0 || (t.type != down::kOtaBlock && t.type != down::kOtaEnd))
    {
        return false;
    }
    Tlv extra;
    if (r.next(extra) != 0)
    {
        return false; // exactly one TLV
    }
    type = t.type;
    value = t.value;
    valueLen = t.len;
    return true;
}

size_t encodeStatus(const Status &s, uint8_t *out, size_t cap)
{
    if (s.kind == StatusKind::Progress)
    {
        if (cap < kStatusProgressLen)
        {
            return 0;
        }
        putU16(out, s.base);
        putU32(out + 2, (uint32_t)(s.window >> 32));
        putU32(out + 6, (uint32_t)s.window);
        putU16(out + 10, s.slot);
        return kStatusProgressLen;
    }
    if (cap < kStatusEventLen)
    {
        return 0;
    }
    out[0] = (uint8_t)s.kind;
    out[1] = s.arg;
    return kStatusEventLen;
}

bool decodeStatus(const uint8_t *v, size_t len, Status &s)
{
    if (v == nullptr)
    {
        return false;
    }
    if (len == kStatusProgressLen)
    {
        s.kind = StatusKind::Progress;
        s.arg = 0;
        s.base = getU16(v);
        s.window = (uint64_t)getU32(v + 2) << 32 | getU32(v + 6);
        s.slot = getU16(v + 10);
        return true;
    }
    if (len != kStatusEventLen || v[0] == (uint8_t)StatusKind::Progress || v[0] > (uint8_t)StatusKind::BatteryLow)
    {
        return false; // other lengths and unknown kinds are skipped by the receiver
    }
    s.kind = (StatusKind)v[0];
    s.arg = v[1];
    s.base = 0;
    s.window = 0;
    s.slot = 0;
    return true;
}

uint16_t blockLength(uint16_t index, uint32_t imageLen, uint8_t blockSize)
{
    const uint32_t off = blockOffset(index, blockSize);
    if (blockSize == 0 || off >= imageLen)
    {
        return 0;
    }
    const uint32_t rest = imageLen - off;
    return (uint16_t)(rest < blockSize ? rest : blockSize);
}

bool blocksInPage(uint32_t page, uint32_t imageLen, uint8_t blockSize, uint32_t pageSize, uint16_t &first,
                  uint16_t &last)
{
    const uint32_t start = page * pageSize;
    if (blockSize == 0 || pageSize == 0 || start >= imageLen)
    {
        return false;
    }
    uint32_t end = start + pageSize; // exclusive
    if (end > imageLen)
    {
        end = imageLen;
    }
    first = (uint16_t)(start / blockSize);
    last = (uint16_t)((end - 1) / blockSize);
    return true;
}

void Bitmap::reset(uint16_t n)
{
    n_ = n > kMaxBlocks ? kMaxBlocks : n;
    memset(bits_, 0, sizeof(bits_));
}

void Bitmap::set(uint16_t i)
{
    if (i < n_)
    {
        bits_[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}

void Bitmap::clear(uint16_t i)
{
    if (i < n_)
    {
        bits_[i >> 3] &= (uint8_t)~(1u << (i & 7));
    }
}

uint16_t Bitmap::count() const
{
    uint16_t c = 0;
    for (uint16_t i = 0; i < n_; i++)
    {
        c += test(i);
    }
    return c;
}

uint16_t Bitmap::firstMissing() const
{
    for (uint16_t i = 0; i < n_; i++)
    {
        if (!test(i))
        {
            return i;
        }
    }
    return n_;
}

uint64_t Bitmap::window(uint16_t base) const
{
    uint64_t w = 0;
    for (uint16_t b = 0; b < kWindowBlocks; b++)
    {
        const uint32_t i = (uint32_t)base + b;
        if (i < n_ && test((uint16_t)i))
        {
            w |= (uint64_t)1 << b;
        }
    }
    return w;
}

bool Bitmap::applyStatus(uint16_t base, uint64_t window)
{
    if (base > n_)
    {
        return false; // not a valid status for this image
    }
    for (uint16_t i = 0; i < n_; i++)
    {
        const uint32_t b = (uint32_t)i - base; // wraps for i < base
        if (i < base || (b < kWindowBlocks && (window >> b) & 1))
        {
            set(i);
        }
        else
        {
            clear(i); // missing in the window, or above it
        }
    }
    return true;
}

void Bitmap::load(uint16_t n, const uint8_t *bytes)
{
    reset(n);
    memcpy(bits_, bytes, kBytes);
    for (uint16_t i = n_; i < kMaxBlocks; i++)
    {
        bits_[i >> 3] &= (uint8_t)~(1u << (i & 7));
    }
}

uint32_t airtimeUs(size_t frameLen, uint8_t sf)
{
    if (sf < 7 || sf > 12)
    {
        return 0;
    }
    const uint32_t symUs = (1u << sf) * 8; // 2^sf / 125 kHz = 2^sf * 8 us
    const int32_t de = sf >= 11 ? 1 : 0;   // low data rate optimisation
    const int32_t num = 8 * (int32_t)frameLen - 4 * sf + 28 + 16;
    const int32_t den = 4 * (sf - 2 * de);
    const int32_t blocks = num > 0 ? (num + den - 1) / den : 0;
    const uint32_t payloadSym = 8 + (uint32_t)blocks * 5;
    return (16 * 4 + 17) * symUs / 4 + payloadSym * symUs; // preamble 16 + 4.25 symbols
}

uint16_t cadenceFor(uint32_t frameAirtimeUs, uint8_t k)
{
    // OTA share of the hour = 10 % * kOtaSharePermille / 1000 (4.8 %); on
    // average one frame per cadence in k of k + 1 slots:
    //   cadence >= frame * k / (k + 1) / (0.1 * share / 1000)
    const uint64_t num = (uint64_t)frameAirtimeUs * k * 10000;
    const uint64_t den = (uint64_t)(k + 1) * kOtaSharePermille;
    const uint64_t cadenceUs = (num + den - 1) / den;
    uint64_t seconds = (cadenceUs + 999999) / 1000000;
    const uint64_t minRound = (kMinRoundS + k) / ((uint64_t)k + 1); // (K + 1) * C >= 720 s
    if (seconds < minRound)
    {
        seconds = minRound;
    }
    return (uint16_t)(seconds * 10);
}

} // namespace ota
} // namespace ep
