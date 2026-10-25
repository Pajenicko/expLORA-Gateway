/**
 * expLORA Gateway
 *
 * UpdateCheck - see UpdateCheck.h
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "UpdateCheck.h"

#include <memory>
#include <new>

#include "../Data/Logging.h"
#include "../Protocol/OtaImage.h"
#include "../Storage/OtaStore.h"
#include "../config.h"
#include "GatewayOta.h"
#include "SensorFetch.h"

namespace UpdateCheck
{
namespace
{
std::mutex mtx;
std::unique_ptr<Manifest::Doc> current; // the accepted manifest of this run

void ver(uint32_t v, char *t)
{
    OtaImage::versionText(v, t, 16);
}
} // namespace

std::mutex &mutex()
{
    return mtx;
}

const Manifest::Doc *docLocked()
{
    return current.get();
}

Manifest::Offer gatewayOfferLocked(const Manifest::Doc *d)
{
    return d ? Manifest::gatewayOffer(*d, GatewayImage::runningVersionOf(FIRMWARE_VERSION)) : Manifest::Offer::None;
}

Manifest::Offer sensorOfferLocked(const Manifest::Doc *d, uint16_t hw, uint32_t *storedFw, bool *stored)
{
    OtaStore::Meta meta;
    const bool have = OtaStore::get(hw, meta) && meta.valid;
    if (storedFw)
    {
        *storedFw = have ? meta.fw : 0;
    }
    if (stored)
    {
        *stored = have;
    }
    return Manifest::sensorOffer(d ? d->sensorFor(hw) : nullptr, have, meta.fw, have && meta.isSigned());
}

void failed(const String &text)
{
    Logger::error("Update check " + text);
    GatewayOta::recordCheck(text);
    // The offers are those of the last check: a failed one offers nothing
    // (no stale offers next to a "failed" result)
    std::lock_guard<std::mutex> lock(mtx);
    current.reset();
}

bool evaluate(const char *file, size_t len, String &text)
{
    std::unique_ptr<Manifest::Doc> d(new (std::nothrow) Manifest::Doc());
    if (!d)
    {
        failed(text = "failed: no memory for the manifest");
        return false;
    }
    const Manifest::Limits &lim = Manifest::FIRMWARE_LIMITS;
    char why[80], t[176], v[16];
    const Manifest::Result r = GatewayOta::checkManifest(file, len, lim, *d, why, sizeof(why));
    if (r != Manifest::Result::Ok)
    {
        char key[12] = "";
        if (r == Manifest::Result::NoKey)
        {
            snprintf(key, sizeof(key), " %08lx", (unsigned long)d->keyId);
        }
        snprintf(t, sizeof(t), "refused: %s%s%s%s%s", Manifest::resultText(r), key, why[0] ? " (" : "", why,
                 why[0] ? ")" : "");
        text = t;
        // A bad signature or a replay is an attack or a broken release:
        // an error; the rest a warning
        Logger::log(r == Manifest::Result::BadSig || r == Manifest::Result::Replay ? LogLevel::ERROR
                                                                                   : LogLevel::WARNING,
                    "Update check: manifest " + text);
        GatewayOta::recordCheck(text);
        std::lock_guard<std::mutex> lock(mtx);
        current.reset(); // nothing offered from a refused manifest
        return false;
    }
    ver(d->gateway.version, v);
    const char *what;
    switch (d->gatewayInvalid ? Manifest::Offer::None
                              : Manifest::gatewayOffer(*d, GatewayImage::runningVersionOf(FIRMWARE_VERSION)))
    {
    case Manifest::Offer::Install:
        what = " available";
        break;
    case Manifest::Offer::None:
        what = "no gateway package for this board";
        v[0] = 0;
        break;
    case Manifest::Offer::Unknown:
        what = " offered - the running version cannot be compared";
        break;
    default:
        what = "up to date";
        v[0] = 0;
        break;
    }
    if (d->gatewayInvalid)
    {
        // This board's entry breaks a rule: no gateway offer (the sensor
        // packages are still used)
        static const char PREFIX[] = "Update check ";
        snprintf(t, sizeof(t), "%sok (seq %lu): gateway entry invalid (%s)", PREFIX, (unsigned long)d->seq, why);
        Logger::warning(t);
        memmove(t, t + sizeof(PREFIX) - 1, strlen(t + sizeof(PREFIX) - 1) + 1); // the check text without it
    }
    else
    {
        snprintf(t, sizeof(t), "ok (seq %lu): %s%s%s", (unsigned long)d->seq, v[0] ? "v" : "", v, what);
    }
    if (d->skipped)
    {
        // Each one was logged by the check
        const size_t used = strlen(t);
        snprintf(t + used, sizeof(t) - used, ", %u skipped", (unsigned)d->skipped);
    }
    text = t;
    // One info line: seq, key and every version offered
    int n = snprintf(t, sizeof(t), "Update check: manifest seq %lu (key %08lx): gateway %s%s (running v" FIRMWARE_VERSION
                     ")", (unsigned long)d->seq, (unsigned long)d->keyId, d->hasGateway ? "v" : "-",
                     d->hasGateway ? (ver(d->gateway.version, v), v) : "");
    for (uint8_t i = 0; i < d->sensorCount && n > 0 && (size_t)n < sizeof(t); i++)
    {
        ver(d->sensor[i].pkg.version, v);
        n += snprintf(t + n, sizeof(t) - n, ", HW %04X v%s", d->sensor[i].hw, v);
    }
    Logger::info(t);
    GatewayOta::recordCheck(text);
    std::lock_guard<std::mutex> lock(mtx);
    current = std::move(d);
    return true;
}

void printStatus(Print &out, const char *state)
{
    const GatewayOta::View v = GatewayOta::view(0);
    out.printf("{\"state\":\"%s\",\"at\":%lu,\"seq\":%lu,\"text\":\"", state, (unsigned long)v.checkAt,
               (unsigned long)v.manifestSeq);
    // The texts are the gateway's own (no quotes); made safe anyway
    for (const char *s = v.checkText.c_str(); *s; s++)
    {
        out.print(*s == '"' || *s == '\\' || (unsigned char)*s < 0x20 ? '\'' : *s);
    }
    out.print("\"}");
}
} // namespace UpdateCheck
