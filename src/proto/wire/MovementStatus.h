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

#ifndef MANGOS_WIRE_MOVEMENTSTATUS_H
#define MANGOS_WIRE_MOVEMENTSTATUS_H

#include "Platform/Define.h"

namespace Wire
{
    struct Vec4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float o = 0.0f;

        bool operator==(Vec4 const& r) const { return x == r.x && y == r.y && z == r.z && o == r.o; }
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        bool operator==(Vec3 const& r) const { return x == r.x && y == r.y && z == r.z; }
    };

    /**
     * @brief The movement-status block the 4.3.4 client and server exchange.
     *
     * Pure value: no ObjectGuid, no Unit, nothing from src/game. GUIDs are raw
     * 64-bit numbers; presence of optional blocks is explicit (has.*, fall.present,
     * transport.present) so a decode->encode round trip is lossless.
     */
    struct MovementStatus
    {
        uint64 guid = 0;     ///< the mover
        uint64 guid2 = 0;    ///< a second unit some layouts carry (vehicle seat changes)
        uint32 flags = 0;    ///< MovementFlags: 30 bits on the wire, "present" iff non-zero. A value using bit 30 or 31 is announced present but truncated (real 4.3.4 flags fit).
        uint32 flags2 = 0;   ///< MovementFlags2: 12 bits on the wire, "present" iff non-zero; same truncation rule above bit 11.
        uint32 time = 0;     ///< client movement time, written only when has.timestamp
        Vec4   pos;          ///< o written only when has.orientation
        float  pitch = 0.0f;           ///< when has.pitch
        float  splineElevation = 0.0f; ///< when has.splineElevation
        uint32 counter = 0;  ///< movement counter (acks). The legacy reader discards it; we do not.
        int8   byteParam = 0;
        float  value = 0.0f;   ///< ExtraFloat: the speed a SET/UPDATE/ACK carries, or a collision height
        uint8  twoBits = 0;    ///< ExtraTwoBits: the collision-height change reason

        struct
        {
            bool orientation = true;
            bool pitch = false;
            bool timestamp = true;
            bool spline = false;
            bool splineElevation = false;
            bool unknownBit = false;      ///< the layout's unnamed bit, carried so a captured packet re-encodes byte-identical
            /// A decoded packet announced its flags block present but the block was zero.
            /// Presence is otherwise derived from the value; this remembers the one case
            /// that derivation cannot, so the packet re-encodes byte-identical.
            bool emptyFlagsBlock = false;
            bool emptyFlags2Block = false; ///< same, for flags2
            bool heightChangeFailed = false; ///< SMSG_MOVE_UPDATE's HasHeightChangeFailed bit
        } has;

        struct
        {
            bool   present = false;      ///< HasFallData
            bool   hasDirection = false; ///< HasFallDirection (only meaningful when present)
            uint32 time = 0;
            float  vertical = 0.0f;
            float  horizontal = 0.0f;    ///< when hasDirection
            float  cosAngle = 0.0f;      ///< when hasDirection
            float  sinAngle = 0.0f;      ///< when hasDirection
        } fall;

        struct
        {
            bool   present = false;  ///< HasTransportData
            uint64 guid = 0;
            Vec4   pos;
            uint32 time = 0;
            uint32 time2 = 0;        ///< when hasTime2
            uint32 vehicleId = 0;    ///< when hasVehicleId (the legacy reader called this transport time 3)
            int8   seat = -1;
            bool   hasTime2 = false;
            bool   hasVehicleId = false;
        } transport;

        /// Field-wise. Fields under an absent gate (e.g. fall.hasDirection while
        /// !fall.present, transport.* while !transport.present) are compared too, so
        /// a fixture must leave them at their defaults for a round trip to compare equal.
        bool operator==(MovementStatus const& r) const
        {
            return guid == r.guid && guid2 == r.guid2 && flags == r.flags && flags2 == r.flags2 &&
                   time == r.time && pos == r.pos && pitch == r.pitch &&
                   splineElevation == r.splineElevation && counter == r.counter &&
                   byteParam == r.byteParam &&
                   value == r.value && twoBits == r.twoBits && has.heightChangeFailed == r.has.heightChangeFailed &&
                   has.orientation == r.has.orientation && has.pitch == r.has.pitch &&
                   has.timestamp == r.has.timestamp && has.spline == r.has.spline &&
                   has.splineElevation == r.has.splineElevation && has.unknownBit == r.has.unknownBit &&
                   has.emptyFlagsBlock == r.has.emptyFlagsBlock && has.emptyFlags2Block == r.has.emptyFlags2Block &&
                   fall.present == r.fall.present && fall.hasDirection == r.fall.hasDirection &&
                   fall.time == r.fall.time && fall.vertical == r.fall.vertical &&
                   fall.horizontal == r.fall.horizontal && fall.cosAngle == r.fall.cosAngle &&
                   fall.sinAngle == r.fall.sinAngle &&
                   transport.present == r.transport.present && transport.guid == r.transport.guid &&
                   transport.pos == r.transport.pos && transport.time == r.transport.time &&
                   transport.time2 == r.transport.time2 && transport.vehicleId == r.transport.vehicleId &&
                   transport.seat == r.transport.seat && transport.hasTime2 == r.transport.hasTime2 &&
                   transport.hasVehicleId == r.transport.hasVehicleId;
        }
    };
}

#endif
