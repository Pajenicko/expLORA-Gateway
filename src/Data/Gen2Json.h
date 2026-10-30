/**
 * expLORA Gateway
 *
 * Generation 2 sensor fields in sensors.json ("gen2" array) that need more
 * than a plain copy: the revert record, the slot offset and the last
 * reported configuration version. Templates over
 * the ArduinoJson object type, so the native tests use the same code as the
 * firmware (test/test_gen2_persist).
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include "../Protocol/Gen2Config.h"

namespace Gen2Json
{
// "revFw" / "revCrc": the image that reverted on this sensor (spec 14.5).
// Absent = no record - also once the version ran confirmed (gateway 2.0.2)
template <typename Obj>
void writeRevert(Obj obj, bool has, uint32_t fw, uint32_t crc)
{
    if (has)
    {
        obj["revFw"] = fw;
        obj["revCrc"] = crc;
    }
}

template <typename Obj>
void readRevert(const Obj &obj, bool &has, uint32_t &fw, uint32_t &crc)
{
    has = obj.containsKey("revFw");
    fw = has ? (obj["revFw"] | 0u) : 0u; // | 0u: values >= 2^31 read back (ArduinoJson)
    crc = has ? (obj["revCrc"] | 0u) : 0u;
}

// "slot": the effective slot (all firmware reads it); "slotAuto" (2.0.2):
// false = set by the admin. Without "slotAuto" (written before 2.0.2) slot 0
// is the old default, i.e. automatic (Gen2Config::legacySlotAuto)
template <typename Obj>
void writeSlot(Obj obj, const Gen2Config::Values &v)
{
    obj["slot"] = v.slotS;
    obj["slotAuto"] = v.slotAuto;
}

template <typename Obj>
void readSlot(const Obj &obj, Gen2Config::Values &v)
{
    v.slotS = obj["slot"] | v.slotS;
    v.slotAuto = obj.containsKey("slotAuto") ? (obj["slotAuto"] | true) : Gen2Config::legacySlotAuto(v.slotS);
}

// "cfgRun" (2.0.2): the configuration version the sensor last reported,
// written only while it differs from the desired one (the sensor has not
// applied it yet). Absent = in sync, or unknown (a file written before) -
// both read as the desired version; 0 or an invalid number is not stored
template <typename Obj>
void writeCfgRun(Obj obj, uint8_t desired, uint8_t lastRun)
{
    if (Gen2Config::validVersion(lastRun) && lastRun != desired)
    {
        obj["cfgRun"] = lastRun;
    }
}

template <typename Obj>
uint8_t readCfgRun(const Obj &obj, uint8_t desired)
{
    const unsigned v = obj["cfgRun"] | 0u;
    return v >= 1 && v <= 15 ? (uint8_t)v : desired;
}
} // namespace Gen2Json
