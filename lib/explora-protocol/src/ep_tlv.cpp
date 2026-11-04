/**
 * expLORA protocol generation 2 - TLV payload
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ep_tlv.h"
#include "ep_bytes.h"

#include <cstring>

namespace ep
{

static size_t fixedLen(LenClass cls)
{
    switch (cls)
    {
    case LenClass::One:
        return 1;
    case LenClass::Two:
        return 2;
    case LenClass::Four:
        return 4;
    default:
        return 0;
    }
}

bool TlvWriter::add(uint8_t type, uint8_t channel, const uint8_t *value, size_t len)
{
    const LenClass cls = tlvClass(type);
    const bool var = cls == LenClass::Var;
    if (!ok_ || (type & kTlvChannelBit) || (var ? len > 255 : len != fixedLen(cls)) || (len > 0 && value == nullptr))
    {
        ok_ = false;
        return false;
    }
    const size_t need = 1 + (channel ? 1 : 0) + (var ? 1 : 0) + len;
    if (len_ + need > cap_)
    {
        ok_ = false;
        return false;
    }
    buf_[len_++] = (uint8_t)(type | (channel ? kTlvChannelBit : 0));
    if (channel)
    {
        buf_[len_++] = channel;
    }
    if (var)
    {
        buf_[len_++] = (uint8_t)len;
    }
    memcpy(buf_ + len_, value, len);
    len_ += len;
    return true;
}

bool TlvWriter::addU8(uint8_t type, uint8_t v, uint8_t channel)
{
    return add(type, channel, &v, 1);
}

bool TlvWriter::addU16(uint8_t type, uint16_t v, uint8_t channel)
{
    uint8_t b[2];
    putU16(b, v);
    return add(type, channel, b, 2);
}

bool TlvWriter::addU32(uint8_t type, uint32_t v, uint8_t channel)
{
    uint8_t b[4];
    putU32(b, v);
    return add(type, channel, b, 4);
}

int TlvReader::next(Tlv &t)
{
    if (pos_ >= len_)
    {
        return 0;
    }
    const uint8_t type = buf_[pos_++];
    t.type = type & (uint8_t)~kTlvChannelBit;
    t.channel = 0;
    if (type & kTlvChannelBit)
    {
        if (pos_ >= len_)
            return -1;
        t.channel = buf_[pos_++];
    }
    size_t len = fixedLen(tlvClass(type));
    if (tlvClass(type) == LenClass::Var)
    {
        if (pos_ >= len_)
            return -1;
        len = buf_[pos_++];
    }
    if (len > len_ - pos_)
    {
        pos_ = len_;
        return -1;
    }
    t.len = (uint8_t)len;
    t.value = buf_ + pos_;
    pos_ += len;
    return 1;
}

} // namespace ep
