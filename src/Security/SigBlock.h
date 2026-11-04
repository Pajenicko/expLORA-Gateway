/**
 * expLORA Gateway
 *
 * SigBlock - the 128-byte signature block appended to a gateway or sensor
 * firmware file (layout, parsing, the binding checks) and the two stream
 * helpers that separate it from the payload while the file is received.
 * Pure C++ (no Arduino, no mbedtls), unit-tested natively in
 * test/test_sigblock; the signature itself is checked by SigVerify.
 *
 * Block (all integers little-endian):
 *
 *   off   0  magic "EXSG"          4 B
 *         4  block version (1)     u16
 *         6  kind                  u16  1 = gateway, 2 = sensor
 *         8  target                u32  gateway board (1 Pro, 2 Lite) or sensor HW type
 *        12  firmware version      u32  maj << 16 | min << 8 | pat
 *        16  payload length        u32  bytes of the file before the block
 *        20  key id                u32  first 4 B of SHA-256(public key, 65 B
 *                                       uncompressed point), big-endian value
 *        24  SHA-256(payload)      32 B
 *        56  signature r || s      64 B ECDSA P-256 over SHA-256(bytes 0..55)
 *       120  reserved              8 B  must be 0
 *
 * The same layout is written by tools/release/explora_sigblock.py.
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
#include <cstring>

namespace SigBlock
{
constexpr size_t SIZE = 128;
constexpr size_t SIGNED_BYTES = 56; // the signature covers SHA-256 of bytes 0..55
constexpr size_t HASH_OFFSET = 24;
constexpr size_t SIG_OFFSET = 56;
constexpr size_t RESERVED_OFFSET = 120;
constexpr uint16_t BLOCK_VERSION = 1;
constexpr uint8_t MAGIC[4] = {'E', 'X', 'S', 'G'};

enum class Kind : uint16_t
{
    Gateway = 1,
    Sensor = 2,
};

// Gateway board ids (target of a gateway block)
constexpr uint32_t TARGET_PRO = 1;
constexpr uint32_t TARGET_LITE = 2;

struct Block
{
    uint16_t version = 0; // block version
    Kind kind = Kind::Gateway;
    uint32_t target = 0;
    uint32_t fwVersion = 0;
    uint32_t payloadLength = 0;
    uint32_t keyId = 0;
    uint8_t payloadHash[32] = {};
    uint8_t signature[64] = {}; // r || s, 32 B each, big-endian
    uint8_t raw[SIZE] = {};     // the block as received (raw[0..55] is signed)
};

enum class Format : uint8_t
{
    Ok,
    NoMagic,
    BadVersion,  // block version other than 1
    BadKind,     // neither gateway nor sensor
    BadReserved, // reserved bytes not zero
    BadKeyId,    // key id 0 (marks an empty key slot; explora_keygen never makes such a key)
};

inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

inline bool hasMagic(const uint8_t *b)
{
    return memcmp(b, MAGIC, sizeof(MAGIC)) == 0;
}

// Parses 128 bytes; out is filled only on Ok
inline Format parse(const uint8_t b[SIZE], Block &out)
{
    if (!hasMagic(b))
    {
        return Format::NoMagic;
    }
    if (rd16(b + 4) != BLOCK_VERSION)
    {
        return Format::BadVersion;
    }
    uint16_t kind = rd16(b + 6);
    if (kind != (uint16_t)Kind::Gateway && kind != (uint16_t)Kind::Sensor)
    {
        return Format::BadKind;
    }
    for (size_t i = RESERVED_OFFSET; i < SIZE; i++)
    {
        if (b[i] != 0)
        {
            return Format::BadReserved;
        }
    }
    if (rd32(b + 20) == 0)
    {
        return Format::BadKeyId;
    }
    out.version = BLOCK_VERSION;
    out.kind = (Kind)kind;
    out.target = rd32(b + 8);
    out.fwVersion = rd32(b + 12);
    out.payloadLength = rd32(b + 16);
    out.keyId = rd32(b + 20);
    memcpy(out.payloadHash, b + HASH_OFFSET, 32);
    memcpy(out.signature, b + SIG_OFFSET, 64);
    memcpy(out.raw, b, SIZE);
    return Format::Ok;
}

// Writes the block layout (signature as given; reserved zero). Used by the
// tests; the release tool has its own copy in Python
inline void encode(const Block &in, uint8_t out[SIZE])
{
    memset(out, 0, SIZE);
    memcpy(out, MAGIC, sizeof(MAGIC));
    wr16(out + 4, BLOCK_VERSION);
    wr16(out + 6, (uint16_t)in.kind);
    wr32(out + 8, in.target);
    wr32(out + 12, in.fwVersion);
    wr32(out + 16, in.payloadLength);
    wr32(out + 20, in.keyId);
    memcpy(out + HASH_OFFSET, in.payloadHash, 32);
    memcpy(out + SIG_OFFSET, in.signature, 64);
}

// "maj.min.pat" (each 0..255), optionally followed by a '-' or '+' suffix
// that is ignored ("2.0.0-dev" = 2.0.0). Anything else is refused
inline bool parseVersion(const char *s, uint32_t &out)
{
    uint32_t part[3] = {};
    for (int i = 0; i < 3; i++)
    {
        if (*s < '0' || *s > '9')
        {
            return false;
        }
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || v > 255)
            {
                return false;
            }
        }
        part[i] = v;
        if (i < 2 && *s++ != '.')
        {
            return false;
        }
    }
    if (*s != '\0' && *s != '-' && *s != '+')
    {
        return false;
    }
    out = part[0] << 16 | part[1] << 8 | part[2];
    return true;
}

// What the receiver expects from the block, all taken from the file itself
// (the board this firmware runs on, the image's own version, the received
// length and hash)
// A default-constructed Expect matches nothing: kind 0 is no valid kind and a
// null hash makes SigVerify::verify() return Error.
struct Expect
{
    Kind kind = static_cast<Kind>(0);
    uint32_t target = 0;
    uint32_t fwVersion = 0;
    uint32_t payloadLength = 0;
    const uint8_t *payloadHash = nullptr; // 32 B
};

enum class Binding : uint8_t
{
    Ok,
    WrongKind,
    WrongTarget,
    WrongVersion,
    WrongLength,
    WrongHash,
};

inline Binding checkBinding(const Block &b, const Expect &e)
{
    if (b.kind != e.kind)
    {
        return Binding::WrongKind;
    }
    if (b.target != e.target)
    {
        return Binding::WrongTarget;
    }
    if (b.fwVersion != e.fwVersion)
    {
        return Binding::WrongVersion;
    }
    if (b.payloadLength != e.payloadLength)
    {
        return Binding::WrongLength;
    }
    if (memcmp(b.payloadHash, e.payloadHash, 32) != 0)
    {
        return Binding::WrongHash;
    }
    return Binding::Ok;
}

// How a received file ended
enum class Tail : uint8_t
{
    Unsigned, // no block: the whole file is the payload
    Signed,   // a well-formed block follows the payload (not yet verified)
    BadBlock, // something follows the payload that is not a valid block
};

// Gateway stream: the total length is not known up front. Every byte is
// passed to sink.update() except the last SIZE bytes seen so far, which
// stay here. At the end finish() decides: a well-formed block whose payload
// length equals the bytes before it -> Signed (the sink saw exactly the
// payload); no magic there, or fewer than SIZE bytes in all -> Unsigned and
// the held bytes go to the sink too (it saw the whole file); magic present
// but the block is malformed or its length disagrees -> BadBlock.
// Sink: any type with update(const uint8_t *, size_t). A download that is
// shorter or longer than announced (Content-Length, manifest size) must be
// refused by the caller before the tail is classified: a cut signed file
// otherwise ends Unsigned
class TailRing
{
public:
    template <class Sink> void write(const uint8_t *data, size_t len, Sink &sink)
    {
        if (finished_)
        {
            return;
        }
        total_ += len;
        if (held_ + len <= SIZE)
        {
            memcpy(buf_ + held_, data, len);
            held_ += len;
            return;
        }
        // The oldest held_ + len - SIZE bytes leave: first from buf_, then data
        size_t out = held_ + len - SIZE;
        size_t fromBuf = out < held_ ? out : held_;
        sink.update(buf_, fromBuf);
        size_t fromData = out - fromBuf;
        if (fromData)
        {
            sink.update(data, fromData);
        }
        // Keep the rest of buf_ followed by the rest of data (exactly SIZE)
        memmove(buf_, buf_ + fromBuf, held_ - fromBuf);
        size_t keep = held_ - fromBuf;
        memcpy(buf_ + keep, data + fromData, len - fromData);
        held_ = SIZE;
    }

    // Idempotent: a second call returns the first result (block and format
    // again) without passing anything to the sink. write() after finish()
    // is ignored (the stream is over)
    template <class Sink> Tail finish(Sink &sink, Block &block, Format &format)
    {
        if (finished_)
        {
            format = format_;
            if (tail_ != Tail::Unsigned && format_ == Format::Ok)
            {
                parse(buf_, block);
            }
            return tail_;
        }
        finished_ = true;
        format_ = Format::NoMagic;
        if (held_ == SIZE && hasMagic(buf_))
        {
            format_ = parse(buf_, block);
            tail_ = format_ == Format::Ok && (uint64_t)block.payloadLength + SIZE == total_ ? Tail::Signed
                                                                                          : Tail::BadBlock;
            format = format_;
            return tail_;
        }
        sink.update(buf_, held_);
        held_ = 0;
        tail_ = Tail::Unsigned;
        format = format_;
        return tail_;
    }

    uint64_t total() const { return total_; }
    bool finished() const { return finished_; }

private:
    uint8_t buf_[SIZE] = {};
    size_t held_ = 0;
    uint64_t total_ = 0;
    bool finished_ = false;
    Tail tail_ = Tail::Unsigned;
    Format format_ = Format::NoMagic;
};

// Sensor stream: the payload length (image + trailer, size word at 0x20 +
// 32) is read from the image's first bytes BEFORE this object is created -
// it is fixed for the whole stream, there is no way to set it later.
// write() returns how many of the given bytes still belong to the payload
// (always a prefix); the rest are kept here. At the end: nothing after the
// payload -> Unsigned; exactly SIZE bytes forming a well-formed block ->
// Signed; anything else (shorter, longer, no magic, malformed) -> BadBlock.
// A stream shorter than the payload also ends Unsigned: the caller must
// refuse it by its length (payloadComplete()) before looking at the tail
class SensorTail
{
public:
    explicit SensorTail(uint32_t payloadLength) : payload_(payloadLength) {}

    size_t write(const uint8_t *data, size_t len)
    {
        size_t take = 0;
        if (seen_ < payload_)
        {
            uint64_t left = payload_ - seen_;
            take = len < left ? len : (size_t)left;
        }
        seen_ += len;
        size_t rest = len - take;
        size_t room = SIZE - held_;
        size_t keep = rest < room ? rest : room;
        memcpy(buf_ + held_, data + take, keep);
        held_ += keep;
        extra_ += rest; // all bytes after the payload, kept or not
        return take;
    }

    Tail finish(Block &block, Format &format) const
    {
        format = Format::NoMagic;
        if (extra_ == 0)
        {
            return Tail::Unsigned;
        }
        if (extra_ != SIZE)
        {
            return Tail::BadBlock;
        }
        format = parse(buf_, block);
        if (format != Format::Ok || block.payloadLength != payload_)
        {
            return Tail::BadBlock;
        }
        return Tail::Signed;
    }

    uint64_t extra() const { return extra_; }
    // All payload bytes arrived
    bool payloadComplete() const { return seen_ >= payload_; }

private:
    uint64_t payload_ = 0;
    uint64_t seen_ = 0;
    uint64_t extra_ = 0;
    uint8_t buf_[SIZE] = {};
    size_t held_ = 0;
};
} // namespace SigBlock
