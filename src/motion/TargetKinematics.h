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

#ifndef MANGOS_MOTION_TARGETKINEMATICS_H
#define MANGOS_MOTION_TARGETKINEMATICS_H

#include "MoveIntent.h"

namespace Motion
{
    /// What the shell can say about a target's motion without interpreting it (design §3).
    struct TargetMotionInput
    {
        bool  splineRunning = false;   ///< a server spline is in progress
        bool  splineLinear = false;    ///< not Catmull-Rom (isSmooth() false)
        bool  splineCyclic = false;    ///< isCyclic()
        bool  splineAirborne = false;  ///< Airborne(): a jump, a fall, a knockback
        Vector3 splineFrom;            ///< the live spline position (frame coordinates)
        Vector3 splineTo;              ///< the spline's current destination (frame coordinates)
        bool  playerMoved = false;     ///< a client-moved unit: the flags below apply
        bool  forward = false, backward = false, strafeLeft = false, strafeRight = false;
        bool  falling = false;         ///< MOVEFLAG_FALLING | FALLING_FAR
        float facing = 0.0f;           ///< the unit's facing (frame)
        float speed = 0.0f;            ///< the unit's speed for the mode its flags select (yd/s); the spline's when one runs
    };

    /// The classified answer: a velocity to lead with, or none.
    struct TargetMotion
    {
        Vector3 velocity;              ///< frame yd/s; zero when standing or untrusted
        bool    trusted = false;       ///< a lead may use it
        bool    moving = false;        ///< the target is in motion (trusted or not)
    };

    /// Design §3: a linear, non-cyclic, non-airborne spline is trusted (the chord to its current
    /// destination at its speed); a smooth, cyclic or ballistic spline moves but is untrusted; a
    /// client-moved unit combines its forward/backward and strafe flags, normalised, at its speed,
    /// untrusted while falling; anything else stands.
    TargetMotion ClassifyTargetMotion(TargetMotionInput const& in);
}

#endif
