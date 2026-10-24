/**
 * expLORA Gateway
 *
 * GatewayOta - installs the gateway's own firmware (M4c S3): writing the
 * other app slot through esp_ota_* (the file is classified while it streams,
 * Update/GatewayImage.h), activation split from writing, the admin's
 * confirmation of an unsigned image, the install record
 * (/update/state.json, Update/UpdateState.h) and the validation of a new
 * firmware that lets the bootloader roll back a firmware that does not run.
 *
 * Writing: Writer::begin() / write() / finish() (one writer at a time - the
 * manual upload in the AsyncTCP task or a URL download in the loop task).
 * finish() checks the length, classifies the tail and verifies the image
 * (esp_ota_end); conclude() then activates a verified image
 * (esp_ota_set_boot_partition), keeps an unsigned one waiting for the
 * confirmation (POST /firmware/confirm, 10 min, bound to the image and the
 * login session) or refuses it (the slot's first sector is erased so a
 * refused image can never boot). A verified image older than the running
 * firmware waits for the confirmation too (a downgrade is never silent).
 * An image that waits and is dropped (expired, "Signed images only"
 * switched on, or left waiting across a restart) is erased the same way.
 * The slot stays claimed until conclude() has applied its decision, and
 * after an activation no other install starts until the restart. The caller
 * restarts after an activation.
 *
 * Validation: the precompiled bootloader has
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE; verifyRollbackLater() (main.cpp)
 * returns true, so an activated image starts as PENDING_VERIFY and is marked
 * valid by process() after 120 s of loop() with the web server up and the
 * liveness checks healthy. A reset before that (crash loop, watchdog,
 * liveness restart) makes the bootloader go back to the previous firmware,
 * which then logs "rolled back from vX". A restart the admin asks for in
 * the web UI marks the image valid first (beforeRestart()). No new install
 * starts while the running image waits for its validation (esp_ota_begin
 * refuses it; the reason names the seconds left); no update check either.
 * Marking the image valid is retried (bounded) if the otadata write fails.
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
#include <memory>

#include "../Data/Logging.h"
#include "GatewayImage.h"

#include <esp_partition.h>

namespace GatewayOta
{
// setup(), after the filesystem and the logger: the install record against
// the running image (rollback, new firmware waiting for validation)
void begin(Logger &logger);

// loop(), every pass: validation of a new firmware, expiry of an image
// waiting for its confirmation
void process(bool webUp, bool healthy);

// A deliberate restart (web UI): a firmware waiting for its validation is
// marked valid first (the admin reached its web UI)
void beforeRestart(const char *why);

// The running image waits for its validation; leftS = seconds left;
// failed = marking it valid failed MARK_TRIES times (the next restart rolls
// back)
bool validationPending(uint32_t *leftS = nullptr, bool *failed = nullptr);

// "" or why the running firmware refuses installs and update checks now
// (it waits for its validation)
String validationRefusal();

// Why no install can start now ("" = it can): the running firmware waits
// for its validation, another install writes the slot, or an image was
// activated and the gateway is about to restart
String installRefusal();

// "Signed images only" was switched on: an unsigned image waiting for its
// confirmation is dropped and erased
void signedOnlyOn();

struct Result;

class Writer
{
public:
    Writer();
    ~Writer(); // aborts an unfinished write
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    // "" or the reason. Drops an image that waits for a confirmation (its
    // slot is written again)
    String begin();
    // "" or the reason (the write is aborted then). The image header is held
    // until it is complete and checked against this chip
    String write(const uint8_t *data, size_t len);
    // The end: length, tail, signature, esp_ota_end(). Safe to call twice
    GatewayImage::Outcome finish(uint64_t announced);
    void abort();
    // Done with the slot (conclude() calls it): the next writer may start
    void release();
    bool active() const { return handle_ != 0; }
    uint64_t received() const { return received_; }
    const esp_partition_t *partition() const { return part_; }

    struct State; // hash and stream (heap, only while writing)

private:
    friend Result conclude(Writer &w, const GatewayImage::Outcome &o, uint64_t session, bool signedOnly,
                           const char *source);
    void releaseLocked(); // release() with the module lock held

    const esp_partition_t *part_ = nullptr;
    uint32_t handle_ = 0;
    uint64_t received_ = 0;
    uint8_t head_[GatewayImage::HEADER_SIZE] = {};
    size_t headLen_ = 0;
    GatewayImage::Outcome outcome_;
    bool haveOutcome_ = false;
    std::unique_ptr<State> st_;

    bool owns_ = false; // holds the install slot (one writer at a time)
    String writeFlash(const uint8_t *data, size_t len);
};

// What conclude() did
struct Result
{
    GatewayImage::Activation action = GatewayImage::Activation::Refuse;
    bool activated = false; // the boot partition points to the new image: restart
    String message;         // for the page / log
    char digestHex[65] = "";
};

// After a successful finish(): activate / wait for a confirmation / refuse.
// session: the login session that wrote it (WebAuth::sessionTag); source:
// "upload" / "URL" for the log
Result conclude(Writer &w, const GatewayImage::Outcome &o, uint64_t session, bool signedOnly, const char *source);

// A finished image that is not concluded (the upload turned out invalid):
// erased, never activated
void discard(Writer &w);

// POST /firmware/confirm: activates the waiting image. "" = activated
// (restart), else the reason
String confirm(const char *digestHex, uint64_t session, bool signedOnly);

// For the Firmware page
struct View
{
    String runningSig;      // how the running image was installed ("verified (key ..)", "unsigned", "unknown")
    bool validating = false;
    uint32_t validateLeftS = 0;
    bool validateFailed = false; // marking it valid failed: the next restart rolls back
    bool waiting = false;   // an image waits for a confirmation
    bool waitingMine = false; // ... from this session
    bool waitingDowngrade = false; // ... a verified older version (not an unsigned one)
    String waitingText;     // "unsigned, 1 830 144 B, SHA-256 ab12…"
    String waitingDigest;   // 64 hex
    uint32_t waitingLeftS = 0;
    String lastText;        // last install result
};
View view(uint64_t session);

// /diag/heap "update" object
void printDiagJson(Print &out);
} // namespace GatewayOta
