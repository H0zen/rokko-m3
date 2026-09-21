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

// ONE LEG OF MOVEMENT: the polyline that went out on the wire, kept rather than
// re-simulated.
//
// The client is handed a list of points and a speed and then runs its own clock (see
// ClientRules.h). So the server has no simulation to perform and nothing to integrate --
// it already knows the whole future of this leg the moment it sends it. Keeping the
// points costs one float per point more than throwing them away, and buys an exact
// position at any instant for about fifteen instructions.
//
// WHAT IT IS NOT. Not a spline: the curve, if any, is the client's business and we do not
// reproduce it. Not a state machine: a leg does not start, pause or finish, it is simply
// consulted at a time. Not frame-aware: the points are in whatever frame they were built
// in, and asking a leg from one frame about another is the caller's mistake to avoid.
//
// COST. Launching a leg reuses the last one's storage, so a chase that re-aims twice a
// second allocates once and never again. Reading a position walks a cursor that only ever
// moves forward, so a tick that asks every mover where it is does no searching at all.

#include "Geometry/Vector3.h"

#include <cstdint>
#include <vector>

namespace Move
{
    class Leg
    {
        public:
            Leg() : m_speed(0.0f), m_startMs(0), m_cursor(0) {}

            /// Take a polyline and a speed. `startMs` is the server clock at the moment
            /// the client begins it. Returns false, and leaves the leg empty, for input
            /// no mover could follow: fewer than two points, a speed that is not
            /// positive, or a path with no length in it.
            bool Launch(const Geometry::Vector3* points, uint16_t count, float speed,
                        uint32_t startMs);

            void Clear();

            bool Running() const { return m_points.size() >= 2; }
            uint16_t Count() const { return uint16_t(m_points.size()); }
            const Geometry::Vector3& Point(uint16_t i) const { return m_points[i]; }
            const Geometry::Vector3& Start() const { return m_points.front(); }
            const Geometry::Vector3& End() const { return m_points.back(); }

            float Speed() const { return m_speed; }
            float Length() const { return m_cumulative.empty() ? 0.0f : m_cumulative.back(); }
            uint32_t StartTime() const { return m_startMs; }

            /// How long the leg takes at our speed, in milliseconds. What we would put in
            /// the packet -- and what the client will replace with its own arithmetic over
            /// its own polyline, which is why this is a statement of speed and not a
            /// promise about when the mover lands.
            uint32_t Duration() const;
            uint32_t EndTime() const { return m_startMs + Duration(); }

            /// Where the mover is at `nowMs`. Clamped at both ends: before the leg starts
            /// it is at the first point, after it ends at the last.
            Geometry::Vector3 At(uint32_t nowMs) const;

            /// Which way it faces at `nowMs` -- the direction of the segment it is on, in
            /// [0, 2pi). Not stored; a leg that has stopped keeps its final heading.
            float FacingAt(uint32_t nowMs) const;

            bool Arrived(uint32_t nowMs) const;

            /// The moment the mover passes `distance` yards along the path, clamped to the
            /// leg. This is what makes a crossing schedulable instead of polled: the whole
            /// path is known now, so the time it cuts a boundary is known now too.
            uint32_t TimeAtDistance(float distance) const;

            /// How far along the path the mover is at `nowMs`, in yards.
            float DistanceAt(uint32_t nowMs) const;

        private:
            /// Elapsed milliseconds, as a signed difference so a wrapped server clock
            /// still reads correctly, and negative before the leg begins.
            int32_t Elapsed(uint32_t nowMs) const { return int32_t(nowMs - m_startMs); }

            /// The segment holding `distance`, advancing the cursor. Never returns the
            /// last index, so `seg` and `seg + 1` are always both valid points.
            uint16_t SegmentAt(float distance) const;

            std::vector<Geometry::Vector3> m_points;
            /// Distance from the first point to each point; m_cumulative[0] is zero and
            /// the last entry is the whole length. Filled once, at launch, out of the same
            /// walk that has to measure the path anyway to state its duration.
            std::vector<float> m_cumulative;

            float m_speed;
            uint32_t m_startMs;

            /// Queries within a tick run forward in time, so the segment we want is almost
            /// always the one we wanted last time or the next one. Mutable because reading
            /// a position is a const question whatever it does to the bookmark.
            mutable uint16_t m_cursor;
    };
}
