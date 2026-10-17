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
#include <esp_sntp.h>
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

// Station runs whenever credentials exist and the device is not in config mode
bool Uplink::stationEnabled() const
{
    return !config.configMode && config.wifiSSID.length() > 0;
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
    if (dnsDirty)
    {
        applyDns();
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
    bool wantAp;
    if (local)
    {
        // Keep the AP for AP_TIMEOUT after local access came up
        wantAp = apActive && (now - localAccessSince < AP_TIMEOUT);
    }
    else if (!stationEnabled())
    {
        // No credentials - the AP is the only way in
        wantAp = true;
    }
    else
    {
        // Restore the AP only after a sustained outage
        wantAp = apActive || (now - localAccessLostAt > AP_RESTORE_DELAY);
    }

    if (wantAp && !apActive && now - lastApAttempt > AP_RETRY_INTERVAL)
    {
        lastApAttempt = now;
        startAp();
    }
    else if (!wantAp && apActive)
    {
        logger.info("AP timeout reached with local network access. Switching to client mode only.");
        stopAp();
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

    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    // Channel 6, open, max 4 clients (the radio follows the station's channel when connected)
    if (WiFi.softAP(apName.c_str(), nullptr, 6, 0, 4))
    {
        apActive = true;
        logger.info("AP started with SSID: " + apName + ", IP: " + WiFi.softAPIP().toString());
    }
    else
    {
        logger.error("Failed to start AP: " + apName);
    }
}

// Stop the AP, keep the station
void Uplink::stopAp()
{
    WiFi.softAPdisconnect(true);
    apActive = false;
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
    else if (WiFi.STA.hasIP())
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
void Uplink::applyDns()
{
    dnsDirty = false;
    NetworkInterface *ifc = interfaceFor(active);
    if (!ifc)
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
    return ethernetHasIP() || WiFi.STA.hasIP();
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

IPAddress Uplink::getApIP() const
{
    return apActive ? WiFi.softAPIP() : IPAddress();
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
