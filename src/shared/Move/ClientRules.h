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

// WHAT THE CLIENT DOES WITH A SPLINE WE SEND IT.
//
// Transcribed from Wow.exe build 15595, sub_5CB460 (the SMSG_MONSTER_MOVE handler) and
// sub_A27900 (the speed ceiling it consults). Pure functions of plain numbers: no unit,
// no packet, no map. They exist so the server can ask, before sending, what the client
// will actually do -- instead of assuming it obeys.
//
// The one fact everything here serves: THE CLIENT DOES NOT TRUST OUR DURATION. It sums
// the length of the polyline IT ended up with, divides by a speed of its own choosing,
// and uses that. So the duration we send is not a schedule, it is how we spell a speed --
// and the speed survives the trip only while it stays under the ceiling below.

#include "Geometry/Vector3.h"

#include <cstdint>

namespace Move
{
    namespace Client
    {
        /// Squared distance beyond which the client puts its OWN position in front of the
        /// first point we sent, instead of starting where we said. (1/36 yd)^2 -- 2.8 cm.
        /// Past it the client's polyline is longer than ours and its arrival drifts, so
        /// the server's own first point should be the position the server believes in.
        const float SPLICE_THRESHOLD_SQ = 0.00077160494f;

        /// A polyline this short or shorter keeps the duration we sent, untouched.
        const float RETIME_MIN_LENGTH = 0.16666667f;

        /// The ceiling is four times the mover's speed, but never less than this.
        const float CEILING_FACTOR = 4.0f;
        const float CEILING_FLOOR = 28.0f;

        /// Any of these spline flags makes the ceiling a flat 50 instead: a jump, a fall,
        /// a parabola. The client stops reasoning about the mover's walking speed there.
        const uint32_t CEILING_SPECIAL_FLAGS = 0x02000A40u;
        const float CEILING_SPECIAL = 50.0f;

        /// Below this the client refuses to divide and keeps the duration we sent.
        const float RETIME_MIN_SPEED = 0.00000095367432f;

        /// The fastest the client will let a spline move, whatever duration we ask for.
        /// NOT the mover's speed: four times it, floored, or a flat 50 for a jump. It is
        /// a guard against a spline used as a teleport, not a speed limit -- which is why
        /// a leg that exceeds it is always a bug on our side.
        inline float SpeedCeiling(float moverSpeed, uint32_t splineFlags)
        {
            if ((splineFlags & CEILING_SPECIAL_FLAGS) != 0)
            {
                return CEILING_SPECIAL;
            }
            const float scaled = moverSpeed * CEILING_FACTOR;
            return scaled <= CEILING_FLOOR ? CEILING_FLOOR : scaled;
        }

        /// The speed the client will actually travel at, given the length of the polyline
        /// IT built and the duration we sent. min(ceiling, what we asked for).
        inline float ActualSpeed(float clientLength, uint32_t sentDuration, float ceiling)
        {
            if (sentDuration == 0)
            {
                return ceiling;
            }
            const float asked = clientLength / (float(sentDuration) * 0.001f);
            return ceiling <= asked ? ceiling : asked;
        }

        /// The duration the client replaces ours with.
        inline uint32_t Retime(float clientLength, uint32_t sentDuration, float ceiling)
        {
            if (clientLength <= RETIME_MIN_LENGTH)
            {
                return sentDuration;
            }
            const float speed = ActualSpeed(clientLength, sentDuration, ceiling);
            if (speed <= RETIME_MIN_SPEED)
            {
                return sentDuration;
            }
            const uint32_t retimed = uint32_t((clientLength / speed) * 1000.0f);
            return retimed <= 1u ? 1u : retimed;
        }

        /// Will the client discard our first point and start from where it already thinks
        /// the mover is? True means our polyline and its polyline are different lengths,
        /// and every timing we derived is about a path it will not walk.
        inline bool SplicesOwnStart(const Geometry::Vector3& clientPos,
                                    const Geometry::Vector3& ourFirstPoint)
        {
            return (clientPos - ourFirstPoint).squaredMagnitude() > SPLICE_THRESHOLD_SQ;
        }

        /// Does this leg ask the client to move faster than it will allow? A leg that does
        /// is silently stretched: the mover arrives late and nothing reports it.
        inline bool ExceedsCeiling(float length, uint32_t duration, float ceiling)
        {
            if (duration == 0 || length <= RETIME_MIN_LENGTH)
            {
                return false;
            }
            return length / (float(duration) * 0.001f) > ceiling;
        }

        /// How many samples per segment the client uses to measure a curved spline's
        /// length. Measuring with fewer UNDERSTATES the curve -- a polyline through the
        /// samples is a set of chords, and a chord is shorter than the arc it spans -- so
        /// a server that samples more coarsely writes a duration that is too short, and
        /// the client stretches it back out. The error is one-sided: never too long.
        const uint32_t ARC_STEPS = 20;

        /// One uniform Catmull-Rom segment, sampled `steps` times: the length the client
        /// will attribute to the piece of curve between p1 and p2. The four points are the
        /// segment and its two neighbours, as the basis requires.
        inline float CatmullRomLength(const Geometry::Vector3& p0, const Geometry::Vector3& p1,
                                      const Geometry::Vector3& p2, const Geometry::Vector3& p3,
                                      uint32_t steps = ARC_STEPS)
        {
            if (steps == 0)
            {
                steps = 1;
            }
            Geometry::Vector3 previous = p1;
            float length = 0.0f;
            for (uint32_t i = 1; i <= steps; ++i)
            {
                const float t = float(i) / float(steps);
                const float t2 = t * t;
                const float t3 = t2 * t;
                const Geometry::Vector3 at =
                    (p1 * 2.0f + (p2 - p0) * t +
                     (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                     (p1 * 3.0f - p2 * 3.0f + p3 - p0) * t3) * 0.5f;
                length += (at - previous).magnitude();
                previous = at;
            }
            return length;
        }

        /// The client's own gravity, in yards per second squared, as it appears in the fall
        /// arithmetic of Wow.exe 15595. A server that uses a different one draws a parabola
        /// the client will not draw: it computes the arc itself from this number and the
        /// vertical acceleration in the packet.
        const float GRAVITY = 19.291105f;

        /// The apex of a ballistic arc launched straight up at `verticalSpeed`. What a
        /// knockback has to tell the client, since the height is not on the wire -- the
        /// acceleration and the duration are, and the client derives the rest.
        inline float ApexHeight(float verticalSpeed)
        {
            return (verticalSpeed * verticalSpeed) / (2.0f * GRAVITY);
        }

        /// Granularity of a packed path point on the wire. ByteBuffer::appendPackXYZ
        /// stores each axis as `(int)(offset / 0.25f)` -- truncated, not rounded -- so
        /// everything inside one quarter-yard bucket arrives as the same coordinate.
        const float WIRE_STEP = 0.25f;

        /// Shortest segment worth sending: enough to tell two points apart, and no more.
        ///
        /// This once tried to clear WIRE_STEP, on the reasoning that a packed point is
        /// quantised and two points in one bucket arrive as one. That reasoning covers
        /// only the INTERMEDIATE points of a packed path -- the first and last go out as
        /// raw floats, and a Catmull-Rom path is not packed at all -- but the floor was
        /// applied to every point of every path. Half a yard is real distance: legs
        /// shorter than that were dropped whole, and a creature with closely spaced
        /// waypoints stopped between them instead of walking.
        ///
        /// What the client actually chokes on is two points it sees as the SAME point,
        /// and the case that produces it is exact: a patrol restarting from the node it
        /// just waited on, where the forced first point IS the next waypoint. A tenth of
        /// a yard names that and takes nothing real with it.
        const float MIN_SEGMENT = 0.1f;

        /// Strip consecutive points the client would see as one, in place, and answer how
        /// many remain.
        ///
        /// The server forces a leg's first point to where it believes the mover is. When a
        /// patrol reaches a node, waits, and sets off again from that node, that forced
        /// point IS the next waypoint, and the path leaves with two identical points at
        /// the front. The client does not deduplicate -- only its cyclic path does -- so it
        /// builds a segment of no length and divides by it.
        ///
        /// Keeping the FIRST of each run matters: the first point is the mover's own
        /// position, and replacing it with a waypoint that merely rounds to the same place
        /// would move the leg's start off the mover.
        inline uint16_t StripDeadSegments(Geometry::Vector3* points, uint16_t count,
                                          float minSegment = MIN_SEGMENT)
        {
            if (!points || count == 0)
            {
                return 0;
            }
            const float floorSq = minSegment * minSegment;
            uint16_t kept = 1;
            for (uint16_t i = 1; i < count; ++i)
            {
                if ((points[i] - points[kept - 1]).squaredMagnitude() >= floorSq)
                {
                    points[kept++] = points[i];
                }
            }
            return kept;
        }

        /// HOW FAR A PACKED PATH MAY REACH FROM ITS OWN MIDPOINT.
        ///
        /// A linear path does not send its intermediate points. It sends the first and the
        /// last as raw floats and every point between them as an offset from
        /// midpoint(first, last), quantised at WIRE_STEP and truncated into signed fields
        /// of 11, 11 and 10 bits (ByteBuffer::appendPackXYZ; the client decodes it in
        /// sub_5AA9D0). Eleven signed bits of quarter-yards reach 256 yd, ten reach 128.
        ///
        /// One yard past that the field does not clamp, it WRAPS: the offset comes out
        /// with the opposite sign and the client walks to a point on the other side of the
        /// mover. The test is per point and against the midpoint, not against a bounding
        /// box -- a path whose two ends are close together can still swing far from the
        /// line between them, and a box would pass it.
        const float PACK_REACH_XY = 256.0f;
        const float PACK_REACH_Z = 128.0f;

        inline bool PacksWithoutWrapping(const Geometry::Vector3* points, uint16_t count)
        {
            if (!points || count < 3)
            {
                return true;
            }
            const Geometry::Vector3 middle = (points[0] + points[count - 1]) * 0.5f;
            for (uint16_t i = 1; i + 1 < count; ++i)
            {
                const Geometry::Vector3 offset = middle - points[i];
                if (offset.x <= -PACK_REACH_XY || offset.x >= PACK_REACH_XY ||
                    offset.y <= -PACK_REACH_XY || offset.y >= PACK_REACH_XY ||
                    offset.z <= -PACK_REACH_Z || offset.z >= PACK_REACH_Z)
                {
                    return false;
                }
            }
            return true;
        }

        /// The longest leading run of a polyline that survives packing, never fewer than
        /// two points. Shortening the run moves the midpoint as well as dropping points,
        /// so this walks down rather than assuming the property is monotone.
        inline uint16_t PackableRun(const Geometry::Vector3* points, uint16_t count)
        {
            for (uint16_t run = count; run > 2; --run)
            {
                if (PacksWithoutWrapping(points, run))
                {
                    return run;
                }
            }
            return count >= 2 ? uint16_t(2) : count;
        }

        /// What the client will make of a leg we are about to send.
        struct Verdict
        {
            float ceiling = 0.0f;
            float askedSpeed = 0.0f;
            /// The leg asks to travel faster than the client permits. Always our bug: the
            /// ceiling is four times the mover's own speed, so nothing legitimate reaches it.
            bool tooFast = false;
            uint32_t ourDuration = 0;
            uint32_t theirDuration = 0;
            /// Milliseconds the client will add to our arrival. Positive means late.
            int32_t stretchMs = 0;
        };

        inline Verdict Inspect(float length, uint32_t ourDuration, float moverSpeed,
                               uint32_t splineFlags)
        {
            Verdict v;
            v.ceiling = SpeedCeiling(moverSpeed, splineFlags);
            v.ourDuration = ourDuration;
            v.askedSpeed = (ourDuration != 0) ? length / (float(ourDuration) * 0.001f) : 0.0f;
            v.tooFast = ExceedsCeiling(length, ourDuration, v.ceiling);
            v.theirDuration = Retime(length, ourDuration, v.ceiling);
            v.stretchMs = int32_t(v.theirDuration) - int32_t(ourDuration);
            return v;
        }
    }
}
