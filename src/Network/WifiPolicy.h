/**
 * expLORA Gateway
 *
 * WifiPolicy - pure decisions of the WiFi part of the connectivity policy
 * (station, access point, driver), kept free of Arduino so the native tests
 * can pin them (test/test_wifi_policy)
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
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

#include <cstdint>

namespace WifiPolicy
{
    // The "Enable WiFi" setting switches WiFi off on a board that has the
    // switch (another local access, i.e. ethernet) - with or without WiFi
    // credentials: an Ethernet-only Pro never had any. The access point
    // rules below keep such a board reachable (apWanted)
    inline bool switchedOff(bool boardHasSwitch, bool wifiEnabled)
    {
        return boardHasSwitch && !wifiEnabled;
    }

    // The station runs with credentials, outside configuration mode, and
    // only while WiFi is not switched off
    inline bool stationWanted(bool switchedOff, bool configMode, bool hasCredentials)
    {
        return !switchedOff && !configMode && hasCredentials;
    }

    struct ApInput
    {
        bool localAccess;  // ethernet or station has an address
        bool apActive;
        bool stationWanted;
        bool switchedOff;
        bool bootGrace;             // less than apTimeout since boot (latched off by the caller)
        uint32_t now;
        uint32_t localAccessSince;  // when local access came up
        uint32_t localAccessLostAt; // when it went down (or boot)
        uint32_t apTimeout;         // AP_TIMEOUT
        uint32_t restoreDelay;      // AP_RESTORE_DELAY
    };

    // Whether the access point should be up (uint32_t: millis() arithmetic wraps)
    inline bool apWanted(const ApInput &in)
    {
        if (in.switchedOff)
        {
            // WiFi switched off. "Has an Ethernet address" does not prove
            // the gateway is reachable (a wrong static address, DHCP on an
            // isolated VLAN) and there is no reset button: the AP runs for
            // apTimeout after every boot, so a power cycle is always a way
            // back in
            if (in.bootGrace)
            {
                return true;
            }
            if (in.localAccess)
            {
                // A recovery AP times out once Ethernet is back
                return in.apActive && (in.now - in.localAccessSince < in.apTimeout);
            }
            // No Ethernet address: the recovery AP after a sustained outage,
            // then it stays while there is none
            return in.apActive || (in.now - in.localAccessLostAt > in.restoreDelay);
        }
        if (in.localAccess)
        {
            // Keep the AP for apTimeout after local access came up
            return in.apActive && (in.now - in.localAccessSince < in.apTimeout);
        }
        if (!in.stationWanted)
        {
            // No credentials / configuration mode - the AP is the only way in
            return true;
        }
        // Station retrying: restore the AP only after a sustained outage
        // (gives DHCP time at boot)
        return in.apActive || (in.now - in.localAccessLostAt > in.restoreDelay);
    }

    // Boot grace with WiFi switched off: over once the access point has run
    // for apTimeout since it first came up after boot - an AP that could not
    // start at once (memory) still gets its full time, so a power cycle is
    // always a way back in (uint32_t: millis() arithmetic wraps)
    inline bool bootGraceOver(bool apUpSinceBoot, uint32_t now, uint32_t apFirstUpAt, uint32_t apTimeout)
    {
        return apUpSinceBoot && now - apFirstUpAt >= apTimeout;
    }

    // MQTT holds its TLS session (~40 kB) back while the access point is the
    // way in (WiFi switched off, or no local access) and its last start
    // failed for lack of memory - for at most maxTries failed starts in a
    // row: if the AP still cannot start with the session released, the
    // memory was not MQTT's and holding it back does not help
    inline bool apStarved(bool apIsWayIn, uint32_t failedStarts, bool lastFailureLowMemory, uint32_t maxTries)
    {
        return apIsWayIn && lastFailureLowMemory && failedStarts > 0 && failedStarts <= maxTries;
    }

    // The WiFi driver (and its ~30 kB of internal RAM) is needed only while
    // the station or the access point runs
    inline bool driverWanted(bool stationWanted, bool apWanted)
    {
        return stationWanted || apWanted;
    }
}
