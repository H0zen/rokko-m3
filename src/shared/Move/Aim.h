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

// WHERE DOES THIS THING WANT TO BE? -- the whole of what the seven movement behaviours
// actually decide, which is one point each.
//
// Chase, follow, wander, patrol, home, flee and point are not seven systems. Three of them
// return a point somebody already stored -- a patrol node, a spawn anchor, a scripted
// destination -- and need no code at all. The four below compute one, and each is a dozen
// lines of geometry. What they have in common is not a base class, it is a signature: look
// at the world, answer with a point.
//
// RANDOMNESS IS INJECTED, never drawn here. A wander that calls rand() cannot be tested,
// cannot be replayed from a scenario, and cannot be reproduced from a bug report. Every
// roll arrives as a number in [0, 1).
//
// NOTHING HERE TOUCHES A UNIT, a map or the ground. These answer where the mover wants to
// be; whether it can stand there is the terrain's business and the router's, and asking
// them is the caller's job after this returns.

#include "Geometry/Vector3.h"

#include <cstdint>

namespace Move
{
    /// What a picker is allowed to look at. Filled by the caller; no pointers into the
    /// game, so a scenario can build one out of numbers and get the same answers.
    struct Sight
    {
        Geometry::Vector3 at;             ///< where the mover is
        float facing = 0.0f;
        float extent = 0.0f;              ///< the mover's own radius

        Geometry::Vector3 target;         ///< a chase or follow target; a fright source for flee
        float targetFacing = 0.0f;
        float targetExtent = 0.0f;

        Geometry::Vector3 anchor;         ///< spawn point, for wander and home
    };

    /// The point a chaser wants to stand on: short of the target by `gap`, on the line
    /// between the two, so the bodies end up touching rather than interpenetrating.
    ///
    /// Returns the mover's own position when the two are already on top of each other --
    /// there is no line to stand on then, and inventing a direction would send the mover
    /// somewhere arbitrary. Standing still is the honest answer, and the caller's re-aim
    /// test will keep it still.
    Geometry::Vector3 ContactPoint(const Sight& sight, float gap);

    /// The point a follower keeps station on: `distance` from the target, at `angle`
    /// measured from the target's own facing. A pet at angle pi walks behind its owner and
    /// turns with them, which is what makes a follow look like a follow and not a chase.
    Geometry::Vector3 StationPoint(const Sight& sight, float distance, float angle);

    /// A point to wander to, somewhere in the ring between `minRadius` and `maxRadius`
    /// around the anchor. Both rolls are in [0, 1): one for the bearing, one for how far.
    ///
    /// The distance roll is spread over the AREA, not the radius, so points do not pile up
    /// near the middle -- a uniform roll on the radius puts half the picks in the inner
    /// quarter of the circle and makes a wanderer look tethered.
    Geometry::Vector3 WanderPoint(const Sight& sight, float minRadius, float maxRadius,
                                  float bearingRoll, float distanceRoll);

    /// Bounds a flee keeps the mover inside, relative to whatever frightened it.
    struct FleeBand
    {
        float minQuiet = 0.0f;   ///< closer than this, bolt straight away
        float maxQuiet = 0.0f;   ///< further than this, drift back toward the fright
        float jitter = 0.0f;     ///< how much the bearing may wander, radians either side
    };

    /// Where a frightened thing runs. Three cases, and the middle one is the point of the
    /// whole thing: inside the band it mills about rather than running in a straight line
    /// forever, which is what fear looks like.
    ///
    /// Past the band it drifts BACK toward the fright source, and that bearing is the
    /// source's bearing plus pi. Negating the bearing instead mirrors it across the x axis,
    /// which points at the source only on the north-south axis and points straight away on
    /// the east-west one -- a bug this signature exists to make hard to write.
    Geometry::Vector3 FleePoint(const Sight& sight, const FleeBand& band,
                                float bearingRoll, float distanceRoll);

    /// Is a new destination worth laying a leg for?
    ///
    /// The question that decides what a chase costs. A target that shuffled a few inches
    /// has not moved anywhere, and re-aiming at it spends a spline packet, a route query
    /// and a scheduler batch to arrive at the same place. Anything under the tolerance is
    /// the same destination.
    bool WorthReAiming(const Geometry::Vector3& aimedAt, const Geometry::Vector3& wanted,
                       float tolerance);

    /// The tolerance a chase should use for its own body: half its extent, never less than
    /// a quarter yard. Scaling with the mover means a dragon does not re-aim over a
    /// distance that is nothing to it, while a rat still tracks closely.
    float ReAimTolerance(float extent);
}
