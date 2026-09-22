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

// The kernel's taxi flight (P5-B family 5): pure, driven with scripted Sights. The
// generator's arithmetic (the node the mover has reached from the driver's path index with
// the leading slot; the events' alternation; the cut at a map's end) is pinned here.

#include "TestHarness.h"
#include "BehaviourModel.h"
#include "TaxiMove.h"
#include "../game/Object/TaxiDestinationsString.h"    // header-only: the suite links no game library
#include "../game/Object/TaxiRoute.h"                 // same: TaxiResume is inline and TaxiRoute::Weld is never called here

#include <vector>

using namespace Motion;

namespace
{
    /// The taxi never draws, routes or reads the port: every answer here is inert.
    class NullServices : public Services
    {
        public:
            bool RandomPoint(Vector3 const&, float, Vector3&) override { return false; }
            bool Ground(Vector3 const&, float&) override { return false; }
            float Frand(float min, float) override { return min; }
            uint32 Urand(uint32 min, uint32) override { return min; }
            int32 Irand(int32 min, int32) override { return min; }
            RouteResult Route(Vector3 const&, Vector3 const&, PointsArray&) override { return RouteResult(); }
            void ResetRoute() override {}
            bool CanMove() const override { return true; }
            bool Casting() const override { return false; }
            bool WaypointPaused() const override { return false; }
            bool Anchor(Vector3&) const override { return false; }
            bool CanFly() const override { return false; }
            bool StandingSpot(Vector3 const&, float, float, Vector3&) override { return false; }
            bool Fright(uint64, Vector3&, float&) override { return false; }
            bool GroundPoint(Vector3 const&, Vector3&) override { return false; }
            bool ClaimHeld(Motion::Kind) const override { return false; }
    };

    NullServices g_null;

    TaxiBehaviour::Node N(uint32 map, float x, uint32 arrival = 0, uint32 departure = 0, bool seam = false)
    {
        TaxiBehaviour::Node n;
        n.mapId = map;
        n.pos = Vector3(x, 0.0f, 100.0f);
        n.arrivalEvent = arrival;
        n.departureEvent = departure;
        n.seam = seam;
        return n;
    }

    /// Five nodes on one map, x = 0..400, arrival events 100+i, departure events 200+i.
    TaxiBehaviour::Params FiveNodes()
    {
        TaxiBehaviour::Params p;
        for (uint32 i = 0; i < 5; ++i)
        {
            p.nodes.push_back(N(1, float(i) * 100.0f, 100 + i, 200 + i));
        }
        p.speed = 30.0f;
        p.mountDisplayId = 1234;
        p.hasLanding = true;
        p.landing = Vector3(400.0f, 0.0f, 97.8f);
        return p;
    }

    Sight At(int32 pathIndex, bool traveling = true)
    {
        Sight s;
        s.status.pathIndex = pathIndex;
        s.status.traveling = traveling;
        s.facing = 1.5f;
        return s;
    }

    Sight ArrivedAt(int32 pathIndex)
    {
        Sight s;
        s.status.pathIndex = pathIndex;
        s.status.arrived = true;
        s.facing = 1.5f;
        return s;
    }

    std::vector<uint32> EventIds(std::vector<Effect> const& effects)
    {
        std::vector<uint32> out;
        for (size_t i = 0; i < effects.size(); ++i)
        {
            if (effects[i].kind == Effect::TaxiEvent)
            {
                out.push_back(effects[i].id);
            }
        }
        return out;
    }

    size_t Count(std::vector<Effect> const& effects, Effect::Kind kind)
    {
        size_t n = 0;
        for (size_t i = 0; i < effects.size(); ++i)
        {
            if (effects[i].kind == kind)
            {
                ++n;
            }
        }
        return n;
    }

    bool IsMoveAlongTheLeg(Step const& s)
    {
        return s.apply && s.intent.act == MoveIntent::Act::Move && s.intent.path != 0 &&
               s.intent.Has(MOVE_FLY) && s.intent.Has(MOVE_SMOOTH) && !s.intent.Has(MOVE_WALK) && s.intent.speed == 30.0f;
    }
}

TEST(MotionTaxi_ActivateTakesOffThenLaysTheLegWithALeadingCopy)
{
    TaxiBehaviour t(FiveNodes());
    Step a = t.Activate(At(0, false), g_null);
    REQUIRE(a.effects.size() == size_t(1));
    CHECK(a.effects[0].kind == Effect::TaxiTakeoff);
    CHECK_EQ(a.effects[0].id, 1234u);
    CHECK(a.resetLeg);
    CHECK(!a.stop && !a.interrupt);
    CHECK(a.roaming == Roaming::Keep);
    CHECK(IsMoveAlongTheLeg(a));
    // The leading slot is a COPY of the first node: the launch overwrites it with the mover's
    // real position, so node 0 itself stays in the spline (retail's shape).
    CHECK_EQ(a.intent.path->size(), size_t(6));
    CHECK_EQ((*a.intent.path)[0].x, 0.0f);
    CHECK_EQ((*a.intent.path)[1].x, 0.0f);
    CHECK_EQ((*a.intent.path)[2].x, 100.0f);
    CHECK_EQ((*a.intent.path)[5].x, 400.0f);
    CHECK_EQ(a.intent.goal.x, 400.0f);
    CHECK(t.Kind() == Motion::Kind::Taxi);
    CHECK(!t.TracksTarget());
    CHECK_EQ(t.Variant(), 0u);
    CHECK(!t.NeedsContactPoint());
    CHECK(t.Relays() == 0);
    CHECK_EQ(t.CurrentNode(), size_t(0));
    CHECK(!t.Crossing());
    // A start mid-route (ContinueTaxiFlight's closest segment): the leg from that node, the
    // earlier nodes' events never.
    TaxiBehaviour::Params p = FiveNodes();
    p.startNode = 2;
    TaxiBehaviour u(p);
    Step b = u.Activate(At(0, false), g_null);
    REQUIRE(IsMoveAlongTheLeg(b));
    CHECK_EQ(b.intent.path->size(), size_t(4));
    CHECK_EQ((*b.intent.path)[0].x, 200.0f);
    CHECK_EQ((*b.intent.path)[1].x, 200.0f);
    CHECK_EQ((*b.intent.path)[3].x, 400.0f);
    CHECK_EQ(u.CurrentNode(), size_t(2));
    Step b2 = u.Tick(At(2), g_null, 100);
    std::vector<uint32> ids = EventIds(b2.effects);
    REQUIRE(ids.size() == size_t(2));
    CHECK_EQ(ids[0], 202u);
    CHECK_EQ(ids[1], 103u);
}

TEST(MotionTaxi_TheNodeFollowsTheDriversIndexAndTheEventsAlternate)
{
    TaxiBehaviour t(FiveNodes());
    t.Activate(At(0, false), g_null);
    std::vector<uint32> all;
    // The segment leaving the slot, then the one leaving the first real node: node 0 both times.
    Step s0 = t.Tick(At(0), g_null, 100);
    CHECK(s0.effects.empty());
    CHECK(IsMoveAlongTheLeg(s0));                     // the leg re-stated each tick; the driver keeps it
    CHECK_EQ(s0.intent.path->size(), size_t(6));
    Step s1 = t.Tick(At(1), g_null, 100);
    CHECK(s1.effects.empty());
    CHECK_EQ(t.CurrentNode(), size_t(0));
    // Leaving node 1's segment: node 0 left (its departure), node 1 reached (its arrival).
    Step s2 = t.Tick(At(2), g_null, 100);
    std::vector<uint32> ids = EventIds(s2.effects);
    REQUIRE(ids.size() == size_t(2));
    CHECK_EQ(ids[0], 200u);
    CHECK(s2.effects[0].flag);                        // a departure
    CHECK_EQ(ids[1], 101u);
    CHECK(!s2.effects[1].flag);                       // an arrival
    CHECK_EQ(t.CurrentNode(), size_t(1));
    all.insert(all.end(), ids.begin(), ids.end());
    // A jump of two: every event in between, in the generator's alternation, in one step.
    Step s4 = t.Tick(At(4), g_null, 100);
    ids = EventIds(s4.effects);
    REQUIRE(ids.size() == size_t(4));
    CHECK_EQ(ids[0], 201u);
    CHECK_EQ(ids[1], 102u);
    CHECK_EQ(ids[2], 202u);
    CHECK_EQ(ids[3], 103u);
    CHECK_EQ(t.CurrentNode(), size_t(3));
    all.insert(all.end(), ids.begin(), ids.end());
    // The leg's end (pathIndex = N): the last node reached, its arrival fired, no departure, Done.
    Step end = t.Tick(ArrivedAt(5), g_null, 100);
    ids = EventIds(end.effects);
    REQUIRE(ids.size() == size_t(2));
    CHECK_EQ(ids[0], 203u);
    CHECK_EQ(ids[1], 104u);
    CHECK(end.apply);
    CHECK(end.intent.act == MoveIntent::Act::Done);
    CHECK_EQ(t.CurrentNode(), size_t(4));
    CHECK(t.EndReason(ArrivedAt(5)) == FinishReason::Arrived);
    all.insert(all.end(), ids.begin(), ids.end());
    // Each id once; the start node's arrival (100) and the last node's departure (204) never.
    CHECK_EQ(all.size(), size_t(8));
    for (size_t i = 0; i < all.size(); ++i)
    {
        CHECK(all[i] != 100u && all[i] != 204u);
        for (size_t j = i + 1; j < all.size(); ++j)
        {
            CHECK(all[i] != all[j]);
        }
    }
    // Standing with the leg finished but no edge (never the driver's case; the fallback): a hold.
    TaxiBehaviour idle(FiveNodes());
    idle.Activate(At(0, false), g_null);
    Step h = idle.Tick(At(0, false), g_null, 100);
    CHECK(h.apply && h.intent.act == MoveIntent::Act::Hold);
    CHECK(h.effects.empty());
}

TEST(MotionTaxi_ACrossingHoldsForTheAckThenResumesPastTheNewMapsFirstNode)
{
    TaxiBehaviour::Params p;
    for (uint32 i = 0; i < 4; ++i)
    {
        p.nodes.push_back(N(1, float(i) * 100.0f, 100 + i, 200 + i));
    }
    for (uint32 i = 4; i < 7; ++i)
    {
        p.nodes.push_back(N(2, 1000.0f + float(i - 4) * 100.0f, 100 + i, 200 + i));
    }
    p.mountDisplayId = 9;
    p.hasLanding = true;
    p.landing = Vector3(1200.0f, 0.0f, 99.0f);
    TaxiBehaviour t(p);
    Step a = t.Activate(At(0, false), g_null);
    REQUIRE(IsMoveAlongTheLeg(a));
    CHECK_EQ(a.intent.path->size(), size_t(5));    // the slot and the four nodes of map 1: the cut at the map's end
    CHECK_EQ(a.intent.goal.x, 300.0f);
    // The leg arrives (pathIndex = 4): the events up to node 3, then the crossing, once, and a hold.
    Step c = t.Tick(ArrivedAt(4), g_null, 100);
    std::vector<uint32> ids = EventIds(c.effects);
    REQUIRE(ids.size() == size_t(6));
    CHECK_EQ(ids[0], 200u);
    CHECK_EQ(ids[5], 103u);
    REQUIRE(Count(c.effects, Effect::TaxiCross) == size_t(1));
    Effect const& cross = c.effects.back();
    CHECK(cross.kind == Effect::TaxiCross);
    CHECK_EQ(cross.raw, 2u);
    CHECK_EQ(cross.point.x, 1000.0f);
    CHECK_EQ(cross.angle, 1.5f);
    CHECK(c.apply && c.intent.act == MoveIntent::Act::Hold);
    CHECK(t.Crossing());
    CHECK_EQ(t.CurrentNode(), size_t(3));
    // Waiting for the worldport ack: no second crossing, no leg, no events.
    Step w = t.Tick(At(0, false), g_null, 100);
    CHECK(w.effects.empty());
    CHECK(w.apply && w.intent.act == MoveIntent::Act::Hold);
    CHECK(t.Crossing());
    // The ack names the map: only the one the crossing aimed at continues the flight.
    CHECK(!t.CrossingLandedOn(1));
    CHECK(!t.CrossingLandedOn(3));
    CHECK(t.CrossingLandedOn(2));
    // The resume after the crossing: no takeoff, the leg from the node AFTER the new map's first
    // (the teleport put the mover on node 4; the generator's SkipCurrentNode), with its own slot.
    Step r = t.Resume(At(0, false), g_null, true);
    CHECK(r.effects.empty());
    CHECK(r.resetLeg);
    REQUIRE(IsMoveAlongTheLeg(r));
    CHECK_EQ(r.intent.path->size(), size_t(3));
    CHECK_EQ((*r.intent.path)[0].x, 1100.0f);
    CHECK_EQ((*r.intent.path)[1].x, 1100.0f);
    CHECK_EQ((*r.intent.path)[2].x, 1200.0f);
    CHECK_EQ(r.intent.goal.x, 1200.0f);
    CHECK(!t.Crossing());
    CHECK_EQ(t.CurrentNode(), size_t(5));
    // The new map's leg ends: node 5 left, node 6 reached, Done. Node 3's departure and node 4's
    // events never fire, as the generator skipped them.
    Step e = t.Tick(ArrivedAt(2), g_null, 100);
    ids = EventIds(e.effects);
    REQUIRE(ids.size() == size_t(2));
    CHECK_EQ(ids[0], 205u);
    CHECK_EQ(ids[1], 106u);
    CHECK(e.apply && e.intent.act == MoveIntent::Act::Done);
    CHECK(t.EndReason(ArrivedAt(2)) == FinishReason::Arrived);
}

TEST(MotionTaxi_TwoCrossingsAndATeleportOntoTheLastNode)
{
    TaxiBehaviour::Params p;
    p.nodes.push_back(N(1, 0.0f));
    p.nodes.push_back(N(1, 100.0f));
    p.nodes.push_back(N(2, 1000.0f));
    p.nodes.push_back(N(2, 1100.0f));
    p.nodes.push_back(N(3, 2000.0f, 104, 204));
    TaxiBehaviour t(p);
    Step a = t.Activate(At(0, false), g_null);
    REQUIRE(IsMoveAlongTheLeg(a));
    CHECK_EQ(a.intent.path->size(), size_t(3));
    Step c1 = t.Tick(ArrivedAt(2), g_null, 100);
    REQUIRE(Count(c1.effects, Effect::TaxiCross) == size_t(1));
    CHECK_EQ(c1.effects.back().raw, 2u);
    CHECK_EQ(c1.effects.back().point.x, 1000.0f);
    CHECK(t.CrossingLandedOn(2));
    Step r1 = t.Resume(At(0, false), g_null, true);
    REQUIRE(IsMoveAlongTheLeg(r1));
    CHECK_EQ(r1.intent.path->size(), size_t(2));   // the slot and node 3: the mover stands on node 2
    CHECK_EQ((*r1.intent.path)[1].x, 1100.0f);
    CHECK_EQ(t.CurrentNode(), size_t(3));
    Step c2 = t.Tick(ArrivedAt(1), g_null, 100);
    REQUIRE(Count(c2.effects, Effect::TaxiCross) == size_t(1));
    CHECK_EQ(c2.effects.back().raw, 3u);
    CHECK_EQ(c2.effects.back().point.x, 2000.0f);
    CHECK(t.CrossingLandedOn(3));
    // The teleport put the mover ON the route's last node: no leg to fly; the next tick ends.
    Step r2 = t.Resume(At(0, false), g_null, true);
    CHECK(r2.resetLeg);
    CHECK(!r2.apply);
    CHECK(r2.effects.empty());
    CHECK_EQ(t.CurrentNode(), size_t(4));
    Step e = t.Tick(At(0, false), g_null, 100);
    CHECK(e.apply && e.intent.act == MoveIntent::Act::Done);
    CHECK(e.effects.empty());                        // node 4's events never: it was never flown to
    CHECK(t.EndReason(At(0, false)) == FinishReason::Arrived);
}

TEST(MotionTaxi_ASeamAdvancesTheRouteWhenLeftAndFiresItsArrivalOnly)
{
    // Two hops welded: node 2 is the incoming hop's last row, kept and marked; it carries both
    // events. Its arrival fires as today; its departure never (the generator broke on the last
    // node's arrival and the next hop started at node 1), and TaxiSeam fires once, when it is left.
    TaxiBehaviour::Params p;
    p.nodes.push_back(N(1, 0.0f, 0, 200));
    p.nodes.push_back(N(1, 100.0f));
    p.nodes.push_back(N(1, 200.0f, 500, 600, true));
    p.nodes.push_back(N(1, 300.0f, 103));
    p.nodes.push_back(N(1, 400.0f));
    TaxiBehaviour t(p);
    t.Activate(At(0, false), g_null);
    Step s2 = t.Tick(At(2), g_null, 100);
    std::vector<uint32> ids = EventIds(s2.effects);
    REQUIRE(ids.size() == size_t(1));
    CHECK_EQ(ids[0], 200u);
    CHECK_EQ(Count(s2.effects, Effect::TaxiSeam), size_t(0));
    Step s4 = t.Tick(At(4), g_null, 100);
    REQUIRE(s4.effects.size() == size_t(3));
    CHECK(s4.effects[0].kind == Effect::TaxiEvent);
    CHECK_EQ(s4.effects[0].id, 500u);
    CHECK(!s4.effects[0].flag);
    CHECK(s4.effects[1].kind == Effect::TaxiSeam);
    CHECK(s4.effects[2].kind == Effect::TaxiEvent);
    CHECK_EQ(s4.effects[2].id, 103u);
    Step end = t.Tick(ArrivedAt(5), g_null, 100);
    CHECK(end.effects.empty());
    CHECK(end.intent.act == MoveIntent::Act::Done);
    // Whole route: no 600 anywhere, one seam.
    CHECK_EQ(Count(s2.effects, Effect::TaxiSeam) + Count(s4.effects, Effect::TaxiSeam) + Count(end.effects, Effect::TaxiSeam), size_t(1));
    ids = EventIds(s4.effects);
    for (size_t i = 0; i < ids.size(); ++i)
    {
        CHECK(ids[i] != 600u);
    }
}

TEST(MotionTaxi_FinishRecipesByReason)
{
    Sight s = At(0, false);
    {
        TaxiBehaviour t(FiveNodes());
        t.Activate(s, g_null);
        Outcome o = t.Finish(FinishReason::Arrived, s, g_null);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::TaxiLand);
        CHECK(o.effects[0].flag);                    // the snap onto the TaxiNodes position
        CHECK_EQ(o.effects[0].point.x, 400.0f);
        CHECK_EQ(o.effects[0].point.z, 97.8f);
        CHECK_EQ(o.effects[0].angle, 1.5f);
        CHECK(!o.interrupt && !o.stop && !o.stopForced);
        CHECK(o.roaming == Roaming::Keep);
    }
    {
        TaxiBehaviour::Params p = FiveNodes();
        p.hasLanding = false;                        // a spell taxi whose destination node has no position
        TaxiBehaviour t(p);
        t.Activate(s, g_null);
        Outcome o = t.Finish(FinishReason::Arrived, s, g_null);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::TaxiLand);
        CHECK(!o.effects[0].flag);
    }
    const FinishReason quiet[] = { FinishReason::Died, FinishReason::Expired, FinishReason::Cleared,
                                   FinishReason::Cut, FinishReason::Blocked, FinishReason::TargetLost };
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); ++i)
    {
        TaxiBehaviour t(FiveNodes());
        t.Activate(s, g_null);
        Outcome o = t.Finish(quiet[i], s, g_null);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::TaxiAbort);
        CHECK(o.effects[0].reason == quiet[i]);
        CHECK(!o.interrupt && !o.stop && !o.stopForced);
    }
    const FinishReason displacing[] = { FinishReason::Superseded, FinishReason::Overridden, FinishReason::Cancelled };
    for (size_t i = 0; i < sizeof(displacing) / sizeof(displacing[0]); ++i)
    {
        TaxiBehaviour t(FiveNodes());
        t.Activate(s, g_null);
        Outcome o = t.Finish(displacing[i], s, g_null);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::TaxiAbort);
        CHECK(o.effects[0].reason == displacing[i]);
        CHECK(o.interrupt);                          // a replaced flight's spline is cut, as every native's
        CHECK(!o.stop && !o.stopForced);
    }
}

TEST(MotionTaxi_ACutOrRefusedLegEndsTheFlightWithItsReason)
{
    TaxiBehaviour t(FiveNodes());
    t.Activate(At(0, false), g_null);
    Sight cut = At(1, false);
    cut.status.cut = true;
    Step s = t.Tick(cut, g_null, 100);
    CHECK(s.apply && s.intent.act == MoveIntent::Act::Done);
    CHECK(s.effects.empty());
    CHECK(t.EndReason(cut) == FinishReason::Cut);
    TaxiBehaviour u(FiveNodes());
    u.Activate(At(0, false), g_null);
    Sight blocked = At(0, false);
    blocked.status.blocked = true;
    Step b = u.Tick(blocked, g_null, 100);
    CHECK(b.apply && b.intent.act == MoveIntent::Act::Done);
    CHECK(u.EndReason(blocked) == FinishReason::Blocked);
    CHECK(u.EndReason(At(0, false)) == FinishReason::Arrived);
}

TEST(MotionTaxi_HooksAndResetPosition)
{
    TaxiBehaviour t(FiveNodes());
    t.Activate(At(0, false), g_null);
    t.Tick(At(2), g_null, 100);                     // node 1 reached
    Step su = t.Suspend();
    CHECK(!su.apply && !su.stop && !su.interrupt && !su.resetLeg && su.effects.empty());   // the arbiter never masks a taxi; the generator's Interrupt was empty
    Step r0 = t.Resume(At(2), g_null, false);
    CHECK(!r0.apply && !r0.resetLeg && r0.effects.empty());
    // A reset without a crossing pending: the generator's Reset, the leg re-laid from the current node, no takeoff.
    Step r1 = t.Resume(At(2), g_null, true);
    CHECK(r1.effects.empty());
    CHECK(r1.resetLeg);
    REQUIRE(IsMoveAlongTheLeg(r1));
    CHECK_EQ(r1.intent.path->size(), size_t(5));
    CHECK_EQ((*r1.intent.path)[0].x, 100.0f);
    CHECK_EQ((*r1.intent.path)[1].x, 100.0f);
    CHECK_EQ(t.CurrentNode(), size_t(1));
    CHECK(!t.CrossingLandedOn(1));                  // no crossing pending: nothing to land on
    Vector3 pos;
    float o = 0.0f;
    CHECK(t.ResetPosition(At(2), g_null, pos, o));
    CHECK_EQ(pos.x, 100.0f);
    CHECK_EQ(pos.z, 100.0f);
    CHECK_EQ(o, 1.5f);
    // A route with no nodes: nothing to fly, the first tick ends it, no crash.
    TaxiBehaviour::Params empty;
    TaxiBehaviour e(empty);
    Step a = e.Activate(At(0, false), g_null);
    CHECK_EQ(Count(a.effects, Effect::TaxiTakeoff), size_t(1));
    CHECK(!a.apply);
    Step d = e.Tick(At(0, false), g_null, 100);
    CHECK(d.apply && d.intent.act == MoveIntent::Act::Done);
    CHECK(!e.ResetPosition(At(0, false), g_null, pos, o));
}

TEST(MotionTaxi_AResetWhileCrossingHoldsUntilTheAckConfirmsTheMap)
{
    TaxiBehaviour::Params p;
    for (uint32 i = 0; i < 4; ++i)
    {
        p.nodes.push_back(N(1, float(i) * 100.0f));
    }
    for (uint32 i = 4; i < 7; ++i)
    {
        p.nodes.push_back(N(2, 1000.0f + float(i - 4) * 100.0f));
    }
    TaxiBehaviour t(p);
    t.Activate(At(0, false), g_null);
    Step c = t.Tick(ArrivedAt(4), g_null, 100);
    REQUIRE(Count(c.effects, Effect::TaxiCross) == size_t(1));
    // A reset before any ack confirmed the map: nothing laid, nothing advanced, still crossing.
    Step r0 = t.Resume(At(0, false), g_null, true);
    CHECK(!r0.apply && !r0.resetLeg && r0.effects.empty());
    CHECK(t.Crossing());
    CHECK_EQ(t.CurrentNode(), size_t(3));
    // The wrong map confirms nothing; the right one latches, and the resume lays the next leg.
    CHECK(!t.CrossingLandedOn(1));
    Step r1 = t.Resume(At(0, false), g_null, true);
    CHECK(!r1.apply && t.Crossing());
    CHECK(t.CrossingLandedOn(2));
    Step r2 = t.Resume(At(0, false), g_null, true);
    REQUIRE(IsMoveAlongTheLeg(r2));
    CHECK_EQ(r2.intent.path->size(), size_t(3));
    CHECK(!t.Crossing());
    CHECK_EQ(t.CurrentNode(), size_t(5));
    // The latch is consumed: a later reset re-lays the current leg as an ordinary reset does.
    Step r3 = t.Resume(At(0, false), g_null, true);
    REQUIRE(IsMoveAlongTheLeg(r3));
    CHECK_EQ(r3.intent.path->size(), size_t(3));
    CHECK_EQ(t.CurrentNode(), size_t(5));
}

TEST(MotionTaxi_ASeamAtAMapCutStillAdvancesTheRoute)
{
    // The hub is the leg's last node before the cut: the seam fires with the crossing.
    TaxiBehaviour::Params p;
    p.nodes.push_back(N(1, 0.0f));
    p.nodes.push_back(N(1, 100.0f, 0, 0, true));
    p.nodes.push_back(N(2, 1000.0f));
    p.nodes.push_back(N(2, 1100.0f));
    TaxiBehaviour t(p);
    t.Activate(At(0, false), g_null);
    Step c = t.Tick(ArrivedAt(2), g_null, 100);
    REQUIRE(c.effects.size() == size_t(2));
    CHECK(c.effects[0].kind == Effect::TaxiSeam);
    CHECK(c.effects[1].kind == Effect::TaxiCross);
    CHECK(t.CrossingLandedOn(2));
    Step r = t.Resume(At(0, false), g_null, true);
    CHECK(r.effects.empty());
    REQUIRE(IsMoveAlongTheLeg(r));
    CHECK_EQ(r.intent.path->size(), size_t(2));
    // The hub is the new map's first node, which the resume skips: the seam fires with the resume.
    TaxiBehaviour::Params q;
    q.nodes.push_back(N(1, 0.0f));
    q.nodes.push_back(N(1, 100.0f));
    q.nodes.push_back(N(2, 1000.0f, 0, 0, true));
    q.nodes.push_back(N(2, 1100.0f));
    q.nodes.push_back(N(2, 1200.0f));
    TaxiBehaviour u(q);
    u.Activate(At(0, false), g_null);
    Step c2 = u.Tick(ArrivedAt(2), g_null, 100);
    REQUIRE(c2.effects.size() == size_t(1));
    CHECK(c2.effects[0].kind == Effect::TaxiCross);
    CHECK(u.CrossingLandedOn(2));
    Step r2 = u.Resume(At(0, false), g_null, true);
    REQUIRE(r2.effects.size() == size_t(1));
    CHECK(r2.effects[0].kind == Effect::TaxiSeam);
    REQUIRE(IsMoveAlongTheLeg(r2));
    CHECK_EQ(r2.intent.path->size(), size_t(3));
    CHECK_EQ(u.CurrentNode(), size_t(3));
}

TEST(MotionTaxi_AStartPastTheEndAndAnIndexPastTheLegAreClamped)
{
    // A start node past the route: clamped to the last node, a two-point leg onto it, then Done.
    TaxiBehaviour::Params p = FiveNodes();
    p.startNode = 9;
    TaxiBehaviour t(p);
    CHECK_EQ(t.CurrentNode(), size_t(4));
    Step a = t.Activate(At(0, false), g_null);
    REQUIRE(IsMoveAlongTheLeg(a));
    CHECK_EQ(a.intent.path->size(), size_t(2));
    CHECK_EQ((*a.intent.path)[1].x, 400.0f);
    Step e = t.Tick(ArrivedAt(1), g_null, 100);
    CHECK(e.effects.empty());
    CHECK(e.apply && e.intent.act == MoveIntent::Act::Done);
    // An index past the leg's points: capped at the leg's last node, every event once, Done.
    TaxiBehaviour u(FiveNodes());
    u.Activate(At(0, false), g_null);
    Step s = u.Tick(At(99), g_null, 100);
    std::vector<uint32> ids = EventIds(s.effects);
    REQUIRE(ids.size() == size_t(8));
    CHECK_EQ(ids[0], 200u);
    CHECK_EQ(ids[7], 104u);
    CHECK(s.apply && s.intent.act == MoveIntent::Act::Done);
    CHECK_EQ(u.CurrentNode(), size_t(4));
}

TEST(TaxiPersistence_RoundTripKeepsTheFactionAndTheRoute)
{
    // The character table's taxi_path column: the flight master's faction then the remaining
    // nodes. The tree's loader read every token as the faction and then every token as a node.
    std::vector<uint32> route;
    route.push_back(5);
    route.push_back(6);
    route.push_back(7);
    const std::string text = TaxiDestinationsString::Format(1234, route);
    CHECK_STR(text.c_str(), "1234 5 6 7 ");
    uint32 faction = 0;
    std::vector<uint32> back;
    CHECK(TaxiDestinationsString::Parse(text, faction, back));
    CHECK_EQ(faction, 1234u);
    REQUIRE(back.size() == size_t(3));
    CHECK_EQ(back[0], 5u);
    CHECK_EQ(back[1], 6u);
    CHECK_EQ(back[2], 7u);
    // An empty route saves as nothing and loads as nothing.
    CHECK_STR(TaxiDestinationsString::Format(1234, std::vector<uint32>()).c_str(), "");
    CHECK(TaxiDestinationsString::Parse("", faction, back));
    CHECK_EQ(faction, 0u);
    CHECK(back.empty());
    // A faction alone is an empty route to the loader (it returns true with no nodes); the saver never writes one.
    CHECK(TaxiDestinationsString::Parse("77", faction, back));
    CHECK_EQ(faction, 77u);
    CHECK(back.empty());
    // Extra whitespace is tolerated; a token that is not a number is refused, not thrown.
    CHECK(TaxiDestinationsString::Parse("  9  10   11 ", faction, back));
    CHECK_EQ(faction, 9u);
    REQUIRE(back.size() == size_t(2));
    CHECK_EQ(back[1], 11u);
    CHECK(!TaxiDestinationsString::Parse("12 x 3", faction, back));
}

// ---- the landing-time resume (design 2026-09-22 §2) -----------------------------------------

namespace
{
    /// A node at (x, 0, 100) on `map`.
    TaxiRouteNode R(uint32 map, float x)
    {
        TaxiRouteNode n;
        n.mapId = map;
        n.x = x;
        n.y = 0.0f;
        n.z = 100.0f;
        return n;
    }

    /// Five nodes on map 1 at x = 0, 100, 200, 300, 400: 400 yd of route, 40 s at 10 yd/s.
    std::vector<TaxiRouteNode> Straight()
    {
        std::vector<TaxiRouteNode> nodes;
        for (uint32 i = 0; i < 5; ++i)
        {
            nodes.push_back(R(1, float(i) * 100.0f));
        }
        return nodes;
    }

    /// 100 yd on map 1, a seam, 100 yd on map 2: 200 flyable yards over two legs.
    std::vector<TaxiRouteNode> AcrossASeam()
    {
        std::vector<TaxiRouteNode> nodes;
        nodes.push_back(R(1, 0.0f));
        nodes.push_back(R(1, 100.0f));
        nodes.push_back(R(2, 1000.0f));
        nodes.push_back(R(2, 1100.0f));
        return nodes;
    }
}

TEST(TaxiResume_ASeamCostsNoDistanceAndNoTime)
{
    // The seam is a teleport, not a flight: 200 yd, not the 900 the raw coordinates would give.
    // The spline is laid one leg per map and crosses by teleport, so the length the landing
    // time is computed from has to agree with it.
    CHECK_EQ(TaxiResume::Length(AcrossASeam(), 0), 200.0f);
    CHECK_EQ(TaxiResume::Length(AcrossASeam(), 1), 100.0f);   // from the seam: the second map alone
    CHECK_EQ(TaxiResume::Length(Straight(), 0), 400.0f);
    CHECK_EQ(TaxiResume::Length(Straight(), 2), 200.0f);
}

TEST(TaxiResume_NoStampReadsAsLanded)
{
    // The old taxi_path string carries no stamp, and neither does a path that came back through
    // a battleground's stored two nodes. Zero is the safe direction: the passenger is put down
    // at the destination he paid for rather than flown from a moment nobody knows.
    const TaxiResume::Resume r = TaxiResume::Decide(Straight(), 10.0f, 5000, 0, 1);
    CHECK(r.verdict == TaxiResume::Verdict::Landed);
    CHECK_EQ(r.x, 400.0f);
    CHECK_EQ(r.mapId, 1u);
    CHECK_EQ(r.node, size_t(4));
}

TEST(TaxiResume_PastTheLandingIsTheDestination)
{
    // A fifteen-minute battleground on a forty-second flight. At the landing second exactly,
    // and after it: down at the destination.
    CHECK(TaxiResume::Decide(Straight(), 10.0f, 5000, 5000, 1).verdict == TaxiResume::Verdict::Landed);
    const TaxiResume::Resume r = TaxiResume::Decide(Straight(), 10.0f, 9000, 5000, 1);
    CHECK(r.verdict == TaxiResume::Verdict::Landed);
    CHECK_EQ(r.x, 400.0f);
    CHECK_EQ(r.flown, 400.0f);
    // Even from another map: the contract is to the destination, and the caller teleports.
    const TaxiResume::Resume elsewhere = TaxiResume::Decide(Straight(), 10.0f, 9000, 5000, 571);
    CHECK(elsewhere.verdict == TaxiResume::Verdict::Landed);
    CHECK_EQ(elsewhere.mapId, 1u);
}

TEST(TaxiResume_BeforeTheLandingIsThePointTheRouteHasReached)
{
    // 400 yd at 10 yd/s: a landing at t = 5000 means a takeoff at 4960. A twenty-second dungeon
    // pop puts him 25 s in, 250 yd along -- half-way down the third leg, flying on to node 3.
    const TaxiResume::Resume r = TaxiResume::Decide(Straight(), 10.0f, 4985, 5000, 1);
    CHECK(r.verdict == TaxiResume::Verdict::Airborne);
    CHECK(!r.clamped);
    CHECK_EQ(r.flown, 250.0f);
    CHECK_EQ(r.total, 400.0f);
    CHECK_EQ(r.node, size_t(3));
    CHECK_EQ(r.x, 250.0f);
    CHECK_EQ(r.mapId, 1u);

    // One second before the landing: 10 yd left, on the last leg, aiming at the last node.
    const TaxiResume::Resume late = TaxiResume::Decide(Straight(), 10.0f, 4999, 5000, 1);
    CHECK(late.verdict == TaxiResume::Verdict::Airborne);
    CHECK_EQ(late.node, size_t(4));
    CHECK_EQ(late.x, 390.0f);

    // A stamp further out than the route is long (a speed changed under a saved flight) reads
    // as the start rather than as a negative distance.
    const TaxiResume::Resume early = TaxiResume::Decide(Straight(), 10.0f, 1000, 5000, 1);
    CHECK(early.verdict == TaxiResume::Verdict::Airborne);
    CHECK_EQ(early.flown, 0.0f);
    CHECK_EQ(early.node, size_t(1));
    CHECK_EQ(early.x, 0.0f);
}

TEST(TaxiResume_AnElapsedPointOnALaterMapIsHeldAtTheSeam)
{
    // 200 yd over two maps, 20 s at 10 yd/s. At 15 s the clock says he is 150 yd along --
    // half-way down map 2's leg -- but he is standing on map 1. He is held at map 1's last node
    // and the kernel's own crossing carries him over from there.
    const TaxiResume::Resume r = TaxiResume::Decide(AcrossASeam(), 10.0f, 1015, 1020, 1);
    CHECK(r.verdict == TaxiResume::Verdict::Airborne);
    CHECK(r.clamped);
    CHECK_EQ(r.node, size_t(1));
    CHECK_EQ(r.mapId, 1u);
    CHECK_EQ(r.x, 100.0f);

    // Standing on map 2 already -- a logout taken right after the crossing, whose saved route
    // still begins on the old map -- and the clock is behind him: he rejoins at map 2's first
    // node instead of being sent back across the seam.
    const TaxiResume::Resume ahead = TaxiResume::Decide(AcrossASeam(), 10.0f, 1015, 1035, 2);
    CHECK(ahead.verdict == TaxiResume::Verdict::Airborne);
    CHECK(ahead.clamped);
    CHECK_EQ(ahead.node, size_t(2));
    CHECK_EQ(ahead.mapId, 2u);

    // And a route with nothing at all on his map cannot be resumed.
    CHECK(TaxiResume::Decide(AcrossASeam(), 10.0f, 1015, 1020, 571).verdict == TaxiResume::Verdict::NoRoute);
}

TEST(TaxiResume_ARouteTooShortOrASpeedOfZeroIsNoRoute)
{
    std::vector<TaxiRouteNode> one;
    one.push_back(R(1, 0.0f));
    CHECK(TaxiResume::Decide(one, 10.0f, 0, 100, 1).verdict == TaxiResume::Verdict::NoRoute);
    CHECK(TaxiResume::Decide(std::vector<TaxiRouteNode>(), 10.0f, 0, 100, 1).verdict == TaxiResume::Verdict::NoRoute);
    CHECK(TaxiResume::Decide(Straight(), 0.0f, 0, 100, 1).verdict == TaxiResume::Verdict::NoRoute);
}

TEST(TaxiPersistence_TheLandingStampRidesInTheStringAndTheOldFormStillLoads)
{
    std::vector<uint32> route;
    route.push_back(5);
    route.push_back(6);
    const std::string text = TaxiDestinationsString::Format(1234, route, 1758547200u);
    CHECK_STR(text.c_str(), "1234 5 6 L1758547200 ");

    uint32 faction = 0;
    uint32 landing = 0;
    std::vector<uint32> back;
    CHECK(TaxiDestinationsString::Parse(text, faction, back, &landing));
    CHECK_EQ(faction, 1234u);
    CHECK_EQ(landing, 1758547200u);
    REQUIRE(back.size() == size_t(2));
    CHECK_EQ(back[0], 5u);
    CHECK_EQ(back[1], 6u);

    // THE OLD FORM. No stamp, so it parses exactly as it always did and the landing reads zero,
    // which the resume takes as "landed". A build that stamps nothing writes the old bytes.
    CHECK_STR(TaxiDestinationsString::Format(1234, route).c_str(), "1234 5 6 ");
    landing = 99;
    CHECK(TaxiDestinationsString::Parse("1234 5 6 ", faction, back, &landing));
    CHECK_EQ(landing, 0u);
    REQUIRE(back.size() == size_t(2));   // and no stamp became a third node
    CHECK_EQ(faction, 1234u);

    // The stamp never takes the faction's slot nor becomes a node, wherever it is found, and a
    // caller that does not want it may pass NULL.
    CHECK(TaxiDestinationsString::Parse("L777 1234 5 6 ", faction, back, &landing));
    CHECK_EQ(landing, 777u);
    CHECK_EQ(faction, 1234u);
    REQUIRE(back.size() == size_t(2));
    CHECK(TaxiDestinationsString::Parse("1234 5 6 L777 ", faction, back));
    REQUIRE(back.size() == size_t(2));
    // A malformed stamp is refused, not silently read as zero.
    CHECK(!TaxiDestinationsString::Parse("1234 5 6 Lx ", faction, back, &landing));
    CHECK(!TaxiDestinationsString::Parse("1234 5 6 L ", faction, back, &landing));
}
