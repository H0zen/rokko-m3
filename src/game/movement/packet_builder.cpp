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

/**
 * @file packet_builder.cpp
 * @brief Movement spline network packet construction
 *
 * This file implements the PacketBuilder class which constructs network
 * packets for movement spline data. Handles:
 *
 * - Monster movement packets (MSG_MONSTER_MOVE)
 * - Linear and Catmull-Rom spline paths
 * - Facing/targeting information
 * - Path compression and packing
 *
 * The packet format includes:
 * - Source position
 * - Spline flags and duration
 * - Path points (absolute or relative)
 * - Final facing/target information
 *
 * @see PacketBuilder for the builder class
 * @see MoveSpline for the movement spline data
 * @see SMSG_MONSTER_MOVE for the opcode
 */

#include "packet_builder.h"
#include "MoveSpline.h"
#include "MonsterMovePath.h"
#include "Util.h"
#include "WorldPacket.h"
#include "../Object/Creature.h"

namespace Movement
{
    /**
     * @namespace Movement
     * @brief Movement system namespace
     *
     * Contains all movement-related classes and functions for
     * spline-based movement and packet construction.
     */
    /**
     * @brief Overloads the >> operator to read a Vector3 from a ByteBuffer.
     * @param b The ByteBuffer to read from.
     * @param v The Vector3 to read.
     */
    inline void operator >> (ByteBuffer& b, Vector3& v)
    {
        b >> v.x >> v.y >> v.z;
    }

    /**
     * @brief Writes the common part of a monster move packet.
     * @param move_spline The MoveSpline object containing movement data.
     * @param data The WorldPacket to write the data to.
     */
    void PacketBuilder::WriteCommonMonsterMovePart(const MoveSpline& move_spline, WorldPacket& data)
    {
        MoveSplineFlag splineflags = move_spline.splineflags;

        data << uint8(0);
        data << move_spline.spline.getPoint(move_spline.spline.first());
        data << move_spline.GetId();

        switch (splineflags & MoveSplineFlag::Mask_Final_Facing)
        {
            default:
                data << uint8(MonsterMoveNormal);
                break;
            case MoveSplineFlag::Final_Target:
                data << uint8(MonsterMoveFacingTarget);
                data << move_spline.facing.target;
                break;
            case MoveSplineFlag::Final_Angle:
                data << uint8(MonsterMoveFacingAngle);
                data << Geometry::Placement::NormalizeOrientation(move_spline.facing.angle);
                break;
            case MoveSplineFlag::Final_Point:
                data << uint8(MonsterMoveFacingSpot);
                data << move_spline.facing.f.x << move_spline.facing.f.y << move_spline.facing.f.z;
                break;
        }

        // add fake Enter_Cycle flag - needed for client-side cyclic movement (client will erase first spline vertex after first cycle done)
        splineflags.enter_cycle = move_spline.isCyclic();
        data << uint32(splineflags & ~MoveSplineFlag::Mask_No_Monster_Move);

        if (splineflags.animation)
        {
            data << splineflags.getAnimationId();
            data << move_spline.effect_start_time;
        }

        data << move_spline.Duration();

        if (splineflags.parabolic)
        {
            data << move_spline.vertical_acceleration;
            data << move_spline.effect_start_time;
        }
    }

    // The three path bodies live in MonsterMovePath.h, so the suite can pin their bytes
    // without a Unit, a Creature or a map behind them (MotionWriters_*_path_body_*).

    /**
     * @brief Writes a monster move packet.
     * @param move_spline The MoveSpline object containing movement data.
     * @param data The WorldPacket to write the data to.
     */
    void PacketBuilder::WriteMonsterMove(const MoveSpline& move_spline, WorldPacket& data)
    {
        WriteCommonMonsterMovePart(move_spline, data);

        const Spline<int32>& spline = move_spline.spline;
        MoveSplineFlag splineflags = move_spline.splineflags;
        if (splineflags & MoveSplineFlag::UncompressedPath)
        {
            if (splineflags.cyclic)
            {
                WriteCatmullRomCyclicPath(spline, data);
            }
            else
            {
                WriteCatmullRomPath(spline, data);
            }
        }
        else
        {
            WriteLinearPath(spline, data);
        }
    }

    void PacketBuilder::WriteCreateBits(const MoveSpline& move_spline, ByteBuffer& data)
    {
        if (!data.WriteBit(!move_spline.Finalized()))
        {
            return;
        }

        uint32 nodes = move_spline.getPath().size();
        bool hasSplineStartTime = move_spline.splineflags & (MoveSplineFlag::Trajectory | MoveSplineFlag::Animation);
        bool hasSplineVerticalAcceleration = (move_spline.splineflags & MoveSplineFlag::Trajectory) && move_spline.effect_start_time < move_spline.Duration();

        data.WriteBits(uint8(move_spline.spline.mode()), 2);
        data.WriteBit(hasSplineStartTime);
        data.WriteBits(nodes, 22);

        switch (move_spline.splineflags & MoveSplineFlag::Mask_Final_Facing)
        {
            case MoveSplineFlag::Final_Target:
            {
                data.WriteBits(2, 2);

                data.WriteGuidMask<4, 3, 7, 2, 6, 1, 0, 5>(ObjectGuid(move_spline.facing.target));
                break;
            }
            case MoveSplineFlag::Final_Angle:
                data.WriteBits(0, 2);
                break;
            case MoveSplineFlag::Final_Point:
                data.WriteBits(1, 2);
                break;
            default:
                data.WriteBits(3, 2);
                break;
        }

        data.WriteBit(hasSplineVerticalAcceleration);
        data.WriteBits(move_spline.splineflags.raw(), 25);
    }

    void PacketBuilder::WriteCreateBytes(const MoveSpline& move_spline, ByteBuffer& data)
    {
        if (!move_spline.Finalized())
        {
            uint32 nodes = move_spline.getPath().size();
            bool hasSplineStartTime = move_spline.splineflags & (MoveSplineFlag::Trajectory | MoveSplineFlag::Animation);
            bool hasSplineVerticalAcceleration = (move_spline.splineflags & MoveSplineFlag::Trajectory) && move_spline.effect_start_time < move_spline.Duration();

            if (hasSplineVerticalAcceleration)
            {
                data << float(move_spline.vertical_acceleration);   // added in 3.1
            }

            data << int32(move_spline.timePassed());

            if (move_spline.splineflags & MoveSplineFlag::Final_Angle)
            {
                data << float(Geometry::Placement::NormalizeOrientation(move_spline.facing.angle));
            }
            else if (move_spline.splineflags & MoveSplineFlag::Final_Target)
            {
                data.WriteGuidBytes<5, 3, 7, 1, 6, 4, 2, 0>(ObjectGuid(move_spline.facing.target));
            }

            for (uint32 i = 0; i < nodes; ++i)
            {
                data << float(move_spline.getPath()[i].z);
                data << float(move_spline.getPath()[i].x);
                data << float(move_spline.getPath()[i].y);
            }

            if (move_spline.splineflags & MoveSplineFlag::Final_Point)
            {
                data << float(move_spline.facing.f.x) << float(move_spline.facing.f.z) << float(move_spline.facing.f.y);
            }

            data << float(1.f);
            data << int32(move_spline.Duration());
            if (hasSplineStartTime)
            {
                data << int32(move_spline.effect_start_time);   // added in 3.1
            }

            data << float(1.f);
        }

        if (!move_spline.isCyclic())
        {
            Vector3 dest = move_spline.FinalDestination();
            data << float(dest.z);
            data << float(dest.x);
            data << float(dest.y);
        }
        else
        {
            data << Vector3::zero();
        }

        data << uint32(move_spline.GetId());
    }
}
