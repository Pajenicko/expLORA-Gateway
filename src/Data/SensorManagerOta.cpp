/**
 * expLORA Gateway
 *
 * Sensor manager: OTA jobs for generation 2 sensors (protocol v2,
 * docs/protocol-v2.md 14.5). The pure state machine is Protocol/OtaJob.h;
 * this file feeds it (UPs from handleGen2Frame, admin actions, launch
 * outcomes, the clock), persists it in /ota/job.json, reads the blocks from
 * the image store and hands OTA frames to the radio task.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "SensorManager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_timer.h>
#include <sys/time.h>

#include "DueTime.h"
#include "../Hardware/FlashPermit.h"
#include "../Protocol/Gen2Crypto.h"
#include "../Protocol/Gen2Downlink.h"
#include "../Storage/OtaStore.h"
#include "../Protocol/OtaImageSigned.h"
#include "../Storage/ConfigManager.h"
#include "../Update/UpdateMode.h"

extern ConfigManager *configManager;

static const char *const OTA_JOB_FILE = "/ota/job.json";
static constexpr uint32_t OTA_SAVE_DELAY_MS = 1500;   // after the response that changed it
static constexpr int64_t OTA_BUILD_LEAD_US = 700000;  // build at most this long before the slot
static constexpr int64_t OTA_MIN_LEAD_US = 300000;    // ... and at least this long (spec 14.5)
static constexpr uint32_t VALID_UNIX = 1700000000;

using OtaJob::State;

static String devIdText(uint32_t devId)
{
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)devId);
    return id;
}

static String versionText(uint32_t fw)
{
    char v[16];
    OtaImage::versionText(fw, v, sizeof(v));
    return v;
}

OtaJob::Now SensorManager::otaNow() const
{
    OtaJob::Now n;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    n.unixS = tv.tv_sec >= VALID_UNIX ? (uint32_t)tv.tv_sec : 0;
    n.us = esp_timer_get_time();
    n.uptimeS = (uint32_t)(n.us / 1000000);
    return n;
}

void SensorManager::otaScheduleSaveLocked(unsigned long nowMs)
{
    otaSaveDue = true;
    otaSaveAtMs = (uint32_t)nowMs + OTA_SAVE_DELAY_MS;
}

OtaJob::Job &SensorManager::otaJobLocked()
{
    if (!otaJobPtr)
    {
        otaJobPtr.reset(new OtaJob::Job());
    }
    return *otaJobPtr;
}

void SensorManager::otaDropIfGoneLocked()
{
    if (otaJobPtr && otaJobPtr->state == State::None && !otaJobPtr->ghost && !otaJobPtr->abortPending)
    {
        otaJobPtr.reset();
        otaFrameOut = false;
    }
}

// --- job.json -----------------------------------------------------------------------------

static void putDeadline(JsonObject o, const char *key, const OtaJob::Deadline &d)
{
    if (!d.armed())
    {
        return;
    }
    JsonArray a = o.createNestedArray(key);
    a.add(d.kind);
    a.add(d.at);
    a.add(d.durS);
}

static void getDeadline(JsonObject o, const char *key, OtaJob::Deadline &d)
{
    JsonArray a = o[key];
    d.clear();
    if (a.isNull() || a.size() != 3)
    {
        return;
    }
    d.kind = a[0] | 0u;
    d.at = a[1] | 0u;
    d.durS = a[2] | 0u;
}

bool SensorManager::otaSaveJob()
{
    DynamicJsonDocument doc(2048);
    bool none = false;
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        none = !otaJobPtr;
    }
    if (none)
    {
        FlashWriteGuard guard;
        LittleFS.remove(OTA_JOB_FILE);
        LittleFS.remove("/ota/job.tmp"); // a stale one would come back at boot
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        if (!otaJobPtr)
        {
            return true;
        }
        const OtaJob::Job &j = *otaJobPtr;
        JsonObject o = doc.to<JsonObject>();
        o["v"] = 1;
        o["state"] = (int)j.state;
        o["dev"] = j.devId;
        o["hw"] = j.hwType;
        o["fw"] = j.fw;
        o["crc"] = j.crc;
        o["len"] = j.len;
        o["baseFw"] = j.baseFw;
        o["baseBoot"] = j.baseBoot;
        o["baseFlags"] = j.baseFlags;
        o["baseRev"] = j.baseFlaggedRevert;
        o["interval"] = j.intervalS;
        o["k"] = j.k;
        o["c"] = j.cadence100ms;
        o["delay"] = j.startDelayMs;
        o["bindValid"] = j.bindValid;
        o["bind"] = j.bindFcnt;
        o["nonce"] = j.sessionNonce;
        o["manualPause"] = j.manualPause;
        o["abortPending"] = j.abortPending;
        o["kept"] = j.kept;
        o["despiteAbort"] = j.despiteAbort;
        o["abortQuiet"] = j.abortQuiet;
        o["abortKept"] = j.abortKept;
        o["swapSeen"] = j.swapSeen;
        o["ghost"] = j.ghost;
        o["max"] = j.maxConfirmed;
        o["noProgress"] = j.noProgress;
        o["laps"] = j.laps;
        o["progressSinceAccept"] = j.progressSinceAccept;
        o["backoff"] = j.backoffS;
        o["why"] = (int)j.why;
        o["whyArg"] = j.whyArg;
        o["pausedOnce"] = j.pausedOnce;
        putDeadline(o, "dlState", j.dlState);
        putDeadline(o, "dlPaused", j.dlPaused);
        putDeadline(o, "dlBackoff", j.dlBackoff);
        putDeadline(o, "dlBattery", j.dlBattery);
        putDeadline(o, "dlAbort", j.dlAbortPending);
        putDeadline(o, "dlRevert", j.dlRevertWatch);
        o["sent"] = j.blocksSent;
        o["repeats"] = j.repeats;
        o["statuses"] = j.statuses;
        o["missed"] = j.missedTotal;
        o["rounds"] = j.rounds;
        o["air"] = j.airMs;
        o["skipped"] = j.skipped;
        o["startValid"] = j.startValid;
        o["startNonce"] = j.startNonce;
        o["expectBind"] = j.expectBind;
        o["startsArmed"] = j.startsArmed;
        o["shrinks"] = j.shrinks;
        o["started"] = j.startedUnix;
        o["lastStatus"] = j.lastStatusUnix;
    }
    if (doc.overflowed())
    {
        logger.error("OTA job does not fit its JSON document - not saved");
        return false;
    }
    FlashWriteGuard guard;
    File f = LittleFS.open("/ota/job.tmp", "w");
    if (!f)
    {
        logger.error("Cannot write /ota/job.tmp");
        return false;
    }
    size_t n = serializeJson(doc, f);
    f.close();
    if (n == 0)
    {
        LittleFS.remove("/ota/job.tmp");
        return false;
    }
    // LittleFS renames over an existing file atomically: there is never a
    // moment without a job.json (a leftover job.tmp is taken at boot)
    return LittleFS.rename("/ota/job.tmp", OTA_JOB_FILE);
}

void SensorManager::otaBegin()
{
    OtaStore::begin(logger);
    DynamicJsonDocument doc(2048);
    bool loaded = false;
    {
        FlashWriteGuard guard;
        if (!LittleFS.exists(OTA_JOB_FILE) && LittleFS.exists("/ota/job.tmp"))
        {
            LittleFS.rename("/ota/job.tmp", OTA_JOB_FILE); // a save cut between write and rename
        }
        File f = LittleFS.open(OTA_JOB_FILE, "r");
        if (f)
        {
            DeserializationError e = deserializeJson(doc, f);
            f.close();
            loaded = !e;
            if (e)
            {
                logger.error(String("OTA job file unreadable (") + e.c_str() + ") - the job is lost");
            }
        }
    }
    if (!loaded)
    {
        return;
    }
    OtaJob::Now n = otaNow();
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        OtaJob::Job &j = otaJobLocked();
        JsonObject o = doc.as<JsonObject>();
        j = OtaJob::Job();
        j.state = (State)(o["state"] | 0u);
        j.devId = o["dev"] | 0u;
        j.hwType = o["hw"] | 0u;
        j.fw = o["fw"] | 0u;
        j.crc = o["crc"] | 0u;
        j.len = o["len"] | 0u;
        j.baseFw = o["baseFw"] | 0u;
        j.baseBoot = o["baseBoot"] | 0xFF;
        j.baseFlags = o["baseFlags"] | 0u;
        j.baseFlaggedRevert = o["baseRev"] | false;
        j.intervalS = o["interval"] | 600;
        j.k = o["k"] | ep::ota::kMinK;
        j.cadence100ms = o["c"] | 260;
        j.startDelayMs = o["delay"] | 4000;
        j.bindValid = o["bindValid"] | false;
        j.bindFcnt = o["bind"] | 0u;
        j.sessionNonce = o["nonce"] | 0u;
        j.manualPause = o["manualPause"] | false;
        j.abortPending = o["abortPending"] | false;
        j.kept = o["kept"] | false;
        j.despiteAbort = o["despiteAbort"] | false;
        j.abortQuiet = o["abortQuiet"] | false;
        j.abortKept = o["abortKept"] | false;
        j.swapSeen = o["swapSeen"] | false;
        j.ghost = o["ghost"] | false;
        j.maxConfirmed = o["max"] | 0u;
        j.noProgress = o["noProgress"] | 0u;
        j.laps = o["laps"] | 0u;
        j.progressSinceAccept = o["progressSinceAccept"] | false;
        j.backoffS = o["backoff"] | 0u;
        j.why = (OtaJob::Why)(o["why"] | 0u);
        j.whyArg = o["whyArg"] | 0u;
        j.pausedOnce = o["pausedOnce"] | false;
        getDeadline(o, "dlState", j.dlState);
        getDeadline(o, "dlPaused", j.dlPaused);
        getDeadline(o, "dlBackoff", j.dlBackoff);
        getDeadline(o, "dlBattery", j.dlBattery);
        getDeadline(o, "dlAbort", j.dlAbortPending);
        getDeadline(o, "dlRevert", j.dlRevertWatch);
        j.blocksSent = o["sent"] | 0u;
        j.repeats = o["repeats"] | 0u;
        j.statuses = o["statuses"] | 0u;
        j.missedTotal = o["missed"] | 0u;
        j.rounds = o["rounds"] | 0u;
        j.airMs = o["air"] | 0u;
        j.skipped = o["skipped"] | 0u;
        j.startValid = o["startValid"] | false;
        j.startNonce = o["startNonce"] | 0u;
        j.expectBind = o["expectBind"] | 0u;
        j.startsArmed = o["startsArmed"] | 0u;
        j.shrinks = o["shrinks"] | 0u;
        j.startedUnix = o["started"] | 0u;
        j.lastStatusUnix = o["lastStatus"] | 0u;
        if ((int)j.state > (int)State::Reverted || j.len > ep::ota::kSlotSize)
        {
            j = OtaJob::Job();
        }
        OtaJob::afterReboot(j, n);
        if (j.state == State::None && !j.ghost)
        {
            otaJobPtr.reset();
            return;
        }
        logger.info("OTA job restored: sensor " + devIdText(j.devId) + ", v" + versionText(j.fw) + ", " +
                    OtaJob::stateName(j.state));
        otaScheduleSaveLocked(millis());
    }
    // Whether the restored job's image is verified (RAM only: the key table
    // of this firmware decides, OtaStore::get applies it). Flash read
    // without the sensor lock; the image cannot change while the job uses it
    uint16_t jobHw = 0;
    uint32_t jobCrc = 0, jobLen = 0;
    bool restored = false;
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        if (otaJobPtr && !OtaJob::terminal(otaJobPtr->state))
        {
            restored = true;
            jobHw = otaJobPtr->hwType;
            jobCrc = otaJobPtr->crc;
            jobLen = otaJobPtr->len;
        }
    }
    if (restored)
    {
        OtaStore::Meta meta;
        const bool stored = OtaStore::get(jobHw, meta);
        const bool verified = !OtaImageSigned::jobHeld(true, stored, meta.crc, meta.len, meta.signedKey, jobCrc, jobLen);
        std::lock_guard<std::mutex> lock(sensorMutex);
        if (otaJobPtr && otaJobPtr->crc == jobCrc)
        {
            otaJobPtr->imageVerified = verified;
        }
    }
    // "Signed images only" holds for a restored job too: one with an
    // unsigned image does not go on by itself
    otaEnforceSignedOnly("after the restart (\"Signed images only\" is on)");
}

// --- UPs of generation 2 sensors (handleGen2Frame, sensorMutex held) ---------------------

void SensorManager::otaApplyLocked(Gen2Sensor &g, const OtaJob::Result &r, const OtaJob::Up &u,
                                   unsigned long nowMs)
{
    (void)u;
    if (!otaJobPtr)
    {
        return;
    }
    const OtaJob::Job &otaJob = *otaJobPtr;
    if (r.changed)
    {
        otaScheduleSaveLocked(nowMs);
    }
    if (r.revertRecord)
    {
        g.hasReverted = true;
        g.revertedFw = otaJob.fw;
        g.revertedCrc = otaJob.crc;
        scheduleGen2SaveLocked(nowMs);
        logger.warning("Generation 2 sensor " + g.name + ": the update to v" + versionText(otaJob.fw) +
                       " was reverted - it is not retried automatically");
    }
    if (r.log)
    {
        String text = String("OTA ") + g.name + " v" + versionText(otaJob.fw) + ": " + OtaJob::stateName(otaJob.state);
        const char *why = OtaJob::whyText(otaJob.why);
        if (*why)
        {
            text += String(" (") + why;
            if (otaJob.why == OtaJob::Why::Refused)
            {
                text += String(": ") + OtaJob::refusalText(otaJob.whyArg);
            }
            else if (otaJob.why == OtaJob::Why::SensorFailed)
            {
                text += String(": ") + OtaJob::failureText(otaJob.whyArg);
            }
            else if (otaJob.why == OtaJob::Why::Left)
            {
                text += String(": ") + OtaJob::leaveText(otaJob.whyArg);
            }
            text += ")";
        }
        if (otaJob.state == State::Done && otaJob.kept)
        {
            text += " - installed, unconfirmed, cannot be reverted (KEPT)";
        }
        if (otaJob.state == State::Done && otaJob.despiteAbort)
        {
            text += " - installed despite abort";
        }
        otaLastEvent = text;
        logger.info(text);
    }
    if (r.abortKept)
    {
        // The abort ended unanswered (an UP with bit 4 = 0, or abortPending expired)
        otaLastEvent = "OTA " + g.name + ": the sensor did not confirm the abort - its transfer state may stay, "
                       "and a job with the same image would resume it";
        logger.info(otaLastEvent);
    }
}

OtaJob::Down SensorManager::otaOnUpLocked(Gen2Sensor &g, OtaJob::Up &u, unsigned long nowMs)
{
    u.curFw = g.hasFw ? g.fwVersion() : 0;
    if (!otaJobPtr || otaJobPtr->devId != g.devId)
    {
        // No job for this sensor. Leftovers of its own finished job (parked
        // when a job for another sensor started): the revert watch and a
        // pending abort; an UP in an OTA session is an orphan
        const OtaJob::Now n = otaNow();
        if (g.otaWatch.armed() && g.otaWatch.reached(n))
        {
            g.otaWatch.clear();
            scheduleGen2SaveLocked(nowMs);
        }
        if (g.otaAbort.armed() && (g.otaAbort.reached(n) || (!u.joinReq && !u.bit4) ||
                                   (u.left && u.leftArg == ep::ota::kLeaveEnd)))
        {
            g.otaAbort.clear();
            scheduleGen2SaveLocked(nowMs);
        }
        if (g.otaWatch.armed() && u.hasStatus && OtaJob::flaggedRevert(u.boot, u.flags))
        {
            g.hasReverted = true;
            g.revertedFw = g.otaWatchFw;
            g.revertedCrc = g.otaWatchCrc;
            g.otaWatch.clear();
            scheduleGen2SaveLocked(nowMs);
            logger.warning("Generation 2 sensor " + g.name + ": the update to v" + versionText(g.revertedFw) +
                           " was reverted - it is not retried automatically");
        }
        if (!u.bit4 || u.joinReq)
        {
            return OtaJob::Down::None;
        }
        return g.otaAbort.armed() ? OtaJob::Down::EndAbort : OtaJob::Down::EndPause;
    }
    // A Verified job of an image that is not verified does not transfer
    // again while "Signed images only" is on (OtaJob::holdBeforeTransfer)
    u.signedOnly = configManager && configManager->signedOnly;
    const bool wasHeld = otaJobPtr->manualPause && otaJobPtr->why == OtaJob::Why::Unsigned;
    OtaJob::Result r = OtaJob::onUp(*otaJobPtr, u, otaNow());
    if (!wasHeld && otaJobPtr->manualPause && otaJobPtr->why == OtaJob::Why::Unsigned)
    {
        logger.warning("OTA " + g.name + ": the sensor would need the image again - held Paused, its image is "
                       "unsigned and \"Signed images only\" is on; abort the job, or switch the option off "
                       "(Firmware page, Advanced mode) and resume");
    }
    if (r.bindMismatch)
    {
        logger.warning("OTA " + g.name + ": accepting UP FCnt " + String(u.fcnt32) + ", expected " +
                       String(otaJobPtr->expectBind) + " - binding to the actual one");
    }
    if (r.adopted)
    {
        logger.info("OTA " + g.name + ": accepting UP of the start was lost - the status takes over its binding");
    }
    otaApplyLocked(g, r, u, nowMs);
    otaDropIfGoneLocked();
    return r.down;
}

// --- loop --------------------------------------------------------------------------------

void SensorManager::otaRunStart()
{
    // The radio is in standby during an update-mode session: a start queued
    // meanwhile waits for its end (otaStartPending stays set)
    if (UpdateMode::active())
    {
        return;
    }
    uint32_t devId = 0;
    OtaJob::Target t;
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        // otaStartPending stays set until the job exists: the image cannot be
        // replaced or deleted while it is verified (otaImageBusy)
        if (!otaStartPending)
        {
            return;
        }
        devId = otaStartDevId;
        int i = findGen2Locked(devId);
        if (i < 0)
        {
            otaStartPending = false;
            return;
        }
        const Gen2Sensor &g = gen2[i];
        t.devId = devId;
        t.hwType = g.hwType;
        t.fw = g.fwVersion();
        t.boot = g.bootState;
        t.flags = g.statusFlags;
        t.intervalS = g.config.intervalS;
    }
    OtaStore::Meta meta;
    const bool stored = OtaStore::get(t.hwType, meta);
    if (!stored || !OtaStore::verify(t.hwType, meta))
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        otaStartPending = false;
        otaLastEvent = "OTA start of " + devIdText(devId) + " refused: " +
                       (stored ? "the stored image failed its CRC check" : "no complete image stored for its HW type");
        logger.error(otaLastEvent);
        return;
    }
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (!otaStartPending || otaStartDevId != devId)
    {
        return; // dropped meanwhile (otaEnforceSignedOnly)
    }
    otaStartPending = false;
    if (otaJobPtr && !OtaJob::terminal(otaJobPtr->state))
    {
        return; // another job started meanwhile
    }
    // "Signed images only" may have been switched on since the start was
    // requested (the request checked it, the job is created only now)
    if (OtaImageSigned::startRule(meta.signedKey, configManager && configManager->signedOnly) ==
        OtaImageSigned::StartRule::Block)
    {
        otaLastEvent = "OTA start of " + devIdText(devId) +
                       " refused: unsigned image - \"Signed images only\" is on (Firmware page, Advanced mode)";
        logger.warning(otaLastEvent);
        return;
    }
    // The previous (finished) job's revert watch and pending abort move to its
    // sensor's record (a new job for that sensor overwrites the abort)
    if (otaJobPtr)
    {
        const OtaJob::Job &old = *otaJobPtr;
        const OtaJob::Now n = otaNow();
        int oi = findGen2Locked(old.devId);
        if (oi >= 0)
        {
            Gen2Sensor &og = gen2[oi];
            if (old.dlRevertWatch.armed() && !old.dlRevertWatch.reached(n))
            {
                og.otaWatch = old.dlRevertWatch;
                og.otaWatchFw = old.fw;
                og.otaWatchCrc = old.crc;
                scheduleGen2SaveLocked(millis());
            }
            if (old.abortPending && old.devId != devId)
            {
                og.otaAbort = old.dlAbortPending;
                scheduleGen2SaveLocked(millis());
            }
        }
    }
    if (int ni = findGen2Locked(devId); ni >= 0 && (gen2[ni].otaAbort.armed() || gen2[ni].otaWatch.armed()))
    {
        // the sensor falls under the new job (its revert rules and abort)
        gen2[ni].otaAbort.clear();
        gen2[ni].otaWatch.clear();
        scheduleGen2SaveLocked(millis());
    }
    OtaJob::Job &otaJob = otaJobLocked();
    OtaJob::Image img;
    img.fw = meta.fw;
    img.crc = meta.crc;
    img.len = meta.len;
    OtaJob::begin(otaJob, t, img, otaNow());
    otaJob.imageVerified = meta.signedKey != 0;
#ifdef OTA_TEST_K64
    // Bench build only: the longest round the sensor accepts (K = 64, C = 46 s)
    otaJob.k = 64;
    otaJob.cadence100ms = 460;
    otaJob.dlState.arm(otaNow(), OtaJob::waitingDeadlineS(otaJob));
#endif
    otaFrameOut = false;
    otaLastSlot = UINT32_MAX;
    otaLastEvent = "OTA " + devIdText(devId) + ": v" + versionText(t.fw) + " -> v" + versionText(meta.fw) +
                   " queued (the start goes with the sensor's next report)";
    logger.info(otaLastEvent);
    otaScheduleSaveLocked(millis());
}

void SensorManager::otaBuildFrame(unsigned long nowMs)
{
    (void)nowMs;
    uint32_t slot = 0;
    bool end = false;
    bool abortEnd = false;
    uint16_t index = 0;
    uint16_t hw = 0;
    uint32_t offset = 0;
    uint16_t blockLen = 0;
    int64_t slotUs = 0;
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        if (!otaJobPtr)
        {
            return;
        }
        OtaJob::Job &j = *otaJobPtr;
        const int64_t nowUs = esp_timer_get_time();
        if (otaFrameOut)
        {
            // A frame whose session ended meanwhile is withdrawn
            bool live = j.state == State::Transfer || j.pendingEnd != OtaJob::PendingEnd::None;
            if (!live)
            {
                RadioTask::otaCancel(otaFrameSeq);
            }
            return;
        }
        if (j.anchorUs != otaLastAnchorUs)
        {
            otaLastAnchorUs = j.anchorUs; // a new anchor (accepting UP or a status): numbering may restart
            otaLastSlot = UINT32_MAX;
        }
        if (!OtaJob::nextBlockSlot(j, nowUs + OTA_MIN_LEAD_US, slot))
        {
            return;
        }
        slotUs = j.slotUs(slot);
        if (slotUs - nowUs > OTA_BUILD_LEAD_US || slot == otaLastSlot)
        {
            return; // not yet / already handled
        }
        // A slot missed by loop() (stall) is just skipped
        if (otaLastSlot != UINT32_MAX && slot > otaLastSlot + 1 && !ep::ota::isStatusSlot(otaLastSlot + 1, j.k))
        {
            otaSkipLoop += slot - otaLastSlot - 1;
            j.skipped += slot - otaLastSlot - 1;
        }
        otaLastSlot = slot;
        int ti = findGen2Locked(j.devId);
        if (ti < 0)
        {
            return;
        }
        // Never under a session other than the one the accepting UP verified under
        const Gen2Session &s = gen2[ti].sessions.current;
        if (!s.valid || s.devNonce != j.sessionNonce || !j.bindValid)
        {
            otaSkipSession++;
            return;
        }
        // Another generation 2 sensor's uplink slot (with a valid clock)
        // (off without a valid clock: the sensors' slots are then unknown)
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        if (tv.tv_sec >= VALID_UNIX)
        {
            const uint64_t launchUnixMs =
                (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000 + (uint64_t)((slotUs - nowUs) / 1000);
            const uint32_t airMs = ep::ota::airtimeUs(ep::ota::kBlockFrameLen, 9) / 1000 + 1;
            for (size_t i = 0; i < gen2Count; i++)
            {
                const Gen2Sensor &o = gen2[i];
                if (o.devId == j.devId || !o.sessionUp)
                {
                    continue;
                }
                if (Gen2Downlink::hitsUplinkSlot(launchUnixMs, airMs, o.config.intervalS, o.config.slotS))
                {
                    otaSkipUplink++;
                    OtaJob::skippedSlot(j);
                    return;
                }
            }
        }
        if (j.pendingEnd != OtaJob::PendingEnd::None)
        {
            end = true;
            abortEnd = j.pendingEnd == OtaJob::PendingEnd::Abort;
        }
        else if (!OtaJob::chooseBlock(j, index))
        {
            return;
        }
        hw = j.hwType;
        offset = ep::ota::blockOffset(index, ep::ota::kBlockSize);
        blockLen = ep::ota::blockLength(index, j.len, ep::ota::kBlockSize);
    }
    // The block from the image store (flash permit, outside sensorMutex)
    uint8_t data[ep::ota::kBlockSize];
    if (!end && (blockLen == 0 || !OtaStore::read(hw, offset, data, blockLen)))
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        otaSkipRead++;
        if (otaJobPtr)
        {
            OtaJob::skippedSlot(*otaJobPtr);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        if (!otaJobPtr)
        {
            return;
        }
        OtaJob::Job &j = *otaJobPtr;
        int ti = findGen2Locked(j.devId);
        if (ti < 0 || otaFrameOut || otaLastSlot != slot)
        {
            return;
        }
        Gen2Session &s = gen2[ti].sessions.current;
        if (!s.valid || s.devNonce != j.sessionNonce)
        {
            return;
        }
        OtaFrame *frame = RadioTask::otaFrameSlot(); // the task's static slot
        if (frame == nullptr)
        {
            otaSkipLoop++;
            return;
        }
        uint8_t payload[ep::kMaxPayloadOta];
        ep::TlvWriter w(payload, sizeof(payload));
        if (end)
        {
            const uint8_t code = abortEnd ? (uint8_t)ep::ota::EndCode::Abort : (uint8_t)ep::ota::EndCode::Pause;
            w.add(ep::down::kOtaEnd, 0, &code, 1);
        }
        else
        {
            uint8_t value[2 + ep::ota::kBlockSize];
            size_t vlen = ep::ota::encodeBlock(index, data, blockLen, value, sizeof(value));
            w.add(ep::down::kOtaBlock, 0, value, vlen);
        }
        // An OTA session DOWN burns a downlink counter even when its launch is skipped
        size_t len = ep::buildData(Gen2Crypto::aes, s.keys, ep::FrameType::Down, j.devId, s.fcntDown++, j.bindFcnt,
                                   ep::kFctrlOtaSession, payload, w.size(), frame->frame, sizeof(frame->frame));
        ep::wipe(payload, sizeof(payload));
        if (len == 0)
        {
            return;
        }
        frame->seq = ++otaSeq == 0 ? ++otaSeq : otaSeq;
        frame->devId = j.devId;
        frame->slotUs = slotUs;
        frame->kind = end ? OtaKind::End : OtaKind::Block;
        frame->index = index;
        frame->len = (uint8_t)len;
        otaFrameOut = true;
        otaFrameSeq = frame->seq;
        RadioTask::otaSubmit();
        otaFrameSlot = slot;
        otaFrameIndex = index;
        otaFrameEnd = end;
    }
    ep::wipe(data, sizeof(data));
}

void SensorManager::otaProcess(unsigned long nowMs)
{
    otaRunStart();
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        const bool keptBefore = otaJobPtr && otaJobPtr->abortKept;
        if (otaJobPtr && OtaJob::tick(*otaJobPtr, otaNow()))
        {
            otaScheduleSaveLocked(nowMs);
            int i = findGen2Locked(otaJobPtr->devId);
            if (i >= 0)
            {
                OtaJob::Result r;
                r.log = true;
                r.abortKept = otaJobPtr->abortKept && !keptBefore; // abortPending expired unanswered
                otaApplyLocked(gen2[i], r, OtaJob::Up(), nowMs);
            }
            otaDropIfGoneLocked();
        }
    }
    otaBuildFrame(nowMs);
    bool save = false;
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        if (otaSaveDue && DueTime::reached((uint32_t)nowMs, otaSaveAtMs))
        {
            otaSaveDue = false;
            save = true;
        }
    }
    const uint32_t saveT0 = micros();
    const bool saved = !save || otaSaveJob();
    if (save)
    {
        const uint32_t us = micros() - saveT0;
        std::lock_guard<std::mutex> lock(sensorMutex);
        otaSaveMaxUs = us > otaSaveMaxUs ? us : otaSaveMaxUs;
    }
    if (!saved)
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        otaSaveDue = true;
        otaSaveAtMs = (uint32_t)nowMs + 60000;
        logger.error("OTA job not saved, retrying in 60 s");
    }
}

void SensorManager::otaResult(const OtaResult &res)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (!otaFrameOut || res.seq != otaFrameSeq)
    {
        return;
    }
    otaFrameOut = false;
    if (!otaJobPtr)
    {
        return;
    }
    OtaJob::Job &otaJob = *otaJobPtr;
    if (res.outcome == OtaOutcome::Sent)
    {
        OtaJob::launched(otaJob, res.kind == OtaKind::Block, res.index, res.airtimeMs, otaNow());
        if (res.kind == OtaKind::End)
        {
            otaJob.pendingEnd = OtaJob::PendingEnd::None;
            otaScheduleSaveLocked(millis());
        }
        return;
    }
    if ((size_t)res.outcome < sizeof(otaSkipRadio) / sizeof(otaSkipRadio[0]))
    {
        otaSkipRadio[(size_t)res.outcome]++;
    }
    if (res.kind == OtaKind::Block)
    {
        OtaJob::skippedSlot(otaJob);
    }
}

// --- web --------------------------------------------------------------------------------

SensorManager::OtaCheck SensorManager::otaCheckStart(uint32_t devId, bool requestStatus)
{
    OtaCheck c;
    uint16_t hw = 0;
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        int i = findGen2Locked(devId);
        if (i < 0)
        {
            c.blockers = "unknown sensor";
            return c;
        }
        hw = gen2[i].hasFw ? gen2[i].hwType : 0;
        if (!gen2[i].hasFw)
        {
            c.blockers += "No JOIN_REQ seen yet (HW type and firmware unknown). ";
        }
    }
    OtaStore::Meta meta;
    if (c.blockers.length() == 0 && !OtaStore::get(hw, meta))
    {
        char t[8];
        snprintf(t, sizeof(t), "%04X", hw);
        c.blockers += String("No image for HW type 0x") + t + " uploaded. ";
    }
    std::lock_guard<std::mutex> lock(sensorMutex);
    int i = findGen2Locked(devId);
    if (i < 0)
    {
        c.blockers = "unknown sensor";
        return c;
    }
    Gen2Sensor &g = gen2[i];
    if (meta.valid)
    {
        c.imageOk = true;
        c.imageFw = meta.fw;
        c.imageLen = meta.len;
        c.imageCrc = meta.crc;
        const uint16_t blocks = ep::ota::blockCount(meta.len, ep::ota::kBlockSize);
        const uint16_t cad = ep::ota::cadenceFor(ep::ota::airtimeUs(ep::ota::kBlockFrameLen, 9), ep::ota::kMinK);
        // blocks x C x (K+1)/K, + the wait for the start (one interval) and the swap / confirmation (~25 min)
        c.estimateS = (uint32_t)((uint64_t)blocks * cad / 10 * (ep::ota::kMinK + 1) / ep::ota::kMinK) +
                      g.config.intervalS + 1500;
    }
    if (((g.caps >> ep::kCapOtaShift) & 3) < 1)
    {
        c.blockers += "The sensor's firmware has no OTA receiver (JOIN_REQ caps). ";
    }
    if (g.statusLen < 24)
    {
        if (requestStatus)
        {
            g.statusRequested = true; // the start is possible when the report arrives
            c.blockers += "No status report yet - requested, try again after the sensor's next report. ";
        }
        else
        {
            c.blockers += "No status report yet - \"Update\" requests one. ";
        }
    }
    else
    {
        if (g.statusFlags & 0x04)
        {
            c.blockers += "RTC not usable or time base failed - the sensor cannot confirm an update. If its "
                          "time base failed, it resets and rejoins by itself: try again after its next report. "
                          "If the flag persists, the RTC is not usable: power-cycle the sensor; if that does "
                          "not help, check the 32.768 kHz crystal. ";
        }
        if (g.bootState == 1)
        {
            c.blockers += "An unconfirmed image is running - wait for its confirmation. ";
        }
        if (g.statusFlags & 0x02)
        {
            c.warnings += "IWDG off on the sensor. ";
        }
    }
    if ((otaJobPtr && !OtaJob::terminal(otaJobPtr->state)) || otaStartPending)
    {
        c.blockers += "Another OTA job is running on this gateway (one at a time). ";
    }
    Gen2Link::Margins m = Gen2Link::margins(g.link, gen2LinkNowS(), g.effectiveTxPower());
    if ((m.upValid() && m.upMargin() < 10) || (m.downValid() && m.downMargin() < 10))
    {
        c.warnings += "Link margin below 10 dB - expect many repeats. ";
    }
    if (g.config.intervalS > 3600)
    {
        c.warnings += "Report interval above 1 h - the start and every resume wait for the sensor's next report. ";
    }
    if (meta.valid && g.hasFw)
    {
        const uint32_t cur = g.fwVersion();
        if (meta.fw < cur)
        {
            c.confirm += "Downgrade from v" + versionText(cur) + " to v" + versionText(meta.fw) + ". ";
        }
        else if (meta.fw == cur)
        {
            c.confirm += "Reinstall of the running version v" + versionText(cur) + ". ";
        }
        if (g.hasReverted && g.revertedFw == meta.fw && g.revertedCrc == meta.crc)
        {
            c.confirm += "This image reverted on this sensor before. ";
        }
    }
    // Signature (M4c S2): an unsigned image (incl. one signed with a key this
    // firmware does not know) needs the admin's confirmation; with "Signed
    // images only" on it cannot be started at all (it may have been stored
    // before the option was switched on). The flag is read without the config
    // mutex (one aligned bool; sensorMutex is held here)
    const OtaImageSigned::StartRule rule =
        OtaImageSigned::startRule(meta.signedKey, configManager && configManager->signedOnly);
    if (meta.valid && rule != OtaImageSigned::StartRule::Ok)
    {
        if (rule == OtaImageSigned::StartRule::Block)
        {
            c.blockers +=
                "Unsigned image - \"Signed images only\" is on (Firmware page, Advanced mode): upload a signed image. ";
        }
        else if (meta.unknownKey)
        {
            char k[9];
            snprintf(k, sizeof(k), "%08lx", (unsigned long)meta.unknownKey);
            c.confirm += String("Unsigned image (signed with an unknown key ") + k + "). ";
        }
        else
        {
            c.confirm += "Unsigned image. ";
        }
    }
    if (c.confirm.length() > 0)
    {
        c.confirmToken = OtaJob::startConfirmToken(c.confirm.c_str(), c.confirm.length(), meta.crc);
    }
    return c;
}

bool SensorManager::otaRequestStart(uint32_t devId, uint32_t confirmToken, String &msg)
{
    OtaCheck c = otaCheckStart(devId, true);
    if (c.blockers.length() > 0)
    {
        msg = "Not started: " + c.blockers;
        return false;
    }
    // The confirmation covers the reasons shown when the page was loaded:
    // another image or another reason since then (e.g. an unsigned image
    // uploaded in another tab) asks again
    if (c.confirm.length() > 0 && confirmToken != c.confirmToken)
    {
        msg = confirmToken == 0 ? "Confirmation needed: " + c.confirm
                                : "The reasons to confirm changed since the page was loaded - confirm again: " +
                                      c.confirm;
        return false;
    }
    std::lock_guard<std::mutex> lock(sensorMutex);
    if ((otaJobPtr && !OtaJob::terminal(otaJobPtr->state)) || otaStartPending)
    {
        msg = "Another OTA job is running";
        return false;
    }
    otaStartPending = true;
    otaStartDevId = devId;
    if (c.confirm.length() > 0)
    {
        logger.warning("OTA of " + devIdText(devId) + " confirmed by the admin: " + c.confirm);
    }
    msg = "Starting: the image is checked, then the start goes with the sensor's next report";
    return true;
}

bool SensorManager::otaAdmin(uint32_t devId, char action, String &msg)
{
    // Resume: the stored image is read first (flash, without the sensor
    // lock; it cannot change while the job uses it - otaImageBusy), the
    // "Signed images only" rule is applied below under the same lock as the
    // resume itself, with the option's value of that moment: switching it on
    // either comes first (refused here) or holds the resumed job again
    // (otaEnforceSignedOnly)
    OtaStore::Meta resumeMeta;
    bool resumeStored = false;
    if (action == 'r')
    {
        bool have = false;
        uint16_t hw = 0;
        {
            std::lock_guard<std::mutex> lock(sensorMutex);
            if (otaJobPtr && otaJobPtr->devId == devId)
            {
                have = true;
                hw = otaJobPtr->hwType;
            }
        }
        resumeStored = have && OtaStore::get(hw, resumeMeta);
    }
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (!otaJobPtr || otaJobPtr->devId != devId)
    {
        msg = "No OTA job for this sensor";
        return false;
    }
    OtaJob::Job &otaJob = *otaJobPtr;
    OtaJob::Now n = otaNow();
    bool ok = false;
    switch (action)
    {
    case 'a':
        // Out of its OTA session no abort reaches the sensor: it keeps the
        // transfer state (spec 14.5)
        ok = OtaJob::abort(otaJob, n);
        msg = ok ? "Aborted - a sensor still in its OTA session discards the transfer; otherwise its transfer "
                   "state may stay and a job with the same image would resume it"
                 : "Abort not possible now";
        break;
    case 'p':
        ok = OtaJob::pause(otaJob, n);
        msg = ok ? "Paused - the progress is kept" : "Pause not possible now";
        break;
    case 'r':
    {
        if (OtaJob::canResume(otaJob, n) && configManager && configManager->signedOnly &&
            OtaImageSigned::jobHeld(true, resumeStored, resumeMeta.crc, resumeMeta.len, resumeMeta.signedKey,
                                    otaJob.crc, otaJob.len))
        {
            // "Signed images only": a job with an unsigned image is not
            // resumed (it stays Paused; abort it or switch the option off)
            msg = "Resume refused: the job's image is unsigned and \"Signed images only\" is on (Firmware page, "
                  "Advanced mode) - abort the job, or switch the option off and resume";
            otaLastEvent = "OTA " + devIdText(devId) + ": " + msg;
            logger.warning(otaLastEvent);
            return false;
        }
        const bool battery = OtaJob::batteryHold(otaJob, n);
        ok = OtaJob::resume(otaJob, n);
        msg = !ok       ? "Resume not possible now"
              : battery ? "Resumed - battery hold lifted, the start goes with the sensor's next report (a sensor "
                          "still low refuses it, which holds the job for 6 h again)"
                        : "Resumed - the start goes with the sensor's next report";
        break;
    }
    case 'c':
        ok = OtaJob::clear(otaJob, n);
        msg = ok ? "Job cleared (gateway only)" : "Clear not possible now";
        break;
    default:
        msg = "Unknown action";
        break;
    }
    if (ok)
    {
        otaScheduleSaveLocked(millis());
        otaLastEvent = "OTA " + devIdText(devId) + ": " + msg;
        logger.info(otaLastEvent);
        otaDropIfGoneLocked();
    }
    return ok;
}

void SensorManager::otaEnforceSignedOnly(const char *when)
{
    if (!configManager || !configManager->signedOnly)
    {
        return;
    }
    bool job = false, pending = false;
    uint16_t jobHw = 0, pendingHw = 0;
    uint32_t jobDev = 0, jobCrc = 0, jobLen = 0, pendingDev = 0;
    {
        std::lock_guard<std::mutex> lock(sensorMutex);
        if (otaJobPtr && (otaJobPtr->state == State::Waiting || otaJobPtr->state == State::Transfer ||
                          otaJobPtr->state == State::Paused))
        {
            job = true;
            jobDev = otaJobPtr->devId;
            jobHw = otaJobPtr->hwType;
            jobCrc = otaJobPtr->crc;
            jobLen = otaJobPtr->len;
        }
        if (otaStartPending)
        {
            int i = findGen2Locked(otaStartDevId);
            if (i >= 0)
            {
                pending = true;
                pendingDev = otaStartDevId;
                pendingHw = gen2[i].hwType;
            }
        }
    }
    if (!job && !pending)
    {
        return;
    }
    // Flash reads without the sensor lock (the images cannot change while a
    // job or a queued start uses them: otaImageBusy)
    OtaStore::Meta jm, pm;
    const bool hold = job && OtaImageSigned::jobHeld(true, OtaStore::get(jobHw, jm), jm.crc, jm.len, jm.signedKey,
                                                     jobCrc, jobLen);
    // A queued start without a stored image is refused by otaRunStart anyway
    const bool drop = pending && OtaStore::get(pendingHw, pm) &&
                      OtaImageSigned::startRule(pm.signedKey, true) == OtaImageSigned::StartRule::Block;
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (drop && otaStartPending && otaStartDevId == pendingDev)
    {
        otaStartPending = false;
        otaLastEvent = "OTA start of " + devIdText(pendingDev) +
                       " dropped: unsigned image - \"Signed images only\" is on (Firmware page, Advanced mode)";
        logger.warning(otaLastEvent);
    }
    if (hold && otaJobPtr && otaJobPtr->devId == jobDev && otaJobPtr->crc == jobCrc &&
        OtaJob::holdUnsigned(*otaJobPtr, otaNow()))
    {
        otaScheduleSaveLocked(millis());
        otaLastEvent = "OTA " + devIdText(jobDev) + ": held Paused " + when +
                       " - its image is unsigned; abort the job, or switch \"Signed images only\" off (Firmware "
                       "page, Advanced mode) and resume";
        logger.warning(otaLastEvent);
    }
}

bool SensorManager::otaJobCopy(OtaJob::Job &out) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (!otaJobPtr)
    {
        return false;
    }
    out = *otaJobPtr;
    return otaJobPtr->state != State::None || otaJobPtr->ghost;
}

OtaJob::State SensorManager::otaJobState(bool &startPending, OtaJob::WaitView *wait) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    startPending = otaStartPending;
    if (wait)
    {
        *wait = otaJobPtr ? OtaJob::waitView(*otaJobPtr, otaNow()) : OtaJob::WaitView();
    }
    return otaJobPtr ? otaJobPtr->state : State::None;
}

String SensorManager::otaLastEventText() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    return otaLastEvent;
}

bool SensorManager::otaImageBusy(uint16_t hwType) const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    return (otaJobPtr && !OtaJob::terminal(otaJobPtr->state) && otaJobPtr->hwType == hwType) ||
           (otaStartPending && [&]() {
               int i = findGen2Locked(otaStartDevId);
               return i >= 0 && gen2[i].hwType == hwType;
           }());
}

void SensorManager::otaImageChanged(uint16_t hwType)
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    if (otaJobPtr && OtaJob::terminal(otaJobPtr->state) && otaJobPtr->hwType == hwType &&
        otaJobPtr->state != State::None)
    {
        OtaJob::Job &otaJob = *otaJobPtr;
        const bool watch = otaJob.dlRevertWatch.armed();
        const OtaJob::Job old = otaJob;
        otaJob = OtaJob::Job();
        if (watch)
        {
            // keep the revert watch of an installed image
            otaJob.ghost = true;
            otaJob.devId = old.devId;
            otaJob.fw = old.fw;
            otaJob.crc = old.crc;
            otaJob.hwType = old.hwType;
            otaJob.dlRevertWatch = old.dlRevertWatch;
        }
        otaDropIfGoneLocked();
        otaScheduleSaveLocked(millis());
    }
}

String SensorManager::otaDiagJson() const
{
    std::lock_guard<std::mutex> lock(sensorMutex);
    String s = "{\"state\":\"";
    s += otaJobPtr ? OtaJob::stateName(otaJobPtr->state) : "none";
    s += "\"";
    if (otaJobPtr && (otaJobPtr->state != State::None || otaJobPtr->ghost))
    {
        const OtaJob::Job &j = *otaJobPtr;
        s += ",\"devId\":\"" + devIdText(j.devId) + "\",\"version\":\"" + versionText(j.fw) + "\",\"blocks\":" +
             String(j.blocks()) + ",\"confirmed\":" + String(j.maxConfirmed) + ",\"sent\":" + String(j.blocksSent) +
             ",\"repeats\":" + String(j.repeats) + ",\"statuses\":" + String(j.statuses) + ",\"missed\":" +
             String(j.missedTotal) + ",\"rounds\":" + String(j.rounds) + ",\"airtimeS\":" + String(j.airMs / 1000) +
             ",\"etaS\":" + String(OtaJob::etaS(j)) + ",\"anchored\":" + String(j.anchorReliable ? "true" : "false") +
             ",\"anchorRejects\":" + String(j.anchorRejects) +
             ",\"abortPending\":" + String(j.abortPending ? "true" : "false") + ",\"manualPause\":" +
             String(j.manualPause ? "true" : "false") + ",\"why\":\"" + OtaJob::whyText(j.why) + "\"";
    }
    s += ",\"skips\":{\"uplinkSlot\":" + String(otaSkipUplink) + ",\"loop\":" + String(otaSkipLoop) +
         ",\"session\":" + String(otaSkipSession) + ",\"read\":" + String(otaSkipRead) + ",\"radio\":{";
    for (size_t i = 1; i < sizeof(otaSkipRadio) / sizeof(otaSkipRadio[0]); i++)
    {
        if (i > 1)
        {
            s += ",";
        }
        s += "\"" + String(RadioTask::otaOutcomeName((OtaOutcome)i)) + "\":" + String(otaSkipRadio[i]);
    }
    const OtaStore::Timing t = OtaStore::timing();
    s += "}},\"timing\":{\"reads\":" + String(t.reads) + ",\"readMaxUs\":" + String(t.readMaxUs) +
         ",\"readAvgUs\":" + String(t.reads ? t.readSumUs / t.reads : 0) + ",\"verifyUs\":" + String(t.verifyUs) +
         ",\"saveMaxUs\":" + String(otaSaveMaxUs) + "}";
    s += ",\"frameBytes\":" + String(sizeof(OtaFrame)) + ",\"jobBytes\":" + String(sizeof(OtaJob::Job)) + "}";
    return s;
}
