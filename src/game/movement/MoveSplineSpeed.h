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

#ifndef MANGOSSERVER_MOVESPLINESPEED_H
#define MANGOSSERVER_MOVESPLINESPEED_H

// UnitMoveType is declared in Unit.h as an UNSCOPED enum with no fixed underlying type
// (Unit.h: `enum UnitMoveType { MOVE_WALK = 0, ... }`), and such an enum cannot be
// opaque-declared: an opaque declaration must carry an enum-base, and a definition without
// one then contradicts it. So this header includes Unit.h rather than forward-declaring the
// type. It is the only way the declaration below compiles.
#include "Unit.h"

namespace Movement
{
    /// Which of a unit's nine speeds the movement flags of the moment name. Defined in
    /// MoveSplineInit.cpp, beside the launch that applies it, and declared here so every other
    /// reader -- the movement kernel's shell, which reports a unit's current speed to the
    /// natives -- shares that one answer instead of copying it. MoveSplineInit.h cannot carry
    /// this declaration: it does not include Unit.h, and must not start to.
    UnitMoveType SelectSpeedType(uint32 moveFlags);
}

#endif
