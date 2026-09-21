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

#pragma once

// PUTTING A LEG ON THE WIRE, AND THE ONE PLACE THAT DOES IT.
//
// Move::MoveWriter decides what the packet must contain and refuses when it cannot make a
// good one. This turns its answer into bytes through Wire::MonsterMoveCodec, which is the
// encoder with provenance -- verified against two hundred and fifty captured families from
// a real client -- and hands it to everyone who can see the mover.
//
// The refusal matters more than the send. A spline the client will not accept does not
// leave the mover standing: it TELEPORTS it to the destination. So a refused leg must never
// become a packet, and the caller is told so rather than finding out on someone's screen.

#include "Move/MoveWriter.h"
#include "Move/Movement.h"

class Unit;

class MoveSend
{
    public:
        /// Encode and broadcast. Answers false when the writer refused, in which case
        /// nothing was sent and the mover has not been told to go anywhere.
        static bool Leg(Unit& unit, const Move::Written& written,
                        const Move::Facing& facing, uint32 splineId = 0);

        /// Stop where the mover stands. A separate form of the same packet: it ends right
        /// after the position, which is why it cannot be expressed as an empty leg.
        static void Halt(Unit& unit);
};
