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

// The tracked target's kinematics (P5-B family 3 design section 3): what the shell reports
// about a target's motion, classified into a velocity a native may lead with, or none.
// Pure: no unit, no spline, no frame.

#include "TestHarness.h"
#include "TargetKinematics.h"
#include "Utilities/MathDefines.h"

#include <cmath>

using namespace Motion;

namespace
{
    /// A spline input: running, with the flags the case is about.
    TargetMotionInput Spline(Vector3 const& from, Vector3 const& to, float speed)
    {
        TargetMotionInput in;
        in.splineRunning = true;
        in.splineLinear = true;
        in.splineFrom = from;
        in.splineTo = to;
        in.speed = speed;
        return in;
    }

    /// A client-moved input at the given facing and speed; the caller sets the flags.
    TargetMotionInput Player(float facing, float speed)
    {
        TargetMotionInput in;
        in.playerMoved = true;
        in.facing = facing;
        in.speed = speed;
        return in;
    }

    bool Near(float a, float b, float tol = 0.001f) { return std::fabs(a - b) <= tol; }
}

TEST(TargetKinematics_LinearSplineIsTrustedAlongItsChord)
{
    // Due east, 10 yards to go, 7 yd/s: the velocity is the chord's direction at the speed,
    // not the chord itself -- the leg's length says nothing about how fast it is walked.
    const TargetMotion m = ClassifyTargetMotion(Spline(Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), 7.0f));
    CHECK(m.moving);
    CHECK(m.trusted);
    CHECK(Near(m.velocity.x, 7.0f));
    CHECK(Near(m.velocity.y, 0.0f));
    CHECK(Near(m.velocity.z, 0.0f));
    CHECK(Near(m.velocity.length(), 7.0f));
}

TEST(TargetKinematics_SmoothSplineMovesButIsNotTrusted)
{
    // A Catmull-Rom leg curves between its points, so the chord to the current destination is
    // not the direction of travel: the target moves, but nothing may lead on it.
    TargetMotionInput in = Spline(Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), 7.0f);
    in.splineLinear = false;
    const TargetMotion m = ClassifyTargetMotion(in);
    CHECK(m.moving);
    CHECK(!m.trusted);
    CHECK_EQ(m.velocity.length(), 0.0f);
}

TEST(TargetKinematics_CyclicSplineMovesButIsNotTrusted)
{
    TargetMotionInput in = Spline(Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), 7.0f);
    in.splineCyclic = true;
    const TargetMotion m = ClassifyTargetMotion(in);
    CHECK(m.moving);
    CHECK(!m.trusted);
    CHECK_EQ(m.velocity.length(), 0.0f);
}

TEST(TargetKinematics_AirborneSplineMovesButIsNotTrusted)
{
    // A jump, a fall or a knockback: the horizontal chord is real but the arc is not walked
    // at the unit's ground speed, and it ends wherever the parabola lands.
    TargetMotionInput in = Spline(Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f), 7.0f);
    in.splineAirborne = true;
    const TargetMotion m = ClassifyTargetMotion(in);
    CHECK(m.moving);
    CHECK(!m.trusted);
    CHECK_EQ(m.velocity.length(), 0.0f);
}

TEST(TargetKinematics_SplineWithNowhereToGoStands)
{
    // The destination is where the unit already is: a finished or a turn-in-place leg.
    const TargetMotion m = ClassifyTargetMotion(Spline(Vector3(4.0f, 5.0f, 6.0f), Vector3(4.0f, 5.0f, 6.0f), 7.0f));
    CHECK(!m.moving);
    CHECK(!m.trusted);
    CHECK_EQ(m.velocity.length(), 0.0f);
}

TEST(TargetKinematics_PlayerRunningForwardFollowsItsFacing)
{
    TargetMotionInput in = Player(M_PI_F / 2.0f, 7.0f);   // due north
    in.forward = true;
    const TargetMotion m = ClassifyTargetMotion(in);
    CHECK(m.moving);
    CHECK(m.trusted);
    CHECK(Near(m.velocity.x, 0.0f));
    CHECK(Near(m.velocity.y, 7.0f));
    CHECK(Near(m.velocity.length(), 7.0f));
}

TEST(TargetKinematics_PlayerStrafingWhileForwardMovesOnANormalisedDiagonal)
{
    // Forward and strafe-left together: the client walks the 45 degree diagonal AT THE SPEED,
    // not at sqrt(2) times it, so the combined direction is normalised before it is scaled.
    TargetMotionInput in = Player(0.0f, 7.0f);   // facing due east
    in.forward = true;
    in.strafeLeft = true;
    const TargetMotion m = ClassifyTargetMotion(in);
    CHECK(m.moving);
    CHECK(m.trusted);
    CHECK(Near(m.velocity.length(), 7.0f));
    CHECK(Near(m.velocity.x, 7.0f / std::sqrt(2.0f)));
    CHECK(Near(m.velocity.y, 7.0f / std::sqrt(2.0f)));   // left of due east is north
}

TEST(TargetKinematics_FallingPlayerMovesButIsNotTrusted)
{
    TargetMotionInput in = Player(0.0f, 7.0f);
    in.forward = true;
    in.falling = true;
    const TargetMotion m = ClassifyTargetMotion(in);
    CHECK(m.moving);
    CHECK(!m.trusted);
    CHECK_EQ(m.velocity.length(), 0.0f);
}

TEST(TargetKinematics_PlayerWithNoMovementFlagsStands)
{
    const TargetMotion m = ClassifyTargetMotion(Player(1.0f, 7.0f));
    CHECK(!m.moving);
    CHECK(!m.trusted);
    CHECK_EQ(m.velocity.length(), 0.0f);
}
