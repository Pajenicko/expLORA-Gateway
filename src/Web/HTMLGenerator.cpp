/**
 * expLORA Gateway
 *
 * HTML content generator implementation file
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

#include "HTMLGenerator.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <atomic>
#include <Arduino.h>
#include <WiFi.h>
#include "../Hardware/PowerSource.h"
#include "../Network/Uplink.h"
#include "../config.h"
#include "../Hardware/RadioTask.h"
#include "../Data/TimeZones.h"
#include "../Protocol/OtaImageSigned.h"
#include "WebAssets.h"
#include "ApiRow.h"
#include "../Protocol/Gen2Values.h"

static std::atomic<bool> advancedMode{false};

void HTMLGenerator::setAdvanced(bool advanced)
{
    advancedMode = advanced;
}

bool HTMLGenerator::adv()
{
    return advancedMode.load();
}

String HTMLGenerator::htmlEscape(const String &text)
{
    String out;
    out.reserve(text.length() + 8);
    for (size_t i = 0; i < text.length(); i++)
    {
        char c = text[i];
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

// Adding HTML header code
// Page head, header with the navigation (by route) and <main>. Without
// navigation (login, first password) only the board name
void HTMLGenerator::addHtmlHeader(String &html, const String &title, Page page, bool isAPMode, bool navigation,
                                  uint16_t refreshS)
{
    html += "<!DOCTYPE html><html lang=en><head><meta charset=UTF-8><meta name=viewport "
            "content='width=device-width,initial-scale=1'><title>";
    html += title;
    html += " - " BOARD_NAME "</title><link rel=stylesheet href='/a.css?v=";
    html += String(WebAssets::TOKEN, HEX);
    html += "'>";
    if (refreshS)
    {
        html += "<meta http-equiv=refresh content=" + String(refreshS) + ">";
    }
    html += "</head><body><header><b>" BOARD_NAME "</b>";
    if (navigation)
    {
        addNavigation(html, page, isAPMode);
    }
    html += "</header><main>";
}

String HTMLGenerator::generateLoginPage(bool hasPassword, const String &error, const String &next)
{
    String html;
    html.reserve(1536);
    addHtmlHeader(html, "Login", Page::None, false, false);
    html += "<section><h2>Log in</h2>";
    if (error.length() > 0)
    {
        html += "<p class=e>" + htmlEscape(error) + "</p>";
    }
    if (!hasPassword)
    {
        html += "<p>No admin password is set yet: log in with an empty password and choose one.</p>";
    }
    html += "<form method=post action='/login'><input type=hidden name=next value='" + htmlEscape(next) +
            "'><label for=pw>Password</label><input type=password id=pw name=password "
            "autocomplete=current-password autofocus><button>Log in</button></form></section>";
    addHtmlFooter(html);
    return html;
}

// Local time with the zone, its UTC offset and whether summer time applies
static String localTimeText(const String &posix)
{
    if (!uplink->isTimeValid())
    {
        return "not synchronized yet";
    }
    struct tm now;
    time_t t = time(nullptr);
    localtime_r(&t, &now);
    char text[40];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %Z", &now);
    char offset[8];
    strftime(offset, sizeof(offset), "%z", &now); // "+0200"
    String result = String(text) + " (UTC" + String(offset).substring(0, 3) + ":" + String(offset).substring(3) + ", ";
    if (now.tm_isdst > 0)
    {
        result += "summer time)";
    }
    else
    {
        result += posix.indexOf(',') >= 0 ? "standard time)" : "no summer time)";
    }
    return result;
}

String HTMLGenerator::generatePasswordPage(bool hasPassword, const String &error, bool apMode, const SystemView &view)
{
    String html;
    html.reserve(hasPassword && view.full ? 6144 : 2048);
    // Until a password exists the user cannot go anywhere else
    addHtmlHeader(html, "System", Page::System, apMode, hasPassword);
    if (error.length() > 0)
    {
        html += "<p class=e>" + htmlEscape(error) + "</p>";
    }
    if (hasPassword && view.full)
    {
        const bool a = adv();
        html += String("<section><h2>Interface</h2><form method=post action='/system/mode'><label class=c><input "
                       "type=radio name=mode value=simple") +
                (a ? "" : " checked") +
                "> Simple - sensors and basic settings</label><label class=c><input type=radio name=mode "
                "value=advanced" +
                (a ? " checked" : "") +
                "> Advanced - also gen 2 radio and security details, logs</label><p class=m>Applies to everyone who "
                "logs in.</p><button>Save</button></form></section>";
        html += "<section><h2>Time zone</h2><p>" + localTimeText(view.timezone) +
                "</p><form method=post action='/timezone'>";
        addTimezoneSelect(html, view.timezoneName, view.timezone);
        html += "<p class=m>Summer time follows the rules of the zone; applies at once.</p><button>Save</button>"
                "</form></section>";
    }
    html += "<section><h2>" + String(hasPassword ? "Admin password" : "Set the admin password") + "</h2>";
    html += hasPassword ? "<p class=m>Forgotten: send <code>reset-password</code> over the USB console (115200 Bd).</p>"
                        : "<p>The web interface is not protected yet: choose a password (at least 8 characters).</p>";
    html += "<form method=post action='/password'><div class=g>";
    if (hasPassword)
    {
        html += "<div><label for=current>Current</label><input type=password id=current name=current "
                "autocomplete=current-password required></div>";
    }
    html += "<div><label for=password>New</label><input type=password id=password name=password "
            "autocomplete=new-password minlength=8 maxlength=64 required></div><div><label for=confirm>Repeat"
            "</label><input type=password id=confirm name=confirm autocomplete=new-password minlength=8 "
            "maxlength=64 required></div></div><button>Change password</button></form></section>";
    if (hasPassword && view.full)
    {
        html += "<section><h2>Restart</h2><a class='b b2' href='/reboot' onclick='return confirm(\"Restart the "
                "gateway?\")'>Restart gateway</a></section>";
    }
    addHtmlFooter(html);
    return html;
}

// Adding HTML footer code
void HTMLGenerator::addHtmlFooter(String &html)
{
    html += adv() ? "</main><footer>" BOARD_NAME " v" FIRMWARE_VERSION " &middot; Advanced mode</footer>"
                  : "</main><footer>" BOARD_NAME " v" FIRMWARE_VERSION " &middot; Simple mode</footer>";
    html += "<script src='/a.js?v=";
    html += String(WebAssets::TOKEN, HEX);
    html += "'></script></body></html>";
}

// Adding CSS for pages


// Adding JavaScript for interactive elements


// Adding navigation
// Navigation: the full one, or the access point's (setup) one
// Navigation: the full one (Logs in Advanced only), or the access point's
// (setup) one
void HTMLGenerator::addNavigation(String &html, Page active, bool apMode)
{
    struct Item
    {
        Page page;
        const char *href, *label;
    };
    static const Item full[] = {{Page::Home, "/", "Home"},           {Page::Sensors, "/sensors", "Sensors"},
                                {Page::Network, "/network", "Network"}, {Page::Mqtt, "/mqtt", "MQTT"},
                                {Page::Firmware, "/firmware", "Firmware"}, {Page::Logs, "/logs", "Logs"},
                                {Page::Api, "/api", "API"},          {Page::System, "/password", "System"},
                                {Page::None, "/logout", "Log out"}};
    static const Item ap[] = {{Page::Config, "/config", "WiFi setup"},
#if defined(HAS_ETHERNET) || defined(HAS_LTE)
                              {Page::Network, "/network", "Network"},
#endif
                              {Page::System, "/password", "System"},
                              {Page::None, "/logout", "Log out"}};
    const Item *items = apMode ? ap : full;
    const size_t n = apMode ? sizeof(ap) / sizeof(ap[0]) : sizeof(full) / sizeof(full[0]);
    const bool a = adv();
    html += "<nav>";
    for (size_t i = 0; i < n; i++)
    {
        if (items[i].page == Page::Logs && !a)
        {
            continue;
        }
        html += "<a href='";
        html += items[i].href;
        html += items[i].page == active && active != Page::None ? "' class=on>" : "'>";
        html += items[i].label;
        html += "</a>";
    }
    html += "</nav>";
}

// In HTMLGenerator.cpp, modify the generateHomePage method
// "3 d 4 h 5 m 6 s" of the uptime
static String uptimeText()
{
    const unsigned long s = millis() / 1000;
    const unsigned d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
    String t;
    if (d)
        t += String(d) + " d ";
    if (d || h)
        t += String(h) + " h ";
    if (d || h || m)
        t += String(m) + " m ";
    return t + String(s % 60) + " s";
}

static void kvRow(String &html, const char *key, const String &value)
{
    html += "<tr><td>";
    html += key;
    html += "</td><td>";
    html += value;
    html += "</td></tr>";
}

// Generating page with sensor list
// Format milliseconds-since-event as a short relative string ("3 min ago",
// "2 h ago", "5 d ago"). Returns "—" if `hasValue` is false.
static String formatRelativeMillis(unsigned long nowMillis, unsigned long thenMillis, bool hasValue)
{
    if (!hasValue) return "never";
    unsigned long deltaMs = nowMillis - thenMillis; // wrap-safe via unsigned subtraction
    unsigned long secs = deltaMs / 1000UL;
    if (secs < 60)   return String(secs) + " s ago";
    unsigned long mins = secs / 60UL;
    if (mins < 60)   return String(mins) + " min ago";
    unsigned long hrs = mins / 60UL;
    if (hrs < 48)    return String(hrs) + " h ago";
    return String(hrs / 24UL) + " d ago";
}

// Readings of a generation 2 sensor in one line
// A reading with its unit for the pages
static String qtyText(Qty::Id q, float v)
{
    static const char *const units[Qty::COUNT] = {" &deg;C", " %",  " hPa",      " ppm",  " lx", " m/s",
                                                  "&deg;",   " mm", " mm today", " mm/h", " V",  " dBm"};
    static const uint8_t decimals[Qty::COUNT] = {1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 2, 0};
    return String(v, (unsigned)decimals[q]) + units[q];
}

// Quantities below `end` whose last reading failed (generation 2 invalid-value
// sentinel), appended to a readings line by their names
static void addFailed(String &t, uint16_t bad, int end)
{
    static const char *const names[Qty::COUNT] = {"Temperature", "Humidity",  "Pressure",   "CO2",
                                                  "Light",       "Wind speed", "Wind direction", "Rain",
                                                  "Daily rain",  "Rain rate", "Battery",    "RSSI"};
    for (int i = 0; i < end; i++)
    {
        if (bad & Qty::mask((Qty::Id)i))
        {
            t += (t.length() ? " &middot; <span class=w>" : "<span class=w>") + String(names[i]) + " failed</span>";
        }
    }
}

String HTMLGenerator::generateHomePage(const std::vector<SensorData> &sensors, const String &mqttStatus)
{
    const bool a = adv();
    const unsigned long now = millis();
    String html;
    html.reserve(1536 + 200 * sensors.size());
    addHtmlHeader(html, "Home", Page::Home, uplink->isCaptive(), true, 60);
    html += "<section><h2>Gateway</h2><table class=kv>";
    kvRow(html, "Internet",
          uplink->isOnline() ? "<span class=ok>&#9679;</span> " + String(uplink->getActiveName()) + ", " +
                                   uplink->getLocalIP().toString()
                             : String("<span class=e>&#9679;</span> offline"));
    if (a)
    {
#if defined(HAS_ETHERNET)
        kvRow(html, "Ethernet",
              uplink->ethernetLinkUp()
                  ? "connected" + String(uplink->getEthernetIP() ? ", " + uplink->getEthernetIP().toString() : String())
                  : String("disconnected"));
#endif
        kvRow(html, "WiFi",
              uplink->wifiSwitchedOff()    ? String("off")
              : uplink->stationConnected() ? "connected to " + htmlEscape(WiFi.SSID())
                                           : String("disconnected"));
        if (uplink->isApActive())
        {
            kvRow(html, "Access point", uplink->getApIP().toString());
        }
    }
    struct tm timeinfo;
    char timeStr[24] = "not set";
    if (getLocalTime(&timeinfo, 0))
    {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &timeinfo);
    }
    kvRow(html, "Time", timeStr);
    kvRow(html, "Uptime", uptimeText());
    kvRow(html, "MQTT", htmlEscape(mqttStatus));
    html += "</table></section>";
    if (!sensors.empty())
    {
        html += "<section><h2>Sensors</h2><table><tr><th>Name</th><th>Readings</th><th class=hm>Battery</th><th>Last "
                "seen</th></tr>";
        for (const SensorData &s : sensors)
        {
            // Readings in Qty order (battery and signal in their own columns)
            const uint16_t valid = s.qValid() & s.qMask();
            String readings;
            for (int i = 0; i < Qty::Battery; i++)
            {
                if (valid & Qty::mask((Qty::Id)i))
                {
                    readings += (readings.length() ? " &middot; " : "") + qtyText((Qty::Id)i, s.value((Qty::Id)i));
                }
            }
            // A failed battery reading is named here too (its column shows "-")
            addFailed(readings, s.gen2 ? s.gen2QBad : 0, Qty::Rssi);
            html += "<tr><td>" + htmlEscape(s.name) + "<br><small>" + (s.gen2 ? "Gen 2" : s.typeName()) +
                    "</small></td><td>" + (readings.length() ? readings : String("<span class=m>no data yet</span>")) +
                    "</td><td class=hm>" +
                    ((valid & Qty::mask(Qty::Battery)) ? qtyText(Qty::Battery, s.batteryVoltage) : String("&ndash;")) +
                    "</td><td" + (s.lastSeen ? ">" : " class=w>") + formatRelativeMillis(now, s.lastSeen, s.lastSeen > 0) +
                    "</td></tr>";
        }
        html += "</table></section>";
    }
    addHtmlFooter(html);
    return html;
}


// Generating configuration page
// WiFi settings form, used by the AP setup page and the network page
void HTMLGenerator::addWiFiForm(String &html, const String &ssid, bool hasPassword, const ConfigManager *timeZone)
{
    html += "<section><h2>WiFi</h2>";
#if defined(HAS_WIFI_SWITCH)
    if (uplink->wifiSwitchedOff())
    {
        html += "<p class=w>WiFi is switched off: saving WiFi settings here switches it back on (restart).</p>";
    }
#endif
    // Stored secrets never go into the page; empty = keep the stored one
    html += "<form method=post action='/config'><label for=ssid>Network (SSID)</label><input id=ssid name=ssid "
            "maxlength=32 required value='" +
            htmlEscape(ssid) + "'><label for=wpw>Password</label><input type=password id=wpw name=password "
            "autocomplete=new-password placeholder='" +
            String(hasPassword ? "(unchanged)" : "(open network)") + "'>";
    if (timeZone)
    {
        addTimezoneSelect(html, timeZone->timezoneName, timeZone->timezone);
    }
    html += "<button>Save and restart</button></form></section>";
}

// Time zone select (field "timezone"): the names of Data/TimeZones.h (an
// option's value is its text), a custom POSIX string from an old setting
// as an extra entry with an empty value (= keep it)
void HTMLGenerator::addTimezoneSelect(String &html, const String &zoneName, const String &zonePosix)
{
    html += "<label for=timezone>Time zone</label><select id=timezone name=timezone>";
    if (zoneName.length() == 0)
    {
        html += "<option value='' selected>Custom: " + htmlEscape(zonePosix) + "</option>";
    }
    for (const TimeZones::Zone &zone : TimeZones::ZONES)
    {
        html += zoneName == zone.name ? "<option selected>" : "<option>";
        html += zone.name;
    }
    html += "</select>";
}

String HTMLGenerator::generateConfigPage(const String &ssid, const String &password, bool configMode, const String &ip,
                                        const ConfigManager &config)
{
    String html;
    html.reserve(6144);
    addHtmlHeader(html, "WiFi setup", Page::Config, configMode);
    html += "<section><h2>Status</h2><table class=kv>";
    if (configMode)
    {
        kvRow(html, "Access point", htmlEscape(uplink->getApName()) + ", " + uplink->getApIP().toString());
    }
    else
    {
        kvRow(html, "WiFi", htmlEscape(ssid) + ", " +
                                (uplink->stationHasIP() ? "connected, " + uplink->getStationIP().toString()
                                                        : String("disconnected")));
    }
    html += "</table></section>";
    addWiFiForm(html, ssid, password.length() > 0, &config);
    addHtmlFooter(html);
    return html;
}
// Generate MQTT Configuration Page
// Everything below "Enable MQTT" only while it is checked; TLS, root topic
// and discovery topic in Advanced only. The hidden markers f (settings
// posted) and adv (Advanced fields posted) sit inside the hidden part: the
// handler keeps every stored value whose field was not posted
String HTMLGenerator::generateMqttPage(const String &host, int port, const String &user, const String &password, bool enabled, bool tls,
                                       const String &prefix, bool haEnabled, String &haPrefix, const String &status)
{
    const bool a = adv();
    String html;
    html.reserve(2560);
    addHtmlHeader(html, "MQTT", Page::Mqtt);
    html += "<section><h2>MQTT</h2><p>Status: " + htmlEscape(status) + "</p><form method=post action='/mqtt'>";
    html += "<label class=c><input type=checkbox id=enabled name=enabled value=1" + String(enabled ? " checked" : "") +
            "> Enable MQTT</label><div data-if=enabled><input type=hidden name=f value=1>";
    html += "<div class=g><div><label for=host>Broker host</label><input id=host name=host maxlength=64 required "
            "value='" +
            htmlEscape(host) + "'></div><div><label for=port>Port</label><input type=number id=port name=port min=1 "
            "max=65535 required value='" +
            String(port) + "'></div><div><label for=user>User <small>(optional)</small></label><input id=user "
            "name=user maxlength=64 value='" +
            htmlEscape(user) + "'></div><div><label for=mpw>Password <small>(optional)</small></label><input "
            "type=password id=mpw name=password maxlength=64 autocomplete=new-password placeholder='" +
            String(password.length() > 0 ? "(unchanged)" : "") + "'></div></div>";
    if (password.length() > 0)
    {
        html += "<label class=c><input type=checkbox name=clearPassword value=1> Remove the stored password</label>";
    }
    html += "<label class=c><input type=checkbox id=haEnabled name=haEnabled value=1" +
            String(haEnabled ? " checked" : "") + "> Home Assistant discovery</label>";
    if (a)
    {
        html += "<input type=hidden name=adv value=1><label class=c><input type=checkbox id=tls name=tls value=1" +
                String(tls ? " checked" : "") + "> TLS <small>(port 8883; the broker certificate is not checked)"
                "</small></label>";
#if defined(HAS_ETHERNET)
        html += "<p class=m>On the Pro a TLS session needs more memory than is free with every interface on.</p>";
#endif
        html += "<div class=g><div><label for=prefix>Root topic</label><input id=prefix name=prefix maxlength=32 "
                "value='" +
                htmlEscape(prefix) + "'></div><div><label for=haPrefix>Discovery topic</label><input id=haPrefix "
                "name=haPrefix maxlength=32 value='" +
                htmlEscape(haPrefix) + "'></div></div>";
    }
    html += "</div><button>Save</button></form></section>";
    addHtmlFooter(html);
    return html;
}

String HTMLGenerator::generateNetworkPagePart(const NetworkPageView &view, size_t part)
{
    // Every piece is generated when the response has room for it; the
    // largest are the status and the WiFi card (< 2 kB)
    const bool a = adv();
    String html;
    html.reserve(1536);
    switch (part)
    {
    case 0:
        addHtmlHeader(html, "Network", Page::Network, view.apMode);
        break;

    case 1:
    {
        // Live uplink state, read in one go (one piece)
        html += "<section><h2>Status</h2><table class=kv>";
        String via = uplink->isOnline() ? "<span class=ok>&#9679;</span> " + String(uplink->getActiveName()) + ", " +
                                              uplink->getLocalIP().toString()
                                        : String("<span class=e>&#9679;</span> offline");
#if defined(HAS_ETHERNET) || defined(HAS_LTE)
        via += " <small>(order: Ethernet, WiFi, LTE)</small>";
#endif
        kvRow(html, "Internet via", via);
#if defined(HAS_ETHERNET)
        kvRow(html, "Ethernet",
              (uplink->getEthernetIP() ? "connected, " + uplink->getEthernetIP().toString()
               : uplink->ethernetLinkUp() ? String(view.ethStatic ? "link up" : "link up, waiting for DHCP")
                                          : String("disconnected")) +
                  (view.ethStatic ? " <small>(static)</small>" : ""));
#endif
        kvRow(html, "WiFi",
              uplink->wifiSwitchedOff() ? String(uplink->isApActive() ? "off (access point up for recovery)" : "off")
              : view.ssid.length() == 0 ? String("not set up")
                                        : (uplink->stationHasIP() ? "connected, " + uplink->getStationIP().toString()
                                                                  : String("disconnected")) +
                                              " <small>(" + htmlEscape(view.ssid) + ")</small>");
#if defined(HAS_LTE)
        {
            const LteModem &lte = uplink->getLte();
            String t = view.lteEnabled ? String(lte.getStateName()) : String("off");
            if (lte.hasIP())
            {
                t += ", " + lte.localIP().toString();
            }
            if (lte.getOperator().length() > 0)
            {
                t += ", " + htmlEscape(lte.getOperator());
            }
            if (lte.getRssiDbm() != 0)
            {
                t += ", " + String(lte.getRssiDbm()) + " dBm";
            }
            kvRow(html, "LTE", t);
            if (a && lte.getModuleName().length() > 0)
            {
                kvRow(html, "Modem", htmlEscape(lte.getModuleName()) + ", IMEI " + htmlEscape(lte.getImei()) +
                                         (lte.getOperator().length() ? ", APN " + htmlEscape(lte.getApn()) : String("")));
            }
            if (a && lte.getLastError().length() > 0)
            {
                kvRow(html, "LTE error", htmlEscape(lte.getLastError()));
            }
        }
#endif
        if (a)
        {
#if defined(HAS_PWR_SRC)
            kvRow(html, "Power", powerSource->getName());
#endif
            kvRow(html, "Time", htmlEscape(uplink->getTimeStatus()));
        }
        html += "</table></section>";
        break;
    }

    case 2:
#if defined(HAS_ETHERNET)
    {
        // Static fields only while "Static" is selected (data-if: hidden
        // fields are disabled, not posted - the stored addresses are kept)
        auto field = [&html](const char *id, const char *label, const String &value, bool required)
        {
            html += String("<div><label for=") + id + ">" + label + "</label><input id=" + id + " name=" + id +
                    " maxlength=15 inputmode=decimal pattern='^(\\d{1,3}\\.){3}\\d{1,3}$' value='" +
                    htmlEscape(value) + "'" + (required ? " required" : "") + "></div>";
        };
        html += "<section><h2>Ethernet</h2><form method=post action='/network'><input type=hidden name=section "
                "value=ethernet><label for=ethMode>Addressing</label><select id=ethMode name=ethMode><option "
                "value=dhcp" +
                String(view.ethStatic ? "" : " selected") + ">DHCP (automatic)<option value=static" +
                String(view.ethStatic ? " selected" : "") + ">Static</select><div data-if='ethMode=static' class=g>";
        field("ip", "IP address", view.ethIp, true);
        field("mask", "Subnet mask", view.ethMask.length() ? view.ethMask : String("255.255.255.0"), true);
        field("gateway", "Gateway", view.ethGateway, true);
        field("dns1", "DNS <small>(empty = gateway)</small>", view.ethDns1, false);
        field("dns2", "DNS 2 <small>(optional)</small>", view.ethDns2, false);
        html += "</div><p class=m>Check a static address before saving: a wrong one leaves only WiFi or the access "
                "point.</p><button>Save and restart</button></form></section>";
    }
#endif
        break;

    case 3:
        // One WiFi card; on the Pro the SSID and password only while "Use
        // WiFi" is on (hidden ones are not posted and are kept)
        html += "<section><h2>WiFi</h2><form method=post action='/network' id=wifiForm><input type=hidden "
                "name=section value=wifi>";
#if defined(HAS_WIFI_SWITCH)
        html += "<label class=c><input type=checkbox id=wifiEnabled name=wifiEnabled value=1" +
                String(view.wifiEnabled ? " checked" : "") + "> Use WiFi</label><div data-if=wifiEnabled>";
#endif
        html += "<label for=ssid>Network (SSID)</label><input id=ssid name=ssid maxlength=32 required value='" +
                htmlEscape(view.ssid) + "'><label for=wpw>Password</label><input type=password id=wpw name=password "
                "autocomplete=new-password placeholder='" +
                String(view.hasWifiPassword ? "(unchanged)" : "(open network)") + "'>";
#if defined(HAS_WIFI_SWITCH)
        html += "</div><p class=m>When off, the setup access point still starts for 5 min after a restart and when "
                "Ethernet is lost.</p>";
        if (view.viaWifi)
        {
            html += "<p class=w>This page is loaded over WiFi: switching it off ends this connection";
            html += view.ethAddress.length() > 0 ? " - reconnect over Ethernet at <a href='http://" +
                                                       htmlEscape(view.ethAddress) + "/network'>" +
                                                       htmlEscape(view.ethAddress) + "</a>.</p>"
                                                 : String(" (Ethernet has no address now).</p>");
        }
#endif
        html += "<button>Save and restart</button></form></section>";
#if defined(HAS_WIFI_SWITCH)
        if (view.viaWifi)
        {
            html += "<script>wifiForm.onsubmit=function(){return wifiEnabled.checked||confirm('This page is loaded "
                    "over WiFi. Switching WiFi off drops this connection after the restart. Continue?')}</script>";
        }
#endif
        break;

    case 4:
#if defined(HAS_LTE)
        // APN and PIN only while LTE is on (hidden ones are kept)
        html += "<section><h2>LTE</h2><form method=post action='/network'><input type=hidden name=section "
                "value=lte><label class=c><input type=checkbox id=lteEnabled name=lteEnabled value=1" +
                String(view.lteEnabled ? " checked" : "") +
                "> Use LTE as a backup</label><div data-if=lteEnabled><div class=g><div><label for=apn>APN "
                "<small>(empty = automatic)</small></label><input id=apn name=apn maxlength=63 value='" +
                htmlEscape(view.lteApn) + "'></div><div><label for=pin>SIM PIN</label><input type=password id=pin "
                "name=pin maxlength=8 inputmode=numeric pattern='[0-9]{4,8}' autocomplete=off placeholder='" +
                String(view.hasLtePin ? "(unchanged)" : "") + "'></div></div>";
        if (view.hasLtePin)
        {
            html += "<label class=c><input type=checkbox name=clearPin value=1> Remove the stored PIN</label>";
        }
        html += "</div><p class=m>Used only when neither Ethernet nor WiFi is available; leave it off without a "
                "modem.</p><button>Save and restart</button></form></section>";
#endif
        break;

    case 5:
        addHtmlFooter(html);
        break;
    }
    return html;
}

// Boot state of status report byte 22 (spec 10.3)
static String bootStateText(uint8_t state)
{
    switch (state)
    {
    case 0:
        return "normal";
    case 1:
        return "updated, not confirmed yet";
    case 2:
        return "updated and confirmed";
    case 3:
        return "reverted (new image not confirmed)";
    case 4:
        return "update rejected (invalid image)";
    case 5:
        return "update rejected (HW type / identity)";
    case 6:
        return "unconfirmed image kept (previous image invalid)";
    case 7:
        return "update aborted (flash verify)";
    case 0xFF:
        return "unknown";
    default:
        return "state " + String(state);
    }
}


// Render the health status as a coloured inline-style badge plus optional
// secondary info (last reject reason, recent counts).
// Health status in a word, coloured
static String renderHealthBadge(const SensorHealth::Stats &h, unsigned long nowMillis)
{
    switch (SensorHealth::status(h, nowMillis))
    {
    case SensorHealth::Status::Healthy:
        return "<span class=ok>OK</span>";
    case SensorHealth::Status::Errors:
        return "<span class=e>Errors</span>";
    case SensorHealth::Status::Stale:
        return "<span class=w>Stale</span>";
    default:
        return "<span class=m>Unknown</span>";
    }
}

// Status of a generation 2 sensor in the sensor list: OK, or Stale after
// three report intervals (+ 10 min) without an uplink
static String gen2StatusText(const Gen2Sensor &g, unsigned long nowMs)
{
    if (!g.seen)
    {
        return "<span class=w>Waiting for the first report</span>";
    }
    const unsigned long staleMs = (unsigned long)g.config.intervalS * 3000UL + 600000UL;
    return nowMs - g.lastSeenMs > staleMs ? "<span class=w>Stale</span>" : "<span class=ok>OK</span>";
}

static String devIdText(uint32_t devId)
{
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)devId);
    return id;
}

// Parts: 0 header, 1 table head, one per v1 row, one per gen 2 row, the
// end of the table with the waiting list, (Advanced) the gen 2 radio card
// head and one part per gen 2 row, the footer
size_t HTMLGenerator::sensorsPageParts(const SensorsPageView &view)
{
    const size_t g = view.gen2.size();
    return 4 + view.entries.size() + g + (view.adv ? 2 + g : 0);
}

String HTMLGenerator::generateSensorsPagePart(const SensorsPageView &view, size_t part)
{
    // One piece per sensor row, so the page never sits in RAM; the largest
    // pieces are the header, the footer and the waiting list (8 rows), each
    // below 3 kB. Every row computes ages against the same 'now'
    const size_t entries = view.entries.size(), gen2 = view.gen2.size();
    const bool a = view.adv;
    const unsigned long now = view.nowMillis;
    String html;
    html.reserve(768);
    if (part == 0)
    {
        html.reserve(1536);
        addHtmlHeader(html, "Sensors", Page::Sensors);
        return html;
    }
    if (part == 1)
    {
        html += "<section><h2>Sensors</h2>";
        if (entries + gen2 == 0)
        {
            html += "<p>No sensors added yet.</p>";
        }
        else
        {
            html += a ? "<table><tr><th>Name</th><th>Type</th><th>ID</th><th>Status</th><th>Last seen</th><th>24h</th>"
                        "<th></th></tr>"
                      : "<table><tr><th>Name</th><th class=hm>Type</th><th>Status</th><th class=hm>Last seen</th><th>"
                        "</th></tr>";
        }
        return html;
    }
    part -= 2;
    if (part < entries)
    {
        const ActiveSensorEntry &entry = view.entries[part];
        const SensorData &s = entry.data;
        const SensorHealth::Stats &h = s.health;
        html += "<tr><td>" + htmlEscape(s.name) + "</td><td" + (a ? ">" : " class=hm>") +
                sensorTypeToString(s.deviceType) + "</td>";
        if (a)
        {
            html += "<td><code>" + formatSN(s.serialNumber) + "</code></td>";
        }
        html += "<td>" + renderHealthBadge(h, now);
        if (a && h.hasLastReject && h.consecutiveRejections > 0)
        {
            html += "<br><small>last reject: " + htmlEscape(String(h.lastRejectReason)) + " (" +
                    formatRelativeMillis(now, h.lastRejectAtMillis, true) + ")</small>";
        }
        html += String("</td><td") + (a ? ">" : " class=hm>") + formatRelativeMillis(now, h.lastOkAtMillis, h.hasLastOk) +
                "</td>";
        if (a)
        {
            // 24 h sliding window: OK count, rejections in red
            const uint32_t ok = SensorHealth::okLast24h(h), bad = SensorHealth::rejectedLast24h(h);
            html += "<td>" + (ok == 0 && bad == 0 ? String("&ndash;") : String(ok)) +
                    (bad ? " <span class=e>" + String(bad) + " err</span>" : String("")) + "</td>";
        }
        html += "<td><a class=b href='/sensors/edit?index=" + String(entry.index) + "'>Edit</a></td></tr>";
        return html;
    }
    part -= entries;
    if (part < gen2)
    {
        const Gen2Sensor &g = view.gen2[part];
        const String id = devIdText(g.devId);
        html += "<tr><td>" + htmlEscape(g.name) + "</td><td" + (a ? ">" : " class=hm>") + "Gen 2</td>";
        if (a)
        {
            html += "<td><code>" + id + "</code></td>";
        }
        html += "<td>" + gen2StatusText(g, now) + "</td><td" + (a ? ">" : " class=hm>") +
                formatRelativeMillis(now, g.lastSeenMs, g.seen) + "</td>";
        if (a)
        {
            html += "<td>&ndash;</td>";
        }
        html += "<td><a class=b href='/sensors/gen2?devId=" + id + "'>Edit</a></td></tr>";
        return html;
    }
    part -= gen2;
    if (part == 0)
    {
        html.reserve(2560);
        if (entries + gen2 > 0)
        {
            html += "</table>";
        }
        html += "<a class=b href='/sensors/add'>Add sensor</a><a class='b b2' href='/sensors/add2'>Add gen 2 "
                "sensor</a>";
        if (view.waitingCount > 0)
        {
            html += a ? "<h3>Heard, not added yet</h3><table><tr><th>DevID</th><th>HW / FW</th><th>Signal</th>"
                        "<th>Requests</th><th>Heard</th><th></th></tr>"
                      : "<h3>Heard, not added yet</h3><table><tr><th>DevID</th><th>Signal</th><th>Heard</th><th>"
                        "</th></tr>";
            for (size_t i = 0; i < view.waitingCount; i++)
            {
                const WaitingEntry &w = view.waiting[i];
                const String id = devIdText(w.devId);
                html += "<tr><td><code>" + id + "</code></td>";
                if (a)
                {
                    html += "<td>HW " + String(w.hwType) + ", FW " + String(w.fwMajor) + "." + String(w.fwMinor) + "." +
                            String(w.fwPatch) + "</td>";
                }
                html += "<td>" + String(w.rssi) + " dBm</td>";
                if (a)
                {
                    html += "<td>" + String(w.frames) + "</td>";
                }
                html += "<td>" + formatRelativeMillis(now, w.lastSeenMs, true) + "</td><td><a class=b "
                        "href='/sensors/add2?devId=" + id + "'>Add</a></td></tr>";
            }
            html += "</table><p class=m>Compare the DevID with the label on the sensor.</p>";
        }
        html += "</section>";
        return html;
    }
    part -= 1;
    if (a && part == 0)
    {
        // Downlink duty cycle of the gateway (869.525 MHz, 10 % = 360 s per hour)
        RadioTask::Stats rs = RadioTask::stats();
        const bool high = (uint64_t)rs.dutyUsedMs * 100 >= 360000ULL * 80;
        html += "<section><h2>Gen 2 radio details</h2><p class=m>Downlink duty cycle: " +
                String(rs.dutyUsedMs / 1000.0f, 1) + " s of 360 s in the last hour (JOIN_ACC " +
                String(rs.joinUsedMs / 1000.0f, 1) + " s); responses RX1 " + String(rs.txRx1) + ", RX2 " +
                String(rs.txRx2) + "</p>";
        if (high)
        {
            html += "<p class=e>Above 80 % &ndash; only join answers once the data share is used up.</p>";
        }
        if (gen2 > 0)
        {
            html += "<table><tr><th>Name</th><th>Key</th><th>Reported</th><th>Last heard</th><th>Counters</th></tr>";
        }
        return html;
    }
    if (a && part >= 1 && part < 1 + gen2)
    {
        const Gen2Sensor &g = view.gen2[part - 1];
        // The label key is the root secret: only its first 4 hex digits,
        // enough to tell which label was entered
        char key[16];
        snprintf(key, sizeof(key), "%02X%02X&hellip;", g.label[0], g.label[1]);
        html.reserve(1280);
        html += "<tr><td>" + htmlEscape(g.name) + "</td><td><code>" + String(key) + "</code></td><td>";
        if (g.hasFw)
        {
            html += "HW " + String(g.hwType) + ", FW " + String(g.fwMajor) + "." + String(g.fwMinor) + "." +
                    String(g.fwPatch);
            if (g.bootState != 0xFF && g.bootState != 0)
            {
                html += "<br><small>" + bootStateText(g.bootState) + "</small>";
            }
            html += "<br>";
        }
        html += g.seen ? "<small>" + String(g.rssi) + " dBm, SNR " + String(g.snr, 1) + " dB, " +
                             String(g.freqError / 1000.0f, 1) + " kHz</small>"
                       : String("&ndash;");
        html += "</td><td>" + formatRelativeMillis(now, g.lastSeenMs, g.seen);
        if (g.seen)
        {
            html += "<br><small>devNonce " + String(g.lastJoinDevNonce) + "<br>";
            html += g.sessionUp ? "session up, FCnt " + String(g.lastFcnt) : String("no session yet");
            if (g.sessionUp && !Gen2Config::inSync(g.configVersion, g.cfgVersion))
            {
                html += "<br><span class=w>config v" + String(g.cfgVersion) + " waiting for the sensor</span>";
            }
            if (g.downlinkRssi != INT8_MIN)
            {
                html += "<br>downlink " + String(g.downlinkRssi) + " dBm at the sensor";
            }
            html += "</small>";
        }
        html += "</td><td><small>JOIN_REQ " + String(g.joinRequests) + "<br>MIC failed " + String(g.micFailures);
        if (g.micFailures > 0 && g.joinRequests == 0)
        {
            html += " <span class=e>(wrong key, or paired with another gateway?)</span>";
        }
        html += "<br>replayed " + String(g.replayedJoins) + "<br>UP " + String(g.upFrames) + ", w/o session " +
                String(g.upWithoutSession) + "<br>JOIN_ACC " + String(g.joinAccepts) + ", REJOIN " +
                String(g.rejoins) + "<br>DOWN RX1 " + String(g.downRx1) + ", RX2 " + String(g.downRx2) +
                ", not sent " + String(g.downNotSent) + "</small></td></tr>";
        return html;
    }
    if (a && part == 1 + gen2)
    {
        html += gen2 > 0 ? "</table></section>" : "</section>";
        return html;
    }
    html.reserve(512);
    addHtmlFooter(html);
    return html;
}

String HTMLGenerator::generateGen2AddPage(const String &devId, const String &error)
{
    String html;
    html.reserve(2048);
    addHtmlHeader(html, "Add gen 2 sensor", Page::Sensors);
    html += "<section><h2>Add gen 2 sensor</h2>";
    if (error.length() > 0)
    {
        html += "<p class=e>" + htmlEscape(error) + "</p>";
    }
    html += "<form method=post action='/sensors/add2'><label for=name>Name</label><input id=name name=name "
            "maxlength=32 required><div class=g><div><label for=devId>DevID <small>(label)</small></label><input "
            "id=devId name=devId maxlength=8 pattern='[0-9A-Fa-f]{8}' required value='" +
            htmlEscape(devId) + "'></div><div><label for=key>Key <small>(label)</small></label><input id=key name=key "
            "maxlength=19 autocomplete=off pattern='[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}' "
            "required placeholder='A1B2-C3D4-E5F6-0718'></div></div><button>Add</button><a class='b b2' "
            "href='/sensors'>Cancel</a></form></section>";
    addHtmlFooter(html);
    return html;
}

// One uplink TLV as text (known types scaled, others in hex)
static String describeTlv(const ep::Tlv &t)
{
    String name, value;
    switch (t.type)
    {
    case ep::up::kTemperature:
        name = "Temperature";
        value = String((int16_t)ep::tlvU16(t) / 100.0f, 2) + " &deg;C";
        break;
    case ep::up::kHumidity:
        name = "Humidity";
        value = String(ep::tlvU16(t) / 100.0f, 1) + " %";
        break;
    case ep::up::kPressure:
        name = "Pressure";
        value = String(ep::tlvU16(t) / 10.0f, 1) + " hPa";
        break;
    case ep::up::kCo2:
        name = "CO2";
        value = String(ep::tlvU16(t)) + " ppm";
        break;
    case ep::up::kIlluminance:
        name = "Light";
        value = String(exp2f(ep::tlvU16(t) / 2048.0f) - 1.0f, 0) + " lx";
        break;
    case ep::up::kBattery:
        name = "Battery";
        value = String(ep::tlvU16(t)) + " mV";
        break;
    case ep::up::kDownlinkRssi:
        name = "Downlink RSSI";
        value = String((int8_t)t.value[0]) + " dBm";
        break;
    case ep::up::kDownlinkSnr:
        name = "Downlink SNR";
        value = String((int8_t)t.value[0] / 4.0f, 2) + " dB";
        break;
    case ep::up::kResetCause:
        name = "Reset cause";
        value = "0x" + String(t.value[0], HEX);
        break;
    case ep::up::kRainTips:
        name = "Rain tips";
        value = String(ep::tlvU32(t));
        break;
    case ep::up::kPulses:
        name = "Pulses";
        value = String(ep::tlvU32(t));
        break;
    case ep::up::kStatus:
        name = "Status report";
        value = String(t.len) + " bytes";
        break;
    default:
        name = "Type 0x" + String(t.type, HEX);
        for (uint8_t i = 0; i < t.len && i < 16; i++)
        {
            value += (t.value[i] < 0x10 ? "0" : "") + String(t.value[i], HEX);
        }
        break;
    }
    if (t.channel != 0)
    {
        name += " (channel " + String(t.channel) + ")";
    }
    return "<tr><td>" + name + "</td><td>" + value + "</td></tr>";
}

// Status report (up::kStatus, docs/protocol-v2.md 4.1)
static String describeStatus(const uint8_t *v, size_t len)
{
    if (len < 22 || v[0] != 1)
    {
        return "<p>Unknown status format (" + String(len) + " bytes)</p>";
    }
    auto u16 = [v](size_t o) { return (uint16_t)(v[o] << 8 | v[o + 1]); };
    auto u32 = [v](size_t o) { return (uint32_t)v[o] << 24 | (uint32_t)v[o + 1] << 16 | (uint32_t)v[o + 2] << 8 | v[o + 3]; };
    String html = "<table>";
    html += "<tr><td>Firmware</td><td>" + String(v[1]) + "." + String(v[2]) + "." + String(v[3]) + "</td></tr>";
    html += "<tr><td>Reset cause</td><td>0x" + String(v[4], HEX) + "</td></tr>";
    html += "<tr><td>Uptime</td><td>" + String(u32(5) / 3600.0f, 1) + " h</td></tr>";
    html += "<tr><td>devNonce</td><td>" + String(u32(9)) + "</td></tr>";
    html += "<tr><td>Battery</td><td>" + String(u16(13)) + " mV</td></tr>";
    html += "<tr><td>TX power</td><td>" + String((int8_t)v[15]) + " dBm</td></tr>";
    html += "<tr><td>Interval</td><td>" + String(u16(16)) + " s</td></tr>";
    html += "<tr><td>ACK every</td><td>" + String(v[18]) + ". uplink</td></tr>";
    html += "<tr><td>Missed ACKs</td><td>" + String(v[19]) + "</td></tr>";
    html += "<tr><td>Config version</td><td>v" + String(v[20]) + "</td></tr>";
    html += "<tr><td>Clock</td><td>" + String((v[21] & 1) ? "set" : "not set") + "</td></tr>";
    if (v[21] & 4)
    {
        html += "<tr><td>RTC</td><td><span class=e>not usable (LSE / binary mode / clock source) "
                "&ndash; updates cannot be confirmed</span></td></tr>";
    }
    if (v[21] & 2)
    {
        html += "<tr><td>Watchdog</td><td><span class=e>not started (IWDG_STOP option byte not "
                "programmed)</span></td></tr>";
    }
    if (len >= 24)
    {
        html += "<tr><td>Boot state</td><td>" + bootStateText(v[22]) + "</td></tr>";
        html += "<tr><td>Bootloader</td><td>" + String(v[23] >> 4) + "." + String(v[23] & 0x0F) + "</td></tr>";
    }
    html += "</table>";
    return html;
}

static String txPowerWarning()
{
    return "Above the EU limit of " + String(Gen2Config::EU_TX_POWER_DBM) +
           " dBm ERP for 868.0 MHz (ETSI EN 300 220). The legal maximum depends on the country; the "
           "operator is responsible.";
}

// "worst signal -111 dBm, SNR -8 dB, margin 4 dB" (spec 4.3)
static String linkSummary(const Gen2Link::Margins &m)
{
    String t = "worst signal " + String(m.worstUp) + " dBm";
    // the reported SNR is a lower bound of the true one (spec 4.3)
    t += m.upSnrSamples > 0 ? ", SNR &ge; " + String(m.worstUpSnr > 0 ? "+" : "") + String(m.worstUpSnr) + " dB"
                            : String(", SNR unknown at this power");
    return t + ", margin " + String(m.upMargin()) + " dB" + (m.upSnrLimits() ? " (SNR-limited)" : "");
}

// TX power hint from the link history (spec 4.3); never applied automatically
static String txPowerAdvice(const Gen2Link::Margins &m, int txNow)
{
    const Gen2Link::Recommendation r = Gen2Link::recommend(m, txNow);
    const String signal = linkSummary(m);
    switch (r.advice)
    {
    case Gen2Link::Advice::Lower:
        return "Hint: " + signal + " &ndash; the TX power can be lowered to " + String(r.power) +
               " dBm (margin then " + String(m.upMargin() - (txNow - r.power)) + " dB), saves battery.";
    case Gen2Link::Advice::Raise:
        return "<span class=w>Hint: " + signal + " &ndash; consider " + String(r.power) + " dBm" +
               (r.power > Gen2Config::EU_TX_POWER_DBM
                    ? " &ndash; above the EU limit of " + String(Gen2Config::EU_TX_POWER_DBM) +
                          " dBm ERP, see the warning"
                    : String("; legal limits apply")) +
               ".</span>";
    case Gen2Link::Advice::AtMaximum:
        return "<span class=w>Hint: " + signal +
               " at the maximum power &ndash; improve the antenna or its placement.</span>";
    default:
        if (m.lost > 0)
        {
            Gen2Link::Margins noLoss = m;
            noLoss.lost = 0;
            if (Gen2Link::recommend(noLoss, txNow).advice == Gen2Link::Advice::Lower)
            {
                return "No hint to lower the power: " + String(m.lost) + (m.lost == 1 ? " UP was" : " UPs were") +
                       " lost in the window.";
            }
        }
        if (m.upValid() && m.upSnrSamples == 0)
        {
            return "No hint until an UP at this power arrives.";
        }
        return "";
    }
}

static String fwText(uint32_t fw)
{
    char v[16];
    OtaImage::versionText(fw, v, sizeof(v));
    return v;
}

static String durationText(uint32_t s)
{
    if (s < 120)
    {
        return String(s) + " s";
    }
    if (s < 2 * 3600)
    {
        return String(s / 60) + " min";
    }
    return String(s / 3600) + " h " + String((s % 3600) / 60) + " min";
}

static String otaReason(const OtaJob::Job &j, bool signedOnly)
{
    String r = OtaJob::whyText(j.why);
    if (j.why == OtaJob::Why::Unsigned && !signedOnly)
    {
        r = "unsigned image - held while \"Signed images only\" was on";
    }
    else if (j.why == OtaJob::Why::Refused)
    {
        r += String(": ") + OtaJob::refusalText(j.whyArg);
    }
    else if (j.why == OtaJob::Why::SensorFailed)
    {
        r += String(": ") + OtaJob::failureText(j.whyArg);
    }
    else if (j.why == OtaJob::Why::Left)
    {
        r += String(": ") + OtaJob::leaveText(j.whyArg);
    }
    else if (j.why == OtaJob::Why::NoProgress && j.missedTotal * 2 > j.statuses)
    {
        r += " - uplink too weak (most statuses were not received)";
    }
    else if (j.why == OtaJob::Why::Battery && j.state == OtaJob::State::Verified && j.whyArg)
    {
        r = "verified, battery " + String(j.whyArg / 10.0f, 1) + " V - installs when it is >= 2.4 V";
    }
    return r;
}

static String otaButton(const char *id, const char *action, const char *label, const char *cls = "b",
                        const char *ask = nullptr)
{
    String h = "<form method='post' action='/sensors/gen2/ota' class=i";
    if (ask)
    {
        h += " onsubmit='return confirm(\"" + String(ask) + "\")'";
    }
    h += "><input type='hidden' name='devId' value='" + String(id) + "'><input type='hidden' name='action' value='" +
         action + "'><button class='" + cls + "'>" + label + "</button></form> ";
    return h;
}

// OTA card of the generation 2 detail page (spec 14.5)
static void addGen2OtaCard(String &html, const Gen2Sensor &g, const Gen2OtaView &v, const char *id)
{
    html += "<section><h2>Firmware update over the air</h2>";
    if (v.lastEvent.length() > 0)
    {
        html += "<p><small>Last event: " + HTMLGenerator::htmlEscape(v.lastEvent) + "</small></p>";
    }
    const OtaJob::Job &j = v.job;
    const bool mine = v.hasJob && j.devId == g.devId && (j.state != OtaJob::State::None);
    if (v.hasJob && j.devId != g.devId && !OtaJob::terminal(j.state))
    {
        char other[9];
        snprintf(other, sizeof(other), "%08lX", (unsigned long)j.devId);
        html += "<p>Another OTA job is running (sensor <a href='/sensors/gen2?devId=" + String(other) + "'><code>" +
                String(other) + "</code></a>) - one at a time.</p>";
    }
    if (mine)
    {
        const uint16_t n = j.blocks();
        const uint16_t done = j.maxConfirmed < n ? j.maxConfirmed : n;
        html += "<table>";
        html += "<tr><td>State</td><td><strong>" + String(OtaJob::stateName(j.state)) + "</strong>";
        const String reason = otaReason(j, v.signedOnly);
        if (reason.length() > 0)
        {
            html += " <small>(" + HTMLGenerator::htmlEscape(reason) + ")</small>";
        }
        if (j.state == OtaJob::State::Done && j.kept)
        {
            html += " <small>installed, unconfirmed, cannot be reverted (KEPT)</small>";
        }
        if (j.state == OtaJob::State::Done && j.despiteAbort)
        {
            html += " <small>installed despite abort</small>";
        }
        if (j.manualPause && j.why == OtaJob::Why::Unsigned)
        {
            html += v.signedOnly ? " <small>held - Resume needs \"Signed images only\" off</small>"
                                 : " <small>\"Signed images only\" is off now - Resume possible</small>";
        }
        else if (j.manualPause)
        {
            html += " <small>paused by the admin</small>";
        }
        if (j.state == OtaJob::State::Aborted && j.abortKept)
        {
            html += " <small>the sensor did not confirm the abort - its transfer state may stay, and a job with the "
                    "same image would resume it</small>";
        }
        if (j.abortPending)
        {
            html += " <small>abort not delivered yet</small>";
        }
        html += "</td></tr>";
        html += "<tr><td>Image</td><td>v" + fwText(j.fw) + " <small>(" + String(j.len) + " B, from v" +
                fwText(j.baseFw) + ")</small></td></tr>";
        html += "<tr><td>Blocks confirmed / missing / sent</td><td>" + String(done) + " / " + String(n - done) +
                " / " + String(j.blocksSent) + " <small>(" + String(j.repeats) + " repeats)</small></td></tr>";
        html += "<tr><td>Rounds / statuses / missed</td><td>" + String(j.rounds) + " / " + String(j.statuses) + " / " +
                String(j.missedTotal) + "</td></tr>";
        html += "<tr><td>Airtime</td><td>" + String(j.airMs / 1000) + " s this job, " + String(v.otaHourMs / 1000) +
                " s of 172 s in the last hour</td></tr>";
        if (j.state == OtaJob::State::Transfer || j.state == OtaJob::State::Paused ||
            j.state == OtaJob::State::Waiting)
        {
            html += "<tr><td>ETA</td><td>" + durationText(OtaJob::etaS(j)) + " of transfer" +
                    (j.state == OtaJob::State::Transfer && !j.anchorReliable
                         ? String(" <small>(waiting for the first status to anchor the schedule)</small>")
                         : String("")) +
                    "</td></tr>";
        }
        if (j.lastStatusUnix != 0 && v.now.unixS != 0)
        {
            html += "<tr><td>Last status</td><td>" + durationText(v.now.unixS - j.lastStatusUnix) + " ago</td></tr>";
        }
        html += "</table><p>";
        if (OtaJob::canPause(j))
        {
            html += otaButton(id, "pause", "Pause");
        }
        if (OtaJob::canResume(j, v.now))
        {
            html += otaButton(id, "resume", "Resume");
        }
        if (OtaJob::canAbort(j))
        {
            // Out of its session no abort reaches the sensor (spec 14.5)
            html += otaButton(id, "abort", "Abort", "x",
                              "Abort the update? A sensor still in its OTA session discards the transfer; otherwise "
                              "its transfer state may stay and a job with the same image would resume it.");
        }
        if (OtaJob::canClear(j))
        {
            html += otaButton(id, "clear", "Clear job", "x",
                              "Remove the job from the gateway? In Verified this stops the installation.");
        }
        html += "</p>";
        if (OtaJob::canRetry(j))
        {
            // Retry = a new start of the stored image: with the same confirmation
            // as a start (downgrade, reinstall, the image that reverted here)
            if (v.blockers.length() == 0) // blockers are listed below
            {
                html += "<form method='post' action='/sensors/gen2/ota'><input type='hidden' name='devId' value='" +
                        String(id) + "'><input type='hidden' name='action' value='start'>";
                if (v.confirm.length() > 0)
                {
                    html += "<p class=e>" + HTMLGenerator::htmlEscape(v.confirm) +
                            "</p><label><input type='checkbox' name='confirm' value='" + String(v.confirmToken) +
                            "'> I confirm</label> ";
                }
                html += "<button class=b>Retry" +
                        (v.imageOk ? String(" with v") + fwText(v.imageFw) : String("")) + "</button></form>";
            }
        }
        if (j.state == OtaJob::State::Transfer && v.now.unixS == 0)
        {
            html += "<p><small>The gateway has no valid clock: other sensors' report slots are not known, blocks "
                    "may collide with their uplinks.</small></p>";
        }
    }
    // Start
    if (!mine || OtaJob::terminal(j.state))
    {
        if (v.imageOk)
        {
            html += "<p>Image for this sensor: <strong>v" + fwText(v.imageFw) + "</strong> (" + String(v.imageLen) +
                    " B), estimated " + durationText(v.estimateS) + " incl. the swap and confirmation.</p>";
        }
        if (v.blockers.length() > 0)
        {
            html += "<p class=e>" + HTMLGenerator::htmlEscape(v.blockers) + "</p>";
        }
        if (v.warnings.length() > 0)
        {
            html += "<p class=w>Note: " + HTMLGenerator::htmlEscape(v.warnings) + "</p>";
        }
        if (v.imageOk && v.blockers.length() == 0 && !(mine && OtaJob::canRetry(j)))
        {
            html += "<form method='post' action='/sensors/gen2/ota'><input type='hidden' name='devId' value='" +
                    String(id) + "'><input type='hidden' name='action' value='start'>";
            if (v.confirm.length() > 0)
            {
                html += "<p class=e>" + HTMLGenerator::htmlEscape(v.confirm) +
                        "</p><label><input type='checkbox' name='confirm' value='" + String(v.confirmToken) +
                        "'> I confirm</label> ";
            }
            html += "<button class=b>Update to v" + fwText(v.imageFw) + "</button></form>";
        }
    }
    // Image store
    html += "<h3>Images</h3><table><tr><th>HW type</th><th>Version</th><th>Size</th><th>Signature</th><th></th></tr>";
    for (size_t i = 0; i < v.imageCount; i++)
    {
        const OtaStore::Meta &m = v.images[i];
        char hw[8];
        snprintf(hw, sizeof(hw), "%04X", m.hwType);
        char sig[48];
        OtaImageSigned::stateText(m.signedKey, m.unknownKey, sig, sizeof(sig));
        html += "<tr><td>0x" + String(hw) + "</td><td>v" + fwText(m.fw) + "</td><td>" + String(m.len) + " B</td><td" +
                (m.isSigned() ? String(">") : String(" class=w>")) + sig + "</td><td><form method='post' action='/ota/delete' onsubmit='return confirm(\"Delete this image?\")'>"
                "<input type='hidden' name='hw' value='" +
                String(m.hwType) + "'><input type='hidden' name='devId' value='" + String(id) +
                "'><button class=x>Delete</button></form></td></tr>";
    }
    if (v.imageCount == 0)
    {
        html += "<tr><td colspan='5'>none</td></tr>";
    }
    html += "</table>";
    html += "<form method='post' action='/ota/upload?devId=" + String(id) +
            "' enctype='multipart/form-data'><p>Upload a sensor image (<code>firmware.bin</code> with the OTA "
            "trailer, optionally signed): <input type='file' name='image' accept='.bin' required> "
            "<button class=b>Upload</button></p></form>";
    // Or downloaded by the gateway (POST /ota/fetch, update mode); the
    // status is polled, then the page is loaded again with the result
    html += "<p>or from an https:// URL: <input id=fu placeholder='https://example.com/firmware.bin'> <button "
            "class=b onclick='fe()'>Download</button> <span id=fs class=m></span></p>";
    if (v.fetchResult.length() > 0)
    {
        html += "<p><small>Last download: " + HTMLGenerator::htmlEscape(v.fetchResult) + "</small></p>";
    }
    html += "<script>function fe(){var u=fu.value.trim();u&&confirm('Update mode: MQTT and sensor reception pause "
            "meanwhile. Continue?')&&fetch('/ota/fetch',{method:'POST',body:new URLSearchParams({url:u})}).then(r=>"
            "r.text().then(t=>{if(!r.ok)throw Error(t);fp()})).catch(e=>fs.textContent=e.message)}function fp(){"
            "fetch('/ota/fetch/status').then(r=>r.json()).then(d=>{/^[qd]/.test(d.state)?(fs.textContent=d.bytes+"
            "' / '+d.total+' B',setTimeout(fp,1500)):location.href='/sensors/gen2?devId=" +
            String(id) + "'}).catch(()=>setTimeout(fp,3000))}</script>";
    html += String("<p><small>Signed images only: <strong>") + (v.signedOnly ? "on" : "off") +
            "</strong> (Firmware page). " +
            (v.signedOnly ? "Unsigned images are refused and cannot be started or resumed."
                          : "Unsigned images are stored with a warning; a start needs a confirmation.") +
            "</small></p></section>";
}


// Readings of a generation 2 sensor in one line (corrected); raw: also the
// uncorrected values of the last uplink where a correction applies
static String gen2Readings(const Gen2Sensor &g, bool raw)
{
    static const Qty::Id shown[] = {Qty::Temperature, Qty::Humidity, Qty::Pressure, Qty::Co2, Qty::Lux, Qty::Battery};
    String t;
    for (Qty::Id q : shown)
    {
        if (g.qNow & Qty::mask(q))
        {
            t += (t.length() ? " &middot; " : "") + qtyText(q, g.value[q]);
        }
    }
    addFailed(t, g.qBad, Qty::Rssi);
    if (!t.length())
    {
        return "<span class=m>no data yet</span>";
    }
    String r;
    ep::TlvReader reader(g.lastUp, g.lastUpLen);
    ep::Tlv tlv;
    while (raw && reader.next(tlv) == 1)
    {
        Qty::Id q;
        float v;
        if (tlv.channel == 0 && Gen2Values::decode(tlv, q, v) == Gen2Values::Decoded::Value && Qty::info(q).cal != SensorCalibration::FIELDS &&
            (!g.cal.isNone(Qty::info(q).cal) || (q == Qty::Pressure && !g.cal.isNone(SensorCalibration::Altitude))))
        {
            r += (r.length() ? ", " : "") + qtyText(q, v);
        }
    }
    return r.length() ? t + " <small>(raw " + r + ")</small>" : t;
}

// Correction fields of a generation 2 sensor: those of the quantities it
// has reported (bits: SensorCalibration::Field)
// Correction fields of the quantities q (Qty::mask bits) - the type's of a
// generation 1 sensor, the reported ones of a generation 2 sensor (bits:
// SensorCalibration::Field)
static uint16_t correctionsOf(uint16_t q)
{
    uint16_t show = 0;
    for (int i = 0; i < Qty::COUNT; i++)
    {
        const SensorCalibration::Field f = Qty::info((Qty::Id)i).cal;
        if ((q & Qty::mask((Qty::Id)i)) && f != SensorCalibration::FIELDS)
        {
            show |= 1u << f;
            if (i == Qty::Pressure)
            {
                show |= 1u << SensorCalibration::Altitude;
            }
        }
    }
    return show;
}

// Signal in a word (SF9: sensitivity about -126 dBm)
static const char *signalWord(int rssi)
{
    return rssi >= -95 ? "good" : rssi >= -110 ? "fair" : "weak";
}

// Name, (Simple) report interval and the delete button of a generation 2
// sensor; Advanced sets the interval in the configuration card
static void addGen2EditForm(String &html, const Gen2Sensor &g, const String &id, bool simple)
{
    html += "<form method=post action='/sensors/gen2/edit'><input type=hidden name=devId value=" + id +
            "><label for=name>Name</label><input id=name name=name maxlength=32 required value='" +
            HTMLGenerator::htmlEscape(g.name) + "'>";
    if (simple)
    {
        html += "<label for=interval>Report every (s)</label><input type=number id=interval name=interval min=" +
                String(Gen2Config::MIN_INTERVAL_S) + " max=" + String(Gen2Config::MAX_INTERVAL_S) + " value=" +
                String(g.config.intervalS) + " required>";
    }
    const uint16_t show = correctionsOf(g.qMask);
    if (show)
    {
        HTMLGenerator::addCorrections(html, show, g.cal);
    }
    else
    {
        html += "<p class=m>Corrections appear after the first report.</p>";
    }
    html += "<button>Save</button></form><form method=post action='/sensors/delete2' class=i onsubmit='return "
            "confirm(\"Delete this sensor?\")'><input type=hidden name=devId value=" +
            id + "><button class=x>Delete sensor</button></form>";
}

String HTMLGenerator::generateGen2DetailPage(const Gen2Sensor &g, const String &message, const Gen2OtaView *ota)
{
    String html;
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)g.devId);
    const unsigned long now = millis();
    if (!adv())
    {
        // Simple: readings, name, interval, delete - no radio or security details
        html.reserve(2560);
        addHtmlHeader(html, "Gen 2 sensor", Page::Sensors);
        html += "<section><h2>" + htmlEscape(g.name) + " <small>Gen 2 &middot; DevID " + String(id) + "</small></h2>";
        if (message.length() > 0)
        {
            html += "<p class=w>" + htmlEscape(message) + "</p>";
        }
        html += "<table class=kv>";
        kvRow(html, "Readings", gen2Readings(g, false));
        kvRow(html, "Last seen",
              g.seen ? formatRelativeMillis(now, g.lastSeenMs, true) + ", signal " + signalWord(g.rssi) + " (" +
                           String(g.rssi) + " dBm)"
                     : String("never"));
        if (g.hasFw)
        {
            kvRow(html, "Firmware",
                  "HW " + String(g.hwType) + ", " + String(g.fwMajor) + "." + String(g.fwMinor) + "." + String(g.fwPatch));
        }
        html += "</table>";
        addGen2EditForm(html, g, id, true);
        html += "</section>";
        addHtmlFooter(html);
        return html;
    }
    html.reserve(GEN2_ADV_PAGE_SIZE);
    const int8_t txNow = g.effectiveTxPower(); // configured, or what the status report says the sensor uses
    const Gen2Link::Margins link = Gen2Link::margins(g.link, gen2LinkNowS(), txNow);
    const String recommendation = txPowerAdvice(link, txNow);
    addHtmlHeader(html, "Gen 2 sensor", Page::Sensors);
    html += "<section><h2>" + htmlEscape(g.name) + " <small><code>" + String(id) + "</code></small></h2>";
    if (message.length() > 0)
    {
        html += "<p class=w>" + htmlEscape(message) + "</p>";
    }
    html += "<table class=kv>";
    html += "<tr><td>Readings</td><td>" + gen2Readings(g, true) + "</td></tr>";
    html += "<tr><td>Last heard</td><td>" + formatRelativeMillis(now, g.lastSeenMs, g.seen);
    if (ota && ota->hasJob && ota->job.devId == g.devId && ota->job.state == OtaJob::State::Transfer)
    {
        // During a transfer the reports ride in the OTA statuses (spec 14.5)
        html += " <small>(OTA transfer: reports every " + String(ota->job.roundS() / 60) + " min)</small>";
    }
    html += "</td></tr>";
    if (g.hasFw)
    {
        html += "<tr><td>Hardware / firmware</td><td>HW " + String(g.hwType) + ", FW " + String(g.fwMajor) + "." +
                String(g.fwMinor) + "." + String(g.fwPatch) + "</td></tr>";
    }
    html += "<tr><td>Boot state</td><td>" + bootStateText(g.bootState) +
            (g.blVersion ? " <small>(bootloader " + String(g.blVersion >> 4) + "." + String(g.blVersion & 0x0F) + ")</small>"
                         : String("")) +
            "</td></tr>";
    if (g.seen)
    {
        html += "<tr><td>Signal at the gateway</td><td>" + String(g.rssi) + " dBm, SNR " + String(g.snr, 1) +
                " dB, " + String(g.freqError / 1000.0f, 1) + " kHz</td></tr>";
    }
    html += "<tr><td>Link margin</td><td>uplink ";
    html += link.upValid() ? String(link.upMargin()) + " dB <small>(" + linkSummary(link) + "; " +
                                 String(link.upSamples) + " UPs)</small>"
                           : String("&ndash; <small>(" + String(link.upSamples) + " of " +
                                    String(Gen2Link::MIN_UP_SAMPLES) + " UPs needed)</small>");
    html += ", downlink ";
    html += link.downValid()
                ? String(link.downMargin()) + " dB <small>(worst " + String(link.worstDown) + " dBm" +
                      (link.downSnrSamples > 0 ? ", SNR &ge; " + String(link.worstDownSnr > 0 ? "+" : "") +
                                                   String(link.worstDownSnr) + " dB"
                                             : String("")) +
                      ")</small>"
                : String("&ndash; <small>(" + String(link.downSamples) + " of " +
                         String(Gen2Link::MIN_DOWN_SAMPLES) + " reports needed)</small>");
    html += link.lost > 0 ? "<br><span class=w>" + String(link.lost) +
                                (link.lost == 1 ? " UP" : " UPs") + " lost in the window</span>"
                          : String("<br><small>no UPs lost in the window</small>");
    html += "<br><small>over the SF9 limits (" + String(Gen2Link::SENSITIVITY_DBM) + " dBm, SNR " +
            String(Gen2Link::SNR_LIMIT_DB) + " dB), last 7 days / 20 UPs</small>";
    if (!recommendation.isEmpty())
    {
        html += "<br><small>" + recommendation + "</small>";
    }
    html += "</td></tr>";
    html += "<tr><td>TX power (configured)</td><td>" + String(g.config.txPowerDbm) + " dBm";
    if (txNow != g.config.txPowerDbm)
    {
        html += " <small>(the sensor reports " + String(txNow) + " dBm)</small>";
    }
    if (txNow > Gen2Config::EU_TX_POWER_DBM)
    {
        html += "<br><span class=e>" + txPowerWarning() + "</span>";
    }
    html += "</td></tr>";
    html += "<tr><td>Session</td><td>" +
            String(g.sessionUp ? "up, last FCnt " + String(g.lastFcnt) : String("none (the sensor joins or is answered with REJOIN)")) +
            "</td></tr>";
    html += "</table>";
    if (g.lastUpLen > 0)
    {
        html += "<h3>Last uplink</h3><table>";
        ep::TlvReader r(g.lastUp, g.lastUpLen);
        ep::Tlv t;
        while (r.next(t) == 1)
        {
            html += describeTlv(t);
        }
        html += "</table>";
    }
    html += "</section>";

    // Name and corrections (and delete)
    html += "<section><h2>Name and corrections</h2>";
    addGen2EditForm(html, g, id, false);
    html += "</section>";

    // Configuration
    const bool inSync = Gen2Config::inSync(g.configVersion, g.cfgVersion);
    const Gen2Config::Values &c = g.config;
    html += "<section><h2>Configuration</h2><p>";
    if (!g.sessionUp)
    {
        html += "Configuration v" + String(g.cfgVersion) + " is sent with the next join.";
    }
    else if (inSync)
    {
        html += "<span class=ok>Set</span> &ndash; the sensor runs v" + String(g.cfgVersion) + ".";
    }
    else
    {
        html += "<span class=w>Waiting for the sensor</span> &ndash; v" + String(g.cfgVersion) +
                " goes out with its next uplink (it reports v" + String(g.configVersion) + ").";
    }
    html += "</p><form method='post' action='/sensors/gen2/config'>";
    html += "<input type='hidden' name='devId' value='" + String(id) + "'>";
    html += "<label for='interval'>Report interval (s, " + String(Gen2Config::MIN_INTERVAL_S) + "&ndash;" +
            String(Gen2Config::MAX_INTERVAL_S) + "):</label>";
    html += "<input type='number' id='interval' name='interval' min='" + String(Gen2Config::MIN_INTERVAL_S) +
            "' max='" + String(Gen2Config::MAX_INTERVAL_S) + "' value='" + String(c.intervalS) + "' required>";
    html += "<small>Below " + String(Gen2Config::DUTY_SAFE_INTERVAL_S) +
            " s the sensor stretches the interval to its 0.1 % duty-cycle limit.</small>";
    html += "<label for='slot'>Slot offset within the interval (s):</label>";
    html += "<input type='number' id='slot' name='slot' min='0' max='" + String(Gen2Config::MAX_INTERVAL_S - 1) +
            "' value='" + String(c.slotS) + "'>";
    html += "<label for='txPower'>Sensor TX power (dBm, " + String(Gen2Config::MIN_TX_POWER_DBM) + " to " +
            String(Gen2Config::MAX_TX_POWER_DBM) + ", default " + String(Gen2Config::DEFAULT_TX_POWER_DBM) +
            "):</label>";
    html += "<input type='number' id='txPower' name='txPower' min='" + String(Gen2Config::MIN_TX_POWER_DBM) +
            "' max='" + String(Gen2Config::MAX_TX_POWER_DBM) + "' value='" + String(c.txPowerDbm) +
            "' oninput='txWarn.hidden=this.value<=" + String(Gen2Config::EU_TX_POWER_DBM) + "'>";
    html += "<p id=txWarn class=e" + String(c.txPowerDbm > Gen2Config::EU_TX_POWER_DBM ? "" : " hidden") + ">" +
            txPowerWarning() + "</p>";
    if (!recommendation.isEmpty())
    {
        html += "<small>" + recommendation + "</small>";
    }
    html += "<small>After a power change the sensor asks for an ACK on the next 2 uplinks and goes back to the "
            "previous power if none arrives.</small>";
    html += "<label for='ackEvery'>Request an ACK every N-th uplink (0 = never):</label>";
    html += "<input type='number' id='ackEvery' name='ackEvery' min='0' max='255' value='" + String(c.ackEvery) + "'>";
    html += "<label for='ackRetries'>Retries of confirmed reports (0&ndash;15):</label>";
    html += "<input type='number' id='ackRetries' name='ackRetries' min='0' max='15' value='" +
            String(c.ackRetries) + "'>";
    html += "<label for='ackBackoff'>Retry backoff base (s, 1&ndash;15):</label>";
    html += "<input type='number' id='ackBackoff' name='ackBackoff' min='1' max='15' value='" +
            String(c.ackBackoffS) + "'>";
    html += "<button>Save configuration</button></form></section>";

    // Status report
    html += "<section><h2>Status report</h2>";
    if (g.statusLen > 0)
    {
        html += "<p>Received " + formatRelativeMillis(now, g.statusMs, true) + "</p>";
        html += describeStatus(g.status, g.statusLen);
    }
    else
    {
        html += "<p>No status report received yet.</p>";
    }
    html += "<form method='post' action='/sensors/gen2/status'><input type='hidden' name='devId' value='" +
            String(id) + "'>";
    html += g.statusRequested ? "<p>Requested &ndash; the sensor answers after its next uplink.</p>"
                              : "<button>Request status</button>";
    html += "</form></section>";

    // Downlink
    html += "<section><h2>Downlink</h2><table>";
    html += "<tr><td>Sent in RX1 / RX2</td><td>" + String(g.downRx1) + " / " + String(g.downRx2) + "</td></tr>";
    html += "<tr><td>Not sent</td><td>" + String(g.downNotSent);
    if (g.lastDownOutcome != 0xFF)
    {
        html += " <small>(last response: " + String(RadioTask::txOutcomeName((TxOutcome)g.lastDownOutcome)) +
                ")</small>";
    }
    html += "</td></tr>";
    html += "<tr><td>No timed response possible</td><td>" + String(g.noSlot) + "</td></tr>";
    html += "<tr><td>JOIN_ACC / REJOIN</td><td>" + String(g.joinAccepts) + " / " + String(g.rejoins) +
            " <small>(" + String(g.joinLimited) + " held back by the 10 s limit)</small></td></tr>";
    html += "<tr><td>DOWN with configuration</td><td>" + String(g.configDowns) + "</td></tr>";
    if (g.downlinkRssi != INT8_MIN)
    {
        html += "<tr><td>Downlink at the sensor</td><td>" + String(g.downlinkRssi) + " dBm, SNR " +
                String(g.downlinkSnr / 4.0f, 2) + " dB</td></tr>";
    }
    html += "<tr><td>Uplinks verified</td><td>" + String(g.upFrames) + " <small>(MIC failed " +
            String(g.micFailures) + ", without session " + String(g.upWithoutSession) + ")</small></td></tr>";
    html += "</table></section>";

    // Join / devNonce
    html += "<section><h2>Join</h2><table>";
    html += "<tr><td>Last accepted devNonce</td><td>" + (g.hasDevNonce ? String(g.lastDevNonce) : String("none")) +
            "</td></tr>";
    html += "<tr><td>JOIN_REQ verified</td><td>" + String(g.joinRequests) + "</td></tr>";
    html += "<tr><td>JOIN_REQ rejected: devNonce not above the last accepted one</td><td>" +
            String(g.replayedJoins) + "</td></tr></table>";
    if (g.staleJoinStreak >= 3)
    {
        html += "<p class=e>The sensor keeps joining with an old devNonce: it has probably lost "
                "its configuration (reflashed with a full erase, damaged store). Reset the devNonce right before "
                "restarting the sensor.</p>";
    }
    html += "<form method='post' action='/sensors/gen2/resetnonce' onsubmit='return confirm(\"Accept the next join "
            "of this sensor with any devNonce?\")'><input type='hidden' name='devId' value='" +
            String(id) + "'><button class=x>Reset devNonce</button></form></section>";
    if (ota)
    {
        addGen2OtaCard(html, g, *ota, id);
    }
    html += "<a class='b b2' href='/sensors'>Back to sensors</a>";
    addHtmlFooter(html);
    return html;
}

// Page without navigation: <main> with one section (notice / progress)
static void bareHead(String &html, const char *title, uint8_t refreshS, const char *url)
{
    html += "<!DOCTYPE html><html lang=en><head><meta charset=UTF-8><meta name=viewport "
            "content='width=device-width,initial-scale=1'><meta http-equiv=refresh content='" +
            String(refreshS) + (url ? String(";url=") + url : String("")) + "'><title>";
    html += title;
    html += "</title><link rel=stylesheet href='/a.css?v=";
    html += String(WebAssets::TOKEN, HEX);
    html += "'></head><body><main><section><h2>";
    html += title;
    html += "</h2><p>";
}

String HTMLGenerator::generateNoticePage(const char *title, const String &text, uint8_t refreshS, const char *url)
{
    String html;
    html.reserve(640 + text.length());
    bareHead(html, title, refreshS, url);
    html += text + "</p></section></main></body></html>";
    return html;
}

String HTMLGenerator::generateUpdateProgressPage(const String &updateStatus)
{
    String html;
    html.reserve(768);
    bareHead(html, "Update in progress", 15, nullptr);
    html += htmlEscape(updateStatus);
    html += "</p><p class=m>This page reloads every 15 s and shows the firmware page again when the update ends. If "
            "it stops answering, reload it in a few minutes.</p></section></main></body></html>";
    return html;
}

static String versionText(uint32_t v)
{
    char t[16];
    OtaImage::versionText(v, t, sizeof(t));
    return String(t);
}

String HTMLGenerator::generateFirmwarePage(const String &updateStatus, bool signedOnly, const GatewayOta::View &gw,
                                           const UpdateCheckView &c)
{
    String html;
    html.reserve(7680);
    addHtmlHeader(html, "Firmware", Page::Firmware);
    html += "<p id=msg class=w hidden></p>";
    // Update mode: the last session (while one runs, /firmware serves
    // generateUpdateProgressPage instead)
    if (updateStatus.length() > 0)
    {
        html += "<section><h2>Update mode</h2><p>" + htmlEscape(updateStatus) + "</p></section>";
    }
    html += "<section><h2>Firmware</h2><p>Version <b>v" FIRMWARE_VERSION "</b> <small>(installed: " +
            htmlEscape(gw.runningSig) + ")</small></p>";
    if (gw.validating && gw.validateFailed)
    {
        html += "<p class=e>Validation failed (it could not be marked valid) - the next restart rolls back to the "
                "previous firmware.</p>";
    }
    else if (gw.validating && gw.validateLeftS == 0)
    {
        html += "<p class=m>New firmware: being marked valid (retrying).</p>";
    }
    else if (gw.validating)
    {
        html += "<p class=m>New firmware: validated after about " + String(gw.validateLeftS) +
                " s more of normal operation (until then a reset rolls back, and nothing else can be installed).</p>";
    }
    if (gw.lastText.length() > 0)
    {
        html += "<p class=m>Last install: " + htmlEscape(gw.lastText) + "</p>";
    }
    // Updates: the gateway checks the signed manifest itself (Update/UpdateCheck.h)
    html += "<h3>Updates</h3><p>";
    if (gw.checkText.length() > 0)
    {
        html += "Last check";
        if (gw.checkAt)
        {
            const time_t t = gw.checkAt;
            struct tm tm;
            char when[24];
            localtime_r(&t, &tm);
            strftime(when, sizeof(when), " %Y-%m-%d %H:%M", &tm);
            html += when;
        }
        html += ": " + htmlEscape(gw.checkText);
    }
    else
    {
        html += "Not checked yet.";
    }
    html += "</p>";
    if (c.have)
    {
        const String v = versionText(c.gatewayVersion);
        switch (c.gatewayOffer)
        {
        case Manifest::Offer::Install:
            html += "<p><b>Gateway v" + v + " is available</b></p><button onclick=\"ins('" + v + "')\">Install v" + v +
                    "</button> ";
            break;
        case Manifest::Offer::None:
            html += "<p class=m>The manifest has no firmware for this board.</p>";
            break;
        case Manifest::Offer::Unknown:
            html += "<p class=m>v" + v + " is offered, but the running version cannot be compared.</p>";
            break;
        default:
            html += "<p>Up to date.</p>";
            break;
        }
    }
    html += "<button onclick='chk()'>Check now</button><p class=m>The check and a download run in update mode: MQTT "
            "and sensor reception pause; this page may lose the connection for a few minutes.</p>";
    if (adv())
    {
        if (c.have || gw.manifestSeq)
        {
            char key[12];
            snprintf(key, sizeof(key), "%08lx", (unsigned long)c.keyId);
            html += "<p class=m>Manifest seq " + String(gw.manifestSeq) +
                    (c.have ? String(", verified (key ") + key + ")" : String("")) + "</p>";
        }
        if (c.sensors > 0)
        {
            html += "<table><tr><th>Sensor HW<th>Stored<th>Offered<th></tr>";
            for (uint8_t i = 0; i < c.sensors; i++)
            {
                const UpdateCheckView::Sensor &s = c.sensor[i];
                char hw[8];
                snprintf(hw, sizeof(hw), "%04X", s.hw);
                const String v = versionText(s.offered);
                html += String("<tr><td>") + hw + "<td>" + (s.stored ? versionText(s.storedFw) : String("-")) + "<td>" +
                        v + "<td>" +
                        (s.offer == Manifest::Offer::Install
                             ? "<button class=b2 onclick=\"sf('" + String(hw) + "','" + v + "')\">Fetch</button>"
                             : String(Manifest::offerName(s.offer))) +
                        "</tr>";
            }
            html += "</table>";
        }
        html += "<label for=fu>Firmware URL</label><input id=fu placeholder='https://example.com/firmware.bin'>"
                "<button class=b2 onclick=\"var u=fu.value.trim();u?upd(u):alert('Enter a URL first')\">Update "
                "from URL</button>";
    }
    html += "</section>";
    // An unsigned (or older verified) image waiting for the admin's
    // confirmation
    if (gw.waiting)
    {
        html += "<section><h2>Firmware waiting for confirmation</h2><p>Written but not activated: " +
                htmlEscape(gw.waitingText) +
                (gw.waitingDowngrade ? String(". Confirm to downgrade.</p>")
                                     : String(". Not verified - install it only if you trust its source.</p>"));
        const String left = String((gw.waitingLeftS + 59) / 60) + " min";
        html += gw.waitingMine ? "<button onclick='cf(\"" + gw.waitingDigest + "\")'>" +
                                     (gw.waitingDowngrade ? "Downgrade" : "Activate unsigned firmware") +
                                     "</button> <small>(within " + left + ")</small>"
                               : "<p class=m>Only the login session that installed it can confirm it (within " +
                                     left + ").</p>";
        html += "</section>";
    }
    html += "<section><h2>Upload</h2><form id=up><input type=file name=bin accept='.bin,application/octet-stream' "
            "required><p class=m>Do not power off during the upload. A signed file is verified and installed; an "
            "unsigned or older one only after you confirm it.</p><button>Upload and install</button></form>"
            "</section>";
    // One setting for sensor images and the gateway's own firmware
    // (Advanced)
    if (adv())
    {
        html += "<section><h2>Signed images</h2><form method=post action='/firmware/signed'><label class=c><input "
                "type=checkbox name=signedOnly value=1" +
                String(signedOnly ? " checked" : "") +
                "> Signed images only</label><p class=m>Checked against the release keys in this firmware. On: unsigned "
                "gateway firmware and sensor images are refused (an unfinished sensor job with one is held). A wrong "
                "signature is always refused.</p><button>Save</button></form></section>";
    }
    html += "<script>var W='Update mode: MQTT and sensor reception pause, interfaces other "
            "than the uplink may switch off (then reload this page in a few minutes). Continue?';"
            "function J(t){try{return JSON.parse(t)}catch(e){return null}}"
            "function N(m){msg.textContent=m;msg.hidden=false}"
            "function R(r){return r.text().then(function(t){return{ok:r.ok,s:r.status,d:J(t)||{},t:t}})}"
            "function E(x){return x.d.error||x.d.message||x.t||'HTTP '+x.s}"
            "function P(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':"
            "'application/x-www-form-urlencoded'},body:b}).then(R)}"
            "function L(){N('The connection was lost - the gateway may be in update mode. Reload the page in a few "
            "minutes.')}"
            // Poll u until f(answer) is true, then reload (lost answers are
            // retried: the session may switch interfaces off); at most 90
            // polls (3 min) of any kind, a 401 ends it at once
            "function Q(u,f,n){n=n||0;setTimeout(function(){fetch(u).then(R).then(function(x){if(x.s==401)N('Logg"
            "ed out - reload the page.');else if(x.ok&&f(x.d))location.reload();else n<90?Q(u,f,n+1):N('No result"
            " yet - reload the page.')}).catch(function(){n<90?Q(u,f,n+1):L()})},2000)}"
            "function chk(){if(!confirm(W))return;P('/firmware/check','').then(function(x){if(!x.ok)throw Error(E(x"
            "));N('Checking...');Q('/firmware/check',function(d){return d.state=='idle'})}).catch(function(e){e "
            "instanceof TypeError?L():alert('Update check failed: '+e.message)})}"
            "function ins(v){if(!confirm(W))return;N('Installing v'+v+' - wait a few minutes, do not power off.');"
            "setTimeout(function(){location.reload()},120000);P('/firmware/install','version='+v).then(function(x){"
            "if(!x.ok)throw Error(E(x));if(x.d.message)N(x.d.message)}).catch(function(e){e instanceof TypeError?L("
            "):N('Not started: '+e.message)})}"
            "function sf(h,v){if(!confirm(W))return;P('/ota/fetch','hw='+h+'&version='+v).then(function(x){if(!x.ok"
            ")throw Error(E(x));N('Downloading the sensor image...');Q('/ota/fetch/status',function(d){return d."
            "state=='stored'||d.state=='failed'})}).catch(function(e){e instanceof TypeError?L():alert('Not started: "
            "'+e.message)})}"
            "function upd(u){if(!confirm(W))return;N('Firmware update started - wait about 2 minutes, do not "
            "power off.');setTimeout(function(){location.reload()},120000);P('/firmware/update','url='+"
            "encodeURIComponent(u)).then(function(x){if(!x.ok)throw Error(E(x));if(x.d.message)N(x.d.message)})"
            ".catch(function(e){N('Update request sent; if nothing happens, try again ('+e.message+')')})}"
            "function cf(i){P('/firmware/confirm','image='+encodeURIComponent(i)).then(function(x){var m="
            "x.d.message||x.d.error||'No answer';if(!x.ok){alert('Not activated: '+m);location.reload();return}"
            "N(m);setTimeout(function(){location.reload()},60000)}).catch(function(e){alert('Confirmation failed: '"
            "+e.message)})}"
            "up.onsubmit=function(ev){ev.preventDefault();var b=up.querySelector('button');b.disabled=true;"
            "N('Uploading - wait about 2 minutes, do not power off.');fetch('/firmware',{method:'POST',body:"
            "new FormData(up)}).then(R).then(function(x){if(!x.ok)throw Error(E(x));var d=x.d;if(d.status=='confirm')"
            "{msg.hidden=true;if(confirm(d.message+(d.sig=='verified'?'\\n\\nDowngrade to this older firmware now?':"
            "'\\n\\nActivate this unsigned firmware now?')))cf(d.image);else location.reload();return}"
            "N(d.message||'Uploaded - the gateway restarts.');setTimeout(function(){location.reload()},60000)})"
            ".catch(function(e){N('Upload interrupted - if the session expired, reload the page and log in again ('"
            "+e.message+')')}).finally(function(){b.disabled=false})}</script>";
    addHtmlFooter(html);
    return html;
}

// Correction fields (SensorCalibration::Field order; form names and
// limits from SensorCalibration::info)
static const char *const CORR_LABELS[SensorCalibration::FIELDS] = {
    "Temperature (&plusmn;&deg;C)", "Humidity (&plusmn;%)",     "Pressure (&plusmn;hPa)", "CO2 (&plusmn;ppm)",
    "Light (&plusmn;lx)",           "Wind speed (&times;)",     "Wind direction (&plusmn;&deg;)",
    "Rain (&times;)",               "Rain rate (&times;)",      "Altitude (m) <small>for the pressure</small>"};

void HTMLGenerator::addCorrections(String &html, uint16_t show, const SensorCalibration::Set &cal)
{
    html += "<h3>Corrections</h3><p class=m>Applied to every reading before it is shown, sent to MQTT and the "
            "API.</p><div class=g>";
    for (int i = 0; i < SensorCalibration::FIELDS; i++)
    {
        if (!(show & (1u << i)))
        {
            continue;
        }
        const SensorCalibration::FieldInfo &f = SensorCalibration::info((SensorCalibration::Field)i);
        html += "<div><label for=";
        html += f.form;
        html += ">";
        html += CORR_LABELS[i];
        html += "</label><input type=number id=";
        html += f.form;
        html += " name=";
        html += f.form;
        html += f.whole ? " step=1" : " step=any";
        // A stored value outside the limits (an older firmware) would make
        // the browser refuse the form: no limits on that field then
        if (cal.v[i] >= f.min && cal.v[i] <= f.max)
        {
            html += " min=" + String((long)f.min) + " max=" + String((long)f.max);
        }
        char value[24];
        SensorCalibration::formText(value, sizeof(value), (SensorCalibration::Field)i, cal.v[i]);
        html += " value='" + String(value) + "'></div>";
    }
    html += "</div>";
}



// Add (s == nullptr) or edit a generation 1 sensor. Simple edit: name,
// custom URL, corrections (type, serial number and key are not posted -
// the handler keeps them); add and Advanced: also type, serial and key
String HTMLGenerator::generateSensorForm(const SensorData *s, int index)
{
    const bool a = adv();
    String html;
    html.reserve(SENSOR_FORM_SIZE);
    addHtmlHeader(html, s ? "Edit sensor" : "Add sensor", Page::Sensors);
    html += "<section><h2>";
    if (s)
    {
        html += htmlEscape(s->name) + " <small>" + sensorTypeToString(s->deviceType) + " &middot; SN " +
                formatSN(s->serialNumber) + "</small></h2><form method=post action='/sensors/update'><input "
                "type=hidden name=index value=" +
                String(index) + "><input type=hidden name=sn value=" + formatSN(s->serialNumber) + ">";
    }
    else
    {
        html += "Add sensor</h2><form method=post action='/sensors/add'>";
    }
    html += "<label for=name>Name</label><input id=name name=name maxlength=32 required value='" +
            (s ? htmlEscape(s->name) : String("")) + "'>";
    if (!s || a)
    {
        html += "<label for=deviceType>Type</label><select id=deviceType name=deviceType>";
        for (const SensorTypeInfo &t : SENSOR_TYPE_DEFINITIONS)
        {
            if (t.type == SensorType::UNKNOWN)
            {
                continue;
            }
            html += "<option value=" + String((uint8_t)t.type) + (s && s->deviceType == t.type ? " selected>" : ">") +
                    t.name;
            html += (t.q & Qty::mask(Qty::WindSpeed)) ? " &ndash; weather station"
                    : (t.q & Qty::mask(Qty::Co2))     ? " &ndash; temperature, humidity, CO2"
                                                      : " &ndash; temperature, humidity, pressure";
        }
        html += "</select><div class=g><div><label for=serialNumber>Serial number</label><input id=serialNumber "
                "name=serialNumber maxlength=6 required placeholder='1234AB' value='" +
                (s ? formatSN(s->serialNumber) : String("")) +
                "'></div><div><label for=deviceKey>Device key</label><input id=deviceKey name=deviceKey "
                "maxlength=8 autocomplete=off " +
                (s ? String("placeholder='(unchanged)'") : String("required placeholder='DEADBEEF'")) +
                "></div></div>";
    }
    html += "<label for=customUrl>Forward to URL <small>(optional)</small></label><input id=customUrl name=customUrl "
            "maxlength=200 placeholder='https://example.com/api?temp=*TEMP*&amp;hum=*HUM*' value='" +
            (s ? htmlEscape(s->customUrl) : String("")) +
            "'><p class=m>Placeholders: *TEMP* *HUM* *PRESS* *PPM* *LUX* *WIND_SPEED* *WIND_DIR* *RAIN* *DAILY_RAIN* "
            "*RAIN_RATE* *BAT* *RSSI* *SN* *TYPE*</p>";
    if (s)
    {
        addCorrections(html, correctionsOf(s->qMask()), s->cal);
    }
    html += String("<button>") + (s ? "Save" : "Add") + "</button><a class='b b2' href='/sensors'>Cancel</a></form>";
    if (s)
    {
        html += "<a class='b x' href='/sensors/delete?index=" + String(index) + "&amp;sn=" + formatSN(s->serialNumber) +
                "' onclick='return confirm(\"Delete this sensor?\")'>Delete sensor</a>";
    }
    html += "</section>";
    addHtmlFooter(html);
    return html;
}

// Generating logs page
String HTMLGenerator::generateLogsPageHead(LogLevel currentLevel)
{
    String html;
    html.reserve(2048);
    addHtmlHeader(html, "Logs", Page::Logs, false, true, 30);
    html += "<section><h2>Logs</h2><form method=post action='/logs/level'><label for=level>Level</label><select "
            "id=level name=level>";
    static const char *const levels[] = {"ERROR", "WARNING", "INFO", "DEBUG", "VERBOSE"};
    static const LogLevel values[] = {LogLevel::ERROR, LogLevel::WARNING, LogLevel::INFO, LogLevel::DEBUG,
                                      LogLevel::VERBOSE};
    for (size_t i = 0; i < 5; i++)
    {
        html += String("<option") + (currentLevel == values[i] ? " selected>" : ">") + levels[i];
    }
    html += "</select><button>Set level</button><a class='b b2' href='/logs'>Refresh</a><a class='b x' "
            "href='/logs/clear' onclick='return confirm(\"Clear all logs?\")'>Clear</a></form></section><section>";
    return html;
}

// One log line of the logs page
String HTMLGenerator::generateLogEntry(const LogEntry &entry)
{
    const char *logClass = "log-info";
    switch (entry.level)
    {
    case LogLevel::ERROR:
        logClass = "log-error";
        break;
    case LogLevel::WARNING:
        logClass = "log-warning";
        break;
    case LogLevel::DEBUG:
        logClass = "log-debug";
        break;
    case LogLevel::VERBOSE:
        logClass = "log-verbose";
        break;
    default:
        break;
    }
    return "<div class='log-entry " + String(logClass) + "'>" + htmlEscape(entry.getFormattedLog()) + "</div>";
}

String HTMLGenerator::generateLogsPageTail()
{
    String html = "</section>";
    addHtmlFooter(html);
    return html;
}
// Generating JSON for API


// Generating API page
// /api: one sensor (ApiRow.h), its ID string kept alive by the caller
static ApiRow::Row apiRow(const SensorData &s, const String &id, unsigned long now)
{
    ApiRow::Row r;
    r.name = s.name.c_str();
    r.deviceType = s.gen2 ? 0 : (uint8_t)s.deviceType;
    r.typeName = s.typeName();
    r.id = id.c_str();
    r.gen2 = s.gen2;
    r.q = s.qMask();
    r.qValid = s.qValid();
    for (int q = 0; q < Qty::COUNT; q++)
    {
        r.v[q] = s.value((Qty::Id)q);
    }
    r.lastSeenS = s.lastSeen > 0 ? (long)((now - s.lastSeen) / 1000) : -1;
    return r;
}

String HTMLGenerator::apiJsonHead(bool online)
{
    struct tm timeinfo;
    char timeStr[24] = "Time not set";
    if (getLocalTime(&timeinfo, 0))
    {
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    }
    return String("{\"version\":\"" FIRMWARE_VERSION "\",\"time\":\"") + timeStr + "\",\"status\":\"" +
           (online ? "connected" : "disconnected") + "\",\"sensors\":[";
}

String HTMLGenerator::apiJsonRow(const SensorData &s, unsigned long now)
{
    const String id = s.idString();
    StaticJsonDocument<768> doc;
    ApiRow::toJson(doc.to<JsonObject>(), apiRow(s, id, now));
    String out;
    serializeJson(doc, out);
    return out;
}

String HTMLGenerator::apiCsvRow(const SensorData &s, unsigned long now)
{
    const String id = s.idString();
    String line;
    ApiRow::toCsv(line, apiRow(s, id, now));
    return line;
}

String HTMLGenerator::generateAPIPage(const std::vector<SensorData> &sensors)
{
    String html;
    html.reserve(2560);
    addHtmlHeader(html, "API", Page::Api);
    html += "<section><h2>API</h2><p>Sensor readings as JSON or CSV, no login needed.</p><table><tr><th>URL</th>"
            "<th></th></tr><tr><td><a href='/api?format=json'><code>/api?format=json</code></a></td><td>all "
            "sensors, JSON</td></tr><tr><td><a href='/api?format=csv'><code>/api?format=csv</code></a></td><td>all "
            "sensors, CSV</td></tr><tr><td><code>/api?format=json&amp;sensor=ID</code></td><td>one sensor: its "
            "serial number, or a gen 2 DevID (<code>&amp;gen=2</code> if equal to a serial number)</td></tr></table><h3>Example</h3><pre>";
    html += htmlEscape(apiJsonHead(true)) + "\n  " +
            (sensors.empty() ? String("{&quot;deviceType&quot;:1,&quot;typeName&quot;:&quot;CLIMA&quot;,...}")
                             : htmlEscape(apiJsonRow(sensors[0], millis()))) +
            "\n]}</pre><p class=m>A gen 2 sensor has &quot;typeName&quot;:&quot;GEN2&quot;, &quot;gen&quot;:2 and its "
            "DevID as serialNumber. A value is null until the sensor reports; lastSeen is in seconds (-1 never).</p>"
            "</section>";
    addHtmlFooter(html);
    return html;
}
