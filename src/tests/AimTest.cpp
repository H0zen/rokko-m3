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

#include "TestHarness.h"

#include "Geometry/GeometryMath.h"
#include "Move/Aim.h"

#include <cmath>

using Geometry::Vector3;
using Move::Sight;

namespace
{
    bool Near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }

    bool NearPoint(const Vector3& a, const Vector3& b, float tol = 0.01f)
    {
        return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
    }

    float Flat(const Vector3& a, const Vector3& b)
    {
        const float dx = b.x - a.x, dy = b.y - a.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    /// A chaser at the origin and a target ten yards east.
    Sight Facing()
    {
        Sight s;
        s.at = Vector3(0.f, 0.f, 0.f);
        s.extent = 1.0f;
        s.target = Vector3(10.f, 0.f, 0.f);
        s.targetExtent = 2.0f;
        s.anchor = Vector3(0.f, 0.f, 0.f);
        return s;
    }
}

// ---------------------------------------------------------------------------------
//  Chase
// ---------------------------------------------------------------------------------

TEST(ContactPointStopsShortOfTheTargetByBothBodiesAndTheGap)
{
    const Sight s = Facing();
    const Vector3 stand = Move::ContactPoint(s, 0.5f);

    // 1 + 2 + 0.5 back from the target, along the line, so the bodies touch.
    CHECK(NearPoint(stand, Vector3(6.5f, 0.f, 0.f)));
    CHECK(Near(Flat(stand, s.target), 3.5f));
}

// Measured back from the target rather than forward from the mover: a chaser already
// inside the reach must step OUT to the rim, not push through and come out the far side.
TEST(ContactPointPullsAChaserThatIsTooCloseBackOut)
{
    Sight s = Facing();
    s.at = Vector3(9.f, 0.f, 0.f);          // only a yard away, well inside the reach

    const Vector3 stand = Move::ContactPoint(s, 0.5f);
    CHECK(NearPoint(stand, Vector3(6.5f, 0.f, 0.f)));
    CHECK(Near(Flat(stand, s.target), 3.5f));
}

// Two bodies exactly on top of each other have no line between them. Inventing a bearing
// would fling the mover somewhere arbitrary; standing still is the honest answer.
TEST(ContactPointStandsStillWhenThereIsNoLineToStandOn)
{
    Sight s = Facing();
    s.at = s.target;

    CHECK(NearPoint(Move::ContactPoint(s, 0.5f), s.at));
}

TEST(ContactPointWorksFromEveryDirection)
{
    for (int i = 0; i < 16; ++i)
    {
        const float bearing = float(i) * Geometry::pif() / 8.0f;
        Sight s = Facing();
        s.at = Vector3(10.f + 20.f * std::cos(bearing), 20.f * std::sin(bearing), 0.f);

        const Vector3 stand = Move::ContactPoint(s, 0.5f);
        CHECK(Near(Flat(stand, s.target), 3.5f));
    }
}

// ---------------------------------------------------------------------------------
//  Follow
// ---------------------------------------------------------------------------------

// A follower keeps station on the TARGET'S facing, which is what makes a pet walk behind
// its owner and swing round as the owner turns, instead of trailing wherever it came from.
TEST(StationPointTurnsWithTheTargetNotWithTheFollower)
{
    Sight s = Facing();
    s.targetFacing = 0.0f;                       // target looking east

    const Vector3 behind = Move::StationPoint(s, 3.0f, Geometry::pif());
    CHECK(NearPoint(behind, Vector3(7.f, 0.f, 0.f)));

    s.targetFacing = 0.5f * Geometry::pif();     // now looking north
    const Vector3 turned = Move::StationPoint(s, 3.0f, Geometry::pif());
    CHECK(NearPoint(turned, Vector3(10.f, -3.f, 0.f)));
}

TEST(StationPointHoldsItsDistanceWhateverTheAngle)
{
    Sight s = Facing();
    for (int i = 0; i < 12; ++i)
    {
        const float angle = float(i) * Geometry::pif() / 6.0f;
        CHECK(Near(Flat(Move::StationPoint(s, 4.0f, angle), s.target), 4.0f));
    }
}

// ---------------------------------------------------------------------------------
//  Wander
// ---------------------------------------------------------------------------------

TEST(WanderStaysInsideItsRing)
{
    const Sight s = Facing();
    for (int b = 0; b <= 10; ++b)
    {
        for (int d = 0; d <= 10; ++d)
        {
            const Vector3 p = Move::WanderPoint(s, 2.0f, 8.0f, b / 11.0f, d / 11.0f);
            const float r = Flat(p, s.anchor);
            CHECK(r >= 2.0f - 0.01f);
            CHECK(r <= 8.0f + 0.01f);
        }
    }
}

// A uniform roll on the RADIUS puts half of every draw in the inner quarter of the circle
// and makes a wanderer look tethered. Spreading over the area fixes it, and the test that
// catches the difference is where the median lands.
TEST(WanderSpreadsOverTheAreaNotTheRadius)
{
    const Sight s = Facing();
    const Vector3 median = Move::WanderPoint(s, 0.0f, 10.0f, 0.0f, 0.5f);

    // Half the draws inside, half outside, is the radius that halves the AREA: r/sqrt(2).
    CHECK(Near(Flat(median, s.anchor), 10.0f / std::sqrt(2.0f), 0.05f));
}

TEST(WanderSurvivesADegenerateRing)
{
    const Sight s = Facing();
    const Vector3 p = Move::WanderPoint(s, 5.0f, 1.0f, 0.3f, 0.7f);
    CHECK(Near(Flat(p, s.anchor), 5.0f));
    CHECK(p.isFinite());
}

// ---------------------------------------------------------------------------------
//  Flee
// ---------------------------------------------------------------------------------

namespace
{
    Move::FleeBand Band()
    {
        Move::FleeBand b;
        b.minQuiet = 8.0f;
        b.maxQuiet = 20.0f;
        b.jitter = 0.0f;                 // no jitter, so the bearing can be asserted
        return b;
    }
}

TEST(FleeBoltsStraightAwayWhenTheFrightIsOnTopOfIt)
{
    Sight s = Facing();
    s.at = Vector3(2.f, 0.f, 0.f);       // two yards east of the fright at the origin
    s.target = Vector3(0.f, 0.f, 0.f);

    const Vector3 p = Move::FleePoint(s, Band(), 0.5f, 0.5f);
    CHECK(p.x > s.at.x);                                  // further east, away from it
    CHECK(Near(Flat(p, s.target), 8.0f, 0.05f));          // out to the near edge
}

// The case a sign error hides in. Past the band the mover drifts BACK toward the fright;
// negating the bearing instead mirrors it across the x axis, which happens to look right
// on the north-south axis and is exactly backwards on the east-west one.
TEST(FleeDriftsBackTowardTheFrightAndNotItsMirrorImage)
{
    Move::FleeBand band = Band();
    Sight s = Facing();
    s.target = Vector3(0.f, 0.f, 0.f);

    // Approached from four directions; every one has to move the mover closer.
    const Vector3 spots[4] = {Vector3(40.f, 0.f, 0.f), Vector3(-40.f, 0.f, 0.f),
                              Vector3(0.f, 40.f, 0.f), Vector3(0.f, -40.f, 0.f)};
    for (int i = 0; i < 4; ++i)
    {
        s.at = spots[i];
        const Vector3 p = Move::FleePoint(s, band, 0.5f, 0.5f);
        CHECK(Flat(p, s.target) < Flat(s.at, s.target));
    }
}

TEST(FleeMillsAboutInsideTheBandInsteadOfRunningInOneLine)
{
    Sight s = Facing();
    s.target = Vector3(0.f, 0.f, 0.f);
    s.at = Vector3(14.f, 0.f, 0.f);          // comfortably inside 8..20

    // The bearing comes from the draw here, so two different rolls go different ways.
    const Vector3 a = Move::FleePoint(s, Band(), 0.1f, 0.5f);
    const Vector3 b = Move::FleePoint(s, Band(), 0.6f, 0.5f);
    CHECK(!NearPoint(a, b, 1.0f));
}

TEST(FleeStillPicksSomewhereWhenStandingInsideTheFright)
{
    Sight s = Facing();
    s.target = Vector3(5.f, 5.f, 0.f);
    s.at = s.target;

    const Vector3 p = Move::FleePoint(s, Band(), 0.25f, 0.5f);
    CHECK(p.isFinite());
    CHECK(Flat(p, s.at) > 0.0f);
}

// ---------------------------------------------------------------------------------
//  The rule that decides what a chase costs
// ---------------------------------------------------------------------------------

TEST(ReAimingIgnoresATargetThatOnlyShuffled)
{
    const Vector3 aimed(10.f, 10.f, 0.f);

    CHECK(!Move::WorthReAiming(aimed, aimed, 0.5f));
    CHECK(!Move::WorthReAiming(aimed, aimed + Vector3(0.2f, 0.f, 0.f), 0.5f));
    CHECK(Move::WorthReAiming(aimed, aimed + Vector3(0.8f, 0.f, 0.f), 0.5f));
}

TEST(ReAimingCountsHeightAsMovementToo)
{
    const Vector3 aimed(10.f, 10.f, 0.f);
    CHECK(Move::WorthReAiming(aimed, aimed + Vector3(0.f, 0.f, 3.f), 0.5f));
}

// A dragon should not re-aim over a distance that is nothing to it, and a rat should still
// track closely -- so the tolerance scales with the body, with a floor for the small ones.
TEST(ReAimToleranceScalesWithTheBodyButNeverVanishes)
{
    CHECK(Near(Move::ReAimTolerance(0.0f), 0.25f));
    CHECK(Near(Move::ReAimTolerance(0.3f), 0.25f));
    CHECK(Near(Move::ReAimTolerance(8.0f), 4.0f));
}

// The whole point, stated as a measurement: a target jittering under the tolerance costs
// no legs at all, however many times it is looked at.
TEST(AJitteringTargetCostsNoLegs)
{
    Sight s = Facing();
    const float tolerance = Move::ReAimTolerance(s.extent);

    Vector3 aimed = Move::ContactPoint(s, 0.5f);
    int legs = 1;
    for (int i = 0; i < 200; ++i)
    {
        const float wobble = 0.05f * float((i % 5) - 2);
        s.target = Vector3(10.f + wobble, wobble, 0.f);

        const Vector3 wanted = Move::ContactPoint(s, 0.5f);
        if (Move::WorthReAiming(aimed, wanted, tolerance))
        {
            aimed = wanted;
            ++legs;
        }
    }
    CHECK_EQ(legs, 1);
}

TEST(ATargetThatActuallyWalksAwayIsFollowed)
{
    Sight s = Facing();
    const float tolerance = Move::ReAimTolerance(s.extent);

    Vector3 aimed = Move::ContactPoint(s, 0.5f);
    int legs = 0;
    for (int i = 1; i <= 40; ++i)
    {
        s.target = Vector3(10.f + float(i), 0.f, 0.f);
        const Vector3 wanted = Move::ContactPoint(s, 0.5f);
        if (Move::WorthReAiming(aimed, wanted, tolerance))
        {
            aimed = wanted;
            ++legs;
        }
    }
    // Forty yards at a quarter-yard tolerance, but nowhere near forty legs: the test is
    // that it tracks at all, and that it does not lay one per step.
    CHECK(legs > 0);
    CHECK(legs <= 40);
}
