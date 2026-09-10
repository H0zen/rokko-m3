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

#ifndef MANGOS_MOVEMENT_BRIDGE_H
#define MANGOS_MOVEMENT_BRIDGE_H

#include "wire/MovementStatus.h"

class MovementInfo;

/**
 * The game's movement record and the wire's status, mapped both ways (P2-B).
 * MovementInfo stays what every handler and every Unit stores; the wire codec
 * is what reads and writes it. Every field of either side has a row in the
 * other, so a status decoded from a packet, mapped into the record and mapped
 * back is the same status -- which is what the bridge witness in WireParity
 * checks on every packet when the shadow is on.
 */
namespace Movement
{
    Wire::MovementStatus ToWire(MovementInfo const& record);
    void FromWire(Wire::MovementStatus const& status, MovementInfo& record);
}

#endif
