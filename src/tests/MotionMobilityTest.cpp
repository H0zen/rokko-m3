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

// The kernel's block state (P5-A): the reasons a unit may not be moved, each
// counted by source, and the table of what a selected behaviour may do under
// them. Pure: no arbiter, no unit.

#include "TestHarness.h"
#include "Mobility.h"

using namespace Motion;

namespace
{
    int I(Inhibition i) { return static_cast<int>(i); }
    const uint64 ROOT_A = InhibitSource(SourceDomain::Aura, 1, 339);
    const uint64 ROOT_B = InhibitSource(SourceDomain::Aura, 2, 339);
    const uint64 STUN_A = InhibitSource(SourceDomain::Aura, 1, 853);
}

TEST(MotionMobility_ReasonHoldsWhileAnySourceHolds)
{
    Mobility m;
    CHECK(!m.Inhibited(Inhibition::Rooted));
    CHECK(m.Inhibit(Inhibition::Rooted, ROOT_A));      // the first source: an edge
    CHECK(!m.Inhibit(Inhibition::Rooted, ROOT_B));     // a second source: no edge
    CHECK(!m.Inhibit(Inhibition::Rooted, ROOT_B));     // the same source again: idempotent, no edge
    CHECK_EQ(int(m.Sources(Inhibition::Rooted).size()), 2);
    CHECK(!m.Uninhibit(Inhibition::Rooted, ROOT_A));   // one of two gone: still rooted
    CHECK(m.Inhibited(Inhibition::Rooted));
    CHECK(!m.Uninhibit(Inhibition::Rooted, ROOT_A));   // an unknown source: a no-op
    CHECK(m.Uninhibit(Inhibition::Rooted, ROOT_B));    // the last source: an edge
    CHECK(!m.Inhibited(Inhibition::Rooted));
    CHECK_EQ(int(m.Reasons()), 0);
}

TEST(MotionMobility_ReasonsBitsAndDominant)
{
    Mobility m;
    m.Inhibit(Inhibition::Rooted, ROOT_A);
    m.Inhibit(Inhibition::Stunned, STUN_A);
    CHECK_EQ(int(m.Reasons()), int(ReasonRooted | ReasonStunned));
    MobilityDecision d = Decide(Selected::Ordinary, m.Reasons());
    CHECK(!d.ticks);
    CHECK(!d.mayMove);
    CHECK(!d.mayTurn);
    CHECK_EQ(I(d.dominant), I(Inhibition::Stunned));   // Dead, Stunned, Possessed, Rooted: the first active
    m.Uninhibit(Inhibition::Stunned, STUN_A);
    d = Decide(Selected::Ordinary, m.Reasons());
    CHECK(!d.mayMove);
    CHECK(d.mayTurn);
    CHECK_EQ(I(d.dominant), I(Inhibition::Rooted));
}

TEST(MotionMobility_TableOrdinaryAndControl)
{
    // Rooted: no move, turn; the Control claim is paused too.
    MobilityDecision d = Decide(Selected::Control, ReasonRooted | ReasonFeared);
    CHECK(!d.ticks); CHECK(!d.mayMove); CHECK(d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Rooted));
    // Stunned: nothing, for either class.
    d = Decide(Selected::Ordinary, ReasonStunned);
    CHECK(!d.ticks); CHECK(!d.mayMove); CHECK(!d.mayTurn);
    d = Decide(Selected::Control, ReasonStunned | ReasonConfused);
    CHECK(!d.ticks); CHECK(!d.mayMove); CHECK(!d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Stunned));
    // Dead: nothing.
    d = Decide(Selected::Control, ReasonDead | ReasonFeared);
    CHECK(!d.ticks); CHECK(!d.mayMove); CHECK(!d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Dead));
    // Possessed: the possessor moves the body, so an ordinary behaviour pauses; a Control
    // claim plays and the possessor is the one locked out (reference 15.4.1).
    d = Decide(Selected::Ordinary, ReasonPossessed);
    CHECK(!d.ticks); CHECK(!d.mayMove); CHECK(!d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Possessed));
    d = Decide(Selected::Control, ReasonPossessed | ReasonFeared);
    CHECK(d.ticks); CHECK(d.mayMove); CHECK(d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Count));
    // Nothing active: everything allowed.
    d = Decide(Selected::Ordinary, 0);
    CHECK(d.ticks); CHECK(d.mayMove); CHECK(d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Count));
    // Nothing selected: reported, never blocking.
    d = Decide(Selected::None, ReasonRooted);
    CHECK(!d.mayMove); CHECK(d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Rooted));
}

TEST(MotionMobility_TableDistractAndTaxi)
{
    // A distract's clock runs under every reason (its tick moves nothing); the unit does not
    // move or turn while stunned or rooted, and the 10 s run out meanwhile.
    MobilityDecision d = Decide(Selected::Distract, ReasonStunned);
    CHECK(d.ticks); CHECK(!d.mayMove); CHECK(!d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Stunned));
    d = Decide(Selected::Distract, ReasonRooted);
    CHECK(d.ticks); CHECK(!d.mayMove); CHECK(d.mayTurn);
    d = Decide(Selected::Distract, ReasonDead);
    CHECK(!d.ticks); CHECK(!d.mayMove); CHECK(!d.mayTurn);
    d = Decide(Selected::Distract, ReasonPossessed);
    CHECK(d.ticks); CHECK(!d.mayMove); CHECK(!d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Possessed));
    // A flight goes on under a root or a stun (nothing lands on a passenger; a scripted flight
    // on a stunned unit keeps flying, reference 15.6.2); death ends it like everything else.
    d = Decide(Selected::Taxi, ReasonRooted | ReasonStunned);
    CHECK(d.ticks); CHECK(d.mayMove); CHECK(d.mayTurn); CHECK_EQ(I(d.dominant), I(Inhibition::Count));
    d = Decide(Selected::Taxi, ReasonDead);
    CHECK(!d.ticks); CHECK(!d.mayMove);
    d = Decide(Selected::Taxi, ReasonPossessed);
    CHECK(d.ticks); CHECK(d.mayMove);
}

TEST(MotionMobility_DropDomainReleasesOnlyThatDomain)
{
    // An aura source and a seat source both hold Rooted; dropping the Aura domain (as death
    // does) leaves the seat's, which has a release path of its own.
    Mobility m;
    const uint64 aura = InhibitSource(SourceDomain::Aura, 1, 339);
    const uint64 seat = InhibitSource(SourceDomain::Seat, 2, 0);
    m.Inhibit(Inhibition::Rooted, aura);
    m.Inhibit(Inhibition::Rooted, seat);
    CHECK_EQ(int(m.Sources(Inhibition::Rooted).size()), 2);

    m.DropDomain(SourceDomain::Aura);
    CHECK(m.Inhibited(Inhibition::Rooted));
    CHECK_EQ(int(m.Sources(Inhibition::Rooted).size()), 1);
    CHECK(m.Sources(Inhibition::Rooted)[0] == seat);
}

TEST(MotionMobility_SourcesAndNames)
{
    // The domain sits in the top nibble; a ControlClaim-shaped aura identity (spell << 40)
    // never reaches it for any 4.3.4 spell id (< 2^20).
    CHECK(InhibitSource(SourceDomain::Aura, 7, 339) != InhibitSource(SourceDomain::Seat, 7, 339));
    CHECK(InhibitSource(SourceDomain::Possession, 7) != InhibitSource(SourceDomain::Possession, 8));
    CHECK_EQ(int(InhibitSource(SourceDomain::Aura, 7, 339) >> 60), 0);
    CHECK_EQ(int(kDeathSource >> 60), int(SourceDomain::Death));
    CHECK_EQ(int(InhibitSource(SourceDomain::Seat, 7, 0xFFFFFFFFu) >> 60), int(SourceDomain::Seat));   // the domain survives a full extra
    CHECK_STR(InhibitionName(Inhibition::Rooted), "Rooted");
    CHECK_STR(InhibitionName(Inhibition::Possessed), "Possessed");
    CHECK_STR(InhibitionName(Inhibition::Count), "none");
}
