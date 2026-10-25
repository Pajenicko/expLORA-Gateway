/**
 * expLORA Gateway — SensorCalibration
 *
 * Pure, header-only sensor calibration / correction helpers. Free of
 * Arduino dependencies so the logic can be exercised by host-side
 * unit tests under PlatformIO's `native` env.
 *
 * Three correction families:
 *   - Additive offsets   (temperature, humidity, pressure, ppm, lux)
 *   - Multiplicative     (wind speed, rain amount, rain rate)
 *   - Wind direction     (signed degree offset with modular wrap)
 *
 * The first two are trivial wrappers but live here so the calibration
 * model is described in one place and so the test suite documents the
 * expected behaviour at the system boundary.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace SensorCalibration
{

// Additive offset for sensors whose correction is a simple delta in the
// reading's own unit (°C, %RH, hPa, ppm, lux).
inline float applyOffset(float rawValue, float offset)
{
    return rawValue + offset;
}

// Multiplicative correction (a unit-less scaling factor). Used for
// wind speed, rain amount and rain rate where calibration is expressed
// as a percentage adjustment (e.g. 1.10 = +10 %).
inline float applyMultiplier(float rawValue, float multiplier)
{
    return rawValue * multiplier;
}

// Wind direction correction with proper modular arithmetic.
//
// `rawDirection` is the value reported by the sensor (typically 0..359
// or up to 360 for due north). `correctionDegrees` can be positive or
// negative and may exceed one full rotation.
//
// The historical inline implementation in SensorManager used
// `(uint16_t + int) % 360`, which suffers from C++ integer-promotion
// pitfalls: a negative result of `+` stays negative through `%`, then
// silently wraps to a huge value when stored back into a uint16_t.
// This helper does the arithmetic in `long` and normalises negatives
// before casting, so e.g. (10°, -20°) correctly yields 350°.
//
// Existing display convention is preserved: a post-modulo result of 0
// is reported as 360 ("true north" → 360°), keeping the visible range
// at 1..360 instead of 0..359.
inline uint16_t applyWindDirectionCorrection(uint16_t rawDirection, int correctionDegrees)
{
    long combined = static_cast<long>(rawDirection) + static_cast<long>(correctionDegrees);
    long mod = combined % 360;
    if (mod < 0)
    {
        mod += 360;
    }
    if (mod == 0)
    {
        return 360; // preserve historical "north as 360" convention
    }
    return static_cast<uint16_t>(mod);
}

// ---------------------------------------------------------------------------
// One sensor's corrections (generation 1 and 2 share them)
// ---------------------------------------------------------------------------

// Fields of a Set: offsets, multipliers, the wind direction offset and the
// altitude for the pressure conversion
enum Field : uint8_t
{
    Temperature,   // + degC
    Humidity,      // + %RH
    Pressure,      // + hPa (before the altitude conversion)
    Co2,           // + ppm
    Lux,           // + lx
    WindSpeed,     // x
    WindDirection, // + deg, wraps
    RainAmount,    // x
    RainRate,      // x
    Altitude,      // m (0 = none)
    FIELDS
};

struct FieldInfo
{
    const char *key;  // sensors.json key (the generation 1 names)
    const char *form; // web form field
    const char *name; // in messages
    float none;       // the value without a correction
    float min, max;   // accepted from the form
    bool whole;       // stored and shown as an integer
};

inline const FieldInfo &info(Field f)
{
    static const FieldInfo table[FIELDS] = {
        {"temperatureCorrection", "tempCorr", "temperature", 0, -50, 50, false},
        {"humidityCorrection", "humCorr", "humidity", 0, -50, 50, false},
        {"pressureCorrection", "pressCorr", "pressure", 0, -200, 200, false},
        {"ppmCorrection", "ppmCorr", "CO2", 0, -5000, 5000, false},
        {"luxCorrection", "luxCorr", "light", 0, -100000, 100000, false},
        {"windSpeedCorrection", "windSpeedCorr", "wind speed", 1, 0, 10, false},
        {"windDirectionCorrection", "windDirCorr", "wind direction", 0, -360, 360, true},
        {"rainAmountCorrection", "rainAmountCorr", "rain", 1, 0, 10, false},
        {"rainRateCorrection", "rainRateCorr", "rain rate", 1, 0, 10, false},
        {"altitude", "altitude", "altitude", 0, 0, 9000, true},
    };
    return table[f];
}

struct Set
{
    float v[FIELDS];
    Set()
    {
        for (int f = 0; f < FIELDS; f++)
        {
            v[f] = info((Field)f).none;
        }
    }
    float operator[](Field f) const { return v[f]; }
    float &operator[](Field f) { return v[f]; }
    bool isNone(Field f) const { return v[f] == info(f).none; }
};

// A stored value as form text: the shortest that reads back as the same
// float (an unchanged save is lossless, e.g. a 1.x 0.125), whole fields
// as integers
inline void formText(char *out, size_t cap, Field f, float v)
{
    if (info(f).whole)
    {
        snprintf(out, cap, "%ld", (long)v);
        return;
    }
    for (int d = 0; d <= 9; d++)
    {
        snprintf(out, cap, "%.*f", d, (double)v);
        if ((float)strtod(out, nullptr) == v)
        {
            return;
        }
    }
    snprintf(out, cap, "%.9g", (double)v);
}

enum class Parsed : uint8_t
{
    Ok,
    NotNumber,
    OutOfRange
};

// A posted form value (trimmed) of field f into out: empty = no
// correction; the stored value posted back unchanged is kept even outside
// min..max (an older firmware did not check it), so it never blocks saving
// the other fields; anything else must be a number within min..max
inline Parsed parse(const char *text, Field f, float stored, float &out)
{
    const FieldInfo &i = info(f);
    if (*text == '\0')
    {
        out = i.none;
        return Parsed::Ok;
    }
    char *end = nullptr;
    double d = strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite((float)d))
    {
        return Parsed::NotNumber;
    }
    if (i.whole)
    {
        // In range of long before the cast (a larger value is undefined
        // behaviour there); far outside every limit and any stored value
        if (!(std::fabs(d) < 1e9))
        {
            return Parsed::OutOfRange;
        }
        d = (double)(long)d;
    }
    const float x = (float)d;
    if (x != stored && (x < i.min || x > i.max))
    {
        return Parsed::OutOfRange;
    }
    out = x;
    return Parsed::Ok;
}

// Generation 2 corrections are stored rounded to 0.001 (CalibrationJson
// writeJson): an entered value is rounded the same way, so what is applied
// now is what a restart reads back
inline float milli(float x)
{
    return (float)(std::round(x * 1000.0) / 1000.0);
}

// A reading corrected by the field of its quantity (the altitude is not a
// reading: pressureAtAltitude)
inline float apply(const Set &s, Field f, float raw)
{
    switch (f)
    {
    case WindSpeed:
    case RainAmount:
    case RainRate:
        return applyMultiplier(raw, s[f]);
    case WindDirection:
        return applyWindDirectionCorrection((uint16_t)raw, (int)s[f]);
    case Altitude:
    case FIELDS:
        return raw;
    default:
        return applyOffset(raw, s[f]);
    }
}

// Relative (sea level) pressure from the station pressure at altitude_m
// (barometric formula with the measured temperature); unchanged at 0 m
inline float pressureAtAltitude(float pressureHpa, int altitudeM, float temperatureC)
{
    if (altitudeM == 0)
    {
        return pressureHpa;
    }
    const double g = 9.80665, m = 0.0289644, r = 8.3144598, l = 0.0065;
    const double t = temperatureC + 273.15;
    return (float)(pressureHpa / std::pow(1 - (l * altitudeM) / t, (g * m) / (r * l)));
}

} // namespace SensorCalibration
