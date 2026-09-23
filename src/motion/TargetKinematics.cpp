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

#include "TargetKinematics.h"

#include <cmath>

namespace Motion
{
    /**
     * @brief Classifies what the shell reported about a target's motion.
     * @param in The shell's raw observation.
     * @return The velocity a native may lead with, and whether the target moves at all.
     */
    TargetMotion ClassifyTargetMotion(TargetMotionInput const& in)
    {
        TargetMotion out;
        if (in.splineRunning)
        {
            // The chord is taken in 3D: a climbing or diving target (a flyer, a swimmer, a
            // ramp) travels its vertical too, and flattening it both shortened the velocity
            // and pointed it the wrong way -- a vertical-only leg read as standing still.
            const Vector3 chord = in.splineTo - in.splineFrom;
            const float len = chord.length();
            out.moving = len > 0.01f && in.speed > 0.0f;
            if (!out.moving) { return out; }

            // A cycle wraps and a parabola falls: neither may be extrapolated over a horizon,
            // so both move untrusted. Smoothness is NOT in that company -- see below.
            if (in.splineCyclic || in.splineAirborne) { return out; }

            if (in.splineLinear)
            {
                out.velocity = chord * (in.speed / len);
                out.trusted = true;
                return out;
            }

            // A Catmull-Rom. This used to be refused outright, and the reason given was true
            // of the CHORD: it runs to the current destination, and on a curve that is not the
            // way the unit is going. But the curve knows which way it is going -- that is its
            // derivative -- and once the shell hands that over there is nothing left to refuse.
            // The refusal cost real behaviour: while it stood, any leader on a routed leg the
            // pathfinder had bent went untrusted, which silently switched off the follow's
            // horizon (Movement.FollowHorizonMs) and the chase's lead (CHASE_LEAD_MS) for as
            // long as that leg ran.
            if (!in.splineHeadingValid) { return out; }
            const float heading = in.splineHeading.length();
            if (heading <= 0.0f) { return out; }
            out.velocity = in.splineHeading * (in.speed / heading);
            out.trusted = true;
            return out;
        }
        if (in.playerMoved)
        {
            const float ahead = (in.forward ? 1.0f : 0.0f) - (in.backward ? 1.0f : 0.0f);
            const float side = (in.strafeLeft ? 1.0f : 0.0f) - (in.strafeRight ? 1.0f : 0.0f);
            if ((ahead == 0.0f && side == 0.0f) || in.speed <= 0.0f) { return out; }
            out.moving = true;
            if (in.falling) { return out; }
            // forward is +facing; left is +90 degrees from it
            const float c = std::cos(in.facing), s = std::sin(in.facing);
            Vector3 dir(ahead * c - side * s, ahead * s + side * c, 0.0f);
            const float n = dir.length();
            out.velocity = dir * (in.speed / n);
            out.trusted = true;
        }
        return out;
    }
}
