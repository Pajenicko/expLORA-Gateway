/**
 * expLORA Gateway - protocol v2 milestone M0 timing spike (throw-away code)
 *
 * Built only in the `pro_spike` / `lite_spike` environments (spike/platformio.ini).
 * The production radio task replaces this in milestone M2a.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <Arduino.h>
#include "../../src/Data/Logging.h"
#include "../../src/Hardware/LoRaRadio.h"
#include "../../src/Hardware/SPI_Manager.h"

class SensorManager;

// Radio owned by the spike task; gen1 packets reach loop() through a queue
LoRaRadio *spikeCreateRadio(Logger &logger, SPIManager *spiManager);

// Serial console commands starting with "spike-" (see spike/README.md)
void spikeCommand(const String &line, SensorManager *sensors);

// Load generators driven from loop()
void spikeLoop(SensorManager *sensors);
