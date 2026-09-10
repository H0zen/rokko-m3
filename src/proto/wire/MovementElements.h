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

#ifndef MANGOS_WIRE_MOVEMENTELEMENTS_H
#define MANGOS_WIRE_MOVEMENTELEMENTS_H

#include "Platform/Define.h"

namespace Wire
{
    /**
     * @brief One step of a 4.3.4 movement-status layout.
     *
     * A packet's layout is a sequence of these, terminated by End. The same
     * vocabulary drives both the reader and the writer (MovementCodec.h), so a
     * sequence can never be right for one direction and wrong for the other.
     * Names follow the legacy MSE* enum that lived in src/game/movement's now-
     * retired MovementStructures.h (P2-B) where it had a name for the slot, and
     * the Cataclysm Preservation Project's where it did not (the vehicle id,
     * which the legacy header misnamed as a third transport time, and the five
     * P1 additions at the end). The registry in src/proto/wire is the only
     * layout source now; every element's ordinal below End is what once let the
     * fence test map the legacy arrays onto these by value.
     */
    enum class Element : uint8
    {
        Flags,
        Flags2,
        Timestamp,
        HasPitch,
        GuidBit0, GuidBit1, GuidBit2, GuidBit3, GuidBit4, GuidBit5, GuidBit6, GuidBit7,
        Guid2Bit0, Guid2Bit1, Guid2Bit2, Guid2Bit3, Guid2Bit4, Guid2Bit5, Guid2Bit6, Guid2Bit7,
        HasUnknownBit,
        HasMovementFlags,
        HasMovementFlags2,
        HasTimestamp,
        HasOrientation,
        HasFallData,
        HasFallDirection,
        HasTransportData,
        HasTransportTime2,
        HasVehicleId,
        TransportGuidBit0, TransportGuidBit1, TransportGuidBit2, TransportGuidBit3,
        TransportGuidBit4, TransportGuidBit5, TransportGuidBit6, TransportGuidBit7,
        HasSpline,
        HasSplineElevation,
        PositionX,
        PositionY,
        PositionZ,
        PositionO,
        GuidByte0, GuidByte1, GuidByte2, GuidByte3, GuidByte4, GuidByte5, GuidByte6, GuidByte7,
        Guid2Byte0, Guid2Byte1, Guid2Byte2, Guid2Byte3, Guid2Byte4, Guid2Byte5, Guid2Byte6, Guid2Byte7,
        Pitch,
        FallTime,
        TransportGuidByte0, TransportGuidByte1, TransportGuidByte2, TransportGuidByte3,
        TransportGuidByte4, TransportGuidByte5, TransportGuidByte6, TransportGuidByte7,
        SplineElevation,
        FallHorizontalSpeed,
        FallVerticalSpeed,
        FallCosAngle,
        FallSinAngle,
        TransportSeat,
        TransportPositionO,
        TransportPositionX,
        TransportPositionY,
        TransportPositionZ,
        TransportTime,
        TransportTime2,
        TransportVehicleId,
        MovementCounter,
        ByteParam,

        // P1: what the Cataclysm Preservation Project's tables use beyond the legacy set.
        ExtraFloat,             ///< a float the packet carries outside the status: a speed, a collision height
        ExtraTwoBits,           ///< a two-bit field in the bit section: the collision-height change reason
        HasHeightChangeFailed,  ///< SMSG_MOVE_UPDATE's extra bit
        OneBit,                 ///< a bit the client always writes as 1; written as 1, read and discarded
        FlushBits,              ///< end the bit section here: the writer pads to a byte, the reader realigns

        End
    };

    /// A layout: Elements terminated by Element::End. Tables are static storage.
    using Sequence = Element const*;
}

#endif
