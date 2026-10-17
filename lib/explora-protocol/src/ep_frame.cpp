/**
 * expLORA protocol generation 2 - frame codec
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ep_frame.h"
#include "ep_bytes.h"

#include <cstring>

namespace ep
{

void makeCtrBlock(Dir dir, CryptoCode code, uint32_t devId, uint32_t counter, uint8_t block[kAesBlock])
{
    memset(block, 0, kAesBlock);
    block[0] = 0x01;
    block[1] = (uint8_t)dir;
    block[2] = (uint8_t)code;
    putU32(block + 3, devId);
    putU32(block + 7, counter);
    block[15] = 1; // first block i = 1
}

void computeMic(AesEncryptFn aes, const uint8_t micKey[kKeyLen], Dir dir, CryptoCode code, uint32_t devId,
                uint32_t counter, uint32_t bind, const uint8_t *msg, size_t msgLen, uint8_t mic[kMicLen])
{
    uint8_t b0[kAesBlock];
    b0[0] = 0x49;
    b0[1] = (uint8_t)dir;
    b0[2] = (uint8_t)code;
    putU32(b0 + 3, devId);
    putU32(b0 + 7, counter);
    putU32(b0 + 11, bind);
    b0[15] = (uint8_t)msgLen;
    uint8_t full[kAesBlock];
    Cmac c(aes, micKey);
    c.update(b0, sizeof(b0));
    c.update(msg, msgLen);
    c.finish(full);
    memcpy(mic, full, kMicLen);
}

static bool micMatches(AesEncryptFn aes, const uint8_t micKey[kKeyLen], Dir dir, CryptoCode code, uint32_t devId,
                       uint32_t counter, uint32_t bind, const uint8_t *frame, size_t len)
{
    uint8_t mic[kMicLen];
    computeMic(aes, micKey, dir, code, devId, counter, bind, frame, len - kMicLen, mic);
    return equalConstTime(mic, frame + len - kMicLen, kMicLen);
}

bool peekFrame(const uint8_t *frame, size_t len, FrameInfo &info)
{
    if (frame == nullptr || len < 1 + 4 + kMicLen || len > kMaxFrame || (frame[0] & kMarkerMask) != kMarker)
    {
        return false;
    }
    info.type = (FrameType)((frame[0] >> 3) & 0x03);
    info.version = frame[0] & 0x07;
    if (info.version != kProtocolVersion)
    {
        return false;
    }
    info.devId = getU32(frame + 1);
    info.fcnt16 = 0;
    info.fctrl = 0;
    info.joinKind = JoinAccKind::Accept;
    switch (info.type)
    {
    case FrameType::Up:
    case FrameType::Down:
        if (len < kDataOverhead || len - kDataOverhead > kMaxPayload)
        {
            return false;
        }
        info.fcnt16 = getU16(frame + 5);
        info.fctrl = frame[7];
        return true;
    case FrameType::JoinReq:
        return len == kJoinReqLen && frame[5] == kJoinReqMagic0 && frame[6] == kJoinReqMagic1;
    case FrameType::JoinAcc:
        if (len < kJoinAccHeaderLen + kMicLen)
        {
            return false;
        }
        info.joinKind = (JoinAccKind)frame[5];
        if (info.joinKind == JoinAccKind::Accept)
        {
            return len >= kJoinAcceptMinLen;
        }
        return info.joinKind == JoinAccKind::Rejoin && len == kRejoinLen;
    }
    return false;
}

// --- UP / DOWN -------------------------------------------------------------

static Dir dirOf(FrameType type)
{
    return type == FrameType::Down || type == FrameType::JoinAcc ? Dir::Downlink : Dir::Uplink;
}

size_t buildData(AesEncryptFn aes, const KeyPair &keys, FrameType type, uint32_t devId, uint32_t fcnt32,
                 uint32_t confFcnt, uint8_t fctrl, const uint8_t *payload, size_t payloadLen, uint8_t *out,
                 size_t outCap)
{
    if ((type != FrameType::Up && type != FrameType::Down) || payloadLen > kMaxPayload ||
        outCap < payloadLen + kDataOverhead || (payloadLen > 0 && payload == nullptr))
    {
        return 0;
    }
    const Dir dir = dirOf(type);
    const CryptoCode code = type == FrameType::Up ? CryptoCode::Up : CryptoCode::Down;
    const uint32_t bind = type == FrameType::Down ? confFcnt : 0;

    out[0] = makeMhdr(type);
    putU32(out + 1, devId);
    putU16(out + 5, (uint16_t)fcnt32);
    out[7] = fctrl;
    memmove(out + kDataHeaderLen, payload, payloadLen);
    uint8_t a[kAesBlock];
    makeCtrBlock(dir, code, devId, fcnt32, a);
    ctrXor(aes, keys.enc, a, out + kDataHeaderLen, payloadLen);
    size_t len = kDataHeaderLen + payloadLen;
    computeMic(aes, keys.mic, dir, code, devId, fcnt32, bind, out, len, out + len);
    return len + kMicLen;
}

bool openData(AesEncryptFn aes, const KeyPair &keys, FrameType expected, const uint8_t *frame, size_t len,
              uint32_t fcnt32, uint32_t confFcnt, uint8_t *payloadOut, size_t &payloadLen)
{
    FrameInfo info;
    if ((expected != FrameType::Up && expected != FrameType::Down) || !peekFrame(frame, len, info) ||
        info.type != expected || (uint16_t)fcnt32 != info.fcnt16)
    {
        return false;
    }
    const Dir dir = dirOf(info.type);
    const CryptoCode code = info.type == FrameType::Up ? CryptoCode::Up : CryptoCode::Down;
    const uint32_t bind = info.type == FrameType::Down ? confFcnt : 0;
    if (!micMatches(aes, keys.mic, dir, code, info.devId, fcnt32, bind, frame, len))
    {
        return false;
    }
    payloadLen = len - kDataOverhead;
    memcpy(payloadOut, frame + kDataHeaderLen, payloadLen);
    uint8_t a[kAesBlock];
    makeCtrBlock(dir, code, info.devId, fcnt32, a);
    ctrXor(aes, keys.enc, a, payloadOut, payloadLen);
    return true;
}

// --- JOIN_REQ --------------------------------------------------------------

size_t buildJoinRequest(AesEncryptFn aes, const KeyPair &joinKeys, const JoinRequest &req, uint8_t *out,
                        size_t outCap)
{
    if (outCap < kJoinReqLen)
    {
        return 0;
    }
    out[0] = makeMhdr(FrameType::JoinReq);
    putU32(out + 1, req.devId);
    out[5] = kJoinReqMagic0;
    out[6] = kJoinReqMagic1;
    putU32(out + 7, req.devNonce);
    putU16(out + 11, req.hwType);
    out[13] = req.fwMajor;
    out[14] = req.fwMinor;
    out[15] = req.fwPatch;
    out[16] = req.caps;
    out[17] = req.configVersion & kFctrlCfgMask;
    computeMic(aes, joinKeys.mic, Dir::Uplink, CryptoCode::JoinReq, req.devId, req.devNonce, 0, out,
               kJoinReqLen - kMicLen, out + kJoinReqLen - kMicLen);
    return kJoinReqLen;
}

bool parseJoinRequest(const uint8_t *frame, size_t len, JoinRequest &req)
{
    FrameInfo info;
    if (!peekFrame(frame, len, info) || info.type != FrameType::JoinReq)
    {
        return false;
    }
    req.devId = info.devId;
    req.devNonce = getU32(frame + 7);
    req.hwType = getU16(frame + 11);
    req.fwMajor = frame[13];
    req.fwMinor = frame[14];
    req.fwPatch = frame[15];
    req.caps = frame[16];
    req.configVersion = frame[17] & kFctrlCfgMask;
    return true;
}

bool verifyJoinRequest(AesEncryptFn aes, const KeyPair &joinKeys, const uint8_t *frame, size_t len)
{
    JoinRequest req;
    if (!parseJoinRequest(frame, len, req))
    {
        return false;
    }
    return micMatches(aes, joinKeys.mic, Dir::Uplink, CryptoCode::JoinReq, req.devId, req.devNonce, 0, frame, len);
}

// --- JOIN_ACC --------------------------------------------------------------

size_t buildJoinAccept(AesEncryptFn aes, const KeyPair &joinKeys, uint32_t devId, uint32_t devNonce,
                       uint32_t gwNonce, uint8_t configVersion, const uint8_t *tlv, size_t tlvLen, uint8_t *out,
                       size_t outCap)
{
    // Bound tlvLen before adding, so a huge value cannot wrap the sum
    if (tlvLen > kMaxFrame - kJoinAcceptMinLen || (tlvLen > 0 && tlv == nullptr))
    {
        return 0;
    }
    const size_t len = kJoinAcceptMinLen + tlvLen;
    if (outCap < len)
    {
        return 0;
    }
    out[0] = makeMhdr(FrameType::JoinAcc);
    putU32(out + 1, devId);
    out[5] = (uint8_t)JoinAccKind::Accept;
    uint8_t *body = out + kJoinAccHeaderLen;
    putU32(body, gwNonce);
    body[4] = configVersion & kFctrlCfgMask;
    memmove(body + 5, tlv, tlvLen);
    uint8_t a[kAesBlock];
    makeCtrBlock(Dir::Downlink, CryptoCode::JoinAccept, devId, devNonce, a);
    ctrXor(aes, joinKeys.enc, a, body, 5 + tlvLen);
    computeMic(aes, joinKeys.mic, Dir::Downlink, CryptoCode::JoinAccept, devId, devNonce, 0, out, len - kMicLen,
               out + len - kMicLen);
    return len;
}

bool openJoinAccept(AesEncryptFn aes, const KeyPair &joinKeys, const uint8_t *frame, size_t len, uint32_t devNonce,
                    uint32_t &gwNonce, uint8_t &configVersion, uint8_t *tlvOut, size_t &tlvLen)
{
    FrameInfo info;
    if (!peekFrame(frame, len, info) || info.type != FrameType::JoinAcc || info.joinKind != JoinAccKind::Accept)
    {
        return false;
    }
    if (!micMatches(aes, joinKeys.mic, Dir::Downlink, CryptoCode::JoinAccept, info.devId, devNonce, 0, frame, len))
    {
        return false;
    }
    const size_t bodyLen = len - kJoinAccHeaderLen - kMicLen;
    uint8_t tmp[kMaxFrame];
    memcpy(tmp, frame + kJoinAccHeaderLen, bodyLen);
    uint8_t a[kAesBlock];
    makeCtrBlock(Dir::Downlink, CryptoCode::JoinAccept, info.devId, devNonce, a);
    ctrXor(aes, joinKeys.enc, a, tmp, bodyLen);
    tlvLen = bodyLen - 5;
    gwNonce = getU32(tmp);
    configVersion = tmp[4] & kFctrlCfgMask;
    memcpy(tlvOut, tmp + 5, tlvLen);
    wipe(tmp, sizeof(tmp));
    return true;
}

size_t buildRejoin(AesEncryptFn aes, const KeyPair &joinKeys, uint32_t devId, uint16_t upFcnt16,
                   const uint8_t upMic[kMicLen], uint8_t *out, size_t outCap)
{
    if (outCap < kRejoinLen)
    {
        return 0;
    }
    out[0] = makeMhdr(FrameType::JoinAcc);
    putU32(out + 1, devId);
    out[5] = (uint8_t)JoinAccKind::Rejoin;
    computeMic(aes, joinKeys.mic, Dir::Downlink, CryptoCode::Rejoin, devId, upFcnt16, getU32(upMic), out,
               kJoinAccHeaderLen, out + kJoinAccHeaderLen);
    return kRejoinLen;
}

bool verifyRejoin(AesEncryptFn aes, const KeyPair &joinKeys, const uint8_t *frame, size_t len, uint16_t upFcnt16,
                  const uint8_t upMic[kMicLen])
{
    FrameInfo info;
    if (!peekFrame(frame, len, info) || info.type != FrameType::JoinAcc || info.joinKind != JoinAccKind::Rejoin)
    {
        return false;
    }
    return micMatches(aes, joinKeys.mic, Dir::Downlink, CryptoCode::Rejoin, info.devId, upFcnt16, getU32(upMic),
                      frame, len);
}

} // namespace ep
