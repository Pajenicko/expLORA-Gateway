/**
 * expLORA Gateway
 *
 * VersionMarker - see VersionMarker.h
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "VersionMarker.h"

#include "../config.h"

namespace VersionMarker
{
__attribute__((used)) const char TEXT[] = "EXPLORA_FW_VERSION=" FIRMWARE_VERSION;
}
