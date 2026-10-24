/**
 * expLORA Gateway
 *
 * Uplink - connectivity policy implementation file
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

#include "Uplink.h"
#include "WifiPolicy.h"
#include <esp_sntp.h>
#include <esp_wifi.h>
#if defined(HAS_LTE)
#include <PPP.h>
#endif

// Station connect wait during boot (matches the old 20 x 500 ms)
static constexpr unsigned long WIFI_BOOT_CONNECT_TIMEOUT = 10000;
// One reconnect attempt is given this long before it counts as failed
static constexpr unsigned long WIFI_RECONNECT_ATTEMPT_TIMEOUT = 5000;
// Short outages do not bring the AP up
static constexpr unsigned long AP_RESTORE_DELAY = 10000;
// Retry interval when the AP fails to start
static constexpr unsigned long AP_RETRY_INTERVAL = 5000;
// The global DNS list is compared with the active interface's servers this often
static constexpr unsigned long DNS_CHECK_INTERVAL = 2000;
// A failed AP start counts as lack of memory below this much free 8-bit
// heap (the driver with the AP takes ~36 kB on the Pro)
static constexpr uint32_t AP_START_LOW_MEMORY = 48000;
// MQTT holds its TLS session back for at most this many failed AP starts in
// a row (WifiPolicy::apStarved)
static constexpr uint32_t AP_STARVED_MAX_TRIES = Uplink::apStarvedMaxTries();
#if defined(HAS_ETHERNET)
// Above WiFi STA (100), so a connected cable carries traffic; PPP stays at 20
static constexpr int ETH_ROUTE_PRIO = 150;
#endif

volatile bool Uplink::timeSyncedFlag = false;
volatile uint32_t Uplink::lastTimeSync = 0;

Uplink::Uplink(ConfigManager &cfg, Logger &log)
    : config(cfg), logger(log)
{
    apName = makeApName();
}

// "Enable WiFi" off (boards with HAS_WIFI_SWITCH, with or without
// credentials); read once in begin(): a change applies with the restart
// that follows it
bool Uplink::wifiSwitchedOff() const
{
    return switchedOffAtBoot;
}

// Station runs whenever credentials exist, the device is not in config mode
// and WiFi is not switched off
bool Uplink::stationEnabled() const
{
    return WifiPolicy::stationWanted(wifiSwitchedOff(), config.configMode, config.wifiSSID.length() > 0);
}

// Initialised (esp_wifi_init), started or not: the driver's RAM is taken
// from the init on. Arduino's WiFi.getMode() reports WIFI_OFF for a driver
// whose esp_wifi_start() failed although it is initialised
bool Uplink::wifiDriverRunning() const
{
    wifi_mode_t mode;
    return esp_wifi_get_mode(&mode) == ESP_OK;
}

// The station's state; never touches the WiFi netif while WiFi is switched
// off (the loop task may be deinitialising the driver meanwhile)
bool Uplink::stationHasIP() const
{
    return !switchedOffAtBoot && WiFi.STA.hasIP();
}

bool Uplink::stationConnected() const
{
    return !switchedOffAtBoot && WiFi.STA.connected();
}

IPAddress Uplink::getStationIP() const
{
    return stationHasIP() ? WiFi.STA.localIP() : IPAddress();
}

// Stop and deinitialise the WiFi driver (WiFi switched off, AP not needed).
// Arduino deinitialises only a started driver: one left initialised by a
// failed esp_wifi_start() is started first. true = the driver is off
bool Uplink::stopDriver()
{
    if (!wifiDriverRunning())
    {
        return true;
    }
    // Station mode for that start: it sends nothing until a connect (an AP
    // start would briefly broadcast the driver's default AP configuration)
    if (WiFi.getMode() == WIFI_OFF && !WiFi.mode(WIFI_STA))
    {
        return false;
    }
    return WiFi.mode(WIFI_OFF) && !wifiDriverRunning();
}

// Start AP and/or station
void Uplink::begin()
{
    // Credentials live in our own configuration: do not let the WiFi library
    // write them to NVS on every mode change / begin() (a flash write outside
    // the flash write permit, Hardware/FlashPermit.h)
    WiFi.persistent(false);

    logger.info("Configuring WiFi. ConfigMode: " + String(config.configMode ? "true" : "false") +
                ", SSID length: " + String(config.wifiSSID.length()));

    sntp_set_time_sync_notification_cb(onTimeSync);

#if defined(HAS_WIFI_SWITCH)
    const bool boardHasSwitch = true;
#else
    const bool boardHasSwitch = false;
#endif
    {
        std::lock_guard<std::recursive_mutex> guard(config.mutex());
        switchedOffAtBoot = WifiPolicy::switchedOff(boardHasSwitch, config.wifiEnabled);
    }

    Network.onEvent([this](arduino_event_id_t event, arduino_event_info_t)
                    {
        switch (event)
        {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            onGotIp(Type::WiFi, WiFi.STA);
            break;
#if defined(HAS_ETHERNET)
        case ARDUINO_EVENT_ETH_GOT_IP:
            onGotIp(Type::Ethernet, ETH);
            break;
#endif
#if defined(HAS_LTE)
        case ARDUINO_EVENT_PPP_GOT_IP:
            onGotIp(Type::LTE, PPP);
            break;
#endif
        default:
            break;
        } });

    startEthernet();
#if defined(HAS_LTE)
    lte = new LteModem(config, logger);
    lte->begin();
#endif

    if (wifiSwitchedOff())
    {
        // WiFi switched off: the station never starts. The access point
        // runs for AP_TIMEOUT after every boot (an Ethernet address does not
        // prove the gateway is reachable - wrong static address, isolated
        // VLAN - and a power cycle is the only reset there is), then it
        // stops and the driver is deinitialised (its ~30 kB of internal RAM
        // are freed) while Ethernet has an address; without one it stays
        logger.info("WiFi disabled in configuration - no station; the access point runs for " +
                    String(AP_TIMEOUT / 60000) + " min after boot, then only while Ethernet has no address");
        startAp();
        lastApAttempt = millis();
        processEthernet();
        if (hasLocalAccess())
        {
            localWasUp = true;
            localAccessSince = millis();
        }
        else
        {
            localAccessLostAt = millis();
        }
        captive = apActive && !hasLocalAccess();
        selectActive();
        if (isOnline())
        {
            startTimeSync();
        }
        return;
    }

    if (!stationEnabled())
    {
        // Configuration mode or no credentials - AP (+ ethernet)
        logger.info("Starting in AP mode only");
        WiFi.mode(WIFI_AP);
        startAp();
        config.enableConfigMode(true);
        localAccessLostAt = millis();
        captive = !hasLocalAccess();
        selectActive();
        return;
    }

    // Credentials available - AP + station, the AP times out once connected
    logger.info("Starting in AP+STA mode (dual mode)");
    WiFi.mode(WIFI_AP_STA);
    startAp();

    logger.info("Attempting to connect to WiFi: " + config.wifiSSID);
    WiFi.begin(config.wifiSSID.c_str(), config.wifiPassword.c_str());

    unsigned long start = millis();
    while (!WiFi.STA.connected() && millis() - start < WIFI_BOOT_CONNECT_TIMEOUT)
    {
        delay(500);
        Serial.print(".");
    }
    config.lastWifiAttempt = millis();

    if (!WiFi.STA.hasIP())
    {
        // Give DHCP a moment after association
        start = millis();
        while (WiFi.STA.connected() && !WiFi.STA.hasIP() && millis() - start < 3000)
        {
            delay(100);
        }
    }

    if (WiFi.STA.hasIP())
    {
        logger.info("WiFi connected! IP: " + WiFi.localIP().toString());
        config.enableConfigMode(false);
        stationWasConnected = true;
        stationEverConnected = true;
    }
    else
    {
        logger.warning("Failed to connect to WiFi. SSID: " + config.wifiSSID +
                       ", keeping AP active and retrying every " + String(WIFI_RECONNECT_INTERVAL / 1000) + " s");
    }

    processEthernet();
    if (hasLocalAccess())
    {
        localWasUp = true;
        localAccessSince = millis();
    }
    else
    {
        localAccessLostAt = millis();
    }

    captive = !hasLocalAccess();
    selectActive();
    if (isOnline())
    {
        startTimeSync();
    }
}

// Call from loop()
void Uplink::process()
{
    unsigned long now = millis();

    processEthernet();
    processStation(now);
#if defined(HAS_LTE)
    lte->process();
#endif
    processLocalAccess(now);

    bool wasOnline = isOnline();
    selectActive();
    // lwIP's global DNS list can also be cleared behind our back: the
    // Ethernet DHCP client's teardown zeroed it ~2 s after the cable went
    // (bench, after the LTE servers had been applied), so HTTPS over LTE
    // failed in DNS until the next interface change. Checked every
    // DNS_CHECK_INTERVAL, refilled only when empty: a non-empty list may be
    // an interface's fresh DHCP servers that its GOT_IP event is about to
    // record (overwriting them made LTE's servers stick to Ethernet)
    if (dnsDirty)
    {
        applyDns(false);
        applyDefaultRoute();
    }
    else if (now - lastDnsCheck >= DNS_CHECK_INTERVAL)
    {
        lastDnsCheck = now;
        applyDns(true);
        applyDefaultRoute();
    }
    if (!wasOnline && isOnline())
    {
        startTimeSync();
    }

    processAp(now);

    // Report the first successful SNTP sync (callback runs in the SNTP task)
    if (timeSyncedFlag && !timeSyncLogged)
    {
        timeSyncLogged = true;
        logger.setTimeInitialized(true);
        logger.info("NTP time synchronized successfully");
    }

    // A stale clock is still used (it drifts by seconds per day), just flagged
    static bool staleLogged = false;
    if (timeSyncLogged && isTimeStale() != staleLogged)
    {
        staleLogged = !staleLogged;
        if (staleLogged)
        {
            logger.warning("No successful NTP sync for " + String(TIME_STALE_AFTER_S / 3600) + " h");
        }
    }
}

// Station: detect transitions, retry while down
void Uplink::processStation(unsigned long now)
{
    if (!stationEnabled())
    {
        return;
    }

    bool connected = WiFi.STA.hasIP();
    if (connected != stationWasConnected)
    {
        stationWasConnected = connected;
        if (connected)
        {
            logger.info(String(stationEverConnected ? "WiFi reconnected! IP: " : "WiFi connected! IP: ") +
                        WiFi.localIP().toString());
            stationEverConnected = true;
            reconnecting = false;
        }
        else
        {
            logger.warning("WiFi disconnected");
        }
    }

    if (connected)
    {
        return;
    }

    // Non-blocking reconnect: one WiFi.begin() per WIFI_RECONNECT_INTERVAL
    if (!reconnecting)
    {
        if (now - config.lastWifiAttempt > WIFI_RECONNECT_INTERVAL)
        {
            logger.info("Attempting to reconnect to WiFi...");
            config.lastWifiAttempt = now;
            String ssid, password; // copies: the web task may save new ones
            {
                std::lock_guard<std::recursive_mutex> guard(config.mutex());
                ssid = config.wifiSSID;
                password = config.wifiPassword;
            }
            WiFi.begin(ssid.c_str(), password.c_str());
            reconnecting = true;
            reconnectStartedAt = now;
        }
    }
    else if (now - reconnectStartedAt > WIFI_RECONNECT_ATTEMPT_TIMEOUT)
    {
        logger.warning("Failed to reconnect to WiFi");
        reconnecting = false;
    }
}

// Track when local access (any interface) came up or went down
void Uplink::processLocalAccess(unsigned long now)
{
    bool local = hasLocalAccess();
    if (local == localWasUp)
    {
        return;
    }
    localWasUp = local;
    if (local)
    {
        localAccessSince = now;
    }
    else
    {
        localAccessLostAt = now;
    }
}

// Access point and captive portal
void Uplink::processAp(unsigned long now)
{
    bool local = hasLocalAccess();
    bool switchedOff = wifiSwitchedOff();
    // Latched (millis() wraps after 49 days); counted from the first AP start
    if (!bootGraceOver)
    {
        if (apActive && !apUpSinceBoot)
        {
            apUpSinceBoot = true;
            apFirstUpAt = now;
        }
        bootGraceOver = WifiPolicy::bootGraceOver(apUpSinceBoot, now, apFirstUpAt, AP_TIMEOUT);
    }
    WifiPolicy::ApInput in;
    in.localAccess = local;
    in.apActive = apActive;
    in.stationWanted = stationEnabled();
    in.switchedOff = switchedOff;
    in.bootGrace = !bootGraceOver;
    in.now = now;
    in.localAccessSince = localAccessSince;
    in.localAccessLostAt = localAccessLostAt;
    in.apTimeout = AP_TIMEOUT;
    in.restoreDelay = AP_RESTORE_DELAY;
    bool wantAp = WifiPolicy::apWanted(in);

    if (wantAp && !apActive && now - lastApAttempt > AP_RETRY_INTERVAL)
    {
        lastApAttempt = now;
        if (switchedOff && !in.bootGrace && apStartFailures == 0)
        {
            logger.warning("WiFi is disabled, but there is no Ethernet address - starting the access point for recovery");
        }
        startAp();
    }
    else if (!wantAp && apActive)
    {
        logger.info(switchedOff ? "AP timeout reached with Ethernet access, WiFi disabled - stopping the access point"
                                : "AP timeout reached with local network access. Switching to client mode only.");
        stopAp();
        if (switchedOff && !wifiDriverRunning())
        {
            logger.info("WiFi driver stopped");
        }
    }
    // Failed starts count until the AP runs or is no longer wanted. MQTT
    // holds its TLS session back meanwhile (apStarved()) when the AP is the
    // way in (WiFi switched off, or no local access at all) and the start
    // failed for lack of memory - for AP_STARVED_MAX_TRIES failed starts
    if (!wantAp || apActive)
    {
        apStartFailures = 0;
    }
    bool wasStarved = starved;
    starved = WifiPolicy::apStarved(switchedOff || !local, apStartFailures, apFailLowMemory, AP_STARVED_MAX_TRIES);
    if (wasStarved && !starved && apStartFailures > AP_STARVED_MAX_TRIES)
    {
        logger.warning("The access point still fails to start with the MQTT TLS session released - not holding it back");
    }

    // WiFi switched off and the AP not needed: stop and deinitialise the
    // driver (softAPdisconnect(true) does it already when the AP ran alone;
    // this also covers a failed AP start that left the driver initialised).
    // A failed stop is retried every AP_RETRY_INTERVAL and logged once
    if (switchedOff && !WifiPolicy::driverWanted(stationEnabled(), wantAp) && !apActive && wifiDriverRunning() &&
        now - lastDriverStop > AP_RETRY_INTERVAL)
    {
        lastDriverStop = now;
        if (stopDriver())
        {
            driverStopFailedLogged = false;
            logger.info("WiFi driver stopped");
        }
        else if (!driverStopFailedLogged)
        {
            driverStopFailedLogged = true;
            logger.warning("WiFi driver could not be stopped - retrying");
        }
    }

    bool wantCaptive = apActive && !local;
    if (wantCaptive != captive)
    {
        captive = wantCaptive;
        logger.info(captive ? "No local network access - captive portal enabled"
                            : "Local network access - captive portal disabled");
        if (captiveCallback)
        {
            captiveCallback(captive);
        }
    }
}

// Bring the AP up without touching the station
void Uplink::startAp()
{
    wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_OFF)
    {
        WiFi.mode(WIFI_AP);
    }
    else if (mode == WIFI_STA)
    {
        WiFi.mode(WIFI_AP_STA);
    }

    // Not started (esp_wifi_init / start failed, e.g. no memory): retried
    // after AP_RETRY_INTERVAL
    bool started = WiFi.getMode() != WIFI_OFF;
    if (started)
    {
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
    }
    // Channel 6, open, max 4 clients (the radio follows the station's channel when connected)
    if (started && WiFi.softAP(apName.c_str(), nullptr, 6, 0, 4))
    {
        apStartFailures = 0;
        IPAddress ip = WiFi.softAPIP();
        apIp = (uint32_t)ip;
        apActive = true;
        logger.info("AP started with SSID: " + apName + ", IP: " + ip.toString());
    }
    else
    {
        uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        apStartFailures = apStartFailures + 1;
        apFailFree = free8;
        apFailLowMemory = free8 < AP_START_LOW_MEMORY;
        // Retried every AP_RETRY_INTERVAL for as long as the AP is wanted
        // (with WiFi off and the AP failing, also the boot grace: it ends
        // only after the AP ran): logged at failures 1-3, then at powers of
        // two and every 30 min
        uint32_t n = apStartFailures;
        if (n <= 3 || (n & (n - 1)) == 0 || n % 360 == 0)
        {
            logger.error("Failed to start AP: " + apName + " (8-bit heap " + String(free8) + " B free, failed start " +
                         String(n) + ", retried every " + String(AP_RETRY_INTERVAL / 1000) + " s)");
        }
    }
}

// Stop the AP, keep the station. Web handlers (another task) read apActive
// and the AP address: both are cleared before the driver tears the AP down
void Uplink::stopAp()
{
    apActive = false;
    apIp = 0;
    WiFi.softAPdisconnect(true);
}

// Pick the interface carrying traffic
void Uplink::selectActive()
{
    // Same order as the route priorities
    Type next = Type::None;
    if (ethernetHasIP())
    {
        next = Type::Ethernet;
    }
    else if (stationHasIP())
    {
        next = Type::WiFi;
    }
#if defined(HAS_LTE)
    else if (lte->hasIP())
    {
        next = Type::LTE;
    }
#endif

    if (next != active)
    {
        Type previous = active;
        active = next;
        dnsDirty = true;
        if (active != Type::None)
        {
            logger.info(String("Uplink: ") + getActiveName() + ", IP " + getLocalIP().toString());
        }
        else
        {
            logger.warning("Uplink: offline");
        }
        if (previous != Type::None && activeChangeCallback)
        {
            activeChangeCallback(active);
        }
    }
}

// Event task: the global DNS list now holds this interface's servers
void Uplink::onGotIp(Type type, NetworkInterface &ifc)
{
    IPAddress main = ifc.dnsIP(0);
    IPAddress backup = ifc.dnsIP(1);
    portENTER_CRITICAL(&dnsMux);
    dnsServers[(size_t)type][0] = main;
    dnsServers[(size_t)type][1] = backup;
    portEXIT_CRITICAL(&dnsMux);
    dnsDirty = true;
}

// Point the global DNS list at the active interface's servers
void Uplink::applyDns(bool onlyIfCleared)
{
    if (!onlyIfCleared)
    {
        dnsDirty = false;
    }
    NetworkInterface *ifc = interfaceFor(active);
    if (!ifc || (onlyIfCleared && ifc->dnsIP(0) != IPAddress()))
    {
        return;
    }
    portENTER_CRITICAL(&dnsMux);
    IPAddress main = dnsServers[(size_t)active][0];
    IPAddress backup = dnsServers[(size_t)active][1];
    portEXIT_CRITICAL(&dnsMux);
    if (main == IPAddress() || ifc->dnsIP(0) == main)
    {
        return;
    }
    ifc->dnsIP(0, main);
    if (backup != IPAddress())
    {
        ifc->dnsIP(1, backup);
    }
    logger.info(String("DNS: ") + main.toString() + " (" + getActiveName() + ")");
}

// Point lwIP's default route at the active interface. ESP-IDF picks the
// default netif by route priority among netifs that are "up", and an
// Ethernet netif without a cable stays up (no address): with Ethernet
// gone it kept the default route and LTE traffic was "Host is
// unreachable" (bench, recovery AP + LTE). Set explicitly on every
// change and re-checked with the DNS servers
void Uplink::applyDefaultRoute()
{
    NetworkInterface *ifc = interfaceFor(active);
    if (!ifc || ifc->isDefault())
    {
        return;
    }
    if (ifc->setDefault())
    {
        logger.info(String("Default route: ") + getActiveName());
    }
}

NetworkInterface *Uplink::interfaceFor(Type type) const
{
    switch (type)
    {
#if defined(HAS_ETHERNET)
    case Type::Ethernet:
        return &ETH;
#endif
    case Type::WiFi:
        return &WiFi.STA;
#if defined(HAS_LTE)
    case Type::LTE:
        return &PPP;
#endif
    default:
        return nullptr;
    }
}

bool Uplink::hasLocalAccess() const
{
    return ethernetHasIP() || stationHasIP();
}

// Ethernet: oscillator, PHY, DHCP
void Uplink::startEthernet()
{
#if defined(HAS_ETHERNET)
    // The RMII reference clock comes from an external oscillator on GPIO0,
    // gated by ETH_OSC_EN_PIN (pulled down, so GPIO0 is free for boot strapping)
    pinMode(ETH_OSC_EN_PIN, OUTPUT);
    digitalWrite(ETH_OSC_EN_PIN, HIGH);
    delay(10);

    // EMAC receive task: at most 1.1 kB of the default 4 kB stack used
    ETH.setTaskStackSize(3072);
    if (!ETH.begin(ETH_PHY_LAN8720, ETH_PHY_ADDRESS, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_NRST_PIN, ETH_CLOCK_GPIO0_IN))
    {
        logger.error("Failed to start Ethernet");
        digitalWrite(ETH_OSC_EN_PIN, LOW);
        return;
    }
    ETH.setRoutePrio(ETH_ROUTE_PRIO);
    ethernetStarted = true;

    if (config.ethStatic)
    {
        IPAddress ip, mask, gateway, dns1, dns2;
        ip.fromString(config.ethIp);
        mask.fromString(config.ethMask);
        gateway.fromString(config.ethGateway);
        if (!dns1.fromString(config.ethDns1))
        {
            dns1 = gateway;
        }
        dns2.fromString(config.ethDns2);
        if (ETH.config(ip, gateway, mask, dns1, dns2))
        {
            logger.info("Ethernet static address " + ip.toString() + ", gateway " + gateway.toString() +
                        ", DNS " + dns1.toString());
        }
        else
        {
            logger.error("Ethernet static address configuration failed, using DHCP");
        }
    }
    logger.info("Ethernet started, waiting for link");
#endif
}

// Ethernet: log link and address changes
void Uplink::processEthernet()
{
#if defined(HAS_ETHERNET)
    if (!ethernetStarted)
    {
        return;
    }
    bool up = ETH.hasIP();
    if (up != ethernetWasUp)
    {
        ethernetWasUp = up;
        if (up)
        {
            logger.info("Ethernet connected (" + String(ETH.linkSpeed()) + " Mb/s " +
                        (ETH.fullDuplex() ? "FD" : "HD") + "), IP: " + ETH.localIP().toString());
        }
        else
        {
            logger.warning("Ethernet disconnected");
        }
    }
#endif
}

bool Uplink::ethernetHasIP() const
{
#if defined(HAS_ETHERNET)
    // hasIP() stays true without a link for a static address (the core
    // clears only the DHCP bit on disconnect)
    return ethernetStarted && ETH.connected() && ETH.hasIP();
#else
    return false;
#endif
}

bool Uplink::ethernetLinkUp() const
{
#if defined(HAS_ETHERNET)
    return ethernetStarted && ETH.connected();
#else
    return false;
#endif
}

IPAddress Uplink::getEthernetIP() const
{
#if defined(HAS_ETHERNET)
    if (ethernetHasIP())
    {
        return ETH.localIP();
    }
#endif
    return IPAddress();
}

const char *Uplink::getActiveName() const
{
    switch (active)
    {
    case Type::Ethernet:
        return "Ethernet";
    case Type::WiFi:
        return "WiFi";
    case Type::LTE:
        return "LTE";
    default:
        return "none";
    }
}

IPAddress Uplink::getLocalIP() const
{
    switch (active)
    {
    case Type::Ethernet:
        return getEthernetIP();
    case Type::WiFi:
        return WiFi.STA.localIP();
#if defined(HAS_LTE)
    case Type::LTE:
        return lte->localIP();
#endif
    default:
        return IPAddress();
    }
}

// Cached when the AP starts: web handlers never call into the driver
IPAddress Uplink::getApIP() const
{
    return IPAddress((uint32_t)apIp.load());
}

// (Re)start SNTP; the result arrives in onTimeSync(). SNTP itself re-syncs
// periodically and retries after failures, without touching the clock.
void Uplink::startTimeSync()
{
    // Accept an NTP server announced by DHCP (router) in the first slot
    esp_sntp_servermode_dhcp(true);
    String tz;
    {
        std::lock_guard<std::recursive_mutex> guard(config.mutex());
        tz = config.timezone;
    }
    configTzTime(tz.c_str(), NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

// SNTP task
void Uplink::onTimeSync(struct timeval *tv)
{
    lastTimeSync = (uint32_t)(tv ? tv->tv_sec : time(nullptr));
    timeSyncedFlag = true;
}

bool Uplink::isTimeValid() const
{
    return time(nullptr) > 1700000000; // after 2023-11
}

bool Uplink::isTimeStale() const
{
    return lastTimeSync == 0 || time(nullptr) - (time_t)lastTimeSync > TIME_STALE_AFTER_S;
}

String Uplink::getTimeStatus() const
{
    if (!isTimeValid())
    {
        return "not synchronized";
    }
    struct tm now;
    time_t t = time(nullptr);
    localtime_r(&t, &now);
    char text[24];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &now);
    String status = text;
    long age = (long)(t - (time_t)lastTimeSync);
    if (age < 120)
    {
        status += " (synced " + String(age) + " s ago)";
    }
    else if (age < 7200)
    {
        status += " (synced " + String(age / 60) + " min ago)";
    }
    else
    {
        status += " (synced " + String(age / 3600) + " h ago)";
    }
    if (isTimeStale())
    {
        status += " - not verified for over " + String(TIME_STALE_AFTER_S / 3600) + " h";
    }
    return status;
}
