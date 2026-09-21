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
#include "Timer.h"
#include "MoveStats.h"
#include "wire/MonsterMoveCodec.h"
#include "MotionMaster.h"
#include "Geometry/Placement.h"

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
                   Move::Kind shape, uint32 splineId)
{
    if (!written.Ok())
    {
        // Nothing goes out. The client would either crash on it or teleport the mover, and
        // neither is better than the mover staying put until the caller tries again.
        MoveStats::Refused(uint8(written.refusal), uint8(shape),
                           unit.GetObjectGuid().GetRawValue(), unit.GetEntry());
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
    MoveStats::Sent(uint32(written.middle.size()) + 2);
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

void MoveSend::SeatMove(Unit& passenger, const Geometry::Vector3& to, bool board,
                        bool hasFacing, float facing)
{
    Wire::MonsterMove move;
    move.mover = passenger.GetObjectGuid().GetRawValue();
    move.start = Point(passenger.Where().Pos());
    move.flags = board ? Move::SPLINE_BOARD_VEHICLE : Move::SPLINE_EXIT_VEHICLE;
    move.duration = 0;
    move.destination = Point(to);
    move.path = Wire::SplinePath::Linear;

    if (hasFacing)
    {
        move.type = Wire::MonsterMoveType::FacingAngle;
        move.facingAngle = facing;
    }

    WorldPacket data(SMSG_MONSTER_MOVE, 64);
    Wire::EncodeMonsterMove(data, SMSG_MONSTER_MOVE, move);
    passenger.SendMessageToSet(&data, true);
}

void MoveSend::Face(Unit& unit, float orientation)
{
    Wire::MonsterMove move;
    move.mover = unit.GetObjectGuid().GetRawValue();
    move.start = Point(unit.Where().Pos());
    move.type = Wire::MonsterMoveType::FacingAngle;
    move.facingAngle = orientation;
    move.duration = 0;
    move.destination = move.start;

    WorldPacket data(SMSG_MONSTER_MOVE, 48);
    Wire::EncodeMonsterMove(data, SMSG_MONSTER_MOVE, move);
    unit.SendMessageToSet(&data, true);
    MoveStats::Turned();
}

void MoveSend::CreateBits(Unit const& unit, ByteBuffer& data)
{
    MotionMaster const* motion = unit.GetMotionMaster();
    const bool moving = motion && motion->IsMoving();
    if (!data.WriteBit(moving))
    {
        return;
    }

    Move::Route const& route = motion->InFlight();
    Move::Facing const& facing = motion->SentFacing();

    // Every leg this server sends is linear: the client rounds the corners itself when
    // asked, and a curved spline would be timed by the chords of its control points while
    // it travelled the true arc, which is a speed error we have no reason to pay.
    data.WriteBits(uint8(0), 2);

    const bool hasStartTime = (motion->SentFlags() & (Move::SPLINE_PARABOLA | Move::SPLINE_ANIMATION)) != 0;
    data.WriteBit(hasStartTime);
    data.WriteBits(uint32(route.Count()), 22);

    switch (facing.mode)
    {
        case Move::Facing::Mode::Unit:
            data.WriteBits(2, 2);
            data.WriteGuidMask<4, 3, 7, 2, 6, 1, 0, 5>(ObjectGuid(facing.unit));
            break;
        case Move::Facing::Mode::Angle:
            data.WriteBits(0, 2);
            break;
        case Move::Facing::Mode::Spot:
            data.WriteBits(1, 2);
            break;
        case Move::Facing::Mode::Travel:
        default:
            data.WriteBits(3, 2);
            break;
    }

    // No parabola is ever sent with a vertical acceleration block yet; when one is, this is
    // where it is announced.
    data.WriteBit(false);
    data.WriteBits(motion->SentFlags() & 0x1FFFFFF, 25);
}

void MoveSend::CreateBytes(Unit const& unit, ByteBuffer& data)
{
    MotionMaster const* motion = unit.GetMotionMaster();
    const bool moving = motion && motion->IsMoving();

    if (moving)
    {
        Move::Route const& route = motion->InFlight();
        Move::Facing const& facing = motion->SentFacing();

        // How far into the leg the observer is arriving. The client adds this to its own
        // clock, so the creature appears where the others already see it rather than
        // starting the walk again from the first point.
        data << int32(getMSTime() - route.StartTime());

        if (facing.mode == Move::Facing::Mode::Angle)
        {
            data << float(Geometry::Placement::NormalizeOrientation(facing.angle));
        }
        else if (facing.mode == Move::Facing::Mode::Unit)
        {
            data.WriteGuidBytes<5, 3, 7, 1, 6, 4, 2, 0>(ObjectGuid(facing.unit));
        }

        for (uint16 i = 0; i < route.Count(); ++i)
        {
            Geometry::Vector3 const& at = route.Point(i);
            data << float(at.z);
            data << float(at.x);
            data << float(at.y);
        }

        if (facing.mode == Move::Facing::Mode::Spot)
        {
            data << float(facing.spot.x) << float(facing.spot.z) << float(facing.spot.y);
        }

        data << float(1.f);
        data << int32(motion->SentDuration());
        if ((motion->SentFlags() & (Move::SPLINE_PARABOLA | Move::SPLINE_ANIMATION)) != 0)
        {
            data << int32(0);
        }
        data << float(1.f);
    }

    // The final destination, which the client keeps whether a spline runs or not.
    if (moving)
    {
        Geometry::Vector3 const& end = motion->InFlight().End();
        data << float(end.z);
        data << float(end.x);
        data << float(end.y);
    }
    else
    {
        data << float(0.0f) << float(0.0f) << float(0.0f);
    }

    data << uint32(motion ? motion->SentId() : 0);
}
