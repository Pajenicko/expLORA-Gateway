/**
 * expLORA Gateway
 *
 * Logging system implementation file
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

#include "Logging.h"
#include <cstdarg>
#include <time.h>
#include <esp_heap_caps.h>
#include <Arduino.h>

// Initialization of static variables
LogLevel Logger::currentLevel = LogLevel::INFO;
LogRing Logger::ring;
uint8_t *Logger::ringBuffer = nullptr;
std::mutex Logger::logMutex;
bool Logger::initialized = false;

bool Logger::timeInitialized = false;

void Logger::setTimeInitialized(bool initialized)
{
    timeInitialized = initialized;
}

// Logger initialization
bool Logger::init(size_t ringBytes)
{
    // If already initialized, first free the memory
    if (initialized)
    {
        deinit();
    }

    ringBuffer = static_cast<uint8_t *>(malloc(ringBytes));
    if (ringBuffer == nullptr)
    {
        Serial.println("Logger: Failed to allocate memory for logs");
        return false;
    }
    Serial.printf("Logger: Allocated %u bytes in RAM for logs\n", (unsigned)ringBytes);
    {
        std::lock_guard<std::mutex> lock(logMutex);
        ring.init(ringBuffer, ringBytes);
    }
    initialized = true;

    // First log after initialization - use char[] instead of String concat
    char initMsg[64];
    snprintf(initMsg, sizeof(initMsg), "Logging system initialized with a %u B ring", (unsigned)ringBytes);
    info(initMsg);

    return true;
}

// Free logger memory
void Logger::deinit()
{
    if (!initialized)
        return;

    std::lock_guard<std::mutex> lock(logMutex);

    ring.init(nullptr, 0);
    free(ringBuffer);
    ringBuffer = nullptr;

    initialized = false;
}

// Set logging level
void Logger::setLogLevel(LogLevel level)
{
    currentLevel = level;
    info("Log level set to: " + levelToString(level));
}

// Get current logging level
LogLevel Logger::getLogLevel()
{
    return currentLevel;
}

// Local time of an entry
LogRing::Time Logger::getTime()
{
    LogRing::Time t;
    if (timeInitialized)
    {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0) && timeinfo.tm_year >= 100 && timeinfo.tm_year <= 355)
        {
            t.valid = true;
            t.year = (uint16_t)(timeinfo.tm_year + 1900);
            t.month = (uint8_t)(timeinfo.tm_mon + 1);
            t.day = (uint8_t)timeinfo.tm_mday;
            t.hour = (uint8_t)timeinfo.tm_hour;
            t.minute = (uint8_t)timeinfo.tm_min;
            t.second = (uint8_t)timeinfo.tm_sec;
        }
    }
    return t;
}

// Add log with specified level
void Logger::log(LogLevel level, const String &message)
{
    if (!initialized || level > currentLevel)
    {
        return;
    }

    // Get timestamp only once
    LogRing::Time t = getTime();
    char timeStamp[LogRing::TIME_TEXT];
    LogRing::formatTime(t, timeStamp);

    // Print to serial port
    Serial.print(timeStamp);
    Serial.print(F(" ["));
    Serial.print(levelToString(level));
    Serial.print(F("] "));
    Serial.println(message);

    std::lock_guard<std::mutex> lock(logMutex);

    // Store a capped version (the last 3 chars become an ellipsis)
    if ((size_t)message.length() > LOG_MESSAGE_CAPACITY)
    {
        // On the heap: logged from tasks with small stacks too
        String capped = message.substring(0, LOG_MESSAGE_CAPACITY - 3);
        capped += F("...");
        ring.push((uint8_t)level, t, capped.c_str(), capped.length());
    }
    else
    {
        ring.push((uint8_t)level, t, message.c_str(), message.length());
    }
}

void Logger::logf(LogLevel level, const char *format, ...)
{
    if (!initialized || level > currentLevel)
    {
        return;
    }
    char text[160];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    log(level, String(text));
}

// Helper methods for different log levels
void Logger::error(const String &message)
{
    log(LogLevel::ERROR, message);
}

void Logger::warning(const String &message)
{
    log(LogLevel::WARNING, message);
}

void Logger::info(const String &message)
{
    log(LogLevel::INFO, message);
}

void Logger::debug(const String &message)
{
    log(LogLevel::DEBUG, message);
}

void Logger::verbose(const String &message)
{
    log(LogLevel::VERBOSE, message);
}

bool Logger::copyNewest(size_t n, LogEntry &out)
{
    uint8_t level;
    char time[LogRing::TIME_TEXT];
    char msg[LOG_MESSAGE_CAPACITY + 1];
    size_t len;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (!ring.get(n, level, time, msg, sizeof(msg), len))
        {
            return false;
        }
    }
    out.level = (LogLevel)level;
    out.formattedTime = time;
    out.message = String();
    out.message.concat(msg, len < sizeof(msg) ? len : sizeof(msg) - 1);
    return true;
}

// Clear all logs
void Logger::clearLogs()
{
    std::lock_guard<std::mutex> lock(logMutex);
    ring.clear();
}

// Convert log level from string
LogLevel Logger::levelFromString(const String &levelStr)
{
    if (levelStr.equalsIgnoreCase("ERROR"))
        return LogLevel::ERROR;
    if (levelStr.equalsIgnoreCase("WARNING"))
        return LogLevel::WARNING;
    if (levelStr.equalsIgnoreCase("INFO"))
        return LogLevel::INFO;
    if (levelStr.equalsIgnoreCase("DEBUG"))
        return LogLevel::DEBUG;
    if (levelStr.equalsIgnoreCase("VERBOSE"))
        return LogLevel::VERBOSE;

    return LogLevel::INFO;
}

// Convert log level to string
String Logger::levelToString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::WARNING:
        return "WARNING";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::VERBOSE:
        return "VERBOSE";
    default:
        return "UNKNOWN";
    }
}
