/**
 * expLORA protocol generation 2 - frame codec
 *
 * Byte 0 of every frame (MHDR): 101 TT VVV
 *   101  generation 2 marker
 *   TT   frame type: 0 JOIN_REQ, 1 JOIN_ACC, 2 UP, 3 DOWN
 *   VVV  protocol version (1)
 * Bytes 1-4: DevID (big endian). Last 4 bytes: MIC.
 *
 * UP / DOWN    MHDR | DevID | FCnt16 | FCtrl | payload (AES-CTR, Ks_enc) | MIC (CMAC, Ks_mic)
 * JOIN_REQ     MHDR | DevID | "JR" | devNonce | hwType | fw major.minor.patch | caps | cfgVer | MIC (K_join_mic)
 * JOIN_ACC     MHDR | DevID | kind | body | MIC (K_join_mic)
 *              kind 0 ACCEPT: body = AES-CTR(K_join_enc, gwNonce | cfgVer | config TLVs)
 *              kind 1 REJOIN: no body; MIC bound to FCnt16 + MIC of the triggering uplink
 *
 * Crypto blocks (16 bytes, integers big endian):
 *   A_i = 01 | dir | code | DevID | counter | 00 00 00 00 | i        (i = 1, 2, ...)
 *   B0  = 49 | dir | code | DevID | counter | bind        | len(msg)
 *   MIC = CMAC(mic key, B0 | frame without MIC)[0..3]
 * dir 0 = uplink, 1 = downlink. code = frame type, REJOIN 0x11.
 * counter: FCnt32 for UP/DOWN, devNonce for JOIN_REQ/JOIN_ACC, FCnt16 of the
 * triggering uplink for REJOIN. bind: DOWN = FCnt32 of the uplink it answers
 * (ConfFCnt), REJOIN = MIC of the triggering uplink, otherwise 0.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "ep_crypto.h"
#include "ep_keys.h"

namespace ep
{

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kMarkerMask = 0xE0;
constexpr uint8_t kMarker = 0xA0;

constexpr size_t kMicLen = 4;
constexpr size_t kMaxFrame = 255;   // LoRa payload limit
constexpr size_t kMaxPayload = 222; // TLV bytes in UP / DOWN (keeps airtime bounded)
constexpr size_t kDataHeaderLen = 8;
constexpr size_t kDataOverhead = kDataHeaderLen + kMicLen;
constexpr size_t kJoinReqLen = 22;
constexpr size_t kJoinAccHeaderLen = 6; // MHDR + DevID + kind
constexpr size_t kJoinAcceptMinLen = kJoinAccHeaderLen + 5 + kMicLen;
constexpr size_t kRejoinLen = kJoinAccHeaderLen + kMicLen;

enum class FrameType : uint8_t
{
    JoinReq = 0,
    JoinAcc = 1,
    Up = 2,
    Down = 3,
};

enum class Dir : uint8_t
{
    Uplink = 0,
    Downlink = 1,
};

// Code byte in A_i / B0
enum class CryptoCode : uint8_t
{
    JoinReq = 0x00,
    JoinAccept = 0x01,
    Up = 0x02,
    Down = 0x03,
    Rejoin = 0x11,
};

enum class JoinAccKind : uint8_t
{
    Accept = 0,
    Rejoin = 1,
};

// FCtrl (byte 7 of UP / DOWN)
constexpr uint8_t kFctrlAck = 0x80;     // UP: gateway must answer; DOWN: acknowledges the uplink
constexpr uint8_t kFctrlPending = 0x40; // UP: sensor has more to send; DOWN: gateway has more queued
constexpr uint8_t kFctrlClassC = 0x20;  // UP: sensor listens continuously (mains powered)
constexpr uint8_t kFctrlCfgMask = 0x0F; // UP: config version applied; DOWN: config version held by the gateway

constexpr uint8_t kJoinReqMagic0 = 'J';
constexpr uint8_t kJoinReqMagic1 = 'R';

// JOIN_REQ caps byte
constexpr uint8_t kCapClassC = 0x01;
constexpr uint8_t kCapRx2 = 0x02;

inline uint8_t makeMhdr(FrameType type)
{
    return (uint8_t)(kMarker | ((uint8_t)type << 3) | kProtocolVersion);
}

// Common header of any generation 2 frame (no crypto). False for gen1
// frames, other protocol versions and frames too short for their type.
struct FrameInfo
{
    FrameType type;
    uint8_t version;
    uint32_t devId;
    uint16_t fcnt16;      // UP / DOWN
    uint8_t fctrl;        // UP / DOWN
    JoinAccKind joinKind; // JOIN_ACC
};
bool peekFrame(const uint8_t *frame, size_t len, FrameInfo &info);

// --- UP / DOWN -------------------------------------------------------------

// Build an UP or DOWN frame. confFcnt = FCnt32 of the acknowledged uplink
// (DOWN; ignored for UP). Returns the frame length, 0 on error.
size_t buildData(AesEncryptFn aes, const KeyPair &keys, FrameType type, uint32_t devId, uint32_t fcnt32,
                 uint32_t confFcnt, uint8_t fctrl, const uint8_t *payload, size_t payloadLen, uint8_t *out,
                 size_t outCap);

// Verify the MIC with the expanded counter and decrypt. payloadOut must hold
// len - kDataOverhead bytes. Nothing is written when the MIC fails.
// Mandatory order on reception:
//   1. peekFrame()  - type, DevID, FCnt16 (no crypto)
//   2. expandFcnt() - FCnt32 against the last accepted counter (ep_counter.h)
//   3. openData()   - MIC check + decryption with that FCnt32
//   4. only now store FCnt32 as the last accepted counter
// Storing the counter before the MIC verified lets a forged frame block the
// session; skipping step 2 accepts replays.
bool openData(AesEncryptFn aes, const KeyPair &keys, const uint8_t *frame, size_t len, uint32_t fcnt32,
              uint32_t confFcnt, uint8_t *payloadOut, size_t &payloadLen);

// --- JOIN_REQ --------------------------------------------------------------

struct JoinRequest
{
    uint32_t devId;
    uint32_t devNonce;
    uint16_t hwType;
    uint8_t fwMajor, fwMinor, fwPatch;
    uint8_t caps;
    uint8_t configVersion;
};

size_t buildJoinRequest(AesEncryptFn aes, const KeyPair &joinKeys, const JoinRequest &req, uint8_t *out,
                        size_t outCap);
// Fields only (fixed length, magic, MHDR) - for the "waiting to be added" list
bool parseJoinRequest(const uint8_t *frame, size_t len, JoinRequest &req);
bool verifyJoinRequest(AesEncryptFn aes, const KeyPair &joinKeys, const uint8_t *frame, size_t len);

// --- JOIN_ACC --------------------------------------------------------------

size_t buildJoinAccept(AesEncryptFn aes, const KeyPair &joinKeys, uint32_t devId, uint32_t devNonce,
                       uint32_t gwNonce, uint8_t configVersion, const uint8_t *tlv, size_t tlvLen, uint8_t *out,
                       size_t outCap);
// tlvOut must hold len - kJoinAcceptMinLen bytes
bool openJoinAccept(AesEncryptFn aes, const KeyPair &joinKeys, const uint8_t *frame, size_t len, uint32_t devNonce,
                    uint32_t &gwNonce, uint8_t &configVersion, uint8_t *tlvOut, size_t &tlvLen);

size_t buildRejoin(AesEncryptFn aes, const KeyPair &joinKeys, uint32_t devId, uint16_t upFcnt16,
                   const uint8_t upMic[kMicLen], uint8_t *out, size_t outCap);
bool verifyRejoin(AesEncryptFn aes, const KeyPair &joinKeys, const uint8_t *frame, size_t len, uint16_t upFcnt16,
                  const uint8_t upMic[kMicLen]);

// --- building blocks (exposed for tests) -------------------------------------

void makeCtrBlock(Dir dir, CryptoCode code, uint32_t devId, uint32_t counter, uint8_t block[kAesBlock]);
void computeMic(AesEncryptFn aes, const uint8_t micKey[kKeyLen], Dir dir, CryptoCode code, uint32_t devId,
                uint32_t counter, uint32_t bind, const uint8_t *msg, size_t msgLen, uint8_t mic[kMicLen]);

} // namespace ep
