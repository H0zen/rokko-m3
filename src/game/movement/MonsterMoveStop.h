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

#ifndef MANGOS_MOVEMENT_MONSTERMOVESTOP_H
#define MANGOS_MOVEMENT_MONSTERMOVESTOP_H

#include "Utilities/ByteBuffer.h"
#include "wire/GuidCodec.h"

/**
 * THE POINT-CARRYING STOP, on its own so the suite can pin its bytes.
 *
 * `peer/retail-taxi-flights-2026-09-22.md` §3: retail has two stop encodings and they are
 * near-complementary. The NO-POINT stop cancels a spline that is still running (6,108 of 6,137
 * in the corpus). The POINT-CARRYING stop -- one waypoint equal to the mover's own position, to
 * the last printed digit in 1,017 of 1,017 cases -- is what it sends when there is NOTHING to
 * cancel (693 of 1,017), and it is the one it sends before each control change at a taxi's
 * takeoff and landing, twice each way. "You are here, stop."
 *
 * The layout is the classic one and stops dead at the type byte: a spline of type 1 reads
 * nothing after it, so there is no flags word, no duration and no facing. Wire::MonsterMove's
 * own encoder agrees (src/proto/wire/MonsterMoveCodec.cpp, `case MonsterMoveType::Stop:
 * return;`), and MonsterMoveCodec_stop_form_ends_at_the_type pins that side.
 */
namespace Movement
{
    /// MonsterMoveType::MonsterMoveStop of MoveSpline.h, repeated so this header needs no
    /// spline header at all and the suite can include it alone. MoveSplineInit.cpp
    /// static_asserts the two against each other, so they cannot drift.
    static const uint8 kMonsterMoveStopType = 1;

    /**
     * @brief Writes a point-carrying stop's body.
     * @param out          The packet, already carrying its opcode and nothing else.
     * @param mover        The stopped unit's guid; written packed.
     * @param onTransport  True for the SMSG_MONSTER_MOVE_TRANSPORT form, which inserts the
     *                     vessel and the seat between the mover and the exit byte.
     * @param transport    The vessel's guid, packed; read only when `onTransport`.
     * @param seat         The seat, or -1 for a deck (a ship has no seats); as above.
     * @param x            The position the stop pins. Retail's is the mover's own, and both
     * @param y            the takeoff's and the landing's are exactly that.
     * @param z
     * @param splineId     The spline id the stop is issued under.
     */
    inline void WriteMonsterMoveStop(ByteBuffer& out, uint64 mover,
                                     bool onTransport, uint64 transport, int8 seat,
                                     float x, float y, float z, uint32 splineId)
    {
        Wire::WritePackedGuid(out, mover);
        if (onTransport)
        {
            Wire::WritePackedGuid(out, transport);
            out << int8(seat);
        }
        out << uint8(0);                        // SplineData: not an involuntary exit
        out << float(x) << float(y) << float(z);
        out << uint32(splineId);
        out << uint8(kMonsterMoveStopType);     // and NOTHING after it
    }
}

#endif
