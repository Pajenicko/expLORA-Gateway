/**
 * expLORA Gateway
 *
 * Web interface login header file
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
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
#include <ESPAsyncWebServer.h>
#include "../Data/Logging.h"
#include "../Storage/ConfigManager.h"

/**
 * Admin password and login sessions of the web interface
 *
 * The password is stored as PBKDF2-HMAC-SHA256 with a random salt. Without a
 * stored password (fresh flash, or an update from a version without login)
 * an empty password logs in and the user must set one before anything else.
 *
 * Sessions are random tokens in an HttpOnly, SameSite=Strict cookie, kept in
 * RAM (a restart logs everyone out). Repeated wrong passwords lock the
 * login for a while - per client address, so others are not locked out.
 */
class WebAuth
{
public:
    static constexpr size_t MIN_PASSWORD_LENGTH = 8;
    static constexpr size_t MAX_PASSWORD_LENGTH = 64;

    WebAuth(ConfigManager &config, Logger &log);

    bool hasPassword() const;

    // Login: checks the password (empty when none is set), creates a session
    // and returns its token; empty token on failure, retryAfterS set when
    // the login is locked
    String login(AsyncWebServerRequest *request, const String &password, unsigned long &retryAfterS);

    // Session cookie of the request is valid (refreshes its idle timer)
    bool isAuthorized(AsyncWebServerRequest *request);

    void logout(AsyncWebServerRequest *request);

    // Tag of the request's login session (64-bit hash of its token), 0 when
    // the request has no valid session. Binds a firmware confirmation to the
    // session that installed the image; a new login gets another tag
    uint64_t sessionTag(AsyncWebServerRequest *request);

    // Set a new password; ends all sessions. Returns an error, empty on success
    String changePassword(AsyncWebServerRequest *request, const String &current, const String &password,
                          const String &confirm);

    // New session for the user who just changed the password (bypasses the
    // login throttle, which another client may have armed)
    String sessionAfterPasswordChange() { return createSession(); }

    // Recovery over the serial console (physical access)
    void resetPassword();

    // "Set-Cookie" value for a session token (empty token clears the cookie)
    static String cookieFor(const String &token);

private:
    struct Session
    {
        char token[33];
        uint64_t lastUsed; // ms since boot (64-bit: no wrap)
    };

    // Failed password attempts per client address
    struct Attempts
    {
        uint32_t ip;
        uint8_t failures;
        uint64_t lockedUntil;
        uint64_t lastFailureAt; // failures expire LOGIN_LOCK_TIME after the last one
    };

    static constexpr size_t MAX_SESSIONS = 4;
    static constexpr size_t MAX_TRACKED_CLIENTS = 8;
    static constexpr uint64_t SESSION_IDLE_TIMEOUT = 24ULL * 3600 * 1000;
    static constexpr uint8_t MAX_FAILED_LOGINS = 5;
    static constexpr uint64_t LOGIN_LOCK_TIME = 60000;

    ConfigManager &config;
    Logger &logger;

    Session sessions[MAX_SESSIONS] = {};
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    Attempts attempts[MAX_TRACKED_CLIENTS] = {};
    // All clients together: past GLOBAL_FAILURE_LIMIT wrong passwords within a
    // minute (an attack from changing addresses), one attempt per second
    static constexpr uint8_t GLOBAL_FAILURE_LIMIT = 10;
    uint64_t lastFailure = 0;
    uint64_t globalWindowStart = 0;
    uint8_t globalFailures = 0;

    static uint64_t nowMs();
    static uint32_t clientIp(AsyncWebServerRequest *request);
    // Entry of the client, nullptr when the table is full of clients with
    // recent failures (then only the global 1 s spacing applies)
    Attempts *attemptsFor(uint32_t ip);
    // Seconds until the client may try again, 0 = allowed
    unsigned long lockedFor(uint32_t ip);
    void recordFailure(uint32_t ip);

    bool verifyPassword(const String &password) const;
    static String hashPassword(const String &password, const String &saltHex);
    static String randomHex(size_t bytes);
    static String sessionToken(AsyncWebServerRequest *request);
    String createSession();
    void endAllSessions();
};
