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

#ifndef MANGOS_WIRE_MONSTERMOVECODEC_H
#define MANGOS_WIRE_MONSTERMOVECODEC_H

#include "Platform/Define.h"
#include "wire/MovementCodec.h"
#include "wire/MovementStatus.h"

#include <vector>

class ByteBuffer;

namespace Wire
{
    /// SMSG_MONSTER_MOVE's per-move type byte (MoveSplineInit's MonsterMoveType):
    /// Stop ends the packet right there; the three facings each carry their own
    /// payload before the spline flags.
    enum class MonsterMoveType : uint8 { Normal = 0, Stop = 1, FacingSpot = 2, FacingTarget = 3, FacingAngle = 4 };

    /// Whether the spline's points are the raw list (Uncompressed) or the
    /// destination plus packed middle offsets (Linear, WriteLinearPath).
    enum class SplinePath : uint8 { Linear, Uncompressed };

    const uint32 kSplineFlagAnimation        = 0x01000000;
    const uint32 kSplineFlagTrajectory       = 0x02000000;
    const uint32 kSplineFlagUncompressedPath = 0x00400000;

    /// SMSG_MONSTER_MOVE and SMSG_MONSTER_MOVE_TRANSPORT (PacketBuilder::WriteMonsterMove,
    /// MoveSplineInit::Launch/Stop): the most common packet the server sends, and the one
    /// whose shape depends on the most -- the transport form, the facing the mover ends
    /// up looking at, the animation and trajectory blocks, and either an uncompressed path
    /// or a linear one with its middle points kept packed.
    ///
    /// Provenance: SMSG_MONSTER_MOVE(_TRANSPORT) -- the tree's own builder
    /// (PacketBuilder::WriteMonsterMove), the client's handler sub_140248800, and the
    /// real-client families golden x250, every one of them a linear path. The
    /// uncompressed, cyclic, animation and trajectory forms have no witness on any
    /// wire yet: unit fixtures cover them, this tree has not seen one live.
    struct MonsterMove
    {
        uint64 mover = 0;
        /// Decode output: the opcode says which form it was, so the encoder takes
        /// it from the opcode too and never reads this back.
        bool   onTransport = false;        // the SMSG_MONSTER_MOVE_TRANSPORT form
        uint64 transport = 0;
        int8   seat = -1;
        uint8  exitVoluntary = 0;          // the byte after the guids (CPP: VehicleExitVoluntary; the tree writes 0)
        Vec3   start;
        uint32 id = 0;
        MonsterMoveType type = MonsterMoveType::Normal;
        uint64 facingTarget = 0;           // FacingTarget: a raw uint64 guid
        float  facingAngle = 0.0f;         // FacingAngle
        Vec3   facingSpot;                 // FacingSpot
        uint32 flags = 0;                  // MoveSplineFlag bits as written (Mask_No_Monster_Move already cleared by the writer)
        uint8  animationId = 0;            // when flags & kSplineFlagAnimation
        int32  animationStart = 0;
        uint32 duration = 0;
        float  verticalAcceleration = 0.0f; // when flags & kSplineFlagTrajectory
        int32  parabolicStart = 0;
        /// Decode output: kSplineFlagUncompressedPath is what is on the wire and
        /// what both directions branch on, so the encoder never reads this back.
        SplinePath path = SplinePath::Linear;
        std::vector<Vec3>   points;         // Uncompressed: every point, in order (a cyclic writer's fake first point included, it is on the wire)
        Vec3                destination;    // Linear
        std::vector<uint32> packedOffsets;  // Linear: the middle points, packed by appendPackXYZ, kept packed
    };

    void EncodeMonsterMove(ByteBuffer& out, uint16 opcode, MonsterMove const& v);
    DecodeResult DecodeMonsterMove(ByteBuffer& in, uint16 opcode, MonsterMove& out);
}

#endif
