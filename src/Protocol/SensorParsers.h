/**
 * expLORA Gateway — SensorParsers
 *
 * Pure, header-only parsers that turn raw decrypted LoRa packet bytes
 * into per-sensor structs. No Arduino dependencies — designed to be
 * exercised by host-side unit tests under PlatformIO's `native` env.
 *
 * Wire format (shared by every sensor type):
 *   byte 0     packet flags (not parsed here)
 *   byte 1     device type    (not parsed here, used for dispatch)
 *   byte 2..4  serial number  (24-bit big-endian)
 *   byte 5..6  battery raw    (uint16 big-endian, millivolts)
 *   byte 7     number of values
 *   byte 8+    sensor-specific payload (see each parse* function)
 *
 * Length requirements stated below cover ONLY the bytes the parser
 * actually reads. They do NOT include the trailing checksum byte —
 * checksum validation is the caller's job (see LoRaCrypto).
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace SensorParsers
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline uint16_t readU16BE(const uint8_t *data, size_t offset)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

inline int16_t readI16BE(const uint8_t *data, size_t offset)
{
    return static_cast<int16_t>(readU16BE(data, offset));
}

inline uint32_t readU32BE(const uint8_t *data, size_t offset)
{
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
}

inline uint32_t readU24BE(const uint8_t *data, size_t offset)
{
    return (static_cast<uint32_t>(data[offset]) << 16) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           static_cast<uint32_t>(data[offset + 2]);
}

// ---------------------------------------------------------------------------
// Common header (serial number + battery voltage)
// ---------------------------------------------------------------------------

struct CommonHeader
{
    uint32_t serialNumber;  // 24-bit value, upper byte always 0
    float    batteryVoltage; // volts (raw mV / 1000)
};

// Minimum bytes to parse: 7 (indices 0..6).
inline bool parseCommonHeader(const uint8_t *data, uint8_t len, CommonHeader &out)
{
    if (len < 7) return false;
    out.serialNumber   = readU24BE(data, 2);
    out.batteryVoltage = readU16BE(data, 5) / 1000.0f;
    return true;
}

// ---------------------------------------------------------------------------
// BME280 — temperature, pressure, humidity
// ---------------------------------------------------------------------------

struct BME280Data
{
    float temperature; // °C (raw int16 / 100)
    float pressure;    // hPa (raw uint16 / 10)
    float humidity;    // %   (raw uint16 / 100)
};

// Minimum bytes to parse: 14 (indices 0..13).
inline bool parseBME280(const uint8_t *data, uint8_t len, BME280Data &out)
{
    if (len < 14) return false;
    out.temperature = readI16BE(data, 8)  / 100.0f;
    out.pressure    = readU16BE(data, 10) / 10.0f;
    out.humidity    = readU16BE(data, 12) / 100.0f;
    return true;
}

// ---------------------------------------------------------------------------
// SCD40 — temperature, CO2, humidity
// ---------------------------------------------------------------------------

struct SCD40Data
{
    float temperature; // °C
    float ppm;         // ppm (raw uint16, no scaling)
    float humidity;    // %
};

// Minimum bytes to parse: 14 (indices 0..13).
inline bool parseSCD40(const uint8_t *data, uint8_t len, SCD40Data &out)
{
    if (len < 14) return false;
    out.temperature = readI16BE(data, 8)  / 100.0f;
    out.ppm         = static_cast<float>(readU16BE(data, 10));
    out.humidity    = readU16BE(data, 12) / 100.0f;
    return true;
}

// ---------------------------------------------------------------------------
// METEO — temp / pressure / humidity / wind / rain (extended packet adds
// rain rate)
//
// Basic packet is 20 bytes (indices 0..19), extended is 22 bytes
// (indices 0..21). hasRainRate signals which variant was decoded.
// ---------------------------------------------------------------------------

struct METEOData
{
    float    temperature;    // °C
    float    pressure;       // hPa
    float    humidity;       // %
    float    windSpeed;      // m/s (raw uint16 * 0.111111)
    uint16_t windDirection;  // degrees (raw uint16, no scaling)
    float    rainAmount;     // mm   (raw uint16 / 5000)
    float    rainRate;       // mm/h (raw uint16 / 500), 0 if not present
    bool     hasRainRate;    // true only if len >= 22
};

// Minimum bytes to parse: 20 (basic) or 22 (extended).
inline bool parseMETEO(const uint8_t *data, uint8_t len, METEOData &out)
{
    if (len < 20) return false;
    out.temperature   = readI16BE(data, 8)  / 100.0f;
    out.pressure      = readU16BE(data, 10) / 10.0f;
    out.humidity      = readU16BE(data, 12) / 100.0f;
    out.windSpeed     = readU16BE(data, 14) * 0.111111f;
    out.windDirection = readU16BE(data, 16);
    out.rainAmount    = readU16BE(data, 18) / 5000.0f;
    if (len >= 22)
    {
        out.rainRate    = readU16BE(data, 20) / 500.0f;
        out.hasRainRate = true;
    }
    else
    {
        out.rainRate    = 0.0f;
        out.hasRainRate = false;
    }
    return true;
}

} // namespace SensorParsers
