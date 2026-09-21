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

#include "Move/MoveWriter.h"

#include <cmath>
#include <limits>
#include <string>

using Geometry::Vector3;
using Move::MoveWriter;
using Move::Refusal;
using Move::Written;

namespace
{
    const float WALK = 2.5f;
    const float RUN = 7.0f;

    Written WriteLine(float fromX, float toX, float speed, float moverSpeed = RUN)
    {
        const Vector3 points[2] = { Vector3(fromX, 0.0f, 0.0f), Vector3(toX, 0.0f, 0.0f) };
        return MoveWriter::Write(points, 2, speed, moverSpeed);
    }
}

TEST(AStraightLegBecomesAStartADestinationAndNoMiddle)
{
    const Written w = WriteLine(0.0f, 14.0f, RUN);
    CHECK(w.Ok());
    CHECK(std::fabs(w.start.x - 0.0f) < 0.001f);
    CHECK(std::fabs(w.destination.x - 14.0f) < 0.001f);
    CHECK_EQ(w.middle.size(), size_t(0));
    CHECK(std::fabs(w.length - 14.0f) < 0.001f);
    CHECK_EQ(w.duration, uint32_t(2000));
}

TEST(TheMiddlePointsAreEverythingBetweenTheEnds)
{
    const Vector3 points[4] =
    {
        Vector3(0.0f, 0.0f, 0.0f), Vector3(5.0f, 0.0f, 0.0f),
        Vector3(10.0f, 0.0f, 0.0f), Vector3(15.0f, 0.0f, 0.0f)
    };
    const Written w = MoveWriter::Write(points, 4, RUN, RUN);
    CHECK(w.Ok());
    CHECK_EQ(w.middle.size(), size_t(2));
    CHECK(std::fabs(w.middle[0].x - 5.0f) < 0.001f);
    CHECK(std::fabs(w.middle[1].x - 10.0f) < 0.001f);
}

TEST(FewerThanTwoPointsIsNotALeg)
{
    const Vector3 one(1.0f, 2.0f, 3.0f);
    const Written w = MoveWriter::Write(&one, 1, RUN, RUN);
    CHECK(!w.Ok());
    CHECK(w.refusal == Refusal::TooFewPoints);
}

// The crash this whole writer exists to prevent: the client divides by a segment of no
// length inside a fixed-point atan2 and dies with ERROR #132.
TEST(ADuplicatedPointIsStrippedRatherThanSent)
{
    const Vector3 points[3] =
    {
        Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f)
    };
    const Written w = MoveWriter::Write(points, 3, RUN, RUN);
    CHECK(w.Ok());
    CHECK_EQ(w.middle.size(), size_t(0));
    CHECK(std::fabs(w.start.x - 0.0f) < 0.001f);
    CHECK(std::fabs(w.destination.x - 10.0f) < 0.001f);
}

TEST(APathThatCollapsesEntirelyIsRefusedNotSentEmpty)
{
    const Vector3 points[3] =
    {
        Vector3(3.0f, 3.0f, 3.0f), Vector3(3.0f, 3.0f, 3.0f), Vector3(3.0f, 3.0f, 3.0f)
    };
    const Written w = MoveWriter::Write(points, 3, RUN, RUN);
    CHECK(!w.Ok());
    CHECK(w.refusal == Refusal::NoLength);
}

// Packed offsets are quarter-yards in signed 11/11/10 fields measured from the midpoint of
// the first and last point. One yard past the edge the field changes sign and the mover
// walks to the far side of the map, so the writer refuses instead.
TEST(AMiddlePointBeyondThePackedReachIsRefused)
{
    const Vector3 points[3] =
    {
        Vector3(0.0f, 0.0f, 0.0f),
        Vector3(0.0f, 400.0f, 0.0f),
        Vector3(10.0f, 0.0f, 0.0f)
    };
    const Written w = MoveWriter::Write(points, 3, RUN, RUN);
    CHECK(!w.Ok());
    CHECK(w.refusal == Refusal::OutOfPackReach);
}

TEST(TheSameReachIsNotATestOfTheBoundingBox)
{
    // Both ends close together, but the path swings far from the line between them: a
    // bounding-box test over the ends would pass this and the packed field would wrap.
    const Vector3 points[3] =
    {
        Vector3(0.0f, 0.0f, 0.0f),
        Vector3(300.0f, 0.0f, 0.0f),
        Vector3(1.0f, 0.0f, 0.0f)
    };
    const Written w = MoveWriter::Write(points, 3, RUN, RUN);
    CHECK(!w.Ok());
    CHECK(w.refusal == Refusal::OutOfPackReach);
}

TEST(AnUncompressedPathHasNoReachLimitBecauseNothingIsPacked)
{
    const Vector3 points[3] =
    {
        Vector3(0.0f, 0.0f, 0.0f),
        Vector3(0.0f, 400.0f, 0.0f),
        Vector3(10.0f, 0.0f, 0.0f)
    };
    const Written w = MoveWriter::Write(points, 3, RUN, RUN, Move::SPLINE_UNCOMPRESSED);
    CHECK(w.Ok());
}

// Above the ceiling the client does not refuse, it stretches: the mover arrives late and
// nothing on the wire says so. Since the server's position function is only right while the
// client agrees with it, that is a correctness failure, not a slow leg.
TEST(ALegAboveTheClientsCeilingIsRefused)
{
    // The ceiling is max(4 * moverSpeed, 28). At a mover speed of 7 that is 28, so asking
    // for 60 yd/s over a long leg is well past it.
    const Written w = WriteLine(0.0f, 600.0f, 60.0f, RUN);
    CHECK(!w.Ok());
    CHECK(w.refusal == Refusal::ExceedsCeiling);
}

TEST(ALegExactlyAtWalkingPaceIsFine)
{
    const Written w = WriteLine(0.0f, 25.0f, WALK, WALK);
    CHECK(w.Ok());
    CHECK_EQ(w.duration, uint32_t(10000));
    CHECK(std::fabs(w.speed - WALK) < 0.01f);
}

// The floor of 28 is why a slow mover can still be sent a fast leg: a knockback on a
// walking creature is not above the ceiling just because the creature walks.
TEST(TheCeilingHasAFloorOfTwentyEightRegardlessOfTheMoversPace)
{
    const Written w = WriteLine(0.0f, 100.0f, 20.0f, 1.0f);
    CHECK(w.Ok());
}

TEST(ANonFiniteCoordinateIsRefusedBeforeAnythingElse)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Vector3 points[2] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(nan, 0.0f, 0.0f) };
    const Written w = MoveWriter::Write(points, 2, RUN, RUN);
    CHECK(!w.Ok());
    CHECK(w.refusal == Refusal::NotFinite);
}

// The client rounds corners itself when asked, which is what a Catmull-Rom path was for --
// and unlike one, it introduces no disagreement between the length we timed and the length
// it walks.
TEST(AnOrdinaryLegAsksTheClientToRoundItsCorners)
{
    const Written w = WriteLine(0.0f, 30.0f, RUN);
    CHECK(w.Ok());
    CHECK((w.flags & Move::SPLINE_ROUND_CORNERS) != 0);
}

TEST(AParabolaDoesNotAskForRoundedCorners)
{
    const Vector3 points[2] = { Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f) };
    const Written w = MoveWriter::Write(points, 2, RUN, RUN, Move::SPLINE_PARABOLA);
    CHECK(w.Ok());
    CHECK((w.flags & Move::SPLINE_ROUND_CORNERS) == 0);
    CHECK((w.flags & Move::SPLINE_PARABOLA) != 0);
}

TEST(TheDurationIsNeverZeroSoTheClientNeverDividesByIt)
{
    const Written w = WriteLine(0.0f, 0.2f, 100.0f, 100.0f);
    CHECK(w.Ok());
    CHECK(w.duration >= uint32_t(1));
}

// A route already launched holds the same polyline, so writing from one must produce the
// same packet as writing from its points.
TEST(WritingFromALaunchedRouteMatchesWritingFromItsPoints)
{
    const Vector3 points[3] =
    {
        Vector3(0.0f, 0.0f, 0.0f), Vector3(6.0f, 0.0f, 0.0f), Vector3(6.0f, 8.0f, 0.0f)
    };
    Move::Route route;
    CHECK(route.Launch(points, 3, RUN, 1000));

    const Written fromRoute = MoveWriter::Write(route, RUN);
    const Written fromPoints = MoveWriter::Write(points, 3, RUN, RUN);
    CHECK(fromRoute.Ok());
    CHECK(fromPoints.Ok());
    CHECK_EQ(fromRoute.duration, fromPoints.duration);
    CHECK_EQ(fromRoute.middle.size(), fromPoints.middle.size());
    CHECK(std::fabs(fromRoute.length - fromPoints.length) < 0.001f);
}

TEST(AnEmptyRouteIsRefusedRatherThanRead)
{
    Move::Route route;
    const Written w = MoveWriter::Write(route, RUN);
    CHECK(!w.Ok());
    CHECK(w.refusal == Refusal::TooFewPoints);
}

TEST(EveryRefusalHasAName)
{
    CHECK(std::string(MoveWriter::Why(Refusal::None)) == "none");
    CHECK(std::string(MoveWriter::Why(Refusal::TooFewPoints)).size() > 0);
    CHECK(std::string(MoveWriter::Why(Refusal::NoLength)).size() > 0);
    CHECK(std::string(MoveWriter::Why(Refusal::ExceedsCeiling)).size() > 0);
    CHECK(std::string(MoveWriter::Why(Refusal::OutOfPackReach)).size() > 0);
    CHECK(std::string(MoveWriter::Why(Refusal::NotFinite)).size() > 0);
}
