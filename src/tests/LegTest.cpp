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
#include "Move/ClientRules.h"
#include "Move/Leg.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

using Geometry::Vector3;
using Move::Leg;

namespace
{
    bool Near(float a, float b, float tol = 0.001f) { return std::fabs(a - b) <= tol; }

    bool NearPoint(const Vector3& a, const Vector3& b, float tol = 0.001f)
    {
        return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
    }

    /// A straight ten-yard walk east at one yard per second, starting at t = 1000.
    Leg Straight()
    {
        const Vector3 pts[2] = {Vector3(0.f, 0.f, 0.f), Vector3(10.f, 0.f, 0.f)};
        Leg leg;
        leg.Launch(pts, 2, 1.0f, 1000);
        return leg;
    }

    /// Ten east, then ten north. Twenty yards over two segments.
    Leg Corner()
    {
        const Vector3 pts[3] = {Vector3(0.f, 0.f, 0.f), Vector3(10.f, 0.f, 0.f),
                                Vector3(10.f, 10.f, 0.f)};
        Leg leg;
        leg.Launch(pts, 3, 2.0f, 0);
        return leg;
    }
}

// ---------------------------------------------------------------------------------
//  The client's own rules, transcribed from Wow.exe 15595 (sub_5CB460, sub_A27900).
// ---------------------------------------------------------------------------------

// sub_A27900 in full: four times the mover's speed, floored at 28, unless a jump or a
// fall is in the flags, in which case a flat 50.
TEST(ClientCeilingIsFourTimesSpeedNotSpeed)
{
    CHECK(Near(Move::Client::SpeedCeiling(7.0f, 0), 28.0f));
    CHECK(Near(Move::Client::SpeedCeiling(10.0f, 0), 40.0f));
}

TEST(ClientCeilingNeverFallsBelowTwentyEight)
{
    CHECK(Near(Move::Client::SpeedCeiling(1.0f, 0), 28.0f));
    CHECK(Near(Move::Client::SpeedCeiling(0.0f, 0), 28.0f));
}

TEST(ClientCeilingIsFlatFiftyForAJumpOrAFall)
{
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        const uint32_t flag = 1u << bit;
        if ((flag & Move::Client::CEILING_SPECIAL_FLAGS) == 0)
        {
            continue;
        }
        CHECK(Near(Move::Client::SpeedCeiling(100.0f, flag), 50.0f));
    }
}

// The whole point of the transcription: the duration we send is how we spell a speed, and
// the speed is what survives. Feed the client the duration we would have written and it
// comes back travelling at exactly the speed we meant.
TEST(ClientRetimingPreservesTheSpeedWeAskedFor)
{
    const float length = 30.0f;
    const float speed = 7.0f;
    const uint32_t sent = uint32_t((length / speed) * 1000.0f);

    const float ceiling = Move::Client::SpeedCeiling(speed, 0);
    CHECK(Near(Move::Client::ActualSpeed(length, sent, ceiling), speed, 0.01f));
    CHECK(!Move::Client::ExceedsCeiling(length, sent, ceiling));
}

// And the case that is a bug on our side today and reports itself nowhere: a duration
// implying more than the ceiling is not obeyed, it is stretched, and the mover lands late.
TEST(ClientStretchesALegThatAsksToGoTooFast)
{
    const float length = 300.0f;
    const uint32_t sent = 1000;                     // 300 yd/s
    const float ceiling = Move::Client::SpeedCeiling(7.0f, 0);   // 28

    CHECK(Move::Client::ExceedsCeiling(length, sent, ceiling));
    CHECK(Near(Move::Client::ActualSpeed(length, sent, ceiling), 28.0f));

    const uint32_t retimed = Move::Client::Retime(length, sent, ceiling);
    CHECK(retimed > sent);
    CHECK(Near(float(retimed), (300.0f / 28.0f) * 1000.0f, 2.0f));
}

// Under a sixth of a yard the client keeps what we sent and does no arithmetic at all.
TEST(ClientLeavesAVeryShortLegAlone)
{
    const uint32_t sent = 40;
    CHECK_EQ(Move::Client::Retime(0.1f, sent, 28.0f), sent);
    CHECK_EQ(Move::Client::Retime(Move::Client::RETIME_MIN_LENGTH, sent, 28.0f), sent);
}

// (1/36)^2. Inside it the client starts where we said; outside it the client splices its
// own position in front and walks a longer path than the one we timed.
TEST(ClientSplicesItsOwnStartBeyondTwoPointEightCentimetres)
{
    const Vector3 at(100.f, 100.f, 10.f);

    CHECK(!Move::Client::SplicesOwnStart(at, at));
    CHECK(!Move::Client::SplicesOwnStart(at, at + Vector3(0.02f, 0.f, 0.f)));
    CHECK(Move::Client::SplicesOwnStart(at, at + Vector3(0.05f, 0.f, 0.f)));
    CHECK(Move::Client::SplicesOwnStart(at, at + Vector3(0.f, 0.f, 1.0f)));
}

// ---------------------------------------------------------------------------------
//  The leg itself.
// ---------------------------------------------------------------------------------

TEST(LegFixturesLaunch)
{
    CHECK(Straight().Running());
    CHECK(Corner().Running());
}

TEST(LegRefusesInputNoMoverCouldFollow)
{
    const Vector3 one(1.f, 2.f, 3.f);
    const Vector3 same[2] = {one, one};
    Leg leg;

    CHECK(!leg.Launch(nullptr, 2, 1.0f, 0));
    CHECK(!leg.Launch(same, 1, 1.0f, 0));
    CHECK(!leg.Launch(same, 2, 0.0f, 0));
    CHECK(!leg.Launch(same, 2, -1.0f, 0));
    CHECK(!leg.Launch(same, 2, 1.0f, 0));       // two points, no length between them
    CHECK(!leg.Running());
    CHECK_EQ(int(leg.Count()), 0);
}

TEST(LegRefusesAPointThatIsNotANumber)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Vector3 pts[2] = {Vector3(0.f, 0.f, 0.f), Vector3(nan, 0.f, 0.f)};
    Leg leg;
    CHECK(!leg.Launch(pts, 2, 1.0f, 0));
    CHECK(!leg.Running());
}

TEST(LegMeasuresItsOwnPathAndStatesItsDuration)
{
    const Leg leg = Corner();
    CHECK(Near(leg.Length(), 20.0f));
    CHECK_EQ(leg.Duration(), uint32_t(10000));      // 20 yd at 2 yd/s
    CHECK_EQ(leg.EndTime(), uint32_t(10000));
}

TEST(LegIsAtItsStartBeforeItBegins)
{
    const Leg leg = Straight();
    CHECK(NearPoint(leg.At(0), Vector3(0.f, 0.f, 0.f)));
    CHECK(NearPoint(leg.At(999), Vector3(0.f, 0.f, 0.f)));
    CHECK(NearPoint(leg.At(1000), Vector3(0.f, 0.f, 0.f)));
    CHECK(!leg.Arrived(999));
}

TEST(LegInterpolatesAlongOneSegment)
{
    const Leg leg = Straight();
    CHECK(NearPoint(leg.At(3000), Vector3(2.f, 0.f, 0.f)));
    CHECK(NearPoint(leg.At(6000), Vector3(5.f, 0.f, 0.f)));
    CHECK(Near(leg.DistanceAt(6000), 5.0f));
}

TEST(LegStopsAtItsEndAndStaysThere)
{
    const Leg leg = Straight();
    CHECK(NearPoint(leg.At(11000), Vector3(10.f, 0.f, 0.f)));
    CHECK(NearPoint(leg.At(99000), Vector3(10.f, 0.f, 0.f)));
    CHECK(leg.Arrived(11000));
    CHECK(!leg.Arrived(10999));
    CHECK(Near(leg.DistanceAt(99000), 10.0f));
}

TEST(LegCrossesFromOneSegmentToTheNext)
{
    const Leg leg = Corner();
    CHECK(NearPoint(leg.At(2500), Vector3(5.f, 0.f, 0.f)));     // 5 yd in
    CHECK(NearPoint(leg.At(5000), Vector3(10.f, 0.f, 0.f)));    // exactly the corner
    CHECK(NearPoint(leg.At(7500), Vector3(10.f, 5.f, 0.f)));    // 5 yd up the second leg
}

// The cursor is a bookmark, not a cache of the answer. Walking forward and jumping about
// have to give the same positions, or every scenario replay reads differently than the
// tick that produced it.
TEST(LegReadsTheSameWhetherWalkedForwardOrProbedAtRandom)
{
    const Leg forward = Corner();
    const Leg probed = Corner();

    std::vector<Vector3> walked;
    for (uint32_t t = 0; t <= 11000; t += 250)
    {
        walked.push_back(forward.At(t));
    }

    const uint32_t order[6] = {9000, 1000, 10500, 250, 5000, 7750};
    for (uint32_t i = 0; i < 6; ++i)
    {
        (void)probed.At(order[i]);
    }

    size_t k = 0;
    for (uint32_t t = 0; t <= 11000; t += 250, ++k)
    {
        CHECK(NearPoint(probed.At(t), walked[k]));
    }
}

TEST(LegFacesAlongTheSegmentItIsOn)
{
    const Leg leg = Corner();
    CHECK(Near(leg.FacingAt(2500), 0.0f));                                // east
    CHECK(Near(leg.FacingAt(7500), 0.5f * Geometry::pif()));              // north
}

// A mover that has stopped keeps looking the way it was going; it does not snap north.
TEST(LegKeepsItsLastHeadingAfterItArrives)
{
    const Leg leg = Corner();
    CHECK(Near(leg.FacingAt(99000), 0.5f * Geometry::pif()));
}

// This is what makes a boundary crossing schedulable instead of polled: the whole path is
// known at launch, so the moment it passes any distance is known at launch too.
TEST(LegAnswersWhenItWillBeSomewhereWithoutBeingTicked)
{
    const Leg leg = Corner();

    const uint32_t atCorner = leg.TimeAtDistance(10.0f);
    CHECK_EQ(atCorner, uint32_t(5000));
    CHECK(NearPoint(leg.At(atCorner), leg.Point(1)));

    CHECK_EQ(leg.TimeAtDistance(-5.0f), leg.StartTime());
    CHECK_EQ(leg.TimeAtDistance(1000.0f), leg.EndTime());
}

// The server clock wraps every 49 days, and a leg launched a second before the wrap must
// keep answering afterwards rather than reporting itself finished forever.
TEST(LegSurvivesTheServerClockWrapping)
{
    const Vector3 pts[2] = {Vector3(0.f, 0.f, 0.f), Vector3(10.f, 0.f, 0.f)};
    const uint32_t justBeforeWrap = 0xFFFFFC18u;        // 1000 ms short of zero
    Leg leg;
    REQUIRE(leg.Launch(pts, 2, 1.0f, justBeforeWrap));

    CHECK(NearPoint(leg.At(justBeforeWrap + 2000u), Vector3(2.f, 0.f, 0.f)));
    CHECK(NearPoint(leg.At(1000u), Vector3(2.f, 0.f, 0.f)));     // same instant, wrapped
    CHECK(!leg.Arrived(1000u));
    CHECK(leg.Arrived(9000u));
}

// A path may repeat a point -- a routed corridor often does -- and stepping over the
// zero-length segment must not divide by it.
TEST(LegStepsOverADuplicatedPointWithoutDividingByZero)
{
    const Vector3 pts[4] = {Vector3(0.f, 0.f, 0.f), Vector3(5.f, 0.f, 0.f),
                            Vector3(5.f, 0.f, 0.f), Vector3(10.f, 0.f, 0.f)};
    Leg leg;
    REQUIRE(leg.Launch(pts, 4, 1.0f, 0));

    CHECK(Near(leg.Length(), 10.0f));
    CHECK(NearPoint(leg.At(5000), Vector3(5.f, 0.f, 0.f)));
    CHECK(NearPoint(leg.At(7000), Vector3(7.f, 0.f, 0.f)));
    CHECK(leg.At(5000).isFinite());
}

TEST(LegRelaunchKeepsNothingOfTheOldOne)
{
    const Vector3 first[3] = {Vector3(0.f, 0.f, 0.f), Vector3(10.f, 0.f, 0.f),
                              Vector3(20.f, 0.f, 0.f)};
    const Vector3 second[2] = {Vector3(0.f, 0.f, 0.f), Vector3(0.f, 4.f, 0.f)};

    Leg leg;
    REQUIRE(leg.Launch(first, 3, 1.0f, 0));
    (void)leg.At(15000);                                  // park the cursor at the end

    REQUIRE(leg.Launch(second, 2, 2.0f, 0));
    CHECK_EQ(int(leg.Count()), 2);
    CHECK(Near(leg.Length(), 4.0f));
    CHECK(NearPoint(leg.At(1000), Vector3(0.f, 2.f, 0.f)));
}

// ---------------------------------------------------------------------------------
//  The two together: what we would send, run through what the client would do with it.
// ---------------------------------------------------------------------------------

// The round trip that matters. A leg built by the server, timed by the server, handed to
// the client's arithmetic, comes back at the server's own speed -- so the server's At()
// and the client's rendering agree without either simulating the other.
TEST(ALegWeBuildSurvivesTheClientsArithmeticUnchanged)
{
    const Vector3 pts[4] = {Vector3(0.f, 0.f, 0.f), Vector3(12.f, 0.f, 0.f),
                            Vector3(12.f, 9.f, 0.f), Vector3(20.f, 9.f, 0.f)};
    Leg leg;
    REQUIRE(leg.Launch(pts, 4, 7.0f, 5000));
    CHECK(Near(leg.Length(), 29.0f));

    const float ceiling = Move::Client::SpeedCeiling(7.0f, 0);
    const uint32_t sent = leg.Duration();

    CHECK(!Move::Client::ExceedsCeiling(leg.Length(), sent, ceiling));
    CHECK(Near(Move::Client::ActualSpeed(leg.Length(), sent, ceiling), leg.Speed(), 0.01f));

    // And the client's own duration matches the leg's, so both agree on the arrival.
    const uint32_t theirs = Move::Client::Retime(leg.Length(), sent, ceiling);
    CHECK(Near(float(theirs), float(sent), 2.0f));
}

// The failure this guard is for. Ask for a walking creature to cross thirty yards in a
// tenth of a second and the client simply refuses, without telling anyone.
TEST(ALegThatOutrunsTheCeilingIsCaughtBeforeItIsSent)
{
    const Vector3 pts[2] = {Vector3(0.f, 0.f, 0.f), Vector3(30.f, 0.f, 0.f)};
    Leg leg;
    REQUIRE(leg.Launch(pts, 2, 300.0f, 0));

    const float ceiling = Move::Client::SpeedCeiling(7.0f, 0);
    CHECK(Move::Client::ExceedsCeiling(leg.Length(), leg.Duration(), ceiling));

    const uint32_t theirs = Move::Client::Retime(leg.Length(), leg.Duration(), ceiling);
    CHECK(theirs > leg.Duration() * 8);
}

// ---------------------------------------------------------------------------------
//  Measuring a curve: why the server has to count the same way the client does.
// ---------------------------------------------------------------------------------

// A length is measured by sampling the curve and summing the chords between samples, and
// a chord is always shorter than the arc it spans. So a coarser count is not noisier, it
// is biased -- always short, never long.
TEST(ArcLengthAlwaysGrowsWithTheNumberOfSamples)
{
    const Vector3 p0(-10.f, 0.f, 0.f);
    const Vector3 p1(0.f, 0.f, 0.f);
    const Vector3 p2(10.f, 10.f, 0.f);
    const Vector3 p3(20.f, 0.f, 0.f);

    float previous = 0.0f;
    const uint32_t counts[6] = {1, 2, 3, 5, 10, 20};
    for (uint32_t i = 0; i < 6; ++i)
    {
        const float measured = Move::Client::CatmullRomLength(p0, p1, p2, p3, counts[i]);
        CHECK(measured >= previous);
        previous = measured;
    }
}

// The number the tree used to carry against the number the client uses. Three samples
// understate a real turn, and the shortfall is the duration we hand the client -- which it
// then stretches, landing the mover late on every curve.
TEST(ThreeSamplesUnderstateACurveTheClientMeasuresAtTwenty)
{
    const Vector3 p0(-10.f, 0.f, 0.f);
    const Vector3 p1(0.f, 0.f, 0.f);
    const Vector3 p2(10.f, 10.f, 0.f);
    const Vector3 p3(20.f, 0.f, 0.f);

    const float coarse = Move::Client::CatmullRomLength(p0, p1, p2, p3, 3);
    const float theirs = Move::Client::CatmullRomLength(p0, p1, p2, p3, Move::Client::ARC_STEPS);

    CHECK(coarse < theirs);

    // Understating the length by this much is understating the duration by the same
    // fraction, because one is the other divided by a speed both sides agree on.
    const float shortfall = (theirs - coarse) / theirs;
    CHECK(shortfall > 0.002f);
}

// A straight run has no curvature to miss, so the sample count cannot change it. This is
// why matching the client costs nothing on the paths the server sends most.
TEST(ASampleCountCannotChangeAStraightSegment)
{
    const Vector3 p0(-10.f, 0.f, 0.f);
    const Vector3 p1(0.f, 0.f, 0.f);
    const Vector3 p2(10.f, 0.f, 0.f);
    const Vector3 p3(20.f, 0.f, 0.f);

    CHECK(Near(Move::Client::CatmullRomLength(p0, p1, p2, p3, 1), 10.0f, 0.01f));
    CHECK(Near(Move::Client::CatmullRomLength(p0, p1, p2, p3, 20), 10.0f, 0.01f));
}

// ---------------------------------------------------------------------------------
//  The verdict the server asks for before it sends.
// ---------------------------------------------------------------------------------

TEST(VerdictPassesALegThatAsksForNothingUnusual)
{
    const Move::Client::Verdict v = Move::Client::Inspect(29.0f, 4142, 7.0f, 0);

    CHECK(!v.tooFast);
    CHECK(Near(v.askedSpeed, 7.0f, 0.02f));
    CHECK(Near(v.ceiling, 28.0f));
    CHECK(std::abs(v.stretchMs) <= 2);
}

TEST(VerdictNamesTheStretchAClientWillAddToALegThatIsTooFast)
{
    const Move::Client::Verdict v = Move::Client::Inspect(300.0f, 1000, 7.0f, 0);

    CHECK(v.tooFast);
    CHECK(Near(v.askedSpeed, 300.0f));
    CHECK(Near(v.ceiling, 28.0f));
    CHECK(v.stretchMs > 9000);
    CHECK_EQ(v.theirDuration, uint32_t(v.ourDuration + v.stretchMs));
}

// A jump is measured against the flat fifty, not against four times a walk, so a leg that
// would be refused on foot is perfectly ordinary in the air.
TEST(VerdictJudgesAJumpAgainstTheFlatFifty)
{
    const uint32_t trajectory = 0x02000000;

    CHECK(Move::Client::Inspect(40.0f, 1000, 7.0f, 0).tooFast);
    CHECK(!Move::Client::Inspect(40.0f, 1000, 7.0f, trajectory).tooFast);
    CHECK(Near(Move::Client::Inspect(40.0f, 1000, 7.0f, trajectory).ceiling, 50.0f));
}

// ---------------------------------------------------------------------------------
//  Segments the client cannot be handed.
// ---------------------------------------------------------------------------------

namespace
{
    std::vector<Vector3> Stripped(std::vector<Vector3> pts)
    {
        const uint16_t kept = Move::Client::StripDeadSegments(pts.data(), uint16_t(pts.size()));
        pts.resize(kept);
        return pts;
    }
}

// The exact shape that took the client down. A patrol reaches a node, waits, and sets off
// again: the server forces point zero to the mover's own position, which IS the node that
// is already point one, and the path leaves with a segment of no length in it.
TEST(StripRemovesTheDuplicateAPatrolRestartCreates)
{
    const Vector3 node(-8974.f, -109.f, 84.6f);
    const std::vector<Vector3> path = {node, node, Vector3(-8966.f, -112.f, 84.f)};

    const std::vector<Vector3> out = Stripped(path);
    REQUIRE(out.size() == 2u);
    CHECK(NearPoint(out[0], node));
    CHECK(NearPoint(out[1], Vector3(-8966.f, -112.f, 84.f)));
}

// The first of a run is kept, never the last. Point zero is the mover's own position, and
// replacing it with a waypoint that merely rounds to the same place moves the leg's start
// off the mover -- which is the thing forcing point zero exists to prevent.
TEST(StripKeepsTheMoversOwnPointAndDropsTheWaypointOnTopOfIt)
{
    const Vector3 mover(10.0f, 10.0f, 5.0f);
    const Vector3 almost(10.02f, 10.0f, 5.0f);
    const std::vector<Vector3> out = Stripped({mover, almost, Vector3(20.f, 10.f, 5.f)});

    REQUIRE(out.size() == 2u);
    CHECK(NearPoint(out[0], mover));
}

TEST(StripLeavesARealPathAlone)
{
    const std::vector<Vector3> path = {Vector3(0.f, 0.f, 0.f), Vector3(5.f, 0.f, 0.f),
                                       Vector3(10.f, 5.f, 0.f), Vector3(20.f, 5.f, 0.f)};
    CHECK_EQ(Stripped(path).size(), size_t(4));
}

TEST(StripCollapsesARunOfIdenticalPointsToOne)
{
    const Vector3 a(1.f, 1.f, 1.f);
    const std::vector<Vector3> out = Stripped({a, a, a, a, Vector3(9.f, 1.f, 1.f)});
    REQUIRE(out.size() == 2u);
    CHECK(NearPoint(out[0], a));
}

// A path that goes nowhere at all collapses to a single point, and the CALLER must not
// act on that by refusing the leg: MotionDriver reads a refusal as "blocked" and a patrol
// then abandons the node. The strip reports it; MoveSplineInit keeps the original path.
TEST(StripReducesAPathThatGoesNowhereToOnePoint)
{
    const Vector3 a(3.f, 4.f, 5.f);
    CHECK_EQ(Stripped({a, a, a}).size(), size_t(1));
}

// The shape that matters for a patrol restart: the forced first point duplicates the next
// waypoint, but the rest of the path is real, so stripping leaves a usable leg rather than
// collapsing it. This is the case the strip exists for and the one it must not overreach on.
TEST(StripKeepsAPatrolLegUsableAfterRemovingTheDuplicate)
{
    const Vector3 node(-8974.f, -109.f, 84.6f);
    const std::vector<Vector3> path = {node, node, Vector3(-8966.f, -112.f, 84.f),
                                       Vector3(-8955.f, -112.7f, 83.5f)};

    const std::vector<Vector3> out = Stripped(path);
    CHECK_EQ(out.size(), size_t(3));
    CHECK(NearPoint(out[0], node));
}

TEST(StripHandlesTheEmptyAndSingleCases)
{
    CHECK_EQ(Move::Client::StripDeadSegments(nullptr, 0), uint16_t(0));

    std::vector<Vector3> one = {Vector3(1.f, 2.f, 3.f)};
    CHECK_EQ(Move::Client::StripDeadSegments(one.data(), 1), uint16_t(1));
}

// Height alone is a real segment: a lift or a stair has two points over one spot.
TEST(StripCountsHeightAsDistance)
{
    const std::vector<Vector3> out =
        Stripped({Vector3(0.f, 0.f, 0.f), Vector3(0.f, 0.f, 4.f)});
    CHECK_EQ(out.size(), size_t(2));
}

// The floor is set by the wire, not by taste. appendPackXYZ stores each axis of an
// intermediate point as (int)(offset / 0.25f), so two points inside one bucket arrive as
// the same coordinate -- and a strip floor under that bucket lets exactly the degenerate
// segment it exists to remove be recreated by the encoding.
TEST(StripFloorIsSmallEnoughToKeepRealMovement)
{
    // Half a yard is real distance. A floor that large dropped whole legs and left
    // creatures with closely spaced waypoints standing between them, so the floor names
    // only "the same point", not "not far enough to bother".
    CHECK(Move::Client::MIN_SEGMENT < Move::Client::WIRE_STEP);

    const Vector3 a(100.f, 100.f, 10.f);
    const std::vector<Vector3> shortButReal = {a, a + Vector3(0.4f, 0.f, 0.f),
                                               Vector3(120.f, 100.f, 10.f)};
    CHECK_EQ(Stripped(shortButReal).size(), size_t(3));
}

