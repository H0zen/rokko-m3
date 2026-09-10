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

#include "MovementBridge.h"

#include "Unit.h"

Wire::MovementStatus Movement::ToWire(MovementInfo const& record)
{
    MovementInfo::StatusInfo const& si = record.GetStatusInfo();
    Wire::MovementStatus w;
    w.guid   = record.GetGuid().GetRawValue();
    w.guid2  = record.GetGuid2().GetRawValue();
    w.flags  = uint32(record.GetMovementFlags());
    w.flags2 = uint32(record.GetMovementFlags2());
    w.has.timestamp = si.hasTimeStamp;
    w.time = si.hasTimeStamp ? record.GetTime() : 0;
    w.pos.x = record.GetPos()->x;
    w.pos.y = record.GetPos()->y;
    w.pos.z = record.GetPos()->z;
    w.has.orientation = si.hasOrientation;
    w.pos.o = si.hasOrientation ? record.GetPos()->o : 0.0f;
    w.has.pitch = si.hasPitch;
    w.pitch = si.hasPitch ? record.GetPitch() : 0.0f;
    w.has.spline = si.hasSpline;
    w.has.splineElevation = si.hasSplineElevation;
    w.splineElevation = si.hasSplineElevation ? record.GetSplineElevation() : 0.0f;
    w.fall.present = si.hasFallData;
    w.fall.hasDirection = si.hasFallDirection;
    if (si.hasFallData)
    {
        w.fall.time = record.GetFallTime();
        w.fall.vertical = record.GetJumpInfo().velocity;
        if (si.hasFallDirection)
        {
            w.fall.horizontal = record.GetJumpInfo().xyspeed;
            w.fall.cosAngle = record.GetJumpInfo().cosAngle;
            w.fall.sinAngle = record.GetJumpInfo().sinAngle;
        }
    }
    // The gate is explicit now: SetTransportData/ClearTransportData keep it in
    // step with t_guid, so it no longer has to be inferred from a non-empty guid.
    w.transport.present = si.hasTransportData;
    if (w.transport.present)
    {
        w.transport.guid = record.GetTransportGuid().GetRawValue();
        w.transport.pos.x = record.GetTransportPos()->x;
        w.transport.pos.y = record.GetTransportPos()->y;
        w.transport.pos.z = record.GetTransportPos()->z;
        w.transport.pos.o = record.GetTransportPos()->o;
        w.transport.time = record.GetTransportTime();
        w.transport.seat = record.GetTransportSeat();
        w.transport.hasTime2 = si.hasTransportTime2;
        w.transport.time2 = si.hasTransportTime2 ? record.GetTransportTime2() : 0;
        w.transport.hasVehicleId = si.hasVehicleId;
        w.transport.vehicleId = record.GetVehicleId();
    }
    w.byteParam = record.GetByteParam();
    w.counter = record.GetCounter();
    w.value = record.GetExtraFloat();
    w.twoBits = record.GetExtraTwoBits();
    w.has.unknownBit = si.hasUnknownBit;
    w.has.emptyFlagsBlock = si.hasEmptyFlagsBlock;
    w.has.emptyFlags2Block = si.hasEmptyFlags2Block;
    w.has.heightChangeFailed = si.hasHeightChangeFailed;
    return w;
}

void Movement::FromWire(Wire::MovementStatus const& s, MovementInfo& r)
{
    r.guid = ObjectGuid(s.guid);
    r.guid2 = ObjectGuid(s.guid2);
    r.moveFlags = s.flags;
    r.moveFlags2 = uint16(s.flags2);
    r.si.hasTimeStamp = s.has.timestamp;
    r.time = s.time;
    r.pos.x = s.pos.x; r.pos.y = s.pos.y; r.pos.z = s.pos.z;
    r.si.hasOrientation = s.has.orientation;
    r.pos.o = s.pos.o;
    r.si.hasPitch = s.has.pitch;
    r.s_pitch = s.pitch;
    r.si.hasSpline = s.has.spline;
    r.si.hasSplineElevation = s.has.splineElevation;
    r.splineElevation = s.splineElevation;
    r.si.hasFallData = s.fall.present;
    r.si.hasFallDirection = s.fall.hasDirection;
    r.fallTime = s.fall.time;
    r.jump.velocity = s.fall.vertical;
    r.jump.xyspeed = s.fall.horizontal;
    r.jump.cosAngle = s.fall.cosAngle;
    r.jump.sinAngle = s.fall.sinAngle;
    r.si.hasTransportData = s.transport.present;
    r.t_guid = ObjectGuid(s.transport.guid);
    r.t_pos.x = s.transport.pos.x; r.t_pos.y = s.transport.pos.y; r.t_pos.z = s.transport.pos.z; r.t_pos.o = s.transport.pos.o;
    r.t_time = s.transport.time;
    r.t_seat = s.transport.seat;
    r.si.hasTransportTime2 = s.transport.hasTime2;
    r.t_time2 = s.transport.time2;
    r.si.hasVehicleId = s.transport.hasVehicleId;
    r.vehicleId = s.transport.vehicleId;
    r.byteParam = s.byteParam;
    r.counter = s.counter;
    r.extraFloat = s.value;
    r.extraTwoBits = s.twoBits;
    r.si.hasUnknownBit = s.has.unknownBit;
    r.si.hasEmptyFlagsBlock = s.has.emptyFlagsBlock;
    r.si.hasEmptyFlags2Block = s.has.emptyFlags2Block;
    r.si.hasHeightChangeFailed = s.has.heightChangeFailed;
}
