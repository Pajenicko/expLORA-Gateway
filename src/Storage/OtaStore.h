/**
 * expLORA Gateway
 *
 * OTA image store for generation 2 sensors (protocol v2, docs/protocol-v2.md
 * 14.5): one image per HW type (at most 4) in LittleFS under /ota/.
 *
 * An upload is streamed to /ota/upload.tmp under the flash write permit,
 * the CRC-32 running over the stream (OtaImage::Streamer), never buffered in
 * RAM; at the end it is validated (length from the vector table word, trailer
 * magic / version / load address / CRC, size, the OTA flags) and stored as
 * /ota/hw<HHHH>.bin with /ota/hw<HHHH>.json (HW type, firmware version,
 * length, CRC, flags, upload time): the description goes to /ota/meta.tmp,
 * then upload.tmp and meta.tmp are renamed in this order (a failed last
 * rename writes the .json directly; a description write counts only once it
 * reads back as written). A leftover upload.tmp is deleted at
 * boot; a leftover meta.tmp (reset during the commit) is dropped right away
 * when upload.tmp was still there or meta.tmp is unreadable, otherwise
 * finished or dropped after a verify of the .bin (OtaCommitRecovery.h); a
 * .bin or .json without its pair and a .bin not as long as its .json are
 * deleted.
 * Block reads (loop()) take the flash permit too: a LittleFS access stalls
 * the flash like a write.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <Arduino.h>
#include "../Data/Logging.h"
#include "../Protocol/OtaImage.h"

namespace OtaStore
{
struct Meta
{
    bool valid = false;
    uint16_t hwType = 0;
    uint32_t fw = 0;
    uint32_t len = 0;
    uint32_t crc = 0;
    uint32_t flags = 0;
    uint32_t uploadUnix = 0;
};

// setup(): /ota exists, a leftover upload.tmp is gone, a cut commit sorted out
void begin(Logger &logger);

// Upload (web server task), one at a time
bool uploadBegin(String &error);
bool uploadWrite(const uint8_t *data, size_t len, String &error);
// Validate the finished upload; meta describes it (not yet stored)
bool uploadValidate(Meta &meta, String &error);
// Store it as the image of its HW type (replaces an older one)
bool uploadCommit(const Meta &meta, String &error);
void uploadAbort();
bool uploadRunning();

// Images
size_t list(Meta *out, size_t max);
bool get(uint16_t hwType, Meta &meta);
bool remove(uint16_t hwType);
// len bytes from offset of the image of hwType (loop())
bool read(uint16_t hwType, uint32_t offset, uint8_t *buf, size_t len);
// Re-verify the stored image against its CRC (job start)
bool verify(uint16_t hwType, const Meta &meta);
// Bytes used by the store (diagnostics)
uint32_t usedBytes();
// Timing of the flash accesses in loop() (diagnostics, M4b hardware test):
// block reads (incl. the wait for the flash permit) and the last CRC verify
struct Timing
{
    uint32_t reads = 0, readMaxUs = 0, readSumUs = 0, verifyUs = 0;
};
Timing timing();
} // namespace OtaStore
