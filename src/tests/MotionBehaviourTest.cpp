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

// The kernel's native behaviours (P5-B family 1): pure, driven with scripted Sights.

#include "TestHarness.h"
#include "BehaviourModel.h"
#include "SimpleMoves.h"
#include "DefaultMoves.h"
#include "TrackingMoves.h"
#include "ControlMoves.h"
#include "Utilities/MathDefines.h"

#include <cmath>

using namespace Motion;

TEST(MotionBehaviour_IntentValuesAreKernelSafe)
{
    MoveIntent m = MoveIntent::Move(Vector3(1.0f, 2.0f, 3.0f), MOVE_WALK).AtSpeed(24.0f);
    CHECK(m.act == MoveIntent::Act::Move);
    CHECK(m.Has(MOVE_WALK));
    CHECK_EQ(m.speed, 24.0f);
    EffectLaunch l;
    l.kind = EffectLaunch::Jump;
    l.point = Vector3(4.0f, 5.0f, 6.0f);
    MoveIntent j = MoveIntent::Launch(l);
    CHECK(j.act == MoveIntent::Act::Launch);
    CHECK_EQ(j.launch.point.y, 5.0f);
    Facing f = Facing::ToTarget(0x1234ull);
    CHECK(f.mode == Facing::Mode::Target);
    CHECK_EQ(f.target, uint64(0x1234ull));
}

namespace
{
    Sight Free()
    {
        Sight s;
        s.canReact = true;
        s.canMove = true;
        s.alive = true;
        return s;
    }
    Sight Arrived()
    {
        Sight s = Free();
        s.status.arrived = true;
        return s;
    }
    Sight Cut()
    {
        Sight s = Free();
        s.status.cut = true;
        return s;
    }
    Sight Blocked()
    {
        Sight s = Free();
        s.status.blocked = true;
        return s;
    }
    Sight Traveling()
    {
        Sight s = Free();
        s.status.traveling = true;
        return s;
    }
    Sight Partial()
    {
        Sight s = Free();
        s.status.partial = true;
        return s;
    }
    bool HasEffect(Outcome const& o, Effect::Kind k)
    {
        for (size_t i = 0; i < o.effects.size(); ++i)
        {
            if (o.effects[i].kind == k)
            {
                return true;
            }
        }
        return false;
    }
    PointBehaviour::Params PointTo(float x, float y, float z, uint32 id = 7)
    {
        PointBehaviour::Params p;
        p.id = id;
        p.goal = Vector3(x, y, z);
        return p;
    }

    /// Records every call it answers; a native under test never draws or routes on its own.
    class FakeServices : public Services
    {
        public:
            bool RandomPoint(Vector3 const& centre, float radius, Vector3& out) override
            {
                calls.push_back("random");
                randomRadius = radius;
                if (randomFails)
                {
                    return false;
                }
                out = Vector3(centre.x + radius, centre.y, centre.z);
                return true;
            }
            bool Ground(Vector3 const& at, float& z) override
            {
                calls.push_back("ground");
                z = at.z;
                return true;
            }
            float Frand(float min, float /*max*/) override { calls.push_back("frand"); return min; }
            uint32 Urand(uint32 min, uint32 /*max*/) override { calls.push_back("urand"); return min; }
            int32 Irand(int32 /*min*/, int32 /*max*/) override { calls.push_back("irand"); return irandValue; }
            RouteResult Route(Vector3 const& from, Vector3 const& to, PointsArray& points) override
            {
                calls.push_back("route");
                RouteResult r;
                r.usable = routeUsable;
                r.routed = routeRouted;
                r.partial = routePartial;
                r.progresses = routeProgresses;
                if (routeUsable)
                {
                    // Only a usable route hands back geometry; the adapter's own Route never
                    // appends to whatever `points` already held.
                    points.clear();
                    points.push_back(from);
                    points.push_back(to);
                }
                return r;
            }
            void ResetRoute() override { calls.push_back("resetRoute"); }
            bool CanMove() const override { return canMove; }
            bool Casting() const override { return casting; }
            bool WaypointPaused() const override { return waypointPaused; }
            bool Anchor(Vector3& out) const override
            {
                if (!anchorSet)
                {
                    return false;
                }
                out = anchorPoint;
                return true;
            }
            bool CanFly() const override { return canFly; }   // a live read, like the four above: never logged in `calls`
            bool StandingSpot(Vector3 const& center, float distance2d, float absAngle, Vector3& out) override
            {
                calls.push_back("spot");
                spotCenter = center;
                spotDistance = distance2d;
                spotAngle = absAngle;
                if (spotFails)
                {
                    return false;
                }
                out = Vector3(center.x + distance2d * std::cos(absAngle),
                              center.y + distance2d * std::sin(absAngle),
                              center.z);
                return true;
            }
            bool Fright(uint64 /*rawGuid*/, Vector3& position, float& distance) override
            {
                calls.push_back("fright");
                if (!frightResolved)
                {
                    return false;
                }
                position = frightPosition;
                distance = frightDistance;
                return true;
            }
            bool GroundPoint(Vector3 const& guess, Vector3& out) override
            {
                calls.push_back("groundPoint");
                lastGuess = guess;
                if (groundFails)
                {
                    return false;
                }
                out = guess;   // the fake keeps the guess whole (the shell's drops it onto the floor)
                return true;
            }
            bool ClaimHeld(Motion::Kind kind) const override   // a live read, never logged
            {
                return kind == Motion::Kind::Fear ? fearHeld : (kind == Motion::Kind::Confused ? confuseHeld : false);
            }

            /// Restores every flag/value to its default and clears the call log.
            void Reset()
            {
                routeUsable = false;
                routeRouted = false;
                routePartial = false;
                routeProgresses = false;
                canMove = true;
                casting = false;
                waypointPaused = false;
                anchorSet = false;
                anchorPoint = Vector3();
                canFly = false;
                irandValue = 50;
                randomFails = false;
                spotFails = false;
                spotCenter = Vector3();
                spotDistance = 0.0f;
                spotAngle = 0.0f;
                frightResolved = false;
                frightPosition = Vector3();
                frightDistance = 0.0f;
                groundFails = false;
                lastGuess = Vector3();
                fearHeld = false;
                confuseHeld = false;
                randomRadius = 0.0f;
                calls.clear();
            }

            bool routeUsable = false;
            bool routeRouted = false;
            bool routePartial = false;
            bool routeProgresses = false;
            bool canMove = true;       ///< the live !UNIT_STAT_CAN_NOT_MOVE the prepare re-reads.
            bool casting = false;
            bool waypointPaused = false;
            bool anchorSet = false;
            Vector3 anchorPoint;
            bool canFly = false;       ///< the live Creature::CanFly() the wander re-reads every tick.
            int32 irandValue = 50;     ///< Irand's answer; 50 is at or above 30, so the wander's break path draws the rest through Urand.
            bool randomFails = false;  ///< RandomPoint returns false instead of a point.
            bool spotFails = false;    ///< StandingSpot returns false instead of a point.
            Vector3 spotCenter;        ///< the centre of the last StandingSpot call.
            float spotDistance = 0.0f; ///< its distance2d.
            float spotAngle = 0.0f;    ///< its absAngle.
            bool frightResolved = false;   ///< Fright answers a position and a distance.
            Vector3 frightPosition;
            float frightDistance = 0.0f;
            bool groundFails = false;      ///< GroundPoint returns false instead of the point.
            Vector3 lastGuess;             ///< the last guess GroundPoint was asked about.
            bool fearHeld = false;         ///< ClaimHeld(Fear)
            bool confuseHeld = false;      ///< ClaimHeld(Confused)
            float randomRadius = 0.0f;     ///< the radius of the last RandomPoint ask.
            std::vector<std::string> calls;
    };

    FakeServices g_svc;
}

TEST(MotionBehaviour_PointActivatesThenLaysItsLegOnTheFirstTick)
{
    PointBehaviour b(PointTo(10.0f, 0.0f, 0.0f));
    Step a = b.Activate(Free(), g_svc);
    CHECK(a.stop);
    CHECK(a.resetLeg);
    CHECK(a.roaming == Roaming::SetBoth);
    CHECK(!a.apply);                                     // the leg is laid by the tick, not the activation
    Step t = b.Tick(Free(), g_svc, 100);
    CHECK(t.apply);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t.intent.goal.x, 10.0f);
    CHECK(t.roaming == Roaming::SetBoth);
}

TEST(MotionBehaviour_PointActivationUnderCanNotReactDoesNothing)
{
    PointBehaviour b(PointTo(10.0f, 0.0f, 0.0f));
    Sight s = Free();
    s.canReact = false;
    Step a = b.Activate(s, g_svc);
    CHECK(!a.stop);
    CHECK(a.roaming == Roaming::Keep);
}

TEST(MotionBehaviour_PointHoldsWithMoveBitClearedWhileItCannotMove)
{
    PointBehaviour b(PointTo(10.0f, 0.0f, 0.0f));
    b.Activate(Free(), g_svc);
    Sight s = Free();
    s.canMove = false;
    Step t = b.Tick(s, g_svc, 100);
    CHECK(t.intent.act == MoveIntent::Act::Hold);
    CHECK(t.roaming == Roaming::ClearMove);
}

TEST(MotionBehaviour_PointEndsAndInformsOnArrivedBlockedAndCut)
{
    {
        PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f, 55));
        b.Activate(Free(), g_svc);
        CHECK(b.Tick(Arrived(), g_svc, 100).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Arrived);
        Outcome o = b.Finish(FinishReason::Arrived, Free(), g_svc);
        CHECK(HasEffect(o, Effect::Inform));
        CHECK(HasEffect(o, Effect::SummonedInform));
        CHECK_EQ(o.effects[0].id, 55u);
        CHECK(o.roaming == Roaming::ClearBoth);
    }
    {
        PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
        b.Activate(Free(), g_svc);
        CHECK(b.Tick(Blocked(), g_svc, 100).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Blocked);
        CHECK(HasEffect(b.Finish(FinishReason::Blocked, Free(), g_svc), Effect::Inform));
    }
    {
        PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
        b.Activate(Free(), g_svc);
        CHECK(b.Tick(Cut(), g_svc, 100).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Cut);
        CHECK(HasEffect(b.Finish(FinishReason::Cut, Free(), g_svc), Effect::Inform));   // told anyway, as the generator
    }
}

TEST(MotionBehaviour_PointDisplacedInformsNothingAndInterrupts)
{
    // All three displacing reasons, not just Superseded: the native's Outcome::interrupt is
    // unconditional (the shell's own suspension bookkeeping decides whether to act on it).
    const FinishReason reasons[] = { FinishReason::Superseded, FinishReason::Overridden, FinishReason::Cancelled };
    for (FinishReason reason : reasons)
    {
        PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
        b.Activate(Free(), g_svc);
        b.Tick(Free(), g_svc, 100);
        Outcome o = b.Finish(reason, Free(), g_svc);
        CHECK(o.effects.empty());
        CHECK(o.interrupt);
        CHECK(o.roaming == Roaming::ClearBoth);
    }
}

TEST(MotionBehaviour_PointSuspendedThenResumedWithResetRelaysFromTheSpot)
{
    PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
    b.Activate(Free(), g_svc);
    b.Tick(Free(), g_svc, 100);
    Step s = b.Suspend();
    CHECK(s.interrupt);
    CHECK(s.resetLeg);
    CHECK(s.roaming == Roaming::ClearBoth);
    Step r = b.Resume(Free(), g_svc, true);
    CHECK(r.stop);
    CHECK(r.resetLeg);
    CHECK(b.Resume(Free(), g_svc, false).roaming == Roaming::Keep);
    // A leg cut while suspended is not the behaviour's own end: no inform on a Cleared finish either way.
    Outcome o = b.Finish(FinishReason::Cleared, Free(), g_svc);
    CHECK(!HasEffect(o, Effect::Inform));   // m_done never set: the tick never saw an edge
}

TEST(MotionBehaviour_PointRestatesTheGoalOnPartial)
{
    PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
    b.Activate(Free(), g_svc);
    Step t = b.Tick(Partial(), g_svc, 100);
    CHECK(t.intent.act == MoveIntent::Act::Move);   // not an end
    CHECK_EQ(t.intent.goal.x, 1.0f);                // the same goal, re-stated
}

TEST(MotionBehaviour_AssistRunCallsAssistanceOnEveryNonDisplacingFinishAndNeverInforms)
{
    PointBehaviour::Params p = PointTo(1.0f, 2.0f, 3.0f, 0);
    p.kind = Kind::AssistRun;
    p.flags = MOVE_WALK;
    PointBehaviour b(p);
    b.Activate(Free(), g_svc);
    Step moving = b.Tick(Free(), g_svc, 100);
    CHECK(moving.intent.act == MoveIntent::Act::Move);
    CHECK(moving.intent.Has(MOVE_WALK));            // the flags pass through to the leg
    b.Tick(Arrived(), g_svc, 100);
    Outcome arrived = b.Finish(FinishReason::Arrived, Free(), g_svc);
    CHECK(!HasEffect(arrived, Effect::Inform));
    CHECK(HasEffect(arrived, Effect::CallAssistance));
    CHECK(HasEffect(arrived, Effect::SeekAssistDistract));
    CHECK(arrived.effects[0].kind == Effect::CallAssistance);   // the order: assistance first, then the distract
    Sight dead = Free();
    dead.alive = false;
    Outcome died = b.Finish(FinishReason::Died, dead, g_svc);
    CHECK(HasEffect(died, Effect::CallAssistance));
    CHECK(!HasEffect(died, Effect::SeekAssistDistract));       // not alive: no distract
    CHECK(!HasEffect(b.Finish(FinishReason::Superseded, Free(), g_svc), Effect::CallAssistance));
}

TEST(MotionBehaviour_DistractCountsDownStrictlyAndAssistDistractAttacksOnFinish)
{
    DistractBehaviour d(Kind::Distract, 1000);
    CHECK(d.Tick(Free(), g_svc, 400).intent.act != MoveIntent::Act::Done);
    CHECK(d.Tick(Free(), g_svc, 600).intent.act != MoveIntent::Act::Done);   // equality: alive at zero
    CHECK(d.Tick(Free(), g_svc, 1).intent.act == MoveIntent::Act::Done);
    CHECK(d.EndReason(Free()) == FinishReason::Expired);
    CHECK(d.Finish(FinishReason::Expired, Free(), g_svc).effects.empty());
    DistractBehaviour a(Kind::AssistDistract, 10);
    CHECK(HasEffect(a.Finish(FinishReason::Superseded, Free(), g_svc), Effect::AttackVictim));
    CHECK(HasEffect(a.Finish(FinishReason::Expired, Free(), g_svc), Effect::AttackVictim));
}

TEST(MotionBehaviour_EffectLaunchesOnceHoldsWhileTravelingAndEnds)
{
    EffectLaunch l;
    l.kind = EffectLaunch::Jump;
    l.point = Vector3(5.0f, 5.0f, 0.0f);
    EffectBehaviour e(66, l);
    Step a = e.Activate(Free(), g_svc);
    CHECK(a.apply);
    CHECK(a.intent.act == MoveIntent::Act::Launch);
    CHECK(!e.Activate(Free(), g_svc).apply);   // once
    CHECK(e.Tick(Traveling(), g_svc, 100).intent.act == MoveIntent::Act::Hold);
    Sight landed = Free();
    landed.landed = true;
    CHECK(e.Tick(landed, g_svc, 100).intent.act == MoveIntent::Act::Done);
    CHECK(e.EndReason(landed) == FinishReason::Arrived);
    Outcome o = e.Finish(FinishReason::Arrived, landed, g_svc);
    CHECK(o.effects.size() == 2);
    CHECK(o.effects[0].kind == Effect::Inform);
    CHECK_EQ(o.effects[0].id, 66u);
    CHECK(o.effects[1].kind == Effect::ReengageVictim);
}

TEST(MotionBehaviour_EffectDisplacedInformsOnlyIfLanded_CutOnItsOwnTickInforms)
{
    EffectLaunch l;
    l.kind = EffectLaunch::Jump;
    // All three displacing reasons, not just Superseded.
    const FinishReason displacing[] = { FinishReason::Superseded, FinishReason::Overridden, FinishReason::Cancelled };
    for (FinishReason reason : displacing)
    {
        EffectBehaviour e(71, l);
        e.Activate(Free(), g_svc);
        CHECK(e.Finish(reason, Free(), g_svc).effects.empty());    // flying: nothing happened
        Sight landed = Free();
        landed.landed = true;
        Outcome o = e.Finish(reason, landed, g_svc);
        CHECK(HasEffect(o, Effect::Inform));            // landed, unconsumed: the effect happened
        CHECK(HasEffect(o, Effect::ReengageVictim));    // the whole finalizer runs when landed, re-engage included
    }
    {
        EffectBehaviour e(72, l);
        e.Activate(Free(), g_svc);
        CHECK(e.Tick(Cut(), g_svc, 100).intent.act == MoveIntent::Act::Done);   // not traveling: over
        CHECK(e.EndReason(Cut()) == FinishReason::Cut);
        Outcome o = e.Finish(FinishReason::Cut, Cut(), g_svc);
        CHECK(HasEffect(o, Effect::Inform));            // its own tick ended it: told anyway
        CHECK(HasEffect(o, Effect::ReengageVictim));
    }
    {
        EffectBehaviour e(73, l);
        e.Activate(Free(), g_svc);
        Outcome o = e.Finish(FinishReason::Died, Free(), g_svc);   // not displacing, not landed, not done
        CHECK(!HasEffect(o, Effect::Inform));
        CHECK(HasEffect(o, Effect::ReengageVictim));        // the re-engage is independent of the inform
    }
}

TEST(MotionBehaviour_ChargeRelaysOnDriftWithinBudgetAndEndsWhenTheTargetIsLost)
{
    PointBehaviour::Params p = PointTo(0.0f, 0.0f, 0.0f, 0);
    p.target = 0x42ull;
    p.speed = 24.0f;
    p.informs = false;
    PointBehaviour b(p);
    CHECK(b.TracksTarget());
    CHECK_EQ(b.Target(), uint64(0x42ull));
    b.Activate(Free(), g_svc);
    Sight s = Free();
    s.hasTarget = true;
    s.targetPoint = Vector3(10.0f, 0.0f, 0.0f);
    Step t1 = b.Tick(s, g_svc, 100);
    CHECK(t1.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t1.intent.goal.x, 10.0f);
    CHECK_EQ(t1.intent.speed, 24.0f);
    s.targetPoint = Vector3(11.0f, 0.0f, 0.0f);            // 1 yd: under the tolerance
    CHECK_EQ(b.Tick(s, g_svc, 100).intent.goal.x, 10.0f);
    CHECK_EQ(b.RelayCount(), 0u);
    s.targetPoint = Vector3(13.0f, 0.0f, 0.0f);            // 3 yd, but only 200 ms since the leg
    CHECK_EQ(b.Tick(s, g_svc, 100).intent.goal.x, 10.0f);
    CHECK_EQ(b.Tick(s, g_svc, 300).intent.goal.x, 13.0f);         // 600 ms: within budget, re-laid
    CHECK_EQ(b.RelayCount(), 1u);
    // A leg that ended where the target WAS: the target walked on past the tolerance, so a
    // fresh leg is laid at once (no budget wait), not an arrival.
    {
        Sight ended = s;
        ended.status.arrived = true;
        ended.targetPoint = Vector3(16.0f, 0.0f, 0.0f);   // 3 yd past the laid goal of 13
        Step again = b.Tick(ended, g_svc, 50);
        CHECK(again.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(again.intent.goal.x, 16.0f);
        CHECK_EQ(b.RelayCount(), 2u);
        // ... and a leg that ended within the tolerance is an arrival.
        Sight close = s;
        close.status.arrived = true;
        close.targetPoint = Vector3(16.5f, 0.0f, 0.0f);
        CHECK(b.Tick(close, g_svc, 50).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(close) == FinishReason::Arrived);
    }
    {
        PointBehaviour::Params lp = PointTo(0.0f, 0.0f, 0.0f, 0);
        lp.target = 0x42ull;
        lp.informs = false;
        PointBehaviour lost(lp);
        lost.Activate(Free(), g_svc);
        Sight gone = Free();
        gone.hasTarget = false;
        CHECK(lost.Tick(gone, g_svc, 100).intent.act == MoveIntent::Act::Done);
        CHECK(lost.EndReason(gone) == FinishReason::TargetLost);
        CHECK(lost.Finish(FinishReason::TargetLost, gone, g_svc).effects.empty());   // never informs
    }

    // `informs = false` must hold on a real edge too, not only on TargetLost (which never
    // sets m_done and so would pass this check vacuously even with the guard deleted).
    {
        PointBehaviour::Params ap = PointTo(0.0f, 0.0f, 0.0f, 0);
        ap.target = 0x42ull;
        ap.informs = false;
        PointBehaviour arriving(ap);
        arriving.Activate(Free(), g_svc);
        Sight there = Free();
        there.hasTarget = true;
        there.targetPoint = Vector3(10.0f, 0.0f, 0.0f);
        CHECK(arriving.Tick(there, g_svc, 100).intent.act == MoveIntent::Act::Move);   // the first leg
        there.status.arrived = true;                                             // ended at the target's point
        CHECK(arriving.Tick(there, g_svc, 100).intent.act == MoveIntent::Act::Done);
        CHECK(arriving.EndReason(there) == FinishReason::Arrived);
        Outcome arrivedOutcome = arriving.Finish(FinishReason::Arrived, there, g_svc);
        CHECK(!HasEffect(arrivedOutcome, Effect::Inform));
        CHECK(!HasEffect(arrivedOutcome, Effect::SummonedInform));
    }

    // The swoop: no tracked target, `informs = false`, a fixed goal — same silence on arrival.
    {
        PointBehaviour::Params sp = PointTo(1.0f, 2.0f, 3.0f, 0);
        sp.informs = false;
        PointBehaviour swoop(sp);
        swoop.Activate(Free(), g_svc);
        CHECK(swoop.Tick(Arrived(), g_svc, 100).intent.act == MoveIntent::Act::Done);
        Outcome swoopOutcome = swoop.Finish(FinishReason::Arrived, Free(), g_svc);
        CHECK(!HasEffect(swoopOutcome, Effect::Inform));
        CHECK(!HasEffect(swoopOutcome, Effect::SummonedInform));
    }
}

TEST(MotionBehaviour_IdleDoesNothingAndFinishesSilently)
{
    IdleBehaviour i;
    CHECK(!i.Activate(Free(), g_svc).apply);
    CHECK(!i.Tick(Free(), g_svc, 100).apply);
    CHECK(i.Finish(FinishReason::Superseded, Free(), g_svc).effects.empty());
}

TEST(MotionBehaviour_FlyLandLaysAStraightFlyingLegAndInformsAsAPoint)
{
    PointBehaviour::Params p = PointTo(4.0f, 5.0f, 6.0f, 5);
    p.kind = Kind::FlyLand;
    p.flags = MOVE_FLY | MOVE_STRAIGHT;
    PointBehaviour b(p);
    CHECK(b.Kind() == Kind::FlyLand);
    b.Activate(Free(), g_svc);
    Step t = b.Tick(Free(), g_svc, 100);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK(t.intent.Has(MOVE_FLY));
    CHECK(t.intent.Has(MOVE_STRAIGHT));
    CHECK(b.Tick(Arrived(), g_svc, 100).intent.act == MoveIntent::Act::Done);
    Outcome o = b.Finish(FinishReason::Arrived, Free(), g_svc);
    CHECK(HasEffect(o, Effect::Inform));
    CHECK(o.effects[0].who == Kind::FlyLand);
    CHECK_EQ(o.effects[0].id, 5u);
}

TEST(MotionBehaviour_EffectFactoriesSetOnlyTheirFields)
{
    Effect raw = Effect::Raw(7, 3);
    CHECK(raw.kind == Effect::InformRaw);
    CHECK_EQ(raw.raw, 7u);
    CHECK_EQ(raw.id, 3u);
    CHECK(!raw.flag);
    CHECK(raw.who == Kind::Idle);

    Effect walk = Effect::Walk(true);
    CHECK(walk.kind == Effect::SetWalk);
    CHECK(walk.flag);
    CHECK_EQ(walk.raw, 0u);
    CHECK_EQ(walk.id, 0u);

    Step s;
    CHECK(s.effects.empty());
    CHECK(!s.again);
}

TEST(MotionBehaviour_WanderDrawsTheGeneratorsSequence)
{
    FakeServices svc;
    WanderBehaviour::Params p;
    p.centre = Vector3(0.0f, 0.0f, 0.0f);
    p.radius = 10.0f;
    WanderBehaviour w(p);
    Step a = w.Activate(Free(), svc);
    CHECK_EQ(svc.calls.size(), size_t(2));                 // tilt, angle: frand, frand
    CHECK(svc.calls[0] == "frand" && svc.calls[1] == "frand");
    CHECK(a.roaming == Roaming::SetRoam);
    svc.calls.clear();
    Step t = w.Tick(Free(), svc, 100);                       // rest passed at once: a hop
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK(t.intent.Has(MOVE_WALK));
    CHECK(t.roaming == Roaming::SetMove);
    CHECK_EQ(svc.calls.size(), size_t(3));                 // random point, the no-break roll, the rest
    CHECK(svc.calls[0] == "random" && svc.calls[1] == "irand" && svc.calls[2] == "urand");
    svc.calls.clear();
    Sight moving = Free();
    moving.status.traveling = true;
    CHECK(w.Tick(moving, svc, 100).intent.act == MoveIntent::Act::Move);   // re-stated, no draw
    CHECK(svc.calls.empty());
    Sight arrived = Free();
    arrived.status.arrived = true;
    CHECK(w.Tick(arrived, svc, 100).intent.act == MoveIntent::Act::Hold);  // resting 3000 ms
    CHECK(svc.calls.empty());
    CHECK(w.Tick(Free(), svc, 2800).intent.act == MoveIntent::Act::Hold);  // 100 ms left
    CHECK(w.Tick(Free(), svc, 100).intent.act == MoveIntent::Act::Move);   // 3000 ms: the next hop
}

TEST(MotionBehaviour_WanderNoBreakAndAirborneSkipTheRest)
{
    FakeServices svc;
    svc.irandValue = 10;                                     // < 30: no break
    WanderBehaviour::Params p;
    p.radius = 10.0f;
    WanderBehaviour w(p);
    w.Activate(Free(), svc);
    svc.calls.clear();
    w.Tick(Free(), svc, 100);
    CHECK_EQ(svc.calls.size(), size_t(2));                 // random point, irand; no urand
    Sight arrived = Free();
    arrived.status.arrived = true;
    w.Tick(arrived, svc, 100);
    CHECK(w.Tick(Free(), svc, 50).intent.act == MoveIntent::Act::Move);   // 50 ms retry rest
    FakeServices air;
    air.canFly = true;                                       // the live read, not a parameter: the generator asked CanFly() each tick
    WanderBehaviour::Params ap;
    ap.radius = 10.0f;
    ap.verticalZ = 5.0f;
    WanderBehaviour f(ap);
    f.Activate(Free(), air);
    air.calls.clear();
    Step hop = f.Tick(Free(), air, 100);
    CHECK(hop.intent.Has(MOVE_FLY) && hop.intent.Has(MOVE_STRAIGHT));
    CHECK_EQ(air.calls.size(), size_t(3));                 // frand step, frand radius, ground; no roll, no rest
    CHECK(air.calls[0] == "frand" && air.calls[1] == "frand" && air.calls[2] == "ground");

    // The same vertical band with the flight taken away: a ground hop, drawn the ground way.
    // Give it back and the very next hop orbits -- nothing about the leash was re-requested.
    {
        FakeServices ground;                                 // canFly defaults false
        WanderBehaviour::Params gp;
        gp.radius = 10.0f;
        gp.verticalZ = 5.0f;
        WanderBehaviour g(gp);
        g.Activate(Free(), ground);
        ground.calls.clear();
        Step walked = g.Tick(Free(), ground, 100);
        CHECK(walked.intent.Has(MOVE_WALK));
        CHECK(!walked.intent.Has(MOVE_FLY));
        CHECK_EQ(ground.calls.size(), size_t(3));            // random point, the no-break roll, the rest
        CHECK(ground.calls[0] == "random" && ground.calls[1] == "irand" && ground.calls[2] == "urand");

        ground.canFly = true;
        ground.calls.clear();
        Sight landed = Free();
        landed.status.arrived = true;
        g.Tick(landed, ground, 100);                         // lands; the 3000 ms rest the ground hop banked runs
        Step orbited = g.Tick(Free(), ground, 3000);
        CHECK(orbited.intent.Has(MOVE_FLY) && orbited.intent.Has(MOVE_STRAIGHT));
        CHECK_EQ(ground.calls.size(), size_t(3));            // frand step, frand radius, ground; no roll, no rest
        CHECK(ground.calls[0] == "frand" && ground.calls[1] == "frand" && ground.calls[2] == "ground");
    }
}

TEST(MotionBehaviour_WanderRetriesWithBackoffAndRestoresTheWalk)
{
    FakeServices svc;
    svc.randomFails = true;
    WanderBehaviour::Params p;
    p.radius = 10.0f;
    WanderBehaviour w(p);
    w.Activate(Free(), svc);
    CHECK(w.Tick(Free(), svc, 100).intent.act == MoveIntent::Act::Hold);   // no point: 50 ms
    CHECK(w.Tick(Free(), svc, 49).intent.act == MoveIntent::Act::Hold);
    CHECK(w.Tick(Free(), svc, 1).intent.act == MoveIntent::Act::Hold);     // retried: 100 ms now
    CHECK(w.Tick(Free(), svc, 99).intent.act == MoveIntent::Act::Hold);
    Sight blocked = Free();
    blocked.status.blocked = true;
    w.Tick(blocked, svc, 1);                                 // a blocked leg also backs off
    Sight running = Free();
    running.runningState = true;
    Outcome o = w.Finish(FinishReason::Cleared, running, svc);
    CHECK(o.roaming == Roaming::ClearBoth);
    CHECK(o.effects.size() == 1 && o.effects[0].kind == Effect::SetWalk && !o.effects[0].flag);
    w.Tick(running, svc, 1);                                 // runningState true: the last hook Suspend reads
    Step s = w.Suspend();
    CHECK(s.interrupt && s.resetLeg);
    CHECK(s.roaming == Roaming::ClearBoth);
    CHECK(s.effects.size() == 1 && s.effects[0].kind == Effect::SetWalk && !s.effects[0].flag);
}

TEST(MotionBehaviour_WanderStopsOnlyOnADisplacingFinish)
{
    FakeServices svc;
    WanderBehaviour::Params p;
    p.radius = 10.0f;
    WanderBehaviour w(p);
    w.Activate(Free(), svc);
    Sight running = Free();
    running.runningState = true;

    Outcome superseded = w.Finish(FinishReason::Superseded, running, svc);
    CHECK(superseded.interrupt);
    CHECK(superseded.roaming == Roaming::ClearBoth);
    CHECK(superseded.effects.size() == 1 && superseded.effects[0].kind == Effect::SetWalk && superseded.effects[0].flag == !running.runningState);

    Outcome cleared = w.Finish(FinishReason::Cleared, running, svc);
    CHECK(!cleared.interrupt);
    CHECK(cleared.roaming == Roaming::ClearBoth);
    CHECK(cleared.effects.size() == 1 && cleared.effects[0].kind == Effect::SetWalk && cleared.effects[0].flag == !running.runningState);

    CHECK(w.Finish(FinishReason::Overridden, running, svc).interrupt);
    CHECK(w.Finish(FinishReason::Cancelled, running, svc).interrupt);
    CHECK(!w.Finish(FinishReason::Expired, running, svc).interrupt);
}

namespace
{
    PatrolBehaviour::Node MakeNode(uint32 id, float x, float y, float z)
    {
        PatrolBehaviour::Node n;
        n.id = id;
        n.pos = Vector3(x, y, z);
        return n;
    }
}

TEST(MotionBehaviour_PatrolArrivesInTheGeneratorsOrder)
{
    FakeServices svc;
    svc.routeUsable = true;
    svc.routeRouted = true;   // every leg is a real route: welding is on the table

    PatrolBehaviour::Node n1 = MakeNode(1, 10.0f, 0.0f, 0.0f);
    PatrolBehaviour::Node n2 = MakeNode(2, 20.0f, 0.0f, 0.0f);
    n2.scriptId = 5;
    n2.emote = 6;
    n2.textIds = { 100, 101 };
    n2.textAnywhere = true;
    n2.delay = 1000;
    PatrolBehaviour::Node n3 = MakeNode(3, 30.0f, 0.0f, 0.0f);

    PatrolBehaviour::Params p;
    p.nodes = { n1, n2, n3 };
    p.inform.waypoint = 411;

    PatrolBehaviour b(p);
    b.Activate(Free(), svc);

    Sight start = Free();
    start.position = Vector3(0.0f, 0.0f, 0.0f);
    Step first = b.Tick(start, svc, 100);
    CHECK(first.apply);
    CHECK(first.intent.act == MoveIntent::Act::Move);
    // Node 1 carries no delay/script/behaviour, so the weld passes straight through it; node
    // 2's script stops the weld there -- the leg's destination is node 2, not node 1.
    CHECK_EQ(b.LegPointCount(), size_t(3));
    CHECK_EQ(first.intent.goal.x, 20.0f);
    CHECK(first.intent.Has(MOVE_REQUIRE_PATH));
    CHECK(first.intent.Has(MOVE_WALK));
    CHECK(first.roaming == Roaming::SetMove);
    CHECK_EQ(first.effects.size(), size_t(1));
    CHECK(first.effects[0].kind == Effect::SetWalk);
    CHECK(first.effects[0].flag);

    // The welding pass reset the router before its first route, as the generator built a fresh
    // query per BuildSmoothPath pass; the legs welded inside the pass then share it.
    {
        size_t reset = svc.calls.size();
        size_t route = svc.calls.size();
        for (size_t i = 0; i < svc.calls.size(); ++i)
        {
            if (reset == svc.calls.size() && svc.calls[i] == "resetRoute") { reset = i; }
            if (route == svc.calls.size() && svc.calls[i] == "route") { route = i; }
        }
        CHECK(route < svc.calls.size());
        CHECK(reset < route);
    }

    // The whole welded spline (start, node 1, node 2) finalizes at once: both endpoints are
    // reached together.
    Sight finalized = Free();
    finalized.status.traveling = false;
    finalized.status.pathIndex = 2;

    Step atNode1 = b.Tick(finalized, svc, 100);
    CHECK(atNode1.again);
    CHECK(atNode1.roaming == Roaming::ClearMove);
    CHECK_EQ(atNode1.effects.size(), size_t(1));
    CHECK(atNode1.effects[0].kind == Effect::InformRaw);
    CHECK_EQ(atNode1.effects[0].raw, 411u);
    CHECK_EQ(atNode1.effects[0].id, 1u);

    Step atNode2 = b.Tick(finalized, svc, 0);
    CHECK(atNode2.again);
    CHECK(atNode2.roaming == Roaming::ClearMove);
    CHECK_EQ(atNode2.effects.size(), size_t(4));
    CHECK(atNode2.effects[0].kind == Effect::RunScript);
    CHECK_EQ(atNode2.effects[0].id, 5u);
    CHECK(atNode2.effects[1].kind == Effect::Emote);
    CHECK_EQ(atNode2.effects[1].id, 6u);
    CHECK(atNode2.effects[2].kind == Effect::Say);
    CHECK_EQ(atNode2.effects[2].id, 100u);   // Urand(0, 1) answers min = 0 -> textIds[0]
    CHECK(atNode2.effects[3].kind == Effect::InformRaw);
    CHECK_EQ(atNode2.effects[3].raw, 411u);
    CHECK_EQ(atNode2.effects[3].id, 2u);

    // Node 2's two text ids draw exactly one Urand pick and touch no other RNG method: the
    // shared stream is the native's alone, and its draw count is exactly what the generator drew.
    {
        int urandCount = 0, frandCount = 0, irandCount = 0, randomCount = 0, groundCount = 0;
        for (std::string const& c : svc.calls)
        {
            if (c == "urand") { ++urandCount; }
            else if (c == "frand") { ++frandCount; }
            else if (c == "irand") { ++irandCount; }
            else if (c == "random") { ++randomCount; }
            else if (c == "ground") { ++groundCount; }
        }
        CHECK_EQ(urandCount, 1);
        CHECK_EQ(frandCount, 0);
        CHECK_EQ(irandCount, 0);
        CHECK_EQ(randomCount, 0);
        CHECK_EQ(groundCount, 0);
    }

    Step guarded = b.Tick(finalized, svc, 0);   // the trailing OnArrived, latch-guarded: nothing
    CHECK(guarded.again);
    CHECK(guarded.effects.empty());
    CHECK(guarded.roaming == Roaming::Keep);

    Step waiting = b.Tick(finalized, svc, 0);   // node 2's 1000 ms delay is still running
    CHECK(waiting.apply);
    CHECK(waiting.intent.act == MoveIntent::Act::Hold);

    // The wait passed: prepare from node 2 toward node 3, node 3 has no behaviour either, so
    // the weld keeps going and wraps clean around the cyclic path onto node 1, stopping only
    // at node 2's own script -- the destination is node 2 again, by the long way round.
    Step prepared = b.Tick(Free(), svc, 1000);
    CHECK(prepared.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(b.LegPointCount(), size_t(4));   // start, node 3, node 1, node 2
    CHECK_EQ(prepared.intent.goal.x, 20.0f);

    // A node with exactly one text id draws no Urand: the pick only happens with two or more
    // (a separate, single-node path, so the wraparound demonstration above stays untouched).
    {
        FakeServices oneText;
        PatrolBehaviour::Node solo = MakeNode(1, 5.0f, 0.0f, 0.0f);
        solo.textIds = { 200 };
        solo.textAnywhere = true;
        PatrolBehaviour::Params sp;
        sp.nodes = { solo };
        PatrolBehaviour ob(sp);
        ob.Activate(Free(), oneText);
        ob.Tick(Free(), oneText, 0);           // prepares the only leg
        Sight finalizedSolo = Free();
        finalizedSolo.status.traveling = false;
        Step arrived = ob.Tick(finalizedSolo, oneText, 0);
        CHECK_EQ(arrived.effects.size(), size_t(2));
        CHECK(arrived.effects[0].kind == Effect::Say);
        CHECK_EQ(arrived.effects[0].id, 200u);
        CHECK(arrived.effects[1].kind == Effect::InformRaw);
        bool sawUrand = false;
        for (std::string const& c : oneText.calls) { if (c == "urand") { sawUrand = true; } }
        CHECK(!sawUrand);
    }
}

TEST(MotionBehaviour_PatrolExternalPrepareInformEndsTheRoundAndHonoursSetNextWaypoint)
{
    FakeServices svc;   // routeUsable defaults false: an externally-scripted path never welds anyway

    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 0.0f, 0.0f, 0.0f), MakeNode(2, 10.0f, 0.0f, 0.0f), MakeNode(3, 20.0f, 0.0f, 0.0f) };
    p.external = true;
    p.externalOrigin = true;
    p.inform.externalMove = 700;
    p.inform.externalStart = 701;
    p.inform.externalLast = 702;

    PatrolBehaviour b(p);
    b.Activate(Free(), svc);
    b.Tick(Free(), svc, 0);                     // the first leg: straight to node 1, the starting node

    Sight finalized = Free();
    finalized.status.traveling = false;
    b.Tick(finalized, svc, 0);                  // arrives at node 1: Raw(externalMove, 1), consumed

    Step informed = b.Tick(finalized, svc, 0);   // the arrivals phase drains; StartPrepare's external inform fires
    CHECK(informed.again);                       // the round ends on the inform; the shell re-checks the selection
    CHECK(!informed.apply);                      // and nothing is laid before the hook has run
    CHECK_EQ(informed.effects.size(), size_t(1));
    CHECK(informed.effects[0].kind == Effect::InformRaw);
    CHECK_EQ(informed.effects[0].raw, 701u);      // externalStart: node 2 was next
    CHECK_EQ(informed.effects[0].id, 2u);

    CHECK(b.SetNextWaypoint(3));                 // the hook retargets the patrol, as it may

    Step toNode3 = b.Tick(finalized, svc, 0);    // the round after: the hook's node wins over the named one
    CHECK(toNode3.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(toNode3.intent.goal.x, 20.0f);
    CHECK_EQ(b.CurrentNode(), 3u);

    // SetNextWaypoint's Reset(1) is the generator's own: the very next external tick sees the
    // 1 ms wait not yet passed, and -- since it was called from inside the PrepareInform hook,
    // one tick too late to affect the leg PrepareLeg just built -- re-runs StartPrepare once
    // more (rebuilding the same leg, arrivalDone still false) before the leg actually gets to
    // run.
    Step rebuilt = b.Tick(finalized, svc, 1);
    CHECK(rebuilt.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(rebuilt.intent.goal.x, 20.0f);

    b.Tick(finalized, svc, 0);                   // arrives at node 3: Raw(externalMove, 3), consumed
    Step wrap = b.Tick(finalized, svc, 0);       // node 3 is last: the wrap uses externalLast, naming node 1
    CHECK(wrap.again);
    CHECK_EQ(wrap.effects.size(), size_t(1));
    CHECK(wrap.effects[0].kind == Effect::InformRaw);
    CHECK_EQ(wrap.effects[0].raw, 702u);          // externalLast
    CHECK_EQ(wrap.effects[0].id, 1u);             // wrapped back to node 1
}

TEST(MotionBehaviour_PatrolSkipsDeadNodesAndForcesALegAfterALap)
{
    FakeServices svc;   // routeUsable defaults false: a plain, unwelded leg each time

    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 0.0f, 0.0f, 0.0f), MakeNode(2, 10.0f, 0.0f, 0.0f) };
    PatrolBehaviour b(p);
    b.Activate(Free(), svc);

    Step first = b.Tick(Blocked(), svc, 100);    // the first unreachable node: 50 ms, then on to node 2
    CHECK(first.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(first.intent.goal.x, 10.0f);
    CHECK(first.intent.Has(MOVE_REQUIRE_PATH));
    CHECK_EQ(b.CurrentNode(), 2u);

    Step second = b.Tick(Blocked(), svc, 100);   // a whole lap unreachable: the next leg is forced, unrouted
    CHECK(second.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(second.intent.goal.x, 0.0f);
    CHECK(!second.intent.Has(MOVE_REQUIRE_PATH));
    CHECK_EQ(b.CurrentNode(), 1u);
}

TEST(MotionBehaviour_PatrolFacesOnlyAWaitingNode)
{
    FakeServices svc;
    {
        PatrolBehaviour::Node n = MakeNode(1, 5.0f, 0.0f, 0.0f);
        n.orientation = 1.5f;
        n.delay = 500;
        PatrolBehaviour::Params p;
        p.nodes = { n };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        Step t = b.Tick(Free(), svc, 100);
        CHECK(t.intent.facing.mode == Facing::Mode::Angle);
        CHECK_EQ(t.intent.facing.angle, 1.5f);
    }
    {
        PatrolBehaviour::Node n = MakeNode(1, 5.0f, 0.0f, 0.0f);
        n.orientation = 1.5f;   // a heading, but nothing to stop for: no facing
        n.delay = 0;
        PatrolBehaviour::Params p;
        p.nodes = { n };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        Step t = b.Tick(Free(), svc, 100);
        CHECK(t.intent.facing.mode == Facing::Mode::None);
    }
    {
        PatrolBehaviour::Node n = MakeNode(1, 5.0f, 0.0f, 0.0f);
        n.delay = 500;   // a wait, but no heading on the node (orientation left at 100 = none)
        PatrolBehaviour::Params p;
        p.nodes = { n };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        Step t = b.Tick(Free(), svc, 100);
        CHECK(t.intent.facing.mode == Facing::Mode::None);
    }
}

namespace
{
    /// A Services stub whose route always lands `SEAM` yards short of the requested goal --
    /// mimicking a navmesh-adjusted endpoint -- so BuildSmoothPath's cross-leg seam check has
    /// something to trip over.
    class SeamServices : public Services
    {
        public:
            bool RandomPoint(Vector3 const&, float, Vector3&) override { return false; }
            bool Ground(Vector3 const&, float&) override { return false; }
            float Frand(float min, float) override { return min; }
            uint32 Urand(uint32 min, uint32) override { return min; }
            int32 Irand(int32 min, int32) override { return min; }
            RouteResult Route(Vector3 const& from, Vector3 const& to, PointsArray& points) override
            {
                RouteResult r;
                r.usable = true;
                r.routed = true;
                points.clear();
                points.push_back(from);
                Vector3 end = to;
                if (to.x > 15.0f)   // only the leg into node 3 lands short
                {
                    end.x -= 0.2f;
                }
                points.push_back(end);
                return r;
            }
            void ResetRoute() override {}
            bool CanMove() const override { return true; }
            bool Casting() const override { return false; }
            bool WaypointPaused() const override { return false; }
            bool Anchor(Vector3&) const override { return false; }
            bool CanFly() const override { return false; }
            bool StandingSpot(Vector3 const&, float, float, Vector3&) override { return false; }
            bool Fright(uint64, Vector3&, float&) override { return false; }
            bool GroundPoint(Vector3 const& guess, Vector3& out) override { out = guess; return true; }
            bool ClaimHeld(Motion::Kind) const override { return false; }
    };

    /// A Services stub whose route hands back a middle point within the drop tolerance of its
    /// own endpoint, so the merge's within-leg near-duplicate filter has something to drop.
    class DupPointServices : public Services
    {
        public:
            bool RandomPoint(Vector3 const&, float, Vector3&) override { return false; }
            bool Ground(Vector3 const&, float&) override { return false; }
            float Frand(float min, float) override { return min; }
            uint32 Urand(uint32 min, uint32) override { return min; }
            int32 Irand(int32 min, int32) override { return min; }
            RouteResult Route(Vector3 const& from, Vector3 const& to, PointsArray& points) override
            {
                RouteResult r;
                r.usable = true;
                r.routed = true;
                points.clear();
                points.push_back(from);
                Vector3 near = to;
                near.x -= 0.05f;   // under WAYPOINT_SMOOTHING_MIN_SEGMENT_LENGTH (0.1)
                points.push_back(near);
                points.push_back(to);
                return r;
            }
            void ResetRoute() override {}
            bool CanMove() const override { return true; }
            bool Casting() const override { return false; }
            bool WaypointPaused() const override { return false; }
            bool Anchor(Vector3&) const override { return false; }
            bool CanFly() const override { return false; }
            bool StandingSpot(Vector3 const&, float, float, Vector3&) override { return false; }
            bool Fright(uint64, Vector3&, float&) override { return false; }
            bool GroundPoint(Vector3 const& guess, Vector3& out) override { out = guess; return true; }
            bool ClaimHeld(Motion::Kind) const override { return false; }
    };
}

TEST(MotionBehaviour_PatrolWeldingRules)
{
    // usable && !routed: a straight-line fallback is not welded through.
    {
        FakeServices svc;
        svc.routeUsable = true;
        svc.routeRouted = false;
        PatrolBehaviour::Params p;
        p.nodes = { MakeNode(1, 5.0f, 0.0f, 0.0f), MakeNode(2, 10.0f, 0.0f, 0.0f) };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        b.Tick(Free(), svc, 100);
        CHECK_EQ(b.LegPointCount(), size_t(0));
    }
    // A seam >= 0.1 yd between two legs stops the weld from extending further, keeping what
    // was already welded.
    {
        SeamServices svc;
        PatrolBehaviour::Params p;
        p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f), MakeNode(3, 30.0f, 0.0f, 0.0f) };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        Step t = b.Tick(Free(), svc, 100);
        CHECK_EQ(b.LegPointCount(), size_t(3));   // start, node 1, node 2 -- the seam into node 3 stops it there
        CHECK_EQ(t.intent.goal.x, 20.0f);
    }
    // The packable offset budget rolls a rejected point back rather than keeping a partial one.
    {
        FakeServices svc;
        svc.routeUsable = true;
        svc.routeRouted = true;
        PatrolBehaviour::Params p;
        p.nodes = { MakeNode(1, 50.0f, 0.0f, 0.0f), MakeNode(2, 100.0f, 0.0f, 0.0f), MakeNode(3, 300.0f, 0.0f, 0.0f) };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        Step t = b.Tick(Free(), svc, 100);
        CHECK_EQ(b.LegPointCount(), size_t(3));   // 0, 50, 100 -- node 3 would blow the 200 yd XY span
        CHECK_EQ(t.intent.goal.x, 100.0f);
    }
    // A full lap stops before welding the path onto itself.
    {
        FakeServices svc;
        svc.routeUsable = true;
        svc.routeRouted = true;
        PatrolBehaviour::Params p;
        p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f) };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        Step t = b.Tick(Free(), svc, 100);
        CHECK_EQ(b.LegPointCount(), size_t(3));   // not 32-ish: the wrap back to node 1 is refused
        CHECK_EQ(t.intent.goal.x, 20.0f);
    }
    // A near-duplicate point inside a single leg is dropped by the merge.
    {
        DupPointServices svc;
        PatrolBehaviour::Params p;
        p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f) };
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        b.Tick(Free(), svc, 100);
        // Each leg's own endpoint lands 0.05 yd from the point already kept and is dropped: 3
        // surviving points, not 5.
        CHECK_EQ(b.LegPointCount(), size_t(3));
    }
}

TEST(MotionBehaviour_PatrolResetPosition)
{
    FakeServices svc;
    PatrolBehaviour::Node n1 = MakeNode(1, 0.0f, 0.0f, 0.0f);
    PatrolBehaviour::Node n2 = MakeNode(2, 10.0f, 0.0f, 0.0f);
    PatrolBehaviour::Node n3 = MakeNode(3, 10.0f, 10.0f, 0.0f);
    n3.orientation = 1.25f;
    PatrolBehaviour::Params p;
    p.nodes = { n1, n2, n3 };

    // The anchor: face the node the patrol was heading for, not the anchor's own facing.
    {
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        CHECK(b.SetNextWaypoint(2));   // heading for node 2 at (10, 0, 0)
        FakeServices anchored;
        anchored.anchorSet = true;
        anchored.anchorPoint = Vector3(0.0f, 0.0f, 0.0f);
        Sight sight = Free();
        sight.facing = 0.75f;
        Vector3 pos;
        float o = 0.5f;
        CHECK(b.ResetPosition(sight, anchored, pos, o));
        CHECK(pos == Vector3(0.0f, 0.0f, 0.0f));
        CHECK_EQ(o, 0.0f);   // atan2(0, 10) == 0
    }
    // The last node reached, with a fixed orientation: the node's own heading, no bearing math.
    {
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        CHECK(b.SetNextWaypoint(3));
        FakeServices unwelded;   // routeUsable defaults false: an unwelded, single-node leg
        b.Tick(Free(), unwelded, 1);            // SetNextWaypoint's 1 ms wait passes
        Sight finalized = Free();
        finalized.status.traveling = false;
        b.Tick(finalized, unwelded, 0);         // arrives at node 3: m_lastReached = 3
        CHECK_EQ(b.LastReached(), 3u);
        Vector3 pos;
        float o = 0.0f;
        CHECK(b.ResetPosition(Free(), svc, pos, o));   // svc.Anchor answers false: no anchor
        CHECK(pos == n3.pos);
        CHECK_EQ(o, 1.25f);
    }
    // No orientation on the last reached node: the bearing from the previous node (wrapping to
    // the last node in the path when the last reached one is the first).
    {
        PatrolBehaviour b(p);
        b.Activate(Free(), svc);
        CHECK(b.SetNextWaypoint(1));
        FakeServices unwelded;
        b.Tick(Free(), unwelded, 1);            // SetNextWaypoint's 1 ms wait passes
        Sight finalized = Free();
        finalized.status.traveling = false;
        b.Tick(finalized, unwelded, 0);         // arrives at node 1: m_lastReached = 1
        CHECK_EQ(b.LastReached(), 1u);
        Vector3 pos;
        float o = 0.0f;
        CHECK(b.ResetPosition(Free(), svc, pos, o));
        CHECK(pos == n1.pos);
        float expected = std::atan2(n1.pos.y - n3.pos.y, n1.pos.x - n3.pos.x);
        expected = (expected >= 0.0f) ? expected : 6.28318530718f + expected;
        CHECK_EQ(o, expected);
    }
}

TEST(MotionBehaviour_PatrolPauseAndPauseTime)
{
    FakeServices svc;
    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 0.0f, 0.0f, 0.0f), MakeNode(2, 10.0f, 0.0f, 0.0f) };
    PatrolBehaviour b(p);
    b.Activate(Free(), svc);

    Step paused = b.Pause(1000);
    CHECK(paused.stop);
    CHECK(b.Tick(Free(), svc, 999).intent.act == MoveIntent::Act::Hold);   // 1 ms left

    b.AddToPauseTime(-5000);   // extend the pause (a negative diff, as the shell passes it)
    CHECK(b.Tick(Free(), svc, 999).intent.act == MoveIntent::Act::Hold);   // still held

    b.AddToPauseTime(10000);   // cut the remaining wait short; clamped at 0, never negative
    Step prepared = b.Tick(Free(), svc, 0);
    CHECK(prepared.intent.act == MoveIntent::Act::Move);   // free: a fresh leg toward the same node

    // A wait already running is left alone: Pause must not shorten it with a later, smaller one.
    Step first = b.Pause(50);
    CHECK(first.stop);
    Step second = b.Pause(9999);
    CHECK(second.stop);
    CHECK(b.Tick(Free(), svc, 40).intent.act == MoveIntent::Act::Hold);   // 50 - 40 = 10 ms left, not 9999
}

TEST(MotionBehaviour_PatrolArrivesMidSplineAndKeepsRoaming)
{
    FakeServices svc;
    svc.routeUsable = true;
    svc.routeRouted = true;   // every leg is a real route: the whole cyclic path welds into one spline

    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f), MakeNode(3, 30.0f, 0.0f, 0.0f) };

    PatrolBehaviour b(p);
    b.Activate(Free(), svc);

    Sight start = Free();
    start.position = Vector3(0.0f, 0.0f, 0.0f);
    Step first = b.Tick(start, svc, 0);
    CHECK(first.apply);
    CHECK(first.intent.act == MoveIntent::Act::Move);
    // All three nodes are plain: the weld covers the whole cyclic path (the full-lap guard
    // only refuses re-welding node 1 a second time) -- one spline of start + node 1 + node 2 +
    // node 3.
    CHECK_EQ(b.LegPointCount(), size_t(4));
    CHECK(first.intent.path != nullptr);   // Along's non-owning pointer at the behaviour's own, stable array

    // The spline is still running when it passes node 1's endpoint (index 1 of the 4 points).
    Sight midSpline = Free();
    midSpline.status.traveling = true;
    midSpline.status.pathIndex = 1;

    Step arriveNode1 = b.Tick(midSpline, svc, 100);
    CHECK(arriveNode1.again);
    CHECK(arriveNode1.roaming == Roaming::ClearMove);
    CHECK_EQ(arriveNode1.effects.size(), size_t(1));
    CHECK(arriveNode1.effects[0].kind == Effect::InformRaw);
    CHECK_EQ(arriveNode1.effects[0].id, 1u);
    CHECK_EQ(b.CurrentNode(), 1u);
    CHECK_EQ(b.LastReached(), 1u);

    // The arrivals phase drains (pathIndex hasn't reached node 2's endpoint yet: nothing more
    // queued) and re-states the same, still-running leg with ROAMING_MOVE re-added.
    Step drain = b.Tick(midSpline, svc, 0);
    CHECK(drain.apply);
    CHECK(drain.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(drain.intent.goal.x, 30.0f);   // the same leg goal: node 3
    CHECK(drain.roaming == Roaming::SetMove);
    CHECK(drain.effects.empty());
    CHECK(!drain.again);

    // A partial route: the leg is re-stated, not an arrival.
    Step partial = b.Tick(Partial(), svc, 0);
    CHECK(partial.apply);
    CHECK(partial.intent.act == MoveIntent::Act::Move);

    // Blocked after a partial approach is treated as arrived: a finalized path collects the
    // remaining segment nodes (2 and 3), then the trailing current-node arrival is swallowed
    // by the latch, then a fresh prepare follows.
    Sight blockedFinal = Free();
    blockedFinal.status.blocked = true;
    blockedFinal.status.pathIndex = 3;

    Step arriveNode2 = b.Tick(blockedFinal, svc, 0);
    CHECK(arriveNode2.again);
    CHECK_EQ(arriveNode2.effects.size(), size_t(1));
    CHECK(arriveNode2.effects[0].kind == Effect::InformRaw);
    CHECK_EQ(arriveNode2.effects[0].id, 2u);

    Step arriveNode3 = b.Tick(blockedFinal, svc, 0);
    CHECK(arriveNode3.again);
    CHECK_EQ(arriveNode3.effects.size(), size_t(1));
    CHECK(arriveNode3.effects[0].kind == Effect::InformRaw);
    CHECK_EQ(arriveNode3.effects[0].id, 3u);

    Step guardedTrailing = b.Tick(blockedFinal, svc, 0);
    CHECK(guardedTrailing.again);
    CHECK(guardedTrailing.effects.empty());

    Step afterFinal = b.Tick(blockedFinal, svc, 0);
    CHECK(afterFinal.apply);
    CHECK(afterFinal.intent.act == MoveIntent::Act::Move);

    // A cut Sight while a leg is held drops the segment and prepares a fresh leg at once,
    // rather than falling into the arrival-collecting path below it.
    {
        FakeServices svc2;
        svc2.routeUsable = true;
        svc2.routeRouted = true;
        PatrolBehaviour::Params p2;
        p2.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f) };
        PatrolBehaviour cutB(p2);
        cutB.Activate(Free(), svc2);
        Sight from0 = Free();
        from0.position = Vector3(0.0f, 0.0f, 0.0f);
        cutB.Tick(from0, svc2, 0);
        CHECK_EQ(cutB.LegPointCount(), size_t(3));   // start, node 1, node 2

        Sight cutSight = Free();
        cutSight.status.cut = true;
        cutSight.position = Vector3(5.0f, 0.0f, 0.0f);
        Step afterCut = cutB.Tick(cutSight, svc2, 0);
        CHECK(afterCut.apply);
        CHECK(afterCut.intent.act == MoveIntent::Act::Move);   // a fresh prepare, not a hold or an arrival
        CHECK_EQ(cutB.LegPointCount(), size_t(3));             // the old segment dropped, a clean weld rebuilt from the cut spot

        svc2.casting = true;
        Step held = cutB.Tick(Free(), svc2, 0);
        CHECK(held.intent.act == MoveIntent::Act::Hold);
        CHECK(held.effects.empty());
        svc2.casting = false;

        Sight noMove = Free();
        noMove.canMove = false;
        Step heldNoMove = cutB.Tick(noMove, svc2, 0);
        CHECK(heldNoMove.intent.act == MoveIntent::Act::Hold);
        CHECK(heldNoMove.roaming == Roaming::ClearMove);
    }
}

TEST(MotionBehaviour_PatrolSetNextWaypointInsideAnArrivalDropsTheQueuedArrivals)
{
    // A three-node weld. Node 3 waits, which is where the weld ends anyway (the full-lap guard
    // refuses to weld the path onto itself), and the wait is what keeps the patrol still after
    // the redirect: an arrival resets the move timer to its own node's delay, as the
    // generator's OnArrived did, overwriting SetNextWaypoint's 1 ms.
    PatrolBehaviour::Node n3 = MakeNode(3, 30.0f, 0.0f, 0.0f);
    n3.delay = 1000;

    Sight start = Free();
    start.position = Vector3(0.0f, 0.0f, 0.0f);
    Sight finalized = Free();
    finalized.status.traveling = false;
    finalized.status.pathIndex = 3;   // past every endpoint of the four-point leg

    // A hook's SetNextWaypoint, made from inside node 1's arrival: it clears the segment, and
    // the arrivals that segment still owed (nodes 2 and 3) go with it -- the generator's own
    // ProcessSegmentProgress loop ended the moment m_segment was cleared.
    {
        FakeServices svc;
        svc.routeUsable = true;
        svc.routeRouted = true;
        PatrolBehaviour::Params p;
        p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f), n3 };
        p.inform.waypoint = 411;
        PatrolBehaviour b(p);
        b.Activate(start, svc);
        b.Tick(start, svc, 100);
        CHECK_EQ(b.LegPointCount(), size_t(4));   // start, node 1, node 2, node 3

        Step atNode1 = b.Tick(finalized, svc, 100);   // nodes 1, 2 and 3 queue, plus the trailing entry
        CHECK(atNode1.again);
        CHECK_EQ(atNode1.effects.size(), size_t(1));
        CHECK(atNode1.effects[0].kind == Effect::InformRaw);
        CHECK_EQ(atNode1.effects[0].raw, 411u);
        CHECK_EQ(atNode1.effects[0].id, 1u);

        CHECK(b.SetNextWaypoint(3));

        // Not node 2: its queued arrival went with the segment. Only the trailing entry is left,
        // and with the latch cleared and node 3 current it runs the generator's own quirk -- an
        // arrival at the node the hook just told the patrol to walk to.
        Step trailing = b.Tick(finalized, svc, 0);
        CHECK(trailing.again);
        CHECK_EQ(trailing.effects.size(), size_t(1));
        CHECK(trailing.effects[0].kind == Effect::InformRaw);
        CHECK_EQ(trailing.effects[0].raw, 411u);
        CHECK_EQ(trailing.effects[0].id, 3u);
        CHECK_EQ(b.LastReached(), 3u);

        Step held = b.Tick(finalized, svc, 0);   // the segment cleared, node 3's wait running
        CHECK(held.apply);
        CHECK(held.intent.act == MoveIntent::Act::Hold);
        CHECK_EQ(b.CurrentNode(), 3u);
    }
    // The same drain under Pause, which leaves the latch alone: the trailing entry finds
    // m_isArrivalDone still set from node 1 and informs nothing at all.
    {
        FakeServices svc;
        svc.routeUsable = true;
        svc.routeRouted = true;
        PatrolBehaviour::Params p;
        p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f), n3 };
        p.inform.waypoint = 411;
        PatrolBehaviour b(p);
        b.Activate(start, svc);
        b.Tick(start, svc, 100);

        Step atNode1 = b.Tick(finalized, svc, 100);
        CHECK(atNode1.again);
        CHECK_EQ(atNode1.effects.size(), size_t(1));
        CHECK_EQ(atNode1.effects[0].id, 1u);

        Step paused = b.Pause(1000);
        CHECK(paused.stop);

        Step trailing = b.Tick(finalized, svc, 0);
        CHECK(trailing.again);
        CHECK(trailing.effects.empty());
        CHECK(trailing.roaming == Roaming::Keep);
        CHECK_EQ(b.LastReached(), 1u);

        // Node 1's own delay is zero, and the generator ran Stop(node.delay) as OnArrived's LAST
        // line -- after the inform hook that called Pause. So the pause is overwritten by that
        // zero and the drain prepares the next leg straight away, from node 2; the weld carries
        // it on through to node 3, where the path's only wait is.
        Step prepared = b.Tick(finalized, svc, 0);
        CHECK(prepared.apply);
        CHECK(prepared.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(prepared.intent.goal.x, 30.0f);
        CHECK_EQ(b.CurrentNode(), 2u);
    }
}

TEST(MotionBehaviour_PatrolNodeDelayOutlivesAHooksSetNextWaypoint)
{
    // The generator's Stop(node.delay) was OnArrived's last line: it ran after MovementInform, so
    // whatever the hook installed lost to the node's own delay. Node 2 waits 5 s and a hook fired
    // from its inform redirects the patrol to node 3 with SetNextWaypoint's 1 ms -- the 5 s wins.
    // (The delay sits on the weld's LAST node because that is the only place it can sit: a node
    //  that waits ends the weld, and a one-waypoint segment is dropped, so a delay on the first
    //  node would leave no tracked segment to arrive through at all.)
    FakeServices svc;
    svc.routeUsable = true;
    svc.routeRouted = true;

    PatrolBehaviour::Node n2 = MakeNode(2, 20.0f, 0.0f, 0.0f);
    n2.delay = 5000;
    PatrolBehaviour::Node n3 = MakeNode(3, 30.0f, 0.0f, 0.0f);
    n3.delay = 5000;   // and this one keeps the redirect's own leg unwelded, so its goal is node 3 itself

    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), n2, n3 };
    p.inform.waypoint = 411;
    PatrolBehaviour b(p);
    b.Activate(Free(), svc);

    Sight start = Free();
    start.position = Vector3(0.0f, 0.0f, 0.0f);
    b.Tick(start, svc, 0);
    CHECK_EQ(b.LegPointCount(), size_t(3));   // start, node 1, node 2: the weld ends at node 2's wait

    // The spline is still running as it passes both endpoints, so the arrivals come from the
    // segment alone -- no trailing entry follows them.
    Sight midSpline = Free();
    midSpline.status.traveling = true;
    midSpline.status.pathIndex = 2;

    Step atNode1 = b.Tick(midSpline, svc, 100);
    CHECK(atNode1.again);
    CHECK_EQ(atNode1.effects.size(), size_t(1));
    CHECK_EQ(atNode1.effects[0].id, 1u);

    Step atNode2 = b.Tick(midSpline, svc, 0);
    CHECK(atNode2.again);
    CHECK_EQ(atNode2.effects.size(), size_t(1));
    CHECK_EQ(atNode2.effects[0].id, 2u);

    CHECK(b.SetNextWaypoint(3));   // node 2's inform hook redirects the patrol, as it may

    Step held = b.Tick(midSpline, svc, 0);
    CHECK(held.apply);
    CHECK(held.intent.act == MoveIntent::Act::Hold);   // the leg is gone and node 2's 5 s is running

    CHECK(b.Tick(Free(), svc, 4999).intent.act == MoveIntent::Act::Hold);   // 1 ms left of the node's delay, not of the hook's

    Step prepared = b.Tick(Free(), svc, 1);
    CHECK(prepared.apply);
    CHECK(prepared.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(prepared.intent.goal.x, 30.0f);
    CHECK_EQ(b.CurrentNode(), 3u);
}

TEST(MotionBehaviour_PatrolAZeroDelayNodeDropsAHooksPause)
{
    // The same order, the other way round: node 1 does not wait at all, so the Stop(0) the
    // generator ran after the inform overwrote a Pause the hook had just installed.
    FakeServices svc;
    svc.routeUsable = true;
    svc.routeRouted = true;

    PatrolBehaviour::Node n2 = MakeNode(2, 20.0f, 0.0f, 0.0f);
    n2.delay = 5000;   // ends the weld at node 2, as above

    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), n2, MakeNode(3, 30.0f, 0.0f, 0.0f) };
    p.inform.waypoint = 411;
    PatrolBehaviour b(p);
    b.Activate(Free(), svc);

    Sight start = Free();
    start.position = Vector3(0.0f, 0.0f, 0.0f);
    b.Tick(start, svc, 0);
    CHECK_EQ(b.LegPointCount(), size_t(3));

    Sight midSpline = Free();
    midSpline.status.traveling = true;
    midSpline.status.pathIndex = 1;   // node 1's endpoint only: one segment arrival, no trailing entry

    Step atNode1 = b.Tick(midSpline, svc, 100);
    CHECK(atNode1.again);
    CHECK_EQ(atNode1.effects.size(), size_t(1));
    CHECK_EQ(atNode1.effects[0].id, 1u);

    Step paused = b.Pause(2000);   // the hook of node 1's inform holds the patrol
    CHECK(paused.stop);

    Step drained = b.Tick(midSpline, svc, 0);
    CHECK(drained.apply);
    CHECK(drained.intent.act == MoveIntent::Act::Hold);   // the pause dropped the leg; nothing to re-state

    Step prepared = b.Tick(Free(), svc, 100);             // 100 ms, not 2000: the node's zero delay won
    CHECK(prepared.apply);
    CHECK(prepared.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(prepared.intent.goal.x, 20.0f);
    CHECK_EQ(b.CurrentNode(), 2u);
}

TEST(MotionBehaviour_PatrolReplaceNodesTakesEffectAtTheNextPrepare)
{
    // A GM's waypoint edit: the shell hands the patrol a freshly read path, and the patrol keeps
    // walking -- its current node, its wait and the leg in flight are its own progress. The
    // generator read the manager's node map live, which is what this reproduces.
    FakeServices svc;   // routeUsable defaults false: a plain, unwelded leg each time

    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 10.0f, 0.0f, 0.0f), MakeNode(2, 20.0f, 0.0f, 0.0f), MakeNode(3, 30.0f, 0.0f, 0.0f) };
    p.inform.waypoint = 411;
    p.revision = 3;
    PatrolBehaviour b(p);
    b.Activate(Free(), svc);
    CHECK_EQ(b.Revision(), 3u);

    Step first = b.Tick(Free(), svc, 0);
    CHECK(first.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(first.intent.goal.x, 10.0f);
    CHECK_EQ(b.CurrentNode(), 1u);

    // Node 2 moved ten yards further out, node 3 deleted.
    std::vector<PatrolBehaviour::Node> edited;
    edited.push_back(MakeNode(1, 10.0f, 0.0f, 0.0f));
    edited.push_back(MakeNode(2, 30.0f, 0.0f, 0.0f));
    b.ReplaceNodes(edited, 4);
    CHECK_EQ(b.Revision(), 4u);
    CHECK_EQ(b.CurrentNode(), 1u);   // the walk in flight is untouched by the edit

    Sight finalized = Free();
    finalized.status.traveling = false;
    Step arrived = b.Tick(finalized, svc, 0);
    CHECK(arrived.again);
    CHECK_EQ(arrived.effects.size(), size_t(1));
    CHECK_EQ(arrived.effects[0].id, 1u);

    Step prepared = b.Tick(finalized, svc, 0);
    CHECK(prepared.apply);
    CHECK(prepared.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(prepared.intent.goal.x, 30.0f);   // the moved node 2, read at the prepare
    CHECK_EQ(b.CurrentNode(), 2u);

    // The whole path deleted under it: no nodes left, so the patrol holds where it stands.
    b.ReplaceNodes(std::vector<PatrolBehaviour::Node>(), 5);
    CHECK(!b.HasPath());
    Step empty = b.Tick(Free(), svc, 0);
    CHECK(empty.apply);
    CHECK(empty.intent.act == MoveIntent::Act::Hold);
    CHECK(empty.roaming == Roaming::ClearMove);
}

TEST(MotionBehaviour_PatrolPrepareRereadsCanMoveAfterTheNodesEffects)
{
    FakeServices svc;   // routeUsable defaults false: a plain, unwelded leg each time
    PatrolBehaviour::Node n1 = MakeNode(1, 10.0f, 0.0f, 0.0f);
    n1.spell = 4444;    // the arrival casts, and a cast may root or stun the caster
    PatrolBehaviour::Params p;
    p.nodes = { n1, MakeNode(2, 20.0f, 0.0f, 0.0f) };
    p.inform.waypoint = 411;
    PatrolBehaviour b(p);
    b.Activate(Free(), svc);
    b.Tick(Free(), svc, 0);                      // the leg toward node 1

    Sight finalized = Free();
    finalized.status.traveling = false;
    Step arrived = b.Tick(finalized, svc, 0);    // node 1's arrival: the spell is cast here
    CHECK(arrived.again);
    CHECK_EQ(arrived.effects.size(), size_t(2));
    CHECK(arrived.effects[0].kind == Effect::CastSpell);
    CHECK_EQ(arrived.effects[0].id, 4444u);

    // The cast rooted the unit. The Sight is the tick's one snapshot, taken before any of the
    // arrival's effects ran, so it still reads canMove -- only the live port knows better, and
    // the prepare is exactly where the generator re-read it.
    svc.canMove = false;
    Step prepared = b.Tick(finalized, svc, 0);
    CHECK(prepared.apply);
    CHECK(prepared.intent.act == MoveIntent::Act::Hold);
    CHECK(prepared.effects.empty());
    CHECK_EQ(b.LegPointCount(), size_t(0));
    CHECK_EQ(b.CurrentNode(), 1u);               // the node loop did not advance: no leg was laid
}

TEST(MotionBehaviour_PatrolLifecycleSteps)
{
    FakeServices svc;
    PatrolBehaviour::Params p;
    p.nodes = { MakeNode(1, 0.0f, 0.0f, 0.0f) };
    PatrolBehaviour b(p);

    Step activated = b.Activate(Free(), svc);
    CHECK(activated.roaming == Roaming::SetRoam);
    CHECK_EQ(activated.effects.size(), size_t(1));
    CHECK(activated.effects[0].kind == Effect::ClearWaypointPaused);
    CHECK(activated.resetLeg);

    Step resumedReset = b.Resume(Free(), svc, true);
    CHECK(resumedReset.roaming == Roaming::SetRoam);
    CHECK(resumedReset.effects.empty());
    CHECK(resumedReset.resetLeg);

    Step resumedNoReset = b.Resume(Free(), svc, false);
    CHECK(!resumedNoReset.apply);
    CHECK(resumedNoReset.roaming == Roaming::Keep);
    CHECK(!resumedNoReset.resetLeg);
    CHECK(!resumedNoReset.interrupt);

    Sight running = Free();
    running.runningState = true;
    b.Tick(running, svc, 0);   // records m_lastRunning for Suspend()'s walk restore

    Step suspended = b.Suspend();
    CHECK(suspended.interrupt);
    CHECK(suspended.roaming == Roaming::ClearBoth);
    CHECK(suspended.effects.size() == 1 && suspended.effects[0].kind == Effect::SetWalk && !suspended.effects[0].flag);
    CHECK(suspended.resetLeg);

    Outcome superseded = b.Finish(FinishReason::Superseded, running, svc);
    CHECK(superseded.interrupt);
    CHECK(superseded.roaming == Roaming::ClearBoth);
    CHECK(superseded.effects.size() == 1 && superseded.effects[0].kind == Effect::SetWalk && superseded.effects[0].flag == !running.runningState);

    Outcome cleared = b.Finish(FinishReason::Cleared, running, svc);
    CHECK(!cleared.interrupt);
    CHECK(cleared.roaming == Roaming::ClearBoth);
    CHECK(cleared.effects.size() == 1 && cleared.effects[0].kind == Effect::SetWalk && cleared.effects[0].flag == !running.runningState);
}

// ---- The tracking natives (P5-B family 3) -----------------------------------------

namespace
{
    /// A creature at the origin with a valid victim 10 yd ahead on +x: two 1.5 combat
    /// reaches (reachSum 3.0, so the client's melee range is max(3.0 + 4/3, 5) = 5.0) and
    /// two 0.5 bounding radii. The chase's band is therefore 3.5 yd out, 5.0 yd back.
    Sight Tracked()
    {
        Sight s = Free();
        s.isCreature = true;
        s.position = Vector3(0.0f, 0.0f, 0.0f);
        s.facing = 0.0f;
        s.extent = 0.5f;
        s.target.valid = true;
        s.target.position = Vector3(10.0f, 0.0f, 0.0f);
        s.target.facing = 0.0f;
        s.target.extent = 0.5f;
        s.target.reachSum = 3.0f;
        s.target.meleeRange = 5.0f;
        s.target.isVictim = true;
        return s;
    }

    /// The same, with a leg running toward the goal the driver reports.
    Sight TrackedTraveling(Vector3 const& legGoal)
    {
        Sight s = Tracked();
        s.status.traveling = true;
        s.status.legGoal = legGoal;
        return s;
    }

    bool Close(float a, float b, float eps = 0.001f)
    {
        return std::fabs(a - b) < eps;
    }
    bool Close(Vector3 const& a, Vector3 const& b, float eps = 0.001f)
    {
        return Close(a.x, b.x, eps) && Close(a.y, b.y, eps) && Close(a.z, b.z, eps);
    }

    /// The chase's opaque state masks: CHASE = 1, CHASE_MOVE = 2.
    ChaseBehaviour::ChaseParams Chasing(uint64 target = 42)
    {
        ChaseBehaviour::ChaseParams p;
        p.target = target;
        p.offset = 0.0f;
        p.angle = 0.0f;
        p.stateSet = 1;
        p.stateMove = 2;
        p.routineMs = 1000;
        return p;
    }

    /// The follow's: FOLLOW = 4, FOLLOW_MOVE = 8; the cadence and the horizon 400 ms.
    FollowBehaviour::FollowParams Following(uint64 target = 77)
    {
        FollowBehaviour::FollowParams p;
        p.target = target;
        p.offset = 0.0f;
        p.angle = 0.0f;
        p.stateSet = 4;
        p.stateMove = 8;
        p.routineMs = 400;
        p.horizonMs = 400;
        p.recalcRange = 1.5f;
        return p;
    }

    /// A leader is no one's victim; it faces 1.25 rad, so the bearing it is followed on and
    /// the facing copied at rest are both visible.
    Sight Leading()
    {
        Sight s = Tracked();
        s.target.isVictim = false;
        s.target.facing = 1.25f;
        return s;
    }
}

TEST(MotionBehaviour_ChaseDerivesRetailsBandAndFacesItsVictim)
{
    FakeServices svc;
    ChaseBehaviour b(Chasing());

    Step a = b.Activate(Tracked(), svc);
    CHECK(a.resetLeg);
    CHECK(!a.apply);                                  // the leg is laid by the tick, not the activation
    REQUIRE(a.effects.size() == size_t(2));
    CHECK(a.effects[0].kind == Effect::StateRaw);     // the bit first, as the generator's Initialize set it
    CHECK_EQ(a.effects[0].setMask, 1u);               // CHASE; never CHASE_MOVE, which follows a laid leg
    CHECK_EQ(a.effects[0].clearMask, 0u);
    CHECK(a.effects[1].kind == Effect::SetWalk);
    CHECK(!a.effects[1].flag);                        // a chase runs

    Step t = b.Tick(Tracked(), svc, 100);
    REQUIRE(svc.calls.size() == size_t(1));
    CHECK_STR(svc.calls[0], "spot");
    CHECK(Close(svc.spotCenter, Vector3(10.0f, 0.0f, 0.0f)));   // the live centre, not a placement
    CHECK(Close(svc.spotDistance, 3.5f));                       // offset + CONTACT_DISTANCE + reachSum
    CHECK(Close(svc.spotAngle, M_PI_F));                        // head-on: from the victim back to the mover
    CHECK(t.apply);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK(Close(t.intent.goal, Vector3(6.5f, 0.0f, 0.0f)));
    CHECK_EQ(t.intent.flags, uint32(MOVE_REQUIRE_PATH));
    CHECK(t.intent.facing.mode == Facing::Mode::Target);
    CHECK_EQ(t.intent.facing.target, uint64(42));
    REQUIRE(t.effects.size() == size_t(2));
    CHECK(t.effects[0].kind == Effect::StateRaw);
    CHECK_EQ(t.effects[0].setMask, 2u);                         // CHASE_MOVE, with the leg
    CHECK_EQ(t.effects[0].clearMask, 0u);
    CHECK(t.effects[1].kind == Effect::EngageInReach);          // the mover has not set off yet: still idle
    REQUIRE(b.Relays() != 0);
    CHECK_EQ(b.Relays()->first, 1u);
    CHECK_EQ(b.Relays()->Total(), 1u);
}

TEST(MotionBehaviour_ChaseRechecksOnceASecondAndCountsRecoveries)
{
    {
        // The routine cadence: nine 100 ms ticks never re-check at all, and the tenth
        // re-checks but finds the victim still inside the 5.0 yd re-approach edge.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;
        CHECK_EQ(b.Relays()->first, 1u);

        for (int i = 1; i <= 9; ++i)
        {
            Sight drifting = TrackedTraveling(goal);
            drifting.target.position = Vector3(10.0f + 0.1f * float(i), 0.0f, 0.0f);
            b.Tick(drifting, svc, 100);
        }
        CHECK_EQ(b.Relays()->Total(), 1u);

        Sight inside = TrackedTraveling(goal);
        inside.target.position = Vector3(11.0f, 0.0f, 0.0f);    // 4.5 yd from the goal
        b.Tick(inside, svc, 100);                               // the cadence fires here
        CHECK_EQ(b.Relays()->Total(), 1u);                      // and finds no drift

        Sight outside = TrackedTraveling(goal);
        outside.target.position = Vector3(16.0f, 0.0f, 0.0f);   // 9.5 yd: past the edge
        for (int i = 0; i < 10; ++i)
        {
            b.Tick(outside, svc, 100);
        }
        CHECK_EQ(b.Relays()->routine, 1u);                      // exactly one re-lay a second
        CHECK_EQ(b.Relays()->Total(), 2u);
    }
    {
        // A cut leg on a tick that may move: the recovery derives at once, counted apart.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;
        Sight cut = Tracked();
        cut.status.cut = true;
        cut.status.legGoal = goal;
        Step t = b.Tick(cut, svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(b.Relays()->cut, 1u);
        CHECK_EQ(b.Relays()->Total(), 2u);
    }
    {
        // The same edge arriving on a tick that holds: latched, spent on the next tick that moves.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;
        Sight held = Tracked();
        held.canMove = false;
        held.status.cut = true;
        held.status.legGoal = goal;
        CHECK(b.Tick(held, svc, 100).intent.act == MoveIntent::Act::Hold);
        CHECK_EQ(b.Relays()->Total(), 1u);                      // nothing derived under the hold
        Step t = b.Tick(TrackedTraveling(goal), svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(b.Relays()->cut, 1u);
    }
    {
        // A partial leg: it ended short of its goal, so go on from where it stopped.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;
        Sight partial = Tracked();
        partial.status.partial = true;
        partial.status.legGoal = goal;
        Step t = b.Tick(partial, svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(b.Relays()->partial, 1u);
    }
    {
        // A refused leg: the driver found no route under REQUIRE_PATH (or a partial one that
        // made no progress), laid nothing and never touched the leg goal. Without this cause
        // the native would stand until the cadence expired, where the generator's next 100 ms
        // poll re-derived at once.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;
        Sight refused = Tracked();
        refused.status.blocked = true;      // the once-only edge, as Blocked() carries it
        refused.status.legGoal = goal;
        Step t = b.Tick(refused, svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(b.Relays()->blocked, 1u);
        CHECK_EQ(b.Relays()->Total(), 2u);
        CHECK_EQ(svc.calls.size(), size_t(2));                  // a second spot, on the tick of the refusal
    }
    {
        // A tick carrying several edges counts one cause, most specific first.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;
        Sight both = Tracked();
        both.status.cut = true;
        both.status.blocked = true;
        both.status.legGoal = goal;
        b.Tick(both, svc, 100);
        CHECK_EQ(b.Relays()->cut, 1u);
        CHECK_EQ(b.Relays()->blocked, 0u);
    }
    {
        // A finished leg whose victim has moved on, inside the cadence: the counted recovery
        // the generator only caught on its next 100 ms poll.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;
        Sight finished = Tracked();
        finished.status.arrived = true;
        finished.status.legGoal = goal;
        finished.target.position = Vector3(16.0f, 0.0f, 0.0f);
        Step t = b.Tick(finished, svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(b.Relays()->finished, 1u);
        CHECK_EQ(b.Relays()->Total(), 2u);
    }
}

TEST(MotionBehaviour_ChaseHoldsForCastsAndStatesAndLosesItsVictim)
{
    FakeServices svc;
    ChaseBehaviour b(Chasing());
    b.Activate(Tracked(), svc);
    b.Tick(Tracked(), svc, 100);

    svc.casting = true;
    Sight traveling = Tracked();
    traveling.status.traveling = true;
    Step underCast = b.Tick(traveling, svc, 100);
    CHECK(underCast.intent.act == MoveIntent::Act::Hold);
    CHECK(underCast.stop);                              // StopMoving: the leg still ran
    CHECK(underCast.effects.empty());
    Step stillCasting = b.Tick(Tracked(), svc, 100);
    CHECK(stillCasting.intent.act == MoveIntent::Act::Hold);
    CHECK(stillCasting.stop);                           // unconditional: StopMoving also clears the _MOVE bits
    svc.casting = false;

    Sight rooted = Tracked();
    rooted.canMove = false;
    Step held = b.Tick(rooted, svc, 100);
    CHECK(held.intent.act == MoveIntent::Act::Hold);
    REQUIRE(held.effects.size() == size_t(1));
    CHECK(held.effects[0].kind == Effect::StateRaw);
    CHECK_EQ(held.effects[0].setMask, 0u);
    CHECK_EQ(held.effects[0].clearMask, 2u);            // CHASE_MOVE only: the chase itself stands

    Sight noCombatMovement = Tracked();
    noCombatMovement.combatMovementHeld = true;
    Step frozen = b.Tick(noCombatMovement, svc, 100);
    CHECK(frozen.intent.act == MoveIntent::Act::Hold);
    REQUIRE(frozen.effects.size() == size_t(1));
    CHECK_EQ(frozen.effects[0].clearMask, 2u);

    Sight notMyVictim = Tracked();
    notMyVictim.target.isVictim = false;
    Step lost = b.Tick(notMyVictim, svc, 100);
    CHECK(lost.intent.act == MoveIntent::Act::Hold);
    REQUIRE(lost.effects.size() == size_t(1));
    CHECK_EQ(lost.effects[0].clearMask, 2u);

    Sight dead = Tracked();
    dead.alive = false;
    Step corpse = b.Tick(dead, svc, 100);
    CHECK(corpse.intent.act == MoveIntent::Act::Hold);
    CHECK(corpse.effects.empty());

    Sight gone = Tracked();
    gone.target.valid = false;
    CHECK(b.Tick(gone, svc, 100).intent.act == MoveIntent::Act::Done);
    CHECK(b.EndReason(gone) == FinishReason::TargetLost);
    CHECK(b.TracksTarget());
    CHECK_EQ(b.Target(), uint64(42));
}

TEST(MotionBehaviour_ACastStopsAStandingChaserToo)
{
    // The generator's gate was `if (!owner.IsStopped()) owner.StopMoving();` and IsStopped()
    // reads the _MOVE unit states, not the spline: a chaser standing at its spot with CHASE_MOVE
    // still set was stopped too, and StopMoving clears UNIT_STAT_MOVING before it returns early
    // on a finalized spline (Unit::StopMoving). So the stop is unconditional here; the shell puts
    // nothing on the wire for a spline that has already run out.
    FakeServices svc;
    ChaseBehaviour b(Chasing());
    b.Activate(Tracked(), svc);
    b.Tick(Tracked(), svc, 100);

    svc.casting = true;
    Sight standing = Tracked();
    CHECK(!standing.status.traveling);                  // no leg is running
    Step underCast = b.Tick(standing, svc, 100);
    CHECK(underCast.intent.act == MoveIntent::Act::Hold);
    CHECK(underCast.stop);                              // the move bit still has to go
    CHECK(underCast.effects.empty());
}

TEST(MotionBehaviour_ChaseEngagesOnEveryIdleTick)
{
    {
        // A chase begun already inside contact: the very first tick derives a spot AND
        // engages, as the generator's ReachTarget did on any tick that was not traveling.
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        Sight inContact = Tracked();
        inContact.target.position = Vector3(3.0f, 0.0f, 0.0f);
        b.Activate(inContact, svc);
        Step t = b.Tick(inContact, svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Move);
        REQUIRE(t.effects.size() == size_t(2));
        CHECK(t.effects[0].kind == Effect::StateRaw);           // the _MOVE bit, with the leg
        CHECK_EQ(t.effects[0].setMask, 2u);
        CHECK(t.effects[1].kind == Effect::EngageInReach);      // and the attack, decided live
        CHECK_EQ(b.Relays()->first, 1u);
    }

    FakeServices svc;
    ChaseBehaviour b(Chasing());
    b.Activate(Tracked(), svc);
    const Vector3 goal = b.Tick(Tracked(), svc, 100).intent.goal;

    Sight arrived = Tracked();
    arrived.status.arrived = true;
    arrived.status.legGoal = goal;
    Step reached = b.Tick(arrived, svc, 100);
    CHECK(reached.intent.act == MoveIntent::Act::Hold);
    CHECK(reached.intent.facing.mode == Facing::Mode::Target);
    CHECK_EQ(reached.intent.facing.target, uint64(42));
    REQUIRE(reached.effects.size() == size_t(1));
    CHECK(reached.effects[0].kind == Effect::EngageInReach);

    Sight idle = Tracked();
    idle.status.legGoal = goal;
    Step again = b.Tick(idle, svc, 100);
    CHECK(again.intent.act == MoveIntent::Act::Hold);
    CHECK(again.intent.facing.mode == Facing::Mode::Target);
    REQUIRE(again.effects.size() == size_t(1));
    CHECK(again.effects[0].kind == Effect::EngageInReach);   // re-emitted: a stale miss never latches
    CHECK_EQ(svc.calls.size(), size_t(1));                   // and no fresh spot was derived
    CHECK_EQ(b.Relays()->Total(), 1u);
}

TEST(MotionBehaviour_ChaseLeadIsAnExperiment)
{
    Sight running = Tracked();
    running.target.velocity = Vector3(7.0f, 0.0f, 0.0f);
    running.target.velocityTrusted = true;
    {
        FakeServices svc;
        ChaseBehaviour b(Chasing());                        // the lead is off by default
        b.Activate(running, svc);
        b.Tick(running, svc, 100);
        CHECK(Close(svc.spotCenter, Vector3(10.0f, 0.0f, 0.0f)));   // retail's aim: the live position
    }
    {
        FakeServices svc;
        ChaseBehaviour::ChaseParams p = Chasing();
        p.lead = true;
        p.leadMs = 500;
        ChaseBehaviour b(p);
        b.Activate(running, svc);
        b.Tick(running, svc, 100);
        CHECK(Close(svc.spotCenter, Vector3(13.5f, 0.0f, 0.0f)));   // 7 yd/s for half a second
    }
    {
        FakeServices svc;
        ChaseBehaviour::ChaseParams p = Chasing();
        p.lead = true;
        p.leadMs = 500;
        ChaseBehaviour b(p);
        Sight untrusted = running;
        untrusted.target.velocityTrusted = false;
        b.Activate(untrusted, svc);
        b.Tick(untrusted, svc, 100);
        CHECK(Close(svc.spotCenter, Vector3(10.0f, 0.0f, 0.0f)));   // no lead off an untrusted velocity
    }
}

TEST(MotionBehaviour_FollowAimsOneCadenceAheadAndCopiesTheLeadersFacingAtRest)
{
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        Step a = b.Activate(Leading(), svc);
        CHECK(a.resetLeg);
        CHECK(!a.apply);
        REQUIRE(a.effects.size() == size_t(2));
        CHECK(a.effects[0].kind == Effect::StateRaw);   // the bit precedes the sync that reads it
        CHECK_EQ(a.effects[0].setMask, 4u);
        CHECK_EQ(a.effects[0].clearMask, 0u);
        CHECK(a.effects[1].kind == Effect::SyncSpeed);

        Sight running = Leading();
        running.target.velocity = Vector3(7.0f, 0.0f, 0.0f);
        running.target.velocityTrusted = true;
        Step leg = b.Tick(running, svc, 100);
        CHECK(Close(svc.spotCenter, Vector3(12.8f, 0.0f, 0.0f)));   // one 400 ms cadence ahead
        CHECK(Close(svc.spotDistance, 1.0f));                       // offset + own extent + the leader's
        CHECK(Close(svc.spotAngle, 1.25f));                         // the leader's facing + the angle
        CHECK(leg.intent.act == MoveIntent::Act::Move);
        CHECK(leg.intent.facing.mode == Facing::Mode::None);        // never baked into a moving leg
        REQUIRE(leg.effects.size() == size_t(1));
        CHECK_EQ(leg.effects[0].setMask, 8u);
    }
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        Sight untrusted = Leading();
        untrusted.target.velocity = Vector3(7.0f, 0.0f, 0.0f);      // a smooth, cyclic or airborne spline
        b.Activate(untrusted, svc);
        b.Tick(untrusted, svc, 100);
        CHECK(Close(svc.spotCenter, Vector3(10.0f, 0.0f, 0.0f)));   // the horizon is zero unless trusted
    }
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        b.Activate(Leading(), svc);
        const Vector3 goal = b.Tick(Leading(), svc, 100).intent.goal;
        Sight idle = Leading();
        idle.status.legGoal = goal;
        Step held = b.Tick(idle, svc, 100);
        CHECK(held.intent.act == MoveIntent::Act::Hold);
        CHECK(held.intent.facing.mode == Facing::Mode::Angle);      // the leader's facing, at rest only
        CHECK(Close(held.intent.facing.angle, 1.25f));
        CHECK(held.effects.empty());                                // a follower never engages
        CHECK_EQ(svc.calls.size(), size_t(1));
    }
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        b.Activate(Leading(), svc);
        Step suspended = b.Suspend();
        CHECK(suspended.interrupt);
        CHECK(suspended.resetLeg);
        REQUIRE(suspended.effects.size() == size_t(2));
        CHECK(suspended.effects[0].kind == Effect::StateRaw);
        CHECK_EQ(suspended.effects[0].setMask, 0u);
        CHECK_EQ(suspended.effects[0].clearMask, 12u);              // FOLLOW | FOLLOW_MOVE
        CHECK(suspended.effects[1].kind == Effect::SyncSpeed);

        Outcome superseded = b.Finish(FinishReason::Superseded, Leading(), svc);
        CHECK(superseded.interrupt);
        REQUIRE(superseded.effects.size() == size_t(2));
        CHECK(superseded.effects[0].kind == Effect::StateRaw);
        CHECK_EQ(superseded.effects[0].clearMask, 12u);
        CHECK(superseded.effects[1].kind == Effect::SyncSpeed);

        Outcome cleared = b.Finish(FinishReason::Cleared, Leading(), svc);
        CHECK(!cleared.interrupt);                                  // Finalize never stopped the mover
        REQUIRE(cleared.effects.size() == size_t(2));
        CHECK_EQ(cleared.effects[0].clearMask, 12u);
        CHECK(cleared.effects[1].kind == Effect::SyncSpeed);

        Sight gone = Leading();
        gone.target.valid = false;
        CHECK(b.Tick(gone, svc, 100).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(gone) == FinishReason::TargetLost);
    }
}

TEST(MotionBehaviour_FollowSetsItsBitBeforeItSyncsSpeed)
{
    // Load-bearing order, not cosmetics: Unit::UpdateSpeed's pet branch copies the owner's
    // rate only while UNIT_STAT_FOLLOW is set (UnitSpeed.cpp), and the deleted
    // FollowMovementGenerator::Initialize did addUnitState(UNIT_STAT_FOLLOW) first and
    // SyncSpeedWithMaster second. A sync performed ahead of the bit reads the pet's own rate,
    // so the pet would trail its mounted master until some later UpdateSpeed happened to run.
    FakeServices svc;
    FollowBehaviour b(Following());
    Step a = b.Activate(Leading(), svc);
    REQUIRE(a.effects.size() == size_t(2));
    CHECK(a.effects[0].kind == Effect::StateRaw);
    CHECK_EQ(a.effects[0].setMask, 4u);                 // FOLLOW
    CHECK(a.effects[1].kind == Effect::SyncSpeed);      // reads the bit the line above just set
}

TEST(MotionBehaviour_FollowWalksWithItsLeaderAndAPetForcesTheDestination)
{
    Sight walking = Leading();
    walking.target.walking = true;
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        b.Activate(walking, svc);
        Step t = b.Tick(walking, svc, 100);
        CHECK(t.intent.Has(MOVE_WALK));                     // a creature mirrors its leader's gait
        CHECK(t.intent.Has(MOVE_REQUIRE_PATH));
        CHECK(!t.intent.Has(MOVE_FORCE_DEST));
    }
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        Sight pet = walking;
        pet.isPet = true;
        b.Activate(pet, svc);
        Step t = b.Tick(pet, svc, 100);
        CHECK(t.intent.Has(MOVE_FORCE_DEST));               // the navmesh shortcut a pet is allowed
        CHECK(t.intent.Has(MOVE_WALK));
    }
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        Sight player = walking;
        player.isCreature = false;
        b.Activate(player, svc);
        Step t = b.Tick(player, svc, 100);
        CHECK(!t.intent.Has(MOVE_WALK));                    // a player follower never walks
        CHECK(t.intent.Has(MOVE_REQUIRE_PATH));
    }
    {
        FakeServices svc;
        FollowBehaviour b(Following());
        Sight standing = Leading();                         // the leader stands: no walk mirror
        b.Activate(standing, svc);
        Step t = b.Tick(standing, svc, 100);
        CHECK(!t.intent.Has(MOVE_WALK));
    }
}

TEST(MotionBehaviour_ChaseAtAnAngleAimsOffItsVictimsFacingAndBakesNoFacing)
{
    FakeServices svc;
    ChaseBehaviour::ChaseParams p = Chasing();
    p.angle = 0.5f;                                 // a flanking chase: a tank's add, a pet on a side
    ChaseBehaviour b(p);
    Sight s = Tracked();
    s.target.facing = 1.0f;
    b.Activate(s, svc);
    Step t = b.Tick(s, svc, 100);
    CHECK(Close(svc.spotAngle, 1.5f));              // the victim's facing + the angle, not the head-on bearing
    CHECK(Close(svc.spotDistance, 3.5f));           // the band is the same
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK(t.intent.facing.mode == Facing::Mode::None);   // only a head-on chase turns to its victim
}

TEST(MotionBehaviour_TrackingDriftMeasuresHeightForFliersAndSwimmersOnly)
{
    // 4 yd from the leg's goal on the ground and 4 yd above it: 4.0 in two dimensions, well
    // inside the 5.0 yd re-approach edge, but 5.66 in three, well past it.
    Sight aloft = Tracked();
    aloft.status.traveling = true;
    aloft.status.legGoal = Vector3(6.0f, 0.0f, 0.0f);
    aloft.target.position = Vector3(10.0f, 0.0f, 4.0f);
    {
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        b.Tick(Tracked(), svc, 100);
        for (int i = 0; i < 10; ++i)
        {
            b.Tick(aloft, svc, 100);                // a full cadence
        }
        CHECK_EQ(b.Relays()->Total(), 1u);          // a walker never looks up
    }
    {
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        b.Tick(Tracked(), svc, 100);
        Sight flier = aloft;
        flier.canFlyHint = true;
        for (int i = 0; i < 10; ++i)
        {
            b.Tick(flier, svc, 100);
        }
        CHECK_EQ(b.Relays()->routine, 1u);
    }
    {
        FakeServices svc;
        ChaseBehaviour b(Chasing());
        b.Activate(Tracked(), svc);
        b.Tick(Tracked(), svc, 100);
        Sight swimmer = aloft;
        swimmer.swimming = true;                    // the water column, the same rule
        for (int i = 0; i < 10; ++i)
        {
            b.Tick(swimmer, svc, 100);
        }
        CHECK_EQ(b.Relays()->routine, 1u);
    }
}

TEST(MotionBehaviour_FollowRechecksOnItsOwnCadenceAndTolerance)
{
    // The generator's tolerance with the config honoured: 1.5 - 0.5 + 1.0 x (0.5 + 0.5),
    // plus the leader's 0.5 radius again, is 2.5 yd; the cadence is 400 ms, not the chase's
    // second.
    FakeServices svc;
    FollowBehaviour b(Following());
    b.Activate(Leading(), svc);
    b.Tick(Leading(), svc, 100);
    CHECK_EQ(b.Relays()->first, 1u);

    Sight inside = Leading();
    inside.status.traveling = true;
    inside.status.legGoal = Vector3(12.4f, 0.0f, 0.0f);     // the leader is 2.4 yd from it
    for (int i = 0; i < 3; ++i)
    {
        b.Tick(inside, svc, 100);
    }
    CHECK_EQ(b.Relays()->Total(), 1u);                      // 300 ms: inside the cadence
    b.Tick(inside, svc, 100);                               // 400 ms: it fires
    CHECK_EQ(b.Relays()->Total(), 1u);                      // and finds the spot still good

    Sight outside = inside;
    outside.status.legGoal = Vector3(12.6f, 0.0f, 0.0f);    // 2.6 yd: past the tolerance
    for (int i = 0; i < 4; ++i)
    {
        b.Tick(outside, svc, 100);
    }
    CHECK_EQ(b.Relays()->routine, 1u);
    CHECK_EQ(b.Relays()->Total(), 2u);
}

TEST(MotionBehaviour_TrackingFallsBackToTheCentreWhenNoSpotIsFree)
{
    FakeServices svc;
    svc.spotFails = true;                           // the selector found nothing free
    ChaseBehaviour b(Chasing());
    b.Activate(Tracked(), svc);
    Step t = b.Tick(Tracked(), svc, 100);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK(Close(t.intent.goal, Vector3(10.0f, 0.0f, 0.0f)));   // the centre itself, as NearPoint fell back
    CHECK_EQ(b.Relays()->first, 1u);                           // the derive happened either way
}

TEST(MotionBehaviour_TrackingResumesOnlyOnAReset)
{
    FakeServices svc;
    FollowBehaviour b(Following());
    b.Activate(Leading(), svc);
    b.Tick(Leading(), svc, 100);
    CHECK_EQ(b.Relays()->first, 1u);

    Step plain = b.Resume(Leading(), svc, false);
    CHECK(!plain.apply);
    CHECK(!plain.resetLeg);
    CHECK(!plain.interrupt);
    CHECK(plain.effects.empty());

    Step reset = b.Resume(Leading(), svc, true);    // the generator's Reset was its Initialize
    CHECK(reset.resetLeg);
    CHECK(!reset.apply);
    REQUIRE(reset.effects.size() == size_t(2));
    CHECK(reset.effects[0].kind == Effect::StateRaw);   // Resume(reset) is Activate: the same order
    CHECK_EQ(reset.effects[0].setMask, 4u);
    CHECK(reset.effects[1].kind == Effect::SyncSpeed);
    Step t = b.Tick(Leading(), svc, 100);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(b.Relays()->first, 2u);                // the reset forgot the spot: a first one again
}

namespace
{
    /// The evade return's params: a distinct world point and heading so the leg and the
    /// facing are both checkable, and an opaque dynamic-state clear mask.
    HomeBehaviour::Params Homing()
    {
        HomeBehaviour::Params p;
        p.home = Vector3(12.0f, -4.0f, 2.0f);
        p.facing = 1.5f;
        p.stateClear = 0x30u;
        return p;
    }
}

TEST(MotionBehaviour_HomeClearsOnItsFirstTickAndForcesItsEndpoint)
{
    FakeServices svc;
    HomeBehaviour b(Homing());

    Step a = b.Activate(Free(), svc);
    CHECK(a.resetLeg);              // the clear waits for the first tick
    CHECK(a.effects.empty());
    CHECK(!a.apply);

    Step t1 = b.Tick(Free(), svc, 100);
    REQUIRE(t1.effects.size() == size_t(1));
    CHECK(t1.effects[0].kind == Effect::StateRaw);
    CHECK_EQ(t1.effects[0].setMask, 0u);
    CHECK_EQ(t1.effects[0].clearMask, 0x30u);
    CHECK(t1.apply);
    CHECK(t1.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t1.intent.goal.x, 12.0f);
    CHECK_EQ(t1.intent.goal.y, -4.0f);
    CHECK_EQ(t1.intent.goal.z, 2.0f);
    CHECK_EQ(t1.intent.flags, uint32(MOVE_FORCE_DEST));
    CHECK(t1.intent.facing.mode == Facing::Mode::Angle);
    CHECK_EQ(t1.intent.facing.angle, 1.5f);

    Step t2 = b.Tick(Free(), svc, 100);
    CHECK(t2.effects.empty());      // the clear happened once, on the first tick only
    CHECK(t2.apply);
    CHECK(t2.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t2.intent.flags, uint32(MOVE_FORCE_DEST));

    Step t3 = b.Tick(Cut(), svc, 100);   // a stop on the way is not an arrival: the leg is re-stated
    CHECK(t3.effects.empty());
    CHECK(t3.apply);
    CHECK(t3.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t3.intent.goal.x, 12.0f);
    CHECK_EQ(t3.intent.flags, uint32(MOVE_FORCE_DEST));
}

TEST(MotionBehaviour_HomeEndsOnArrivalOrBlockAndRestoresOnlyThen)
{
    FakeServices svc;
    {
        HomeBehaviour b(Homing());
        b.Activate(Free(), svc);
        b.Tick(Free(), svc, 100);
        Step t = b.Tick(Arrived(), svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Arrived);

        Outcome o = b.Finish(FinishReason::Arrived, Free(), svc);
        REQUIRE(o.effects.size() == size_t(4));
        CHECK(o.effects[0].kind == Effect::RestoreTemporaryFaction);
        CHECK(o.effects[1].kind == Effect::SetWalk);
        CHECK(o.effects[1].flag);                    // not running, not levitating
        CHECK(o.effects[2].kind == Effect::LoadAddon);
        CHECK(o.effects[3].kind == Effect::JustReachedHome);
        CHECK(!o.interrupt);                          // the generator's Interrupt was a no-op
    }
    {
        // A creature that could not be sent home at all still counts as home: evade must
        // always terminate.
        HomeBehaviour b(Homing());
        b.Activate(Free(), svc);
        Step t = b.Tick(Blocked(), svc, 100);
        CHECK(t.intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Arrived);
        CHECK(!b.Finish(FinishReason::Arrived, Free(), svc).effects.empty());   // it did arrive, home
    }
    {
        // Displaced after an arrival: the recipe never runs twice and Finish never interrupts.
        HomeBehaviour b(Homing());
        b.Activate(Free(), svc);
        b.Tick(Free(), svc, 100);
        b.Tick(Arrived(), svc, 100);
        Outcome o = b.Finish(FinishReason::Superseded, Free(), svc);
        CHECK(o.effects.empty());
        CHECK(!o.interrupt);
    }
    {
        // Finished before any arrival at all: no recipe.
        HomeBehaviour b(Homing());
        b.Activate(Free(), svc);
        b.Tick(Free(), svc, 100);
        Outcome o = b.Finish(FinishReason::Arrived, Free(), svc);
        CHECK(o.effects.empty());
    }
}

TEST(MotionBehaviour_ModelGrowsForTheControlMoves)
{
    // The per-effect owner rule: the state mirror is every owner's, the rest a creature's.
    CHECK_EQ(Effect::Owners(Effect::StateRaw), uint8(Effect::OwnerAny));
    CHECK_EQ(Effect::Owners(Effect::SetWalk), uint8(Effect::OwnerCreature));
    CHECK_EQ(Effect::Owners(Effect::ClearTarget), uint8(Effect::OwnerCreature));
    CHECK_EQ(Effect::Owners(Effect::ClearFleeingFlag), uint8(Effect::OwnerCreature));
    CHECK_EQ(Effect::Owners(Effect::RestoreGait), uint8(Effect::OwnerCreature));
    CHECK_EQ(Effect::Owners(Effect::AttackVictim), uint8(Effect::OwnerCreature));
    Outcome o;
    CHECK(!o.stop && !o.stopForced);
    Sight s;
    CHECK(!s.notMove);
    // Only the charge asks for the contact point; a plain point and the idle do not.
    PointBehaviour::Params charge = PointTo(1.0f, 2.0f, 3.0f);
    charge.target = 42;
    CHECK(PointBehaviour(charge).NeedsContactPoint());
    CHECK(!PointBehaviour(PointTo(1.0f, 2.0f, 3.0f)).NeedsContactPoint());
    CHECK_EQ(PointBehaviour(charge).Variant(), 0u);
    CHECK(!IdleBehaviour().NeedsContactPoint());
}

TEST(MotionBehaviour_ModelGrowsForTheTaxi)
{
    // The six taxi operations are a player's alone (no creature ever holds a taxi); the state
    // mirror stays every owner's.
    CHECK_EQ(Effect::Owners(Effect::TaxiTakeoff), uint8(Effect::OwnerPlayer));
    CHECK_EQ(Effect::Owners(Effect::TaxiEvent), uint8(Effect::OwnerPlayer));
    CHECK_EQ(Effect::Owners(Effect::TaxiSeam), uint8(Effect::OwnerPlayer));
    CHECK_EQ(Effect::Owners(Effect::TaxiCross), uint8(Effect::OwnerPlayer));
    CHECK_EQ(Effect::Owners(Effect::TaxiLand), uint8(Effect::OwnerPlayer));
    CHECK_EQ(Effect::Owners(Effect::TaxiAbort), uint8(Effect::OwnerPlayer));
    CHECK_EQ(Effect::Owners(Effect::Inform), uint8(Effect::OwnerCreature));
    // MOVE_SMOOTH is a bit of its own beside the five flags the driver already reads.
    CHECK_EQ(uint32(MOVE_SMOOTH), 0x20u);
    CHECK_EQ(uint32(MOVE_SMOOTH) & uint32(MOVE_WALK | MOVE_FLY | MOVE_STRAIGHT | MOVE_FORCE_DEST | MOVE_REQUIRE_PATH), 0u);
    MoveIntent m = MoveIntent::Move(Vector3(1.0f, 2.0f, 3.0f), MOVE_FLY | MOVE_SMOOTH);
    CHECK(m.Has(MOVE_SMOOTH));
    CHECK(m.Has(MOVE_FLY));
    // The factories set only their fields.
    Effect k = Effect::Takeoff(77);
    CHECK(k.kind == Effect::TaxiTakeoff);
    CHECK_EQ(k.id, 77u);
    Effect d = Effect::NodeEvent(5, true);
    CHECK(d.kind == Effect::TaxiEvent);
    CHECK_EQ(d.id, 5u);
    CHECK(d.flag);
    Effect a = Effect::NodeEvent(6, false);
    CHECK(!a.flag);
    Effect s = Effect::Seam();
    CHECK(s.kind == Effect::TaxiSeam);
    Effect c = Effect::Cross(571, Vector3(1.0f, 2.0f, 3.0f), 0.5f);
    CHECK(c.kind == Effect::TaxiCross);
    CHECK_EQ(c.raw, 571u);
    CHECK_EQ(c.point.y, 2.0f);
    CHECK_EQ(c.angle, 0.5f);
    Effect l = Effect::Land(true, Vector3(4.0f, 5.0f, 6.0f), 0.25f);
    CHECK(l.kind == Effect::TaxiLand);
    CHECK(l.flag);
    CHECK_EQ(l.point.z, 6.0f);
    CHECK_EQ(l.angle, 0.25f);
    Effect ab = Effect::Abort(FinishReason::Died);
    CHECK(ab.kind == Effect::TaxiAbort);
    CHECK(ab.reason == FinishReason::Died);
    Effect plain(Effect::Inform);
    CHECK(plain.reason == FinishReason::Arrived);
    CHECK_EQ(plain.angle, 0.0f);
}

namespace
{
    /// The flee's opaque move bit: 0x40000 (the shell's UNIT_STAT_FLEEING_MOVE); the confuse's 0x400.
    FearBehaviour::Params Feared(uint32 timeLimitMs = 0)
    {
        FearBehaviour::Params p;
        p.fright = 77;
        p.timeLimitMs = timeLimitMs;
        p.stateFleeingMove = 0x40000;
        return p;
    }
    ConfusedBehaviour::Params Confusing(float radius = 10.0f)
    {
        ConfusedBehaviour::Params p;
        p.stateConfusedMove = 0x400;
        p.radius = radius;
        return p;
    }
    /// A creature standing at (x, y), free to move.
    Sight Standing(float x, float y)
    {
        Sight s = Free();
        s.position = Vector3(x, y, 0.0f);
        s.isCreature = true;
        return s;
    }
    Sight Player(float x, float y)
    {
        Sight s = Standing(x, y);
        s.isCreature = false;
        return s;
    }
    /// The fright 6 yd east of the mover at the origin.
    void FrightEast(FakeServices& svc)
    {
        svc.frightResolved = true;
        svc.frightPosition = Vector3(6.0f, 0.0f, 0.0f);
        svc.frightDistance = 6.0f;
    }
    bool CallsAre(FakeServices const& svc, std::vector<std::string> const& expected)
    {
        if (svc.calls.size() != expected.size())
        {
            return false;
        }
        for (size_t i = 0; i < expected.size(); ++i)
        {
            if (svc.calls[i] != expected[i])
            {
                return false;
            }
        }
        return true;
    }
}

TEST(MotionBehaviour_FearHooksAreTheGeneratorsInitializeInterruptReset)
{
    FakeServices svc;
    FearBehaviour f(Feared());
    Sight s = Standing(0.0f, 0.0f);
    Step a = f.Activate(s, svc);
    CHECK(a.stop);                                   // the generator's add-then-StopMoving nets to a cleared bit: no StateRaw here
    CHECK(!a.interrupt);
    CHECK(a.resetLeg);
    CHECK(!a.apply);
    REQUIRE(a.effects.size() == size_t(2));
    CHECK(a.effects[0].kind == Effect::SetWalk);
    CHECK(!a.effects[0].flag);                       // a flee runs
    CHECK(a.effects[1].kind == Effect::ClearTarget);
    CHECK(svc.calls.empty());                        // no draw before the first tick
    CHECK_EQ(f.Target(), uint64(77));
    CHECK(!f.TracksTarget());                        // the fright is the port's, at the pick
    CHECK(!f.NeedsContactPoint());
    CHECK_EQ(f.Variant(), 0u);
    CHECK_EQ(FearBehaviour(Feared(3000)).Variant(), 1u);
    CHECK(f.EndReason(s) == FinishReason::Expired);
    Step su = f.Suspend();
    CHECK(su.interrupt && su.resetLeg && !su.stop && !su.apply);
    REQUIRE(su.effects.size() == size_t(1));
    CHECK(su.effects[0].kind == Effect::StateRaw);
    CHECK_EQ(su.effects[0].setMask, 0u);
    CHECK_EQ(su.effects[0].clearMask, 0x40000u);
    Step r = f.Resume(s, svc, false);
    CHECK(!r.stop && !r.resetLeg && r.effects.empty());   // a resume without a reset does nothing
    Step rr = f.Resume(s, svc, true);
    CHECK(rr.stop && rr.resetLeg);                   // a reset is the Initialize again
    CHECK_EQ(rr.effects.size(), size_t(2));
    // Suspend()'s forgetting: after a laid leg, a suspend and a resume WITHOUT a reset must not
    // re-state the stale leg. The tick falls to the rest countdown instead; with the rest still
    // running it returns Hold and draws nothing.
    FrightEast(svc);
    f.Activate(s, svc);
    f.Tick(s, svc, 100);                             // a bolt, rest 800
    f.Suspend();
    f.Resume(s, svc, false);
    svc.calls.clear();
    Sight moving = Standing(0.0f, 0.0f);
    moving.status.traveling = true;
    CHECK(f.Tick(moving, svc, 100).intent.act == MoveIntent::Act::Hold);
    CHECK(svc.calls.empty());
}

TEST(MotionBehaviour_FearPickDrawsInTheGeneratorsOrder)
{
    FakeServices svc;
    FrightEast(svc);
    FearBehaviour f(Feared());
    Sight s = Standing(0.0f, 0.0f);
    f.Activate(s, svc);
    Step t = f.Tick(s, svc, 100);                    // rest 0: the first pick at once
    CHECK(CallsAre(svc, { "frand", "fright", "frand", "frand", "groundPoint", "urand" }));
    CHECK(t.apply);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t.intent.flags, uint32(MOVE_REQUIRE_PATH));
    CHECK(Close(t.intent.pathLengthLimit, 30.0f));
    REQUIRE(t.effects.size() == size_t(1));
    CHECK(t.effects[0].kind == Effect::StateRaw);
    CHECK_EQ(t.effects[0].setMask, 0x40000u);
    CHECK_EQ(t.effects[0].clearMask, 0u);
    // The fake's Frand answers its minimum. Inside 28 yd: dist = 0.4 * (28 - 6) = 8.8 at the
    // bearing from the fright to the mover (pi) plus the minimum jitter (-pi/8) = 7pi/8:
    // west and a little north; the guess sits half a yard up.
    CHECK(Close(svc.lastGuess.x, 8.8f * std::cos(7.0f * M_PI_F / 8.0f), 0.01f));
    CHECK(Close(svc.lastGuess.y, 8.8f * std::sin(7.0f * M_PI_F / 8.0f), 0.01f));
    CHECK(Close(svc.lastGuess.z, 0.5f));
    CHECK(Close(t.intent.goal, svc.lastGuess));      // the fake hands the guess back whole
    // Traveling: the leg re-stated, no draw, no length limit on the re-statement, no effect.
    svc.calls.clear();
    Sight moving = Standing(0.0f, 0.0f);
    moving.status.traveling = true;
    Step m = f.Tick(moving, svc, 100);
    CHECK(m.intent.act == MoveIntent::Act::Move);
    CHECK(Close(m.intent.goal, t.intent.goal));
    CHECK(Close(m.intent.pathLengthLimit, 0.0f));
    CHECK(m.effects.empty());
    CHECK(svc.calls.empty());
    // Standing again: the rest (urand's minimum, 800) counts only now; 700 in, still holding.
    CHECK(f.Tick(s, svc, 700).intent.act == MoveIntent::Act::Hold);
    CHECK(svc.calls.empty());
    Step n = f.Tick(s, svc, 100);                    // 800: the next bolt
    CHECK(n.intent.act == MoveIntent::Act::Move);
    CHECK(CallsAre(svc, { "frand", "fright", "frand", "frand", "groundPoint", "urand" }));
}

TEST(MotionBehaviour_FearRefusedGroundAndBlockedLegRetryWithoutTheRestDraw)
{
    FakeServices svc;
    FrightEast(svc);
    svc.groundFails = true;
    FearBehaviour f(Feared());
    Sight s = Standing(0.0f, 0.0f);
    f.Activate(s, svc);
    Step t = f.Tick(s, svc, 100);
    CHECK(CallsAre(svc, { "frand", "fright", "frand", "frand", "groundPoint" }));   // three frand, no urand
    CHECK(t.intent.act == MoveIntent::Act::Hold);
    CHECK(t.effects.empty());
    svc.calls.clear();
    CHECK(f.Tick(s, svc, 49).intent.act == MoveIntent::Act::Hold);   // the 50 ms retry
    CHECK(svc.calls.empty());
    f.Tick(s, svc, 1);
    CHECK_EQ(svc.calls.size(), size_t(5));                          // picked again, refused again
    // A leg the driver refused: the point forgotten, the retry set, and the tick FALLS THROUGH
    // -- with a diff past 50 ms it picks again in the same tick (the generator's order).
    svc.groundFails = false;
    svc.calls.clear();
    f.Tick(s, svc, 50);                                             // a point laid
    CHECK_EQ(svc.calls.size(), size_t(6));
    svc.calls.clear();
    Sight blocked = Standing(0.0f, 0.0f);
    blocked.status.blocked = true;
    Step b = f.Tick(blocked, svc, 100);
    CHECK(b.intent.act == MoveIntent::Act::Move);                   // the same tick picked again
    CHECK_EQ(svc.calls.size(), size_t(6));
    svc.calls.clear();
    Sight blockedShort = Standing(0.0f, 0.0f);
    blockedShort.status.blocked = true;
    CHECK(f.Tick(blockedShort, svc, 20).intent.act == MoveIntent::Act::Hold);   // 20 of the 50: holding, no draw
    CHECK(svc.calls.empty());
}

TEST(MotionBehaviour_FearBandsAndAnUnresolvedFright)
{
    // Beyond 43 yd: drift back, dist = 0.4 * 15 = 6 at (pi + pi - pi/4) = 7pi/4: toward the fright.
    {
        FakeServices svc;
        svc.frightResolved = true;
        svc.frightPosition = Vector3(50.0f, 0.0f, 0.0f);
        svc.frightDistance = 50.0f;
        FearBehaviour f(Feared());
        Sight s = Standing(0.0f, 0.0f);
        f.Activate(s, svc);
        f.Tick(s, svc, 100);
        CHECK(Close(svc.lastGuess.x, 6.0f * std::cos(7.0f * M_PI_F / 4.0f), 0.01f));
        CHECK(Close(svc.lastGuess.y, 6.0f * std::sin(7.0f * M_PI_F / 4.0f), 0.01f));
        CHECK(svc.lastGuess.x > 0.0f);
    }
    // Inside the band: dist = 0.6 * 15 = 9 at the second random draw (the fake's 0): due east.
    {
        FakeServices svc;
        svc.frightResolved = true;
        svc.frightPosition = Vector3(35.0f, 0.0f, 0.0f);
        svc.frightDistance = 35.0f;
        FearBehaviour f(Feared());
        Sight s = Standing(0.0f, 0.0f);
        f.Activate(s, svc);
        f.Tick(s, svc, 100);
        CHECK(CallsAre(svc, { "frand", "fright", "frand", "frand", "groundPoint", "urand" }));
        CHECK(Close(svc.lastGuess, Vector3(9.0f, 0.0f, 0.5f), 0.01f));
    }
    // No fright at all: the first draw's bearing (0) holds, the close band from a zero distance:
    // dist = 0.4 * 28 = 11.2, angle 0 - pi/8.
    {
        FakeServices svc;
        FearBehaviour f(Feared());
        Sight s = Standing(0.0f, 0.0f);
        f.Activate(s, svc);
        f.Tick(s, svc, 100);
        CHECK(CallsAre(svc, { "frand", "fright", "frand", "frand", "groundPoint", "urand" }));   // the fright asked, found nothing
        CHECK(Close(svc.lastGuess.x, 11.2f * std::cos(-M_PI_F / 8.0f), 0.01f));
        CHECK(Close(svc.lastGuess.y, 11.2f * std::sin(-M_PI_F / 8.0f), 0.01f));
    }
    // A fright within 0.2 yd: resolved, but its bearing is not taken; the distance still counts.
    {
        FakeServices svc;
        svc.frightResolved = true;
        svc.frightPosition = Vector3(0.1f, 0.0f, 0.0f);
        svc.frightDistance = 0.1f;
        FearBehaviour f(Feared());
        Sight s = Standing(0.0f, 0.0f);
        f.Activate(s, svc);
        f.Tick(s, svc, 100);
        CHECK(Close(svc.lastGuess.x, 0.4f * 27.9f * std::cos(-M_PI_F / 8.0f), 0.01f));
    }
}

TEST(MotionBehaviour_FearTimedClockEndsBeforeAnyDraw)
{
    FakeServices svc;
    FrightEast(svc);
    FearBehaviour f(Feared(1000));
    Sight s = Standing(0.0f, 0.0f);
    f.Activate(s, svc);
    Step t = f.Tick(s, svc, 600);                    // 400 ms left: a bolt
    CHECK(t.intent.act == MoveIntent::Act::Move);
    svc.calls.clear();
    Sight moving = Standing(0.0f, 0.0f);
    moving.status.traveling = true;
    CHECK(f.Tick(moving, svc, 300).intent.act == MoveIntent::Act::Move);   // 100 ms left
    CHECK(svc.calls.empty());
    Step d = f.Tick(s, svc, 100);                    // 0 left: Done, before the rest or a pick
    CHECK(d.intent.act == MoveIntent::Act::Done);
    CHECK(svc.calls.empty());
    CHECK(d.effects.empty());
    CHECK(f.EndReason(s) == FinishReason::Expired);
    // The clock keeps its remaining time across a reset: it is not re-armed. Straddle the
    // reset midway and let the two halves add to the limit.
    FearBehaviour g(Feared(1000));
    g.Activate(s, svc);
    g.Tick(s, svc, 600);                                             // a bolt, 400 ms left
    g.Resume(s, svc, true);
    svc.calls.clear();
    CHECK(g.Tick(s, svc, 400).intent.act == MoveIntent::Act::Done);   // 600 + 400 = 1000 ends it, before any draw
    CHECK(svc.calls.empty());
    // A dead mover ends the untimed flee too.
    FearBehaviour h(Feared());
    h.Activate(s, svc);
    Sight dead = Standing(0.0f, 0.0f);
    dead.alive = false;
    svc.calls.clear();
    CHECK(h.Tick(dead, svc, 100).intent.act == MoveIntent::Act::Done);
    CHECK(svc.calls.empty());
}

TEST(MotionBehaviour_FearFinishRecipesByReason)
{
    FakeServices svc;
    Sight creature = Standing(0.0f, 0.0f);
    Sight player = Player(0.0f, 0.0f);
    // An aura's release (Cancelled) on a creature with no surviving fear claim: the interrupt,
    // the bit cleared, then the gait restored LIVE, after the clear.
    {
        FearBehaviour f(Feared());
        Outcome o = f.Finish(FinishReason::Cancelled, creature, svc);
        CHECK(o.interrupt);
        CHECK(!o.stop && !o.stopForced);
        REQUIRE(o.effects.size() == size_t(2));
        CHECK(o.effects[0].kind == Effect::StateRaw);
        CHECK_EQ(o.effects[0].clearMask, 0x40000u);
        CHECK(o.effects[1].kind == Effect::RestoreGait);
    }
    // The same with another fear claim surviving: the run is its, no restore.
    {
        svc.fearHeld = true;
        FearBehaviour f(Feared());
        Outcome o = f.Finish(FinishReason::Superseded, creature, svc);
        CHECK(o.interrupt);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::StateRaw);
        svc.fearHeld = false;
    }
    // A displacing finish on a player: no gait, no stop of its own (the interrupt stops it).
    {
        FearBehaviour f(Feared());
        Outcome o = f.Finish(FinishReason::Overridden, player, svc);
        CHECK(o.interrupt && !o.stop);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::StateRaw);
    }
    // The untimed Finalize (Expired, Died, Cleared) on a creature: the gait read BEFORE the
    // clear, then the clear; no interrupt.
    {
        FearBehaviour f(Feared());
        FinishReason const reasons[] = { FinishReason::Expired, FinishReason::Died, FinishReason::Cleared };
        for (size_t i = 0; i < 3; ++i)
        {
            Outcome o = f.Finish(reasons[i], creature, svc);
            CHECK(!o.interrupt && !o.stop);
            REQUIRE(o.effects.size() == size_t(2));
            CHECK(o.effects[0].kind == Effect::RestoreGait);
            CHECK(o.effects[1].kind == Effect::StateRaw);
            CHECK_EQ(o.effects[1].clearMask, 0x40000u);
        }
    }
    // The untimed Finalize on a player: the soft stop, then the clear; no gait effect.
    {
        FearBehaviour f(Feared());
        Outcome o = f.Finish(FinishReason::Died, player, svc);
        CHECK(o.stop && !o.stopForced && !o.interrupt);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::StateRaw);
    }
    // The timed Finalize: the clear, the flag dropped when no fear claim survives, the gait
    // restored unconditionally (design §6.5: the generator left the run), the re-engage.
    {
        FearBehaviour f(Feared(3000));
        Outcome o = f.Finish(FinishReason::Expired, creature, svc);
        CHECK(!o.interrupt && !o.stop);
        REQUIRE(o.effects.size() == size_t(4));
        CHECK(o.effects[0].kind == Effect::StateRaw);
        CHECK(o.effects[1].kind == Effect::ClearFleeingFlag);
        CHECK(o.effects[2].kind == Effect::RestoreGait);     // after the clear, before the re-engage
        CHECK(o.effects[3].kind == Effect::AttackVictim);
        svc.fearHeld = true;
        Outcome held = f.Finish(FinishReason::Expired, creature, svc);
        REQUIRE(held.effects.size() == size_t(3));
        CHECK(held.effects[0].kind == Effect::StateRaw);
        CHECK(held.effects[1].kind == Effect::RestoreGait);
        CHECK(held.effects[2].kind == Effect::AttackVictim);   // the flag is the survivor's
        svc.fearHeld = false;
        // A timed flee cancelled (the possession's take): the displacing recipe, as the untimed one.
        Outcome cancelled = f.Finish(FinishReason::Cancelled, creature, svc);
        CHECK(cancelled.interrupt);
        REQUIRE(cancelled.effects.size() == size_t(2));
        CHECK(cancelled.effects[1].kind == Effect::RestoreGait);
    }
    // A displacing finish of an already-suspended behaviour: the generator's Interrupt, which
    // carried the move bit's clear, was skipped for a behaviour already suspended, and Suspend()
    // had cleared the bit itself -- a bit set since then belongs to the claim that drives now,
    // so this finish must leave it alone (read from the Sight's `suspended`, filled by the
    // adapter from its own Suspend()/Resume() bookkeeping, not a second flag on the native); the
    // gait restore stays unconditional on suspension.
    {
        Sight suspended = creature;
        suspended.suspended = true;
        FearBehaviour f(Feared());
        f.Activate(creature, svc);
        f.Suspend();
        Outcome o = f.Finish(FinishReason::Cancelled, suspended, svc);
        CHECK(o.interrupt);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::RestoreGait);
        svc.fearHeld = true;
        Outcome held = f.Finish(FinishReason::Cancelled, suspended, svc);
        CHECK(held.interrupt);
        CHECK(held.effects.empty());
        svc.fearHeld = false;
        // Resumed without a reset: the Sight passed is no longer suspended, so the clear is back.
        f.Resume(creature, svc, false);
        Outcome resumed = f.Finish(FinishReason::Cancelled, creature, svc);
        REQUIRE(resumed.effects.size() == size_t(2));
        CHECK(resumed.effects[0].kind == Effect::StateRaw);
        CHECK(resumed.effects[1].kind == Effect::RestoreGait);
        // Suspended, then a reset: a reset re-activates, so the clear is back too.
        f.Suspend();
        f.Resume(creature, svc, true);
        Outcome afterReset = f.Finish(FinishReason::Cancelled, creature, svc);
        REQUIRE(afterReset.effects.size() == size_t(2));
        CHECK(afterReset.effects[0].kind == Effect::StateRaw);
        CHECK(afterReset.effects[1].kind == Effect::RestoreGait);
    }
    svc.fearHeld = false;   // later cases keep their assumptions
}

TEST(MotionBehaviour_ConfusedHooksKeepTheAnchorThroughAReset)
{
    FakeServices svc;
    ConfusedBehaviour c(Confusing());
    Sight s = Standing(3.0f, 4.0f);
    Step a = c.Activate(s, svc);
    CHECK(Close(c.Anchor(), Vector3(3.0f, 4.0f, 0.0f)));
    CHECK(a.stop && a.resetLeg && !a.interrupt && !a.apply);
    REQUIRE(a.effects.size() == size_t(1));
    CHECK(a.effects[0].kind == Effect::StateRaw);   // after the stop: the bit ends SET
    CHECK_EQ(a.effects[0].setMask, 0x400u);
    CHECK_EQ(a.effects[0].clearMask, 0u);
    CHECK(svc.calls.empty());
    CHECK(c.EndReason(s) == FinishReason::Expired);
    CHECK(!c.TracksTarget());
    CHECK_EQ(c.Variant(), 0u);
    // Held at activation (UNIT_STAT_NOT_MOVE: a root, a stun, a distract's stand): no stop, no bit, the leg still forgotten.
    Sight held = Standing(3.0f, 4.0f);
    held.notMove = true;
    ConfusedBehaviour d(Confusing());
    Step h = d.Activate(held, svc);
    CHECK(!h.stop && h.resetLeg && h.effects.empty());
    Sight dead = Standing(3.0f, 4.0f);
    dead.alive = false;
    ConfusedBehaviour e(Confusing());
    Step dd = e.Activate(dead, svc);
    CHECK(!dd.stop && dd.resetLeg && dd.effects.empty());
    // The suspend: the interrupt, the bit cleared, the leg forgotten.
    Step su = c.Suspend();
    CHECK(su.interrupt && su.resetLeg && !su.stop);
    REQUIRE(su.effects.size() == size_t(1));
    CHECK_EQ(su.effects[0].clearMask, 0x400u);
    // A reset from somewhere else: the stop and the bit again, the anchor untouched.
    Sight elsewhere = Standing(30.0f, 40.0f);
    Step r = c.Resume(elsewhere, svc, true);
    CHECK(r.stop && r.resetLeg);
    CHECK(Close(c.Anchor(), Vector3(3.0f, 4.0f, 0.0f)));
    CHECK(c.Resume(elsewhere, svc, false).effects.empty());
    // Suspend()'s forgetting: after a laid leg, a suspend and a resume WITHOUT a reset must not
    // re-state the stale lurch. traveling && haveLurch is false, so a Hold until the stagger
    // passes, then a fresh lurch.
    c.Activate(s, svc);
    c.Tick(s, svc, 100);                             // a lurch, stagger 800
    c.Suspend();
    c.Resume(s, svc, false);
    svc.calls.clear();
    Sight moving = Standing(0.0f, 0.0f);
    moving.status.traveling = true;
    CHECK(c.Tick(moving, svc, 100).intent.act == MoveIntent::Act::Hold);
    CHECK(svc.calls.empty());
    CHECK(c.Tick(moving, svc, 700).intent.act == MoveIntent::Act::Move);
    CHECK(CallsAre(svc, { "random", "urand" }));
}

TEST(MotionBehaviour_ConfusedLurchesFromTheAnchorAtAWalkAndSupersedesMidLeg)
{
    FakeServices svc;
    ConfusedBehaviour c(Confusing(10.0f));
    Sight s = Standing(3.0f, 4.0f);
    c.Activate(s, svc);
    Step t = c.Tick(s, svc, 100);                    // the stagger 0: a lurch at once
    CHECK(CallsAre(svc, { "random", "urand" }));
    CHECK(Close(svc.randomRadius, 10.0f));           // the radius passed through
    CHECK(t.apply);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t.intent.flags, uint32(MOVE_WALK));
    CHECK(Close(t.intent.goal, Vector3(13.0f, 4.0f, 0.0f)));   // the fake: the anchor + the radius along x
    REQUIRE(t.effects.size() == size_t(1));
    CHECK(t.effects[0].kind == Effect::StateRaw);   // re-asserted every tick
    CHECK_EQ(t.effects[0].setMask, 0x400u);
    // Traveling: the lurch re-stated, the bit again, no draw; the stagger (800) counts meanwhile.
    svc.calls.clear();
    Sight moving = Standing(5.0f, 4.0f);
    moving.status.traveling = true;
    Step m = c.Tick(moving, svc, 700);
    CHECK(m.intent.act == MoveIntent::Act::Move);
    CHECK(Close(m.intent.goal, t.intent.goal));
    CHECK_EQ(m.effects.size(), size_t(1));
    CHECK(svc.calls.empty());
    // 800 ms in and still traveling: a fresh point supersedes the leg being walked.
    Step n = c.Tick(moving, svc, 100);
    CHECK(n.intent.act == MoveIntent::Act::Move);
    CHECK(CallsAre(svc, { "random", "urand" }));
    // The anchor stays the pick's centre wherever the unit is.
    CHECK(Close(n.intent.goal, Vector3(13.0f, 4.0f, 0.0f)));
    // Standing with the stagger not passed: a hold, the bit still written.
    svc.calls.clear();
    Step h = c.Tick(s, svc, 100);
    CHECK(h.intent.act == MoveIntent::Act::Hold);
    CHECK_EQ(h.effects.size(), size_t(1));
    CHECK(svc.calls.empty());
}

TEST(MotionBehaviour_ConfusedRetryDoublesAndABlockedTickAdvancesItTwice)
{
    FakeServices svc;
    svc.randomFails = true;
    ConfusedBehaviour c(Confusing());
    Sight s = Standing(0.0f, 0.0f);
    c.Activate(s, svc);
    // Each refused pick draws through the port (random alone, no urand) and waits 50, 100, 200, 400, 800, 800.
    const uint32 waits[] = { 50, 100, 200, 400, 800, 800 };
    CHECK(c.Tick(s, svc, 100).intent.act == MoveIntent::Act::Hold);   // the first pick, refused: 50
    CHECK(CallsAre(svc, { "random" }));
    for (size_t i = 0; i < 6; ++i)
    {
        svc.calls.clear();
        CHECK(c.Tick(s, svc, waits[i] - 1).intent.act == MoveIntent::Act::Hold);   // one short: no draw
        CHECK(svc.calls.empty());
        CHECK(c.Tick(s, svc, 1).intent.act == MoveIntent::Act::Hold);             // passed: the next refused pick
        CHECK(CallsAre(svc, { "random" }));
    }
    // A blocked tick: the retry set once for the refusal, the tick falls through, the diff
    // passes it, the pick is refused again and the retry advances a second time.
    ConfusedBehaviour d(Confusing());
    d.Activate(s, svc);
    svc.calls.clear();
    Sight blocked = Standing(0.0f, 0.0f);
    blocked.status.blocked = true;
    CHECK(d.Tick(blocked, svc, 100).intent.act == MoveIntent::Act::Hold);   // 50 set, passed, refused: 100 set
    CHECK(CallsAre(svc, { "random" }));
    svc.calls.clear();
    CHECK(d.Tick(s, svc, 99).intent.act == MoveIntent::Act::Hold);          // holding the 100
    CHECK(svc.calls.empty());
    CHECK(d.Tick(s, svc, 1).intent.act == MoveIntent::Act::Hold);           // passed: refused again, 200 next
    CHECK(CallsAre(svc, { "random" }));
    svc.calls.clear();
    CHECK(d.Tick(s, svc, 199).intent.act == MoveIntent::Act::Hold);
    CHECK(svc.calls.empty());
    // A lurch that gets laid ends the streak: the next refusal waits 50 again.
    svc.randomFails = false;
    CHECK(d.Tick(s, svc, 1).intent.act == MoveIntent::Act::Move);
    Sight moving = Standing(0.0f, 0.0f);
    moving.status.traveling = true;
    d.Tick(moving, svc, 100);                                               // seen traveling: retries 0
    svc.randomFails = true;
    svc.calls.clear();
    d.Tick(moving, svc, 700);                                               // the stagger (800) passed: refused, 50
    CHECK(CallsAre(svc, { "random" }));
    svc.calls.clear();
    CHECK(d.Tick(s, svc, 49).intent.act == MoveIntent::Act::Hold);
    CHECK(svc.calls.empty());
    d.Tick(s, svc, 1);
    CHECK(CallsAre(svc, { "random" }));
}

TEST(MotionBehaviour_ConfusedFinishRecipesByReason)
{
    FakeServices svc;
    Sight creature = Standing(0.0f, 0.0f);
    Sight player = Player(0.0f, 0.0f);
    ConfusedBehaviour c(Confusing());
    // Displacing: the interrupt and the explicit clear (the moving mask does not hold the bit); nothing else.
    Outcome d = c.Finish(FinishReason::Cancelled, creature, svc);
    CHECK(d.interrupt && !d.stop && !d.stopForced);
    REQUIRE(d.effects.size() == size_t(1));
    CHECK(d.effects[0].kind == Effect::StateRaw);
    CHECK_EQ(d.effects[0].clearMask, 0x400u);
    Outcome dp = c.Finish(FinishReason::Superseded, player, svc);
    CHECK(dp.interrupt && !dp.stopForced);
    // The Finalize (Expired, Died, Cleared): the clear; a player's forced stop, a creature's spline left alone.
    FinishReason const reasons[] = { FinishReason::Expired, FinishReason::Died, FinishReason::Cleared };
    for (size_t i = 0; i < 3; ++i)
    {
        Outcome o = c.Finish(reasons[i], creature, svc);
        CHECK(!o.interrupt && !o.stop && !o.stopForced);
        REQUIRE(o.effects.size() == size_t(1));
        CHECK(o.effects[0].kind == Effect::StateRaw);
        Outcome p = c.Finish(reasons[i], player, svc);
        CHECK(p.stopForced && !p.stop && !p.interrupt);
        CHECK_EQ(p.effects.size(), size_t(1));
    }
    // A displacing finish of an already-suspended behaviour: Suspend() already cleared the
    // bit, so the finish must leave it alone (read from the Sight's `suspended`); resumed
    // without a reset, the clear is back.
    {
        Sight suspended = creature;
        suspended.suspended = true;
        c.Activate(creature, svc);
        c.Suspend();
        Outcome o = c.Finish(FinishReason::Superseded, suspended, svc);
        CHECK(o.interrupt);
        CHECK(o.effects.empty());
        c.Resume(creature, svc, false);
        Outcome resumed = c.Finish(FinishReason::Cancelled, creature, svc);
        CHECK(resumed.interrupt);
        REQUIRE(resumed.effects.size() == size_t(1));
        CHECK(resumed.effects[0].kind == Effect::StateRaw);
        // Suspended, then a reset: a reset re-activates, so the clear is back after a reset too.
        c.Suspend();
        c.Resume(creature, svc, true);
        Outcome afterReset = c.Finish(FinishReason::Cancelled, creature, svc);
        CHECK(afterReset.interrupt);
        REQUIRE(afterReset.effects.size() == size_t(1));
        CHECK(afterReset.effects[0].kind == Effect::StateRaw);
    }
}
