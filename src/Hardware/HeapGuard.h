/**
 * expLORA Gateway
 *
 * Usable heap and the memory guards built on it (pure part, unit-tested
 * natively in test/test_heap_guard)
 *
 * ESP.getFreeHeap() / ESP.getMaxAllocHeap() count MALLOC_CAP_INTERNAL. On the
 * classic ESP32 (Pro) that includes ~33 kB of IRAM that only allows 32-bit
 * access (one 32,756 B block): malloc, String, lwIP and mbedTLS never get it.
 * Measured on the Pro (25add44): idle internal 76,944 / block 36,852, but the
 * 8-bit heap 43,752 / 36,852; during 5 parallel /sensors the internal figures
 * were 38,876 / 32,756 while the 8-bit heap was down to 5,684 / 4,340 and
 * allocations failed (connection resets, once an abort in the Ethernet RX
 * task). On the ESP32-S3 (Lite) both are the same, ~109 kB at idle. Every
 * decision here uses the 8-bit heap.
 *
 * On a board with PSRAM in the heap MALLOC_CAP_8BIT also counts the PSRAM.
 * mbedTLS allocates internal RAM only (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC),
 * so the TLS checks use tlsHeap() = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
 * on the Pro and the Lite (no PSRAM) both views are the same.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace HeapGuard
{
// Free bytes and largest free block of the heap malloc can use
struct Heap
{
    size_t free;
    size_t largest;
};

#ifdef ESP_PLATFORM
inline Heap usable()
{
    return {heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)};
}

// The internal 8-bit heap, where mbedTLS allocates (without PSRAM = usable())
inline Heap tlsHeap()
{
    return {heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)};
}
#endif

// Left free after a guarded peak for everything else: Ethernet / WiFi RX
// frames, lwIP (new connections, their 503 answers), newlib locks, the loop
// task (logs, radio packets, MQTT, sensor saves). The Pro failed at
// 5.7 kB free / 4.3 kB largest block
constexpr size_t BASE_RESERVE = 8000;

// Per heavy response already running: what it may still allocate after it
// was admitted. Handlers run one after another in the AsyncTCP task and a
// response starts sending before its handler's task event ends, so the
// next admission sees what the running ones hold; they can still grow by
// their lwIP in-flight copy (up to TCP_SND_BUF, 5,744 B) and, for a streamed
// page, by a next piece larger than the current one (~2 kB)
constexpr size_t ACTIVE_GROWTH = 8000;

// Page built as one String and sent with send(): the String and the
// response's copy of it, then while it goes out the remaining content, its
// substring and the piece being written (<= TCP_SND_BUF), plus the lwIP
// in-flight copy (together <= TCP_SND_BUF) and the connection itself
// (request, headers, ~1.5 kB): 2 x size + 8 kB
constexpr size_t STRING_PAGE_OVERHEAD = 8000;
inline size_t stringPagePeak(size_t pageSize)
{
    return 2 * pageSize + STRING_PAGE_OVERHEAD;
}

// Page streamed piece by piece (chunked): the piece (<= ~4 kB), the
// response's send buffer (<= TCP_SND_BUF + chunk header, the largest single
// allocation), the lwIP in-flight copy (<= TCP_SND_BUF) and the connection.
// Measured: ~6-11 kB per page in flight (Pro / Lite, 5 parallel)
constexpr size_t STREAMED_PAGE_PEAK = 16000;
constexpr size_t STREAMED_PAGE_BLOCK = 6144;

enum class Admit
{
    Ok,
    Busy,     // maxActive heavy responses already running
    LowMemory // not enough heap for this one next to the running ones
};

// May a heavy response with this peak and largest single allocation start
// while `active` others run? Pro idle (43,752 / 36,852): the largest page
// (home page with 20 sensors, 2 x 9,096 + 8,000 = 26.2 kB) needs 34.2 kB;
// a second streamed page next to a running one needs 32 kB
inline Admit admit(Heap heap, size_t peak, size_t block, unsigned active, unsigned maxActive)
{
    if (active >= maxActive)
    {
        return Admit::Busy;
    }
    if (heap.free < peak + BASE_RESERVE + active * ACTIVE_GROWTH || heap.largest < block)
    {
        return Admit::LowMemory;
    }
    return Admit::Ok;
}

// One TLS 1.2 session (Arduino's mbedTLS: 16 kB record buffers both ways,
// CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN 16384, no asymmetric / variable /
// dynamic buffers, the peer certificate kept). Each record buffer is its own
// allocation of 13 header + 16 IV + 48 MAC + 256 padding + 16,384 = 16,717 B.
// Measured on the Lite (bench 2, d8aff35): a whole update check (handshake
// with the CA bundle, GET of the manifest) took the 8-bit heap from 109,588
// to a low of 58,448 B = 51.1 kB for the session and what the system did
// meanwhile. The Pro at idle (~43 kB free, largest block 40,948) never got
// through a handshake: low-water 1,164 B, X509 allocation failed, Ethernet
// receive buffers dropped. Peak 52 kB, plus BASE_RESERVE for the rest of
// the system. The two record buffers are separate mallocs of 16,717 B each
// (in and out, allocated one after the other at the session setup). The
// block check guarantees only the first of them: it needs the largest block
// to hold one buffer (with the heap's block header). The second one is
// covered only by TLS_MIN_FREE, i.e. it fits if the rest of the first block
// or another block still holds 16.7 kB - not guaranteed on a fragmented
// heap; then the session fails cleanly (mbedTLS allocation error, the check
// or download is reported as failed) instead of crashing. A stricter check
// (largest >= 2 buffers, 33,466 B) is not used: the heap API gives only the
// largest block, and the Lite after one session had 33,780 B there with
// 109 kB free, so a few more sessions would refuse HTTPS on a heap that
// still has room for both buffers in two blocks
constexpr size_t TLS_RECORD_BUFFER = 16717;
constexpr size_t TLS_PEAK = 52000;
constexpr size_t TLS_MIN_FREE = TLS_PEAK + BASE_RESERVE;
constexpr size_t TLS_MIN_BLOCK = TLS_RECORD_BUFFER + 16;

// Use with tlsHeap() (internal RAM, see the top of this file)
inline bool tlsFits(Heap heap)
{
    return heap.free >= TLS_MIN_FREE && heap.largest >= TLS_MIN_BLOCK;
}

// A persistent MQTT TLS session is not transient like an update check: it
// keeps ~40 kB (both record buffers, the session, the peer certificate) for
// as long as it is connected, and the rest of the system (web UI, LTE, the
// radio) has to live next to it. Measured on the Pro (aa462a9, 10 min with
// the session): 67.8 kB free before connecting left 27.4 kB / largest block
// 22.5 kB with the session up, the web UI answered 1 of 60 pages (59 x 503);
// 81.6 kB left 41.4 kB (12 of 60 pages, the rest a clean 503), 102.6 kB left
// 62.2 kB (24 of 60). So MQTT connects over TLS only with 80 kB free (and
// the block of TLS_MIN_BLOCK) - on the Pro that means WiFi switched off; the
// Lite (~109 kB idle) fits. The transient HTTPS checks keep TLS_MIN_FREE
constexpr size_t MQTT_TLS_MIN_FREE = 80000;

// Use with tlsHeap()
inline bool mqttTlsFits(Heap heap)
{
    return heap.free >= MQTT_TLS_MIN_FREE && heap.largest >= TLS_MIN_BLOCK;
}
} // namespace HeapGuard
