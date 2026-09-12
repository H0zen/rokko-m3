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

#include "Control.hpp"
#include "Opcodes.h"
#include "wire/MovementSequences.h"
#include "wire/MoverCodec.h"

namespace loadtest
{
    // Through the codec now (both sides): the layout is the same packed guid and byte
    // Control.hpp's comment names, but a decode failure -- too short, or bytes left
    // over -- is Wire::DecodeControlUpdate's call, not this function's own size check.
    bool ReadControlUpdate(WorldPacket& packet, uint64& guid, uint8& allow)
    {
        packet.rpos(0);
        Wire::ControlUpdate v;
        Wire::DecodeResult const r = Wire::DecodeControlUpdate(packet, v);
        if (!r.ok() || r.consumed != packet.size())
        {
            return false;
        }
        guid = v.guid;
        allow = v.allowMove;
        return true;
    }

    WorldPacket MakeSelectActiveMover(uint64 guid)
    {
        WorldPacket packet(CMSG_SET_ACTIVE_MOVER, 9);
        Wire::ActiveMover m;
        m.guid = guid;
        Wire::EncodeActiveMover(packet, CMSG_SET_ACTIVE_MOVER, m);
        return packet;
    }

    WorldPacket MakeNotActiveMover(Wire::MovementStatus const& status)
    {
        WorldPacket packet(CMSG_MOVE_NOT_ACTIVE_MOVER, 64);
        Wire::Encode(packet, Wire::SequenceFor(CMSG_MOVE_NOT_ACTIVE_MOVER), status);
        return packet;
    }
}
