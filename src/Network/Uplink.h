/**
 * expLORA Gateway
 *
 * Uplink - connectivity policy header file
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

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <functional>
#include "../config.h"
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"
#if defined(HAS_ETHERNET)
#include <ETH.h>
#endif
#include "LteModem.h"

/**
 * Connectivity policy
 *
 * Decides which interfaces run and which one carries traffic; the interfaces
 * themselves are the core's NetworkInterface objects (WiFi.STA, WiFi.AP, ...),
 * so clients (MQTT, HTTP, NTP) just use NetworkClient and the default route.
 *
 * - ethernet (boards with HAS_ETHERNET): DHCP, preferred over WiFi through
 *   the netif route priority, so the default route follows link state
 * - station: connects with the configured credentials, retries every
 *   WIFI_RECONNECT_INTERVAL while down
 * - LTE (boards with HAS_LTE): PPP session kept up as a hot standby, carries
 *   traffic only when neither ethernet nor station has an address
 * - access point: up for AP_TIMEOUT after local access (ethernet or station)
 *   comes up, and whenever local access is lost (immediately when there are no
 *   WiFi credentials, otherwise after AP_RESTORE_DELAY)
 * - captive portal (DNS + config UI) whenever the AP is up without local access
 * - "Enable WiFi" switch (wifiEnabled, boards with HAS_WIFI_SWITCH = ethernet;
 *   the Lite has no such setting, WiFi is its only uplink), applied by a
 *   restart like the LTE switch, with or without WiFi credentials. Off:
 *   the station is never started. The AP runs for AP_TIMEOUT from its first
 *   start after every boot (however long that start takes) - an Ethernet address does not prove the gateway is reachable
 *   (wrong static address, DHCP on an isolated VLAN) and there is no reset
 *   button, so a power cycle is always a way back in. After that, while
 *   ethernet has an address, the AP is stopped and the WiFi driver
 *   deinitialised (frees ~30 kB of internal RAM on the Pro, room for TLS).
 *   When ethernet has no address for AP_RESTORE_DELAY the AP (network page
 *   included, captive portal without other access) comes up again and stays
 *   until ethernet has had an address for AP_TIMEOUT. LTE does not count as
 *   local access (it is not reachable from the LAN). Saving WiFi
 *   credentials (/config) switches WiFi back on. If the AP cannot start
 *   for lack of memory (e.g. LTE + an MQTT TLS session), apStarved() makes
 *   MQTT drop its TLS session for a few retries.
 *   Pure rules: WifiPolicy.h (native tests in test/test_wifi_policy)
 * - DNS: lwIP keeps one global server list that every interface overwrites
 *   when it gets an address, so the servers of the active interface are
 *   remembered per interface and re-applied (also when something else
 *   cleared the list: refilled when found empty, checked every 2 s); the default route is set to the
 *   active interface the same way (an Ethernet netif without a cable keeps
 *   its route priority). esp_netif_set_default_netif is a sticky override:
 *   ESP-IDF's route_prio choice no longer applies (until that netif is
 *   destroyed), so the uplink owns the default route; after an interface
 *   loses its address the route and DNS follow when loop() sees the uplink
 *   change, at most 2 s later (re-applying them from the address-loss
 *   events made the Pro reset under a connection flood - not done)
 * - WiFi switched off and the AP failing to start (memory): the boot grace
 *   lasts until the AP has run for AP_TIMEOUT, so with an AP that never
 *   starts it never ends - the driver stays loaded and the start is retried
 *   every 5 s (log rate-limited); the trade-off keeps a power cycle a way
 *   back in
 * - time: SNTP is (re)started when the device goes online, without blocking
 */
class Uplink
{
public:
    enum class Type : uint8_t
    {
        None,
        Ethernet,
        WiFi,
        LTE,
    };

    Uplink(ConfigManager &config, Logger &log);

    // Start AP and/or station; waits up to WIFI_BOOT_CONNECT_TIMEOUT for the station
    void begin();

    // Call from loop()
    void process();

    // Some uplink has an IP address
    bool isOnline() const { return active != Type::None; }

    // The gateway is reachable on a local network (ethernet or station has an IP)
    bool hasLocalAccess() const;

    // Interface carrying traffic and its address
    Type getActive() const { return active; }
    const char *getActiveName() const;

    // Time: valid once synced; stale when the last sync is older than
    // TIME_STALE_AFTER_S (the clock keeps running meanwhile)
    bool isTimeValid() const;
    time_t getLastTimeSync() const { return (time_t)lastTimeSync; }
    bool isTimeStale() const;
    // "2026-10-17 10:15:00 (synced 5 min ago)" or "not synchronized"
    String getTimeStatus() const;
    IPAddress getLocalIP() const;

    // "Enable WiFi" is off (never true on boards without HAS_WIFI_SWITCH)
    bool wifiSwitchedOff() const;
    // The WiFi driver is initialised (holds its RAM), started or not
    bool wifiDriverRunning() const;

    // Station state, safe from any task (false / 0.0.0.0 while WiFi is
    // switched off, without touching the driver)
    bool stationHasIP() const;
    bool stationConnected() const;
    IPAddress getStationIP() const;

    // Access point (any task; the address is cached, no driver call)
    bool isApActive() const { return apActive.load(); }
    bool isCaptive() const { return captive; }
    const String &getApName() const { return apName; }
    IPAddress getApIP() const;
    // The AP is the way in but failed to start for lack of memory: MQTT
    // drops / holds back its TLS session (for a few failed starts, see
    // WifiPolicy::apStarved), so the admin is not locked out
    bool apStarved() const { return starved.load(); }
    // Failed AP starts in a row and the free 8-bit heap at the last one
    uint32_t apFailedStarts() const { return apStartFailures.load(); }
    uint32_t apFailedStartFree() const { return apFailFree.load(); }
    static constexpr uint32_t apStarvedMaxTries() { return 3; }

    // Ethernet state (false on boards without ethernet)
    bool ethernetLinkUp() const;
    IPAddress getEthernetIP() const;

#if defined(HAS_LTE)
    const LteModem &getLte() const { return *lte; }
#endif

    // Called when the captive portal turns on or off
    void setCaptiveCallback(std::function<void(bool)> callback) { captiveCallback = callback; }

    // Called when the interface carrying traffic changes (open connections keep
    // the old source address, so clients should reconnect)
    void setActiveChangeCallback(std::function<void(Type)> callback) { activeChangeCallback = callback; }

private:
    ConfigManager &config;
    Logger &logger;

    String apName;
    std::atomic<bool> apActive{false};
    std::atomic<uint32_t> apIp{0};          // AP address while it runs
    std::atomic<uint32_t> apStartFailures{0}; // failed starts in a row
    std::atomic<uint32_t> apFailFree{0};      // free 8-bit heap at the last one
    bool apFailLowMemory = false;             // ... below AP_START_LOW_MEMORY
    std::atomic<bool> starved{false};       // see apStarved()
    unsigned long lastApAttempt = 0;
    bool apUpSinceBoot = false;             // the AP has run since boot
    unsigned long apFirstUpAt = 0;          // when it first came up
    bool bootGraceOver = false;             // the AP ran for AP_TIMEOUT since then (latched)
    unsigned long lastDriverStop = 0;
    bool driverStopFailedLogged = false;
    bool captive = false;
    Type active = Type::None;
    bool switchedOffAtBoot = false; // "Enable WiFi" off, read in begin()

    bool localWasUp = false;
    unsigned long localAccessSince = 0; // when local access came up
    unsigned long localAccessLostAt = 0;
    bool ethernetStarted = false;
    bool ethernetWasUp = false;
    bool stationWasConnected = false;
    bool stationEverConnected = false;
    bool reconnecting = false;
    unsigned long reconnectStartedAt = 0;

    static volatile bool timeSyncedFlag;
    // Wall clock of the last successful SNTP sync; 32-bit so the SNTP task's
    // write cannot be read torn by another task
    static volatile uint32_t lastTimeSync;
    bool timeSyncLogged = false;

    std::function<void(bool)> captiveCallback;
    std::function<void(Type)> activeChangeCallback;

#if defined(HAS_LTE)
    LteModem *lte = nullptr;
#endif

    // DNS servers per Type, captured on each GOT_IP event (event task)
    static constexpr size_t TYPE_COUNT = 4;
    IPAddress dnsServers[TYPE_COUNT][2];
    portMUX_TYPE dnsMux = portMUX_INITIALIZER_UNLOCKED;
    volatile bool dnsDirty = false;
    unsigned long lastDnsCheck = 0;   // see DNS_CHECK_INTERVAL

    void onGotIp(Type type, NetworkInterface &ifc);
    void applyDefaultRoute();
    void applyDns(bool onlyIfCleared);
    NetworkInterface *interfaceFor(Type type) const;

    bool stationEnabled() const;
    bool ethernetHasIP() const;
    void startEthernet();
    void processEthernet();
    void processStation(unsigned long now);
    void processLocalAccess(unsigned long now);
    void processAp(unsigned long now);
    void startAp();
    void stopAp();
    bool stopDriver();
    void selectActive();
    void startTimeSync();
    static void onTimeSync(struct timeval *tv);
};

// Global instance (created in main.cpp)
extern Uplink *uplink;
