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

#ifndef MANGOS_MOTION_MOVEINTENT_H
#define MANGOS_MOTION_MOVEINTENT_H

#include "Platform/Define.h"
#include "Geometry/Vector3.h"
#include "Utilities/MathDefines.h"

#include <cmath>
#include <vector>

/**
 * The intent model's values (design v2 §4.4; P5-B family 1 §3): what a behaviour wants
 * this tick, and what the shell's driver reports about the live leg. Kernel-safe: a
 * target is an opaque raw guid, a path a vector of points; the shell translates.
 */
namespace Motion
{
    using Vector3 = Geometry::Vector3;
    using PointsArray = std::vector<Vector3>;

    /// The 2D bearing from one frame point to another, normalised to [0, 2*PI) as
    /// WorldObject::GetAngle normalises; distances survive a change of frame, angles do not.
    inline float AngleFromTo(Vector3 const& from, Vector3 const& to)
    {
        const float a = std::atan2(to.y - from.y, to.x - from.x);
        return (a >= 0.0f) ? a : (2 * M_PI_F + a);
    }

    /// The orientation a leg ends in (the client faces the travel direction while moving).
    struct Facing
    {
        enum class Mode : uint8
        {
            None,   ///< Keep facing the travel direction (the default).
            Angle,  ///< Hold a fixed heading.
            Target, ///< Point at a unit (a raw guid the shell resolves).
            Spot    ///< Face a fixed point.
        };

        Mode    mode = Mode::None;
        float   angle = 0.0f;
        uint64  target = 0;
        Vector3 spot;

        static Facing ToAngle(float o) { Facing f; f.mode = Mode::Angle; f.angle = o; return f; }
        static Facing ToTarget(uint64 rawGuid) { Facing f; f.mode = Mode::Target; f.target = rawGuid; return f; }
        static Facing ToSpot(Vector3 const& p) { Facing f; f.mode = Mode::Spot; f.spot = p; return f; }
    };

    /// Per-leg modifiers, orthogonal to where the leg is going.
    enum MoveFlags : uint32
    {
        MOVE_NONE         = 0x00,
        MOVE_WALK         = 0x01, ///< Walk pace, else run.
        MOVE_FLY          = 0x02, ///< Catmull-Rom spline + flying animation.
        MOVE_STRAIGHT     = 0x04, ///< Do not route: go straight there.
        MOVE_FORCE_DEST   = 0x08, ///< Arrive at the exact goal even if unroutable.
        MOVE_REQUIRE_PATH = 0x10  ///< Refuse the leg when the router found no real path.
    };

    /// How an Effect launches its spline at first selection (a jump, a knockback arc, a fall).
    struct EffectLaunch
    {
        enum Kind : uint8 { None, Jump, Fall };
        Kind    kind = None;
        Vector3 point;             ///< the landing point (a fall: the floor under the unit)
        float   speed = 0.0f;      ///< horizontal speed (jump)
        float   height = 0.0f;     ///< parabola height (jump)
        Facing  facing;            ///< the facing jump ends facing this (Mode::None: the travel direction)
    };

    /**
     * What a behaviour wants this tick. Hold: stay put (optionally turning to `facing`).
     * Move: travel to `goal`, the driver routes and lays the leg. Launch: the Effect's
     * spline, once. Done: the behaviour has finished and asks to be retired.
     */
    struct MoveIntent
    {
        enum class Act : uint8 { Hold, Move, Launch, Done };

        Act          act = Act::Hold;
        Vector3      goal;                  ///< Move: destination, in WORLD coordinates (the shell converts to the mover's frame).
        Facing       facing;
        uint32       flags = MOVE_NONE;
        float        speed = 0.0f;          ///< Move: a speed override in yd/s; 0 = the unit's own pace.
        float        pathLengthLimit = 0.0f;
        PointsArray const* path = nullptr;  ///< Move: exact geometry (non-owning, stable for the leg's life).
        EffectLaunch launch;                ///< Launch: the spline.

        bool Has(MoveFlags f) const { return (flags & f) != 0; }

        static MoveIntent Hold(Facing f = {}) { MoveIntent i; i.act = Act::Hold; i.facing = f; return i; }
        static MoveIntent Move(Vector3 const& to, uint32 moveFlags = MOVE_NONE, Facing f = {})
        {
            MoveIntent i; i.act = Act::Move; i.goal = to; i.flags = moveFlags; i.facing = f; return i;
        }
        static MoveIntent Launch(EffectLaunch const& l) { MoveIntent i; i.act = Act::Launch; i.launch = l; return i; }
        static MoveIntent Done() { MoveIntent i; i.act = Act::Done; return i; }

        MoveIntent& Along(PointsArray const& points) { path = &points; return *this; }
        MoveIntent& WithinLength(float yards) { pathLengthLimit = yards; return *this; }
        MoveIntent& AtSpeed(float ydPerSec) { speed = ydPerSec; return *this; }
    };

    /// The driver's read-only view of leg progress. `arrived`, `cut`, `partial` and `blocked` are edges: reported once, on the tick after the event.
    struct MoveStatus
    {
        bool    traveling = false;
        bool    arrived = false;
        bool    cut = false;
        bool    partial = false;
        bool    blocked = false;
        int32   pathIndex = 0;
        Vector3 legGoal;
    };
}

#endif
