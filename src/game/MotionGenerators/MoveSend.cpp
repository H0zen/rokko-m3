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

#include "MoveSend.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "wire/MonsterMoveCodec.h"

namespace
{
    Wire::Vec3 Point(const Geometry::Vector3& v)
    {
        Wire::Vec3 out;
        out.x = v.x;
        out.y = v.y;
        out.z = v.z;
        return out;
    }

    /// One middle point of a linear path, as the client decodes it: the offset from the
    /// midpoint of the first and last point, in quarter-yards, truncated into signed
    /// fields of 11, 11 and 10 bits. The writer has already proved every offset is inside
    /// the representable range, so nothing here can wrap.
    uint32 Pack(const Geometry::Vector3& middle, const Geometry::Vector3& point)
    {
        const Geometry::Vector3 offset = middle - point;
        uint32 packed = 0;
        packed |= (uint32(int(offset.x / 0.25f)) & 0x7FF);
        packed |= (uint32(int(offset.y / 0.25f)) & 0x7FF) << 11;
        packed |= (uint32(int(offset.z / 0.25f)) & 0x3FF) << 22;
        return packed;
    }
}

bool MoveSend::Leg(Unit& unit, const Move::Written& written, const Move::Facing& facing,
                   uint32 splineId)
{
    if (!written.Ok())
    {
        // Nothing goes out. The client would either crash on it or teleport the mover, and
        // neither is better than the mover staying put until the caller tries again.
        return false;
    }

    Wire::MonsterMove move;
    move.mover = unit.GetObjectGuid().GetRawValue();
    move.start = Point(written.start);
    move.id = splineId;
    move.flags = written.flags;
    move.duration = written.duration;
    move.destination = Point(written.destination);
    move.path = (written.flags & Move::SPLINE_UNCOMPRESSED) != 0
        ? Wire::SplinePath::Uncompressed
        : Wire::SplinePath::Linear;

    switch (facing.mode)
    {
        case Move::Facing::Mode::Angle:
            move.type = Wire::MonsterMoveType::FacingAngle;
            move.facingAngle = facing.angle;
            break;
        case Move::Facing::Mode::Spot:
            move.type = Wire::MonsterMoveType::FacingSpot;
            move.facingSpot = Point(facing.spot);
            break;
        case Move::Facing::Mode::Unit:
            move.type = Wire::MonsterMoveType::FacingTarget;
            move.facingTarget = facing.unit;
            break;
        case Move::Facing::Mode::Travel:
        default:
            // Nothing to say: while the spline runs the client takes the facing from the
            // tangent by itself, so a leg that ends facing the way it travelled needs no
            // field and no second packet.
            move.type = Wire::MonsterMoveType::Normal;
            break;
    }

    if (move.path == Wire::SplinePath::Uncompressed)
    {
        move.points.push_back(Point(written.start));
        for (size_t i = 0; i < written.middle.size(); ++i)
        {
            move.points.push_back(Point(written.middle[i]));
        }
        move.points.push_back(Point(written.destination));
    }
    else
    {
        // The midpoint of the first and last point is what the client subtracts from, so it
        // is what we measure against. It is the packet's own start and destination, not the
        // midpoint of a bounding box and not the middle of the path.
        const Geometry::Vector3 midpoint = (written.start + written.destination) * 0.5f;
        for (size_t i = 0; i < written.middle.size(); ++i)
        {
            move.packedOffsets.push_back(Pack(midpoint, written.middle[i]));
        }
    }

    WorldPacket data(SMSG_MONSTER_MOVE, 64);
    Wire::EncodeMonsterMove(data, SMSG_MONSTER_MOVE, move);
    unit.SendMessageToSet(&data, true);
    return true;
}

void MoveSend::Halt(Unit& unit)
{
    Wire::MonsterMove move;
    move.mover = unit.GetObjectGuid().GetRawValue();
    move.start = Point(unit.Where().Pos());
    move.type = Wire::MonsterMoveType::Stop;

    WorldPacket data(SMSG_MONSTER_MOVE, 32);
    Wire::EncodeMonsterMove(data, SMSG_MONSTER_MOVE, move);
    unit.SendMessageToSet(&data, true);
}
