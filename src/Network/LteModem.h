/**
 * expLORA Gateway
 *
 * LTE modem (PPP over UART) header file
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
#include "../config.h"
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"

#if defined(HAS_LTE)

/**
 * LTE modem (SIMCom A7682E) driven through the core's PPP interface
 *
 * Non-blocking state machine, call process() from loop():
 *   Off -> Booting (power on, wait for AT) -> SimCheck (SIM / PIN)
 *   -> Starting (PPP driver) -> Registering (wait for network)
 *   -> Connecting (CMUX, PPP) -> Online
 * Failures end in PowerCycle (AT+CPOF, power off, retry later). SIM problems
 * that a retry cannot fix (PIN rejected or missing, PUK lock) end in
 * Disabled until the configuration changes, so a wrong PIN is tried once.
 *
 * The PPP netif keeps its default route priority (20), below ethernet and
 * WiFi, so the session is a hot standby used only when nothing else is up.
 */
class LteModem
{
public:
    enum class State : uint8_t
    {
        Disabled,
        Off,
        Booting,
        SimCheck,
        Starting,
        Registering,
        Connecting,
        Online,
        PowerCycle,
        Suspended, // update mode: modem off, back to Off (a full attach) on resume()
    };

    LteModem(ConfigManager &config, Logger &log);

    void begin();
    void process();

    // Update mode (loop task): power the modem down and keep it off (frees
    // the PPP / esp_modem memory) until resume(), which attaches again from
    // scratch (~20-60 s). suspend() blocks for the modem's power-down
    // (~3-6 s). false: nothing to suspend (disabled or already suspended)
    bool suspend();
    void resume();
    bool suspended() const { return state == State::Suspended; }

    State getState() const { return state; }
    const char *getStateName() const;
    bool hasIP() const;
    IPAddress localIP() const;

    // Cached diagnostics, refreshed while registered
    int getSignalQuality() const { return csq; } // 0..31, 99 = unknown
    int getRssiDbm() const;                      // 0 = unknown
    // Copies: the strings are rewritten by the loop task while web handlers
    // read them from the async server task
    String getOperator() const { return readText(operatorName); }
    String getModuleName() const { return readText(moduleName); }
    String getImei() const { return readText(imei); }
    String getLastError() const { return readText(lastError); }
    String getApn() const; // configured, or the one detected from the network

private:
    ConfigManager &config;
    Logger &logger;

    State state = State::Disabled;
    unsigned long stateSince = 0;
    unsigned long lastPoll = 0;
    uint8_t failures = 0;
    bool pppStarted = false;
    bool pinSent = false;
    bool pinAccepted = false; // the modem answered OK to AT+CPIN=<pin>
    bool atUartOpen = false; // raw AT access before the PPP driver owns the UART
    String rxBuffer;

    int csq = 99;
    String operatorName;
    String moduleName;
    String imei;
    String lastError;
    String detectedApn;
    String apn; // configured APN (copy taken at begin())
    String pin; // configured SIM PIN (copy taken at begin())
    SemaphoreHandle_t textLock;

    String readText(const String &field) const;
    void writeText(String &field, const String &value);

    void setState(State next);
    void fail(const String &reason);
    void disable(const String &reason);
    void checkSim(unsigned long now);
    void powerOn();
    void powerOff();
    bool probeAt(unsigned long now);
    void startPpp();
    void stopPpp();
    void refreshSignal();
    void readOperator();
    void resolveApn();
};

#endif
