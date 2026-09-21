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

#include "Move/Aim.h"

#include "Geometry/GeometryMath.h"

#include <cmath>

namespace Move
{
    namespace
    {
        using Geometry::Vector3;

        /// Two points this close in the plane have no direction between them.
        const float NO_DIRECTION_SQ = 1e-8f;

        Vector3 Around(const Vector3& centre, float distance, float bearing)
        {
            return Vector3(centre.x + distance * std::cos(bearing),
                           centre.y + distance * std::sin(bearing),
                           centre.z);
        }

        /// The flat bearing from `from` to `to`, or the fallback when they coincide.
        bool FlatBearing(const Vector3& from, const Vector3& to, float& out)
        {
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            if (dx * dx + dy * dy < NO_DIRECTION_SQ)
            {
                return false;
            }
            out = std::atan2(dy, dx);
            return true;
        }
    }

    Geometry::Vector3 ContactPoint(const Sight& sight, float gap)
    {
        float bearing = 0.0f;
        if (!FlatBearing(sight.at, sight.target, bearing))
        {
            return sight.at;
        }

        const float reach = gap + sight.extent + sight.targetExtent;
        // Measured BACK from the target, not forward from the mover: forward would need
        // the separation, and when the mover is already inside the reach that overshoots
        // through the target and out the far side.
        return Around(sight.target, reach, bearing + Geometry::pif());
    }

    Geometry::Vector3 StationPoint(const Sight& sight, float distance, float angle)
    {
        return Around(sight.target, distance, sight.targetFacing + angle);
    }

    Geometry::Vector3 WanderPoint(const Sight& sight, float minRadius, float maxRadius,
                                  float bearingRoll, float distanceRoll)
    {
        if (maxRadius < minRadius)
        {
            maxRadius = minRadius;
        }

        // sqrt on the roll spreads the picks evenly over the ring's AREA. Without it the
        // radius is uniform, which puts half of every draw inside the inner quarter of the
        // circle and makes a wanderer look tethered to its spawn.
        const float t = distanceRoll <= 0.0f ? 0.0f : std::sqrt(distanceRoll);
        const float distance = minRadius + t * (maxRadius - minRadius);
        const float bearing = bearingRoll * 2.0f * Geometry::pif();

        return Around(sight.anchor, distance, bearing);
    }

    Geometry::Vector3 FleePoint(const Sight& sight, const FleeBand& band,
                                float bearingRoll, float distanceRoll)
    {
        // The bearing FROM the fright TO the mover: the way it would run if nothing else
        // mattered. A fright the mover is standing inside has no direction, so the draw
        // stands in and it bolts somewhere rather than nowhere.
        float away = bearingRoll * 2.0f * Geometry::pif();
        (void)FlatBearing(sight.target, sight.at, away);

        const float dx = sight.at.x - sight.target.x;
        const float dy = sight.at.y - sight.target.y;
        const float distance = std::sqrt(dx * dx + dy * dy);

        const float jitter = (distanceRoll * 2.0f - 1.0f) * band.jitter;
        const float span = band.maxQuiet - band.minQuiet;

        if (distance < band.minQuiet)
        {
            // Too close: straight out, and far enough to clear the band.
            return Around(sight.at, band.minQuiet - distance, away + jitter);
        }
        if (distance > band.maxQuiet)
        {
            // Beyond the band, so back toward the fright -- the bearing plus pi, because
            // the stored bearing points away from it.
            return Around(sight.at, span, away + Geometry::pif() + jitter);
        }
        // Inside the band: mill about. The bearing is the draw, not the fright's, which is
        // what stops a frightened thing running in one straight line until the fear ends.
        return Around(sight.at, span, bearingRoll * 2.0f * Geometry::pif());
    }

    bool WorthReAiming(const Geometry::Vector3& aimedAt, const Geometry::Vector3& wanted,
                       float tolerance)
    {
        const Vector3 drift = wanted - aimedAt;
        return drift.squaredMagnitude() > tolerance * tolerance;
    }

    float ReAimTolerance(float extent)
    {
        const float half = extent * 0.5f;
        return half < 0.25f ? 0.25f : half;
    }
}
