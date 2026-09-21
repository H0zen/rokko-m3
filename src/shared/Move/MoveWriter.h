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

// A ROUTE, TURNED INTO THE ONE MESSAGE THE CLIENT UNDERSTANDS -- OR REFUSED.
//
// Everything the server can say about movement is one packet, and this is the only place
// that builds it. The rules it enforces are the client's, read out of Wow.exe 15595, and
// every one of them is a way the client can be made to do something other than what we
// meant:
//
//   * A segment of no length divides by zero inside a fixed-point atan2 and takes the
//     client down with ERROR #132. It deduplicates nothing except a cyclic path's closing
//     point, so this must.
//   * Packed offsets are quarter-yards in signed 11/11/10 fields, measured from the
//     midpoint of the first and last point. One yard past the edge the field does not
//     saturate, it changes sign, and the mover walks to the far side of the map.
//   * Asking for more speed than the ceiling does not fail, it stretches: the mover
//     arrives late and nothing says so.
//   * A spline the client refuses does not leave the mover standing -- it TELEPORTS it to
//     the destination. So a packet that would be refused must never be sent, and a writer
//     that cannot produce a good one says no instead of guessing.
//
// WHAT IS NEVER SENT: a Catmull-Rom path. The client times a curve by the chords of its
// control points but travels its true, twenty-samples-per-segment length, so every curved
// spline moves faster than asked. It will round the corners itself if asked (the smoothing
// flag below), which costs nothing and has no such error. So every path that leaves here
// is linear.

#include "Move/Route.h"
#include "Geometry/Vector3.h"

#include <cstdint>
#include <vector>

namespace Move
{
    using Geometry::Vector3;

    /// The spline flags this writer sets, by the value the client tests. Named for what
    /// the client does with them, not for what the old server called them.
    enum SplineFlag : uint32_t
    {
        SPLINE_NONE          = 0x00000000,
        SPLINE_FALLING       = 0x00000040,
        SPLINE_WALK          = 0x00000100,
        SPLINE_CYCLIC        = 0x00001000,
        SPLINE_BOARD_VEHICLE = 0x00008000,  ///< the client refuses the spline and snaps to the seat
        SPLINE_EXIT_VEHICLE  = 0x00010000,  ///< likewise, in the other direction
        SPLINE_BACKWARD      = 0x00080000,  ///< the client subtracts pi from the travel facing
        SPLINE_ROUND_CORNERS = 0x00100000,  ///< the client builds its own entry and exit curves
        SPLINE_UNCOMPRESSED  = 0x00400000,  ///< every point raw, no packing and no reach limit
        SPLINE_ANIMATION     = 0x01000000,
        SPLINE_PARABOLA      = 0x02000000
    };

    /// Why a packet was not produced. Never a reason to send anything anyway: each of these
    /// ends with the client doing something worse than nothing.
    enum class Refusal : uint8_t
    {
        None,
        TooFewPoints,     ///< fewer than two distinct points: there is no leg here
        NoLength,         ///< every point collapsed into one
        ExceedsCeiling,   ///< the client would stretch this leg and the mover would arrive late
        OutOfPackReach,   ///< a middle point sits beyond the packed field and would wrap
        NotFinite         ///< a coordinate is NaN or infinite
    };

    /// What the writer produced: the points as the client will rebuild them, the duration
    /// as the client will compute it, and the flags actually set.
    struct Written
    {
        Refusal refusal = Refusal::None;
        bool Ok() const { return refusal == Refusal::None; }

        Vector3 start;                 ///< the position field at the head of the packet
        Vector3 destination;           ///< the last point
        std::vector<Vector3> middle;   ///< the points between, in order; packed by the codec
        uint32_t duration = 0;
        uint32_t flags = 0;
        float length = 0.0f;           ///< the chord sum the client will time this by
        float speed = 0.0f;            ///< what the mover will actually travel at
    };

    class MoveWriter
    {
        public:
            /// Turn a polyline travelled at `speed` into what the packet must carry.
            /// `moverSpeed` is the unit's own pace, which the client's ceiling is four
            /// times; it is NOT the speed the leg travels at.
            static Written Write(const Vector3* points, uint16_t count, float speed,
                                 float moverSpeed, uint32_t extraFlags = SPLINE_NONE);

            /// The same for a route already launched, so a caller that holds one does not
            /// have to take it apart.
            static Written Write(const Route& route, float moverSpeed,
                                 uint32_t extraFlags = SPLINE_NONE);

            /// A short name for a refusal, for logs and tests.
            static const char* Why(Refusal refusal);
    };
}
