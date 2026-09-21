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

#include "Move/Movement.h"

using Geometry::Vector3;
using Move::Behaviour;
using Move::Kind;
using Move::Layer;
using Move::Movement;
using Move::Plan;
using Move::World;

namespace
{
    /// A World that answers from plain numbers. Every query is recorded, because a
    /// behaviour asking the map twice for the same fact in one wake-up is the thing this
    /// design exists to prevent.
    class FlatWorld : public World
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
                out = centre + Vector3(radius, 0.0f, 0.0f);
                return true;
            }
            float Frand(float lo, float) override { return lo; }
            uint32_t Urand(uint32_t lo, uint32_t) override { return lo; }
            float Pace(uint32_t) const override { return 5.0f; }
            Vector3 Here() const override { return here; }
            float Heading() const override { return 0.0f; }

            Vector3 here;
            bool routable = true;
            uint32_t routes = 0;
    };

    /// A behaviour that walks to one point and then asks to be woken after a delay. Small
    /// enough to reason about, complete enough to exercise every path through Movement.
    class GoTo : public Behaviour
    {
        public:
            GoTo(Kind kind, const Vector3& to, uint32_t restMs)
                : m_kind(kind), m_to(to), m_rest(restMs) {}

            Kind What() const override { return m_kind; }

            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override
            {
                ++decides;
                sawRestart = restart;
                m_points.clear();
                if (!world.Route(world.Here(), m_to, m_points))
                {
                    return nowMs + 500;
                }
                out.send = true;
                out.points = &m_points[0];
                out.count = uint16_t(m_points.size());
                return nowMs + m_rest;
            }

            uint32_t Arrived(uint32_t nowMs, bool cut, World&, Plan&) override
            {
                ++arrivals;
                lastCut = cut;
                return nowMs + m_rest;
            }

            uint32_t decides = 0;
            uint32_t arrivals = 0;
            bool sawRestart = false;
            bool lastCut = false;

        private:
            Kind m_kind;
            Vector3 m_to;
            uint32_t m_rest;
            std::vector<Vector3> m_points;
    };
}

TEST(AKindKnowsItsOwnPriorityWithoutBeingTold)
{
    CHECK(Move::LayerOf(Kind::Patrol) == Layer::Default);
    CHECK(Move::LayerOf(Kind::Chase) == Layer::Combat);
    CHECK(Move::LayerOf(Kind::Fear) == Layer::Control);
    CHECK(Move::LayerOf(Kind::Taxi) == Layer::Taxi);
    CHECK(Move::PolicyOf(Kind::Taxi) == Move::Policy::Override);
    CHECK(Move::PolicyOf(Kind::Patrol) == Move::Policy::Supersede);
}

TEST(AMovementWithNothingHeldWantsNoWakeUp)
{
    FlatWorld world;
    Movement m;
    Plan plan;
    CHECK_EQ(m.Wake(1000, world, plan), uint32_t(0));
    CHECK(!plan.send);
    Kind running = Kind::Count;
    CHECK(!m.Running(running));
}

TEST(TheHighestPriorityHeldIsWhatRuns)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000));
    Kind running = Kind::Count;
    CHECK(m.Running(running));
    CHECK(running == Kind::Patrol);

    m.Take(new GoTo(Kind::Chase, Vector3(20.0f, 0.0f, 0.0f), 500));
    CHECK(m.Running(running));
    CHECK(running == Kind::Chase);

    // A lower priority arriving later does not become the answer.
    m.Take(new GoTo(Kind::Wander, Vector3(1.0f, 0.0f, 0.0f), 100));
    CHECK(m.Running(running));
    CHECK(running == Kind::Chase);
}

TEST(WakingSendsTheSelectedBehavioursRouteAndNobodyElses)
{
    FlatWorld world;
    Movement m;
    GoTo* patrol = new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000);
    GoTo* chase = new GoTo(Kind::Chase, Vector3(20.0f, 0.0f, 0.0f), 500);
    m.Take(patrol);
    m.Take(chase);

    Plan plan;
    const uint32_t due = m.Wake(1000, world, plan);
    CHECK_EQ(due, uint32_t(1500));
    CHECK(plan.send);
    CHECK_EQ(int(chase->decides), 1);
    CHECK_EQ(int(patrol->decides), 0);
}

TEST(TheDueTimeIsWhatTheMapWaitsFor)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000));

    Plan plan;
    CHECK_EQ(m.Wake(1000, world, plan), uint32_t(3000));
    CHECK_EQ(m.DueAt(), uint32_t(3000));
}

// The whole reason the route is kept rather than re-simulated: once it is launched the
// mover's position at any instant is arithmetic, not a poll.
TEST(TheRouteInFlightAnswersThePositionAtAnyInstant)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 5000));

    Plan plan;
    m.Wake(0, world, plan);
    CHECK(m.InFlight().Running());

    // Ten yards at five yards a second: halfway at one second.
    const Vector3 at = m.InFlight().At(1000);
    CHECK(std::fabs(at.x - 5.0f) < 0.01f);
    CHECK_EQ(m.InFlight().Duration(), uint32_t(2000));
}

TEST(OnlyTheBehaviourThatLaidTheRouteHearsThatItEnded)
{
    FlatWorld world;
    Movement m;
    GoTo* patrol = new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000);
    m.Take(patrol);

    Plan plan;
    m.Wake(0, world, plan);
    CHECK_EQ(int(patrol->decides), 1);

    Plan second;
    m.Ended(2000, false, world, second);
    CHECK_EQ(int(patrol->arrivals), 1);
    CHECK(!patrol->lastCut);
}

// A chase that takes over mid-route must not be handed the patrol's arrival: the route
// that ended was not its own, and telling it otherwise is how a behaviour ends up believing
// it reached somewhere it never went.
TEST(ABehaviourThatDidNotLayTheRouteIsAskedAfreshInstead)
{
    FlatWorld world;
    Movement m;
    GoTo* patrol = new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000);
    m.Take(patrol);

    Plan plan;
    m.Wake(0, world, plan);

    GoTo* chase = new GoTo(Kind::Chase, Vector3(20.0f, 0.0f, 0.0f), 500);
    m.Take(chase);

    Plan second;
    m.Ended(1000, false, world, second);
    CHECK_EQ(int(chase->arrivals), 0);
    CHECK_EQ(int(chase->decides), 1);
    CHECK(chase->sawRestart);
    CHECK_EQ(int(patrol->arrivals), 0);
}

TEST(ACutRouteSaysSoToTheBehaviourThatLaidIt)
{
    FlatWorld world;
    Movement m;
    GoTo* patrol = new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000);
    m.Take(patrol);

    Plan plan;
    m.Wake(0, world, plan);
    Plan second;
    m.Ended(500, true, world, second);
    CHECK(patrol->lastCut);
}

// Suspend is what lets a fear interrupt a chase and the chase resume afterwards without
// anyone having stored it: the slot beneath keeps its behaviour.
TEST(DroppingTheTopBehaviourBringsBackTheOneBeneath)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Chase, Vector3(20.0f, 0.0f, 0.0f), 500));
    m.Take(new GoTo(Kind::Fear, Vector3(-20.0f, 0.0f, 0.0f), 300));

    Kind running = Kind::Count;
    CHECK(m.Running(running));
    CHECK(running == Kind::Fear);

    m.Drop(Kind::Fear);
    CHECK(m.Running(running));
    CHECK(running == Kind::Chase);
}

// Override empties what is beneath it and destroys those behaviours: a taxi does not come
// back to a patrol when it lands, it comes back to nothing.
TEST(AnOverridingKindEmptiesEverythingBeneathIt)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000));
    m.Take(new GoTo(Kind::Chase, Vector3(20.0f, 0.0f, 0.0f), 500));
    m.Take(new GoTo(Kind::Taxi, Vector3(500.0f, 0.0f, 0.0f), 60000));

    CHECK(!m.Held(Kind::Patrol));
    CHECK(!m.Held(Kind::Chase));

    m.Drop(Kind::Taxi);
    Kind running = Kind::Count;
    CHECK(!m.Running(running));
}

TEST(DroppingAKindThatIsNotHeldChangesNothing)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000));
    m.Drop(Kind::Chase);
    m.Drop(Kind::Wander);

    Kind running = Kind::Count;
    CHECK(m.Running(running));
    CHECK(running == Kind::Patrol);
}

// Two kinds share the Default layer, and a layer holds one thing: requesting a wander over
// a patrol replaces it rather than stacking beside it.
TEST(TwoKindsOnOneLayerReplaceEachOther)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000));
    m.Take(new GoTo(Kind::Wander, Vector3(1.0f, 0.0f, 0.0f), 100));

    CHECK(!m.Held(Kind::Patrol));
    CHECK(m.Held(Kind::Wander) != 0);
}

// A behaviour that cannot find a way does NOT get to fail silently: it asks to be woken
// again. Nothing about the failure advances any state, which is what keeps a creature from
// abandoning a destination it merely could not reach this second.
TEST(AnUnroutableWakeUpAsksToBeWokenAgainAndSendsNothing)
{
    FlatWorld world;
    world.routable = false;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000));

    Plan plan;
    const uint32_t due = m.Wake(1000, world, plan);
    CHECK_EQ(due, uint32_t(1500));
    CHECK(!plan.send);
    CHECK(!m.InFlight().Running());
}

TEST(ClearingLeavesNothingHeldAndNoRouteInFlight)
{
    FlatWorld world;
    Movement m;
    m.Take(new GoTo(Kind::Patrol, Vector3(10.0f, 0.0f, 0.0f), 2000));
    Plan plan;
    m.Wake(0, world, plan);
    CHECK(m.InFlight().Running());

    m.Clear();
    Kind running = Kind::Count;
    CHECK(!m.Running(running));
    CHECK(!m.InFlight().Running());
    CHECK_EQ(m.DueAt(), uint32_t(0));
}
