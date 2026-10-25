/**
 * expLORA Gateway
 *
 * UpdateCheck - "Check now" (M4c S5): the gateway itself checks the signed
 * package manifest (Update/Manifest.h) and decides what is offered. The
 * web server downloads the file in an update-mode session (loop task) and
 * hands it to evaluate(): signature, JSON rules, replay floor (the highest
 * accepted seq, /update/state.json via GatewayOta), then the result is kept
 * in RAM for the Firmware page, the status JSON and the installs from the
 * manifest; the result text and time are stored with the seq.
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
#include <mutex>

#include "Manifest.h"

namespace UpdateCheck
{
// Loop task: a downloaded manifest. true = accepted;
// text = the result (also logged and recorded)
bool evaluate(const char *file, size_t len, String &text);
// Loop task: the check failed before there was a manifest to evaluate
// (also logged and recorded); the offers of an earlier check are dropped
void failed(const String &text);

// Any task: the accepted manifest of this run (nullptr = none) under the
// lock - copy what is needed, do not keep the pointer
std::mutex &mutex();
const Manifest::Doc *docLocked();

// The offer for the running firmware (Offer::None without a manifest)
Manifest::Offer gatewayOfferLocked(const Manifest::Doc *d);
// The offer for a HW type against the stored image (reads the store)
Manifest::Offer sensorOfferLocked(const Manifest::Doc *d, uint16_t hw, uint32_t *storedFw = nullptr,
                                  bool *stored = nullptr);

// GET /firmware/check: {"state":..,"at":..,"seq":..,"text":..} (state:
// "idle", "queued", "checking"; the offers are on the Firmware page)
void printStatus(Print &out, const char *state);
} // namespace UpdateCheck
