/**
 * expLORA Gateway
 *
 * AutoUpdate - the automatic updates (M4c S6), driven from loop(); the
 * decisions are in Update/AutoUpdatePolicy.h.
 *
 * Off by default: two switches on the Firmware page ("gateway", "sensors")
 * and the window (default 02-05 local). With a switch on and a valid clock
 * (SNTP synced in this run, last sync within 24 h), the gateway runs once
 * per local day inside the window, at the minute its MAC gives:
 * - one update-mode session (WebPortal::runScheduled): the manifest check;
 *   then, with "gateway" on, a newer gateway firmware the manifest offers
 *   (verified only - "Signed images only" does not matter here) unless it
 *   got three strikes (AutoUpdatePolicy.h: an attempt that reached the
 *   activation and was not validated - a crash or a rollback - a
 *   refused image or HTTP 404 / 410, or a download the gateway did not survive;
 *   one per night; other download failures are no strike) - it
 *   restarts the gateway; then, with "sensors" on, every newer sensor image
 *   the manifest offers for a HW type whose image no unfinished job uses
 *   (verified only), unless that version was refused on three nights (an
 *   image that is not stored or an HTTP 404 / 410; transport failures are no
 *   strike);
 * - the session starts only when no sensor job needs the radio (a Paused
 *   one does not), no start is queued, no upload / manual download / check
 *   runs, no gateway image waits for the admin's confirmation, the running
 *   firmware is validated and no restart is pending - otherwise it is
 *   deferred and tried again every 5 minutes inside the window (after a
 *   deferral by a sensor job up to 1 h past its end); a failed run is
 *   retried after 15 min, 30 min, 1 h ... (+-10 %) while the window lasts
 *   (the wait is saved before each run: a restart does not run it early).
 * With "sensors" on, the stored signed images are then installed one sensor
 * at a time, outside the update-mode session, in DevID round robin: a
 * sensor gets the image of its HW type when it is newer than its firmware,
 * did not revert on it before (by version), did not fail twice
 * automatically, and its start needs no confirmation and has no blocker.
 * Automatic jobs start at any time of day (the transfer is radio-paced, no
 * TLS), but never while a gateway update is pending (the sensor queue must
 * not starve the gateway) or a new gateway firmware is being validated,
 * never inside a window before its run and never
 * when the job's estimate would run into the next window (a job in Transfer
 * would block the nightly session). Manual actions always win: an
 * automatic job never aborts or replaces an admin's job, and starts only
 * when nothing else runs. An automatic job counts as a failure for its
 * sensor when it fails or the gateway aborts it; an abort by the admin does
 * not (that sensor is then left alone for 24 h), nor does a start refused
 * before the job existed (tried again after 30 min).
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

#include "AutoUpdatePolicy.h"

namespace AutoUpdate
{
// setup(), after GatewayOta::begin()
void begin();
// loop(), every pass (acts at most once a second)
void process();

// WebPortal::runScheduled(): nullptr = the gateway version may be
// installed automatically, else why not (" (...)", appended to the result):
// three strikes, or a strike tonight
const char *gatewayRefusal(uint32_t version);
// ... right before its download: marked on flash (a restart during the
// download counts as a strike at the next start, AutoUpdatePolicy.h)
void beforeGatewayDownload(uint32_t version);
// ... the image is downloaded, verified and matches the manifest, right
// before its activation: the strike is counted and the night marked done
// on flash (a restart into the new firmware does not run again tonight; a
// crash or a rollback keeps the strike, the validation clears it). false:
// it could not be saved - the install is refused
bool beforeGatewayActivation(uint32_t version);
// ... it ended without an activation
void gatewayInstallEnded(uint32_t version, AutoUpdatePolicy::GwEnd how);
// Sensor image fetch from the manifest: not after three nights with a
// refused image (transport failures are no strike)
bool sensorFetchWanted(uint16_t hw, uint32_t version);
void sensorFetched(uint16_t hw, uint32_t version, AutoUpdatePolicy::FetchEnd how);

// POST /firmware/auto saved new settings (logged, schedule recomputed by
// the loop task)
void settingsChanged(const AutoUpdatePolicy::Settings &s);

// For the Firmware page (any task)
struct View
{
    AutoUpdatePolicy::Settings s;
    bool clockOk = false;
    char next[24] = "";      // the next run, local time ("" = none planned)
    char last[UpdateState::RUN_TEXT_MAX] = ""; // the last run's result
    uint32_t lastAt = 0;
    char note[88] = "";      // deferred / waiting: why ("deferred: " + the longest refusal, 73)
    uint32_t pendingGw = 0;  // a newer gateway version waits (0 = none)
    uint32_t gwVer = 0;      // automatic install attempts of this version
    uint8_t gwFails = 0;
    uint32_t job = 0;        // DevID of the automatic sensor job running
    uint32_t nextSensor = 0; // DevID the next automatic job is for
    uint32_t cursor = 0;     // DevID of the last automatic start (round robin)
    uint8_t queue = 0;       // sensors waiting for an automatic update
    char sensorNote[64] = "";
};
View view();

// /diag/heap: ,"auto":{...} inside the "update" object
void printDiagJson(Print &out);
} // namespace AutoUpdate
