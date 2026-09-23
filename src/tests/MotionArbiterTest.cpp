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

// The movement kernel's arbiter (design v2 §4, P3-A): the kind/layer/policy
// tables and the pure selection model. Nothing here touches a unit, a driver
// or a map; the model only decides.

#include "TestHarness.h"
#include "Arbiter.h"
#include <string>

using namespace Motion;

namespace
{
    int L(Layer layer) { return static_cast<int>(layer); }
    int P(Policy policy) { return static_cast<int>(policy); }
    int K(Kind kind) { return static_cast<int>(kind); }

    MoveRequest Req(Kind kind, uint32 id = 0, bool resumeCombat = false)
    {
        MoveRequest r;
        r.kind = kind;
        r.id = id;
        r.resumeCombat = resumeCombat;
        return r;
    }

    /// The shell's Motion::ControlClaim shape (MotionMaster.h), spelled out because this file is
    /// kernel-only: spell << 40 | effect << 32 | caster counter. Motion::ClaimSpell reads the
    /// spell back out of it, and a zero there is what marks a claim nobody's aura took.
    uint64 ControlClaimShape(uint32 spellId, uint8 effIndex, uint32 casterCounter)
    {
        return (uint64(spellId) << 40) | (uint64(effIndex) << 32) | uint64(casterCounter);
    }

    MoveRequest Claim(Kind kind, uint64 claim, uint32 id = 0)
    {
        MoveRequest r;
        r.kind = kind;
        r.id = id;
        r.claim = claim;
        return r;
    }

    int SelectedKind(Arbiter const& m)
    {
        std::optional<Held> sel = m.Selected();
        return sel ? K(sel->kind) : -1;
    }

    int CountEvents(std::vector<Event> const& events, Event::Kind kind, Kind who)
    {
        int n = 0;
        for (Event const& e : events)
        {
            if (e.kind == kind && e.who == who)
            {
                ++n;
            }
        }
        return n;
    }

    bool HasFinished(std::vector<Event> const& events, Kind who, uint32 id, FinishReason reason)
    {
        for (Event const& e : events)
        {
            if (e.kind == Event::Kind::Finished && e.who == who && e.id == id && e.reason == reason)
            {
                return true;
            }
        }
        return false;
    }

    int Size(Arbiter const& m) { return static_cast<int>(m.Contents().size()); }
}

TEST(MotionArbiter_Contract_LayerTable)
{
    CHECK_EQ(L(LayerOf(Kind::Idle)), L(Layer::Default));
    CHECK_EQ(L(LayerOf(Kind::Wander)), L(Layer::Default));
    CHECK_EQ(L(LayerOf(Kind::Patrol)), L(Layer::Default));
    CHECK_EQ(L(LayerOf(Kind::Follow)), L(Layer::Default));
    CHECK_EQ(L(LayerOf(Kind::Chase)), L(Layer::Combat));
    CHECK_EQ(L(LayerOf(Kind::Point)), L(Layer::Scripted));
    CHECK_EQ(L(LayerOf(Kind::FlyLand)), L(Layer::Scripted));
    CHECK_EQ(L(LayerOf(Kind::Home)), L(Layer::Scripted));
    CHECK_EQ(L(LayerOf(Kind::AssistRun)), L(Layer::Scripted));
    CHECK_EQ(L(LayerOf(Kind::Distract)), L(Layer::Distract));
    CHECK_EQ(L(LayerOf(Kind::AssistDistract)), L(Layer::Distract));
    CHECK_EQ(L(LayerOf(Kind::Fear)), L(Layer::Control));
    CHECK_EQ(L(LayerOf(Kind::Confused)), L(Layer::Control));
    CHECK_EQ(L(LayerOf(Kind::Effect)), L(Layer::Forced));
    CHECK_EQ(L(LayerOf(Kind::Taxi)), L(Layer::Taxi));
}

TEST(MotionArbiter_Contract_PolicyTable)
{
    CHECK_EQ(P(PolicyOf(Kind::Wander, false)), P(Policy::Override));    // public MoveRandomAroundPoint
    CHECK_EQ(P(PolicyOf(Kind::Patrol, false)), P(Policy::Override));    // D7
    CHECK_EQ(P(PolicyOf(Kind::Follow, false)), P(Policy::Supersede));
    CHECK_EQ(P(PolicyOf(Kind::Idle, false)), P(Policy::Suspend));
    CHECK_EQ(P(PolicyOf(Kind::Chase, false)), P(Policy::Supersede));    // D6: update
    CHECK_EQ(P(PolicyOf(Kind::Point, false)), P(Policy::Override));     // D2
    CHECK_EQ(P(PolicyOf(Kind::Point, true)), P(Policy::Suspend));       // resumeCombat
    CHECK_EQ(P(PolicyOf(Kind::FlyLand, false)), P(Policy::Override));
    CHECK_EQ(P(PolicyOf(Kind::Home, false)), P(Policy::Override));
    CHECK_EQ(P(PolicyOf(Kind::AssistRun, false)), P(Policy::Override));
    CHECK_EQ(P(PolicyOf(Kind::Distract, false)), P(Policy::Suspend));
    CHECK_EQ(P(PolicyOf(Kind::AssistDistract, false)), P(Policy::Suspend));
    CHECK_EQ(P(PolicyOf(Kind::Fear, false)), P(Policy::Suspend));
    CHECK_EQ(P(PolicyOf(Kind::Confused, false)), P(Policy::Suspend));
    CHECK_EQ(P(PolicyOf(Kind::Effect, false)), P(Policy::Suspend));
    CHECK_EQ(P(PolicyOf(Kind::Taxi, false)), P(Policy::Override));      // D8 split handled in the model
}

TEST(MotionArbiter_Contract_SelfExpiringMatchesMutate)
{
    // MotionMaster::Mutate expires a HOME, DISTRACT or EFFECT top before pushing
    // anything; AssistDistract reports its own type, which is not in that switch.
    CHECK(SelfExpiring(Kind::Home));
    CHECK(SelfExpiring(Kind::Distract));
    CHECK(!SelfExpiring(Kind::AssistDistract));
    CHECK(SelfExpiring(Kind::Effect));
    CHECK(!SelfExpiring(Kind::Point));
    CHECK(!SelfExpiring(Kind::Chase));
    CHECK(!SelfExpiring(Kind::Fear));
}

TEST(MotionArbiter_Contract_Names)
{
    CHECK_STR(KindName(Kind::Idle), "Idle");
    CHECK_STR(KindName(Kind::AssistRun), "AssistRun");
    CHECK_STR(KindName(Kind::AssistDistract), "AssistDistract");
    CHECK_STR(KindName(Kind::Taxi), "Taxi");
    CHECK_STR(KindName(Kind::Count), "?");
    CHECK_STR(LayerName(Layer::Default), "Default");
    CHECK_STR(LayerName(Layer::Taxi), "Taxi");
    CHECK_STR(LayerName(Layer::Count), "?");
    CHECK_STR(ReasonName(FinishReason::Arrived), "Arrived");
    CHECK_STR(ReasonName(FinishReason::Died), "Died");
}

TEST(MotionArbiter_EmptyThenFactoryDefault)
{
    Arbiter m;
    CHECK(m.Empty());
    CHECK_EQ(SelectedKind(m), -1);
    m.InstallDefault(Kind::Wander);
    CHECK(!m.Empty());
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));
    CHECK_EQ(Size(m), 1);
}

TEST(MotionArbiter_ChaseAboveDefault_AndUpdatesInPlace)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    CHECK_EQ(SelectedKind(m), K(Kind::Chase));
    const uint32 firstSeq = m.Combat()->seq;
    m.Request(Req(Kind::Chase));                                      // D6: update, no second chase
    CHECK_EQ(Size(m), 2);
    CHECK(m.Combat()->seq != firstSeq);
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Suspended, Kind::Wander), 1);
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Chase), 0);
}

TEST(MotionArbiter_PointOverridesCombat_DefaultResumesAfter)
{
    Arbiter m;
    m.InstallDefault(Kind::Patrol);
    m.Request(Req(Kind::Chase));
    m.Request(Req(Kind::Point, 7));
    CHECK_EQ(SelectedKind(m), K(Kind::Point));
    CHECK(!m.Combat());                                               // D2: combat cancelled
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Chase), 1);
    m.ExpireSelected();                                               // the point arrives
    CHECK_EQ(SelectedKind(m), K(Kind::Patrol));
    ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Point), 1);
    CHECK_EQ(CountEvents(ev, Event::Kind::Resumed, Kind::Patrol), 1);
}

TEST(MotionArbiter_PointWithResumeCombatSuspends)
{
    Arbiter m;
    m.InstallDefault(Kind::Patrol);
    m.Request(Req(Kind::Chase));
    m.Request(Req(Kind::Point, 7, true));
    CHECK(m.Combat());
    m.ExpireSelected();
    CHECK_EQ(SelectedKind(m), K(Kind::Chase));
}

TEST(MotionArbiter_PointSupersedesPoint)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Point, 1));
    m.Request(Req(Kind::Point, 2));
    CHECK_EQ(Size(m), 2);                                             // default + one point
    CHECK_EQ(static_cast<int>(m.Command(Layer::Scripted)->id), 2);
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Point, 1, FinishReason::Superseded));
}

TEST(MotionArbiter_FearSuspendsPoint_PointResumes)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Point, 3));
    m.Request(Claim(Kind::Fear, 1));
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    CHECK(m.Command(Layer::Scripted));                                // still held, masked
    m.CancelControl(Kind::Fear);                                      // D3: by identity
    CHECK_EQ(SelectedKind(m), K(Kind::Point));
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Resumed, Kind::Point), 1);
}

TEST(MotionArbiter_PointUnderFearWaits)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 1));
    m.Request(Req(Kind::Point, 4));
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));                         // D1: layer order, not push order
    CHECK(m.Command(Layer::Scripted));
}

TEST(MotionArbiter_EffectKeepsCombat_EffectSupersedesEffect)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.Request(Req(Kind::Effect, 10));
    CHECK(m.Combat());
    m.Request(Req(Kind::Effect, 11));                                 // knockback during a knockback
    CHECK(m.Combat());
    CHECK_EQ(static_cast<int>(m.Command(Layer::Forced)->id), 11);
    m.ExpireSelected();                                               // lands
    CHECK_EQ(SelectedKind(m), K(Kind::Chase));
}

TEST(MotionArbiter_SelfExpiringCancelledByAnyRequest)
{
    // MotionMaster::Mutate expires a HOME/DISTRACT/EFFECT top before pushing.
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Home));
    m.Request(Req(Kind::Chase));                                      // aggro on the way home
    CHECK(!m.Command(Layer::Scripted));
    CHECK_EQ(SelectedKind(m), K(Kind::Chase));
    m.Request(Req(Kind::Distract));
    m.Request(Req(Kind::Point, 5));
    CHECK(!m.Command(Layer::Distract));
    CHECK_EQ(SelectedKind(m), K(Kind::Point));
}

TEST(MotionArbiter_TaxiCancelsScriptedAndCombat_KeepsControlAndDefault)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Confused, 1));
    m.Request(Req(Kind::Point, 6));
    m.Request(Req(Kind::Chase));
    m.Request(Req(Kind::Taxi));
    CHECK_EQ(SelectedKind(m), K(Kind::Taxi));
    CHECK(!m.Command(Layer::Scripted));
    CHECK(!m.Combat());
    CHECK(m.Command(Layer::Control));                                 // D8
    CHECK(m.Default());
    m.ExpireSelected();                                               // landed
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));
}

TEST(MotionArbiter_FollowIsDefault_FallbackOnTargetLost)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Clear(false);                                                   // MoveFollow does Clear() first
    m.Request(Req(Kind::Follow));
    CHECK_EQ(SelectedKind(m), K(Kind::Follow));
    CHECK_EQ(Size(m), 1);
    m.ExpireSelected();                                               // Follow::Update returns false: target gone
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));                       // retained fallback default
}

TEST(MotionArbiter_IdleOnNonEmptyIsMaskingCommand)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.Clear(false);                                                   // possession: Clear(false) + MoveIdle
    m.Request(Req(Kind::Idle));
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
    CHECK_EQ(K(m.Default()->kind), K(Kind::Wander));                  // default untouched beneath
    m.ExpireSelected();                                               // MovementExpired
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));
}

TEST(MotionArbiter_ClearProjections)
{
    Arbiter m;
    m.InstallDefault(Kind::Patrol);
    m.Request(Req(Kind::Chase));
    m.Request(Req(Kind::Point, 8));
    m.Request(Claim(Kind::Fear, 1));
    m.Clear(false);                                                   // everything but the bottom and the claims
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    CHECK_EQ(Size(m), 2);
    m.Clear(true);                                                    // the bottom and the claims too
    CHECK(m.Empty());
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Patrol), 1);
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Fear), 1);
}

TEST(MotionArbiter_ExpireAtDepthOneIsNoOp)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.ExpireSelected();
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));
    CHECK_EQ(static_cast<int>(m.DrainEvents().size()), 0);
}

TEST(MotionArbiter_LowerRequestWhileHigherCommandRuns_NoResumeEvent)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 1));
    m.DrainEvents();
    m.Request(Req(Kind::Chase));
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(static_cast<int>(ev.size()), 0);                         // stored masked, nothing suspended or resumed
    CHECK(m.Combat());
}

TEST(MotionArbiter_IdleOverPointSupersedesIt)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Point, 9));
    m.DrainEvents();
    m.Request(Req(Kind::Idle));
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
    CHECK_EQ(K(m.Command(Layer::Scripted)->kind), K(Kind::Idle));
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Point, 9, FinishReason::Superseded));
    m.ExpireSelected();                                               // the idle expires
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));                       // the default, never the stale point
}

TEST(MotionArbiter_IdleTwiceIsNoOp)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Idle));
    m.DrainEvents();
    m.Request(Req(Kind::Idle));
    CHECK_EQ(static_cast<int>(m.DrainEvents().size()), 0);
    CHECK_EQ(Size(m), 2);
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
}

TEST(MotionArbiter_ExpireKindFinishesMaskedEntryOnly)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 1));
    m.Request(Req(Kind::Point, 4));                                   // masked beneath the fear
    m.DrainEvents();
    m.Expire(Kind::Point);                                            // the stack's point expired
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));                         // the fear is untouched
    CHECK(!m.Command(Layer::Scripted));
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Finished, Kind::Point), 1);
}

TEST(MotionArbiter_ExpireKindNotHeldIsNoOp)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.DrainEvents();
    m.Expire(Kind::Point);                                            // a stale point the model never held
    CHECK_EQ(SelectedKind(m), K(Kind::Chase));
    CHECK_EQ(static_cast<int>(m.DrainEvents().size()), 0);
}

TEST(MotionArbiter_ExpireKindPrefersIdleCommandOverDefault)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Idle));                                       // MoveIdle on a non-empty stack
    m.Expire(Kind::Idle);
    CHECK(!m.Command(Layer::Scripted));
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));                         // the default remains
}

TEST(MotionArbiter_ExpireKindFollowRestoresFallback)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Clear(false);
    m.Request(Req(Kind::Follow));
    m.Expire(Kind::Follow);
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));
}

TEST(MotionArbiter_PushedWanderClearedRevealsFactoryDefault)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Wander));                                     // EventAI change-movement: pushed over the factory default
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));
    m.Clear(false);                                                   // evade: the stack pops it, the factory default resumes
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
    CHECK_EQ(Size(m), 1);
}

TEST(MotionArbiter_FollowAfterPushedPatrolKeepsFactoryFallback)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Patrol));
    m.Clear(false);                                                   // MoveFollow clears first
    m.Request(Req(Kind::Follow));
    CHECK_EQ(SelectedKind(m), K(Kind::Follow));
    m.Expire(Kind::Follow);                                           // target gone
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));                         // the factory default, not the popped waypoint
}

TEST(MotionArbiter_ExpirePushedDefaultPromotesFallback)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Wander));
    m.ExpireSelected();                                               // MovementExpired at depth two pops the pushed default
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
    m.ExpireSelected();                                               // and at depth one it is a no-op
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
}

TEST(MotionArbiter_Claims_FearThenConfuse_ConfuseSelected_FearSuspendedNotFinished)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11));
    m.DrainEvents();
    m.Request(Claim(Kind::Confused, 22));
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));
    CHECK_EQ(static_cast<int>(m.Claims().size()), 2);
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Suspended, Kind::Fear), 1);
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Fear), 0);
}

TEST(MotionArbiter_Claims_ReleaseSelectedConfuse_FearResumes)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Confused, 22));
    m.DrainEvents();
    m.Release(22);
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    CHECK_EQ(static_cast<int>(m.Claims().size()), 1);
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Confused, 0, FinishReason::Cancelled));
    CHECK_EQ(CountEvents(ev, Event::Kind::Resumed, Kind::Fear), 1);
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Fear), 0);
}

TEST(MotionArbiter_Claims_ReleaseUnselectedFear_NoSelectionEvent)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Confused, 22));
    m.DrainEvents();
    m.Release(11);
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Fear, 0, FinishReason::Cancelled));
    CHECK_EQ(CountEvents(ev, Event::Kind::Suspended, Kind::Confused), 0);
    CHECK_EQ(CountEvents(ev, Event::Kind::Resumed, Kind::Confused), 0);
}

TEST(MotionArbiter_Claims_SameIdentityUpdatesInPlace)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11, 1));
    const uint32 firstSeq = m.Claims()[0].seq;
    m.DrainEvents();
    m.Request(Claim(Kind::Fear, 11, 2));
    CHECK_EQ(static_cast<int>(m.Claims().size()), 1);
    CHECK_EQ(static_cast<int>(m.Claims()[0].id), 2);
    CHECK(m.Claims()[0].seq != firstSeq);
    CHECK_EQ(static_cast<int>(m.DrainEvents().size()), 0);
}

TEST(MotionArbiter_Claims_TaxiMasksAllClaims_ResumeAfterTaxi)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Confused, 22));
    m.DrainEvents();
    m.Request(Req(Kind::Taxi));
    CHECK_EQ(SelectedKind(m), K(Kind::Taxi));
    CHECK_EQ(static_cast<int>(m.Claims().size()), 2);
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Suspended, Kind::Confused), 1);
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Fear), 0);
    m.ExpireSelected();                                               // landed
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Resumed, Kind::Confused), 1);
}

TEST(MotionArbiter_Claims_CancelControlReleasesEveryClaimOfKind)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Fear, 12));
    m.Request(Claim(Kind::Confused, 22));
    m.DrainEvents();
    m.CancelControl(Kind::Fear);
    CHECK_EQ(static_cast<int>(m.Claims().size()), 1);
    CHECK_EQ(K(m.Claims()[0].kind), K(Kind::Confused));
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Finished, Kind::Fear), 2);
    m.CancelControl(Kind::Confused);
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
}

TEST(MotionArbiter_Claims_ZeroClaimIgnored)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Fear));                                       // no identity: ignored
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
    CHECK_EQ(static_cast<int>(m.Claims().size()), 0);
    CHECK_EQ(static_cast<int>(m.DrainEvents().size()), 0);
}

TEST(MotionArbiter_Claims_ExpireKindFinishesNewestClaimOfKindOnly)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Fear, 12));
    m.Request(Claim(Kind::Confused, 22));
    m.DrainEvents();
    m.Expire(Kind::Fear);                                             // a timed fear ran out
    CHECK_EQ(static_cast<int>(m.Claims().size()), 2);
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Fear), 1);
    bool newest = false;
    for (Event const& e : ev)
    {
        if (e.kind == Event::Kind::Finished && e.claim == 12 && e.reason == FinishReason::Expired)
        {
            newest = true;
        }
    }
    CHECK(newest);
    m.Expire(Kind::Fear);
    m.Expire(Kind::Fear);                                             // none left: a no-op
    CHECK_EQ(static_cast<int>(m.Claims().size()), 1);
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Finished, Kind::Fear), 1);
}

TEST(MotionArbiter_Claims_ExpireSelectedLeavesASelectedClaim)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Point, 3));
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Confused, 22));
    m.DrainEvents();
    m.ExpireSelected();                                               // a generic expiry: not the confuse's own end
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Confused), 0);
    CHECK_EQ(static_cast<int>(m.Claims().size()), 2);
    m.Release(22);                                                    // the confuse's aura ends it
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Confused, 0, FinishReason::Cancelled));
    CHECK_EQ(CountEvents(ev, Event::Kind::Resumed, Kind::Fear), 1);
    m.FinishSelected(FinishReason::Cut);                              // the fear's own leg cut (the timed flee's end)
    CHECK_EQ(SelectedKind(m), K(Kind::Point));
    ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Fear, 0, FinishReason::Cut));
    CHECK_EQ(CountEvents(ev, Event::Kind::Resumed, Kind::Point), 1);
    CHECK_EQ(static_cast<int>(m.Claims().size()), 0);
}

TEST(MotionArbiter_Claims_ContentsCountLiveClaimsInPrecedenceOrder)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Confused, 22));
    m.Request(Claim(Kind::Fear, 13));
    CHECK_EQ(Size(m), 5);                                             // default, combat, three claims
    std::vector<Held> claims = m.Claims();
    REQUIRE(static_cast<int>(claims.size()) == 3);
    CHECK_EQ(K(claims[0].kind), K(Kind::Confused));
    CHECK_EQ(static_cast<int>(claims[1].claim), 13);                  // the newer fear before the older
    CHECK_EQ(static_cast<int>(claims[2].claim), 11);
    std::optional<Held> sel = m.Selected();
    REQUIRE(sel.has_value());
    CHECK_EQ(static_cast<int>(sel->claim), static_cast<int>(claims[0].claim));
    std::vector<Held> all = m.Contents();
    CHECK_EQ(K(all[2].kind), K(Kind::Confused));                      // the claims sit after default and combat
}

TEST(MotionArbiter_Claims_DefaultOverrideLeavesClaims)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Chase));
    m.Request(Claim(Kind::Fear, 11));
    m.DrainEvents();
    m.Request(Req(Kind::Wander));                                     // an Override default under a fear
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    CHECK(!m.Combat());                                               // combat overridden
    CHECK_EQ(static_cast<int>(m.Claims().size()), 1);                 // the claim untouched
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Fear), 0);
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Chase), 1);
}

TEST(MotionArbiter_EventRow_CombatStartedCancelsDistractAndAssistDistract)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Distract));
    m.DrainEvents();
    m.Notify(ExternalEvent::CombatStarted);
    CHECK(!m.Command(Layer::Distract));
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Distract, 0, FinishReason::Cancelled));
    m.Request(Req(Kind::AssistDistract));
    m.DrainEvents();
    m.Notify(ExternalEvent::CombatStarted);
    CHECK(HasFinished(m.DrainEvents(), Kind::AssistDistract, 0, FinishReason::Cancelled));
}

TEST(MotionArbiter_EventRow_CombatStartedLeavesOtherLayers)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Point, 3));
    m.Request(Claim(Kind::Fear, 1));
    m.Request(Req(Kind::Chase));
    m.DrainEvents();
    m.Notify(ExternalEvent::CombatStarted);
    CHECK(m.Command(Layer::Scripted));
    CHECK(m.Command(Layer::Control));
    CHECK(m.Combat());
    CHECK_EQ(static_cast<int>(m.DrainEvents().size()), 0);
}

TEST(MotionArbiter_Generations_RequestDuringNormalCompletionSurvives)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::AssistRun));
    {
        Transaction tx(m, TransactionKind::Normal);   // the shell delivering AssistRun's finish
        m.FinishSelected(FinishReason::Arrived);
        m.Request(Req(Kind::AssistDistract));         // the finalizer's request
        CHECK_EQ(SelectedKind(m), K(Kind::AssistDistract));
    }
    CHECK_EQ(SelectedKind(m), K(Kind::AssistDistract));
    CHECK(m.Command(Layer::Distract));
}

TEST(MotionArbiter_Generations_RequestDuringClearAllIsDiscardedAtCommit)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::AssistRun));
    m.DrainEvents();
    {
        Transaction tx(m, TransactionKind::ClearAll);
        m.Clear(true);
        m.Request(Req(Kind::AssistDistract));         // AssistRun::Finalize -> MoveSeekAssistanceDistract
        CHECK_EQ(SelectedKind(m), K(Kind::AssistDistract));   // visible during the hook
        CHECK(m.InDiscardingTransaction());
    }
    CHECK(m.Empty());                                 // gone at commit, as DirectClean keeps clearing
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::AssistRun, 0, FinishReason::Cleared));
    CHECK(HasFinished(ev, Kind::AssistDistract, 0, FinishReason::Cleared));
    CHECK(!m.InDiscardingTransaction());
}

TEST(MotionArbiter_Generations_RequestDuringClearDropsDefaultAndCommand)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Point, 5));
    m.DrainEvents();
    {
        Transaction tx(m, TransactionKind::Clear);
        m.Clear(false);
        m.Request(Req(Kind::Patrol));                 // a finalizer swapping the default: popped, as DirectClean pops it
        m.Request(Req(Kind::Point, 6));               // a finalizer's point: dropped
        CHECK_EQ(SelectedKind(m), K(Kind::Point));    // visible while the hook runs
    }
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));         // the factory default beneath
    CHECK_EQ(Size(m), 1);
    CHECK(!m.Command(Layer::Scripted));
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Point, 6, FinishReason::Cleared));
    CHECK(HasFinished(ev, Kind::Patrol, 0, FinishReason::Cleared));
}

TEST(MotionArbiter_Generations_NestedTransactionJoinsOutermost)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    const uint32 g0 = m.Generation();
    {
        Transaction outer(m, TransactionKind::Death);
        const uint32 g1 = m.Generation();
        CHECK(g1 != g0);
        {
            Transaction inner(m, TransactionKind::Normal);
            CHECK_EQ(static_cast<int>(m.Generation()), static_cast<int>(g1));
            CHECK(m.InDiscardingTransaction());       // the outermost decides
            m.Request(Req(Kind::Chase));
        }
        CHECK(m.Combat());                            // the inner commit swept nothing
    }
    CHECK(!m.Combat());                               // the outer one did
    CHECK(HasFinished(m.DrainEvents(), Kind::Chase, 0, FinishReason::Died));
}

TEST(MotionArbiter_Death_FinishesEverythingDied_ModelEmpty)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Patrol));                     // pushed default over the factory one
    m.Request(Req(Kind::Chase));
    m.Request(Req(Kind::Point, 7, true));             // resumeCombat: the chase stays beneath
    m.Request(Claim(Kind::Fear, 1));
    m.Request(Req(Kind::Effect, 8));
    m.DrainEvents();
    m.Die();
    CHECK(m.Empty());
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Patrol, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Idle, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Chase, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Point, 7, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Fear, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Effect, 8, FinishReason::Died));
    CHECK_EQ(CountEvents(ev, Event::Kind::Suspended, Kind::Effect), 0);
    CHECK_EQ(CountEvents(ev, Event::Kind::Resumed, Kind::Chase), 0);
}

TEST(MotionArbiter_Death_RequestDuringDeathDiscarded)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.DrainEvents();
    {
        Transaction tx(m, TransactionKind::Death);
        m.Die();
        m.Request(Req(Kind::Chase));                  // a finalizer re-engaging while still alive
        CHECK(m.Combat());
        m.Request(Req(Kind::Follow));                 // and a default request that overrides nothing
        CHECK(m.Default());
    }
    CHECK(m.Empty());
    std::vector<Event> ev = m.DrainEvents();
    int chaseDied = 0;
    for (Event const& e : ev)
    {
        if (e.kind == Event::Kind::Finished && e.who == Kind::Chase && e.reason == FinishReason::Died)
        {
            ++chaseDied;
        }
    }
    CHECK_EQ(chaseDied, 2);                           // once by Die, once by the sweep
    CHECK(HasFinished(ev, Kind::Follow, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Wander, 0, FinishReason::Died));
}

TEST(MotionArbiter_Death_TwoDefaultRequestsDuringDeathBothDiscarded)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.DrainEvents();
    {
        Transaction tx(m, TransactionKind::Death);
        m.Die();
        m.Request(Req(Kind::Wander));                 // a finalizer's default
        m.Request(Req(Kind::Patrol));                 // and another, retaining the first as fallback
        CHECK_EQ(SelectedKind(m), K(Kind::Patrol));
    }
    CHECK(m.Empty());
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Patrol, 0, FinishReason::Died));
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Wander), 2);
}

TEST(MotionArbiter_Death_NestedInNormalStillDiscards)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.DrainEvents();
    {
        Transaction outer(m, TransactionKind::Normal);   // the shell delivering a completion
        m.Die();                                          // the hook killed the unit
        CHECK(m.InDiscardingTransaction());
        m.Request(Req(Kind::Chase));                      // a finalizer re-engaging while still alive
        CHECK(m.Combat());
    }
    CHECK(m.Empty());
    CHECK(!m.InDiscardingTransaction());
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Chase, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Wander, 0, FinishReason::Died));
}

TEST(MotionArbiter_Ring_RecordsBeforeAndAfter)
{
    Arbiter m;
    m.EnableRing();
    CHECK_EQ(static_cast<int>(m.Decisions().size()), 0);
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Point, 4));
    std::vector<Decision> d = m.Decisions();
    REQUIRE(static_cast<int>(d.size()) == 2);
    CHECK_EQ(static_cast<int>(d[0].op), static_cast<int>(Decision::Op::InstallDefault));
    CHECK(!d[0].hadBefore);
    CHECK(d[0].hadAfter);
    CHECK_EQ(K(d[0].after.kind), K(Kind::Wander));
    CHECK_EQ(static_cast<int>(d[1].op), static_cast<int>(Decision::Op::Request));
    CHECK_EQ(K(d[1].kind), K(Kind::Point));
    CHECK_EQ(static_cast<int>(d[1].id), 4);
    CHECK(d[1].hadBefore);
    CHECK_EQ(K(d[1].before.kind), K(Kind::Wander));
    CHECK_EQ(K(d[1].after.kind), K(Kind::Point));
    CHECK(d[1].generation > d[0].generation);
    m.Die();
    d = m.Decisions();
    CHECK_EQ(static_cast<int>(d.back().op), static_cast<int>(Decision::Op::Die));
    CHECK(!d.back().hadAfter);
}

TEST(MotionArbiter_Ring_WrapsAt32)
{
    Arbiter m;
    m.EnableRing();
    m.InstallDefault(Kind::Wander);
    for (uint32 i = 1; i <= 40; ++i)
    {
        m.Request(Req(Kind::Point, i));
    }
    std::vector<Decision> d = m.Decisions();
    CHECK_EQ(static_cast<int>(d.size()), static_cast<int>(Arbiter::kRingSize));
    CHECK_EQ(static_cast<int>(d.front().id), 9);      // 41 entries recorded, the oldest 9 fell out
    CHECK_EQ(static_cast<int>(d.back().id), 40);
}

TEST(MotionArbiter_Generations_LaterDiscardingGuardSweepsOnlyItsOwn)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    {
        Transaction tx(m, TransactionKind::Clear);
        m.Request(Req(Kind::Patrol));                 // doomed, popped at this commit
    }
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
    m.DrainEvents();
    {
        Transaction tx(m, TransactionKind::ClearAll); // a later discarding guard that clears nothing itself
        m.Request(Req(Kind::Point, 1));
    }
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));         // the factory default was never this guard's to sweep
    CHECK(!m.Empty());
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Point, 1, FinishReason::Cleared));
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Idle), 0);
}

TEST(MotionArbiter_Generations_NestedClearDoomsWhileOpen)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.DrainEvents();
    {
        Transaction outer(m, TransactionKind::Normal);   // a hook's facade call inside a normal completion
        {
            Transaction clear(m, TransactionKind::Clear); // the shell's Clear(false) scope
            m.Clear(false);
            CHECK(m.InDiscardingTransaction());
            m.Request(Req(Kind::Chase, 7));               // a finalizer re-engaging while the clear is open
            CHECK(m.Combat());
        }
        CHECK(!m.InDiscardingTransaction());              // the nested clear closed: the outer kind is Normal again
    }
    CHECK(!m.Combat());                                    // swept at the outermost commit
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Chase, 0, FinishReason::Cleared));
    CHECK(HasFinished(ev, Kind::Chase, 7, FinishReason::Cleared));
    CHECK(m.Default() && m.Default()->kind == Kind::Wander);
}

TEST(MotionArbiter_Generations_RequestAfterNestedClearSurvives)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.DrainEvents();
    {
        Transaction outer(m, TransactionKind::Normal);   // MoveTargetedHome's own scope
        {
            Transaction clear(m, TransactionKind::Clear);
            m.Clear(false);
        }
        m.Request(Req(Kind::Home));                       // the home requested after the clear closed
        CHECK(!m.InDiscardingTransaction());
    }
    CHECK(m.Selected() && m.Selected()->kind == Kind::Home);
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Chase, 0, FinishReason::Cleared));
    CHECK(!HasFinished(ev, Kind::Home, 0, FinishReason::Cleared));
}

TEST(MotionArbiter_Generations_DeathInsideNestedClearStaysDeath)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.DrainEvents();
    {
        Transaction outer(m, TransactionKind::Normal);
        {
            Transaction clear(m, TransactionKind::Clear);
            m.Die();                                       // a finalizer killed the unit during the clear
        }
        CHECK(m.InDiscardingTransaction());                // Death is sticky past the clear's end
        m.Request(Req(Kind::Chase));                       // requested after the clear closed, still dead
    }
    CHECK(m.Empty());
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Wander, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Chase, 0, FinishReason::Died));
}

TEST(MotionArbiter_Generations_NestedClearAllInsideClearRevertsToClear)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.DrainEvents();
    {
        Transaction outer(m, TransactionKind::Normal);
        {
            Transaction clear(m, TransactionKind::Clear);
            {
                Transaction all(m, TransactionKind::ClearAll);
                m.Clear(true);                             // the default goes too
                CHECK(m.Empty());
            }
            CHECK(m.InDiscardingTransaction());            // back to the enclosing clear, still discarding
            m.Request(Req(Kind::Point, 3));                // doomed by the clear that is still open
        }
        CHECK(!m.InDiscardingTransaction());
    }
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Wander, 0, FinishReason::Cleared));
    CHECK(HasFinished(ev, Kind::Point, 3, FinishReason::Cleared));
}

TEST(MotionArbiter_Death_InstallDefaultInsideDeathGuardSurvives)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.DrainEvents();
    {
        Transaction tx(m, TransactionKind::Death);
        m.Die();
        m.InstallDefault(Kind::Idle);                 // the shell's post-death idle, inside the death's own guard
    }
    CHECK(!m.Empty());
    CHECK_EQ(SelectedKind(m), K(Kind::Idle));
    std::vector<Event> ev = m.DrainEvents();
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Idle), 0);
    CHECK(HasFinished(ev, Kind::Wander, 0, FinishReason::Died));
    CHECK(HasFinished(ev, Kind::Chase, 0, FinishReason::Died));
}

TEST(MotionArbiter_Death_DieTwiceInOneGuardIsIdempotent)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.DrainEvents();
    {
        Transaction tx(m, TransactionKind::Death);
        m.Die();
        m.Die();
    }
    CHECK(m.Empty());
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Finished, Kind::Wander), 1);
}

TEST(MotionArbiter_Death_EventsInAscendingLayerOrder)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Chase));
    m.Request(Req(Kind::Point, 7, true));
    m.Request(Claim(Kind::Fear, 1));
    m.Request(Req(Kind::Effect, 8));
    m.DrainEvents();
    m.Die();
    std::vector<Event> ev = m.DrainEvents();
    std::vector<Kind> order;
    for (Event const& e : ev)
    {
        if (e.kind == Event::Kind::Finished)
        {
            order.push_back(e.who);
        }
    }
    REQUIRE(static_cast<int>(order.size()) == 5);
    CHECK_EQ(K(order[0]), K(Kind::Idle));
    CHECK_EQ(K(order[1]), K(Kind::Chase));
    CHECK_EQ(K(order[2]), K(Kind::Point));
    CHECK_EQ(K(order[3]), K(Kind::Fear));
    CHECK_EQ(K(order[4]), K(Kind::Effect));
}

TEST(MotionArbiter_Claims_ClearLeavesClaims_ClearAllFinishesThemInPrecedenceOrder)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 11));
    m.Request(Claim(Kind::Confused, 22));
    m.Request(Claim(Kind::Fear, 13));
    m.DrainEvents();
    m.Clear(false);                                                   // a script's clear: the claims are the auras', not its
    CHECK_EQ(static_cast<int>(m.Claims().size()), 3);
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));
    std::vector<Event> ev = m.DrainEvents();
    for (Event const& e : ev)
    {
        CHECK(!(e.kind == Event::Kind::Finished && e.claim != 0));
    }
    CHECK(m.HasClaim(Kind::Fear));
    CHECK(m.HasClaim(Kind::Confused));
    m.Clear(true);                                                    // the full reset takes them, Confused first, then the newer fear
    CHECK(!m.HasClaim(Kind::Fear));
    CHECK(!m.HasClaim(Kind::Confused));
    ev = m.DrainEvents();
    std::vector<uint64> order;
    for (Event const& e : ev)
    {
        if (e.kind == Event::Kind::Finished && e.claim != 0)
        {
            order.push_back(e.claim);
        }
    }
    REQUIRE(static_cast<int>(order.size()) == 3);
    CHECK_EQ(static_cast<int>(order[0]), 22);
    CHECK_EQ(static_cast<int>(order[1]), 13);
    CHECK_EQ(static_cast<int>(order[2]), 11);
}

/// HasAuraClaim tells a fear AURA from the AI's own low-health flee, which takes a Fear claim
/// with the same kind and the same reason but no spell in its identity (Motion::ClaimSpell = 0,
/// the shape Creature::DoFleeToGetAssistance and a script's fear both pass). Unit::UpdateSpeed
/// gates retail's x1.25 on this, so a claim that answers the wrong way is a mob running at the
/// wrong speed. It follows the claims: it goes when the last aura claim goes, whichever way it
/// goes -- a release, a full clear or the death -- which is the whole reason the answer lives
/// on the claim and not on a flag somebody has to remember to unset.
TEST(MotionArbiter_Claims_AnAuraClaimIsToldFromTheAIsOwnFlee)
{
    const uint64 auraFear = ControlClaimShape(5782, 0, 7);   // the warlock's Fear from caster 7
    const uint64 aiFlee   = ControlClaimShape(0, 0, 9);      // DoFleeToGetAssistance: the victim, no spell
    CHECK_EQ(ClaimSpell(auraFear), 5782u);
    CHECK_EQ(ClaimSpell(aiFlee), 0u);

    Arbiter m;
    m.InstallDefault(Kind::Idle);
    CHECK(!m.HasAuraClaim(Kind::Fear));
    m.Request(Claim(Kind::Fear, aiFlee));
    CHECK(m.HasClaim(Kind::Fear));
    CHECK(!m.HasAuraClaim(Kind::Fear));                      // the reason is held; the aura is not
    m.Request(Claim(Kind::Fear, auraFear));                  // an aura lands on top of the AI's flee
    CHECK(m.HasAuraClaim(Kind::Fear));
    CHECK(!m.HasAuraClaim(Kind::Confused));                  // the kind is honoured
    CHECK(m.Release(auraFear));                              // the aura goes, the AI's flee survives
    CHECK(m.HasClaim(Kind::Fear));
    CHECK(!m.HasAuraClaim(Kind::Fear));                      // and the quarter goes with the aura
    m.Request(Claim(Kind::Fear, auraFear));
    CHECK(m.HasAuraClaim(Kind::Fear));
    m.Clear(true);                                           // a full reset takes the claims with it
    CHECK(!m.HasAuraClaim(Kind::Fear));
    m.DrainEvents();

    // The other ordering, which is the one that tells "any HELD aura claim" from "the SELECTED
    // claim": the aura first, then a NEWER AI flee, which wins the selection because the newest
    // claim of a kind outranks the older. An implementation that read only the selection would
    // answer false here and pass every assertion above.
    Arbiter n;
    n.InstallDefault(Kind::Idle);
    n.Request(Claim(Kind::Fear, auraFear));
    n.Request(Claim(Kind::Fear, aiFlee));
    REQUIRE(n.Selected().has_value());
    CHECK_EQ(static_cast<int>(n.Selected()->claim), static_cast<int>(aiFlee));   // the AI's flee drives
    CHECK(n.HasAuraClaim(Kind::Fear));                       // and the aura is still HELD beneath it
    CHECK(n.Release(aiFlee));                                // the flee ends, the aura drives again
    CHECK(n.HasAuraClaim(Kind::Fear));
    CHECK(n.Release(auraFear));                              // only the aura's own release ends it
    CHECK(!n.HasAuraClaim(Kind::Fear));
    CHECK(!n.HasClaim(Kind::Fear));
    n.DrainEvents();
}

TEST(MotionArbiter_Claims_ClearUnderAClaimKeepsItSelected_ReleaseResumesTheDefault)
{
    Arbiter m;
    m.InstallDefault(Kind::Patrol);
    m.Request(Req(Kind::Chase));
    m.Request(Claim(Kind::Fear, 11));
    m.DrainEvents();
    m.Clear(false);                                                   // a script's clear under the fear
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Chase, 0, FinishReason::Cleared));
    CHECK_EQ(CountEvents(ev, Event::Kind::Finished, Kind::Fear), 0);
    m.Request(Req(Kind::Point, 5));                                   // the script's point waits beneath the fear
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    m.DrainEvents();
    CHECK(!m.Release(12345));                                         // unknown identity: no claim went, selection unchanged
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    CHECK(m.Release(11));                                             // the fear's aura ends: the point runs
    CHECK_EQ(SelectedKind(m), K(Kind::Point));
    ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Fear, 0, FinishReason::Cancelled));
}

TEST(MotionArbiter_InstallDefault_DropsFallback)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Patrol));                     // pushed: Idle retained beneath
    m.InstallDefault(Kind::Wander);                   // the factory default changes: the fallback goes
    CHECK_EQ(Size(m), 1);
    m.Clear(false);                                   // nothing to pop onto
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));
    CHECK_EQ(Size(m), 1);
}

TEST(MotionArbiter_Ring_CommitRecordsOnlyWhenItSwept)
{
    Arbiter m;
    m.EnableRing();
    m.InstallDefault(Kind::Idle);
    {
        Transaction tx(m, TransactionKind::ClearAll);
        m.Request(Req(Kind::Point, 1));
    }
    std::vector<Decision> d = m.Decisions();
    REQUIRE(static_cast<int>(d.size()) == 3);         // InstallDefault, Request, Commit
    CHECK_EQ(static_cast<int>(d.back().op), static_cast<int>(Decision::Op::Commit));
    {
        Transaction tx(m, TransactionKind::ClearAll); // nothing created, nothing swept, nothing recorded
    }
    CHECK_EQ(static_cast<int>(m.Decisions().size()), 3);
}

TEST(MotionArbiter_Ring_RefusedClaimStillRecorded)
{
    Arbiter m;
    m.EnableRing();
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Fear));                       // no identity: refused
    std::vector<Decision> d = m.Decisions();
    REQUIRE(static_cast<int>(d.size()) == 2);
    CHECK_EQ(static_cast<int>(d.back().op), static_cast<int>(Decision::Op::Request));
    CHECK_EQ(K(d.back().kind), K(Kind::Fear));
    CHECK_EQ(K(d.back().after.kind), K(Kind::Idle));
}

TEST(MotionArbiter_Ring_ExpireSelectedKeepsItsLabel)
{
    Arbiter m;
    m.EnableRing();
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Chase));
    m.ExpireSelected();                               // delegates to the finisher, records as itself
    std::vector<Decision> d = m.Decisions();
    REQUIRE(static_cast<int>(d.size()) == 3);
    CHECK_EQ(static_cast<int>(d.back().op), static_cast<int>(Decision::Op::ExpireSelected));
    CHECK_EQ(K(d.back().kind), K(Kind::Chase));
}

/// The shell's own refusal (P5-B family 1: a knockback arc on a rooted unit): the ring
/// carries the line, the model holds nothing and nothing was stamped.
TEST(MotionArbiter_RefuseHoldsNothing)
{
    Arbiter m;
    m.EnableRing();
    m.Refuse(Req(Kind::Effect, 9));
    CHECK_EQ(Size(m), 0);
    std::vector<Decision> d = m.Decisions();
    REQUIRE(static_cast<int>(d.size()) == 1);
    CHECK_EQ(static_cast<int>(d.back().op), static_cast<int>(Decision::Op::Refused));
    CHECK_EQ(K(d.back().kind), K(Kind::Effect));
    CHECK_EQ(static_cast<int>(d.back().id), 9);
    CHECK(!d.back().hadAfter);
}

TEST(MotionArbiter_Ring_NotifyRecordsTheEvent)
{
    Arbiter m;
    m.EnableRing();
    m.InstallDefault(Kind::Wander);
    m.Notify(ExternalEvent::CombatStarted);
    std::vector<Decision> d = m.Decisions();
    REQUIRE(static_cast<int>(d.size()) == 2);
    CHECK_EQ(static_cast<int>(d.back().op), static_cast<int>(Decision::Op::Notify));
    CHECK_EQ(static_cast<int>(d.back().id), static_cast<int>(ExternalEvent::CombatStarted));
}

TEST(MotionArbiter_ClearAllFinishesParkedFactoryDefault)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Patrol));                     // Idle parked beneath
    m.DrainEvents();
    m.Clear(true);
    CHECK(m.Empty());
    std::vector<Event> ev = m.DrainEvents();
    CHECK(HasFinished(ev, Kind::Patrol, 0, FinishReason::Cleared));
    CHECK(HasFinished(ev, Kind::Idle, 0, FinishReason::Cleared));
}

TEST(MotionArbiter_Reselect_NeverRunEntryIsNotResumed)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Claim(Kind::Fear, 1));
    m.Request(Req(Kind::Point, 3));                   // masked under the fear, never ran
    m.Request(Req(Kind::Chase));                      // masked too, and newer
    m.DrainEvents();
    m.Release(1);
    CHECK_EQ(SelectedKind(m), K(Kind::Point));
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Resumed, Kind::Point), 0);   // a first start, not a resume
    m.ExpireSelected();                               // the point arrives
    CHECK_EQ(SelectedKind(m), K(Kind::Chase));
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Resumed, Kind::Chase), 0);   // never ran either
    m.Request(Req(Kind::Point, 4, true));             // a point over the running chase, keeping it
    m.DrainEvents();
    m.ExpireSelected();
    CHECK_EQ(SelectedKind(m), K(Kind::Chase));
    CHECK_EQ(CountEvents(m.DrainEvents(), Event::Kind::Resumed, Kind::Chase), 1);   // it had run: resumed
}

TEST(MotionArbiter_Event_CarriesTheEntrySeq)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Point, 4));
    const uint32 pointSeq = m.Command(Layer::Scripted)->seq;
    const uint32 wanderSeq = m.Default()->seq;
    m.DrainEvents();
    m.ExpireSelected();
    std::vector<Event> ev = m.DrainEvents();
    bool finished = false;
    bool resumed = false;
    for (Event const& e : ev)
    {
        if (e.kind == Event::Kind::Finished && e.who == Kind::Point)
        {
            finished = e.seq == pointSeq;
        }
        if (e.kind == Event::Kind::Resumed && e.who == Kind::Wander)
        {
            resumed = e.seq == wanderSeq;
        }
    }
    CHECK(finished);
    CHECK(resumed);
    CHECK_EQ(static_cast<int>(m.LastSeq()), static_cast<int>(pointSeq));
}

TEST(MotionArbiter_Fallback_IsVisible)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    CHECK(!m.Fallback());
    m.Request(Req(Kind::Patrol));
    REQUIRE(m.Fallback().has_value());
    CHECK_EQ(K(m.Fallback()->kind), K(Kind::Idle));
    CHECK(!m.HasEvents() == false);                   // the swap queued an event
    m.DrainEvents();
    CHECK(!m.HasEvents());
    m.Clear(false);
    CHECK(!m.Fallback());
}

TEST(MotionArbiter_Holds_AnswersWithoutContents)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    const uint32 idleSeq = m.Default()->seq;
    CHECK(m.Holds(idleSeq));
    CHECK(!m.Holds(idleSeq + 100));                   // never handed out
    CHECK(!m.Holds(0));
    m.Request(Req(Kind::Patrol));
    const uint32 patrolSeq = m.Default()->seq;
    REQUIRE(m.Fallback().has_value());
    CHECK(m.Holds(idleSeq));                          // the parked fallback, as Contents() never lists it
    CHECK(m.Holds(patrolSeq));
    m.Request(Req(Kind::Point, 7));
    const uint32 pointSeq = m.Command(Layer::Scripted)->seq;
    CHECK(m.Holds(pointSeq));
    m.Request(Claim(Kind::Fear, 11));
    const uint32 fearSeq = m.Claims()[0].seq;
    CHECK(m.Holds(fearSeq));
    m.FinishSelected(FinishReason::Expired);
    CHECK(!m.Holds(fearSeq));                         // finished: gone from the claim set
    CHECK(m.Holds(pointSeq));
}

TEST(MotionArbiter_Ring_OffByDefault_OnWhenEnabled)
{
    Arbiter m;
    CHECK(!m.RingEnabled());
    m.InstallDefault(Kind::Wander);
    m.Request(Req(Kind::Point, 1));
    CHECK_EQ(static_cast<int>(m.Decisions().size()), 0);
    m.EnableRing();
    CHECK(m.RingEnabled());
    m.Request(Req(Kind::Point, 2));
    std::vector<Decision> d = m.Decisions();
    REQUIRE(static_cast<int>(d.size()) == 1);
    CHECK_EQ(static_cast<int>(d[0].id), 2);
    CHECK_STR(OpName(d[0].op), "Request");
    m.EnableRing();                                   // idempotent: keeps what it has
    CHECK_EQ(static_cast<int>(m.Decisions().size()), 1);
}

namespace
{
    const uint64 ROOT_SRC = InhibitSource(SourceDomain::Aura, 9, 339);
    const uint64 STUN_SRC = InhibitSource(SourceDomain::Aura, 9, 853);
    const uint64 ROOT_SRC2 = InhibitSource(SourceDomain::Seat, 3, 1);

    int CountBlocked(std::vector<Event> const& events, Event::Kind kind, Kind who)
    {
        int n = 0;
        for (Event const& e : events)
        {
            if (e.kind == kind && e.who == who && e.reason == FinishReason::Blocked)
            {
                ++n;
            }
        }
        return n;
    }
}

TEST(MotionArbiter_Block_RootPausesTheFearOnceAndResumesItOnce)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Claim(Kind::Fear, 0x5782));
    m.DrainEvents();
    const Held fear = *m.Selected();

    CHECK(m.Inhibit(Inhibition::Rooted, ROOT_SRC));
    std::vector<Event> events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Suspended, Kind::Fear), 1);
    CHECK_EQ(CountEvents(events, Event::Kind::Finished, Kind::Fear), 0);
    CHECK(!m.Evaluate().ticks);
    CHECK(!m.Evaluate().mayMove);
    CHECK(m.Evaluate().mayTurn);
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));            // the claim survives the root
    CHECK_EQ(int(m.Selected()->seq), int(fear.seq));

    CHECK(!m.Inhibit(Inhibition::Rooted, ROOT_SRC2));     // a second root: no edge, no second pause
    CHECK_EQ(CountBlocked(m.DrainEvents(), Event::Kind::Suspended, Kind::Fear), 0);
    CHECK(!m.Uninhibit(Inhibition::Rooted, ROOT_SRC));    // one root left: still paused
    CHECK_EQ(CountBlocked(m.DrainEvents(), Event::Kind::Resumed, Kind::Fear), 0);

    CHECK(m.Uninhibit(Inhibition::Rooted, ROOT_SRC2));    // the last root: the flee resumes once
    events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Resumed, Kind::Fear), 1);
    CHECK(m.Evaluate().mayMove);
    CHECK_EQ(int(m.Selected()->seq), int(fear.seq));
    CHECK(m.HasClaim(Kind::Fear));
}

TEST(MotionArbiter_Block_StunThenConfuseThenFearResumeInOrder)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Claim(Kind::Fear, 0x5782));
    m.Request(Claim(Kind::Confused, 0x118));
    m.DrainEvents();
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));          // Confused outranks Fear

    m.Inhibit(Inhibition::Stunned, STUN_SRC);
    std::vector<Event> events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Suspended, Kind::Confused), 1);
    CHECK(!m.Evaluate().mayTurn);                           // a stun keeps nothing
    CHECK_EQ(int(m.Evaluate().reasons & (ReasonFeared | ReasonConfused | ReasonStunned)), int(ReasonFeared | ReasonConfused | ReasonStunned));

    m.Uninhibit(Inhibition::Stunned, STUN_SRC);
    events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Resumed, Kind::Confused), 1);   // the wander plays again
    CHECK_EQ(SelectedKind(m), K(Kind::Confused));

    CHECK(m.Release(0x118));                                // the confuse ends first: the flee resumes
    events = m.DrainEvents();
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    CHECK_EQ(CountEvents(events, Event::Kind::Resumed, Kind::Fear), 1);
    CHECK_EQ(CountBlocked(events, Event::Kind::Suspended, Kind::Fear), 0);
    CHECK(m.Evaluate().mayMove);
}

TEST(MotionArbiter_Block_AClaimArrivingUnderARootStartsPaused)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.DrainEvents();
    m.Inhibit(Inhibition::Rooted, ROOT_SRC);
    std::vector<Event> events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Suspended, Kind::Wander), 1);

    m.Request(Claim(Kind::Fear, 0x5782));                   // a fear lands on a rooted unit
    events = m.DrainEvents();
    CHECK_EQ(SelectedKind(m), K(Kind::Fear));
    CHECK_EQ(CountBlocked(events, Event::Kind::Suspended, Kind::Fear), 1);   // paused at once
    CHECK(!m.Evaluate().mayMove);
    CHECK_EQ(CountEvents(events, Event::Kind::Finished, Kind::Fear), 0);

    m.Uninhibit(Inhibition::Rooted, ROOT_SRC);
    events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Resumed, Kind::Fear), 1);
    CHECK_EQ(CountBlocked(events, Event::Kind::Resumed, Kind::Wander), 0);   // the wander is masked, not selected
}

TEST(MotionArbiter_Block_TaxiRefusesControlAndDeathInhibits)
{
    Arbiter m;
    m.InstallDefault(Kind::Idle);
    m.Request(Req(Kind::Taxi, 7));
    m.DrainEvents();
    CHECK_EQ(SelectedKind(m), K(Kind::Taxi));

    m.Request(Claim(Kind::Fear, 0x5782));                   // refused at the door: no claim
    CHECK(!m.HasClaim(Kind::Fear));
    CHECK_EQ(SelectedKind(m), K(Kind::Taxi));
    CHECK_EQ(int(m.Evaluate().reasons & ReasonOnTaxi), int(ReasonOnTaxi));

    m.Inhibit(Inhibition::Rooted, ROOT_SRC);                // a root does not stop the flight
    m.Inhibit(Inhibition::Rooted, ROOT_SRC2);               // a seat's root, alongside the aura's
    CHECK(m.Evaluate().ticks);
    CHECK(m.Evaluate().mayMove);

    m.Die();
    CHECK(m.Inhibited(Inhibition::Dead));
    CHECK(m.Empty());
    CHECK(m.Inhibited(Inhibition::Rooted));                 // the seat's root survives death; the aura's does not
    CHECK(m.Uninhibit(Inhibition::Rooted, ROOT_SRC2));      // the seat releases it its own way
    CHECK(!m.Inhibited(Inhibition::Rooted));
    CHECK(m.Uninhibit(Inhibition::Dead, kDeathSource));    // resurrection
    CHECK(!m.Inhibited(Inhibition::Dead));
}

TEST(MotionArbiter_Block_AFinishedPausedEntryFreesTheBlockForTheNext)
{
    Arbiter m;
    m.InstallDefault(Kind::Wander);
    m.Request(Claim(Kind::Fear, 0x5782));
    m.DrainEvents();

    m.Inhibit(Inhibition::Rooted, ROOT_SRC);
    std::vector<Event> events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Suspended, Kind::Fear), 1);

    CHECK(m.Release(0x5782));                                // the paused claim finishes
    events = m.DrainEvents();
    CHECK_EQ(CountEvents(events, Event::Kind::Finished, Kind::Fear), 1);
    CHECK_EQ(CountBlocked(events, Event::Kind::Resumed, Kind::Fear), 0);
    CHECK_EQ(SelectedKind(m), K(Kind::Wander));              // the Wander default is selected next
    CHECK_EQ(CountBlocked(events, Event::Kind::Suspended, Kind::Wander), 1);   // and paused at once: the root still holds

    CHECK(m.Uninhibit(Inhibition::Rooted, ROOT_SRC));
    events = m.DrainEvents();
    CHECK_EQ(CountBlocked(events, Event::Kind::Resumed, Kind::Wander), 1);
}
