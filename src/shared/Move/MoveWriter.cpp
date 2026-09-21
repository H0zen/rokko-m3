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

#include "Move/MoveWriter.h"
#include "Move/ClientRules.h"

#include <vector>

namespace Move
{
    namespace
    {
        bool Finite(const Vector3& p)
        {
            return p.isFinite();
        }
    }

    const char* MoveWriter::Why(Refusal refusal)
    {
        switch (refusal)
        {
            case Refusal::None:           return "none";
            case Refusal::TooFewPoints:   return "too few points";
            case Refusal::NoLength:       return "no length";
            case Refusal::ExceedsCeiling: return "exceeds the client's ceiling";
            case Refusal::OutOfPackReach: return "out of packed reach";
            case Refusal::NotFinite:      return "not finite";
        }
        return "unknown";
    }

    Written MoveWriter::Write(const Route& route, float moverSpeed, uint32_t extraFlags)
    {
        if (!route.Running())
        {
            Written refused;
            refused.refusal = Refusal::TooFewPoints;
            return refused;
        }
        return Write(&route.Point(0), route.Count(), route.Speed(), moverSpeed, extraFlags);
    }

    Written MoveWriter::Write(const Vector3* points, uint16_t count, float speed,
                              float moverSpeed, uint32_t extraFlags)
    {
        Written out;

        if (!points || count < 2)
        {
            out.refusal = Refusal::TooFewPoints;
            return out;
        }

        // A non-finite coordinate is not a leg with a bad shape, it is a leg with no shape.
        // The client's decoder has no answer for it and neither does any test downstream,
        // so it stops here rather than becoming a packet nobody can reason about.
        for (uint16_t i = 0; i < count; ++i)
        {
            if (!Finite(points[i]))
            {
                out.refusal = Refusal::NotFinite;
                return out;
            }
        }

        // The client deduplicates nothing but a cyclic path's closing point, so anything
        // it would see as two identical points has to go before it is sent. Keeping the
        // FIRST of each run matters: point zero is where the mover is, and replacing it
        // with a later point that merely rounds to the same place moves the leg's start.
        std::vector<Vector3> kept(points, points + count);
        const uint16_t remaining = Client::StripDeadSegments(&kept[0], count);
        if (remaining < 2)
        {
            out.refusal = Refusal::NoLength;
            return out;
        }
        kept.resize(remaining);

        // Everything that leaves here is linear, so the packed fields apply and a middle
        // point beyond their reach would come back with its sign flipped. The caller can
        // ask for an uncompressed path, which has no reach limit because nothing is packed.
        const bool packed = (extraFlags & SPLINE_UNCOMPRESSED) == 0;
        if (packed && !Client::PacksWithoutWrapping(&kept[0], remaining))
        {
            out.refusal = Refusal::OutOfPackReach;
            return out;
        }

        // The chord sum, which is exactly what the client will measure: it walks the point
        // array adding straight distances, whatever the flags say about corners.
        float length = 0.0f;
        for (uint16_t i = 1; i < remaining; ++i)
        {
            length += (kept[i] - kept[i - 1]).magnitude();
        }
        if (length <= 0.0f)
        {
            out.refusal = Refusal::NoLength;
            return out;
        }

        const float pace = speed > 0.0f ? speed : moverSpeed;
        if (pace <= 0.0f)
        {
            out.refusal = Refusal::NoLength;
            return out;
        }

        uint32_t duration = uint32_t((length / pace) * 1000.0f);
        if (duration < 1)
        {
            duration = 1;
        }

        // Above the ceiling the client does not refuse, it stretches, and the mover arrives
        // late with nothing on the wire to say so. Since the server's own position function
        // is only correct while the client agrees with it, that silent stretch is not a
        // performance question, it is a correctness one.
        uint32_t flags = extraFlags;
        if (Client::ExceedsCeiling(length, duration, Client::SpeedCeiling(moverSpeed, flags)))
        {
            out.refusal = Refusal::ExceedsCeiling;
            return out;
        }

        // Ask the client to round the corners. It builds the entry curve from the facing
        // the mover already has and extends the exit by up to two yards, which is what a
        // Catmull-Rom path was for -- without the chord-versus-arc error that one carries,
        // because the points we timed are still the points it walks.
        if ((flags & (SPLINE_UNCOMPRESSED | SPLINE_PARABOLA | SPLINE_FALLING)) == 0)
        {
            flags |= SPLINE_ROUND_CORNERS;
        }

        out.start = kept.front();
        out.destination = kept.back();
        out.middle.assign(kept.begin() + 1, kept.end() - 1);
        out.duration = duration;
        out.flags = flags;
        out.length = length;
        out.speed = length / (float(duration) * 0.001f);
        return out;
    }
}
