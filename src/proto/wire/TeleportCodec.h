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

#ifndef MANGOS_WIRE_TELEPORTCODEC_H
#define MANGOS_WIRE_TELEPORTCODEC_H

#include "Platform/Define.h"
#include "wire/MovementCodec.h"
#include "wire/MovementStatus.h"

class ByteBuffer;

namespace Wire
{
    /// SMSG_MOVE_TELEPORT (Player::SendTeleportPacket): the tree's writer plus
    /// CPP's vehicle branch, which the client reads but the tree does not yet
    /// write.
    ///
    /// Provenance: the tree's own writer (Player::SendTeleportPacket), CPP's
    /// MoveTeleport::Write, the client's reader sub_140363890 -- which settled the
    /// vehicle seat as one byte, not four -- and the real-client families golden x3.
    /// The vehicle and transport branches have no witness in this tree yet: nothing
    /// here writes one, so only the client's reader and CPP speak for them.
    struct Teleport
    {
        uint64 guid = 0;
        uint32 counter = 0;
        Vec4   pos;                        // pos.o is the facing
        bool   hasTransport = false;
        uint64 transportGuid = 0;
        bool   hasVehicle = false;
        bool   vehicleExitVoluntary = false;
        bool   vehicleExitTeleport = false;
        uint8  vehicleSeat = 0;   ///< one byte: the client's reader takes one (P1-C task 5)
    };

    void EncodeTeleport(ByteBuffer& out, Teleport const& v);
    DecodeResult DecodeTeleport(ByteBuffer& in, Teleport& out);

    /// CMSG_MOVE_TELEPORT_ACK (HandleMoveTeleportAckOpcode): counter, time,
    /// then a masked guid.
    ///
    /// Provenance: the tree's own handler (WorldSession::HandleMoveTeleportAckOpcode),
    /// CPP's MoveTeleportAck::Read, and the real-client families golden x3 -- the
    /// client built those three, so they are the order's ground truth.
    struct TeleportAck
    {
        uint32 counter = 0;
        uint32 time = 0;
        uint64 guid = 0;
    };

    void EncodeTeleportAck(ByteBuffer& out, TeleportAck const& v);
    DecodeResult DecodeTeleportAck(ByteBuffer& in, TeleportAck& out);
}

#endif
