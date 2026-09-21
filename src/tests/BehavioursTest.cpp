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

#include "Move/Behaviours.h"
#include "Move/ClientRules.h"

#include <cmath>

using Geometry::Vector3;
using Move::Kind;
using Move::Plan;
using Move::Quarry;
using Move::World;

namespace
{
    class OpenField : public World
    {
        public:
            bool Route(const Vector3& from, const Vector3& to,
                       std::vector<Vector3>& points) override
            {
                ++routes;
                if (!routable)
                {
                    return false;
                }
                points.clear();
                points.push_back(from);
                points.push_back(to);
                return true;
            }
            bool Floor(const Vector3&, float& z) override { z = 0.0f; return true; }
            bool Scatter(const Vector3& centre, float radius, Vector3& out) override
            {
                if (!scatterable)
                {
                    return false;
                }
                out = centre + Vector3(radius, 0.0f, 0.0f);
                return true;
            }
            float Frand(float lo, float) override { return lo; }
            uint32_t Urand(uint32_t lo, uint32_t) override { return lo; }
            float Pace(uint32_t) const override { return 7.0f; }
            Vector3 Here() const override { return here; }
            float Heading() const override { return 0.0f; }

            Vector3 here;
            bool routable = true;
            bool scatterable = true;
            uint32_t routes = 0;
    };

    class OneTarget : public Move::Sighting
    {
        public:
            Quarry Look(uint64_t) const override { return quarry; }
            Quarry quarry;
    };

    bool Sent(const Plan& plan) { return plan.send && plan.count >= 2; }

    Move::WalkNodes::Node MakeNode(uint32_t id, float x, uint32_t waitMs = 0)
    {
        Move::WalkNodes::Node n;
        n.id = id;
        n.at = Vector3(x, 0.0f, 0.0f);
        n.waitMs = waitMs;
        return n;
    }
}

// ------------------------------------------------------------------- GoToPoint

TEST(GoingToAPointSendsALegAndReportsOnArrival)
{
    OpenField world;
    Move::GoToPoint go(Kind::Point, Vector3(20.0f, 0.0f, 0.0f), 77);

    Plan plan;
    go.Decide(1000, false, world, plan);
    CHECK(Sent(plan));

    world.here = Vector3(20.0f, 0.0f, 0.0f);
    Plan after;
    go.Arrived(4000, false, world, after);
    CHECK(go.Done());
    CHECK_EQ(after.acts.size(), size_t(1));
    CHECK_EQ(int(after.acts[0].what), int(Move::ACT_ARRIVED));
    CHECK_EQ(after.acts[0].id, uint32_t(77));
}

// A route that cannot be found is a retry. Nothing is reported, nothing is finished, and
// the destination does not move: the creature simply tries again.
TEST(AnUnroutablePointIsRetriedAndNeverReportedAsReached)
{
    OpenField world;
    world.routable = false;
    Move::GoToPoint go(Kind::Point, Vector3(20.0f, 0.0f, 0.0f), 77);

    Plan plan;
    const uint32_t due = go.Decide(1000, false, world, plan);
    CHECK(!plan.send);
    CHECK(due > 1000);
    CHECK(!go.Done());
    CHECK_EQ(plan.acts.size(), size_t(0));
}

// After enough failures it walks straight rather than abandoning the destination. Giving up
// is never an option this class has.
TEST(APointThatCannotBeRoutedIsEventuallyWalkedStraightAt)
{
    OpenField world;
    world.routable = false;
    Move::GoToPoint go(Kind::Point, Vector3(20.0f, 0.0f, 0.0f));

    Plan a, b, c, d;
    go.Decide(1000, false, world, a);
    go.Decide(2000, false, world, b);
    go.Decide(3000, false, world, c);
    go.Decide(4000, false, world, d);
    CHECK(Sent(d));
    CHECK_EQ(int(d.count), 2);
}

TEST(ACutLegIsNotAnArrival)
{
    OpenField world;
    Move::GoToPoint go(Kind::Point, Vector3(20.0f, 0.0f, 0.0f), 5);

    Plan plan;
    go.Decide(1000, false, world, plan);
    Plan cut;
    go.Arrived(2000, true, world, cut);
    CHECK(!go.Done());
    CHECK_EQ(cut.acts.size(), size_t(0));
}

// ------------------------------------------------------------------- WalkNodes

TEST(APatrolWeldsConsecutiveNodesIntoOneLeg)
{
    OpenField world;
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f));
    nodes.push_back(MakeNode(2, 20.0f));
    nodes.push_back(MakeNode(3, 30.0f));
    Move::WalkNodes patrol(nodes);

    Plan plan;
    patrol.Decide(0, false, world, plan);
    CHECK(Sent(plan));
    CHECK_EQ(patrol.RunLength(), size_t(3));
}

TEST(AWeldStopsAtTheFirstNodeThatWaits)
{
    OpenField world;
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f));
    nodes.push_back(MakeNode(2, 20.0f, 3000));
    nodes.push_back(MakeNode(3, 30.0f));
    Move::WalkNodes patrol(nodes);

    Plan plan;
    patrol.Decide(0, false, world, plan);
    CHECK(Sent(plan));
    CHECK_EQ(patrol.RunLength(), size_t(2));
}

TEST(EveryNodeAWeldCoveredIsReportedInOrder)
{
    OpenField world;
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f));
    nodes.push_back(MakeNode(2, 20.0f));
    nodes.push_back(MakeNode(3, 30.0f, 1000));
    Move::WalkNodes patrol(nodes);

    Plan plan;
    patrol.Decide(0, false, world, plan);

    Plan after;
    const uint32_t due = patrol.Arrived(5000, false, world, after);
    CHECK_EQ(after.acts.size(), size_t(3));
    CHECK_EQ(after.acts[0].id, uint32_t(1));
    CHECK_EQ(after.acts[1].id, uint32_t(2));
    CHECK_EQ(after.acts[2].id, uint32_t(3));
    CHECK_EQ(patrol.Reached(), uint32_t(3));
    CHECK_EQ(due, uint32_t(6000));
}

// THE REGRESSION THIS WHOLE DESIGN IS SHAPED AROUND. A leg that cannot be built must leave
// the target node exactly where it was. The old engine marked the arrival as done instead,
// so every blocked leg silently skipped a node and a patrol ended up walking between a few
// scattered points with the rest abandoned.
TEST(AFailedLegNeverAdvancesThePatrolsNode)
{
    OpenField world;
    world.routable = false;
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f));
    nodes.push_back(MakeNode(2, 20.0f));
    nodes.push_back(MakeNode(3, 30.0f));
    Move::WalkNodes patrol(nodes);

    const uint32_t before = patrol.Heading();
    Plan a, b;
    patrol.Decide(0, false, world, a);
    patrol.Decide(1000, false, world, b);
    CHECK(!a.send);
    CHECK(!b.send);
    CHECK_EQ(patrol.Heading(), before);
    CHECK_EQ(patrol.Reached(), uint32_t(0));
}

TEST(ACutPatrolLegReportsNothingAndKeepsItsNode)
{
    OpenField world;
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f, 500));
    nodes.push_back(MakeNode(2, 20.0f, 500));
    Move::WalkNodes patrol(nodes);

    Plan plan;
    patrol.Decide(0, false, world, plan);
    const uint32_t heading = patrol.Heading();

    Plan cut;
    patrol.Arrived(1000, true, world, cut);
    CHECK_EQ(cut.acts.size(), size_t(0));
    CHECK_EQ(patrol.Heading(), heading);
    CHECK_EQ(patrol.Reached(), uint32_t(0));
}

TEST(SettingTheNextWaypointMovesTheTargetAndNothingElse)
{
    OpenField world;
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f, 100));
    nodes.push_back(MakeNode(2, 20.0f, 100));
    nodes.push_back(MakeNode(3, 30.0f, 100));
    Move::WalkNodes patrol(nodes);

    CHECK(patrol.SetNext(3));
    CHECK_EQ(patrol.Heading(), uint32_t(3));
    CHECK(!patrol.SetNext(99));
    CHECK_EQ(patrol.Heading(), uint32_t(3));
}

TEST(APatrolAnchorsAtTheNodeItLastReached)
{
    OpenField world;
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f, 100));
    nodes.push_back(MakeNode(2, 20.0f, 100));
    Move::WalkNodes patrol(nodes);

    Vector3 pos;
    float facing = 0.0f;
    CHECK(!patrol.Anchor(pos, facing));

    Plan plan;
    patrol.Decide(0, false, world, plan);
    Plan after;
    patrol.Arrived(2000, false, world, after);

    CHECK(patrol.Anchor(pos, facing));
    CHECK(std::fabs(pos.x - 10.0f) < 0.001f);
}

// --------------------------------------------------------------------- Pursue

// The arithmetic on its own. The deadline is measured from the moment the target's position
// was REPORTED, not from now: the uncertainty about where it is and about where it will be
// are the same quantity and must not be counted twice.
TEST(TheDriftDeadlineIsMeasuredFromTheReportNotFromNow)
{
    Quarry quarry;
    quarry.known = true;
    quarry.reportedAtMs = 10000;
    quarry.topSpeed = 7.0f;

    // One yard of slack at seven yards a second is about 142 ms.
    const uint32_t deadline = Move::Pursue::DriftDeadline(quarry, 1.0f);
    CHECK(deadline > uint32_t(10100));
    CHECK(deadline < uint32_t(10200));
}

TEST(AStationaryTargetGivesADistantDeadlineRatherThanAnImmediateOne)
{
    Quarry quarry;
    quarry.known = true;
    quarry.reportedAtMs = 1000;
    quarry.topSpeed = 0.0f;
    CHECK(Move::Pursue::DriftDeadline(quarry, 1.0f) > uint32_t(50000));
}

TEST(APursuitAimsShortOfItsTargetNotAtIt)
{
    OpenField world;
    world.here = Vector3(0.0f, 0.0f, 0.0f);
    OneTarget sighting;
    sighting.quarry.known = true;
    sighting.quarry.at = Vector3(30.0f, 0.0f, 0.0f);
    sighting.quarry.reportedAtMs = 0;
    sighting.quarry.topSpeed = 7.0f;

    Move::Pursue chase(Kind::Chase, 42, sighting, 5.0f);
    Plan plan;
    chase.Decide(0, false, world, plan);
    CHECK(Sent(plan));
    CHECK(std::fabs(chase.AimedAt().x - 25.0f) < 0.01f);
}

TEST(APursuitAlreadyInRangeSendsNothingAndWaitsForDrift)
{
    OpenField world;
    world.here = Vector3(0.0f, 0.0f, 0.0f);
    OneTarget sighting;
    sighting.quarry.known = true;
    sighting.quarry.at = Vector3(3.0f, 0.0f, 0.0f);
    sighting.quarry.reportedAtMs = 1000;
    sighting.quarry.topSpeed = 7.0f;

    Move::Pursue chase(Kind::Chase, 42, sighting, 5.0f);
    Plan plan;
    const uint32_t due = chase.Decide(1000, false, world, plan);
    CHECK(!plan.send);
    CHECK(due > uint32_t(1000));
    CHECK_EQ(world.routes, uint32_t(0));
}

TEST(ALostTargetIsReportedOnceAndStopsAskingForWakeUps)
{
    OpenField world;
    OneTarget sighting;
    sighting.quarry.known = false;

    Move::Pursue chase(Kind::Chase, 42, sighting, 5.0f);
    Plan plan;
    const uint32_t due = chase.Decide(1000, false, world, plan);
    CHECK_EQ(due, uint32_t(0));
    CHECK_EQ(plan.acts.size(), size_t(1));
    CHECK_EQ(int(plan.acts[0].what), int(Move::ACT_TARGET_LOST));
}

// -------------------------------------------------------------- Scatter, Flee

TEST(ScatteringDrawsASpotAndWalksToIt)
{
    OpenField world;
    Move::Scatter wander(Kind::Wander, Vector3(0.0f, 0.0f, 0.0f), 10.0f, 2000, 5000);

    Plan plan;
    wander.Decide(0, false, world, plan);
    CHECK(Sent(plan));

    Plan after;
    const uint32_t due = wander.Arrived(3000, false, world, after);
    CHECK_EQ(due, uint32_t(5000));
}

TEST(AScatterThatCannotDrawASpotRetries)
{
    OpenField world;
    world.scatterable = false;
    Move::Scatter wander(Kind::Wander, Vector3(0.0f, 0.0f, 0.0f), 10.0f, 1000, 2000);

    Plan plan;
    const uint32_t due = wander.Decide(1000, false, world, plan);
    CHECK(!plan.send);
    CHECK(due > uint32_t(1000));
}

TEST(FleeingHeadsDirectlyAwayFromItsSource)
{
    OpenField world;
    world.here = Vector3(10.0f, 0.0f, 0.0f);
    Move::FleeFrom flee(Kind::Fear, Vector3(0.0f, 0.0f, 0.0f), 20.0f);

    Plan plan;
    flee.Decide(0, false, world, plan);
    CHECK(Sent(plan));
    CHECK(plan.points[plan.count - 1].x > 25.0f);
}

TEST(FleeingFromTheSpotItStandsOnStillPicksADirection)
{
    OpenField world;
    world.here = Vector3(0.0f, 0.0f, 0.0f);
    Move::FleeFrom flee(Kind::Fear, Vector3(0.0f, 0.0f, 0.0f), 20.0f);

    Plan plan;
    flee.Decide(0, false, world, plan);
    CHECK(Sent(plan));
}

TEST(ABallisticLegLandsOnceAndReportsIt)
{
    OpenField world;
    Move::Ballistic jump(Vector3(10.0f, 0.0f, 5.0f), 20.0f, 8.0f);

    Plan plan;
    jump.Decide(0, false, world, plan);
    CHECK(plan.send);
    CHECK_EQ(int(plan.count), 2);

    Plan after;
    jump.Arrived(1000, false, world, after);
    CHECK(jump.Landed());
    CHECK_EQ(int(after.acts[0].what), int(Move::ACT_LANDED));
}

// ------------------------------------------ what the live log caught

// Thirty-seven percent of every leg the server tried to send was refused for having no
// length, because a shape would happily ask to walk to where the creature already stood.
// The writer caught it -- that is what it is for -- but a shape must not produce it.
TEST(AScatterThatDrawsTheSpotItStandsOnRestsInsteadOfAskingForALeg)
{
    OpenField world;
    world.here = Vector3(10.0f, 0.0f, 0.0f);
    // The fake draws centre + radius on x; with a radius of zero that is exactly here.
    Move::Scatter wander(Kind::Wander, Vector3(10.0f, 0.0f, 0.0f), 0.0f, 2000, 4000);

    Plan plan;
    const uint32_t due = wander.Decide(1000, false, world, plan);
    CHECK(!plan.send);
    CHECK(due >= uint32_t(3000));
}

TEST(APointAlreadyUnderfootIsAnArrivalNotALegThatGetsRefused)
{
    OpenField world;
    world.here = Vector3(5.0f, 0.0f, 0.0f);
    Move::GoToPoint go(Kind::Point, Vector3(5.2f, 0.0f, 0.0f), 31);

    Plan plan;
    go.Decide(1000, false, world, plan);
    CHECK(!plan.send);
    CHECK(go.Done());
    CHECK_EQ(int(plan.acts[0].what), int(Move::ACT_ARRIVED));
}

// A creature spawned exactly on its first waypoint used to ask for a leg of no length every
// time it woke. Standing on the node IS reaching it -- which is not the same as skipping it.
TEST(APatrolStandingOnItsNodeReachesItRatherThanAskingForAnEmptyLeg)
{
    OpenField world;
    world.here = Vector3(10.0f, 0.0f, 0.0f);
    std::vector<Move::WalkNodes::Node> nodes;
    nodes.push_back(MakeNode(1, 10.0f, 2000));
    nodes.push_back(MakeNode(2, 30.0f, 2000));
    Move::WalkNodes patrol(nodes);

    Plan plan;
    const uint32_t due = patrol.Decide(1000, false, world, plan);
    CHECK(!plan.send);
    CHECK_EQ(patrol.Reached(), uint32_t(1));
    CHECK_EQ(patrol.Heading(), uint32_t(2));
    CHECK_EQ(due, uint32_t(3000));
}

// THE WELD HAS A REACH. Packed middle points are offsets from the midpoint of the first and
// last, in signed 11/11/10 bit fields; a yard past the edge the sign flips and the creature
// walks to the far side of the map. The writer refused these; the weld must not build them.
TEST(AWeldStopsBeforeItsPointsOutgrowThePackedFields)
{
    OpenField world;
    world.here = Vector3(0.0f, 0.0f, 0.0f);

    // Nodes marching away in a straight line, far enough that welding all of them would put
    // the middle ones hundreds of yards from the midpoint of the ends.
    std::vector<Move::WalkNodes::Node> nodes;
    for (int i = 1; i <= 12; ++i)
    {
        nodes.push_back(MakeNode(uint32_t(i), float(i) * 120.0f));
    }
    Move::WalkNodes patrol(nodes, false);

    Plan plan;
    patrol.Decide(0, false, world, plan);
    CHECK(Sent(plan));
    CHECK(Move::Client::PacksWithoutWrapping(plan.points, plan.count));
    CHECK(patrol.RunLength() < size_t(12));
}
