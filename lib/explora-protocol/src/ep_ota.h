/**
 * expLORA protocol generation 2 - OTA transfer, pure parts (docs/protocol-v2.md 14)
 *
 * TLV values of OTA start / block / end (downlink) and OTA status (uplink),
 * block index math, the block bitmap with its 64-bit status window, the
 * block schedule (slots, rounds, cadence) and the LoRa airtime used to size
 * the cadence. No radio, no flash, no time source: callers pass numbers.
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

namespace ep
{
namespace ota
{

constexpr uint8_t kTransferVersion = 1;
constexpr uint8_t kBlockSize = 232;          // bytes of image per block (multiple of 8)
constexpr uint32_t kSlotSize = 114688;       // sensor slot B (spec 10)
constexpr uint32_t kMinImageLen = 512;       // vector table + trailer at least
constexpr uint16_t kMaxBlocks = 512;         // bitmap capacity (a full slot is 495 blocks of 232 B)
constexpr uint8_t kWindowBlocks = 64;        // status window
constexpr uint8_t kMinK = 32;                // blocks per round (status every K blocks)
constexpr uint8_t kMaxK = 64;                // a round never leaves the status window
constexpr uint16_t kMinCadence100ms = 20;    // 2 s
constexpr uint16_t kMinStartDelayMs = 3000;  // after the accepting UP's RX2 window
constexpr uint8_t kSilenceRounds = 2;        // sensor leaves after 2 rounds without a valid block
constexpr uint8_t kStatusMissesToPause = 2;  // gateway pauses after 2 status slots without a status
constexpr uint32_t kMinRoundS = 720;         // (K + 1) * C >= 12 min: at most 5 statuses per hour (D7)
constexpr uint32_t kOtaLateMs = 15;          // an OTA launch later than this is skipped (not 40 ms, §7)
constexpr uint32_t kDriftPpm = 200;          // timing allowance of the sensor's receive windows
constexpr uint32_t kRxLeadMs = 20;           // the window opens this much before the slot
constexpr uint32_t kRxTimeoutMs = 200;       // ... and waits this long for a header
constexpr uint32_t kAnchorStepMs = 20;       // a re-anchor moves the schedule by at most this + drift
constexpr uint32_t kOtaSharePermille = 480;  // of the gateway's 10 % budget: 60 % of the 80 % data share

// --- OTA start (DOWN 0xC1) -----------------------------------------------------

struct Start
{
    uint8_t version = kTransferVersion;
    uint32_t imageLen = 0;     // image + 32-byte trailer, multiple of 8
    uint32_t imageCrc = 0;     // the trailer's CRC-32 (last 4 bytes of the image)
    uint32_t fwVersion = 0;    // major << 16 | minor << 8 | patch (trailer)
    uint16_t hwType = 0;       // trailer HW type
    uint8_t blockSize = kBlockSize;
    uint8_t k = kMinK;         // blocks per round; the round's last slot is the status slot
    uint16_t cadence100ms = 0; // slot spacing, 0.1 s
    uint8_t sf = 9;            // spreading factor of the OTA frames
    uint16_t startDelayMs = 4000; // slot 0 = accepting UP's TxDone (gateway: RxDone) + this
};
constexpr size_t kStartLen = 22;

size_t encodeStart(const Start &s, uint8_t out[kStartLen]);
// false when shorter than kStartLen (longer values: extra bytes ignored)
bool decodeStart(const uint8_t *v, size_t len, Start &s);

// Structural check of a start (the sensor's refusal reason, 0 = acceptable).
// Does not check the device: HW type, swap state, battery, RTC.
uint8_t checkStart(const Start &s);
// Frame of one full block: header 8, TLV type + length 2, index 2, data, MIC 4
constexpr size_t kBlockFrameLen = 8 + 2 + 2 + kBlockSize + 4; // 248
// Sensor silence limit of a session: kSilenceRounds rounds without a valid block
inline uint32_t silenceMs(const Start &s)
{
    return (uint32_t)kSilenceRounds * ((uint32_t)s.k + 1) * s.cadence100ms * 100;
}

// --- OTA block (DOWN 0xC2): u16 index | data ---------------------------------

size_t encodeBlock(uint16_t index, const uint8_t *data, size_t len, uint8_t *out, size_t cap);
bool decodeBlock(const uint8_t *v, size_t len, uint16_t &index, const uint8_t *&data, size_t &dataLen);

// --- OTA session DOWN (FCtrl bit 5, spec 14.2) --------------------------------

// The payload of an OTA session DOWN carries exactly one TLV: an OTA block
// (0xC2) or an OTA end (0xC3). false for anything else (the frame is dropped).
// The other FCtrl bits are ignored by the receiver.
bool sessionTlv(const uint8_t *payload, size_t len, uint8_t &type, const uint8_t *&value, size_t &valueLen);

// --- OTA end (DOWN 0xC3): u8 code ---------------------------------------------

enum class EndCode : uint8_t
{
    Pause = 0, // leave the session, keep the progress (resumed by a later start)
    Abort = 1, // leave the session and discard the progress
};

// --- OTA status (UP 0xC3) -------------------------------------------------------

enum class StatusKind : uint8_t
{
    Progress = 0, // 12-byte value: u16 base | u64 window (bit i = block base + i received) | u16 slot
    Accepted = 1, // arg 0 new transfer, 1 resumed
    Refused = 2,  // arg: Refusal
    Complete = 3, // image verified, PENDING written, the sensor restarts into the swap
    Failed = 4,   // arg: Failure (the progress is discarded)
    Left = 5,     // arg: LeaveReason (the progress is kept unless the gateway aborted)
    BatteryLow = 6, // image verified, PENDING waits for the battery; arg = battery mV / 100
};

enum Refusal : uint8_t
{
    kRefuseVersion = 1,     // transfer version not supported
    kRefuseImage = 2,       // image length / block size / K / cadence / SF out of range
    kRefuseHwType = 3,      // HW type differs from the identity
    kRefuseSwapState = 4,   // page 117 not idle-like (an active PLAN)
    kRefuseUnconfirmed = 5, // the running image is not confirmed yet (BOOT / SWAPPED)
    kRefuseBattery = 6,     // battery below 2.4 V under load
    kRefuseRtc = 7,         // RTC not usable: the new image could never be confirmed
    kRefuseFlash = 8,       // slot B / page 118 erase failed
};

enum Failure : uint8_t
{
    kFailCrc = 1,      // CRC over the received image wrong
    kFailTrailer = 2,  // trailer (magic, version, size, load address, vectors) invalid
    kFailIdentity = 3, // image length / CRC / version / HW type differ from the start
    kFailFlash = 4,    // programming failed and the page could not be erased
    kFailNeedsIdentity = 5, // sensor 1.0.1+: the image needs a provisioned identity, the sensor runs a
                            // development identity - verified, but not installed (spec 14.4)
};

enum LeaveReason : uint8_t
{
    kLeaveSilence = 1, // no valid block for silenceMs()
    kLeaveBattery = 2, // battery below 2.3 V under load
    kLeaveEnd = 3,     // OTA end received
    // 4 reserved (a new session needs no "left": the gateway sees the join)
};

struct Status
{
    StatusKind kind = StatusKind::Progress;
    uint8_t arg = 0;
    uint16_t base = 0;   // Progress: lowest missing block (= block count when complete)
    uint64_t window = 0; // Progress: bit i = block base + i received
    uint16_t slot = 0;   // Progress: index of the status slot it is sent in (re-anchors the gateway)
};
// The two value forms are told apart by their length only: a progress value
// is always 12 bytes, an event always 2; other lengths are ignored (future
// forms must use other lengths)
constexpr size_t kStatusProgressLen = 12;
constexpr size_t kStatusEventLen = 2;

size_t encodeStatus(const Status &s, uint8_t *out, size_t cap);
bool decodeStatus(const uint8_t *v, size_t len, Status &s);

// --- block math ------------------------------------------------------------------

// Image trailer flags bits 4-5 (spec 10.1, sensor 1.0.1+): the identity the
// image needs to run. 0 = not declared (every image built before sensor
// 1.0.1), 1 = any identity (bench build e77_bench: a provisioned record or
// the UID-derived development identity), 2 = a provisioned identity record
// (production build e77: without one it stops - an unconfirmed image resets
// and the bootloader reverts it). 3 is reserved and read as 2.
constexpr uint32_t kTrailerIdentityShift = 4;
constexpr uint32_t kTrailerIdentityMask = 0x30;
enum ImageIdentity : uint8_t
{
    kImageIdentityUndeclared = 0,
    kImageIdentityAny = 1,
    kImageIdentityProvisioned = 2,
};

inline uint8_t imageIdentity(uint32_t trailerFlags)
{
    const uint8_t v = (uint8_t)((trailerFlags & kTrailerIdentityMask) >> kTrailerIdentityShift);
    return v > kImageIdentityProvisioned ? (uint8_t)kImageIdentityProvisioned : v;
}

inline uint32_t trailerIdentityFlags(uint8_t identity)
{
    return ((uint32_t)identity << kTrailerIdentityShift) & kTrailerIdentityMask;
}

// The sensor's check before the swap (spec 14.4 completion step 1): an image
// declared for a provisioned identity is not installed on a sensor that runs
// a development identity (it would reset at its first boot and revert after
// three boots). An undeclared image is installed as before (unknown)
inline bool refuseForIdentity(uint32_t trailerFlags, bool provisioned)
{
    return !provisioned && imageIdentity(trailerFlags) == kImageIdentityProvisioned;
}

inline uint16_t blockCount(uint32_t imageLen, uint8_t blockSize)
{
    return blockSize ? (uint16_t)((imageLen + blockSize - 1) / blockSize) : 0;
}
inline uint32_t blockOffset(uint16_t index, uint8_t blockSize)
{
    return (uint32_t)index * blockSize;
}
// Length of block index (the last one is shorter, still a multiple of 8); 0 past the end
uint16_t blockLength(uint16_t index, uint32_t imageLen, uint8_t blockSize);
// Flash pages of slot B the image occupies
inline uint32_t pageCount(uint32_t imageLen, uint32_t pageSize)
{
    return (imageLen + pageSize - 1) / pageSize;
}
// Blocks overlapping flash page `page` of the slot; false when none
bool blocksInPage(uint32_t page, uint32_t imageLen, uint8_t blockSize, uint32_t pageSize, uint16_t &first,
                  uint16_t &last);

// --- bitmap ------------------------------------------------------------------------

class Bitmap
{
public:
    explicit Bitmap(uint16_t n = 0) { reset(n); }
    void reset(uint16_t n);
    uint16_t size() const { return n_; }
    bool test(uint16_t i) const { return i < n_ && (bits_[i >> 3] >> (i & 7)) & 1; }
    void set(uint16_t i);
    void clear(uint16_t i);
    uint16_t count() const;
    bool complete() const { return count() == n_; }
    uint16_t firstMissing() const; // n when complete
    // Status window: base = firstMissing(), bit i = block base + i
    uint64_t window(uint16_t base) const;
    // Gateway: take a progress status as the truth - everything below base
    // received, inside the window exactly the reported bits, nothing above
    // it (the sensor's bitmap can shrink after a resume check). A base past
    // the block count is ignored (false: count the status as missed)
    bool applyStatus(uint16_t base, uint64_t window);
    const uint8_t *bytes() const { return bits_; }
    static constexpr size_t kBytes = kMaxBlocks / 8;
    void load(uint16_t n, const uint8_t *bytes); // from a checkpoint (bits >= n cleared)

private:
    uint16_t n_ = 0;
    uint8_t bits_[kBytes] = {};
};

// --- schedule ----------------------------------------------------------------------

// Slot s at anchor + startDelay + s * cadence. A round is K block slots
// followed by one status slot.
inline bool isStatusSlot(uint32_t slot, uint8_t k)
{
    return slot % ((uint32_t)k + 1) == k;
}
inline uint32_t slotOffsetMs(uint32_t slot, const Start &s)
{
    return s.startDelayMs + slot * (uint32_t)s.cadence100ms * 100;
}
// Half-width of the sensor's timing allowance after `sinceAnchorMs` without
// a received block (drift of both clocks, kDriftPpm)
inline uint32_t driftMs(uint32_t sinceAnchorMs)
{
    return (uint32_t)(((uint64_t)sinceAnchorMs * kDriftPpm + 999999) / 1000000);
}
// Sensor re-anchoring: the measured offset of a block against the schedule
// is applied only up to +-(kAnchorStepMs + drift), so one bogus timestamp
// cannot drag the schedule away
inline int32_t anchorStep(int32_t measuredOffsetMs, uint32_t sinceAnchorMs)
{
    const int32_t lim = (int32_t)(kAnchorStepMs + driftMs(sinceAnchorMs));
    return measuredOffsetMs > lim ? lim : (measuredOffsetMs < -lim ? -lim : measuredOffsetMs);
}

// LoRa time on air, BW 125 kHz, CR 4/5, preamble 16, explicit header, CRC
// on, low data rate optimisation at SF11/12 (spec 1), microseconds
uint32_t airtimeUs(size_t frameLen, uint8_t sf);

// Smallest cadence (0.1 s steps, rounded up to whole seconds) that keeps the
// OTA airtime within kOtaSharePermille of the hour: frameAirtime * K / (K + 1)
// per cadence (the status slot carries no gateway frame), and a round of at
// least kMinRoundS
uint16_t cadenceFor(uint32_t frameAirtimeUs, uint8_t k);

} // namespace ota
} // namespace ep
