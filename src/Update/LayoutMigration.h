/**
 * expLORA Gateway
 *
 * LayoutMigration - the Lite's partition layout v1 (two 1984K app slots)
 * is rewritten in place to v2 (two 2752K slots) by firmware 2.0.1
 * (Update/PartitionTable.h has the layouts, the steps and their rules;
 * docs/protocol-v2.md section 15 the user-facing description).
 *
 * boot() runs in setup() after the filesystem and the logger and before
 * GatewayOta::begin(), the network, the radio and the web server: nothing
 * else writes the flash then. It reads the table at 0x8000 on every board
 * (text() for /diag/heap and the Firmware page) and, on the Lite only, does
 * the step that is due - which restarts the gateway. A running image that
 * is still being validated is never migrated: process() restarts the
 * gateway once it is validated and nothing would be interrupted (no update
 * session, no install or upload, no image waiting for a confirmation, no
 * sensor OTA job in Transfer..Confirming), and the step runs at that boot.
 *
 * The migration erases the previous firmware (the rollback image): after
 * it only the running firmware is in the flash. Power loss while the table
 * sector is rewritten (< 0.1 s) can leave the gateway unbootable
 * (recovery: a full image over USB); a reset at any other point resumes or
 * repeats the step.
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

#include <Arduino.h>

namespace LayoutMigration
{
// setup(): classify the table, do the due step (may restart)
void begin();

// loop(): restart for the migration once the running image is validated
// and the gateway is idle (Lite only; checked every few seconds)
void process();

// "v2 (2752K slots)", "v1 (1984K slots) - migrates after the validation", ...
const char *text();

// nullptr, or why the gateway must not restart now (any task): the v2 table
// write failed and the v1 table could not be written back either, so the
// sector at 0x8000 may not hold a valid table - the running firmware works
// from its copy in RAM, the next start may fail. Every restart the firmware
// itself decides (heap fragmentation, liveness, update mode, installs, the
// automatic updates, the web UI) is refused while it is set; a task
// watchdog or a crash still resets the chip. process() retries the
// write-back every 10 minutes (at most 36 times) and clears it once the v1
// table reads back intact
const char *restartRefusal();
} // namespace LayoutMigration
