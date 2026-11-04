/**
 * expLORA Gateway
 *
 * OTA image store: what to do at boot with a commit cut by a reset (pure,
 * native tests in test/test_ota_store). uploadCommit writes the new
 * description to /ota/meta.tmp, renames upload.tmp to hw<HHHH>.bin and only
 * then meta.tmp to hw<HHHH>.json; a meta.tmp found at boot means the reset
 * came somewhere in between. checkPair() then sorts out each stored pair.
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdint.h>

namespace OtaCommitRecovery
{
enum class Action : unsigned char
{
    DropTmp,     // the old pair stays (meta.tmp removed)
    FinishTmp,   // the .bin is the new image: rename meta.tmp to .json
    DropAll      // the .bin matches neither description: meta.tmp and the pair go
};

// uploadTmp: upload.tmp still exists, tmpValid: meta.tmp parsed - either way
// the .bin rename did not happen (meta.tmp is closed before it, LittleFS keeps
// the order; upload.tmp is then removed as an unfinished upload).
// matchesNew / matchesOld: the .bin against meta.tmp / the old .json (length
// and CRC as at the job start), called only when needed (a CRC pass each).
template <typename New, typename Old> Action decide(bool uploadTmp, bool tmpValid, New matchesNew, Old matchesOld)
{
    if (uploadTmp || !tmpValid)
    {
        return Action::DropTmp;
    }
    if (matchesNew())
    {
        return Action::FinishTmp;
    }
    return matchesOld() ? Action::DropTmp : Action::DropAll;
}

enum class Pair : unsigned char
{
    Keep,
    BadName,     // not hw<HHHH> with upper case hex
    Orphan,      // the .bin or a readable .json missing
    OtherHw,     // the .json describes another HW type
    WrongLength  // the .bin is not as long as the .json says
};

// Boot check of a stored pair (removeOrphans), no CRC pass: a .bin of the
// right length that differs from its .json (an older firmware cut during its
// direct .json write after its single rename) is caught only by the CRC check
// at the job start.
// binLen 0: the .bin is missing.
inline Pair checkPair(bool canonical, uint32_t binLen, bool jsonReadable, uint16_t jsonHw, uint16_t nameHw,
                      uint32_t jsonLen)
{
    if (!canonical)
    {
        return Pair::BadName;
    }
    if (binLen == 0 || !jsonReadable)
    {
        return Pair::Orphan;
    }
    if (jsonHw != nameHw)
    {
        return Pair::OtherHw;
    }
    return binLen == jsonLen ? Pair::Keep : Pair::WrongLength;
}
} // namespace OtaCommitRecovery
