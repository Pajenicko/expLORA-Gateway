/**
 * expLORA Gateway
 *
 * HaDiscovery - MQTT state topics and Home Assistant discovery of generation
 * 1 and 2 sensors: <prefix>/<ID>/<quantity>, unique_id <prefix>_<ID>_<quantity>
 * (pure, native tests)
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
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

#include <ArduinoJson.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "../Data/Quantity.h"

namespace HaDiscovery
{
struct Device
{
    const char *prefix = "";   // root topic
    const char *haPrefix = ""; // discovery prefix
    const char *id = "";       // 6 hex digits (serial number) or 8 (DevID)
    const char *name = "";
    const char *model = "";    // CLIMA / CARBON / METEO / "Gen 2 HW3"
    const char *sw = nullptr;  // firmware version (generation 2), nullptr = none
};

// <haPrefix>/sensor/<prefix>_<id>_<quantity>/config
inline size_t topic(char *out, size_t cap, const Device &d, Qty::Id q)
{
    return (size_t)snprintf(out, cap, "%s/sensor/%s_%s_%s/config", d.haPrefix, d.prefix, d.id, Qty::info(q).mqtt);
}

// <prefix>/<id>/<quantity>
inline size_t stateTopic(char *out, size_t cap, const Device &d, Qty::Id q)
{
    return (size_t)snprintf(out, cap, "%s/%s/%s", d.prefix, d.id, Qty::info(q).mqtt);
}

// State of a quantity whose reading failed: Home Assistant's "no value"
// (the entity shows unknown)
constexpr const char *STATE_NONE = "None";

// The state value: fixed decimals, whole numbers truncated (as 1.x did)
inline size_t stateValue(char *out, size_t cap, Qty::Id q, float v)
{
    const uint8_t d = Qty::info(q).mqttDecimals;
    return d == 0 ? (size_t)snprintf(out, cap, "%ld", (long)v) : (size_t)snprintf(out, cap, "%.*f", (int)d, v);
}

// Discovery payload (the entity names, keys and their order of 1.x: Home
// Assistant keeps the existing entities); 0 when it does not fit cap
inline size_t payload(char *out, size_t cap, const Device &d, Qty::Id q)
{
    const Qty::Info &info = Qty::info(q);
    char key[24], name[64], unique[96], state[96], avail[48];
    snprintf(key, sizeof(key), "%s", info.mqtt);
    key[0] = (char)toupper((unsigned char)key[0]); // "Temperature", "Wind_speed"
    const size_t nameLen = strlen(d.name), keyLen = strlen(key);
    if (q == Qty::DailyRain)
    {
        snprintf(name, sizeof(name), "%s Daily Rain Total", d.name);
    }
    else if (nameLen >= keyLen && strcmp(d.name + nameLen - keyLen, key) == 0)
    {
        snprintf(name, sizeof(name), "%s", d.name); // a name ending with the quantity is used as it is
    }
    else
    {
        snprintf(name, sizeof(name), "%s", key);
    }
    stateTopic(state, sizeof(state), d, q);
    snprintf(unique, sizeof(unique), "%s_%s_%s", d.prefix, d.id, info.mqtt);
    snprintf(avail, sizeof(avail), "%s/status", d.prefix);
    StaticJsonDocument<768> doc;
    doc["name"] = (char *)name; // copied
    doc["state_topic"] = (char *)state;
    doc["value_template"] = "{{ value }}";
    doc["unique_id"] = (char *)unique;
    doc["availability_topic"] = (char *)avail;
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
    doc["device_class"] = info.haClass;
    doc["unit_of_measurement"] = info.unit;
    if (info.haPrecision >= 0)
    {
        doc["suggested_display_precision"] = info.haPrecision;
    }
    JsonObject device = doc.createNestedObject("device");
    device["identifiers"] = d.id;
    device["name"] = d.name;
    device["model"] = d.model;
    device["manufacturer"] = "expLORA";
    if (d.sw)
    {
        device["sw_version"] = d.sw;
    }
    if (doc.overflowed() || measureJson(doc) + 1 > cap)
    {
        return 0;
    }
    return serializeJson(doc, out, cap);
}
} // namespace HaDiscovery
