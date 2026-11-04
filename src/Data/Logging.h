/**
 * expLORA Gateway
 *
 * Logging system header file
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
#include <mutex>
#include "../config.h"
#include "LogRing.h"

// Stored message length is capped (longer ones end in "...")
#ifndef LOG_MESSAGE_CAPACITY
#define LOG_MESSAGE_CAPACITY 300   // bytes kept per log message
#endif

/**
 * Definition of a logging system with support for various levels
 *
 * Allows setting the logging level via the web interface. Logs are kept in
 * a byte ring (LogRing.h, LOG_RING_BYTES): each entry takes only its real
 * size, the oldest ones make room for new ones.
 */

// Logging levels
enum class LogLevel
{
    ERROR = 0,   // Only critical errors
    WARNING = 1, // Errors and warnings
    INFO = 2,    // Basic activity information
    DEBUG = 3,   // Detailed information for debugging
    VERBOSE = 4  // All available information
};

// A log entry as copied out of the ring
struct LogEntry
{
    LogLevel level;          // Log level
    String message;          // Text message
    String formattedTime;    // Formatted time (if available)

    // Convert log level to text representation
    String getLevelString() const
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

    // Get color for the given log level (for web interface)
    String getLevelColor() const
    {
        switch (level)
        {
        case LogLevel::ERROR:
            return "#ff5555"; // Red
        case LogLevel::WARNING:
            return "#ffaa00"; // Orange
        case LogLevel::INFO:
            return "#2196F3"; // Blue
        case LogLevel::DEBUG:
            return "#4CAF50"; // Green
        case LogLevel::VERBOSE:
            return "#9E9E9E"; // Gray
        default:
            return "#000000"; // Black
        }
    }

    // Format log for UI display
    String getFormattedLog() const
    {
        return formattedTime + " [" + getLevelString() + "] " + message;
    }
};

class Logger
{
private:
    static LogLevel currentLevel;
    static LogRing ring;
    static uint8_t *ringBuffer;
    static std::mutex logMutex;
    static bool initialized;
    static bool timeInitialized;

    // Local time of an entry (valid when time is set)
    static LogRing::Time getTime();

public:
    // Logger initialization: ring of ringBytes bytes
    static bool init(size_t ringBytes = LOG_RING_BYTES);

    // Free memory on termination
    static void deinit();

    // Set logging level
    static void setLogLevel(LogLevel level);

    // Get current logging level
    static LogLevel getLogLevel();

    // Add log with specified level
    static void log(LogLevel level, const String &message);
    // printf-style (at most 159 characters; a few String temporaries less
    // in the code than a concatenated message)
    static void logf(LogLevel level, const char *format, ...) __attribute__((format(printf, 2, 3)));

    // Helper methods for different log levels
    static void error(const String &message);
    static void warning(const String &message);
    static void info(const String &message);
    static void debug(const String &message);
    static void verbose(const String &message);

    // Copy of the n-th newest entry (0 = newest) under the log lock; false
    // past the end
    static bool copyNewest(size_t n, LogEntry &out);

    // Clear all logs
    static void clearLogs();

    // Convert log level from string
    static LogLevel levelFromString(const String &levelStr);

    // Convert log level to string
    static String levelToString(LogLevel level);

    // Set time initialization state
    static void setTimeInitialized(bool initialized);

    static bool isTimeInitialized() { return timeInitialized; }

};
