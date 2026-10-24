/**
 * expLORA Gateway
 *
 * VersionMarker - "EXPLORA_FW_VERSION=<FIRMWARE_VERSION>" kept in the
 * image, so tools/release/explora_sign.py can read the version a gateway
 * binary really carries (esp_app_desc_t.version holds the Arduino library
 * builder's id in these builds). Interim: once the build writes
 * FIRMWARE_VERSION into esp_app_desc_t (M4c S3), the tool uses that and
 * this marker can go. Referenced by the boot log, so the linker keeps it.
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

namespace VersionMarker
{
extern const char TEXT[]; // "EXPLORA_FW_VERSION=2.0.0"
constexpr size_t PREFIX_LEN = sizeof("EXPLORA_FW_VERSION=") - 1;
inline const char *version()
{
    return TEXT + PREFIX_LEN;
}
} // namespace VersionMarker
