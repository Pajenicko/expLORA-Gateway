/**
 * expLORA Gateway
 *
 * AutoUpdate - see AutoUpdate.h
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "AutoUpdate.h"

#include <atomic>
#include <mutex>
#include <time.h>

#include "../Data/Logging.h"
#include "../Data/SensorManager.h"
#include "../Network/Uplink.h"
#include "../Protocol/OtaImage.h"
#include "../Storage/ConfigManager.h"
#include "../Storage/OtaStore.h"
#include "../Web/WebServer.h"
#include "../config.h"
#include "GatewayOta.h"
#include "LayoutMigration.h"
#include "UpdateCheck.h"
#include "UpdateMode.h"

extern ConfigManager *configManager;
extern SensorManager *sensorManager;
extern WebPortal *webPortal;
extern Uplink *uplink;

namespace AutoUpdate
{
namespace
{
using namespace AutoUpdatePolicy;

uint8_t digest[32];
uint32_t running = 0; // this firmware's version
Sched sched;
UpdateState::Auto rec; // the persisted record (loop task)
bool recLoaded = false;
uint32_t recTryMs = 0; // the last attempt to read it (retried every RECORD_RETRY_MS)
bool recTried = false;
constexpr uint32_t RECORD_RETRY_MS = 300000;
int32_t runDay = 0;    // the window day of the run in progress
int32_t dayBeforeAct = 0; // rec.day before beforeGatewayActivation() (restored when nothing was activated)
uint32_t lastPassMs = 0, lastSensorMs = 0;
bool sensorDue = true;
std::atomic<bool> settingsDirty{false}; // POST /firmware/auto (web task) -> process()
const char *lastDefer = nullptr; // the deferral last logged (one line per change)
// The last refusal logged per sensor (one line per change)
uint32_t refusedDev[MAX_GEN2_SENSORS], refusedHash[MAX_GEN2_SENSORS];
// Download failures of a gateway version in a row (RAM: no strike)
uint32_t dlVer = 0;
uint8_t dlFails = 0;
// Sensors left alone for a while (RAM): an admin's abort of their
// automatic job (24 h), a start refused before the job existed (30 min)
uint32_t holdDev[MAX_GEN2_SENSORS], holdUntilMs[MAX_GEN2_SENSORS];
constexpr uint32_t HOLD_ABORT_MS = 24UL * 3600 * 1000, HOLD_REFUSED_MS = 30UL * 60 * 1000;

std::mutex viewMtx;
View shown; // loop task writes it once per pass (under viewMtx)

void copyText(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

// "1.2.3"
struct Ver
{
    char t[16];
    explicit Ver(uint32_t v) { OtaImage::versionText(v, t, sizeof(t)); }
};

bool save()
{
    return GatewayOta::setAutoRecord(rec); // logged there when it fails; RAM keeps it
}

void hold(uint32_t devId, uint32_t ms)
{
    size_t i = 0;
    while (i + 1 < MAX_GEN2_SENSORS && holdDev[i] != devId && holdDev[i] != 0)
    {
        i++;
    }
    holdDev[i] = devId;
    holdUntilMs[i] = millis() + ms;
}

bool held(uint32_t devId)
{
    for (size_t i = 0; i < MAX_GEN2_SENSORS && holdDev[i]; i++)
    {
        if (holdDev[i] == devId)
        {
            return (int32_t)(holdUntilMs[i] - millis()) > 0;
        }
    }
    return false;
}

// The automatic install of rec.gwVer: decided once the new firmware is
// validated (strikes cleared) or the previous one runs again (rolled back,
// the strike stays). Checked every pass: the validation takes 120 s
void gatewayOutcome()
{
    const GwBoot b = gatewayBoot(rec.gwAct, rec.gwFails, rec.gwVer, running, GatewayOta::validationPending());
    if (b == GwBoot::Installed)
    {
        snprintf(rec.runText, sizeof(rec.runText), "gateway v%s installed", Ver(rec.gwVer).t);
        rec.gwFails = 0;
        rec.gwAct = false;
        rec.gwDay = 0;
        rec.pendGw = 0;
    }
    else if (b == GwBoot::RolledBack)
    {
        snprintf(rec.runText, sizeof(rec.runText), "gateway v%s rolled back (attempt %u of %u)%s", Ver(rec.gwVer).t,
                 rec.gwFails, GATEWAY_TRIES, gatewayStruck(rec.gwVer, rec.gwFails, rec.gwVer) ? " - given up" : "");
        rec.gwAct = false;
    }
    else
    {
        return;
    }
    rec.runAt = (uint32_t)time(nullptr);
    Logger::logf(b == GwBoot::Installed ? LogLevel::INFO : LogLevel::WARNING, "Automatic update: %s", rec.runText);
    save();
}

// The automatic job ended (or never became a job): counted for its sensor
void trackJob()
{
    if (!rec.job)
    {
        return;
    }
    uint32_t dev = 0, fw = 0, startDev = 0;
    bool startPending = false;
    OtaJob::Why why = OtaJob::Why::None;
    const OtaJob::State st = sensorManager->otaJobBrief(dev, fw, startPending, startDev, &why);
    if (startPending && startDev == rec.job)
    {
        return; // still queued
    }
    const bool mine = dev == rec.job && fw == rec.jobFw;
    if (mine && !OtaJob::terminal(st))
    {
        return;
    }
    // Failed, or aborted by the gateway: a failure. Not counted: an abort by
    // the admin (the sensor is left alone for 24 h) and a start refused
    // before the job existed (tried again after 30 min). Reverted is
    // recorded by the job code, Done needs nothing
    const char *end = st == OtaJob::State::Done ? "done" : "reverted";
    if (!mine)
    {
        end = "not started (refused) - tried again later";
        hold(rec.job, HOLD_REFUSED_MS);
    }
    else if (countsAsFailure(st, why))
    {
        sensorManager->otaAutoFailure(rec.job, rec.jobFw); // logged there
        end = nullptr;
    }
    else if (st == OtaJob::State::Aborted)
    {
        end = "aborted by the admin - not counted, left alone for 24 h";
        hold(rec.job, HOLD_ABORT_MS);
    }
    if (end)
    {
        Logger::logf(LogLevel::INFO, "Automatic update of %08lX to v%s ended: %s", (unsigned long)rec.job,
                     Ver(rec.jobFw).t, end);
    }
    rec.job = rec.jobFw = 0;
    save();
}

// One refusal line per sensor and reason change
void noteRefusal(uint32_t devId, const String &why)
{
    uint32_t h = 2166136261u;
    for (const char *p = why.c_str(); *p; p++)
    {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    size_t i = 0;
    while (i + 1 < MAX_GEN2_SENSORS && refusedDev[i] != devId && refusedDev[i] != 0)
    {
        i++;
    }
    if (refusedDev[i] == devId && refusedHash[i] == h)
    {
        return;
    }
    refusedDev[i] = devId;
    refusedHash[i] = h;
    Logger::logf(LogLevel::WARNING, "Automatic update of %08lX not started: %s", (unsigned long)devId, why.c_str());
}

// Automatic sensor jobs: one at a time, round robin by DevID
const char *sensorStep(const Settings &s, const Clock &c, View &v)
{
    if (rec.job)
    {
        return "";
    }
    if (v.pendingGw)
    {
        return "waiting for the gateway update";
    }
    uint32_t dev, fw, startDev;
    bool startPending;
    const OtaJob::State st = sensorManager->otaJobBrief(dev, fw, startPending, startDev);
    const Activity a = webPortal->autoActivity();
    if (!OtaJob::terminal(st) || startPending || a.upload || a.download || a.session || a.restart || a.confirmWait ||
        a.validation)
    {
        return "waiting: another job or a manual action runs";
    }
    SensorInfo list[MAX_GEN2_SENSORS];
    const size_t n = sensorManager->otaAutoSensors(list, MAX_GEN2_SENSORS);
    uint32_t ids[MAX_GEN2_SENSORS];
    for (size_t i = 0; i < n; i++)
    {
        ids[i] = list[i].devId;
    }
    uint8_t order[MAX_GEN2_SENSORS];
    const size_t k = roundRobin(ids, n, rec.cursor, order);
    const char *note = "";
    for (size_t j = 0; j < k; j++)
    {
        const SensorInfo &si = list[order[j]];
        OtaStore::Meta meta;
        ImageInfo img;
        img.stored = si.hasFw && OtaStore::get(si.hw, meta) && meta.valid;
        img.fw = meta.fw;
        img.verified = meta.isSigned();
        img.needsIdentity = img.stored && OtaImage::needsProvisionedIdentity(meta.flags, img.verified);
        const Skip skip = sensorSkip(si, img);
        if (skip == Skip::DevIdentity)
        {
            noteRefusal(si.devId, skipText(skip)); // visible: one log line per sensor and reason
        }
        if (skip != Skip::None || held(si.devId))
        {
            continue;
        }
        v.queue++;
        if (rec.job || note[0])
        {
            continue; // counted only
        }
        if (!v.nextSensor)
        {
            v.nextSensor = si.devId;
        }
        SensorManager::OtaCheck chk = sensorManager->otaCheckStart(si.devId, true);
        String msg;
        if (chk.blockers.length() || chk.confirm.length())
        {
            noteRefusal(si.devId, chk.blockers.length() ? chk.blockers : "needs a confirmation: " + chk.confirm);
            continue;
        }
        if (!sensorJobFits(sched, s, c, chk.estimateS))
        {
            note = "waiting: the next job would reach the update window";
            continue;
        }
        if (!sensorManager->otaRequestStart(si.devId, 0, msg))
        {
            noteRefusal(si.devId, msg);
            continue;
        }
        Logger::logf(LogLevel::INFO, "Automatic update of %08lX: v%s -> v%s started", (unsigned long)si.devId,
                     Ver(si.fw).t, Ver(img.fw).t);
        rec.job = rec.cursor = si.devId;
        rec.jobFw = img.fw;
        save();
    }
    return note;
}

// The nightly run of window day d
void runNight(const Settings &s, const Clock &c, int32_t d, View &v)
{
    const Defer df = sessionDefer(webPortal->autoActivity());
    const char *why = df != Defer::None                        ? deferText(df)
                      : LayoutMigration::restartRefusal()      ? LayoutMigration::restartRefusal()
                                                               : UpdateMode::wouldRefuse(UpdateMode::Reason::Scheduled);
    Outcome o = Outcome::Deferred;
    String result;
    if (why)
    {
        result = why;
        if (lastDefer != why)
        {
            Logger::logf(LogLevel::WARNING, "Automatic update deferred: %s", why);
            lastDefer = why;
        }
    }
    else
    {
        lastDefer = nullptr;
        runDay = d;
        Logger::logf(LogLevel::INFO, "Automatic update: nightly run (gateway %s, sensors %s)",
                     s.gateway ? "on" : "off", s.sensors ? "on" : "off");
        // Saved first as if it failed: a restart during the run (a crash in
        // the check) waits for the backoff instead of running again at once
        const Sched pre = ifFailed(sched, s, c, d, digest);
        rec.tryDay = pre.retryDay;
        rec.tryStep = pre.step;
        rec.retryAt = pre.retryAt;
        rec.day = pre.doneDay > rec.day ? pre.doneDay : rec.day;
        save();
        o = webPortal->runScheduled(s, result);
    }
    const bool done = finish(sched, s, c, d, o, digest, df == Defer::SensorJob);
    if (rec.graceDay != sched.graceDay)
    {
        rec.graceDay = sched.graceDay; // a restart inside the grace keeps it
        save();
    }
    if (o == Outcome::Deferred && !done)
    {
        snprintf(v.note, sizeof(v.note), "deferred: %s", why);
        return;
    }
    // Ok: the check's own text ("ok (seq N): ...")
    static const char *const HOW[] = {"", "failed: ", "no run tonight - deferred: "};
    char text[UpdateState::RUN_TEXT_MAX];
    snprintf(text, sizeof(text), "%s%s", HOW[(int)o], result.c_str());
    Logger::logf(o == Outcome::Ok ? LogLevel::INFO : LogLevel::WARNING, "Automatic update run: %s%s", text,
                 o != Outcome::Failed ? "" : done ? " - no retry tonight" : " - retried later in the window");
    copyText(rec.runText, sizeof(rec.runText), text);
    rec.runAt = c.unixS;
    if (done)
    {
        rec.day = d;
    }
    rec.tryDay = sched.retryDay;
    rec.tryStep = sched.step;
    rec.retryAt = sched.retryAt;
    save();
}
} // namespace

void begin()
{
    running = GatewayImage::runningVersionOf(FIRMWARE_VERSION);
    uint8_t mac[6];
    getStationMac(mac);
    if (!seed(mac, digest))
    {
        memset(digest, 0, sizeof(digest));
    }
}

const char *gatewayRefusal(uint32_t version)
{
    return gatewayStruck(rec.gwVer, rec.gwFails, version)               ? " (gateway: given up after 3 strikes)"
           : gatewayTriedTonight(rec.gwVer, rec.gwDay, version, runDay) ? " (gateway: tried tonight)"
                                                                        : nullptr;
}

void beforeGatewayDownload(uint32_t version)
{
    rec.gwDl = version;
    rec.gwDlDay = runDay;
    save(); // if this save fails, a crash in the download is not counted
}

bool beforeGatewayActivation(uint32_t version)
{
    const UpdateState::Auto before = rec;
    dayBeforeAct = rec.day;
    rec.gwDl = 0;
    rec.gwDlDay = 0;
    gatewayAttempt(rec.gwVer, rec.gwFails, version);
    rec.gwDay = runDay;
    rec.gwAct = true;
    rec.pendGw = version; // pending until it is validated (sensor jobs wait also after a rollback)
    // The night is done also for the new firmware (no second run tonight)
    rec.day = runDay;
    snprintf(rec.runText, sizeof(rec.runText), "installing gateway v%s (attempt %u of %u)", Ver(version).t,
             rec.gwFails, GATEWAY_TRIES);
    rec.runAt = (uint32_t)time(nullptr);
    if (!save())
    {
        // Without the strike on flash a crash loop of the new firmware
        // would never stop: not installed
        rec = before;
        Logger::logf(LogLevel::ERROR, "Automatic update: gateway v%s not installed - its attempt could not be saved",
                     Ver(version).t);
        return false;
    }
    Logger::logf(LogLevel::INFO, "Automatic update: %s", rec.runText);
    return true;
}

void gatewayInstallEnded(uint32_t version, GwEnd how)
{
    bool changed = rec.gwDl != 0;
    rec.gwDl = 0;
    rec.gwDlDay = 0;
    if (rec.gwAct)
    {
        rec.gwAct = false; // the activation itself failed: the strike stays
        rec.day = dayBeforeAct; // the night is done only by its run (nothing restarted)
        changed = true;
    }
    if (how == GwEnd::Refused)
    {
        if (!gatewayTriedTonight(rec.gwVer, rec.gwDay, version, runDay))
        {
            gatewayAttempt(rec.gwVer, rec.gwFails, version);
            rec.gwDay = runDay;
            changed = true;
        }
        dlVer = version;
        dlFails = DL_TRIES; // no longer blocks the sensor jobs (this run)
    }
    else if (how == GwEnd::Transport)
    {
        dlFails = dlVer == version && dlFails < 255 ? dlFails + 1 : 1;
        dlVer = version;
    }
    if (changed)
    {
        save();
    }
}

bool sensorFetchWanted(uint16_t hw, uint32_t version)
{
    return !fetchStruck(rec.sf, hw, version);
}

void sensorFetched(uint16_t hw, uint32_t version, FetchEnd how)
{
    if (fetchEnded(rec.sf, hw, version, how, runDay))
    {
        save();
    }
}

void settingsChanged(const Settings &s)
{
    Logger::logf(LogLevel::INFO, "Automatic updates: gateway %s, sensors %s, window %02u:00-%02u:00",
                 s.gateway ? "on" : "off", s.sensors ? "on" : "off", s.startH, s.endH);
    settingsDirty = true; // the loop task resets the backoff (it owns the schedule)
}

void process()
{
    if ((uint32_t)(millis() - lastPassMs) < 1000 || !configManager || !webPortal || !sensorManager)
    {
        return;
    }
    lastPassMs = millis();
    if (!recLoaded)
    {
        // Never decide without the record (day done, strikes). Unreadable:
        // tried again every 5 minutes (GatewayOta logs it once an hour)
        if (recTried && (uint32_t)(millis() - recTryMs) < RECORD_RETRY_MS)
        {
            return;
        }
        recTried = true;
        recTryMs = millis();
        if (!GatewayOta::autoRecord(rec))
        {
            std::lock_guard<std::mutex> lock(viewMtx);
            copyText(shown.last, sizeof(shown.last), "update state unreadable - automatic updates wait for it");
            return;
        }
        recLoaded = true;
        sched.doneDay = rec.day;
        sched.graceDay = rec.graceDay;
        restoreRetry(sched, rec.tryDay, rec.tryStep, rec.retryAt);
        if (rec.gwDl)
        {
            // The automatic download of rec.gwDl was still running when the
            // gateway restarted (it crashed it, or the power went): a strike
            const uint32_t dl = rec.gwDl;
            if (downloadInterrupted(rec.gwVer, rec.gwFails, rec.gwDay, dl, rec.gwDlDay))
            {
                snprintf(rec.runText, sizeof(rec.runText), "gateway v%s download interrupted by a restart (attempt %u of %u)%s",
                         Ver(dl).t, rec.gwFails, GATEWAY_TRIES, gatewayStruck(rec.gwVer, rec.gwFails, dl) ? " - given up" : "");
                rec.runAt = (uint32_t)time(nullptr);
                Logger::logf(LogLevel::WARNING, "Automatic update: %s", rec.runText);
            }
            rec.gwDl = 0;
            rec.gwDlDay = 0;
            save();
        }
    }
    if (rec.gwAct || rec.gwFails)
    {
        gatewayOutcome();
    }
    if (settingsDirty)
    {
        settingsDirty = false;
        sched.retryDay = 0; // a new window: no backoff carried over
        sensorDue = true;
        if (rec.tryDay || rec.tryStep || rec.retryAt)
        {
            rec.tryDay = 0;
            rec.tryStep = 0;
            rec.retryAt = 0;
            save();
        }
    }
    Settings s;
    {
        std::lock_guard<std::recursive_mutex> guard(configManager->mutex());
        s = unpack(configManager->autoUpdate);
    }
    View v;
    {
        std::lock_guard<std::mutex> lock(viewMtx);
        v = shown; // notes and the sensor queue stay until decided again
    }
    v.s = s;
    trackJob(); // also when switched off: an automatic job still running is counted
    const time_t now = time(nullptr);
    v.clockOk = uplink && clockValid(uplink->getLastTimeSync() != 0, uplink->isTimeStale(), (uint32_t)now);
    v.next[0] = 0;
    if ((s.gateway || s.sensors) && v.clockOk)
    {
        struct tm tm;
        localtime_r(&now, &tm);
        const Clock c = clockOf(tm, (uint32_t)now);
        if (futureDay(sched.doneDay, c.day))
        {
            sched.doneDay = rec.day = 0;
        }
        const uint32_t offset = checkOffsetS(digest, windowLenS(s));
        // A newer gateway firmware offered by the last accepted manifest -
        // or, until the next check (a restart, a failed check), the version
        // the last one left pending (persisted) if it was attempted and
        // rolled back, for at most PEND_HOLD_DAYS
        {
            // Persisted: only the rollback case, for a few days (persistedPending)
            uint32_t pend = persistedPending(rec.pendGw, rec.gwVer, rec.gwFails, rec.gwDay, c.day) ? rec.pendGw : 0;
            bool checked;
            {
                std::lock_guard<std::mutex> lock(UpdateCheck::mutex());
                const Manifest::Doc *d = UpdateCheck::docLocked();
                checked = d != nullptr;
                if (d)
                {
                    pend = UpdateCheck::gatewayOfferLocked(d) == Manifest::Offer::Install ? d->gateway.version : 0;
                }
            }
            const bool released = dlVer == pend && dlFails >= DL_TRIES;
            if (checked && pendingToSave(pend, released) != rec.pendGw)
            {
                rec.pendGw = pendingToSave(pend, released);
                save();
            }
            if (pend <= running)
            {
                pend = 0; // installed meanwhile (the record is kept for a rollback)
            }
            v.pendingGw = pend && gatewayPending(s.gateway, true, pend, rec.gwVer, rec.gwFails, released)
                              ? pend
                              : 0;
        }
        const Due due = AutoUpdatePolicy::due(sched, s, offset, c);
        if (due.due)
        {
            v.note[0] = 0;
            runNight(s, c, due.day, v);
        }
        if (s.sensors && (sensorDue || (uint32_t)(millis() - lastSensorMs) >= 60000))
        {
            sensorDue = false;
            lastSensorMs = millis();
            v.queue = 0;
            v.nextSensor = 0;
            copyText(v.sensorNote, sizeof(v.sensorNote), sensorStep(s, c, v));
        }
        const Next nx = next(sched, s, offset, c);
        struct tm t = {};
        if (nx.retry)
        {
            const time_t at = nx.at;
            localtime_r(&at, &t);
        }
        else
        {
            int32_t y;
            uint32_t mo, dd;
            civilFromDays(nx.day, y, mo, dd);
            t.tm_year = y - 1900;
            t.tm_mon = (int)mo - 1;
            t.tm_mday = (int)dd;
            t.tm_hour = (int)(nx.sec / 3600);
            t.tm_min = (int)(nx.sec / 60 % 60);
        }
        if (nx.retry || nx.day)
        {
            strftime(v.next, sizeof(v.next), "%Y-%m-%d %H:%M", &t);
        }
    }
    v.job = rec.job;
    v.cursor = rec.cursor;
    copyText(v.last, sizeof(v.last), rec.runText);
    v.lastAt = rec.runAt;
    v.gwVer = rec.gwVer;
    v.gwFails = rec.gwFails;
    std::lock_guard<std::mutex> lock(viewMtx);
    shown = v;
}

View view()
{
    View v;
    {
        std::lock_guard<std::mutex> lock(viewMtx);
        v = shown;
    }
    // The settings as saved now (the rest is at most a second old): a page
    // loaded right after "Save" shows what was saved
    if (configManager)
    {
        std::lock_guard<std::recursive_mutex> guard(configManager->mutex());
        v.s = unpack(configManager->autoUpdate);
    }
    return v;
}

void printDiagJson(Print &out)
{
    View v = view();
    // The texts are the gateway's own (constants, versions, a result whose
    // quotes become ')
    for (char *p = v.last; *p; p++)
    {
        if (*p == '"' || *p == '\\' || (uint8_t)*p < 0x20)
        {
            *p = '\'';
        }
    }
    static const char *const B[] = {"false", "true"};
    out.printf(",\"auto\":{\"gateway\":%s,\"sensors\":%s,\"window\":[%u,%u],\"clock\":%s,\"next\":\"%s\",\"last\":\"%s\","
               "\"lastAt\":%lu,\"note\":\"%s\",\"pendingGw\":\"%s\",\"gwVer\":\"%s\",\"gwFails\":%u,\"job\":\"%08lX\","
               "\"nextSensor\":\"%08lX\",\"cursor\":\"%08lX\",\"queue\":%u,\"sensorNote\":\"%s\"}",
               B[v.s.gateway], B[v.s.sensors], v.s.startH, v.s.endH, B[v.clockOk], v.next, v.last, (unsigned long)v.lastAt,
               v.note, v.pendingGw ? Ver(v.pendingGw).t : "", Ver(v.gwVer).t, v.gwFails, (unsigned long)v.job,
               (unsigned long)v.nextSensor, (unsigned long)v.cursor, v.queue, v.sensorNote);
}
} // namespace AutoUpdate
