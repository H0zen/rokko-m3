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

#ifndef MANGOS_MOTION_WRITERS_H
#define MANGOS_MOTION_WRITERS_H

#include "Change.h"
#include "WorldPacket.h"
#include "wire/MovementStatus.h"

/**
 * A change to a packet (design v2 §7): the matrix says which opcode, the
 * codecs say which bytes. The mover form carries the counter the client
 * echoes; the spline form is a server-driven unit's broadcast; the observer
 * form is sent once the ack confirms and, for the updates that embed a
 * movement status, carries the mover's confirmed status. Every builder
 * returns false and leaves `out` alone when the matrix has no cell for it.
 */
namespace Motion
{
    bool BuildMover(WorldPacket& out, uint64 guid, uint32 counter, Change const& change);
    bool BuildSpline(WorldPacket& out, uint64 guid, Change const& change);
    bool BuildObserver(WorldPacket& out, uint64 guid, uint32 counter, Change const& change, Wire::MovementStatus const& status);
}

#endif
