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

#ifndef MANGOS_WIRE_MOVERCODEC_H
#define MANGOS_WIRE_MOVERCODEC_H

#include "Platform/Define.h"
#include "wire/MovementCodec.h"

class ByteBuffer;

namespace Wire
{
    /// SMSG_MOVE_SET_ACTIVE_MOVER and its CMSG_SET_ACTIVE_MOVER ack: the same
    /// guid, in two different masked-guid orders per opcode.
    ///
    /// Provenance: CMSG_SET_ACTIVE_MOVER -- the tree's own handler
    /// (HandleSetActiveMoverOpcode), CPP's SetActiveMover::Read, and the real-client
    /// families golden. SMSG_MOVE_SET_ACTIVE_MOVER -- CPP's MoveSetActiveMover::Write
    /// and the client's reader sub_140374180 through lift_client_reader.py, which
    /// agreed with it exactly; no writer in this tree sends one yet, so it has no
    /// golden line.
    struct ActiveMover
    {
        uint64 guid = 0;
    };

    void EncodeActiveMover(ByteBuffer& out, uint16 opcode, ActiveMover const& v);   // opcode picks the order
    DecodeResult DecodeActiveMover(ByteBuffer& in, uint16 opcode, ActiveMover& out);

    /// SMSG_CLIENT_CONTROL_UPDATE: a packed guid and a byte.
    ///
    /// Provenance: the tree's own writer (Player::SetClientControl) and the client's
    /// handler sub_1402431B0, which read the same two fields in the same order.
    /// None in the golden: this tree sends none at login.
    struct ControlUpdate
    {
        uint64 guid = 0;
        uint8  allowMove = 0;
    };

    void EncodeControlUpdate(ByteBuffer& out, ControlUpdate const& v);
    DecodeResult DecodeControlUpdate(ByteBuffer& in, ControlUpdate& out);
}

#endif
