/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
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
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "wire/KnockBackCodec.h"
#include "wire/MonsterMoveCodec.h"
#include "wire/MovementFamilies.h"
#include "wire/MovementSequences.h"
#include "wire/MoverCodec.h"
#include "wire/TeleportCodec.h"

#include "Opcodes.h"
#include "Utilities/ByteBuffer.h"

namespace
{
    struct FamilyEntry
    {
        uint16       opcode;
        Wire::Family family;
    };

    // The eight opcodes the generated registry cannot describe, in a fixed
    // order: FamilyIndex/FamilyOpcodeAt expose that order to callers (the
    // capture and the tests) so a report can name "family opcode 3" stably.
    const FamilyEntry kFamilies[] =
    {
        { SMSG_MOVE_KNOCK_BACK,          Wire::Family::KnockBack },
        { SMSG_MOVE_TELEPORT,            Wire::Family::Teleport },
        { CMSG_MOVE_TELEPORT_ACK,        Wire::Family::TeleportAck },
        { SMSG_MOVE_SET_ACTIVE_MOVER,    Wire::Family::ActiveMover },
        { CMSG_SET_ACTIVE_MOVER,         Wire::Family::ActiveMover },
        { SMSG_CLIENT_CONTROL_UPDATE,    Wire::Family::ControlUpdate },
        { SMSG_MONSTER_MOVE,             Wire::Family::MonsterMove },
        { SMSG_MONSTER_MOVE_TRANSPORT,   Wire::Family::MonsterMove },
    };

    const size_t kFamilyCount = sizeof(kFamilies) / sizeof(kFamilies[0]);
}

namespace Wire
{
    Family FamilyFor(uint16 opcode)
    {
        for (size_t i = 0; i < kFamilyCount; ++i)
        {
            if (kFamilies[i].opcode == opcode) { return kFamilies[i].family; }
        }
        return Family::None;
    }

    bool IsFamily(uint16 opcode)
    {
        return FamilyFor(opcode) != Family::None;
    }

    bool IsKnown(uint16 opcode)
    {
        return IsPacketLayout(opcode) || IsFamily(opcode);
    }

    char const* FamilyName(Family family)
    {
        switch (family)
        {
            case Family::None:          return "none";
            case Family::KnockBack:     return "knock back";
            case Family::Teleport:      return "teleport";
            case Family::TeleportAck:   return "teleport ack";
            case Family::ActiveMover:   return "active mover";
            case Family::ControlUpdate: return "control update";
            case Family::MonsterMove:   return "monster move";
        }
        return "?";
    }

    int FamilyIndex(uint16 opcode)
    {
        for (size_t i = 0; i < kFamilyCount; ++i)
        {
            if (kFamilies[i].opcode == opcode) { return int(i); }
        }
        return -1;
    }

    size_t FamilyCount()
    {
        return kFamilyCount;
    }

    uint16 FamilyOpcodeAt(size_t index)
    {
        // 0 is not an opcode, so an out-of-range index answers "no opcode"
        // instead of reading past the table: a caller that walks one row too far
        // gets an answer it can test rather than whatever follows the array.
        if (index >= kFamilyCount) { return 0; }
        return kFamilies[index].opcode;
    }

    Verdict Judge(uint16 opcode, ByteBuffer const& packet, bool pendingWriteBits)
    {
        Verdict v;
        ByteBuffer copy(packet);
        if (pendingWriteBits) { copy.FlushBits(); }
        copy.rpos(0);
        copy.ResetBitReader();
        ByteBuffer again;
        if (IsPacketLayout(opcode))
        {
            MovementStatus status;
            v.decoded = DecodeWhole(copy, SequenceFor(opcode), status, v.result, false);
            if (v.decoded) { Encode(again, SequenceFor(opcode), status); }
        }
        else
        {
            switch (FamilyFor(opcode))
            {
                case Family::ActiveMover:
                {
                    ActiveMover value;
                    v.result = DecodeActiveMover(copy, opcode, value);
                    if (v.result.ok()) { EncodeActiveMover(again, opcode, value); }
                    break;
                }
                case Family::ControlUpdate:
                {
                    ControlUpdate value;
                    v.result = DecodeControlUpdate(copy, value);
                    if (v.result.ok()) { EncodeControlUpdate(again, value); }
                    break;
                }
                case Family::KnockBack:
                {
                    KnockBack value;
                    v.result = DecodeKnockBack(copy, value);
                    if (v.result.ok()) { EncodeKnockBack(again, value); }
                    break;
                }
                case Family::Teleport:
                {
                    Teleport value;
                    v.result = DecodeTeleport(copy, value);
                    if (v.result.ok()) { EncodeTeleport(again, value); }
                    break;
                }
                case Family::TeleportAck:
                {
                    TeleportAck value;
                    v.result = DecodeTeleportAck(copy, value);
                    if (v.result.ok()) { EncodeTeleportAck(again, value); }
                    break;
                }
                case Family::MonsterMove:
                {
                    MonsterMove value;
                    v.result = DecodeMonsterMove(copy, opcode, value);
                    if (v.result.ok()) { EncodeMonsterMove(again, opcode, value); }
                    break;
                }
                default:
                    v.result.error = DecodeError::NoSequence;
                    return v;
            }
            if (v.result.ok() && v.result.consumed != copy.size())
            {
                v.result.error = DecodeError::LeftBytes;
            }
            v.decoded = v.result.ok();
        }
        if (v.decoded)
        {
            again.FlushBits();
            v.reencoded = again.size();
            // Walk both to the first byte that differs. When one is a prefix of
            // the other the walk stops at the shorter length, which is the offset
            // a reader wants: that is where the two stop agreeing.
            const size_t shorter = again.size() < copy.size() ? again.size() : copy.size();
            size_t at = 0;
            while (at < shorter && again.contents()[at] == copy.contents()[at]) { ++at; }
            v.exact = again.size() == copy.size() && at == shorter;
            v.firstDifference = v.exact ? -1 : long(at);
        }
        return v;
    }
}
