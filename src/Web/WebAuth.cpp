/**
 * expLORA Gateway
 *
 * Web interface login implementation file
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

#include "WebAuth.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

// Stored as "pbkdf2$<iterations>$<salt hex>$<hash hex>"
static constexpr unsigned PBKDF2_ITERATIONS = 10000;
static constexpr size_t SALT_BYTES = 16;
static constexpr size_t HASH_BYTES = 32;
static const char SESSION_COOKIE[] = "explora_session";

WebAuth::WebAuth(ConfigManager &cfg, Logger &log) : config(cfg), logger(log)
{
}

// The hash is reset from the loop task (serial console): read it under the lock
bool WebAuth::hasPassword() const
{
    std::lock_guard<std::recursive_mutex> guard(config.mutex());
    return config.adminPasswordHash.length() > 0;
}

String WebAuth::randomHex(size_t bytes)
{
    String out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; i++)
    {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", (uint8_t)esp_random());
        out += hex;
    }
    return out;
}

String WebAuth::hashPassword(const String &password, const String &saltHex)
{
    uint8_t hash[HASH_BYTES];
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                           reinterpret_cast<const unsigned char *>(password.c_str()), password.length(),
                                           reinterpret_cast<const unsigned char *>(saltHex.c_str()), saltHex.length(),
                                           PBKDF2_ITERATIONS, sizeof(hash), hash);
    if (rc != 0)
    {
        return String();
    }
    String out = "pbkdf2$" + String(PBKDF2_ITERATIONS) + "$" + saltHex + "$";
    for (uint8_t b : hash)
    {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", b);
        out += hex;
    }
    return out;
}

bool WebAuth::verifyPassword(const String &password) const
{
    if (!hasPassword())
    {
        return password.length() == 0;
    }
    String stored;
    {
        std::lock_guard<std::recursive_mutex> guard(config.mutex());
        stored = config.adminPasswordHash;
    }
    int first = stored.indexOf('$');
    int second = stored.indexOf('$', first + 1);
    int third = stored.indexOf('$', second + 1);
    if (first < 0 || second < 0 || third < 0)
    {
        return false;
    }
    String computed = hashPassword(password, stored.substring(second + 1, third));
    if (computed.length() != stored.length())
    {
        return false;
    }
    // Constant time
    uint8_t diff = 0;
    for (size_t i = 0; i < stored.length(); i++)
    {
        diff |= computed[i] ^ stored[i];
    }
    return diff == 0;
}

uint64_t WebAuth::nowMs()
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint32_t WebAuth::clientIp(AsyncWebServerRequest *request)
{
    return request->client() ? (uint32_t)request->client()->remoteIP() : 0;
}

// Entry of this client; a new client takes a free or clean entry (no
// failures, no running lock). Entries with failures are never evicted, so
// rotating addresses cannot reset counters - when none is clean, a new
// address has to wait like a locked one.
WebAuth::Attempts *WebAuth::attemptsFor(uint32_t ip)
{
    uint64_t now = nowMs();
    Attempts *clean = nullptr;
    for (Attempts &a : attempts)
    {
        if (a.ip == ip)
        {
            return &a;
        }
        bool expired = a.failures == 0 || now - a.lastFailureAt > LOGIN_LOCK_TIME;
        if (clean == nullptr && (a.ip == 0 || (expired && a.lockedUntil <= now)))
        {
            clean = &a;
        }
    }
    if (clean != nullptr)
    {
        *clean = {ip, 0, 0, 0};
    }
    return clean;
}

unsigned long WebAuth::lockedFor(uint32_t ip)
{
    uint64_t now = nowMs();
    // Global throttle, only while many wrong passwords arrive
    if (globalFailures >= GLOBAL_FAILURE_LIMIT && now - globalWindowStart < 60000 && now - lastFailure < 1000)
    {
        return 1;
    }
    Attempts *a = attemptsFor(ip);
    if (a == nullptr)
    {
        // Table full of recent failures (an attack from many addresses):
        // new addresses get the global 1 s spacing, not a lock-out
        return now - lastFailure < 1000 ? 1 : 0;
    }
    return a->lockedUntil > now ? (unsigned long)((a->lockedUntil - now) / 1000 + 1) : 0;
}

void WebAuth::recordFailure(uint32_t ip)
{
    uint64_t now = nowMs();
    lastFailure = now;
    if (now - globalWindowStart >= 60000)
    {
        globalWindowStart = now;
        globalFailures = 0;
    }
    if (globalFailures < 255)
    {
        globalFailures++;
    }
    Attempts *entry = attemptsFor(ip);
    if (entry == nullptr)
    {
        return;
    }
    Attempts &a = *entry;
    if (a.failures > 0 && now - a.lastFailureAt > LOGIN_LOCK_TIME)
    {
        a.failures = 0; // old failures expired
    }
    a.lastFailureAt = now;
    if (++a.failures >= MAX_FAILED_LOGINS)
    {
        a.failures = 0;
        a.lockedUntil = nowMs() + LOGIN_LOCK_TIME;
        logger.warning("Web login locked for " + String((unsigned long)(LOGIN_LOCK_TIME / 1000)) + " s for " +
                       IPAddress(ip).toString() + " after failed attempts");
    }
    else if (a.failures == 1)
    {
        // First failure only: a guessing client must not flush the log ring
        logger.warning("Web login failed from " + IPAddress(ip).toString());
    }
}

String WebAuth::createSession()
{
    String token = randomHex(16);
    uint64_t now = nowMs();
    portENTER_CRITICAL(&lock);
    // Free slot, expired slot, or the least recently used one
    size_t slot = 0;
    for (size_t i = 0; i < MAX_SESSIONS; i++)
    {
        if (sessions[i].token[0] == 0 || now - sessions[i].lastUsed > SESSION_IDLE_TIMEOUT)
        {
            slot = i;
            break;
        }
        if (sessions[i].lastUsed < sessions[slot].lastUsed)
        {
            slot = i;
        }
    }
    strncpy(sessions[slot].token, token.c_str(), sizeof(sessions[slot].token) - 1);
    sessions[slot].token[sizeof(sessions[slot].token) - 1] = 0;
    sessions[slot].lastUsed = now;
    portEXIT_CRITICAL(&lock);
    return token;
}

void WebAuth::endAllSessions()
{
    portENTER_CRITICAL(&lock);
    memset(sessions, 0, sizeof(sessions));
    portEXIT_CRITICAL(&lock);
}

String WebAuth::login(AsyncWebServerRequest *request, const String &password, unsigned long &retryAfterS)
{
    uint32_t ip = clientIp(request);
    retryAfterS = lockedFor(ip);
    if (retryAfterS > 0)
    {
        return String();
    }
    if (password.length() > MAX_PASSWORD_LENGTH || !verifyPassword(password))
    {
        recordFailure(ip);
        // Report this client's lock only (not the 1 s global throttle)
        Attempts *entry = attemptsFor(ip);
        uint64_t now = nowMs();
        retryAfterS = entry && entry->lockedUntil > now ? (unsigned long)((entry->lockedUntil - now) / 1000 + 1) : 0;
        return String();
    }
    Attempts *entry = attemptsFor(ip);
    if (entry != nullptr)
    {
        *entry = {ip, 0, 0, 0};
    }
    logger.info("Web login from " + IPAddress(ip).toString());
    return createSession();
}

String WebAuth::sessionToken(AsyncWebServerRequest *request)
{
    const AsyncWebHeader *header = request->getHeader("Cookie");
    if (!header)
    {
        return String();
    }
    const String &cookies = header->value();
    String key = String(SESSION_COOKIE) + "=";
    int start = cookies.indexOf(key);
    while (start > 0 && cookies[start - 1] != ' ' && cookies[start - 1] != ';')
    {
        start = cookies.indexOf(key, start + 1);
    }
    if (start < 0)
    {
        return String();
    }
    start += key.length();
    int end = cookies.indexOf(';', start);
    return cookies.substring(start, end < 0 ? cookies.length() : end);
}

bool WebAuth::isAuthorized(AsyncWebServerRequest *request)
{
    String token = sessionToken(request);
    if (token.length() != 32)
    {
        return false;
    }
    uint64_t now = nowMs();
    bool valid = false;
    portENTER_CRITICAL(&lock);
    for (Session &session : sessions)
    {
        if (session.token[0] != 0 && token.equals(session.token))
        {
            if (now - session.lastUsed > SESSION_IDLE_TIMEOUT)
            {
                memset(&session, 0, sizeof(session));
            }
            else
            {
                session.lastUsed = now;
                valid = true;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&lock);
    return valid;
}

uint64_t WebAuth::sessionTag(AsyncWebServerRequest *request)
{
    if (!isAuthorized(request))
    {
        return 0;
    }
    String token = sessionToken(request);
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64
    for (size_t i = 0; i < token.length(); i++)
    {
        h = (h ^ (uint8_t)token[i]) * 1099511628211ULL;
    }
    return h ? h : 1;
}

void WebAuth::logout(AsyncWebServerRequest *request)
{
    String token = sessionToken(request);
    portENTER_CRITICAL(&lock);
    for (Session &session : sessions)
    {
        if (session.token[0] != 0 && token.equals(session.token))
        {
            memset(&session, 0, sizeof(session));
        }
    }
    portEXIT_CRITICAL(&lock);
}

String WebAuth::changePassword(AsyncWebServerRequest *request, const String &current, const String &password,
                               const String &confirm)
{
    uint32_t ip = clientIp(request);
    unsigned long retryAfter = lockedFor(ip);
    if (retryAfter > 0)
    {
        return "Too many failed attempts - try again in " + String(retryAfter) + " s";
    }
    if (hasPassword() && (current.length() > MAX_PASSWORD_LENGTH || !verifyPassword(current)))
    {
        recordFailure(ip);
        return "The current password is wrong";
    }
    if (password.length() < MIN_PASSWORD_LENGTH || password.length() > MAX_PASSWORD_LENGTH)
    {
        return "The password must have " + String(MIN_PASSWORD_LENGTH) + " to " + String(MAX_PASSWORD_LENGTH) +
               " characters";
    }
    if (password != confirm)
    {
        return "The passwords do not match";
    }
    String hash = hashPassword(password, randomHex(SALT_BYTES));
    if (hash.length() == 0 || !config.setAdminPasswordHash(hash))
    {
        return "Saving the password failed";
    }
    endAllSessions();
    logger.info("Admin password changed");
    return "";
}

void WebAuth::resetPassword()
{
    config.setAdminPasswordHash("");
    endAllSessions();
    logger.warning("Admin password reset from the serial console - set a new one on the next login");
}

String WebAuth::cookieFor(const String &token)
{
    String cookie = String(SESSION_COOKIE) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict";
    if (token.length() == 0)
    {
        cookie += "; Max-Age=0";
    }
    return cookie;
}
