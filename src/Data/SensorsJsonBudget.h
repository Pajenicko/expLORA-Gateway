/**
 * expLORA Gateway
 *
 * SensorsJsonBudget - ArduinoJson document sizes for sensors.json, shared by
 * SensorManager::snapshotLocked (write) and parseSensorsLocked (read) and
 * pinned by the native tests (test/test_sensors_json)
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
#include <cstddef>
#include <memory>
#include <new>

namespace SensorsJson
{
    // Table sizes (MAX_SENSORS in config.h, MAX_GEN2_SENSORS in
    // Data/Gen2Sensor.h; SensorManager.cpp asserts they match)
    constexpr size_t MAX_GEN1 = 20;
    constexpr size_t MAX_GEN2 = 10;

    // Written per entry (SensorManager::snapshotLocked): a generation 1
    // sensor has at most 17 members (budget 20); a generation 2 sensor 18
    // members with the otaWatch (5) and otaAbort (3) arrays
    constexpr size_t GEN1_WRITE = JSON_OBJECT_SIZE(20);
    constexpr size_t GEN2_WRITE = JSON_OBJECT_SIZE(18) + JSON_ARRAY_SIZE(5) + JSON_ARRAY_SIZE(3);

    // Read per entry, with slack: a file from another firmware version
    // (members added later, a downgrade) is still a valid configuration
    constexpr size_t GEN1_READ = JSON_OBJECT_SIZE(24);
    constexpr size_t GEN2_READ = JSON_OBJECT_SIZE(24) + 2 * JSON_ARRAY_SIZE(8);

    // A sensors.json larger than this is damage, not a configuration
    constexpr size_t MAX_FILE_BYTES = 32768;

    // textBytes: the strings copied into the document (names, URLs, IDs)
    constexpr size_t writeCapacity(size_t gen1, size_t gen2, size_t textBytes)
    {
        return JSON_OBJECT_SIZE(2) + JSON_ARRAY_SIZE(gen1) + gen1 * GEN1_WRITE + JSON_ARRAY_SIZE(gen2) +
               gen2 * GEN2_WRITE + textBytes + 256;
    }

    // The file's strings are copied into the document, so its size follows
    // the file
    constexpr size_t readCapacity(size_t fileBytes, size_t maxGen1 = MAX_GEN1, size_t maxGen2 = MAX_GEN2)
    {
        return fileBytes + JSON_OBJECT_SIZE(2) + JSON_ARRAY_SIZE(maxGen1) + maxGen1 * GEN1_READ +
               JSON_ARRAY_SIZE(maxGen2) + maxGen2 * GEN2_READ + 256;
    }

    enum class Load
    {
        Ok,       // parsed into doc
        Damage,   // not valid JSON
        NoMemory, // no memory for the document, or more than twice the read budget
    };

    // Parses a sensors.json: with the read budget, and after NoMemory once
    // more with twice the document (another firmware version's larger file);
    // the first document is freed before the second is allocated. factor:
    // the budget multiple used (1 or 2)
    inline Load parse(const char *text, size_t len, std::unique_ptr<DynamicJsonDocument> &doc, size_t &factor)
    {
        size_t capacity = readCapacity(len);
        DeserializationError error = DeserializationError::NoMemory;
        for (factor = 1; factor <= 2; factor++)
        {
            doc.reset();
            doc.reset(new (std::nothrow) DynamicJsonDocument(factor * capacity));
            if (!doc || doc->capacity() == 0)
            {
                doc.reset();
                return Load::NoMemory;
            }
            error = deserializeJson(*doc, text, len);
            if (error != DeserializationError::NoMemory)
            {
                break;
            }
        }
        if (error == DeserializationError::NoMemory)
        {
            factor = 2;
            doc.reset();
            return Load::NoMemory;
        }
        if (error)
        {
            doc.reset();
            return Load::Damage;
        }
        return Load::Ok;
    }
}
