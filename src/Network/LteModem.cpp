/**
 * expLORA Gateway
 *
 * LTE modem (PPP over UART) implementation file
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

#include "LteModem.h"

#if defined(HAS_LTE)

#include <PPP.h>
#include "../Hardware/WatchdogPause.h"

// UART used for the modem (UART0 is the console, UART1 is free but its default
// pins collide with the flash on WROOM modules)
static constexpr uint8_t LTE_UART = 2;
static constexpr int LTE_BAUD = 115200;

// SIM status after the modem answers AT (SIM init can take a few seconds)
static constexpr unsigned long LTE_SIM_TIMEOUT = 20000;
static constexpr unsigned long LTE_SIM_POLL = 1000;
// The modem powers up on its own once +5V_LTE is switched on (PWRKEY is
// held active by hardware); it answers AT after ~10-15 s
static constexpr unsigned long LTE_BOOT_TIMEOUT = 40000;
static constexpr unsigned long LTE_AT_PROBE_INTERVAL = 1000;
static constexpr unsigned long LTE_REGISTER_TIMEOUT = 180000;
static constexpr unsigned long LTE_REGISTER_POLL = 3000;
static constexpr unsigned long LTE_CONNECT_TIMEOUT = 60000;
static constexpr unsigned long LTE_SIGNAL_POLL = 30000;
// Lost PPP session this long while online counts as a failure
static constexpr unsigned long LTE_LOST_IP_GRACE = 30000;
// AT+CPOF needs a few seconds before the supply may be cut
static constexpr unsigned long LTE_POWER_DOWN_DELAY = 3000;
// Retry back-off after a failure: 30 s, 60 s, ... up to 10 min
static constexpr unsigned long LTE_RETRY_BASE = 30000;
static constexpr unsigned long LTE_RETRY_MAX = 600000;
static constexpr uint8_t LTE_MAX_FAILURES = 20;

LteModem::LteModem(ConfigManager &cfg, Logger &log)
    : config(cfg), logger(log), textLock(xSemaphoreCreateMutex())
{
}

String LteModem::readText(const String &field) const
{
    xSemaphoreTake(textLock, portMAX_DELAY);
    String copy = field;
    xSemaphoreGive(textLock);
    return copy;
}

void LteModem::writeText(String &field, const String &value)
{
    xSemaphoreTake(textLock, portMAX_DELAY);
    field = value;
    xSemaphoreGive(textLock);
}

void LteModem::begin()
{
    // Supply off until enabled; UART pins high-impedance so the unpowered
    // modem is not back-fed through the level shifter
    pinMode(LTE_PWR_PIN, OUTPUT);
    digitalWrite(LTE_PWR_PIN, LOW);
    pinMode(LTE_TX_PIN, INPUT);
    pinMode(LTE_RX_PIN, INPUT);
    pinMode(LTE_NET_PIN, INPUT);

    // Own copies: the web task may rewrite the configuration (a change of the
    // LTE settings restarts the gateway, so they never need refreshing)
    {
        std::lock_guard<std::recursive_mutex> guard(config.mutex());
        apn = config.lteApn;
        pin = config.ltePin;
    }

    if (!config.lteEnabled)
    {
        logger.info("LTE disabled in configuration");
        setState(State::Disabled);
        return;
    }
    if (config.ltePinRejected && pin.length() > 0)
    {
        writeText(lastError, "The SIM rejected the stored PIN - enter the correct one");
        logger.error("LTE: the SIM rejected the stored PIN before - not trying it again until it is changed");
        setState(State::Disabled);
        return;
    }
    setState(State::Off);
}

void LteModem::process()
{
    unsigned long now = millis();
    unsigned long inState = now - stateSince;

    switch (state)
    {
    case State::Disabled:
        break;

    case State::Off:
        powerOn();
        setState(State::Booting);
        break;

    case State::Booting:
        if (probeAt(now))
        {
            logger.info("LTE modem responding after " + String(inState / 1000) + " s");
            pinSent = false;
            pinAccepted = false;
            // Verbose errors: tells a wrong PIN from a busy SIM
            Serial2.print("AT+CMEE=2\r");
            Serial2.flush();
            delay(100);
            while (Serial2.available())
            {
                Serial2.read();
            }
            rxBuffer = "";
            setState(State::SimCheck);
        }
        else if (inState > LTE_BOOT_TIMEOUT)
        {
            fail("Modem does not respond to AT");
        }
        break;

    case State::SimCheck:
        if (inState > LTE_SIM_TIMEOUT)
        {
            fail("SIM not ready (inserted?)");
        }
        else
        {
            checkSim(now);
        }
        break;

    case State::Starting:
        // startPpp() moves on or fails synchronously
        break;

    case State::Registering:
        if (now - lastPoll < LTE_REGISTER_POLL)
        {
            break;
        }
        lastPoll = now;
        if (PPP.attached())
        {
            WatchdogPause pause;
            refreshSignal();
            readOperator();
            if (apn.length() == 0)
            {
                resolveApn();
            }
            logger.info("LTE registered: " + operatorName + ", CSQ " + String(csq) +
                        " (" + String(getRssiDbm()) + " dBm), switching to data mode");
            setState(State::Connecting);
            if (!PPP.mode(ESP_MODEM_MODE_CMUX))
            {
                fail("CMUX mode failed");
            }
        }
        else if (inState > LTE_REGISTER_TIMEOUT)
        {
            refreshSignal();
            fail("Network registration timeout (CSQ " + String(csq) + ")");
        }
        break;

    case State::Connecting:
        if (PPP.hasIP())
        {
            logger.info("LTE connected, IP: " + PPP.localIP().toString());
            failures = 0;
            writeText(lastError, "");
            setState(State::Online);
            lastPoll = now;
        }
        else if (inState > LTE_CONNECT_TIMEOUT)
        {
            fail("PPP connection timeout");
        }
        break;

    case State::Online:
        if (!PPP.hasIP())
        {
            // stateSince marks when the session was lost
            if (lastPoll != 0)
            {
                logger.warning("LTE connection lost");
                lastPoll = 0;
                stateSince = now;
            }
            else if (inState > LTE_LOST_IP_GRACE)
            {
                fail("PPP connection lost");
            }
            break;
        }
        if (lastPoll == 0)
        {
            logger.info("LTE connection restored, IP: " + PPP.localIP().toString());
            lastPoll = now;
        }
        if (now - lastPoll > LTE_SIGNAL_POLL)
        {
            lastPoll = now;
            refreshSignal();
        }
        break;

    case State::Suspended:
        break;

    case State::PowerCycle:
    {
        unsigned long backoff = min(LTE_RETRY_BASE * failures, LTE_RETRY_MAX);
        if (inState > backoff)
        {
            logger.info("LTE retrying (attempt " + String(failures + 1) + ")");
            setState(State::Off);
        }
        break;
    }
    }
}

bool LteModem::suspend()
{
    if (state == State::Disabled || state == State::Suspended)
    {
        return false;
    }
    // A modem waiting to retry is already off (fail() powered it down)
    if (state != State::PowerCycle)
    {
        logger.info("LTE: powering the modem off for the update");
        stopPpp();
        powerOff();
    }
    setState(State::Suspended);
    return true;
}

void LteModem::resume()
{
    if (state != State::Suspended)
    {
        return;
    }
    logger.info("LTE: powering the modem on again after the update");
    setState(State::Off);
}

void LteModem::setState(State next)
{
    state = next;
    stateSince = millis();
    lastPoll = 0;
}

void LteModem::fail(const String &reason)
{
    if (failures < LTE_MAX_FAILURES)
    {
        failures++;
    }
    writeText(lastError, reason);
    logger.warning("LTE: " + reason + ", powering off");
    stopPpp();
    powerOff();
    setState(State::PowerCycle);
}

// Not recoverable by retrying - stay off until the configuration changes
void LteModem::disable(const String &reason)
{
    writeText(lastError, reason);
    logger.error("LTE: " + reason + " - LTE stays off until the settings change");
    stopPpp();
    powerOff();
    setState(State::Disabled);
}

// Raw AT: query the SIM, enter the PIN once if needed
void LteModem::checkSim(unsigned long now)
{
    while (Serial2.available())
    {
        rxBuffer += (char)Serial2.read();
        if (rxBuffer.length() > 256)
        {
            rxBuffer.remove(0, 128);
        }
    }

    if (rxBuffer.indexOf("+CPIN: READY") >= 0)
    {
        startPpp();
        return;
    }
    if (rxBuffer.indexOf("SIM PUK") >= 0)
    {
        disable("SIM is locked (PUK required)");
        return;
    }
    if (rxBuffer.indexOf("SIM PIN") >= 0 && !pinSent)
    {
        if (pin.length() == 0)
        {
            disable("SIM requires a PIN");
            return;
        }
        logger.info("LTE entering SIM PIN");
        // Let the rest of the AT+CPIN? reply (its "OK") arrive and drop it,
        // so it is not taken for the answer to the PIN
        delay(300);
        while (Serial2.available())
        {
            Serial2.read();
        }
        rxBuffer = "";
        Serial2.print("AT+CPIN=\"" + pin + "\"\r");
        pinSent = true;
        lastPoll = now;
        return;
    }
    // Direct answer to AT+CPIN=<pin>
    if (pinSent && !pinAccepted)
    {
        int error = rxBuffer.indexOf("ERROR");
        if (error >= 0)
        {
            String detail = rxBuffer.substring(error);
            detail.toLowerCase();
            if (detail.indexOf("busy") >= 0 || detail.indexOf("not inserted") >= 0 ||
                detail.indexOf("failure") >= 0)
            {
                // Transient - the PIN was not checked, try again later
                fail("SIM not ready for the PIN (" + rxBuffer.substring(error, error + 40) + ")");
                return;
            }
            // "incorrect password", or a bare ERROR: assume the PIN is wrong
            // rather than risk the remaining attempts
            config.setLtePinRejected(true);
            disable("SIM PIN rejected (check it - attempts are limited)");
            return;
        }
        if (rxBuffer.indexOf("OK") >= 0)
        {
            pinAccepted = true;
            rxBuffer = "";
            lastPoll = now;
        }
        return;
    }

    // (Re)query; after the PIN give the SIM time to unlock first
    if (now - lastPoll > (pinSent ? 3 * LTE_SIM_POLL : LTE_SIM_POLL))
    {
        lastPoll = now;
        rxBuffer = "";
        Serial2.print("AT+CPIN?\r");
    }
}

void LteModem::powerOn()
{
    pinMode(LTE_RX_PIN, INPUT_PULLUP);
    Serial2.begin(LTE_BAUD, SERIAL_8N1, LTE_RX_PIN, LTE_TX_PIN);
    atUartOpen = true;
    digitalWrite(LTE_PWR_PIN, HIGH);
    rxBuffer = "";
    logger.info("LTE modem powered on");
}

void LteModem::powerOff()
{
    if (atUartOpen)
    {
        // Still in plain AT mode (boot failed) - power down cleanly
        Serial2.print("AT+CPOF\r");
        Serial2.flush();
        delay(LTE_POWER_DOWN_DELAY);
        Serial2.end();
        atUartOpen = false;
    }
    digitalWrite(LTE_PWR_PIN, LOW);
    pinMode(LTE_TX_PIN, INPUT);
    pinMode(LTE_RX_PIN, INPUT);
    csq = 99;
    writeText(operatorName, "");
}

// Send AT once per interval on the raw UART, true when OK came back
bool LteModem::probeAt(unsigned long now)
{
    while (Serial2.available())
    {
        rxBuffer += (char)Serial2.read();
        if (rxBuffer.length() > 128)
        {
            rxBuffer.remove(0, 64);
        }
    }
    if (rxBuffer.indexOf("OK") >= 0)
    {
        return true;
    }
    if (now - lastPoll > LTE_AT_PROBE_INTERVAL)
    {
        lastPoll = now;
        rxBuffer = "";
        Serial2.print("AT\r");
    }
    return false;
}

// Hand the UART to the PPP driver: module info, APN
void LteModem::startPpp()
{
    setState(State::Starting);
    WatchdogPause pause;

    // Echo off for the driver's response parsing
    Serial2.print("ATE0\r");
    Serial2.flush();
    delay(200);
    Serial2.end();
    atUartOpen = false;

    // Empty APN: attach with the network's default, see resolveApn()
    PPP.setApn(apn.c_str());
    // The SIM is already unlocked (checkSim)
    PPP.setPin(nullptr);
    PPP.setPins(LTE_TX_PIN, LTE_RX_PIN);
    if (!PPP.begin(PPP_MODEM_SIM7600, LTE_UART, LTE_BAUD))
    {
        // begin() cleans up after itself; reopen the raw UART so powerOff()
        // can still shut the modem down with AT+CPOF
        Serial2.begin(LTE_BAUD, SERIAL_8N1, LTE_RX_PIN, LTE_TX_PIN);
        atUartOpen = true;
        fail("PPP driver start failed");
        return;
    }
    pppStarted = true;

    // LTE attach already uses PDP context 1, so it must carry the configured
    // APN before registration (the driver sets it only when entering data mode);
    // an empty one also clears an APN left in the modem from earlier use
    String response;
    if (!PPP.cmd("AT+CGDCONT=1,\"IP\",\"" + apn + "\"", response, 3000))
    {
        logger.warning("LTE: setting APN failed: " + response);
    }

    writeText(moduleName, PPP.moduleName());
    writeText(imei, PPP.IMEI());
    logger.info("LTE modem " + moduleName + ", IMEI " + imei);
    setState(State::Registering);
}

void LteModem::stopPpp()
{
    if (!pppStarted)
    {
        return;
    }
    // Modem commands and driver teardown can each take seconds (command
    // timeouts, CMUX close)
    WatchdogPause pause;

    // AT+CPOF; works in command and CMUX mode
    if (PPP.mode() == ESP_MODEM_MODE_DATA || !PPP.powerDown())
    {
        logger.debug("LTE power down command failed");
    }
    delay(LTE_POWER_DOWN_DELAY);
    PPP.end();
    pppStarted = false;
}

// Some networks report only the numeric MCC+MNC
static const char *operatorFromCode(const String &code)
{
    static const struct
    {
        const char *code;
        const char *name;
    } known[] = {
        {"23001", "T-Mobile CZ"},
        {"23002", "O2 CZ"},
        {"23003", "Vodafone CZ"},
        {"23101", "Orange SK"},
        {"23102", "Telekom SK"},
        {"23103", "4ka"},
        {"23106", "O2 SK"},
    };
    for (const auto &k : known)
    {
        if (code == k.code)
        {
            return k.name;
        }
    }
    return nullptr;
}

void LteModem::readOperator()
{
    // Long alphanumeric format for +COPS? (the modem may default to numeric)
    String response;
    PPP.cmd("AT+COPS=3,0", response, 3000);
    String name = PPP.operatorName();
    name.trim();

    bool numeric = name.length() > 0;
    for (size_t i = 0; i < name.length(); i++)
    {
        numeric = numeric && isDigit(name[i]);
    }
    if (numeric && operatorFromCode(name))
    {
        name = operatorFromCode(name);
    }
    writeText(operatorName, name);
}

// Automatic APN: read the one the network assigned to the default bearer
// and use it for the data session
void LteModem::resolveApn()
{
    String response;
    String apn;
    if (PPP.cmd("AT+CGCONTRDP=1", response, 3000))
    {
        // +CGCONTRDP: <cid>,<bearer>,"<apn>",...
        int start = response.indexOf('"');
        int end = start >= 0 ? response.indexOf('"', start + 1) : -1;
        if (end > start)
        {
            apn = response.substring(start + 1, end);
            // Drop the operator suffix (".mnc001.mcc230.gprs")
            String lower = apn;
            lower.toLowerCase();
            int suffix = lower.indexOf(".mnc");
            if (suffix > 0 && lower.indexOf(".mcc", suffix) > 0)
            {
                apn = apn.substring(0, suffix);
            }
        }
    }
    if (apn.length() > 0 && esp_modem_set_apn(PPP.handle(), apn.c_str()) == ESP_OK)
    {
        logger.info("LTE APN from network: " + apn);
    }
    else
    {
        apn = "";
        logger.info("LTE APN not reported by the network, using its default");
    }
    writeText(detectedApn, apn);
}

String LteModem::getApn() const
{
    if (apn.length() > 0)
    {
        return apn;
    }
    String detected = readText(detectedApn);
    return detected.length() > 0 ? detected + " (automatic)" : String("automatic");
}

void LteModem::refreshSignal()
{
    int q = PPP.RSSI();
    csq = q < 0 ? 99 : q;
}

bool LteModem::hasIP() const
{
    return state == State::Online && PPP.hasIP();
}

IPAddress LteModem::localIP() const
{
    return hasIP() ? PPP.localIP() : IPAddress();
}

// 3GPP 27.007: 0 = -113 dBm ... 31 = -51 dBm, 99 = unknown
int LteModem::getRssiDbm() const
{
    return (csq >= 0 && csq <= 31) ? -113 + 2 * csq : 0;
}

const char *LteModem::getStateName() const
{
    switch (state)
    {
    case State::Disabled:
        return "disabled";
    case State::Off:
        return "off";
    case State::Booting:
        return "booting";
    case State::SimCheck:
        return "checking SIM";
    case State::Starting:
        return "starting";
    case State::Registering:
        return "registering";
    case State::Connecting:
        return "connecting";
    case State::Online:
        return "online";
    case State::PowerCycle:
        return "waiting to retry";
    case State::Suspended:
        return "off for an update";
    }
    return "unknown";
}

#endif
