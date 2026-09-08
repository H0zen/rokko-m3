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

#ifndef MANGOS_WIRE_KNOCKBACKCODEC_H
#define MANGOS_WIRE_KNOCKBACKCODEC_H

#include "Platform/Define.h"
#include "wire/MovementCodec.h"

class ByteBuffer;

namespace Wire
{
    /// SMSG_MOVE_KNOCK_BACK (WorldSession::SendKnockBack): a masked guid split
    /// across the two direction/speed halves.
    ///
    /// Provenance: the tree's own writer (WorldSession::SendKnockBack), CPP's
    /// MoveKnockBack::Write, the client's reader sub_140354A80 through
    /// lift_client_reader.py, and the real-client families golden x3 -- all four
    /// agreeing on the same mask, byte and float order.
    struct KnockBack
    {
        uint64 guid = 0;
        uint32 counter = 0;
        float  directionX = 0.0f;
        float  directionY = 0.0f;
        float  horizontal = 0.0f;
        float  vertical = 0.0f;
    };

    void EncodeKnockBack(ByteBuffer& out, KnockBack const& v);
    DecodeResult DecodeKnockBack(ByteBuffer& in, KnockBack& out);
}

#endif
