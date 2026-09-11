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

#ifndef MANGOS_MOTION_PACKET_MATRIX_H
#define MANGOS_MOTION_PACKET_MATRIX_H

#include "Change.h"

/**
 * The per-change packet matrix (design v2 §7): for every change, which
 * opcode carries it to the owning client (with a counter the client echoes),
 * which ack the client answers with, which opcode tells everyone else once
 * the ack confirms it, and which opcode a server-driven unit broadcasts at
 * once instead. Checked in, not derived from names: a cell is an opcode the
 * wire layer knows, or 0 with the reason in the row's note. The test pins
 * which cells are 0.
 *
 * Sources: CPP MovementPacketSender.cpp:29-40 (the speed rows, {spline,
 * mover, observer} per UnitMoveType) and :195-240 (flags to the mover; the
 * generic SMSG_MOVE_UPDATE to observers), CPP Unit.cpp:11210 (root is
 * rebroadcast as the same opcode), this tree's writers (UnitSpeed.cpp,
 * Unit.cpp:7293-7438, CreatureMovement.cpp, MovementHandler.cpp:680,
 * Player.cpp:1624) and the 4.3.4 registry (wire/MovementLayouts.inc).
 */
namespace Motion
{
    enum class Mode : uint8
    {
        ServerDriven,   ///< the server integrates and owns state; observers get the spline family
        ClientDriven    ///< a client owns position; changes are negotiated with counters and acks
    };

    struct MatrixRow
    {
        ChangeType  type;
        bool        apply;      ///< flag rows come in pairs; other rows carry true
        uint16      mover;      ///< to the owning client, with a counter; 0 = none
        uint16      ack;        ///< what the client answers; 0 = none the wire layer knows
        uint16      observer;   ///< to everyone else once confirmed; 0 = none the wire layer knows
        uint16      spline;     ///< ServerDriven units, at once, to everyone; 0 = none
        char const* note;       ///< why a 0 is 0; "" when every cell is filled
    };

    MatrixRow const* RowFor(ChangeType type, bool apply);
    size_t           MatrixSize();
    MatrixRow const& MatrixRowAt(size_t index);
    /// The row whose mover or spline cell is `opcode`; `*isSpline` says which. Null otherwise.
    MatrixRow const* RowForOpcode(uint16 opcode, bool* isSpline);
    /// The row an ack opcode answers to (the first row whose `ack` cell is `opcode`; the
    /// rows of a flag pair share the type, which is all an ack needs). Null for 0 and for an
    /// opcode no row names -- the turn-rate and pitch-rate acks, whose layouts the lifter
    /// could not produce, among them.
    MatrixRow const* RowForAck(uint16 opcode);
}

#endif
