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

#include "Scenario.h"
#include "Harness.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

// The block scenarios (P5-A): the retail overlap rules the kernel's block state reproduces
// (reference 2.5, 3.6, 15.7). Every scenario spawns its own actors at SE and ends them.
// Pt and Spread are ScenariosFlee.cpp's, shared through Scenario.h.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;
        const Pt SE = { -3200.0f, -300.0f, 47.0f };
        const uint32 ROOT = 745;    // Web: a plain root aura, about 5 s, no damage
        const uint32 STUN = 5211;   // Bash: a plain stun aura
        const uint32 FEIGN = 5384;  // Feign Death

        /// S27: two proofs about the client root flag (the mover flag). a: two Rooted
        /// sources of different identity overlap -- a self-cast Web, then a second source
        /// through the kernel's own Inhibit/Uninhibit API (a script's root, not an aura's;
        /// a second Web from another caster would just replace the first per the
        /// reference's same-spell rule, 15.8, so it cannot prove two sources -- only the
        /// kernel's own API can). The flag must hold while either source remains and clear
        /// only once the LATER one releases -- the rule is "last source", not "first cast"
        /// -- so the two sources are asserted at every sample, not just the final one.
        /// b: a Bash stacked over a Web on a creature; a creature's stun leaves the mover's
        /// root flag to the root alone (reference 2.3: a stunned creature is stopped, not
        /// rooted -- only a stunned player or player-charmed unit projects the flag from a
        /// stun), so the flag clears with the Web's end even while the Bash still holds
        /// the creature stunned.
        class StunOverRoot : public Scenario
        {
        public:
            StunOverRoot() : Scenario("stun-over-root", 27) {}

            void Prepare() override
            {
                struct RootSample { uint32 t; bool rooted; bool flag; bool stun; };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(WOLF, SE.x + 15.0f, SE.y, Ground(SE.x + 15.0f, SE.y, SE.z), 0.0f);
                if (!a || !b) { Verdict("rootHeldByLastSource=INVALID(spawn failed) | stunLeavesCreatureRootToTheWeb=INVALID(spawn failed)"); return; }
                const ObjectGuid ga = a->GetObjectGuid(), gb = b->GetObjectGuid();
                auto sa = std::make_shared<std::vector<RootSample> >();
                auto sb = std::make_shared<std::vector<RootSample> >();
                At(500,  [this, ga]() { if (Creature* a = Get(ga)) { a->CastSpell(a, ROOT, true); Log("a: self Web"); } });
                At(500,  [this, gb]() { if (Creature* b = Get(gb)) { b->CastSpell(b, ROOT, true); Log("b: Web"); } });
                At(2500, [this, ga]()
                {
                    if (Creature* a = Get(ga))
                    {
                        a->GetMotionMaster()->Inhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::Script, a->GetObjectGuid().GetCounter(), 27));
                        Log("a: a script root over the Web");
                    }
                });
                At(2500, [this, gb]() { if (Creature* b = Get(gb)) { b->CastSpell(b, STUN, true); Log("b: Bash over the Web"); } });
                At(7500, [this, ga]()
                {
                    if (Creature* a = Get(ga))
                    {
                        a->GetMotionMaster()->Uninhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::Script, a->GetObjectGuid().GetCounter(), 27));
                        Log("a: the script root released");
                    }
                });
                for (uint32 t = 1000; t <= 9500; t += 500)
                {
                    At(t, [this, ga, gb, sa, sb, t]()
                    {
                        Creature* a = Get(ga); Creature* b = Get(gb); if (!a || !b) { return; }
                        RootSample ra = { t, a->IsRooted(), a->m_movementInfo.HasMovementFlag(MOVEFLAG_ROOT), a->Blocked(Motion::ReasonStunned) };
                        RootSample rb = { t, b->IsRooted(), b->m_movementInfo.HasMovementFlag(MOVEFLAG_ROOT), b->Blocked(Motion::ReasonStunned) };
                        sa->push_back(ra);
                        sb->push_back(rb);
                        char line[160];
                        snprintf(line, sizeof(line), "+%5ums a rooted=%d flag=%d stun=%d | b rooted=%d flag=%d stun=%d", t,
                                 ra.rooted ? 1 : 0, ra.flag ? 1 : 0, ra.stun ? 1 : 0,
                                 rb.rooted ? 1 : 0, rb.flag ? 1 : 0, rb.stun ? 1 : 0);
                        Log("%s", line);
                    });
                }
                At(10000, [this, ga, gb, sa, sb]()
                {
                    if (!Get(ga) || !Get(gb)) { Verdict("rootHeldByLastSource=INVALID(lost) | stunLeavesCreatureRootToTheWeb=INVALID(lost)"); return; }
                    if (sa->size() < 3 || sb->size() < 3) { Verdict("rootHeldByLastSource=INVALID(no samples) | stunLeavesCreatureRootToTheWeb=INVALID(no samples)"); return; }
                    std::string bodyA;
                    {
                        bool bug = false;
                        RootSample bad = { 0, false, false, false };
                        for (size_t i = 0; i < sa->size() && !bug; ++i)
                        {
                            RootSample const& s = (*sa)[i];
                            if (s.t <= 7000 && !(s.rooted && s.flag)) { bug = true; bad = s; }
                            else if (s.t >= 8000 && (s.rooted || s.flag)) { bug = true; bad = s; }
                        }
                        char text[160];
                        if (!bug) { snprintf(text, sizeof(text), "rootHeldByLastSource=OK(rooted and flagged at every sample until the script's release, clear after)"); }
                        else { snprintf(text, sizeof(text), "rootHeldByLastSource=BUG(+%ums rooted=%d flag=%d)", bad.t, bad.rooted ? 1 : 0, bad.flag ? 1 : 0); }
                        bodyA = text;
                    }
                    std::string bodyB;
                    {
                        bool bug = false, stunFail = false;
                        RootSample bad = { 0, false, false, false };
                        for (size_t i = 0; i < sb->size() && !bug; ++i)
                        {
                            RootSample const& s = (*sb)[i];
                            if (s.t <= 5000 && !(s.rooted && s.flag)) { bug = true; bad = s; }
                            else if (s.t == 6000 && (s.rooted || s.flag)) { bug = true; bad = s; }
                            else if (s.t == 6000 && !s.stun) { bug = true; stunFail = true; bad = s; }
                            else if (s.t > 6000 && (s.rooted || s.flag)) { bug = true; bad = s; }
                        }
                        char text[200];
                        if (!bug) { snprintf(text, sizeof(text), "stunLeavesCreatureRootToTheWeb=OK(rooted and flagged only until the Web's end, clear at 6.0 s while the Bash still holds the stun)"); }
                        else if (stunFail) { snprintf(text, sizeof(text), "stunLeavesCreatureRootToTheWeb=BUG(+%ums stun=0: the Bash ended before the Web, proves nothing)", bad.t); }
                        else { snprintf(text, sizeof(text), "stunLeavesCreatureRootToTheWeb=BUG(+%ums rooted=%d flag=%d)", bad.t, bad.rooted ? 1 : 0, bad.flag ? 1 : 0); }
                        bodyB = text;
                    }
                    Verdict(bodyA + " | " + bodyB);
                });
            }
        };

        /// S28: a fear that ends before its root: the unit stays rooted where it stands, never resumes the flee,
        /// and moves again only when the root ends (reference 3.6, Fear + Root).
        class RootOutlastsFear : public Scenario
        {
        public:
            RootOutlastsFear() : Scenario("root-outlasts-fear", 28) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("standsAfterFearEnds=INVALID(spawn failed) | movesAfterRootEnds=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000); k->SetMaxHealth(500000); k->SetHealth(500000);
                k->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto rooted = std::make_shared<std::vector<Pt> >();
                auto after = std::make_shared<std::vector<Pt> >();
                auto fearGone = std::make_shared<bool>(true);
                At(500,  [this, g, gk]() { if (Creature* a = Get(g)) { a->SetFeared(true, gk, 5782, 0, 0); Log("feared, mt=%s", TypeName(a)); } });
                At(2000, [this, g]()     { if (Creature* a = Get(g)) { a->CastSpell(a, ROOT, true); Log("Web mid-flee, mt=%s", TypeName(a)); } });
                At(4500, [this, g, gk]() { if (Creature* a = Get(g)) { a->SetFeared(false, gk, 5782, 0, 0); Log("fear ended under the root, mt=%s rooted=%d", TypeName(a), a->IsRooted() ? 1 : 0); } });
                for (uint32 i = 1; i <= 4; ++i)    // 5.0 s .. 6.5 s: rooted (Web, cast at 2.0 s, runs about 5 s)
                {
                    At(4500 + i * 500, [this, g, rooted, fearGone, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        rooted->push_back(p);
                        if (Type(a) == Motion::Kind::Fear) { *fearGone = false; }
                        Log("rooted +%4ums mt=%s rooted=%d at %.1f %.1f", i * 500, TypeName(a), a->IsRooted() ? 1 : 0, p.x, p.y);
                    });
                }
                for (uint32 i = 1; i <= 7; ++i)    // 7.5 s .. 10.5 s: free again, well after the root's end
                {
                    At(7000 + i * 500, [this, g, after, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        after->push_back(p);
                        Log("free +%4ums mt=%s rooted=%d at %.1f %.1f", i * 500, TypeName(a), a->IsRooted() ? 1 : 0, p.x, p.y);
                    });
                }
                At(11000, [this, rooted, after, fearGone]()
                {
                    if (rooted->size() < 3 || after->size() < 3) { Verdict("standsAfterFearEnds=INVALID(no samples) | movesAfterRootEnds=INVALID(no samples)"); return; }
                    const float held = Spread(*rooted);
                    const float freed = Spread(*after);
                    char text[220];
                    snprintf(text, sizeof(text), "standsAfterFearEnds=%s | movesAfterRootEnds=%s",
                             (*fearGone && held < 1.0f) ? "OK(stood within 1 yd, no flee, until the root ended)" : (*fearGone ? "BUG(moved while rooted)" : "BUG(still FLEEING after the fear ended)"),
                             freed > 2.0f ? "OK(moved again after the root)" : "BUG(never moved after the root ended)");
                    Verdict(text);
                });
            }
        };

        /// S29: fear, then confuse, then a stun over both: the stun holds; its end plays the wander
        /// (Confused over Fear); the confuse's end resumes the flee from the current spot (reference 15.7).
        class StunConfuseFearResume : public Scenario
        {
        public:
            StunConfuseFearResume() : Scenario("stun-confuse-fear-resume", 29) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("stunHolds=INVALID(spawn failed) | wanderAfterStun=INVALID(spawn failed) | fleeAfterConfuse=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000); k->SetMaxHealth(500000); k->SetHealth(500000);
                k->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto stunned = std::make_shared<std::vector<Pt> >();
                auto stunKind = std::make_shared<bool>(true);
                auto wanderKind = std::make_shared<bool>(true);
                auto wanderMoved = std::make_shared<bool>(false);
                auto fleeKind = std::make_shared<bool>(true);
                auto fleeMoved = std::make_shared<bool>(false);
                At(500,  [this, g, gk]() { if (Creature* a = Get(g)) { a->SetFeared(true, gk, 5782, 0, 0); Log("feared, mt=%s", TypeName(a)); } });
                At(2000, [this, g, gk]() { if (Creature* a = Get(g)) { a->SetConfused(true, gk, 118, 0); Log("confused over the fear, mt=%s", TypeName(a)); } });
                At(3000, [this, g]()     { if (Creature* a = Get(g)) { a->CastSpell(a, STUN, true); Log("Bash over both, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 4; ++i)   // 3.5 s .. 5.0 s: inside the stun
                {
                    At(3000 + i * 500, [this, g, stunned, stunKind, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        stunned->push_back(p);
                        if (Type(a) != Motion::Kind::Confused) { *stunKind = false; }
                        Log("stunned +%4ums mt=%s stun=%d at %.1f %.1f", i * 500, TypeName(a), a->Blocked(Motion::ReasonStunned) ? 1 : 0, p.x, p.y);
                    });
                }
                At(5500, [this, g]() { if (Creature* a = Get(g)) { a->RemoveAurasDueToSpell(STUN); Log("stun removed, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 6; ++i)   // 6.0 s .. 8.5 s: the wander
                {
                    At(5500 + i * 500, [this, g, wanderKind, wanderMoved, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        if (Type(a) != Motion::Kind::Confused) { *wanderKind = false; }
                        if (a->GetMotionMaster()->Latches().confusedLeg) { *wanderMoved = true; }
                        Log("wander +%4ums mt=%s move=%d", i * 500, TypeName(a), a->GetMotionMaster()->Latches().confusedLeg ? 1 : 0);
                    });
                }
                At(9000, [this, g, gk]() { if (Creature* a = Get(g)) { a->SetConfused(false, gk, 118, 0); Log("confuse ended, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 8; ++i)   // 9.5 s .. 13.0 s: the flee again
                {
                    At(9000 + i * 500, [this, g, fleeKind, fleeMoved, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        if (Type(a) != Motion::Kind::Fear) { *fleeKind = false; }
                        if (a->GetMotionMaster()->Latches().fearLeg) { *fleeMoved = true; }
                        Log("flee +%4ums mt=%s move=%d", i * 500, TypeName(a), a->GetMotionMaster()->Latches().fearLeg ? 1 : 0);
                    });
                }
                At(13500, [this, g, gk, stunned, stunKind, wanderKind, wanderMoved, fleeKind, fleeMoved]()
                {
                    if (Creature* a = Get(g)) { a->SetFeared(false, gk, 5782, 0, 0); }
                    if (stunned->size() < 3) { Verdict("stunHolds=INVALID(no samples) | wanderAfterStun=INVALID | fleeAfterConfuse=INVALID"); return; }
                    const float held = Spread(*stunned);
                    char text[260];
                    snprintf(text, sizeof(text), "stunHolds=%s | wanderAfterStun=%s | fleeAfterConfuse=%s",
                             (held < 1.0f && *stunKind) ? "OK(stood, the confuse stayed selected under the stun)" : "BUG(moved or lost the confuse under the stun)",
                             (*wanderKind && *wanderMoved) ? "OK(the wander played after the stun)" : "BUG(no wander after the stun)",
                             (*fleeKind && *fleeMoved) ? "OK(the flee resumed after the confuse)" : "BUG(no flee after the confuse ended)");
                    Verdict(text);
                });
            }
        };

        /// S30: a stun on a distracted creature: it stands, its facing is kept, and the distract's
        /// 10 s run out under the stun (reference 15.7, Distracted + Stun-type).
        class StunOnDistract : public Scenario
        {
        public:
            StunOnDistract() : Scenario("stun-on-distract", 30) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                if (!a) { Verdict("standsUnderStun=INVALID(spawn failed) | distractRunsOut=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                auto pts = std::make_shared<std::vector<Pt> >();
                auto facing = std::make_shared<std::vector<float> >();
                auto distractKind = std::make_shared<bool>(true);
                At(500,  [this, g]() { if (Creature* a = Get(g)) { a->GetMotionMaster()->MoveDistract(10000); a->SetFacingTo(1.5f); Log("distracted 10 s facing 1.5, mt=%s", TypeName(a)); } });
                At(1500, [this, g]() { if (Creature* a = Get(g)) { a->CastSpell(a, STUN, true); Log("Bash on the distracted wolf, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 6; ++i)   // 2.0 s .. 4.5 s
                {
                    At(1500 + i * 500, [this, g, pts, facing, distractKind, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        pts->push_back(p);
                        facing->push_back(a->Where().Facing());
                        if (Type(a) != Motion::Kind::Distract) { *distractKind = false; }
                        Log("stunned +%4ums mt=%s facing=%.2f at %.1f %.1f", i * 500, TypeName(a), a->Where().Facing(), p.x, p.y);
                    });
                }
                At(11500, [this, g, pts, facing, distractKind]()   // 11.0 s after the distract began: its 10 s are out
                {
                    Creature* a = Get(g); if (!a) { Verdict("standsUnderStun=INVALID(lost) | distractRunsOut=INVALID(lost)"); return; }
                    const float held = Spread(*pts);
                    float turned = 0.0f;
                    for (size_t i = 1; i < facing->size(); ++i) { turned = std::max(turned, std::fabs((*facing)[i] - (*facing)[0])); }
                    const bool over = Type(a) != Motion::Kind::Distract;
                    char text[220];
                    snprintf(text, sizeof(text), "standsUnderStun=%s | distractRunsOut=%s",
                             (held < 1.0f && turned < 0.05f) ? "OK(stood, facing kept)" : "BUG(moved or turned under the stun)",
                             (*distractKind && over) ? "OK(the distract's 10 s ran out under the stun)" : "BUG(still DISTRACT after 11 s)");
                    Log("at 11.5 s mt=%s", TypeName(a));
                    Verdict(text);
                });
            }
        };

        /// S31: a feign pauses a follow without finishing it: the follower stands while the
        /// followed one moves on, and follows again when the feign ends (reference 15.5).
        class FeignKeepsFollow : public Scenario
        {
        public:
            FeignKeepsFollow() : Scenario("feign-keeps-follow", 31) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* leader = Spawn(WOLF, SE.x + 5.0f, SE.y, Ground(SE.x + 5.0f, SE.y, SE.z), 0.0f);
                if (!a || !leader) { Verdict("standsWhileFeigning=INVALID(spawn failed) | followsAfterFeign=INVALID(spawn failed)"); return; }
                leader->SetWalk(false);   // the leader runs; a follower matches its leader's gait (reference: WOLF walks slower than it runs)
                const ObjectGuid g = a->GetObjectGuid(), gLeader = leader->GetObjectGuid();
                auto pts = std::make_shared<std::vector<Pt> >();
                auto followKind = std::make_shared<bool>(true);
                auto closed = std::make_shared<float>(999.0f);
                At(500,  [this, g, gLeader]() { Creature* a = Get(g); Creature* leader = Get(gLeader); if (a && leader) { a->GetMotionMaster()->MoveFollow(leader, 2.0f, 0.0f); Log("follows, mt=%s", TypeName(a)); } });
                At(1000, [this, gLeader]()    { if (Creature* leader = Get(gLeader)) { leader->GetMotionMaster()->MovePoint(1, SE.x + 30.0f, SE.y, Ground(SE.x + 30.0f, SE.y, SE.z), true); Log("the leader runs 25 yd"); } });
                At(2000, [this, g]()     { if (Creature* a = Get(g)) { a->SetFeignDeath(true, a->GetObjectGuid(), FEIGN); Log("feigns, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 6; ++i)   // 2.5 s .. 5.0 s
                {
                    At(2000 + i * 500, [this, g, pts, followKind, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        pts->push_back(p);
                        if (Type(a) != Motion::Kind::Follow) { *followKind = false; }
                        Log("feigning +%4ums mt=%s at %.1f %.1f", i * 500, TypeName(a), p.x, p.y);
                    });
                }
                At(5500, [this, g]() { if (Creature* a = Get(g)) { a->SetFeignDeath(false, a->GetObjectGuid(), FEIGN); Log("feign ends, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 16; ++i)  // 6.0 s .. 13.5 s
                {
                    At(5500 + i * 500, [this, g, gLeader, closed, i]()
                    {
                        Creature* a = Get(g); Creature* leader = Get(gLeader); if (!a || !leader) { return; }
                        const float d = Dist2(a->Where().X(), a->Where().Y(), leader->Where().X(), leader->Where().Y());
                        if (d < *closed) { *closed = d; }
                        Log("after +%4ums mt=%s dist=%.1f", i * 500, TypeName(a), d);
                    });
                }
                At(14000, [this, pts, followKind, closed]()
                {
                    if (pts->size() < 3) { Verdict("standsWhileFeigning=INVALID(no samples) | followsAfterFeign=INVALID(no samples)"); return; }
                    const float held = Spread(*pts);
                    char text[220];
                    snprintf(text, sizeof(text), "standsWhileFeigning=%s | followsAfterFeign=%s",
                             (held < 1.0f && *followKind) ? "OK(stood, the follow stayed selected)" : "BUG(moved or lost the follow while feigning)",
                             *closed < 6.0f ? "OK(closed to the leader again)" : "BUG(never closed in after the feign)");
                    Verdict(text);
                });
            }
        };
    }

    void RegisterBlockScenarios(Runner& r)
    {
        r.Register(new StunOverRoot());
        r.Register(new RootOutlastsFear());
        r.Register(new StunConfuseFearResume());
        r.Register(new StunOnDistract());
        r.Register(new FeignKeepsFollow());
    }
}
