/**
 * expLORA Gateway
 *
 * Log ring: variable-length log records in one fixed byte buffer
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

// Pure C++ (no Arduino): native tests in test/test_log_ring.
//
// Every record takes its real size instead of a fixed slot with reserved
// Strings: header (2 B length, 1 B level, 1 B time flag), the local time
// packed into 6 bytes (when set), the message, and a 2 B trailer length so
// the newest records can be walked backwards. The oldest records are
// dropped to make room, so the ring keeps as many entries as fit.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class LogRing
{
public:
    // Local time of a record; valid = false prints "[Time not set]"
    struct Time
    {
        bool valid = false;
        uint16_t year = 0; // 2000..2255
        uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
    };

    static constexpr size_t HEADER = 4;
    static constexpr size_t TIME_BYTES = 6;
    static constexpr size_t TRAILER = 2;
    static constexpr size_t TIME_TEXT = 20; // "YYYY-MM-DD HH:MM:SS" + NUL

    // "YYYY-MM-DD HH:MM:SS", or "[Time not set]"
    static void formatTime(const Time &t, char (&out)[TIME_TEXT])
    {
        if (t.valid)
        {
            snprintf(out, TIME_TEXT, "%04u-%02u-%02u %02u:%02u:%02u", (unsigned)t.year, (unsigned)t.month,
                     (unsigned)t.day, (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
        }
        else
        {
            snprintf(out, TIME_TEXT, "[Time not set]");
        }
    }

    // buf must stay valid for the ring's lifetime
    void init(uint8_t *buf, size_t size)
    {
        ring = buf;
        cap = size;
        clear();
    }

    void clear()
    {
        head = tail = used = 0;
        records = 0;
    }

    size_t count() const { return records; }
    size_t bytesUsed() const { return used; }
    size_t capacity() const { return cap; }

    // Bytes a record of this message length takes in the ring
    static size_t recordSize(size_t msgLen, bool timeValid)
    {
        return HEADER + (timeValid ? TIME_BYTES : 0) + msgLen + TRAILER;
    }

    // Append a record, dropping the oldest ones until it fits. false (ring
    // unchanged) when it can never fit or the year is outside 2000..2255
    bool push(uint8_t level, const Time &t, const char *msg, size_t len)
    {
        if (ring == nullptr || (t.valid && (t.year < 2000 || t.year > 2255)))
        {
            return false;
        }
        size_t size = recordSize(len, t.valid);
        if (size > cap || size > 0xFFFF)
        {
            return false;
        }
        while (cap - used < size)
        {
            dropOldest();
        }
        uint8_t hdr[HEADER + TIME_BYTES];
        hdr[0] = (uint8_t)(size & 0xFF);
        hdr[1] = (uint8_t)(size >> 8);
        hdr[2] = level;
        hdr[3] = t.valid ? 1 : 0;
        size_t h = HEADER;
        if (t.valid)
        {
            hdr[h++] = (uint8_t)(t.year - 2000);
            hdr[h++] = t.month;
            hdr[h++] = t.day;
            hdr[h++] = t.hour;
            hdr[h++] = t.minute;
            hdr[h++] = t.second;
        }
        write(head, hdr, h);
        write(head + h, reinterpret_cast<const uint8_t *>(msg), len);
        write(head + h + len, hdr, TRAILER); // same two length bytes
        head = (head + size) % cap;
        used += size;
        records++;
        return true;
    }

    // The n-th newest record (0 = newest). time gets the formatted local
    // time (or "[Time not set]"), msg up to msgCap - 1 bytes plus a NUL;
    // msgLen is the stored length. false past the end
    bool get(size_t n, uint8_t &level, char (&time)[TIME_TEXT], char *msg, size_t msgCap, size_t &msgLen) const
    {
        if (n >= records)
        {
            return false;
        }
        size_t end = head; // one past the record
        size_t size = 0;
        for (size_t i = 0; i <= n; i++)
        {
            uint8_t t[TRAILER];
            read((end + cap - TRAILER) % cap, t, TRAILER);
            size = (size_t)t[0] | ((size_t)t[1] << 8);
            if (i < n)
            {
                end = (end + cap - size) % cap;
            }
        }
        size_t start = (end + cap - size) % cap;
        uint8_t hdr[HEADER + TIME_BYTES];
        read(start, hdr, HEADER);
        level = hdr[2];
        size_t h = HEADER;
        Time t;
        if (hdr[3] == 1)
        {
            read(start + HEADER, hdr + HEADER, TIME_BYTES);
            h += TIME_BYTES;
            t.valid = true;
            t.year = (uint16_t)(2000 + hdr[4]);
            t.month = hdr[5];
            t.day = hdr[6];
            t.hour = hdr[7];
            t.minute = hdr[8];
            t.second = hdr[9];
        }
        formatTime(t, time);
        msgLen = size - h - TRAILER;
        size_t take = msgLen < msgCap ? msgLen : (msgCap > 0 ? msgCap - 1 : 0);
        read(start + h, reinterpret_cast<uint8_t *>(msg), take);
        if (msgCap > 0)
        {
            msg[take] = '\0';
        }
        return true;
    }

private:
    uint8_t *ring = nullptr;
    size_t cap = 0;
    size_t head = 0; // next write position
    size_t tail = 0; // oldest record
    size_t used = 0;
    size_t records = 0;

    void dropOldest()
    {
        uint8_t l[2];
        read(tail, l, 2);
        size_t size = (size_t)l[0] | ((size_t)l[1] << 8);
        tail = (tail + size) % cap;
        used -= size;
        records--;
    }

    void write(size_t pos, const uint8_t *src, size_t len)
    {
        pos %= cap;
        size_t first = len < cap - pos ? len : cap - pos;
        memcpy(ring + pos, src, first);
        memcpy(ring, src + first, len - first);
    }

    void read(size_t pos, uint8_t *dst, size_t len) const
    {
        pos %= cap;
        size_t first = len < cap - pos ? len : cap - pos;
        memcpy(dst, ring + pos, first);
        memcpy(dst + first, ring, len - first);
    }
};
