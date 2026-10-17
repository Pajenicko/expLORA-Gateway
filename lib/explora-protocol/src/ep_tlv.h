/**
 * expLORA protocol generation 2 - TLV payload
 *
 * Type byte: LL C TTTTT
 *   LL     length class: 00 = 1 byte, 01 = 2 bytes, 10 = 4 bytes,
 *          11 = a length byte follows (0-255)
 *   C      1 = a channel byte follows the type (absent = channel 0)
 *   TTTTT  type id within the class
 * Order: type [channel] [length] value. Values big endian. A receiver skips
 * unknown types using the length class. Uplink and downlink have separate
 * type registries (namespaces up / down below).
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

enum class LenClass : uint8_t
{
    One = 0,
    Two = 1,
    Four = 2,
    Var = 3,
};

constexpr uint8_t kTlvChannelBit = 0x20;
constexpr uint8_t kTlvIdMask = 0x1F;

constexpr uint8_t tlvType(LenClass cls, uint8_t id)
{
    return (uint8_t)(((uint8_t)cls << 6) | (id & kTlvIdMask));
}
constexpr LenClass tlvClass(uint8_t type)
{
    return (LenClass)(type >> 6);
}

// Invalid-value sentinels (sensor present, reading failed)
constexpr int8_t kInvalidI8 = INT8_MIN;
constexpr uint8_t kInvalidU8 = 0xFF;
constexpr int16_t kInvalidI16 = INT16_MIN;
constexpr uint16_t kInvalidU16 = 0xFFFF;
constexpr uint32_t kInvalidU32 = 0xFFFFFFFF;

// Uplink TLV types (sensor -> gateway)
namespace up
{
constexpr uint8_t kDownlinkRssi = tlvType(LenClass::One, 0x01);  // i8 dBm of the last downlink
constexpr uint8_t kDownlinkSnr = tlvType(LenClass::One, 0x02);   // i8, 0.25 dB
constexpr uint8_t kInputs = tlvType(LenClass::One, 0x03);        // u8 bit mask of digital inputs
constexpr uint8_t kRelays = tlvType(LenClass::One, 0x04);        // u8 bit mask (reserved, phase B)
constexpr uint8_t kPowerSource = tlvType(LenClass::One, 0x05);   // u8: 0 battery, 1 mains, 2 USB, 3 solar/charging
constexpr uint8_t kResetCause = tlvType(LenClass::One, 0x06);    // u8: RCC_CSR flags >> 24 (first UP after boot)
constexpr uint8_t kTemperature = tlvType(LenClass::Two, 0x01);   // i16, 0.01 degC
constexpr uint8_t kHumidity = tlvType(LenClass::Two, 0x02);      // u16, 0.01 %RH
constexpr uint8_t kPressure = tlvType(LenClass::Two, 0x03);      // u16, 0.1 hPa
constexpr uint8_t kCo2 = tlvType(LenClass::Two, 0x04);           // u16, ppm
constexpr uint8_t kIlluminance = tlvType(LenClass::Two, 0x05);   // u16, round(2048 * log2(lux + 1))
constexpr uint8_t kBattery = tlvType(LenClass::Two, 0x06);       // u16, mV
constexpr uint8_t kWindAvg = tlvType(LenClass::Two, 0x07);       // u16, 0.1 m/s
constexpr uint8_t kWindGust = tlvType(LenClass::Two, 0x08);      // u16, 0.1 m/s
constexpr uint8_t kWindDir = tlvType(LenClass::Two, 0x09);       // u16, 0.1 deg (0-3599)
constexpr uint8_t kEvent = tlvType(LenClass::Two, 0x0A);         // u8 event seq | u8 code (button: n << 1 | long)
constexpr uint8_t kRainTips = tlvType(LenClass::Four, 0x01);     // u32 tip counter
constexpr uint8_t kPulses = tlvType(LenClass::Four, 0x02);       // u32 pulse counter (per channel)
constexpr uint8_t kBoardData = tlvType(LenClass::Var, 0x00);     // board specific
constexpr uint8_t kModbus = tlvType(LenClass::Var, 0x01);        // u8 slave | u8 function | u16 start | u16 values...
constexpr uint8_t kStatus = tlvType(LenClass::Var, 0x02);        // answer to down::kStatusRequest (phase B)
} // namespace up

// Downlink TLV types (gateway -> sensor), in DOWN and JOIN_ACC ACCEPT
namespace down
{
constexpr uint8_t kTxPower = tlvType(LenClass::One, 0x01);       // i8 dBm (sensor clamps per band)
constexpr uint8_t kStatusRequest = tlvType(LenClass::One, 0x02); // u8 what (reserved)
constexpr uint8_t kReboot = tlvType(LenClass::One, 0x03);        // u8 0xB0 = reboot
constexpr uint8_t kAckPolicy = tlvType(LenClass::Two, 0x01);     // u8 ACK every N-th UP (0 never) | u4 retries | u4 backoff s
constexpr uint8_t kInterval = tlvType(LenClass::Four, 0x01);     // u16 interval s | u16 slot offset s
constexpr uint8_t kTime = tlvType(LenClass::Four, 0x02);         // u32 unix time at the start of this transmission
constexpr uint8_t kUplinkFreq = tlvType(LenClass::Four, 0x03);   // u32 Hz (revert after 2 uplinks without ACK)
constexpr uint8_t kBoardCommand = tlvType(LenClass::Var, 0x00);  // board specific (e.g. SCD40 recalibration)
} // namespace down

struct Tlv
{
    uint8_t type;
    uint8_t channel;
    uint8_t len;
    const uint8_t *value;
};

class TlvWriter
{
public:
    TlvWriter(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap), len_(0), ok_(true) {}

    // Value length must match the type's length class (any length for Var)
    bool add(uint8_t type, uint8_t channel, const uint8_t *value, size_t len);
    bool addU8(uint8_t type, uint8_t v, uint8_t channel = 0);
    bool addI8(uint8_t type, int8_t v, uint8_t channel = 0) { return addU8(type, (uint8_t)v, channel); }
    bool addU16(uint8_t type, uint16_t v, uint8_t channel = 0);
    bool addI16(uint8_t type, int16_t v, uint8_t channel = 0) { return addU16(type, (uint16_t)v, channel); }
    bool addU32(uint8_t type, uint32_t v, uint8_t channel = 0);

    size_t size() const { return len_; }
    bool ok() const { return ok_; } // false once any add failed

private:
    uint8_t *buf_;
    size_t cap_;
    size_t len_;
    bool ok_;
};

class TlvReader
{
public:
    TlvReader(const uint8_t *buf, size_t len) : buf_(buf), len_(len), pos_(0) {}

    // 1 = next TLV in t, 0 = end, -1 = malformed (truncated)
    int next(Tlv &t);

private:
    const uint8_t *buf_;
    size_t len_;
    size_t pos_;
};

// Value helpers (big endian)
inline uint16_t tlvU16(const Tlv &t)
{
    return (uint16_t)((t.value[0] << 8) | t.value[1]);
}
inline uint32_t tlvU32(const Tlv &t)
{
    return ((uint32_t)t.value[0] << 24) | ((uint32_t)t.value[1] << 16) | ((uint32_t)t.value[2] << 8) | t.value[3];
}

} // namespace ep
