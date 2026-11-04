/**
 * expLORA Gateway
 *
 * Signed sensor images in the OTA image store (M4c S2): an uploaded file is
 * the sensor image (image + 32-byte trailer, the size word at 0x20 gives its
 * length) optionally followed by the 128-byte signature block
 * (Security/SigBlock.h). Splitter passes only the image bytes on - they are
 * stored, CRC-checked (OtaImage::Streamer) and hashed - and keeps whatever
 * follows them in RAM (SigBlock::SensorTail), so the stored .bin stays
 * byte-identical to the unsigned image (sensor, bootloader, job and store
 * recovery see no difference). classify() then decides, failing closed:
 *
 *   Unsigned  nothing after the image
 *   Verified  a block bound to this image (kind sensor, target = trailer HW
 *             type, version = trailer firmware, length = image length,
 *             hash = SHA-256 of the stored bytes) and signed by a key of
 *             the table: signedKey = its key id
 *   NoKey     a well-formed block bound to this image whose key id is not
 *             in the table: stored as unsigned with a note (counts as
 *             unsigned everywhere). Key id 0 is no key id (it marks an
 *             empty slot): such a block is malformed (refused)
 *   refused   anything else after the image (not exactly 128 bytes, no
 *             magic, a malformed block, a binding mismatch, a bad signature
 *             with a known key, a failed check) - never stored, also not for
 *             a logged-in admin; `severe` marks a block that is present but
 *             wrong (logged as an error)
 *
 * Pure C++ apart from the hash and SigVerify (mbedtls; the host build in the
 * native tests, test/test_ota_image).
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
#include <cstdio>
#include <cstring>
#include <optional>

#include "../Security/SigBlock.h"
#include "../Security/SigVerify.h"
#include "OtaImage.h"

namespace OtaImageSigned
{
// Up to the end of the size word every byte belongs to the image (a valid
// image is at least ep::ota::kMinImageLen = 512 bytes long)
constexpr uint32_t HEAD = OtaImage::SIZE_WORD_OFFSET + 4;
static_assert(HEAD < ep::ota::kMinImageLen, "the size word must lie inside the smallest image");

// Problems seen while the file streams in (the upload stops at once)
enum class Early : uint8_t
{
    None,
    Length,   // the size word gives an image shorter than the minimum
    TooLarge, // the size word gives an image larger than the slot
    TailLong, // more than 128 bytes after the image
};

inline const char *earlyText(Early e)
{
    switch (e)
    {
    case Early::None:
        return "ok";
    case Early::Length:
        return OtaImage::errorText(OtaImage::Error::Length);
    case Early::TooLarge:
        return OtaImage::errorText(OtaImage::Error::TooLarge);
    case Early::TailLong:
        return "more than 128 bytes follow the image - not a signature block";
    }
    return "?";
}

// Hash: update(const uint8_t *, size_t) (Security/Sha256.h on the gateway)
template <class Hash> class Splitter
{
public:
    // payload: how many bytes at the start of data belong to the image
    // (always a prefix: the caller stores exactly these); the image bytes
    // also go to the CRC pass and to hash
    Early feed(const uint8_t *data, size_t len, size_t &payload, Hash &hash)
    {
        payload = 0;
        size_t i = 0;
        if (!tail_)
        {
            const size_t need = HEAD - streamer_.total();
            i = len < need ? len : need;
            streamer_.feed(data, i);
            hash.update(data, i);
            payload = i;
            if (streamer_.total() < HEAD)
            {
                return Early::None;
            }
            const uint64_t len64 = (uint64_t)streamer_.sizeWord() + OtaImage::TRAILER_SIZE;
            if (len64 < ep::ota::kMinImageLen)
            {
                return Early::Length;
            }
            if (len64 > OtaImage::MAX_LEN)
            {
                return Early::TooLarge;
            }
            tail_.emplace((uint32_t)len64);
            // The HEAD bytes seen so far are image bytes: counted, nothing kept
            static const uint8_t seen[HEAD] = {};
            tail_->write(seen, HEAD);
        }
        const size_t take = tail_->write(data + i, len - i);
        streamer_.feed(data + i, take);
        hash.update(data + i, take);
        payload = i + take;
        return tail_->extra() > SigBlock::SIZE ? Early::TailLong : Early::None;
    }

    const OtaImage::Streamer &streamer() const { return streamer_; }
    // Bytes after the image (0 before the size word is known)
    uint64_t extra() const { return tail_ ? tail_->extra() : 0; }
    // How the file ended (valid once the image is complete)
    SigBlock::Tail tail(SigBlock::Block &block, SigBlock::Format &format) const
    {
        if (!tail_)
        {
            format = SigBlock::Format::NoMagic;
            return SigBlock::Tail::Unsigned;
        }
        return tail_->finish(block, format);
    }

private:
    OtaImage::Streamer streamer_;
    std::optional<SigBlock::SensorTail> tail_;
};

enum class Class : uint8_t
{
    Refused = 0, // never stored (a value-initialised result is no acceptance)
    Unsigned,
    Verified,
    NoKey,
};

struct Outcome
{
    Class cls = Class::Refused;
    bool severe = false;        // refused for a present but wrong block (logged as an error)
    OtaImage::Error image = OtaImage::Error::None;
    OtaImage::Trailer trailer;
    uint32_t keyId = 0;         // Verified: the key; NoKey: the unknown key id
    SigVerify::Result result = SigVerify::Result::Error;
    char text[112] = "";        // the refusal reason, or how it was stored
};

// The finished upload: the image (OtaImage::validate), then the tail. The
// hash is finished here (finish(uint8_t[32]) -> false when the engine failed)
template <class Hash>
Outcome classify(const Splitter<Hash> &s, Hash &hash, const ReleaseKeys::Key *keys = ReleaseKeys::KEYS,
                 size_t keyCount = ReleaseKeys::SLOTS)
{
    Outcome o;
    o.image = OtaImage::validate(s.streamer(), o.trailer);
    if (o.image != OtaImage::Error::None)
    {
        snprintf(o.text, sizeof(o.text), "%s", OtaImage::errorText(o.image));
        return o;
    }
    SigBlock::Block block;
    SigBlock::Format format = SigBlock::Format::NoMagic;
    const SigBlock::Tail tail = s.tail(block, format);
    if (tail == SigBlock::Tail::Unsigned)
    {
        o.cls = Class::Unsigned;
        snprintf(o.text, sizeof(o.text), "unsigned");
        return o;
    }
    if (tail == SigBlock::Tail::BadBlock)
    {
        if (s.extra() != SigBlock::SIZE)
        {
            snprintf(o.text, sizeof(o.text), "%lu bytes follow the image - not a 128-byte signature block",
                     (unsigned long)s.extra());
        }
        else if (format == SigBlock::Format::NoMagic)
        {
            snprintf(o.text, sizeof(o.text), "the 128 bytes after the image are not a signature block (no magic)");
        }
        else if (format == SigBlock::Format::Ok)
        {
            // Well-formed, but its payload length is not this image's length
            o.severe = true;
            o.result = SigVerify::Result::WrongLength;
            snprintf(o.text, sizeof(o.text), "%s", SigVerify::resultText(o.result));
        }
        else
        {
            o.severe = true;
            o.result = SigVerify::Result::BadBlock;
            snprintf(o.text, sizeof(o.text), "%s", SigVerify::resultText(o.result));
        }
        return o;
    }
    uint8_t digest[32];
    if (!hash.finish(digest))
    {
        snprintf(o.text, sizeof(o.text), "signature check failed (hash engine)");
        return o;
    }
    SigBlock::Expect e;
    e.kind = SigBlock::Kind::Sensor;
    e.target = o.trailer.hwType;
    e.fwVersion = o.trailer.fw;
    e.payloadLength = s.streamer().total();
    e.payloadHash = digest;
    o.result = SigVerify::verify(block, e, keys, keyCount);
    switch (o.result)
    {
    case SigVerify::Result::Ok:
        o.cls = Class::Verified;
        o.keyId = block.keyId;
        snprintf(o.text, sizeof(o.text), "verified (key %08lx)", (unsigned long)o.keyId);
        return o;
    case SigVerify::Result::NoKey:
        o.cls = Class::NoKey;
        o.keyId = block.keyId;
        snprintf(o.text, sizeof(o.text), "unsigned (signed with an unknown key %08lx)", (unsigned long)o.keyId);
        return o;
    case SigVerify::Result::Error:
        // No memory / engine failure: refused (fail closed), not a bad file
        snprintf(o.text, sizeof(o.text), "%s", SigVerify::resultText(o.result));
        return o;
    default:
        o.severe = true;
        snprintf(o.text, sizeof(o.text), "%s", SigVerify::resultText(o.result));
        return o;
    }
}

// Whether a key id is in the table (a description stored by a firmware with
// another table - a rotated-out or bench key - no longer counts as verified)
inline bool knownKey(uint32_t keyId, const ReleaseKeys::Key *keys = ReleaseKeys::KEYS,
                     size_t keyCount = ReleaseKeys::SLOTS)
{
    for (size_t i = 0; keyId != 0 && i < keyCount; i++)
    {
        if (keys[i].keyId == keyId)
        {
            return true;
        }
    }
    return false;
}

// A stored description as this firmware sees it: "signed" by a key it does
// not carry becomes "unknown key" (unsigned)
template <class M> void applyKeyTable(M &m, const ReleaseKeys::Key *keys = ReleaseKeys::KEYS,
                                      size_t keyCount = ReleaseKeys::SLOTS)
{
    if (m.signedKey && !knownKey(m.signedKey, keys, keyCount))
    {
        m.unknownKey = m.signedKey;
        m.signedKey = 0;
    }
}

// The class of an accepted (stored) image from its description
inline Class classOf(uint32_t signedKey, uint32_t unknownKey)
{
    return signedKey ? Class::Verified : unknownKey ? Class::NoKey : Class::Unsigned;
}

// May an upload with this outcome be stored? A verified image always, an
// unsigned one (also with an unknown key) only while "Signed images only"
// is off; a refused one never
inline bool storable(Class c, bool signedOnly)
{
    return c == Class::Verified || (!signedOnly && (c == Class::Unsigned || c == Class::NoKey));
}

// A job start with a stored image: a verified image starts as it is; an
// unsigned one (also with an unknown key) needs the admin's confirmation,
// or cannot start at all while "Signed images only" is on (it may have been
// stored before the option was switched on)
enum class StartRule : uint8_t
{
    Ok,
    Confirm,
    Block,
};

inline StartRule startRule(uint32_t signedKey, bool signedOnly)
{
    if (signedKey)
    {
        return StartRule::Ok;
    }
    return signedOnly ? StartRule::Block : StartRule::Confirm;
}

// An existing job under "Signed images only" (a resume, a job restored at
// boot, the option switched on): it may go on only when the stored image of
// its HW type is the job's image (same CRC and length) and verified. A
// missing or other image counts as unsigned (fail closed). true: hold it
inline bool jobHeld(bool signedOnly, bool stored, uint32_t storedCrc, uint32_t storedLen, uint32_t signedKey,
                    uint32_t jobCrc, uint32_t jobLen)
{
    return signedOnly && !(stored && storedCrc == jobCrc && storedLen == jobLen && signedKey != 0);
}

// The signature state of a stored image for the UI ("verified (key
// 1a2b3c4d)", "unsigned", "unsigned (unknown key 1a2b3c4d)"); out >= 40 B
inline void stateText(uint32_t signedKey, uint32_t unknownKey, char *out, size_t cap)
{
    if (signedKey)
    {
        snprintf(out, cap, "verified (key %08lx)", (unsigned long)signedKey);
    }
    else if (unknownKey)
    {
        snprintf(out, cap, "unsigned (unknown key %08lx)", (unsigned long)unknownKey);
    }
    else
    {
        snprintf(out, cap, "unsigned");
    }
}
} // namespace OtaImageSigned
