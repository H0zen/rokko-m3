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

#ifndef MANGOS_LOADTEST_CONTROL_HPP
#define MANGOS_LOADTEST_CONTROL_HPP

#include "Platform/Define.h"
#include "WorldPacket.h"

namespace Wire
{
    struct MovementStatus;
}

namespace loadtest
{
    /// SMSG_CLIENT_CONTROL_UPDATE: a packed guid and one byte, 1 when this client moves
    /// the unit from now on. False for a packet too short to carry them.
    bool ReadControlUpdate(WorldPacket& packet, uint64& guid, uint8& allow);

    /// CMSG_SET_ACTIVE_MOVER for `guid`, the real client's answer to a grant, in the
    /// codec's order (Wire::EncodeActiveMover).
    WorldPacket MakeSelectActiveMover(uint64 guid);

    /// CMSG_MOVE_NOT_ACTIVE_MOVER carrying `status`, the real client's answer for the
    /// unit it stops moving: on a grant of another unit (naming the one it leaves) and
    /// on a revoke of its own control (naming itself), in the registry layout.
    WorldPacket MakeNotActiveMover(Wire::MovementStatus const& status);
}

#endif
