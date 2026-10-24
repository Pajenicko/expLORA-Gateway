/**
 * expLORA Gateway
 *
 * GatewayImage - the rules of a gateway firmware install (M4c S3), pure C++
 * apart from the hash and SigVerify (mbedtls; the host build in the native
 * tests, test/test_gateway_image):
 *
 * - Stream: every byte of the received file goes through a
 *   SigBlock::TailRing into the hash (the last 128 bytes are held back) and,
 *   unchanged, into the OTA slot (the block lands after the ESP image and
 *   its appended SHA-256, where the bootloader ignores it). finish()
 *   classifies the file like a sensor image (OtaImageSigned):
 *
 *     Unsigned  the last 128 bytes are no block: the hash covers every byte
 *     Verified  a well-formed block bound to this file (kind gateway, target
 *               = this board, length = the bytes before it, hash) and signed
 *               by a key of the table
 *     NoKey     the same with a key id that is not in the table: unsigned
 *     refused   a block that is present but malformed, bound to another
 *               file / board / kind, a bad signature with a known key, a
 *               failed check (fail closed), or a download whose length
 *               differs from the announced one (checked BEFORE the tail: a
 *               cut signed file must not end as "unsigned")
 *
 *   The block's version is not compared with the image: esp_app_desc_t
 *   holds the library builder's id in these builds. The release tool binds
 *   the version to the image when it signs (the image's version marker),
 *   and the signature covers the version field, so a Verified block's
 *   version is trusted as it is.
 * - activation(): Verified activates; unsigned / unknown key needs the
 *   admin's confirmation, or is refused while "Signed images only" is on;
 *   anything refused never activates. A Verified image whose signed version
 *   is lower than the running firmware's (a downgrade) is never activated
 *   silently: it takes the confirmation path too ("Signed images only" does
 *   not refuse it - it is signed); the same version is a reinstall and
 *   activates without a confirmation. If the running firmware's version
 *   text is not maj.min.pat (e.g. a pre-release suffix) nothing can be
 *   compared and every Verified image takes the confirmation path (fails
 *   closed). (Automatic installs, a later stage,
 *   refuse downgrades.)
 * - Confirmation: bound to the written image (its SHA-256 and length), to
 *   the login session that wrote it and valid for 10 minutes; one
 *   confirmation activates exactly that image (checkConfirm()).
 * - leftImageErased(): an image written and left waiting (never activated)
 *   is erased after a restart unless it is the running image or the one the
 *   bootloader starts, the running image is still being validated, or
 *   otadata marks the slot VALID (a stale record never hits the rollback
 *   target).
 * - Validation of a new firmware (rollback): marked valid after 120 s of a
 *   running loop() with the web server up and the liveness checks healthy
 *   (Validation); a reset before that lets the bootloader roll back.
 * - bootOutcome(): what the recorded activation (/update/state.json) means
 *   after a boot.
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../Security/SigBlock.h"
#include "../Security/SigVerify.h"

namespace GatewayImage
{
// esp_image_header_t: magic at 0, chip id (u16) at 12, 24 bytes in all
constexpr uint8_t IMAGE_MAGIC = 0xE9;
constexpr size_t CHIP_ID_OFFSET = 12;
constexpr size_t HEADER_SIZE = 24;

enum class HeadError : uint8_t
{
    None,
    Short,     // fewer than HEADER_SIZE bytes
    NotImage,  // no image magic
    OtherChip, // built for another chip (Lite = ESP32-S3, Pro = ESP32)
};

// The image header against this chip (the first HEADER_SIZE bytes)
inline HeadError checkHead(const uint8_t *b, size_t len, uint16_t chipId)
{
    if (len < HEADER_SIZE)
    {
        return HeadError::Short;
    }
    if (b[0] != IMAGE_MAGIC)
    {
        return HeadError::NotImage;
    }
    if ((uint16_t)(b[CHIP_ID_OFFSET] | b[CHIP_ID_OFFSET + 1] << 8) != chipId)
    {
        return HeadError::OtherChip;
    }
    return HeadError::None;
}

enum class Class : uint8_t
{
    Refused = 0, // a value-initialised outcome never activates
    Unsigned,
    Verified,
    NoKey,
};

inline const char *className(Class c)
{
    switch (c)
    {
    case Class::Unsigned:
        return "unsigned";
    case Class::Verified:
        return "verified";
    case Class::NoKey:
        return "nokey";
    default:
        return "refused";
    }
}

struct Outcome
{
    Class cls = Class::Refused;
    bool severe = false;      // refused for a present but wrong block (logged as an error)
    uint32_t keyId = 0;       // Verified: the key; NoKey: the unknown key id
    uint32_t fwVersion = 0;   // Verified / NoKey: the block's version (maj << 16 | min << 8 | pat)
    uint64_t length = 0;      // bytes received (= written to the slot)
    uint8_t digest[32] = {};  // SHA-256 of the payload (Unsigned: of every byte)
    SigVerify::Result result = SigVerify::Result::Error;
    char text[112] = "";      // the refusal reason, or how the file was classified
};

// Hash: update(const uint8_t *, size_t) / finish(uint8_t[32]) -> false when
// the engine failed (Security/Sha256.h on the gateway)
template <class Hash> class Stream
{
public:
    void feed(const uint8_t *data, size_t len, Hash &hash) { ring_.write(data, len, hash); }
    uint64_t total() const { return ring_.total(); }

    // The end of the file. announced: the length the sender declared
    // (Content-Length; 0 = not known, e.g. a multipart upload ended by its
    // boundary). target: this board (SigBlock::TARGET_PRO / TARGET_LITE).
    // Safe to call twice (the same outcome; the hash is finished once)
    Outcome finish(Hash &hash, uint64_t announced, uint32_t target, const ReleaseKeys::Key *keys = ReleaseKeys::KEYS,
                   size_t keyCount = ReleaseKeys::SLOTS)
    {
        if (done_)
        {
            return outcome_;
        }
        done_ = true;
        Outcome &o = outcome_;
        o.length = ring_.total();
        if (announced != 0 && o.length != announced)
        {
            snprintf(o.text, sizeof(o.text), "received %llu of %llu bytes - the download is incomplete or too long",
                     (unsigned long long)o.length, (unsigned long long)announced);
            return o;
        }
        if (o.length < HEADER_SIZE)
        {
            snprintf(o.text, sizeof(o.text), "file too short for a firmware image");
            return o;
        }
        SigBlock::Block block;
        SigBlock::Format format = SigBlock::Format::NoMagic;
        const SigBlock::Tail tail = ring_.finish(hash, block, format);
        if (tail == SigBlock::Tail::BadBlock)
        {
            o.severe = true;
            o.result = format == SigBlock::Format::Ok ? SigVerify::Result::WrongLength : SigVerify::Result::BadBlock;
            snprintf(o.text, sizeof(o.text), "%s", SigVerify::resultText(o.result));
            return o;
        }
        if (!hash.finish(o.digest))
        {
            snprintf(o.text, sizeof(o.text), "the file could not be checked (hash engine)");
            return o;
        }
        if (tail == SigBlock::Tail::Unsigned)
        {
            o.cls = Class::Unsigned;
            snprintf(o.text, sizeof(o.text), "unsigned");
            return o;
        }
        SigBlock::Expect e;
        e.kind = SigBlock::Kind::Gateway;
        e.target = target;
        // Trusted: covered by the signature (see the top of this file)
        e.fwVersion = block.fwVersion;
        e.payloadLength = (uint32_t)(o.length - SigBlock::SIZE);
        e.payloadHash = o.digest;
        o.result = SigVerify::verify(block, e, keys, keyCount);
        switch (o.result)
        {
        case SigVerify::Result::Ok:
            o.cls = Class::Verified;
            o.keyId = block.keyId;
            o.fwVersion = block.fwVersion;
            snprintf(o.text, sizeof(o.text), "verified (key %08lx)", (unsigned long)o.keyId);
            return o;
        case SigVerify::Result::NoKey:
            o.cls = Class::NoKey;
            o.keyId = block.keyId;
            o.fwVersion = block.fwVersion;
            snprintf(o.text, sizeof(o.text), "unsigned (signed with an unknown key %08lx)", (unsigned long)o.keyId);
            return o;
        case SigVerify::Result::Error:
            snprintf(o.text, sizeof(o.text), "%s", SigVerify::resultText(o.result)); // fail closed
            return o;
        default:
            o.severe = true;
            snprintf(o.text, sizeof(o.text), "%s", SigVerify::resultText(o.result));
            return o;
        }
    }

private:
    SigBlock::TailRing ring_;
    Outcome outcome_;
    bool done_ = false;
};

// "2.0.0" -> 0x020000 (each part 0..255, as SigBlock's version field);
// false on anything else
inline bool parseVersion(const char *s, uint32_t &out)
{
    out = 0;
    if (!s)
    {
        return false;
    }
    uint32_t v = 0;
    for (int part = 0; part < 3; part++)
    {
        if (*s < '0' || *s > '9')
        {
            return false;
        }
        uint32_t n = 0;
        while (*s >= '0' && *s <= '9')
        {
            n = n * 10 + (uint32_t)(*s - '0');
            if (n > 255)
            {
                return false;
            }
            s++;
        }
        v = (v << 8) | n;
        if (part < 2)
        {
            if (*s != '.')
            {
                return false;
            }
            s++;
        }
    }
    if (*s != '\0')
    {
        return false;
    }
    out = v;
    return true;
}

// The running firmware's version when its text is not maj.min.pat (e.g. a
// pre-release suffix): nothing can be compared
constexpr uint32_t VERSION_UNKNOWN = 0xFFFFFFFFu;

// parseVersion(text), or VERSION_UNKNOWN
inline uint32_t runningVersionOf(const char *text)
{
    uint32_t v = 0;
    return parseVersion(text, v) ? v : VERSION_UNKNOWN;
}

// A Verified image that needs the admin's confirmation because of its
// version: older than the running firmware. Fails closed: with the running
// version not known (VERSION_UNKNOWN) every Verified image counts (no
// maj.min.pat block version equals a text that does not parse)
inline bool isDowngrade(Class c, uint32_t fwVersion, uint32_t runningVersion)
{
    return c == Class::Verified && (runningVersion == VERSION_UNKNOWN || fwVersion < runningVersion);
}

// What happens with a written image
enum class Activation : uint8_t
{
    Refuse = 0,
    Activate,
    Confirm, // unsigned / unknown key / an older verified version: only with the admin's confirmation
};

// fwVersion: the block's version (Verified / NoKey); runningVersion: the
// running firmware's (runningVersionOf(FIRMWARE_VERSION))
inline Activation activation(Class c, bool signedOnly, uint32_t fwVersion, uint32_t runningVersion)
{
    if (c == Class::Verified)
    {
        return isDowngrade(c, fwVersion, runningVersion) ? Activation::Confirm : Activation::Activate;
    }
    if (c == Class::Unsigned || c == Class::NoKey)
    {
        return signedOnly ? Activation::Refuse : Activation::Confirm;
    }
    return Activation::Refuse;
}

// --- confirmation of an unsigned image ------------------------------------------------

constexpr uint32_t CONFIRM_VALID_MS = 10UL * 60 * 1000;

struct Pending
{
    bool valid = false;
    uint8_t digest[32] = {};
    uint64_t length = 0;
    uint64_t session = 0; // login session that wrote the image (0 = none: never confirmable)
    uint32_t createdMs = 0;
    bool signedImage = false; // a Verified older version: "Signed images only" does not refuse it
};

enum class Confirm : uint8_t
{
    Ok,
    None,         // no image waits for a confirmation
    Expired,      // older than 10 minutes
    OtherImage,   // the confirmation names another image
    OtherSession, // another login session (or none)
    SignedOnly,   // "Signed images only" was switched on meanwhile
};

inline const char *confirmText(Confirm c)
{
    switch (c)
    {
    case Confirm::Ok:
        return "ok";
    case Confirm::None:
        return "no firmware image waits for a confirmation";
    case Confirm::Expired:
        return "the confirmation expired (10 minutes after the image was written) - install the image again";
    case Confirm::OtherImage:
        return "the confirmation is for another image than the one written";
    case Confirm::OtherSession:
        return "only the login session that installed the image can confirm it";
    case Confirm::SignedOnly:
        return "\"Signed images only\" is on - unsigned images are refused";
    }
    return "?";
}

inline uint32_t confirmLeftMs(const Pending &p, uint32_t nowMs)
{
    const uint32_t age = nowMs - p.createdMs;
    return age >= CONFIRM_VALID_MS ? 0 : CONFIRM_VALID_MS - age;
}

inline Confirm checkConfirm(const Pending &p, const uint8_t digest[32], uint64_t session, uint32_t nowMs,
                            bool signedOnly)
{
    if (!p.valid)
    {
        return Confirm::None;
    }
    if (confirmLeftMs(p, nowMs) == 0)
    {
        return Confirm::Expired;
    }
    if (memcmp(p.digest, digest, 32) != 0)
    {
        return Confirm::OtherImage;
    }
    if (p.session == 0 || p.session != session)
    {
        return Confirm::OtherSession;
    }
    if (signedOnly && !p.signedImage)
    {
        return Confirm::SignedOnly;
    }
    return Confirm::Ok;
}

// 64 hex digits -> 32 bytes (either case); false on anything else
inline bool parseDigest(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64)
    {
        return false;
    }
    for (int i = 0; i < 64; i++)
    {
        const char c = hex[i];
        uint8_t v;
        if (c >= '0' && c <= '9')
        {
            v = (uint8_t)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            v = (uint8_t)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            v = (uint8_t)(c - 'A' + 10);
        }
        else
        {
            return false;
        }
        if (i % 2 == 0)
        {
            out[i / 2] = (uint8_t)(v << 4);
        }
        else
        {
            out[i / 2] |= v;
        }
    }
    return true;
}

inline void digestHex(const uint8_t d[32], char out[65])
{
    static const char *const H = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
    {
        out[2 * i] = H[d[i] >> 4];
        out[2 * i + 1] = H[d[i] & 15];
    }
    out[64] = '\0';
}

// --- validation of a new firmware (rollback) ------------------------------------------

constexpr uint32_t VALIDATE_AFTER_MS = 120000;

// Fed from every loop() pass while the running image waits for its
// validation (bootloader state PENDING_VERIFY). The 120 s count only while
// the web server is up and the liveness checks are healthy; any unhealthy
// pass starts them again. step() returns true once: mark the image valid now
class Validation
{
public:
    bool step(uint32_t nowMs, bool webUp, bool healthy)
    {
        if (done_)
        {
            return false;
        }
        if (!webUp || !healthy)
        {
            since_ = 0;
            counting_ = false;
            return false;
        }
        if (!counting_)
        {
            counting_ = true;
            since_ = nowMs;
            return false;
        }
        if (nowMs - since_ >= VALIDATE_AFTER_MS)
        {
            done_ = true;
            return true;
        }
        return false;
    }
    // Seconds until the validation (the full 120 s while not counting)
    uint32_t leftS(uint32_t nowMs) const
    {
        if (done_)
        {
            return 0;
        }
        if (!counting_)
        {
            return VALIDATE_AFTER_MS / 1000;
        }
        const uint32_t age = nowMs - since_;
        return age >= VALIDATE_AFTER_MS ? 0 : (VALIDATE_AFTER_MS - age + 999) / 1000;
    }
    bool done() const { return done_; }
    // A deliberate restart from the web UI marks the image valid first (the
    // admin reached the UI, the criterion this waits for)
    void markDone() { done_ = true; }

private:
    uint32_t since_ = 0;
    bool counting_ = false;
    bool done_ = false;
};

// --- the recorded activation after a boot ----------------------------------------------

enum class Boot : uint8_t
{
    Nothing,    // no activation recorded
    Running,    // the activated image runs (validated now or later)
    RolledBack, // the bootloader marked it invalid / aborted and went back
    NotRunning, // another image runs and the recorded one was not rolled back
                // (e.g. replaced by an install of another firmware)
};

// An image written and left waiting for a confirmation when the gateway
// restarted (recorded in /update/state.json "written"): its slot's first
// sector is erased so it can never boot - never the running slot, never the
// slot otadata starts (that one is no abandoned image; it was activated).
// The record may be stale (a failed save of the state file), so it is not
// acted on while the running image is still PENDING_VERIFY (the other slot
// is its rollback target; installs are refused then, so no image can have
// been left waiting) nor for a slot otadata marks VALID (an image that ran
// and was validated; esp_ota_begin() clears the otadata entry of the slot
// it writes, so a never-activated image has none)
inline bool leftImageErased(bool recorded, bool isRunning, bool isBootTarget, bool runningPendingVerify,
                            bool slotValid)
{
    return recorded && !isRunning && !isBootTarget && !runningPendingVerify && !slotValid;
}

// pendingRecorded: an activation is recorded; runningIsPending: the running
// partition is the recorded one; pendingInvalid: the bootloader state of the
// recorded partition is ABORTED or INVALID
inline Boot bootOutcome(bool pendingRecorded, bool runningIsPending, bool pendingInvalid)
{
    if (!pendingRecorded)
    {
        return Boot::Nothing;
    }
    if (runningIsPending)
    {
        return Boot::Running;
    }
    return pendingInvalid ? Boot::RolledBack : Boot::NotRunning;
}
} // namespace GatewayImage
