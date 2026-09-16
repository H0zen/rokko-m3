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
}

TEST(MotionBehaviour_PointActivatesThenLaysItsLegOnTheFirstTick)
{
    PointBehaviour b(PointTo(10.0f, 0.0f, 0.0f));
    Step a = b.Activate(Free());
    CHECK(a.stop);
    CHECK(a.resetLeg);
    CHECK(a.roaming == Roaming::SetBoth);
    CHECK(!a.apply);                                     // the leg is laid by the tick, not the activation
    Step t = b.Tick(Free(), 100);
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
    Step a = b.Activate(s);
    CHECK(!a.stop);
    CHECK(a.roaming == Roaming::Keep);
}

TEST(MotionBehaviour_PointHoldsWithMoveBitClearedWhileItCannotMove)
{
    PointBehaviour b(PointTo(10.0f, 0.0f, 0.0f));
    b.Activate(Free());
    Sight s = Free();
    s.canMove = false;
    Step t = b.Tick(s, 100);
    CHECK(t.intent.act == MoveIntent::Act::Hold);
    CHECK(t.roaming == Roaming::ClearMove);
}

TEST(MotionBehaviour_PointEndsAndInformsOnArrivedBlockedAndCut)
{
    {
        PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f, 55));
        b.Activate(Free());
        CHECK(b.Tick(Arrived(), 100).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Arrived);
        Outcome o = b.Finish(FinishReason::Arrived, Free());
        CHECK(HasEffect(o, Effect::Inform));
        CHECK(HasEffect(o, Effect::SummonedInform));
        CHECK_EQ(o.effects[0].id, 55u);
        CHECK(o.roaming == Roaming::ClearBoth);
    }
    {
        PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
        b.Activate(Free());
        CHECK(b.Tick(Blocked(), 100).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Blocked);
        CHECK(HasEffect(b.Finish(FinishReason::Blocked, Free()), Effect::Inform));
    }
    {
        PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
        b.Activate(Free());
        CHECK(b.Tick(Cut(), 100).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(Free()) == FinishReason::Cut);
        CHECK(HasEffect(b.Finish(FinishReason::Cut, Free()), Effect::Inform));   // told anyway, as the generator
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
        b.Activate(Free());
        b.Tick(Free(), 100);
        Outcome o = b.Finish(reason, Free());
        CHECK(o.effects.empty());
        CHECK(o.interrupt);
        CHECK(o.roaming == Roaming::ClearBoth);
    }
}

TEST(MotionBehaviour_PointSuspendedThenResumedWithResetRelaysFromTheSpot)
{
    PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
    b.Activate(Free());
    b.Tick(Free(), 100);
    Step s = b.Suspend();
    CHECK(s.interrupt);
    CHECK(s.resetLeg);
    CHECK(s.roaming == Roaming::ClearBoth);
    Step r = b.Resume(Free(), true);
    CHECK(r.stop);
    CHECK(r.resetLeg);
    CHECK(b.Resume(Free(), false).roaming == Roaming::Keep);
    // A leg cut while suspended is not the behaviour's own end: no inform on a Cleared finish either way.
    Outcome o = b.Finish(FinishReason::Cleared, Free());
    CHECK(!HasEffect(o, Effect::Inform));   // m_done never set: the tick never saw an edge
}

TEST(MotionBehaviour_PointRestatesTheGoalOnPartial)
{
    PointBehaviour b(PointTo(1.0f, 2.0f, 3.0f));
    b.Activate(Free());
    Step t = b.Tick(Partial(), 100);
    CHECK(t.intent.act == MoveIntent::Act::Move);   // not an end
    CHECK_EQ(t.intent.goal.x, 1.0f);                // the same goal, re-stated
}

TEST(MotionBehaviour_AssistRunCallsAssistanceOnEveryNonDisplacingFinishAndNeverInforms)
{
    PointBehaviour::Params p = PointTo(1.0f, 2.0f, 3.0f, 0);
    p.kind = Kind::AssistRun;
    p.flags = MOVE_WALK;
    PointBehaviour b(p);
    b.Activate(Free());
    Step moving = b.Tick(Free(), 100);
    CHECK(moving.intent.act == MoveIntent::Act::Move);
    CHECK(moving.intent.Has(MOVE_WALK));            // the flags pass through to the leg
    b.Tick(Arrived(), 100);
    Outcome arrived = b.Finish(FinishReason::Arrived, Free());
    CHECK(!HasEffect(arrived, Effect::Inform));
    CHECK(HasEffect(arrived, Effect::CallAssistance));
    CHECK(HasEffect(arrived, Effect::SeekAssistDistract));
    CHECK(arrived.effects[0].kind == Effect::CallAssistance);   // the order: assistance first, then the distract
    Sight dead = Free();
    dead.alive = false;
    Outcome died = b.Finish(FinishReason::Died, dead);
    CHECK(HasEffect(died, Effect::CallAssistance));
    CHECK(!HasEffect(died, Effect::SeekAssistDistract));       // not alive: no distract
    CHECK(!HasEffect(b.Finish(FinishReason::Superseded, Free()), Effect::CallAssistance));
}

TEST(MotionBehaviour_DistractCountsDownStrictlyAndAssistDistractAttacksOnFinish)
{
    DistractBehaviour d(Kind::Distract, 1000);
    CHECK(d.Tick(Free(), 400).intent.act != MoveIntent::Act::Done);
    CHECK(d.Tick(Free(), 600).intent.act != MoveIntent::Act::Done);   // equality: alive at zero
    CHECK(d.Tick(Free(), 1).intent.act == MoveIntent::Act::Done);
    CHECK(d.EndReason(Free()) == FinishReason::Expired);
    CHECK(d.Finish(FinishReason::Expired, Free()).effects.empty());
    DistractBehaviour a(Kind::AssistDistract, 10);
    CHECK(HasEffect(a.Finish(FinishReason::Superseded, Free()), Effect::AttackVictim));
    CHECK(HasEffect(a.Finish(FinishReason::Expired, Free()), Effect::AttackVictim));
}

TEST(MotionBehaviour_EffectLaunchesOnceHoldsWhileTravelingAndEnds)
{
    EffectLaunch l;
    l.kind = EffectLaunch::Jump;
    l.point = Vector3(5.0f, 5.0f, 0.0f);
    EffectBehaviour e(66, l);
    Step a = e.Activate(Free());
    CHECK(a.apply);
    CHECK(a.intent.act == MoveIntent::Act::Launch);
    CHECK(!e.Activate(Free()).apply);   // once
    CHECK(e.Tick(Traveling(), 100).intent.act == MoveIntent::Act::Hold);
    Sight landed = Free();
    landed.landed = true;
    CHECK(e.Tick(landed, 100).intent.act == MoveIntent::Act::Done);
    CHECK(e.EndReason(landed) == FinishReason::Arrived);
    Outcome o = e.Finish(FinishReason::Arrived, landed);
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
        e.Activate(Free());
        CHECK(e.Finish(reason, Free()).effects.empty());    // flying: nothing happened
        Sight landed = Free();
        landed.landed = true;
        Outcome o = e.Finish(reason, landed);
        CHECK(HasEffect(o, Effect::Inform));            // landed, unconsumed: the effect happened
        CHECK(HasEffect(o, Effect::ReengageVictim));    // the whole finalizer runs when landed, re-engage included
    }
    {
        EffectBehaviour e(72, l);
        e.Activate(Free());
        CHECK(e.Tick(Cut(), 100).intent.act == MoveIntent::Act::Done);   // not traveling: over
        CHECK(e.EndReason(Cut()) == FinishReason::Cut);
        Outcome o = e.Finish(FinishReason::Cut, Cut());
        CHECK(HasEffect(o, Effect::Inform));            // its own tick ended it: told anyway
        CHECK(HasEffect(o, Effect::ReengageVictim));
    }
    {
        EffectBehaviour e(73, l);
        e.Activate(Free());
        Outcome o = e.Finish(FinishReason::Died, Free());   // not displacing, not landed, not done
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
    b.Activate(Free());
    Sight s = Free();
    s.hasTarget = true;
    s.targetPoint = Vector3(10.0f, 0.0f, 0.0f);
    Step t1 = b.Tick(s, 100);
    CHECK(t1.intent.act == MoveIntent::Act::Move);
    CHECK_EQ(t1.intent.goal.x, 10.0f);
    CHECK_EQ(t1.intent.speed, 24.0f);
    s.targetPoint = Vector3(11.0f, 0.0f, 0.0f);            // 1 yd: under the tolerance
    CHECK_EQ(b.Tick(s, 100).intent.goal.x, 10.0f);
    CHECK_EQ(b.Relays(), 0u);
    s.targetPoint = Vector3(13.0f, 0.0f, 0.0f);            // 3 yd, but only 200 ms since the leg
    CHECK_EQ(b.Tick(s, 100).intent.goal.x, 10.0f);
    CHECK_EQ(b.Tick(s, 300).intent.goal.x, 13.0f);         // 600 ms: within budget, re-laid
    CHECK_EQ(b.Relays(), 1u);
    // A leg that ended where the target WAS: the target walked on past the tolerance, so a
    // fresh leg is laid at once (no budget wait), not an arrival.
    {
        Sight ended = s;
        ended.status.arrived = true;
        ended.targetPoint = Vector3(16.0f, 0.0f, 0.0f);   // 3 yd past the laid goal of 13
        Step again = b.Tick(ended, 50);
        CHECK(again.intent.act == MoveIntent::Act::Move);
        CHECK_EQ(again.intent.goal.x, 16.0f);
        CHECK_EQ(b.Relays(), 2u);
        // ... and a leg that ended within the tolerance is an arrival.
        Sight close = s;
        close.status.arrived = true;
        close.targetPoint = Vector3(16.5f, 0.0f, 0.0f);
        CHECK(b.Tick(close, 50).intent.act == MoveIntent::Act::Done);
        CHECK(b.EndReason(close) == FinishReason::Arrived);
    }
    {
        PointBehaviour::Params lp = PointTo(0.0f, 0.0f, 0.0f, 0);
        lp.target = 0x42ull;
        lp.informs = false;
        PointBehaviour lost(lp);
        lost.Activate(Free());
        Sight gone = Free();
        gone.hasTarget = false;
        CHECK(lost.Tick(gone, 100).intent.act == MoveIntent::Act::Done);
        CHECK(lost.EndReason(gone) == FinishReason::TargetLost);
        CHECK(lost.Finish(FinishReason::TargetLost, gone).effects.empty());   // never informs
    }

    // `informs = false` must hold on a real edge too, not only on TargetLost (which never
    // sets m_done and so would pass this check vacuously even with the guard deleted).
    {
        PointBehaviour::Params ap = PointTo(0.0f, 0.0f, 0.0f, 0);
        ap.target = 0x42ull;
        ap.informs = false;
        PointBehaviour arriving(ap);
        arriving.Activate(Free());
        Sight there = Free();
        there.hasTarget = true;
        there.targetPoint = Vector3(10.0f, 0.0f, 0.0f);
        CHECK(arriving.Tick(there, 100).intent.act == MoveIntent::Act::Move);   // the first leg
        there.status.arrived = true;                                             // ended at the target's point
        CHECK(arriving.Tick(there, 100).intent.act == MoveIntent::Act::Done);
        CHECK(arriving.EndReason(there) == FinishReason::Arrived);
        Outcome arrivedOutcome = arriving.Finish(FinishReason::Arrived, there);
        CHECK(!HasEffect(arrivedOutcome, Effect::Inform));
        CHECK(!HasEffect(arrivedOutcome, Effect::SummonedInform));
    }

    // The swoop: no tracked target, `informs = false`, a fixed goal — same silence on arrival.
    {
        PointBehaviour::Params sp = PointTo(1.0f, 2.0f, 3.0f, 0);
        sp.informs = false;
        PointBehaviour swoop(sp);
        swoop.Activate(Free());
        CHECK(swoop.Tick(Arrived(), 100).intent.act == MoveIntent::Act::Done);
        Outcome swoopOutcome = swoop.Finish(FinishReason::Arrived, Free());
        CHECK(!HasEffect(swoopOutcome, Effect::Inform));
        CHECK(!HasEffect(swoopOutcome, Effect::SummonedInform));
    }
}

TEST(MotionBehaviour_IdleDoesNothingAndFinishesSilently)
{
    IdleBehaviour i;
    CHECK(!i.Activate(Free()).apply);
    CHECK(!i.Tick(Free(), 100).apply);
    CHECK(i.Finish(FinishReason::Superseded, Free()).effects.empty());
}

TEST(MotionBehaviour_FlyLandLaysAStraightFlyingLegAndInformsAsAPoint)
{
    PointBehaviour::Params p = PointTo(4.0f, 5.0f, 6.0f, 5);
    p.kind = Kind::FlyLand;
    p.flags = MOVE_FLY | MOVE_STRAIGHT;
    PointBehaviour b(p);
    CHECK(b.Kind() == Kind::FlyLand);
    b.Activate(Free());
    Step t = b.Tick(Free(), 100);
    CHECK(t.intent.act == MoveIntent::Act::Move);
    CHECK(t.intent.Has(MOVE_FLY));
    CHECK(t.intent.Has(MOVE_STRAIGHT));
    CHECK(b.Tick(Arrived(), 100).intent.act == MoveIntent::Act::Done);
    Outcome o = b.Finish(FinishReason::Arrived, Free());
    CHECK(HasEffect(o, Effect::Inform));
    CHECK(o.effects[0].who == Kind::FlyLand);
    CHECK_EQ(o.effects[0].id, 5u);
}
