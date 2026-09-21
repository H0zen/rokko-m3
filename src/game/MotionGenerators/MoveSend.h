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
class ByteBuffer;

class MoveSend
{
    public:
        /// Encode and broadcast. Answers false when the writer refused, in which case
        /// nothing was sent and the mover has not been told to go anywhere.
        /// `shape` is the Move::Kind that produced the leg; it is only used to name the
        /// culprit when the writer refuses, which is the first thing worth knowing.
        static bool Leg(Unit& unit, const Move::Written& written,
                        const Move::Facing& facing, Move::Kind shape = Move::Kind::Count,
                        uint32 splineId = 0);

        /// Stop where the mover stands. A separate form of the same packet: it ends right
        /// after the position, which is why it cannot be expressed as an empty leg.
        static void Halt(Unit& unit);

        /// Turn on the spot. A leg of no length is the one shape the client cannot decode,
        /// so a turn is its own packet rather than a spline going nowhere.
        static void Face(Unit& unit, float orientation);

        /// Boarding or leaving a seat. NOT a walk: the client refuses a spline carrying
        /// either of these flags (sub_576420 returns early on 0x18000) and snaps the
        /// passenger to the destination instead, which is the whole point of them. So this
        /// goes out unvalidated -- there is no leg to time, no ceiling to exceed and no
        /// zero-length segment to divide by, because no spline is built from it.
        static void SeatMove(Unit& passenger, const Geometry::Vector3& to, bool board,
                             bool hasFacing = false, float facing = 0.0f);

        /// WHAT A LATE OBSERVER IS TOLD.
        ///
        /// A player who comes into range while a creature is already walking gets the leg
        /// in the creature's create block instead of in a MonsterMove. It has to describe
        /// the SAME leg -- same points, same duration, same start -- or that player's
        /// client builds a second spline that drifts from everyone else's. Both halves read
        /// the mover's own UnitMovement, which is the single description there is.
        static void CreateBits(Unit const& unit, ByteBuffer& data);
        static void CreateBytes(Unit const& unit, ByteBuffer& data);
};
