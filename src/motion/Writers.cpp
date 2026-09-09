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

#include "Writers.h"
#include "PacketMatrix.h"
#include "wire/KnockBackCodec.h"
#include "wire/MovementCodec.h"
#include "wire/MovementSequences.h"
#include "wire/TeleportCodec.h"

namespace Motion
{
    namespace
    {
        bool EncodeRegistry(WorldPacket& out, uint16 opcode, Wire::MovementStatus const& status)
        {
            Wire::Sequence const sequence = Wire::SequenceFor(opcode);
            if (!sequence) { return false; }
            out.Initialize(opcode, 64);
            Wire::Encode(out, sequence, status);
            return true;
        }

        Wire::MovementStatus Bare(uint64 guid, uint32 counter, Change const& change)
        {
            Wire::MovementStatus s;
            s.guid = guid;
            s.counter = counter;
            s.value = change.value;
            s.twoBits = change.reason;
            return s;
        }
    }

    bool BuildMover(WorldPacket& out, uint64 guid, uint32 counter, Change const& change)
    {
        MatrixRow const* row = RowFor(change.type, change.apply);
        if (!row || !row->mover) { return false; }
        switch (change.type)
        {
            case ChangeType::KnockBack:
            {
                Wire::KnockBack k;
                k.guid = guid;
                k.counter = counter;
                k.directionX = change.knockBack.directionX;
                k.directionY = change.knockBack.directionY;
                k.horizontal = change.knockBack.horizontal;
                k.vertical = change.knockBack.vertical;
                out.Initialize(row->mover, 64);
                Wire::EncodeKnockBack(out, k);
                return true;
            }
            case ChangeType::Teleport:
            {
                Wire::Teleport t;
                t.guid = guid;
                t.counter = counter;
                t.pos = change.teleport.pos;
                t.hasTransport = change.teleport.hasTransport;
                t.transportGuid = change.teleport.transportGuid;
                out.Initialize(row->mover, 64);
                Wire::EncodeTeleport(out, t);
                return true;
            }
            default:
                return EncodeRegistry(out, row->mover, Bare(guid, counter, change));
        }
    }

    bool BuildSpline(WorldPacket& out, uint64 guid, Change const& change)
    {
        MatrixRow const* row = RowFor(change.type, change.apply);
        if (!row || !row->spline) { return false; }
        return EncodeRegistry(out, row->spline, Bare(guid, 0, change));
    }

    bool BuildObserver(WorldPacket& out, uint64 guid, uint32 counter, Change const& change, Wire::MovementStatus const& status)
    {
        MatrixRow const* row = RowFor(change.type, change.apply);
        if (!row || !row->observer) { return false; }
        if (row->observer == row->mover) { return BuildMover(out, guid, counter, change); }   // root: the same packet again
        Wire::MovementStatus s = status;
        s.guid = guid;
        s.counter = counter;
        s.value = change.value;
        s.twoBits = change.reason;
        return EncodeRegistry(out, row->observer, s);
    }
}
