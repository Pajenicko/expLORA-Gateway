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
    if (WiFi.STA.connected())
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
void HTMLGenerator::addWiFiForm(String &html, const String &ssid, const String &password, const String &timezone)
{
    html += "<div class='card'>";
    html += "<h2>WiFi Settings</h2>";
    html += "<form method='post' action='/config'>";
    html += "<label for='ssid'>SSID:</label>";
    html += "<input type='text' id='ssid' name='ssid' value='" + htmlEscape(ssid) + "' required>";
    // Stored secrets never go into the page; empty = keep the stored one
    html += "<label for='password'>Password:</label>";
    html += "<input type='password' id='password' name='password' autocomplete='new-password' placeholder='" +
            String(password.length() > 0 ? "(unchanged)" : "(open network)") + "'>";
    html += "<label for='timezone'>Timezone:</label>";
    html += "<select id='timezone' name='timezone'>";

    static const char *const zones[][2] = TIMEZONE_OPTIONS;
    for (const auto &zone : zones)
    {
        html += "<option value='" + String(zone[0]) + "'" + String(timezone == zone[0] ? " selected" : "") + ">" +
                zone[1] + "</option>";
    }
    html += "</select>";

    html += "<input type='submit' value='Save and Restart'>";
    html += "</form>";
    html += "</div>";
}

String HTMLGenerator::generateConfigPage(const String &ssid, const String &password, bool configMode, const String &ip, const String &timezone)
{
    String html;
    html.reserve(4096);

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
        html += "<p><strong>Status:</strong> " + String(WiFi.STA.connected() ? "Connected" : "Disconnected") + "</p>";
        if (WiFi.STA.hasIP())
        {
            html += "<p><strong>IP:</strong> " + WiFi.STA.localIP().toString() + "</p>";
        }
    }
    html += "</div>";

    addWiFiForm(html, ssid, password, timezone);

    // Add footer
    addHtmlFooter(html);

    return html;
}
// Generate MQTT Configuration Page
String HTMLGenerator::generateMqttPage(const String &host, int port, const String &user, const String &password, bool enabled, bool tls,
                                       const String &prefix, bool haEnabled, String &haPrefix)
{
    String html;
    html.reserve(4096);

    // Add header
    addHtmlHeader(html, "MQTT Configuration");

    // MQTT configuration form
    html += "<div class='card'>";
    html += "<h2>MQTT Settings</h2>";
    html += "<p>Configure connection to Home Assistant MQTT broker for automatic sensor discovery.</p>";
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

String HTMLGenerator::generateNetworkPage(const ConfigManager &config, const String &ssid, const String &password,
                                         const String &timezone, bool apMode)
{
    String html;
    html.reserve(6144);

    addHtmlHeader(html, "Network", apMode);

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
        html += uplink->ethernetLinkUp() ? (config.ethStatic ? "Link up" : "Link up, waiting for DHCP") : "Disconnected";
    }
    html += String(config.ethStatic ? " (static)" : " (DHCP)") + "</p>";
#endif
    html += "<p><strong>WiFi:</strong> ";
    if (ssid.length() == 0)
    {
        html += "Not configured";
    }
    else
    {
        html += (WiFi.STA.hasIP() ? "Connected, " + WiFi.STA.localIP().toString() : String("Disconnected")) +
                " (" + htmlEscape(ssid) + ")";
    }
    html += "</p>";
#if defined(HAS_LTE)
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
#endif
    html += "</div>";

    addWiFiForm(html, ssid, password, timezone);

#if defined(HAS_ETHERNET)
    {
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
        html += "<option value='dhcp'" + String(config.ethStatic ? "" : " selected") + ">DHCP (automatic)</option>";
        html += "<option value='static'" + String(config.ethStatic ? " selected" : "") + ">Static</option>";
        html += "</select></div>";
        field("ip", "IP address", config.ethIp, true);
        field("mask", "Subnet mask", config.ethMask.length() ? config.ethMask : String("255.255.255.0"), true);
        field("gateway", "Gateway", config.ethGateway, true);
        field("dns1", "Primary DNS (empty = gateway)", config.ethDns1, false);
        field("dns2", "Secondary DNS (optional)", config.ethDns2, false);
        html += "<input type='submit' value='Save and Restart'>";
        html += "</form></div>";
        html += "<script>(function(){var m=document.getElementById('ethMode');"
                "function u(){var s=m.value==='static';"
                "document.querySelectorAll('.eth-static').forEach(function(g){g.style.display=s?'':'none';"
                "var i=g.querySelector('input');i.required=s&&i.dataset.required==='1';i.disabled=!s;});}"
                "m.addEventListener('change',u);u();})();</script>";
    }
#endif

#if defined(HAS_LTE)
    html += "<div class='card'>";
    html += "<h2>LTE Settings</h2>";
    html += "<p>The LTE connection is kept ready and used only when neither Ethernet nor WiFi is available. Changes take effect after a restart.</p>";
    html += "<p>Leave LTE disabled when no modem is fitted.</p>";
    html += "<form method='post' action='/network'>";
    html += "<input type='hidden' name='section' value='lte'>";
    html += "<div class='form-group'>";
    html += "<label for='lteEnabled'>Enable LTE:</label>";
    html += "<input type='checkbox' id='lteEnabled' name='lteEnabled' value='1'" + String(config.lteEnabled ? " checked" : "") + ">";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='apn'>APN (empty = automatic):</label>";
    html += "<input type='text' id='apn' name='apn' maxlength='63' value='" + htmlEscape(config.lteApn) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='pin'>SIM PIN (optional):</label>";
    html += "<input type='password' id='pin' name='pin' maxlength='8' inputmode='numeric' pattern='[0-9]{4,8}' autocomplete='off' placeholder='" +
            String(config.ltePin.length() > 0 ? "(unchanged)" : "") + "'>";
    if (config.ltePin.length() > 0)
    {
        html += "<label><input type='checkbox' name='clearPin' value='1'> Remove the stored PIN</label>";
    }
    html += "</div>";
    html += "<input type='submit' value='Save and Restart'>";
    html += "</form></div>";
#endif

    addHtmlFooter(html);
    return html;
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

String HTMLGenerator::generateSensorsPage(const std::vector<ActiveSensorEntry> &entries,
                                          const std::vector<Gen2Sensor> &gen2, const WaitingEntry *waiting,
                                          size_t waitingCount)
{
    String html;
    // One allocation for the whole page (no PSRAM on the Pro): sized by the rows
    html.reserve(6144 + 700 * entries.size() + 600 * gen2.size() + 300 * waitingCount);
    addHtmlHeader(html, "Sensors");

    html += "<div class='card'><h2>Configured Sensors</h2>";

    if (entries.empty())
    {
        html += "<p>No sensors configured yet.</p>";
    }
    else
    {
        // Snapshot 'now' once per page render so every row computes deltas
        // against the same reference. millis() can wrap every ~50 days but
        // unsigned subtraction in formatRelativeMillis handles that correctly.
        const unsigned long nowMillis = millis();

        html += "<table><tr>"
                "<th>Name</th><th>Type</th><th>Serial</th>"
                "<th>Status</th><th>Last Seen</th><th>24h</th><th>Actions</th></tr>";

        for (const auto &entry : entries)
        {
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
            html += renderHealthBadge(h, nowMillis);
            if (h.hasLastReject && h.consecutiveRejections > 0)
            {
                html += "<br><small style='color:#c0392b;'>";
                html += "last reject: ";
                html += htmlEscape(String(h.lastRejectReason));
                html += " (";
                html += formatRelativeMillis(nowMillis, h.lastRejectAtMillis, true);
                html += ")</small>";
            }
            html += "</td>";

            // Last seen: prefer relative form derived from our millis snapshot;
            // the wall-clock form (getLastSeenString) is kept as a tooltip so
            // users can still see the exact timestamp if NTP is synced.
            html += "<td title='" + s.getLastSeenString() + "'>";
            html += formatRelativeMillis(nowMillis, h.lastOkAtMillis, h.hasLastOk);
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
        html += "</table>";
    }

    html += "<p><a class='btn' href='/sensors/add'>Add New Sensor</a></p>";
    html += "</div>";

    // Generation 2 sensors (protocol v2): identity and what they reported
    const unsigned long now = millis();
    html += "<div class='card'><h2>Generation 2 Sensors</h2>";
    if (gen2.empty())
    {
        html += "<p>No generation 2 sensors added yet.</p>";
    }
    else
    {
        html += "<table><tr><th>Name</th><th>DevID</th><th>Key</th><th>Reported</th>"
                "<th>Last JOIN_REQ</th><th>Counters</th><th>Actions</th></tr>";
        for (const Gen2Sensor &g : gen2)
        {
            // The label key is the root secret: only its first 4 hex digits,
            // enough to tell which label was entered
            char id[9], key[16];
            snprintf(id, sizeof(id), "%08lX", (unsigned long)g.devId);
            snprintf(key, sizeof(key), "%02X%02X&hellip;", g.label[0], g.label[1]);
            html += "<tr><td>" + htmlEscape(g.name) + "</td><td><code>" + String(id) + "</code></td>";
            html += "<td><code>" + String(key) + "</code></td><td>";
            if (g.seen)
            {
                html += "HW " + String(g.hwType) + ", FW " + String(g.fwMajor) + "." + String(g.fwMinor) + "." +
                        String(g.fwPatch) + "<br><small>" + String(g.rssi) + " dBm, SNR " + String(g.snr, 1) +
                        " dB, " + String(g.freqError / 1000.0f, 1) + " kHz</small>";
            }
            else
            {
                html += "&mdash;";
            }
            html += "</td><td>" + formatRelativeMillis(now, g.lastSeenMs, g.seen);
            if (g.seen)
            {
                html += "<br><small>devNonce " + String(g.lastJoinDevNonce) + "</small>";
            }
            html += "</td><td><small>JOIN_REQ " + String(g.joinRequests) + "<br>MIC failed " +
                    String(g.micFailures);
            if (g.micFailures > 0 && g.joinRequests == 0)
            {
                html += " <span style='color:#c0392b;'>(wrong key, or paired with another gateway?)</span>";
            }
            html += "<br>replayed " + String(g.replayedJoins) + "<br>UP w/o session " + String(g.upWithoutSession) +
                    "</small></td>";
            html += "<td><a class='btn btn-delete' href='/sensors/delete2?devId=" + String(id) +
                    "' onclick='return confirm(\"Delete this generation 2 sensor?\")'>Delete</a></td></tr>";
        }
        html += "</table>";
    }
    html += "<p><a class='btn' href='/sensors/add2'>Add Generation 2 Sensor</a></p>";

    html += "<h3>Waiting to be added</h3>";
    if (waitingCount == 0)
    {
        html += "<p><small>No unknown generation 2 sensors heard (a sensor appears here after two "
                "consistent join requests).</small></p>";
    }
    else
    {
        html += "<table><tr><th>DevID</th><th>HW / FW</th><th>RSSI</th><th>Requests</th><th>Last heard</th>"
                "<th></th></tr>";
        for (size_t i = 0; i < waitingCount; i++)
        {
            const WaitingEntry &w = waiting[i];
            char id[9];
            snprintf(id, sizeof(id), "%08lX", (unsigned long)w.devId);
            html += "<tr><td><code>" + String(id) + "</code></td><td>HW " + String(w.hwType) + ", FW " +
                    String(w.fwMajor) + "." + String(w.fwMinor) + "." + String(w.fwPatch) + "</td><td>" +
                    String(w.rssi) + " dBm</td><td>" + String(w.frames) + "</td><td>" +
                    formatRelativeMillis(now, w.lastSeenMs, true) + "</td>";
            html += "<td><a class='btn' href='/sensors/add2?devId=" + String(id) + "'>Add</a></td></tr>";
        }
        html += "</table><p><small>Join requests are not authenticated until the label key is entered: "
                "compare the DevID with the label.</small></p>";
    }
    html += "</div>";
    addHtmlFooter(html);
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
