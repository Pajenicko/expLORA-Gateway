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
#include <Arduino.h>
#include <WiFi.h>
#include "../Hardware/PowerSource.h"
#include "../Network/Uplink.h"
#include "../config.h"
#include "../Hardware/RadioTask.h"
#include "../Data/TimeZones.h"

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
void HTMLGenerator::addHtmlHeader(String &html, const String &title, bool isAPMode, bool navigation)
{
    html += "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<title>" + title + " - " BOARD_NAME "</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

    // Add CSS styles
    addStyles(html);

    html += "</head><body>";
    html += "<header><h1>" BOARD_NAME "</h1><h2>" + title + "</h2></header>";

    // Navigation: full in STA, minimal in AP (Config + Reboot), none on the login page
    if (!navigation)
    {
        html += "<div class='container'>";
    }
    else if (!isAPMode)
    {
        addNavigation(html, title);
    }
    else
    {
        html += "<nav>";
        html += "<a href='/config' class='" + String(title == "Configuration" ? "active" : "") + "'>WiFi Setup</a>";
#if defined(HAS_ETHERNET) || defined(HAS_LTE)
        html += "<a href='/network' class='" + String(title == "Network" ? "active" : "") + "'>Network</a>";
#endif
        html += "<a href='/password' class='" + String(title == "Password" ? "active" : "") + "'>Password</a>";
        html += "<a href='/reboot' onclick=\"return confirm('Are you sure you want to reboot the device?');\">Reboot</a>";
        html += "<a href='/logout'>Logout</a>";
        html += "<a href='javascript:void(0);' class='icon' onclick='toggleMenu()'>&#9776;</a>";
        html += "</nav>";
        html += "<div class='container'>";
    }
}

String HTMLGenerator::generateLoginPage(bool hasPassword, const String &error, const String &next)
{
    String html;
    html.reserve(3072);
    addHtmlHeader(html, "Login", false, false);
    html += "<div class='card'>";
    html += "<h2>Login</h2>";
    if (error.length() > 0)
    {
        html += "<p style='color:#c0392b;'><strong>" + htmlEscape(error) + "</strong></p>";
    }
    if (!hasPassword)
    {
        html += "<p>No admin password is set yet. Log in with an empty password and choose one on the next page.</p>";
    }
    html += "<form method='post' action='/login'>";
    html += "<input type='hidden' name='next' value='" + htmlEscape(next) + "'>";
    html += "<label for='password'>Password:</label>";
    html += "<input type='password' id='password' name='password' autocomplete='current-password' autofocus>";
    html += "<input type='submit' value='Log in'>";
    html += "</form></div>";
    addHtmlFooter(html, true); // no update button before login
    return html;
}

String HTMLGenerator::generatePasswordPage(bool hasPassword, const String &error, bool apMode)
{
    String html;
    html.reserve(3072);
    // Until a password exists the user cannot go anywhere else
    addHtmlHeader(html, "Password", apMode, hasPassword);
    html += "<div class='card'>";
    html += "<h2>" + String(hasPassword ? "Change Admin Password" : "Set Admin Password") + "</h2>";
    if (error.length() > 0)
    {
        html += "<p style='color:#c0392b;'><strong>" + htmlEscape(error) + "</strong></p>";
    }
    if (!hasPassword)
    {
        html += "<p>The web interface is not protected yet. Choose a password (at least 8 characters) to continue.</p>";
    }
    else
    {
        html += "<p>Forgotten password: send <code>reset-password</code> over the USB serial console (115200 Bd).</p>";
    }
    html += "<form method='post' action='/password'>";
    if (hasPassword)
    {
        html += "<label for='current'>Current password:</label>";
        html += "<input type='password' id='current' name='current' autocomplete='current-password' required>";
    }
    html += "<label for='password'>New password:</label>";
    html += "<input type='password' id='password' name='password' autocomplete='new-password' minlength='8' maxlength='64' required>";
    html += "<label for='confirm'>Confirm new password:</label>";
    html += "<input type='password' id='confirm' name='confirm' autocomplete='new-password' minlength='8' maxlength='64' required>";
    html += "<input type='submit' value='Save Password'>";
    html += "</form></div>";
    addHtmlFooter(html, true); // no update button before login
    return html;
}

// Adding HTML footer code
void HTMLGenerator::addHtmlFooter(String &html, bool isAPMode)
{
    html += "<footer>";
    html += "<p>" BOARD_NAME " v" + String(FIRMWARE_VERSION) + " &copy; 2026</p>";
    if (!isAPMode)
    {
        html += "<p><button id='btnUpdate' onclick='checkUpdate()' class='btn' style='font-size:11px;padding:5px 10px;'>Check for update</button></p>";
    }

    /* OTA banner (displayed immediately after update starts) */
    html += "<div id='otaNotice' style='display:none;position:fixed;left:50%;transform:translateX(-50%);bottom:16px;z-index:9999;background:#222;color:#fff;padding:10px 14px;border-radius:8px;font-size:13px;box-shadow:0 2px 8px rgba(0,0,0,0.25);'>";
    html += "  <span id='otaMsg'>Firmware update started. Please wait ~2 minutes. The device will reboot automatically.</span>";
    html += "</div>";

    html += "<script>";
    html += "(function(){";
    html += "  window.showNotice = function(msg){";
    html += "    var n=document.getElementById('otaNotice');";
    html += "    var m=document.getElementById('otaMsg');";
    html += "    if(m&&msg) m.textContent=msg;";
    html += "    if(n) n.style.display='block';";
    html += "  };";
    html += "  window.hideNotice = function(){";
    html += "    var n=document.getElementById('otaNotice');";
    html += "    if(n) n.style.display='none';";
    html += "  };";

    html += "  function parseJsonSafe(text){ try{ return JSON.parse(text); }catch(e){ return null; } }";

    html += "  window.checkUpdate = function(tries){";
    html += "    tries = tries || 0;";
    html += "    fetch('/firmware/check')";
    html += "      .then(function(response){";
    html += "        return response.text().then(function(text){ return { ok: response.ok, status: response.status, data: parseJsonSafe(text), raw:text }; });";
    html += "      })";
    html += "      .then(function(res){";
    html += "        if(res.status===202){ if(tries<60){ setTimeout(function(){ window.checkUpdate(tries+1); },1000); return; } throw new Error('No answer from the update server'); }";
    html += "        if(!res.ok){ throw new Error((res.data && (res.data.error||res.data.message)) || ('HTTP '+res.status)); }";
    html += "        var d=res.data; if(!d){ throw new Error('Bad JSON: '+res.raw); }";
    // Only a newer version (numeric compare of x.y.z), never a downgrade
    html += "        var newer=function(a,b){a=String(a).split('.');b=String(b).split('.');for(var i=0;i<Math.max(a.length,b.length);i++){var x=parseInt(a[i]||0,10),y=parseInt(b[i]||0,10);if(x!==y)return x>y;}return false;};";
    html += "        if(d.version && newer(d.version,'" + String(FIRMWARE_VERSION) + "')){";
    html += "          if(confirm('New version '+d.version+' available. Update now?')){";
    html += "            window.updateFirmware(d.url);";
    html += "          }";
    html += "        } else {";
    html += "          alert('You have the latest version: " + String(FIRMWARE_VERSION) + "');";
    html += "        }";
    html += "      })";
    html += "      .catch(function(err){ alert('Update check failed: '+err.message); });";
    html += "  };";

    html += "  window.updateFirmware = function(url){";
    html += "    var btn=document.getElementById('btnUpdate'); if(btn) btn.disabled=true;";
    html += "    window.showNotice('Firmware update started. Please wait ~2 minutes. Do not power off.');";
    html += "    if (typeof startAutoRefresh==='function') { startAutoRefresh(120000); } else { setTimeout(function(){ location.reload(); },120000); }";
    html += "    fetch('/firmware/update',{";
    html += "      method:'POST',";
    html += "      headers:{'Content-Type':'application/x-www-form-urlencoded'},";
    html += "      body:'url='+encodeURIComponent(url)";
    html += "    })";
    html += "    .then(function(response){";
    html += "      return response.text().then(function(text){ return { ok: response.ok, status: response.status, data: parseJsonSafe(text), raw:text }; });";
    html += "    })";
    html += "    .then(function(res){";
    html += "      if(!res.ok){ throw new Error((res.data && (res.data.error||res.data.message)) || ('HTTP '+res.status)); }";
    html += "      if(res.data && res.data.message){ window.showNotice(res.data.message); }";
    html += "    })";
    html += "    .catch(function(err){";
    html += "      console.error('Firmware update error:', err);";
    html += "      window.showNotice('Update request sent. Device may already be updating. If nothing happens, retry. Error: '+err.message);";
    html += "    })";
    html += "    .finally(function(){ if(btn) btn.disabled=false; });";
    html += "  };";

    html += "})();";
    html += "</script>";
        
    html += "</footer>";


    // Adding JavaScript
    addJavaScript(html);

    html += "</body></html>";
}

// Adding CSS for pages
void HTMLGenerator::addStyles(String &html)
{
    html += "<style>";
    html += "* { box-sizing: border-box; }";
    html += "body { font-family: Arial, sans-serif; margin: 0; padding: 0; line-height: 1.6; }";
    html += "header { background: #0066cc; color: white; padding: 20px; text-align: center; }";
    html += "header h1 { margin: 0; }";
    html += "header h2 { margin: 5px 0 0 0; font-weight: normal; }";

    // Navigation
    html += "nav { background: #333; overflow: hidden; }";
    html += "nav a { float: left; display: block; color: white; text-align: center; padding: 14px 16px; text-decoration: none; }";
    html += "nav a:hover { background: #0066cc; }";
    html += "nav a.active { background: #0066cc; }";
    html += "nav .icon { display: none; }";

    // Cards
    html += ".container { padding: 20px; }";
    html += ".card { background: white; border-radius: 5px; padding: 20px; margin-bottom: 20px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }";

    // Tables
    html += "table { width: 100%; border-collapse: collapse; margin-bottom: 20px; }";
    html += "th, td { text-align: left; padding: 12px; border-bottom: 1px solid #ddd; }";
    html += "th { background-color: #f2f2f2; }";
    html += "tr:hover { background-color: #f5f5f5; }";

    // Forms
    html += "form { margin-top: 20px; }";
    html += "label { display: block; margin-bottom: 5px; font-weight: bold; }";
    html += "input[type='text'], input[type='password'], input[type='number'], select, textarea { ";
    html += "  width: 100%; padding: 10px; margin-bottom: 15px; border: 1px solid #ddd; border-radius: 4px; font-size: 14px; }";
    html += "input[type='submit'], button, .btn { ";
    html += "  background: #0066cc; color: white; border: none; padding: 10px 15px; border-radius: 4px; cursor: pointer; ";
    html += "  text-decoration: none; display: inline-block; font-size: 14px; margin-right: 10px; }";
    html += "input[type='submit']:hover, button:hover, .btn:hover { background: #0055aa; }";
    html += ".btn-delete { background: #cc0000; }";
    html += ".btn-delete:hover { background: #aa0000; }";

    // Responsive design
    html += "@media screen and (max-width: 600px) {";
    html += "  nav a:not(:first-child) { display: none; }";
    html += "  nav a.icon { float: right; display: block; }";
    html += "  nav.responsive { position: relative; }";
    html += "  nav.responsive a.icon { position: absolute; right: 0; top: 0; }";
    html += "  nav.responsive a { float: none; display: block; text-align: left; }";
    html += "}";

    // Special styles for logs
    html += ".log-container { background: #f8f8f8; padding: 10px; border-radius: 4px; max-height: 70vh; overflow-y: auto; }";
    html += ".log-entry { padding: 5px; border-bottom: 1px solid #ddd; font-family: monospace; white-space: pre-wrap; }";
    html += ".log-error { color: #ff5555; }";
    html += ".log-warning { color: #ffaa00; }";
    html += ".log-info { color: #2196F3; }";
    html += ".log-debug { color: #4CAF50; }";
    html += ".log-verbose { color: #9E9E9E; }";

    // Footer
    html += "footer { background: #f2f2f2; padding: 10px; text-align: center; font-size: 12px; color: #666; }";

    html += "</style>";
}

// Adding JavaScript for interactive elements
void HTMLGenerator::addJavaScript(String &html)
{
    html += "<script>";

    // Responsive menu
    html += "function toggleMenu() {";
    html += "  var x = document.getElementsByTagName('nav')[0];";
    html += "  if (x.className === '') {";
    html += "    x.className = 'responsive';";
    html += "  } else {";
    html += "    x.className = '';";
    html += "  }";
    html += "}";

    // Automatic page refresh
    html += "function startAutoRefresh(interval) {";
    html += "  setTimeout(function(){ location.reload(); }, interval);";
    html += "}";

    html += "</script>";
}

// Adding navigation
void HTMLGenerator::addNavigation(String &html, const String &activePage)
{
    html += "<nav>";
    html += "<a href='/' class='" + String(activePage == "Home" ? "active" : "") + "'>Home</a>";
    html += "<a href='/network' class='" + String(activePage == "Network" ? "active" : "") + "'>Network</a>";
    html += "<a href='/sensors' class='" + String(activePage == "Sensors" ? "active" : "") + "'>Sensors</a>";
    html += "<a href='/mqtt' class='" + String(activePage == "MQTT" ? "active" : "") + "'>MQTT</a>";
    html += "<a href='/firmware' class='" + String(activePage == "Firmware" ? "active" : "") + "'>Firmware</a>";
    html += "<a href='/logs' class='" + String(activePage == "Logs" ? "active" : "") + "'>Logs</a>";
    html += "<a href='/api' class='" + String(activePage == "API" ? "active" : "") + "'>API</a>";
    html += "<a href='/password' class='" + String(activePage == "Password" ? "active" : "") + "'>Password</a>";
    html += "<a href='/reboot' onclick=\"return confirm('Are you sure you want to reboot the device?');\">Reboot</a>";
    html += "<a href='/logout'>Logout</a>";
    html += "<a href='javascript:void(0);' class='icon' onclick='toggleMenu()'>&#9776;</a>";
    html += "</nav>";

    html += "<div class='container'>";
}

// In HTMLGenerator.cpp, modify the generateHomePage method
String HTMLGenerator::generateHomePage(const std::vector<SensorData> &sensors)
{
    String html;
    html.reserve(4096);

    // Add header with reduced content
    addHtmlHeader(html, "Home", uplink->isCaptive());

    // Status card
    html += "<div class='card'>";
    html += "<h2>System Status</h2>";

    html += "<p><strong>Mode:</strong> " + String(uplink->isOnline() ? "Client" : "Access Point") + "</p>";

#if defined(HAS_ETHERNET)
    if (uplink->ethernetLinkUp())
    {
        html += "<p><strong>Ethernet:</strong> Connected" +
                String(uplink->getEthernetIP() ? ", " + uplink->getEthernetIP().toString() : String()) + "</p>";
    }
    else
    {
        html += "<p><strong>Ethernet:</strong> Disconnected</p>";
    }
#endif
    if (uplink->wifiSwitchedOff())
    {
        html += "<p><strong>WiFi:</strong> Disabled</p>";
    }
    else if (uplink->stationConnected())
    {
        html += "<p><strong>WiFi:</strong> Connected to " + htmlEscape(WiFi.SSID()) + "</p>";
    }
    else
    {
        html += "<p><strong>WiFi:</strong> Disconnected</p>";
    }
    if (uplink->isOnline())
    {
        html += "<p><strong>IP:</strong> " + uplink->getLocalIP().toString() + " (" + uplink->getActiveName() + ")</p>";
    }
    if (uplink->isApActive())
    {
        html += "<p><strong>AP IP:</strong> " + uplink->getApIP().toString() + "</p>";
    }

    // Current time
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0))
    {
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        html += "<p><strong>Time:</strong> " + String(timeStr) + "</p>";
    }
    else
    {
        html += "<p><strong>Time:</strong> Not set</p>";
    }

    unsigned long seconds = millis() / 1000;
    unsigned int days = seconds / 86400;           // 24*60*60
    unsigned int hours = (seconds % 86400) / 3600; // rest of the day
    unsigned int minutes = (seconds % 3600) / 60;  // rest of the hour
    unsigned int secs = seconds % 60;

    html += "<p><strong>Uptime:</strong> ";
    if (days)
        html += String(days) + " d ";
    if (days || hours)
        html += String(hours) + " h ";
    if (days || hours || minutes)
        html += String(minutes) + " m ";
    html += String(secs) + " s</p>";

    html += "</div>";

    // Active sensors - optimize by checking vector size first
    if (!sensors.empty())
    {
        html += "<div class='card'>";
        html += "<h2>Active Sensors</h2>";

        // Generate table with minimal content
        html += "<table>";
        html += "<tr><th>Name</th><th>Type</th><th>Last Seen</th><th>Data</th></tr>";

        for (const auto &sensor : sensors)
        {
            if (sensor.configured)
            {
                html += "<tr>";
                html += "<td>" + htmlEscape(sensor.name) + "</td>";
                html += "<td>" + sensorTypeToString(sensor.deviceType) + "</td>";
                html += "<td>" + sensor.getLastSeenString() + "</td>";

                // Add sensor data - use the getDataString() method
                html += "<td>" + sensor.getDataString() + "</td>";

                html += "</tr>";

                // Yield to other tasks periodically during table generation
                if (&sensor != &sensors.back())
                {
                    yield();
                }
            }
        }

        html += "</table>";
        html += "</div>";
    }

    // Auto-refresh with longer interval in AP mode
    html += "<script>startAutoRefresh(60000);</script>"; // 60 seconds instead of 30

    // Add footer (hide update button in AP mode)
    addHtmlFooter(html, uplink->isCaptive());

    return html;
}


// Generating configuration page
// WiFi settings form, used by the AP setup page and the network page
void HTMLGenerator::addWiFiForm(String &html, const String &ssid, bool hasPassword, const ConfigManager *timeZone)
{
    html += "<div class='card'>";
    html += "<h2>WiFi Settings</h2>";
#if defined(HAS_WIFI_SWITCH)
    if (uplink->wifiSwitchedOff())
    {
        html += "<p><strong>WiFi is switched off</strong> (WiFi Radio on the Network page). Saving WiFi settings "
                "here switches it back on: the gateway restarts and connects to this network.</p>";
    }
#endif
    html += "<form method='post' action='/config'>";
    html += "<label for='ssid'>SSID:</label>";
    html += "<input type='text' id='ssid' name='ssid' value='" + htmlEscape(ssid) + "' required>";
    // Stored secrets never go into the page; empty = keep the stored one
    html += "<label for='password'>Password:</label>";
    html += "<input type='password' id='password' name='password' autocomplete='new-password' placeholder='" +
            String(hasPassword ? "(unchanged)" : "(open network)") + "'>";
    if (timeZone)
    {
        addTimezoneSelect(html, timeZone->timezoneName, timeZone->timezone);
    }

    html += "<input type='submit' value='Save and Restart'>";
    html += "</form>";
    html += "</div>";
}

// Time zone select (field "timezone"): the names of Data/TimeZones.h (an
// option's value is its text), a custom POSIX string from an old setting
// as an extra entry with an empty value (= keep it)
void HTMLGenerator::addTimezoneSelect(String &html, const String &zoneName, const String &zonePosix)
{
    html += "<label for='timezone'>Time zone:</label>";
    html += "<select id='timezone' name='timezone'>";
    if (zoneName.length() == 0)
    {
        html += "<option value='' selected>Custom: " + htmlEscape(zonePosix) + "</option>";
    }
    for (const TimeZones::Zone &zone : TimeZones::ZONES)
    {
        html += zoneName == zone.name ? "<option selected>" : "<option>";
        html += zone.name;
        html += "</option>";
    }
    html += "</select>";
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

String HTMLGenerator::generateConfigPage(const String &ssid, const String &password, bool configMode, const String &ip,
                                        const ConfigManager &config)
{
    String html;
    html.reserve(8192);

    // Add header - pass configMode to suppress navigation in AP mode
    addHtmlHeader(html, "Configuration", configMode);

    // Current status
    html += "<div class='card'>";
    html += "<h2>Current Status</h2>";
    if (configMode)
    {
        html += "<p><strong>Mode:</strong> Access Point</p>";
        html += "<p><strong>AP Name:</strong> " + uplink->getApName() + "</p>";
        html += "<p><strong>AP IP:</strong> " + uplink->getApIP().toString() + "</p>";
    }
    else
    {
        html += "<p><strong>Mode:</strong> Client</p>";
        html += "<p><strong>SSID:</strong> " + htmlEscape(ssid) + "</p>";
        html += "<p><strong>Status:</strong> " + String(uplink->stationConnected() ? "Connected" : "Disconnected") + "</p>";
        if (uplink->stationHasIP())
        {
            html += "<p><strong>IP:</strong> " + uplink->getStationIP().toString() + "</p>";
        }
    }
    html += "</div>";

    addWiFiForm(html, ssid, password.length() > 0, &config);

    // Add footer
    addHtmlFooter(html);

    return html;
}
// Generate MQTT Configuration Page
String HTMLGenerator::generateMqttPage(const String &host, int port, const String &user, const String &password, bool enabled, bool tls,
                                       const String &prefix, bool haEnabled, String &haPrefix, const String &status)
{
    String html;
    html.reserve(4096);

    // Add header
    addHtmlHeader(html, "MQTT Configuration");

    // MQTT configuration form
    html += "<div class='card'>";
    html += "<h2>MQTT Settings</h2>";
    html += "<p>Configure connection to Home Assistant MQTT broker for automatic sensor discovery.</p>";
    html += "<p><strong>Status:</strong> " + htmlEscape(status) + "</p>";
    html += "<form method='post' action='/mqtt'>";

    // Enable MQTT checkbox
    html += "<div class='form-group'>";
    html += "<label for='enabled'>Enable MQTT:</label>";
    html += "<input type='checkbox' id='enabled' name='enabled' value='1'" + String(enabled ? " checked" : "") + ">";
    html += "</div>";

    // TLS MQTTS checkbox
    html += "<div class='form-group'>";
    html += "<label for='tls'>Enable TLS:</label>";
    html += "<input type='checkbox' id='tls' name='tls' value='0'" + String(tls ? " checked" : "") + ">";
    html += "</div>";

    // MQTT Broker Host
    html += "<div class='form-group'>";
    html += "<label for='host'>MQTT Broker Host:</label>";
    html += "<input type='text' id='host' name='host' value='" + htmlEscape(host) + "' required>";
    html += "</div>";

    // MQTT Port
    html += "<div class='form-group'>";
    html += "<label for='port'>MQTT Port:</label>";
    html += "<input type='number' id='port' name='port' value='" + String(port) + "' required min='1' max='65535'>";
    html += "</div>";

    // Username
    html += "<div class='form-group'>";
    html += "<label for='user'>Username (optional):</label>";
    html += "<input type='text' id='user' name='user' value='" + htmlEscape(user) + "'>";
    html += "</div>";

    // Password
    html += "<div class='form-group'>";
    html += "<label for='password'>Password (optional):</label>";
    html += "<input type='password' id='password' name='password' autocomplete='new-password' placeholder='" +
            String(password.length() > 0 ? "(unchanged)" : "") + "'>";
    if (password.length() > 0)
    {
        html += "<label><input type='checkbox' name='clearPassword' value='1'> Remove the stored password</label>";
    }
    html += "</div>";

    // Root prefix
    html += "<div class='form-group'>";
    html += "<label for='prefix'>Root topic:</label>";
    html += "<input type='text' id='prefix' name='prefix' value='" + htmlEscape(prefix) + "'>";
    html += "</div>";

    // Home assistant discovery checkbox
    html += "<div class='form-group'>";
    html += "<label for='tls'>Enable HA Discovery:</label>";
    html += "<input type='checkbox' id='haEnabled' name='haEnabled' value='0'" + String(haEnabled ? " checked" : "") + ">";
    html += "</div>";

    // HA Prefix
    html += "<div class='form-group'>";
    html += "<label for='haPrefix'>HA discovery topic:</label>";
    html += "<input type='text' id='haPrefix' name='haPrefix' value='" + htmlEscape(haPrefix) + "'>";
    html += "</div>";

    html += "<input type='submit' value='Save MQTT Settings'>";
    html += "</form></div>";

    // Add footer
    addHtmlFooter(html);
    return html;
}

String HTMLGenerator::generateNetworkPagePart(const NetworkPageView &view, size_t part)
{
    // Every piece is generated when the response has room for it: the whole
    // page (about 14 kB with the Ethernet, LTE and power cards on the Pro,
    // 11.4 kB on the Lite) never sits in RAM. The largest pieces are the
    // footer (~3.7 kB), the header (~3.4 kB) and the time zone list (~3.5 kB).
    String html;
    switch (part)
    {
    case 0:
        html.reserve(3584);
        addHtmlHeader(html, "Network", view.apMode);
        break;

    case 1:
        // Live uplink state, read in one go (one piece)
        html.reserve(1536);
        html += "<div class='card'>";
        html += "<h2>Status</h2>";
        html += "<p><strong>Active uplink:</strong> " + String(uplink->getActiveName());
        if (uplink->isOnline())
        {
            html += ", " + uplink->getLocalIP().toString();
        }
        html += "</p>";
#if defined(HAS_ETHERNET) || defined(HAS_LTE)
        html += "<p>Priority: Ethernet, WiFi, LTE - the first one with an address carries traffic.</p>";
#endif
#if defined(HAS_PWR_SRC)
        html += "<p><strong>Power source:</strong> " + String(powerSource->getName()) + "</p>";
#endif
        html += "<p><strong>Time:</strong> " + htmlEscape(uplink->getTimeStatus()) + "</p>";
#if defined(HAS_ETHERNET)
        html += "<p><strong>Ethernet:</strong> ";
        if (uplink->getEthernetIP())
        {
            html += "Connected, " + uplink->getEthernetIP().toString();
        }
        else
        {
            html += uplink->ethernetLinkUp() ? (view.ethStatic ? "Link up" : "Link up, waiting for DHCP") : "Disconnected";
        }
        html += String(view.ethStatic ? " (static)" : " (DHCP)") + "</p>";
#endif
        html += "<p><strong>WiFi:</strong> ";
        if (uplink->wifiSwitchedOff())
        {
            html += uplink->isApActive() ? "Disabled (access point up for recovery)" : "Disabled";
        }
        else if (view.ssid.length() == 0)
        {
            html += "Not configured";
        }
        else
        {
            html += (uplink->stationHasIP() ? "Connected, " + uplink->getStationIP().toString() : String("Disconnected")) +
                    " (" + htmlEscape(view.ssid) + ")";
        }
        html += "</p>";
#if defined(HAS_LTE)
        {
            const LteModem &lte = uplink->getLte();
            html += "<p><strong>LTE:</strong> " + String(lte.getStateName());
            if (lte.hasIP())
            {
                html += ", " + lte.localIP().toString();
            }
            html += "</p>";
            if (lte.getOperator().length() > 0)
            {
                html += "<p><strong>Operator:</strong> " + htmlEscape(lte.getOperator()) + ", APN " + htmlEscape(lte.getApn()) + "</p>";
            }
            if (lte.getRssiDbm() != 0)
            {
                html += "<p><strong>Signal:</strong> " + String(lte.getRssiDbm()) + " dBm (CSQ " + String(lte.getSignalQuality()) + ")</p>";
            }
            if (lte.getModuleName().length() > 0)
            {
                html += "<p><strong>Modem:</strong> " + htmlEscape(lte.getModuleName()) + ", IMEI " + htmlEscape(lte.getImei()) + "</p>";
            }
            if (lte.getLastError().length() > 0)
            {
                html += "<p><strong>Last error:</strong> " + htmlEscape(lte.getLastError()) + "</p>";
            }
        }
#endif
        html += "</div>";
        break;

    case 2:
        html += "<div class='card'>";
        html += "<h2>Time</h2>";
        html += "<p><strong>Local time:</strong> " + localTimeText(view.timezone) + "</p>";
        html += "<form method='post' action='/timezone'>";
        break;

    case 3:
        html.reserve(3840);
        addTimezoneSelect(html, view.timezoneName, view.timezone);
        break;

    case 4:
        html += "<p>Summer time (DST) follows the rules of the selected zone. Takes effect immediately.</p>";
        html += "<input type='submit' value='Save'>";
        html += "</form></div>";
        break;

    case 5:
        html.reserve(768);
        addWiFiForm(html, view.ssid, view.hasWifiPassword, nullptr);
        break;

    case 6:
#if defined(HAS_ETHERNET)
    {
        html.reserve(2560);
        auto field = [&html](const char *id, const char *label, const String &value, bool required)
        {
            html += "<div class='form-group eth-static'>";
            html += "<label for='" + String(id) + "'>" + label + ":</label>";
            html += "<input type='text' id='" + String(id) + "' name='" + id + "' maxlength='15' inputmode='decimal'";
            html += " pattern='^(\\d{1,3}\\.){3}\\d{1,3}$' value='" + htmlEscape(value) + "'";
            html += String(required ? " data-required='1'" : "") + ">";
            html += "</div>";
        };
        html += "<div class='card'>";
        html += "<h2>Ethernet Settings</h2>";
        html += "<p>Changes take effect after a restart. With a static address, check it before saving - a wrong one makes the gateway reachable only over WiFi or its access point.</p>";
        html += "<form method='post' action='/network'>";
        html += "<input type='hidden' name='section' value='ethernet'>";
        html += "<div class='form-group'>";
        html += "<label for='ethMode'>Addressing:</label>";
        html += "<select id='ethMode' name='ethMode'>";
        html += "<option value='dhcp'" + String(view.ethStatic ? "" : " selected") + ">DHCP (automatic)</option>";
        html += "<option value='static'" + String(view.ethStatic ? " selected" : "") + ">Static</option>";
        html += "</select></div>";
        field("ip", "IP address", view.ethIp, true);
        field("mask", "Subnet mask", view.ethMask.length() ? view.ethMask : String("255.255.255.0"), true);
        field("gateway", "Gateway", view.ethGateway, true);
        field("dns1", "Primary DNS (empty = gateway)", view.ethDns1, false);
        field("dns2", "Secondary DNS (optional)", view.ethDns2, false);
        html += "<input type='submit' value='Save and Restart'>";
        html += "</form></div>";
        html += "<script>(function(){var m=document.getElementById('ethMode');"
                "function u(){var s=m.value==='static';"
                "document.querySelectorAll('.eth-static').forEach(function(g){g.style.display=s?'':'none';"
                "var i=g.querySelector('input');i.required=s&&i.dataset.required==='1';i.disabled=!s;});}"
                "m.addEventListener('change',u);u();})();</script>";
    }
#endif
        break;

    case 7:
#if defined(HAS_WIFI_SWITCH)
        html.reserve(3072);
        html += "<div class='card'>";
        html += "<h2>WiFi Radio</h2>";
        html += "<p>With WiFi disabled the gateway uses only Ethernet (and LTE) and never connects to a WiFi network. "
                "The access point still runs for 5 min after every restart, and whenever Ethernet has no address for "
                "10 s (until Ethernet has been back for 5 min), so the gateway stays reachable; the rest of the time the "
                "WiFi radio is off, which frees about 30 kB of memory (e.g. for MQTT over TLS). "
                "Changes take effect after a restart.</p>";
        if (view.viaWifi)
        {
            html += "<p style='color:#c0392b;'><strong>You are connected over WiFi.</strong> Disabling WiFi ends this session; ";
            html += view.ethAddress.length() > 0
                        ? "reconnect over Ethernet at <a href='http://" + htmlEscape(view.ethAddress) + "/network'>" + htmlEscape(view.ethAddress) + "</a>.</p>"
                        : String("Ethernet has no address now, so only the recovery access point will be reachable.</p>");
        }
        html += "<form method='post' action='/network' id='wifiForm'>";
        html += "<input type='hidden' name='section' value='wifi'>";
        html += "<div class='form-group'>";
        html += "<label for='wifiEnabled'>Enable WiFi:</label>";
        html += "<input type='checkbox' id='wifiEnabled' name='wifiEnabled' value='1'" + String(view.wifiEnabled ? " checked" : "") + ">";
        html += "</div>";
        html += "<input type='submit' value='Save and Restart'>";
        html += "</form></div>";
        if (view.viaWifi)
        {
            html += "<script>document.getElementById('wifiForm').addEventListener('submit',function(e){"
                    "if(!document.getElementById('wifiEnabled').checked&&!confirm('This page is loaded over WiFi. "
                    "Disabling WiFi will drop this connection after the restart. Continue?'))e.preventDefault();});</script>";
        }
#endif
#if defined(HAS_LTE)
        html.reserve(html.length() + 1536);
        html += "<div class='card'>";
        html += "<h2>LTE Settings</h2>";
        html += "<p>The LTE connection is kept ready and used only when neither Ethernet nor WiFi is available. Changes take effect after a restart.</p>";
        html += "<p>Leave LTE disabled when no modem is fitted.</p>";
        html += "<form method='post' action='/network'>";
        html += "<input type='hidden' name='section' value='lte'>";
        html += "<div class='form-group'>";
        html += "<label for='lteEnabled'>Enable LTE:</label>";
        html += "<input type='checkbox' id='lteEnabled' name='lteEnabled' value='1'" + String(view.lteEnabled ? " checked" : "") + ">";
        html += "</div>";
        html += "<div class='form-group'>";
        html += "<label for='apn'>APN (empty = automatic):</label>";
        html += "<input type='text' id='apn' name='apn' maxlength='63' value='" + htmlEscape(view.lteApn) + "'>";
        html += "</div>";
        html += "<div class='form-group'>";
        html += "<label for='pin'>SIM PIN (optional):</label>";
        html += "<input type='password' id='pin' name='pin' maxlength='8' inputmode='numeric' pattern='[0-9]{4,8}' autocomplete='off' placeholder='" +
                String(view.hasLtePin ? "(unchanged)" : "") + "'>";
        if (view.hasLtePin)
        {
            html += "<label><input type='checkbox' name='clearPin' value='1'> Remove the stored PIN</label>";
        }
        html += "</div>";
        html += "<input type='submit' value='Save and Restart'>";
        html += "</form></div>";
#endif
        break;

    case 8:
        html.reserve(3840);
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

// Generating page with sensor list
// Format milliseconds-since-event as a short relative string ("3 min ago",
// "2 h ago", "5 d ago"). Returns "—" if `hasValue` is false.
static String formatRelativeMillis(unsigned long nowMillis, unsigned long thenMillis, bool hasValue)
{
    if (!hasValue) return "—";
    unsigned long deltaMs = nowMillis - thenMillis; // wrap-safe via unsigned subtraction
    unsigned long secs = deltaMs / 1000UL;
    if (secs < 60)   return String(secs) + " s ago";
    unsigned long mins = secs / 60UL;
    if (mins < 60)   return String(mins) + " min ago";
    unsigned long hrs = mins / 60UL;
    if (hrs < 48)    return String(hrs) + " h ago";
    return String(hrs / 24UL) + " d ago";
}

// Render the health status as a coloured inline-style badge plus optional
// secondary info (last reject reason, recent counts).
static String renderHealthBadge(const SensorHealth::Stats &h, unsigned long nowMillis)
{
    SensorHealth::Status st = SensorHealth::status(h, nowMillis);
    const char *label = "?";
    const char *bg    = "#bdc3c7"; // grey default
    switch (st)
    {
        case SensorHealth::Status::Healthy: label = "Healthy"; bg = "#27ae60"; break;
        case SensorHealth::Status::Errors:  label = "Errors";  bg = "#e74c3c"; break;
        case SensorHealth::Status::Stale:   label = "Stale";   bg = "#f39c12"; break;
        case SensorHealth::Status::Unknown: label = "Unknown"; bg = "#bdc3c7"; break;
    }
    String badge = "<span style='background:";
    badge += bg;
    badge += ";color:#fff;padding:2px 8px;border-radius:10px;font-size:0.85em;font-weight:bold;'>";
    badge += label;
    badge += "</span>";
    return badge;
}

size_t HTMLGenerator::sensorsPageParts(const SensorsPageView &view)
{
    return 5 + view.entries.size() + view.gen2.size();
}

String HTMLGenerator::generateSensorsPagePart(const SensorsPageView &view, size_t part)
{
    // One piece per sensor row, so the page (6 kB plus ~700 bytes per sensor)
    // never sits in RAM; the largest pieces are the header, the footer and
    // the waiting list (8 rows at most), each below 4 kB
    const size_t entries = view.entries.size(), gen2 = view.gen2.size();
    String html;
    if (part == 0)
    {
        html.reserve(3584);
        addHtmlHeader(html, "Sensors");
    }
    else if (part == 1)
    {
        html += "<div class='card'><h2>Configured Sensors</h2>";
        if (entries == 0)
        {
            html += "<p>No sensors configured yet.</p>";
        }
        else
        {
            html += "<table><tr>"
                    "<th>Name</th><th>Type</th><th>Serial</th>"
                    "<th>Status</th><th>Last Seen</th><th>24h</th><th>Actions</th></tr>";
        }
    }
    else if (part < 2 + entries)
    {
        // Every row computes deltas against the same 'now' (the snapshot's).
        // millis() can wrap every ~50 days but unsigned subtraction in
        // formatRelativeMillis handles that correctly.
        const ActiveSensorEntry &entry = view.entries[part - 2];
        html.reserve(1024);
        const auto &s = entry.data;
        size_t idx    = entry.index;
        const auto &h = s.health;

        uint32_t ok24h     = SensorHealth::okLast24h(h);
        uint32_t rejected  = SensorHealth::rejectedLast24h(h);

        html += "<tr>";
        html += "<td>" + htmlEscape(s.name) + "</td>";
        html += "<td>" + sensorTypeToString(s.deviceType) + "</td>";
        html += "<td>" + formatSN(s.serialNumber) + "</td>";

        // Status: badge + optional last reject reason on the line below
        html += "<td>";
        html += renderHealthBadge(h, view.nowMillis);
        if (h.hasLastReject && h.consecutiveRejections > 0)
        {
            html += "<br><small style='color:#c0392b;'>";
            html += "last reject: ";
            html += htmlEscape(String(h.lastRejectReason));
            html += " (";
            html += formatRelativeMillis(view.nowMillis, h.lastRejectAtMillis, true);
            html += ")</small>";
        }
        html += "</td>";

        // Last seen: prefer relative form derived from our millis snapshot;
        // the wall-clock form (getLastSeenString) is kept as a tooltip so
        // users can still see the exact timestamp if NTP is synced.
        html += "<td title='" + s.getLastSeenString() + "'>";
        html += formatRelativeMillis(view.nowMillis, h.lastOkAtMillis, h.hasLastOk);
        html += "</td>";

        // 24h column: minimal noise when healthy, loud when broken.
        //   - no activity:       em-dash
        //   - all OK:            plain count
        //   - has rejections:    "<ok> · <reject> err" with reject part red+bold
        html += "<td title='Last 24 h sliding window'>";
        if (ok24h == 0 && rejected == 0)
        {
            html += "&mdash;";
        }
        else if (rejected == 0)
        {
            html += String(ok24h);
        }
        else
        {
            html += String(ok24h);
            html += " &middot; <span style='color:#c0392b;font-weight:bold;'>";
            html += String(rejected);
            html += " err</span>";
        }
        html += "</td>";

        html += "<td>";
        html += "<a class='btn' href='/sensors/edit?index=" + String(idx) + "'>Edit</a> ";
        html += "<a class='btn btn-delete' href='/sensors/delete?index=" + String(idx) + "&sn=" +
                formatSN(s.serialNumber) + "' onclick='return confirm(\"Are you sure you want to delete this sensor?\")'>Delete</a>";
        html += "</td></tr>";
    }
    else if (part == 2 + entries)
    {
        if (entries > 0)
        {
            html += "</table>";
        }
        html += "<p><a class='btn' href='/sensors/add'>Add New Sensor</a></p>";
        html += "</div>";

        // Generation 2 sensors (protocol v2): identity and what they reported
        html += "<div class='card'><h2>Generation 2 Sensors</h2>";
        {
            // Downlink duty cycle of the gateway (869.525 MHz, 10 % = 360 s per hour)
            RadioTask::Stats rs = RadioTask::stats();
            bool high = (uint64_t)rs.dutyUsedMs * 100 >= 360000ULL * 80;
            html += "<p><small>Downlink duty cycle: " + String(rs.dutyUsedMs / 1000.0f, 1) +
                    " s of 360 s in the last hour (JOIN_ACC " + String(rs.joinUsedMs / 1000.0f, 1) + " s); responses RX1 " +
                    String(rs.txRx1) + ", RX2 " + String(rs.txRx2) + "</small>";
            if (high)
            {
                html += " <span style='color:#c0392b;'>above 80 % &ndash; only join answers once the data share is used "
                        "up</span>";
            }
            html += "</p>";
        }
        if (gen2 == 0)
        {
            html += "<p>No generation 2 sensors added yet.</p>";
        }
        else
        {
            html += "<table><tr><th>Name</th><th>DevID</th><th>Key</th><th>Reported</th>"
                    "<th>Last heard</th><th>Counters</th><th>Actions</th></tr>";
        }
    }
    else if (part < 3 + entries + gen2)
    {
        const Gen2Sensor &g = view.gen2[part - 3 - entries];
        html.reserve(1536);
        // The label key is the root secret: only its first 4 hex digits,
        // enough to tell which label was entered
        char id[9], key[16];
        snprintf(id, sizeof(id), "%08lX", (unsigned long)g.devId);
        snprintf(key, sizeof(key), "%02X%02X&hellip;", g.label[0], g.label[1]);
        html += "<tr><td>" + htmlEscape(g.name) + "</td><td><code>" + String(id) + "</code></td>";
        html += "<td><code>" + String(key) + "</code></td><td>";
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
        if (g.seen)
        {
            html += "<small>" + String(g.rssi) + " dBm, SNR " + String(g.snr, 1) +
                    " dB, " + String(g.freqError / 1000.0f, 1) + " kHz</small>";
            if (g.temperature != INT16_MIN)
            {
                html += "<br>" + String(g.temperature / 100.0f, 2) + " &deg;C";
            }
            if (g.humidity != 0xFFFF)
            {
                html += ", " + String(g.humidity / 100.0f, 1) + " %";
            }
            if (g.batteryMv != 0xFFFF)
            {
                html += ", " + String(g.batteryMv) + " mV";
            }
        }
        else
        {
            html += "&mdash;";
        }
        html += "</td><td>" + formatRelativeMillis(view.nowMillis, g.lastSeenMs, g.seen);
        if (g.seen)
        {
            html += "<br><small>devNonce " + String(g.lastJoinDevNonce) + "<br>";
            html += g.sessionUp ? "session up, FCnt " + String(g.lastFcnt) : String("no session yet");
            if (g.sessionUp && !Gen2Config::inSync(g.configVersion, g.cfgVersion))
            {
                html += "<br><span style='color:#e67e22;'>config v" + String(g.cfgVersion) +
                        " waiting for the sensor</span>";
            }
            if (g.downlinkRssi != INT8_MIN)
            {
                html += "<br>downlink " + String(g.downlinkRssi) + " dBm at the sensor";
            }
            html += "</small>";
        }
        html += "</td><td><small>JOIN_REQ " + String(g.joinRequests) + "<br>MIC failed " +
                String(g.micFailures);
        if (g.micFailures > 0 && g.joinRequests == 0)
        {
            html += " <span style='color:#c0392b;'>(wrong key, or paired with another gateway?)</span>";
        }
        html += "<br>replayed " + String(g.replayedJoins) + "<br>UP " + String(g.upFrames) + ", w/o session " +
                String(g.upWithoutSession) + "<br>JOIN_ACC " + String(g.joinAccepts) + ", REJOIN " +
                String(g.rejoins) + "<br>DOWN RX1 " + String(g.downRx1) + ", RX2 " + String(g.downRx2) +
                ", not sent " + String(g.downNotSent) + "</small></td>";
        html += "<td><a class='btn' href='/sensors/gen2?devId=" + String(id) + "'>Details</a> ";
        html += "<form method='post' action='/sensors/delete2' style='display:inline' "
                "onsubmit='return confirm(\"Delete this generation 2 sensor?\")'><input type='hidden' "
                "name='devId' value='" + String(id) + "'><button type='submit' class='btn-delete'>Delete</button>"
                "</form></td></tr>";
    }
    else if (part == 3 + entries + gen2)
    {
        html.reserve(3072);
        if (gen2 > 0)
        {
            html += "</table>";
        }
        html += "<p><a class='btn' href='/sensors/add2'>Add Generation 2 Sensor</a></p>";

        html += "<h3>Waiting to be added</h3>";
        if (view.waitingCount == 0)
        {
            html += "<p><small>No unknown generation 2 sensors heard (a sensor appears here after two "
                    "consistent join requests).</small></p>";
        }
        else
        {
            html += "<table><tr><th>DevID</th><th>HW / FW</th><th>RSSI</th><th>Requests</th><th>Last heard</th>"
                    "<th></th></tr>";
            for (size_t i = 0; i < view.waitingCount; i++)
            {
                const WaitingEntry &w = view.waiting[i];
                char id[9];
                snprintf(id, sizeof(id), "%08lX", (unsigned long)w.devId);
                html += "<tr><td><code>" + String(id) + "</code></td><td>HW " + String(w.hwType) + ", FW " +
                        String(w.fwMajor) + "." + String(w.fwMinor) + "." + String(w.fwPatch) + "</td><td>" +
                        String(w.rssi) + " dBm</td><td>" + String(w.frames) + "</td><td>" +
                        formatRelativeMillis(view.nowMillis, w.lastSeenMs, true) + "</td>";
                html += "<td><a class='btn' href='/sensors/add2?devId=" + String(id) + "'>Add</a></td></tr>";
            }
            html += "</table><p><small>Join requests are not authenticated until the label key is entered: "
                    "compare the DevID with the label.</small></p>";
        }
        html += "</div>";
    }
    else if (part == 4 + entries + gen2)
    {
        html.reserve(3840);
        addHtmlFooter(html);
    }
    return html;
}

String HTMLGenerator::generateGen2AddPage(const String &devId, const String &error)
{
    String html;
    html.reserve(3072);
    addHtmlHeader(html, "Add Generation 2 Sensor");
    html += "<div class='card'><h2>Add Generation 2 Sensor</h2>";
    if (error.length() > 0)
    {
        html += "<p style='color:#c0392b;'>" + htmlEscape(error) + "</p>";
    }
    html += "<form method='post' action='/sensors/add2'>";
    html += "<label for='name'>Sensor Name:</label>";
    html += "<input type='text' id='name' name='name' maxlength='32' required>";
    html += "<label for='devId'>DevID (8 hex digits from the label):</label>";
    html += "<input type='text' id='devId' name='devId' maxlength='8' pattern='[0-9A-Fa-f]{8}' required value='" +
            htmlEscape(devId) + "'>";
    html += "<label for='key'>Key (16 hex digits from the label, dashes optional):</label>";
    html += "<input type='text' id='key' name='key' maxlength='19' autocomplete='off' "
            "pattern='[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}' required "
            "placeholder='A1B2-C3D4-E5F6-0718'>";
    html += "<button type='submit'>Add Sensor</button> <a class='btn' href='/sensors'>Cancel</a>";
    html += "</form></div>";
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
        html += "<tr><td>RTC</td><td><span style='color:#c0392b;'>not usable (LSE / binary mode / clock source) "
                "&ndash; updates cannot be confirmed</span></td></tr>";
    }
    if (v[21] & 2)
    {
        html += "<tr><td>Watchdog</td><td><span style='color:#c0392b;'>not started (IWDG_STOP option byte not "
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
        return "<span style='color:#e67e22;'>Hint: " + signal + " &ndash; consider " + String(r.power) + " dBm" +
               (r.power > Gen2Config::EU_TX_POWER_DBM
                    ? " &ndash; above the EU limit of " + String(Gen2Config::EU_TX_POWER_DBM) +
                          " dBm ERP, see the warning"
                    : String("; legal limits apply")) +
               ".</span>";
    case Gen2Link::Advice::AtMaximum:
        return "<span style='color:#e67e22;'>Hint: " + signal +
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

static String otaReason(const OtaJob::Job &j)
{
    String r = OtaJob::whyText(j.why);
    if (j.why == OtaJob::Why::Refused)
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

static String otaButton(const char *id, const char *action, const char *label, const char *cls = "btn",
                        const char *ask = nullptr)
{
    String h = "<form method='post' action='/sensors/gen2/ota' style='display:inline'";
    if (ask)
    {
        h += " onsubmit='return confirm(\"" + String(ask) + "\")'";
    }
    h += "><input type='hidden' name='devId' value='" + String(id) + "'><input type='hidden' name='action' value='" +
         action + "'><button type='submit' class='" + cls + "'>" + label + "</button></form> ";
    return h;
}

// OTA card of the generation 2 detail page (spec 14.5)
static void addGen2OtaCard(String &html, const Gen2Sensor &g, const Gen2OtaView &v, const char *id)
{
    html += "<div class='card'><h2>Firmware update over the air</h2>";
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
        const String reason = otaReason(j);
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
        if (j.manualPause)
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
            html += otaButton(id, "abort", "Abort", "btn-delete",
                              "Abort the update? A sensor still in its OTA session discards the transfer; otherwise "
                              "its transfer state may stay and a job with the same image would resume it.");
        }
        if (OtaJob::canClear(j))
        {
            html += otaButton(id, "clear", "Clear job", "btn-delete",
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
                    html += "<p style='color:#c0392b;'>" + HTMLGenerator::htmlEscape(v.confirm) +
                            "</p><label><input type='checkbox' name='confirm' value='1'> I confirm</label> ";
                }
                html += "<button type='submit' class='btn'>Retry" +
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
            html += "<p style='color:#c0392b;'>" + HTMLGenerator::htmlEscape(v.blockers) + "</p>";
        }
        if (v.warnings.length() > 0)
        {
            html += "<p style='color:#e67e22;'>Note: " + HTMLGenerator::htmlEscape(v.warnings) + "</p>";
        }
        if (v.imageOk && v.blockers.length() == 0 && !(mine && OtaJob::canRetry(j)))
        {
            html += "<form method='post' action='/sensors/gen2/ota'><input type='hidden' name='devId' value='" +
                    String(id) + "'><input type='hidden' name='action' value='start'>";
            if (v.confirm.length() > 0)
            {
                html += "<p style='color:#c0392b;'>" + HTMLGenerator::htmlEscape(v.confirm) +
                        "</p><label><input type='checkbox' name='confirm' value='1'> I confirm</label> ";
            }
            html += "<button type='submit' class='btn'>Update to v" + fwText(v.imageFw) + "</button></form>";
        }
    }
    // Image store
    html += "<h3>Images</h3><table><tr><th>HW type</th><th>Version</th><th>Size</th><th></th></tr>";
    for (size_t i = 0; i < v.imageCount; i++)
    {
        const OtaStore::Meta &m = v.images[i];
        char hw[8];
        snprintf(hw, sizeof(hw), "%04X", m.hwType);
        html += "<tr><td>0x" + String(hw) + "</td><td>v" + fwText(m.fw) + "</td><td>" + String(m.len) +
                " B</td><td><form method='post' action='/ota/delete' onsubmit='return confirm(\"Delete this image?\")'>"
                "<input type='hidden' name='hw' value='" +
                String(m.hwType) + "'><input type='hidden' name='devId' value='" + String(id) +
                "'><button type='submit' class='btn-delete'>Delete</button></form></td></tr>";
    }
    if (v.imageCount == 0)
    {
        html += "<tr><td colspan='4'>none</td></tr>";
    }
    html += "</table>";
    html += "<form method='post' action='/ota/upload?devId=" + String(id) +
            "' enctype='multipart/form-data'><p>Upload a sensor image (<code>firmware.bin</code> with the trailer, "
            "built with the OTA receiver): <input type='file' name='image' accept='.bin' required> "
            "<button type='submit' class='btn'>Upload</button></p></form></div>";
}

String HTMLGenerator::generateGen2DetailPage(const Gen2Sensor &g, const String &message, const Gen2OtaView *ota)
{
    String html;
    html.reserve(9000);
    char id[9];
    snprintf(id, sizeof(id), "%08lX", (unsigned long)g.devId);
    const unsigned long now = millis();
    const int8_t txNow = g.effectiveTxPower(); // configured, or what the status report says the sensor uses
    const Gen2Link::Margins link = Gen2Link::margins(g.link, gen2LinkNowS(), txNow);
    const String recommendation = txPowerAdvice(link, txNow);
    addHtmlHeader(html, "Generation 2 Sensor");
    html += "<div class='card'><h2>" + htmlEscape(g.name) + " <small><code>" + String(id) + "</code></small></h2>";
    if (message.length() > 0)
    {
        html += "<p><strong>" + htmlEscape(message) + "</strong></p>";
    }
    html += "<table>";
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
    html += link.lost > 0 ? "<br><span style='color:#e67e22;'>" + String(link.lost) +
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
        html += "<br><span style='color:#c0392b;'>" + txPowerWarning() + "</span>";
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
    html += "</div>";

    // Configuration
    const bool inSync = Gen2Config::inSync(g.configVersion, g.cfgVersion);
    const Gen2Config::Values &c = g.config;
    html += "<div class='card'><h2>Configuration</h2><p>";
    if (!g.sessionUp)
    {
        html += "Configuration v" + String(g.cfgVersion) + " is sent with the next join.";
    }
    else if (inSync)
    {
        html += "<span style='color:#27ae60;'>Set</span> &ndash; the sensor runs v" + String(g.cfgVersion) + ".";
    }
    else
    {
        html += "<span style='color:#e67e22;'>Waiting for the sensor</span> &ndash; v" + String(g.cfgVersion) +
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
            "' oninput=\"document.getElementById('txWarn').style.display=this.value>" +
            String(Gen2Config::EU_TX_POWER_DBM) + "?'block':'none'\">";
    html += "<p id='txWarn' style='color:#c0392b;display:" +
            String(c.txPowerDbm > Gen2Config::EU_TX_POWER_DBM ? "block" : "none") + ";'>" + txPowerWarning() +
            "</p>";
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
    html += "<button type='submit'>Save configuration</button></form></div>";

    // Status report
    html += "<div class='card'><h2>Status report</h2>";
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
                              : "<button type='submit'>Request status</button>";
    html += "</form></div>";

    // Downlink
    html += "<div class='card'><h2>Downlink</h2><table>";
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
    html += "</table></div>";

    // Join / devNonce
    html += "<div class='card'><h2>Join</h2><table>";
    html += "<tr><td>Last accepted devNonce</td><td>" + (g.hasDevNonce ? String(g.lastDevNonce) : String("none")) +
            "</td></tr>";
    html += "<tr><td>JOIN_REQ verified</td><td>" + String(g.joinRequests) + "</td></tr>";
    html += "<tr><td>JOIN_REQ rejected: devNonce not above the last accepted one</td><td>" +
            String(g.replayedJoins) + "</td></tr></table>";
    if (g.staleJoinStreak >= 3)
    {
        html += "<p style='color:#c0392b;'>The sensor keeps joining with an old devNonce: it has probably lost "
                "its configuration (reflashed with a full erase, damaged store). Reset the devNonce right before "
                "restarting the sensor.</p>";
    }
    html += "<form method='post' action='/sensors/gen2/resetnonce' onsubmit='return confirm(\"Accept the next join "
            "of this sensor with any devNonce?\")'><input type='hidden' name='devId' value='" +
            String(id) + "'><button type='submit' class='btn-delete'>Reset devNonce</button></form></div>";
    if (ota)
    {
        addGen2OtaCard(html, g, *ota, id);
    }
    html += "<p><a class='btn' href='/sensors'>Back to Sensors</a></p>";
    addHtmlFooter(html);
    return html;
}

String HTMLGenerator::generateFirmwarePage() {
    String html;
    html.reserve(4096);
    addHtmlHeader(html, "Firmware");

    // card: Online update
    html += "<div class='card'>";
    html += "<h2>Online Update</h2>";
    html += "<p>Current firmware: <strong>v" + String(FIRMWARE_VERSION) + "</strong></p>";
    html += "<p><button id='btnUpdate' onclick='checkUpdate()' class='btn'>Check for update</button></p>";
    html += "<hr>";
    html += "<label for='fwUrl'>Custom firmware URL:</label>";
    html += "<input type='text' id='fwUrl' placeholder='https://example.com/firmware.bin'>";
    html += "<button class='btn' onclick='(function(){var u=document.getElementById(\"fwUrl\").value.trim(); if(u){ updateFirmware(u); } else { alert(\"Enter firmware URL first\"); }})()'>Update from URL</button>";
    html += "</div>";

    // Card: Manual upload (.bin)
    html += "<div class='card'>";
    html += "<h2>Manual Upload (.bin)</h2>";
    html += "<form id='uploadForm' enctype='multipart/form-data'>";
    html += "  <input type='file' name='bin' id='fwFile' accept='.bin,application/octet-stream' required>";
    html += "  <p><small>Do not power off the device during upload and install.</small></p>";
    html += "  <button type='submit' id='btnUpload' class='btn'>Upload & Install</button>";
    html += "</form>";
    html += "</div>";

    html += "<script>";
    html += "(function(){";
    html += "  function parseJsonSafe(t){ try{return JSON.parse(t);}catch(e){return null;} }";
    html += "  var f=document.getElementById('uploadForm');";
    html += "  var b=document.getElementById('btnUpload');";
    html += "  if(f){";
    html += "    f.addEventListener('submit', function(ev){";
    html += "      ev.preventDefault();";
    html += "      if(!document.getElementById('fwFile').files.length){ alert('Select .bin file first'); return; }";
    html += "      if(b) b.disabled=true;";
    html += "      if(window.showNotice){ window.showNotice('Uploading firmware… Please wait ~2 minutes. Do not power off.'); }";
    html += "      var fd=new FormData(f);";
    html += "      fetch('/firmware',{ method:'POST', body:fd })";
    html += "        .then(function(resp){ return resp.text().then(function(t){ return {ok:resp.ok,status:resp.status,data:parseJsonSafe(t),raw:t}; }); })";
    html += "        .then(function(res){";
    html += "          if(!res.ok){ throw new Error((res.data&&(res.data.error||res.data.message))||('HTTP '+res.status)); }";
    html += "          var msg=(res.data&&res.data.message)?res.data.message:'Firmware uploaded. Installing… device will reboot automatically.';";
    html += "          if(window.showNotice){ window.showNotice(msg); } else { alert(msg); }";
    html += "          setTimeout(function(){ location.reload(); },60000);";
    html += "        })";
    html += "        .catch(function(err){";
    html += "          console.error('Upload error:',err);";
    html += "          if(window.showNotice){ window.showNotice('Upload interrupted - if the session expired, reload the page and log in again. Error: '+err.message); }";
    html += "          else { alert('Upload failed: '+err.message); }";
    html += "        })";
    html += "        .finally(function(){ if(b) b.disabled=false; });";
    html += "    });";
    html += "  }";
    html += "})();";
    html += "</script>";

    addHtmlFooter(html);
    return html;
}

// Generating page for adding a sensor
String HTMLGenerator::generateSensorAddPage()
{
    String html;
    html.reserve(4096);

    // Adding header
    addHtmlHeader(html, "Add Sensor");

    // Form for adding a sensor
    html += "<div class='card'>";
    html += "<h2>Add New Sensor</h2>";
    html += "<form method='post' action='/sensors/add'>";

    // Sensor name
    html += "<label for='name'>Sensor Name:</label>";
    html += "<input type='text' id='name' name='name' required>";

    // Sensor type
    html += "<label for='deviceType'>Device Type:</label>";
    html += "<select id='deviceType' name='deviceType'>";

    // Dynamically generate sensor types from definition
    for (const auto &type : SENSOR_TYPE_DEFINITIONS)
    {
        if (type.type != SensorType::UNKNOWN)
        {
            html += "<option value='" + String(static_cast<uint8_t>(type.type)) + "'>" +
                    String(type.name);

            // Adding information about sensor capabilities
            html += " - ";
            bool first = true;

            if (type.hasTemperature)
            {
                html += "Temperature";
                first = false;
            }

            if (type.hasHumidity)
            {
                html += (first ? "" : ", ") + String("Humidity");
                first = false;
            }

            if (type.hasPressure)
            {
                html += (first ? "" : ", ") + String("Pressure");
                first = false;
            }

            if (type.hasPPM)
            {
                html += (first ? "" : ", ") + String("CO2");
                first = false;
            }

            if (type.hasLux)
            {
                html += (first ? "" : ", ") + String("Light");
            }

            if (type.hasRainAmount)
            {
                html += (first ? "" : ", ") + String("Rain");
            }
            if (type.hasWindSpeed)
            {
                html += (first ? "" : ", ") + String("Wind");
            }

            html += "</option>";
        }
    }

    html += "</select>";

    // Serial number
    html += "<label for='serialNumber'>Serial Number:</label>";
    html += "<input type='text' id='serialNumber' name='serialNumber' placeholder='e.g. 1234567A' required>";

    // Device key
    html += "<label for='deviceKey'>Device Key:</label>";
    html += "<input type='text' id='deviceKey' name='deviceKey' placeholder='e.g. DEADBEEF' required>";

    // New code with a single field
    html += "<label for='customUrl'>Custom URL with Placeholders (Optional):</label>";
    html += "<div id='urlHelp' style='margin-bottom: 10px; font-size: 0.9em;'>";
    html += "<p>Available placeholders:</p>";
    html += "<style>";
    html += ".placeholder-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: 15px; }";
    html += ".placeholder-item { background: #f5f5f5; padding: 8px; border-radius: 4px; }";
    html += ".placeholder-item code { background: #e0e0e0; padding: 2px 4px; border-radius: 3px; font-family: monospace; color: #0066cc; }";
    html += "</style>";
    html += "<div class='placeholder-grid' style='display: grid; grid-template-columns: repeat(3, 1fr); gap: 5px;'>";
    // Environment placeholders
    html += "<div id='tempPlaceholder' class='placeholder-item'>Temperature <code>*TEMP*</code></div>";
    html += "<div id='humPlaceholder' class='placeholder-item'>Humidity <code>*HUM*</code></div>";
    html += "<div id='pressPlaceholder' class='placeholder-item'>Pressure <code>*PRESS*</code></div>";
    html += "<div id='ppmPlaceholder' class='placeholder-item'>CO2 <code>*PPM*</code></div>";
    html += "<div id='luxPlaceholder' class='placeholder-item'>Light <code>*LUX*</code></div>";
    // Weather specific
    html += "<div id='windSpeedPlaceholder' class='placeholder-item'>Wind Speed <code>*WIND_SPEED*</code></div>";
    html += "<div id='windDirPlaceholder' class='placeholder-item'>Wind Direction <code>*WIND_DIR*</code></div>";
    html += "<div id='rainPlaceholder' class='placeholder-item'>Rain Amount <code>*RAIN*</code></div>";
    html += "<div id='dailyRainPlaceholder' class='placeholder-item'>Rain since midnight <code>*DAILY_RAIN*</code></div>";
    html += "<div id='rainRatePlaceholder' class='placeholder-item'>Rain Rate <code>*RAIN_RATE*</code></div>";
    // Device info
    html += "<div class='placeholder-item'>Battery <code>*BAT*</code></div>";
    html += "<div class='placeholder-item'>Signal <code>*RSSI*</code></div>";
    html += "<div class='placeholder-item'>Serial Number <code>*SN*</code></div>";
    html += "<div class='placeholder-item'>Device Type <code>*TYPE*</code></div>";
    html += "</div>"; // end of grid
    html += "</div>"; // end of urlHelp
    html += "<input type='text' id='customUrl' name='customUrl' placeholder='https://example.com/api?temp=*TEMP*&hum=*HUM*'>";

    html += "<h3 style='margin-top: 20px;'>Sensor Reading Corrections</h3>";
    html += "<p>Adjust sensor readings by adding offsets or applying multipliers. Leave at 0 for no correction.</p>";

    // Create a container for correction inputs with two columns
    html += "<div style='display: grid; grid-template-columns: 1fr 1fr; gap: 10px;'>";

    // Add the correction fields with ids that match the dynamic visibility code
    html += "<div id='tempCorrDiv' style='display: none;'>";
    html += "<label for='tempCorr'>Temperature Correction (±°C):</label>";
    html += "<input type='number' id='tempCorr' name='tempCorr' value='0.0' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Add similar divs for other correction fields with default values
    // Humidity
    html += "<div id='humCorrDiv' style='display: none;'>";
    html += "<label for='humCorr'>Humidity Correction (±%):</label>";
    html += "<input type='number' id='humCorr' name='humCorr' value='0.0' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Pressure
    html += "<div id='pressCorrDiv' style='display: none;'>";
    html += "<label for='pressCorr'>Pressure Correction (±hPa):</label>";
    html += "<input type='number' id='pressCorr' name='pressCorr' value='0.0' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // CO2
    html += "<div id='ppmCorrDiv' style='display: none;'>";
    html += "<label for='ppmCorr'>CO2 Correction (±ppm):</label>";
    html += "<input type='number' id='ppmCorr' name='ppmCorr' value='0' step='1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Lux
    html += "<div id='luxCorrDiv' style='display: none;'>";
    html += "<label for='luxCorr'>Light Correction (±lux):</label>";
    html += "<input type='number' id='luxCorr' name='luxCorr' value='0.0' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Wind speed
    html += "<div id='windSpeedCorrDiv' style='display: none;'>";
    html += "<label for='windSpeedCorr'>Wind Speed Correction (multiplier):</label>";
    html += "<input type='number' id='windSpeedCorr' name='windSpeedCorr' value='1.0' step='0.01' min='0.1' max='10'>";
    html += "<small>Values greater than 1.0 increase the reading, less than 1.0 decrease it</small>";
    html += "</div>";

    // Wind direction
    html += "<div id='windDirCorrDiv' style='display: none;'>";
    html += "<label for='windDirCorr'>Wind Direction Correction (±degrees):</label>";
    html += "<input type='number' id='windDirCorr' name='windDirCorr' value='0' step='1' min='-180' max='180'>";
    html += "<small>Positive values rotate clockwise, negative counter-clockwise</small>";
    html += "</div>";

    // Rain amount
    html += "<div id='rainAmountCorrDiv' style='display: none;'>";
    html += "<label for='rainAmountCorr'>Rain Amount Correction (multiplier):</label>";
    html += "<input type='number' id='rainAmountCorr' name='rainAmountCorr' value='1.0' step='0.01' min='0.1' max='10'>";
    html += "<small>Values greater than 1.0 increase the reading, less than 1.0 decrease it</small>";
    html += "</div>";

    // Rain rate
    html += "<div id='rainRateCorrDiv' style='display: none;'>";
    html += "<label for='rainRateCorr'>Rain Rate Correction (multiplier):</label>";
    html += "<input type='number' id='rainRateCorr' name='rainRateCorr' value='1.0' step='0.01' min='0.1' max='10'>";
    html += "<small>Values greater than 1.0 increase the reading, less than 1.0 decrease it</small>";
    html += "</div>";

    // Altitude correction
    html += "<div id='altitudeCorrDiv' style='display: none;'>";
    html += "<label for='altitude'>Altitude (m) - For pressure adjustment:</label>";
    html += "<input type='number' id='altitude' name='altitude' value='0' min='0' max='8848'>";
    html += "<small>Used to convert relative pressure to absolute pressure</small>";
    html += "</div>";

    html += "</div>"; // End of grid

    // Buttons
    html += "<input type='submit' value='Add Sensor'>";
    html += "<a href='/sensors' class='btn' style='background: #999;'>Cancel</a>";
    html += "</form>";
    html += "</div>";
    html += "<script>";
    html += "document.addEventListener('DOMContentLoaded', function() {";
    html += "  var deviceTypeSelect = document.getElementById('deviceType');";
    html += "  var altitudeDiv = document.getElementById('altitudeDiv');";
    html += "  function updateFieldVisibility() {";
    html += "    var type = parseInt(deviceTypeSelect.value);";
    html += "    console.log('Selected device type:', type);";

    // Define which devices have which capabilities
    html += "    var tempDevices = [1, 2, 3, 5, 81];"; // BME280, SCD40, METEO, SHT40, DIY_TEMP
    html += "    var humDevices = [1, 2, 3, 5];";   // BME280, SCD40, METEO, SHT40
    html += "    var pressureDevices = [1, 3];";    // BME280, METEO
    html += "    var co2Devices = [2];";            // SCD40
    html += "    var luxDevices = [4];";            // VEML7700
    html += "    var weatherDevices = [3];";        // METEO

    // Update regular fields
    html += "    document.getElementById('altitudeCorrDiv').style.display = pressureDevices.includes(type) ? 'block' : 'none';";

    // Update correction fields
    html += "    document.getElementById('tempCorrDiv').style.display = tempDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('humCorrDiv').style.display = humDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('pressCorrDiv').style.display = pressureDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('ppmCorrDiv').style.display = co2Devices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('luxCorrDiv').style.display = luxDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windSpeedCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windDirCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainAmountCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainRateCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";

    // Update placeholder visibility
    html += "    updatePlaceholderVisibility();";
    html += "  }";

    html += "  function updatePlaceholderVisibility() {";
    html += "    var type = parseInt(deviceTypeSelect.value);";
    html += "    console.log('Selected device type:', type);"; // Debugging

    // Device type definitions for individual placeholders
    html += "    var tempDevices = [1, 2, 3, 5, 81];"; // BME280, SCD40, METEO, SHT40, DIY_TEMP
    html += "    var humDevices = [1, 2, 3, 5];";   // BME280, SCD40, METEO, SHT40
    html += "    var pressureDevices = [1, 3];";    // BME280, METEO
    html += "    var co2Devices = [2];";            // SCD40
    html += "    var luxDevices = [4];";            // VEML7700
    html += "    var weatherDevices = [3];";        // METEO

    // Update display
    html += "    document.getElementById('tempPlaceholder').style.display = tempDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('humPlaceholder').style.display = humDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('pressPlaceholder').style.display = pressureDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('ppmPlaceholder').style.display = co2Devices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('luxPlaceholder').style.display = luxDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windSpeedPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windDirPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('dailyRainPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainRatePlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "  }";

    // Run updateFieldVisibility when the page loads and when the type changes
    html += "  updateFieldVisibility();";
    html += "  deviceTypeSelect.addEventListener('change', updateFieldVisibility);";
    html += "});";
    html += "</script>";
    // Adding footer
    addHtmlFooter(html);

    return html;
}

// Generating page for editing a sensor
String HTMLGenerator::generateSensorEditPage(const SensorData &sensor, int index)
{
    String html;
    html.reserve(8192);

    // Adding header
    addHtmlHeader(html, "Edit Sensor");

    // Form for editing a sensor
    html += "<div class='card'>";
    html += "<h2>Edit Sensor</h2>";
    html += "<form method='post' action='/sensors/update'>";

    // Hidden index
    html += "<input type='hidden' name='index' value='" + String(index) + "'>";
    // The slot may be reused by another sensor before this form is sent
    html += "<input type='hidden' name='sn' value='" + formatSN(sensor.serialNumber) + "'>";

    // Sensor name
    html += "<label for='name'>Sensor Name:</label>";
    html += "<input type='text' id='name' name='name' value='" + htmlEscape(sensor.name) + "' required>";

    // Sensor type
    html += "<label for='deviceType'>Device Type:</label>";
    html += "<select id='deviceType' name='deviceType'>";

    // Dynamically generate sensor types from definition
    for (const auto &type : SENSOR_TYPE_DEFINITIONS)
    {
        if (type.type != SensorType::UNKNOWN)
        {
            bool isSelected = (type.type == sensor.deviceType);
            html += "<option value='" + String(static_cast<uint8_t>(type.type)) + "'";
            if (isSelected)
            {
                html += " selected";
            }
            html += ">" + String(type.name);

            // Adding information about sensor capabilities
            html += " - ";
            bool first = true;

            if (type.hasTemperature)
            {
                html += "Temperature";
                first = false;
            }

            if (type.hasHumidity)
            {
                html += (first ? "" : ", ") + String("Humidity");
                first = false;
            }

            if (type.hasPressure)
            {
                html += (first ? "" : ", ") + String("Pressure");
                first = false;
            }

            if (type.hasPPM)
            {
                html += (first ? "" : ", ") + String("CO2");
                first = false;
            }

            if (type.hasLux)
            {
                html += (first ? "" : ", ") + String("Light");
            }

            if (type.hasRainAmount)
            {
                html += (first ? "" : ", ") + String("Rain");
            }
            if (type.hasWindSpeed)
            {
                html += (first ? "" : ", ") + String("Wind");
            }

            html += "</option>";
        }
    }

    html += "</select>";

    // Serial number
    html += "<label for='serialNumber'>Serial Number:</label>";
    html += "<input type='text' id='serialNumber' name='serialNumber' value='" + formatSN(sensor.serialNumber) + "' required>";

    // Device key
    html += "<label for='deviceKey'>Device Key:</label>";
    html += "<input type='text' id='deviceKey' name='deviceKey' placeholder='(unchanged)'>";

    // New code with a single field
    html += "<label for='customUrl'>Custom URL with Placeholders (Optional):</label>";
    html += "<div id='urlHelp' style='margin-bottom: 10px; font-size: 0.9em;'>";
    html += "<p>Available placeholders:</p>";

    html += "<style>";
    html += ".placeholder-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-bottom: 15px; }";
    html += ".placeholder-item { background: #f5f5f5; padding: 8px; border-radius: 4px; }";
    html += ".placeholder-item code { background: #e0e0e0; padding: 2px 4px; border-radius: 3px; font-family: monospace; color: #0066cc; }";
    html += "</style>";
    html += "<div class='placeholder-grid' style='display: grid; grid-template-columns: repeat(3, 1fr); gap: 5px;'>";
    // Environment placeholders
    html += "<div id='tempPlaceholder' class='placeholder-item'>Temperature <code>*TEMP*</code></div>";
    html += "<div id='humPlaceholder' class='placeholder-item'>Humidity <code>*HUM*</code></div>";
    html += "<div id='pressPlaceholder' class='placeholder-item'>Pressure <code>*PRESS*</code></div>";
    html += "<div id='ppmPlaceholder' class='placeholder-item'>CO2 <code>*PPM*</code></div>";
    html += "<div id='luxPlaceholder' class='placeholder-item'>Light <code>*LUX*</code></div>";
    // Weather specific
    html += "<div id='windSpeedPlaceholder' class='placeholder-item'>Wind Speed <code>*WIND_SPEED*</code></div>";
    html += "<div id='windDirPlaceholder' class='placeholder-item'>Wind Direction <code>*WIND_DIR*</code></div>";
    html += "<div id='rainPlaceholder' class='placeholder-item'>Rain Amount <code>*RAIN*</code></div>";
    html += "<div id='dailyRainPlaceholder' class='placeholder-item'>Rain since midnight <code>*DAILY_RAIN*</code></div>";
    html += "<div id='rainRatePlaceholder' class='placeholder-item'>Rain Rate <code>*RAIN_RATE*</code></div>";
    // Device info
    html += "<div class='placeholder-item'>Battery <code>*BAT*</code></div>";
    html += "<div class='placeholder-item'>Signal <code>*RSSI*</code></div>";
    html += "<div class='placeholder-item'>Serial Number <code>*SN*</code></div>";
    html += "<div class='placeholder-item'>Device Type <code>*TYPE*</code></div>";
    html += "</div>"; // end of grid
    html += "</div>"; // end of urlHelp
    html += "<input type='text' id='customUrl' name='customUrl' placeholder='https://example.com/api?temp=*TEMP*&hum=*HUM*' value='" + htmlEscape(sensor.customUrl) + "'>";

    html += "<h3 style='margin-top: 20px;'>Sensor Reading Corrections</h3>";
    html += "<p>Adjust sensor readings by adding offsets or applying multipliers. Leave at 0 for no correction.</p>";

    // Create a container for correction inputs with two columns
    html += "<div style='display: grid; grid-template-columns: 1fr 1fr; gap: 10px;'>";

    // Temperature correction
    html += "<div id='tempCorrDiv' style='display: " +
            String(sensor.hasTemperature() ? "block" : "none") + ";'>";
    html += "<label for='tempCorr'>Temperature Correction (±°C):</label>";
    html += "<input type='number' id='tempCorr' name='tempCorr' value='" +
            String(sensor.temperatureCorrection, 2) + "' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Humidity correction
    html += "<div id='humCorrDiv' style='display: " +
            String(sensor.hasHumidity() ? "block" : "none") + ";'>";
    html += "<label for='humCorr'>Humidity Correction (±%):</label>";
    html += "<input type='number' id='humCorr' name='humCorr' value='" +
            String(sensor.humidityCorrection, 2) + "' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Pressure correction
    html += "<div id='pressCorrDiv' style='display: " +
            String(sensor.hasPressure() ? "block" : "none") + ";'>";
    html += "<label for='pressCorr'>Pressure Correction (±hPa):</label>";
    html += "<input type='number' id='pressCorr' name='pressCorr' value='" +
            String(sensor.pressureCorrection, 2) + "' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // CO2 correction
    html += "<div id='ppmCorrDiv' style='display: " +
            String(sensor.hasPPM() ? "block" : "none") + ";'>";
    html += "<label for='ppmCorr'>CO2 Correction (±ppm):</label>";
    html += "<input type='number' id='ppmCorr' name='ppmCorr' value='" +
            String(sensor.ppmCorrection, 0) + "' step='1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Light correction
    html += "<div id='luxCorrDiv' style='display: " +
            String(sensor.hasLux() ? "block" : "none") + ";'>";
    html += "<label for='luxCorr'>Light Correction (±lux):</label>";
    html += "<input type='number' id='luxCorr' name='luxCorr' value='" +
            String(sensor.luxCorrection, 1) + "' step='0.1'>";
    html += "<small>Positive values increase the reading, negative values decrease it</small>";
    html += "</div>";

    // Wind speed correction
    html += "<div id='windSpeedCorrDiv' style='display: " +
            String(sensor.hasWindSpeed() ? "block" : "none") + ";'>";
    html += "<label for='windSpeedCorr'>Wind Speed Correction (multiplier):</label>";
    html += "<input type='number' id='windSpeedCorr' name='windSpeedCorr' value='" +
            String(sensor.windSpeedCorrection, 2) + "' step='0.01' min='0.1' max='10'>";
    html += "<small>Values greater than 1.0 increase the reading, less than 1.0 decrease it</small>";
    html += "</div>";

    // Wind direction correction
    html += "<div id='windDirCorrDiv' style='display: " +
            String(sensor.hasWindDirection() ? "block" : "none") + ";'>";
    html += "<label for='windDirCorr'>Wind Direction Correction (±degrees):</label>";
    html += "<input type='number' id='windDirCorr' name='windDirCorr' value='" +
            String(sensor.windDirectionCorrection) + "' step='1' min='-180' max='180'>";
    html += "<small>Positive values rotate clockwise, negative counter-clockwise</small>";
    html += "</div>";

    // Rain amount correction
    html += "<div id='rainAmountCorrDiv' style='display: " +
            String(sensor.hasRainAmount() ? "block" : "none") + ";'>";
    html += "<label for='rainAmountCorr'>Rain Amount Correction (multiplier):</label>";
    html += "<input type='number' id='rainAmountCorr' name='rainAmountCorr' value='" +
            String(sensor.rainAmountCorrection, 2) + "' step='0.01' min='0.1' max='10'>";
    html += "<small>Values greater than 1.0 increase the reading, less than 1.0 decrease it</small>";
    html += "</div>";

    // Rain rate correction
    html += "<div id='rainRateCorrDiv' style='display: " +
            String(sensor.hasRainRate() ? "block" : "none") + ";'>";
    html += "<label for='rainRateCorr'>Rain Rate Correction (multiplier):</label>";
    html += "<input type='number' id='rainRateCorr' name='rainRateCorr' value='" +
            String(sensor.rainRateCorrection, 2) + "' step='0.01' min='0.1' max='10'>";
    html += "<small>Values greater than 1.0 increase the reading, less than 1.0 decrease it</small>";
    html += "</div>";

    // Altitude - show for all pressure sensors
    html += "<div id='altitudeCorrDiv' style='display: " +
            String(sensor.hasPressure() ? "block" : "none") + ";'>";
    html += "<label for='altitude'>Altitude (m) - For pressure adjustment:</label>";
    html += "<input type='number' id='altitude' name='altitude' value='" +
            String(sensor.altitude) + "' min='0' max='8848'>";
    html += "<small>Used to convert relative pressure to absolute pressure</small>";
    html += "</div>";

    html += "</div>"; // End of grid for corrections

    // Buttons
    html += "<div style='margin-top: 20px;'>";
    html += "<input type='submit' value='Update Sensor'>";
    html += "<a href='/sensors' class='btn' style='background: #999;'>Cancel</a>";
    html += "</div>";

    html += "</form>"; // End of form - make sure this comes after all inputs
    html += "</div>";  // End of card

    // Enhanced JavaScript to dynamically update both sensor-specific fields and correction fields
    html += "<script>";
    html += "document.addEventListener('DOMContentLoaded', function() {";
    html += "  var deviceTypeSelect = document.getElementById('deviceType');";
    html += "  var altitudeDiv = document.getElementById('altitudeDiv');";

    // Create an update function that handles both regular fields and correction fields
    html += "  function updateFieldVisibility() {";
    html += "    var type = parseInt(deviceTypeSelect.value);";
    html += "    console.log('Selected device type:', type);";

    // Define which devices have which capabilities
    html += "    var tempDevices = [1, 2, 3, 5, 81];"; // BME280, SCD40, METEO, SHT40, DIY_TEMP
    html += "    var humDevices = [1, 2, 3, 5];";   // BME280, SCD40, METEO, SHT40
    html += "    var pressureDevices = [1, 3];";    // BME280, METEO
    html += "    var co2Devices = [2];";            // SCD40
    html += "    var luxDevices = [4];";            // VEML7700
    html += "    var weatherDevices = [3];";        // METEO

    // Update regular fields
    html += "    document.getElementById('altitudeCorrDiv').style.display = pressureDevices.includes(type) ? 'block' : 'none';";

    // Update correction fields
    html += "    document.getElementById('tempCorrDiv').style.display = tempDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('humCorrDiv').style.display = humDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('pressCorrDiv').style.display = pressureDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('ppmCorrDiv').style.display = co2Devices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('luxCorrDiv').style.display = luxDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windSpeedCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windDirCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainAmountCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainRateCorrDiv').style.display = weatherDevices.includes(type) ? 'block' : 'none';";

    // Update placeholders visibility (from existing code)
    html += "    updatePlaceholderVisibility();";
    html += "  }";

    // Keep your existing placeholder visibility updating code
    html += "  function updatePlaceholderVisibility() {";
    html += "    var type = parseInt(deviceTypeSelect.value);";
    html += "    console.log('Selected device type:', type);"; // Debugging

    // Device type definitions for individual placeholders
    html += "    var tempDevices = [1, 2, 3, 5, 81];"; // BME280, SCD40, METEO, SHT40, DIY_TEMP
    html += "    var humDevices = [1, 2, 3, 5];";   // BME280, SCD40, METEO, SHT40
    html += "    var pressureDevices = [1, 3];";    // BME280, METEO
    html += "    var co2Devices = [2];";            // SCD40
    html += "    var luxDevices = [4];";            // VEML7700
    html += "    var weatherDevices = [3];";        // METEO

    // Update display
    html += "    document.getElementById('tempPlaceholder').style.display = tempDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('humPlaceholder').style.display = humDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('pressPlaceholder').style.display = pressureDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('ppmPlaceholder').style.display = co2Devices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('luxPlaceholder').style.display = luxDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windSpeedPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('windDirPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('dailyRainPlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "    document.getElementById('rainRatePlaceholder').style.display = weatherDevices.includes(type) ? 'block' : 'none';";
    html += "  }";

    // Make sure the form is set to the current sensor type
    html += "  deviceTypeSelect.value = '" + String(static_cast<uint8_t>(sensor.deviceType)) + "';";

    // Run the update function initially and add event listener for changes
    html += "  updateFieldVisibility();";
    html += "  deviceTypeSelect.addEventListener('change', updateFieldVisibility);";
    html += "});";
    html += "</script>";

    // Adding footer
    addHtmlFooter(html);

    return html;
}

// Generating logs page
String HTMLGenerator::generateLogsPageHead(LogLevel currentLevel)
{
    String html;
    html.reserve(3072);

    // Adding header
    addHtmlHeader(html, "Logs");

    // Log control panel
    html += "<div class='card'>";
    html += "<h2>Log Settings</h2>";

    // Log level selector
    html += "<form method='post' action='/logs/level'>";
    html += "<label for='level'>Log Level:</label>";
    html += "<select id='level' name='level'>";
    html += "<option value='ERROR'" + String(currentLevel == LogLevel::ERROR ? " selected" : "") + ">ERROR</option>";
    html += "<option value='WARNING'" + String(currentLevel == LogLevel::WARNING ? " selected" : "") + ">WARNING</option>";
    html += "<option value='INFO'" + String(currentLevel == LogLevel::INFO ? " selected" : "") + ">INFO</option>";
    html += "<option value='DEBUG'" + String(currentLevel == LogLevel::DEBUG ? " selected" : "") + ">DEBUG</option>";
    html += "<option value='VERBOSE'" + String(currentLevel == LogLevel::VERBOSE ? " selected" : "") + ">VERBOSE</option>";
    html += "</select>";
    html += "<input type='submit' value='Set Level'>";
    html += "</form>";

    // Buttons for working with logs
    html += "<div style='margin-top: 20px;'>";
    html += "<a href='/logs' class='btn'>Refresh</a> ";
    html += "<a href='/logs/clear' class='btn btn-delete' onclick='return confirm(\"Are you sure you want to clear all logs?\")'>Clear Logs</a>";
    html += "</div>";
    html += "</div>";

    // Log content
    html += "<div class='card'>";
    html += "<h2>System Logs</h2>";
    html += "<div class='log-container'>";

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
    String html;
    html += "</div>"; // log-container
    html += "</div>"; // card

    // Auto-refresh for logs
    html += "<script>startAutoRefresh(30000);</script>";

    // Adding footer
    addHtmlFooter(html);

    return html;
}
// Generating JSON for API
DynamicJsonDocument HTMLGenerator::buildAPIJson(const std::vector<SensorData> &sensors)
{
    // Estimating JSON document size
    // Up to ~20 members per sensor (METEO) plus their copied strings
    size_t textSize = 0;
    for (const auto &sensor : sensors)
    {
        textSize += sensor.name.length() + 64;
    }
    const size_t capacity = JSON_OBJECT_SIZE(5) + JSON_ARRAY_SIZE(sensors.size()) +
                            sensors.size() * JSON_OBJECT_SIZE(24) + textSize + 512;

    DynamicJsonDocument doc(capacity);

    // Basic information
    doc["version"] = FIRMWARE_VERSION;

    // Current time
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0))
    {
        char timeStr[64];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        doc["time"] = timeStr;
    }
    else
    {
        doc["time"] = "Time not set";
    }

    doc["status"] = uplink->isOnline() ? "connected" : "disconnected";

    // Sensors array
    JsonArray sensorsArray = doc.createNestedArray("sensors");

    for (const auto &sensor : sensors)
    {
        if (sensor.configured)
        {
            JsonObject sensorObj = sensorsArray.createNestedObject();
            sensor.toJson(sensorObj);
        }
    }

    if (doc.overflowed())
    {
        doc.clear();
        doc["error"] = "response too large";
    }
    return doc;
}

// Generating API page
String HTMLGenerator::generateAPIPage(const std::vector<SensorData> &sensors)
{
    String html;
    html.reserve(4096);

    // Adding header
    addHtmlHeader(html, "API");

    // API documentation
    html += "<div class='card'>";
    html += "<h2>API Documentation</h2>";
    html += "<p>This gateway provides a JSON API for accessing sensor data.</p>";

    html += "<h3>Endpoints</h3>";
    html += "<table>";
    html += "<tr><th>URL</th><th>Description</th></tr>";
    html += "<tr><td><code>/api?format=json</code></td><td>Returns all sensor data in JSON format</td></tr>";
    html += "<tr><td><code>/api?format=csv</code></td><td>Returns sensor data in CSV format</td></tr>";
    html += "<tr><td><code>/api?sensor=XXXX</code></td><td>Returns data for a specific sensor by serial number</td></tr>";
    html += "</table>";

    html += "<h3>Example JSON Response</h3>";
    html += "<pre style='background: #f5f5f5; padding: 10px; overflow-x: auto;'>";
    html += "{\n";
    html += "  \"version\": \"" + String(FIRMWARE_VERSION) + "\",\n";
    html += "  \"time\": \"2026-10-17 12:34:56\",\n";
    html += "  \"status\": \"connected\",\n";
    html += "  \"sensors\": [\n";

    // Generating sensor data example
    if (!sensors.empty())
    {
        const auto &sensor = sensors[0];
        html += "    {\n";
        html += "      \"name\": \"" + htmlEscape(sensor.name) + "\",\n";
        html += "      \"deviceType\": " + String(static_cast<uint8_t>(sensor.deviceType)) + ",\n";
        html += "      \"typeName\": \"" + String(sensor.getTypeInfo().name) + "\",\n";
        html += "      \"serialNumber\": \"" + formatSN(sensor.serialNumber) + "\",\n";
        html += "      \"lastSeen\": " + (sensor.lastSeen > 0 ? String((millis() - sensor.lastSeen) / 1000) : "-1") + ",\n";

        if (sensor.hasTemperature())
        {
            html += "      \"temperature\": " + String(sensor.temperature, 2) + ",\n";
        }

        if (sensor.hasHumidity())
        {
            html += "      \"humidity\": " + String(sensor.humidity, 2) + ",\n";
        }

        if (sensor.hasPressure())
        {
            html += "      \"pressure\": " + String(sensor.pressure, 2) + ",\n";
        }

        if (sensor.hasPPM())
        {
            html += "      \"ppm\": " + String(sensor.ppm, 0) + ",\n";
        }

        if (sensor.hasLux())
        {
            html += "      \"lux\": " + String(sensor.lux, 1) + ",\n";
        }

        html += "      \"batteryVoltage\": " + String(sensor.batteryVoltage, 2) + ",\n";
        html += "      \"rssi\": " + String(sensor.rssi) + "\n";
        html += "    }\n";
    }
    else
    {
        html += "    {\n";
        html += "      \"name\": \"Example Sensor\",\n";
        html += "      \"deviceType\": 1,\n";
        html += "      \"typeName\": \"CLIMA\",\n";
        html += "      \"serialNumber\": \"123456\",\n";
        html += "      \"lastSeen\": 300,\n";
        html += "      \"temperature\": 21.50,\n";
        html += "      \"humidity\": 45.20,\n";
        html += "      \"pressure\": 1013.20,\n";
        html += "      \"batteryVoltage\": 3.82,\n";
        html += "      \"rssi\": -72\n";
        html += "    }\n";
    }

    html += "  ]\n";
    html += "}\n";
    html += "</pre>";

    html += "<h3>Live API</h3>";
    html += "<p>Access the live API here: <a href='/api?format=json' target='_blank'>/api?format=json</a></p>";
    html += "</div>";

    // Adding footer
    addHtmlFooter(html, true); // public page: no update button

    return html;
}
