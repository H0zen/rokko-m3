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
#include "HarnessAI.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Pet.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerRegistry.h"
#include "WorldSession.h"
#include "World.h"
#include "Log.h"
#include "movement/MoveSpline.h"
#include "Utilities/MathDefines.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

// The control family (P5-B family 4 Task 5), orders 54-58: the flee's first bolt and its rest,
// the stagger's envelope and gait, a corpse as a fright, a refreshed aura restarting the flee,
// and the stagger in the air. Every sample is taken at the timeline's 100 ms cadence.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;
        const uint32 FLYER = 1512;     // Duskbat: InhabitType 5 (ground+air), the template flyer-falls-at-death uses
        const uint32 FEAR = 5782;      // the warlock's Fear: any MOD_FEAR spell id serves as the claim's key
        const uint32 POLYMORPH = 118;  // a MOD_CONFUSE spell id for the confuse's claim
        const uint32 FEIGN = 5384;     // the hunter's Feign Death: its one effect is SPELL_AURA_FEIGN_DEATH on the CASTER

        const Pt SE = { -3200.0f, -300.0f, 47.0f };

        float Bearing(float x1, float y1, float x2, float y2)
        {
            const float a = atan2f(y2 - y1, x2 - x1);
            return a >= 0.0f ? a : a + 2.0f * M_PI_F;
        }
        float AngleDiff(float a, float b)
        {
            float d = fabsf(a - b);
            while (d > M_PI_F) { d = fabsf(d - 2.0f * M_PI_F); }
            return d;
        }
        /// The running spline's endpoint, when one runs.
        bool RunningGoal(Creature* c, Movement::Vector3& goal)
        {
            if (c->movespline->Finalized()) { return false; }
            goal = c->movespline->FinalDestination();
            return true;
        }
        bool SameGoal(Movement::Vector3 const& a, Movement::Vector3 const& b)
        {
            return Dist2(a.x, a.y, b.x, b.y) < 0.01f && fabsf(a.z - b.z) < 0.01f;
        }

        /// A stop spline ends exactly where the unit is standing; the shortest flee bolt this
        /// geometry draws from inside the quiet band is 8 yd. Half a yard tells them apart.
        const float kStopSplineYd = 0.5f;

        /// "Standing on the leg's own endpoint", the proof a leg FINISHED when the sampler sees
        /// a fresh goal without ever having seen the unit standing.
        ///
        /// A fresh goal on its own proves nothing of the sort. The only reason a fear's goal
        /// cannot change part-way through a leg today is the production guard at
        /// ControlMoves.cpp:152-155 (`if (sight.status.traveling && m_havePoint) return
        /// Move(m_point)`) -- which is one of the things these scenarios exist to protect. With
        /// that guard gone the behaviour re-picks every tick and never finishes a leg, and a
        /// detector that called every fresh goal a chained bolt would score a run of pure
        /// re-routes as a healthy cadence. So a chain has to show the unit AT the endpoint of
        /// the leg it is chaining from.
        ///
        /// Two yards, from the sampler's own resolution: it runs at 100 ms, and a feared wolf
        /// covers about 0.75 yd in that (6.0 yd/s x 1.25 = 7.5 yd/s), so a leg that ended
        /// cleanly can be up to one window -- under a yard -- along its successor by the time
        /// that successor is first seen. Two yards is that window with room for the ground drop
        /// beneath a goal, and still a quarter of the shortest bolt, so a genuine mid-leg
        /// re-pick cannot hide inside it.
        const float kLegArrivedYd = 2.0f;

        /// The flee's first bolt: a wolf feared by a kobold 6 yd east bolts within pi/8 of due
        /// west for 0.4-1.3 times the 22 yd to the quiet band, with the fear's leg latched and the run
        /// gait on the leg (design §4.1: the close band, the bit with the leg, SetWalk(false));
        /// then the next bolt, on one of the two cadences the flee has had since 2026-09-21 --
        /// CHAINED straight on (no standing seen, or under 300 ms of it) or RESTED 800-1500 ms
        /// standing (measured 700-1700 at the sampler's cadence, and the rest counts only while
        /// standing). Which one this seed draws is the coin's business, not this scenario's;
        /// S64 fear-cadence-and-speed is the one that measures how often each falls.
        class FearBoltsAway : public Scenario
        {
        public:
            FearBoltsAway() : Scenario("fear-bolts-away", 54) {}

            void Prepare() override
            {
                struct St
                {
                    float x0, y0;
                    bool haveFirst, legRan, haveEnd, haveSecond, bitOnLeg, walkOnLeg;
                    bool chainedToSecond;   ///< the second bolt was laid without the wolf ever being SEEN standing
                    bool rerouted;          ///< a fresh goal appeared while the wolf was nowhere near the first bolt's endpoint
                    float rerouteYd;        ///< how far from it, for the verdict
                    uint32 endAt, secondAt;
                    Movement::Vector3 goal;
                };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("boltsAway=INVALID(spawn failed) | runsOnTheLeg=INVALID(spawn failed) | restsBetweenBolts=INVALID(spawn failed)"); return; }
                Silence(a);
                Silence(k);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->haveFirst = st->legRan = st->haveEnd = st->haveSecond = st->bitOnLeg = st->walkOnLeg = false;
                st->chainedToSecond = st->rerouted = false;
                st->rerouteYd = 0.0f;
                st->endAt = st->secondAt = 0;
                At(500, [this, g, gk, st]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    st->x0 = a->Where().X();
                    st->y0 = a->Where().Y();
                    a->SetFeared(true, gk, FEAR, 0, 0);
                    Log("feared by the kobold 6 yd east from (%.1f, %.1f), mt=%s", st->x0, st->y0, TypeName(a));
                });
                for (uint32 i = 1; i <= 55; ++i)
                {
                    At(500 + i * 100, [this, g, st, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const uint32 t = i * 100;
                        Movement::Vector3 goal;
                        const bool running = RunningGoal(a, goal);
                        if (running)
                        {
                            if (!st->haveFirst)
                            {
                                st->haveFirst = true;
                                st->goal = goal;
                                st->bitOnLeg = a->GetMotionMaster()->Latches().fearLeg;
                                st->walkOnLeg = a->IsWalking();
                                Log("+%4ums the first bolt: goal (%.1f, %.1f), %.1f yd, %.0f deg off due west, move=%d walk=%d mt=%s", t, goal.x, goal.y,
                                    Dist2(st->x0, st->y0, goal.x, goal.y), AngleDiff(Bearing(st->x0, st->y0, goal.x, goal.y), M_PI_F) * 180.0f / M_PI_F,
                                    st->bitOnLeg ? 1 : 0, st->walkOnLeg ? 1 : 0, TypeName(a));
                            }
                            // A fresh destination, whether or not the wolf was ever SEEN standing
                            // between the two: a chained bolt is laid inside one 100 ms sampling
                            // window, so waiting for the standing would miss it entirely. But a
                            // fresh goal is only a SECOND BOLT if the first one finished, and
                            // when the standing was never seen the proof of that is the wolf
                            // being at the first bolt's own endpoint (kLegArrivedYd). Anywhere
                            // else and this is a re-pick part-way through the first bolt -- the
                            // regression the traveling guard prevents -- which must read BUG,
                            // not be counted as a chain.
                            else if (!st->haveSecond && !SameGoal(goal, st->goal))
                            {
                                const float fromEnd = Dist2(st->goal.x, st->goal.y, a->Where().X(), a->Where().Y());
                                if (!st->haveEnd && fromEnd > kLegArrivedYd)
                                {
                                    if (!st->rerouted)
                                    {
                                        st->rerouted = true;
                                        st->rerouteYd = fromEnd;
                                        Log("+%4ums RE-ROUTE: a fresh goal (%.1f, %.1f) while the wolf is %.1f yd from the first bolt's endpoint -- that bolt never finished",
                                            t, goal.x, goal.y, fromEnd);
                                    }
                                }
                                else
                                {
                                    st->haveSecond = true;
                                    st->secondAt = t;
                                    st->chainedToSecond = !st->haveEnd;
                                    if (st->haveEnd) { Log("+%4ums the second bolt, %u ms after the first ended", t, t - st->endAt); }
                                    else { Log("+%4ums the second bolt, chained: the wolf was never seen standing, and it is %.1f yd from the first bolt's endpoint", t, fromEnd); }
                                }
                            }
                            st->legRan = true;
                        }
                        else if (st->legRan && !st->haveEnd)
                        {
                            st->haveEnd = true;
                            st->endAt = t;
                            Log("+%4ums the first bolt ended, move=%d", t, a->GetMotionMaster()->Latches().fearLeg ? 1 : 0);
                        }
                    });
                }
                At(6100, [this, st]()
                {
                    char bolt[160], gait[96], rest[120];
                    if (!st->haveFirst)
                    {
                        snprintf(bolt, sizeof(bolt), "BUG(no bolt laid within 5.5 s)");
                        snprintf(gait, sizeof(gait), "INVALID(no bolt)");
                        snprintf(rest, sizeof(rest), "INVALID(no bolt)");
                    }
                    else
                    {
                        const float dist = Dist2(st->x0, st->y0, st->goal.x, st->goal.y);
                        const float off = AngleDiff(Bearing(st->x0, st->y0, st->goal.x, st->goal.y), M_PI_F);
                        const bool away = off <= 0.45f && dist >= 8.0f && dist <= 30.5f;   // pi/8 = 0.39 plus the mesh's slack; 0.4-1.3 x 22 yd, the 30 yd cap
                        snprintf(bolt, sizeof(bolt), "%s(%.0f deg off due west, %.1f yd)", away ? "OK" : "BUG", off * 180.0f / M_PI_F, dist);
                        snprintf(gait, sizeof(gait), "%s(move=%d walk=%d on the leg)", (st->bitOnLeg && !st->walkOnLeg) ? "OK" : "BUG", st->bitOnLeg ? 1 : 0, st->walkOnLeg ? 1 : 0);
                        if (st->rerouted)
                        {
                            // The traveling guard is gone (or the leg was refused): the goal
                            // moved while the wolf was still on its way to the last one, so
                            // there is no second BOLT to time, only a leg that never finished.
                            snprintf(rest, sizeof(rest), "BUG(a fresh goal %.1f yd from the first bolt's endpoint: the bolt never finished, so nothing here is a rest or a chain)", st->rerouteYd);
                        }
                        else if (!st->haveSecond) { snprintf(rest, sizeof(rest), "BUG(no second bolt within 5.5 s)"); }
                        else if (st->chainedToSecond)
                        {
                            // The chained branch: the coin said no rest, so the next bolt went
                            // out as this one ended and the sampler never caught the wolf standing.
                            snprintf(rest, sizeof(rest), "OK(chained: the second bolt at +%ums, no standing seen)", st->secondAt);
                        }
                        else
                        {
                            // Either branch is right; only a rest OUTSIDE the band is a bug.
                            const uint32 gap = st->secondAt - st->endAt;
                            snprintf(rest, sizeof(rest), "%s(%u ms standing between the bolts)", (gap <= 300 || (gap >= 700 && gap <= 1700)) ? "OK" : "BUG", gap);
                        }
                    }
                    std::string text = std::string("boltsAway=") + bolt + " | runsOnTheLeg=" + gait + " | restsBetweenBolts=" + rest;
                    Verdict(text);
                });
            }
        };

        /// The stagger's envelope and gait: every lurch's goal within Movement.ConfuseRadius of
        /// the spot the wolf was confused at, at a walk with the confuse's leg latched, launched every
        /// 800-1500 ms (measured 700-2000 at the sampler's cadence, and a refused point's
        /// doubling retry stretches one gap; a measured gap may double again when a lurch is too
        /// short to be seen: the stagger counts from the launch, mid-leg included, and a lurch
        /// supersedes the last part-way). The radius is read from the config so the verdict
        /// holds for step one's 10 yd and step two's 2.
        class ConfuseLurchesNearAnchor : public Scenario
        {
        public:
            ConfuseLurchesNearAnchor() : Scenario("confuse-lurches-near-anchor", 55) {}

            void Prepare() override
            {
                struct St
                {
                    float ax, ay;
                    uint32 launches, lastLaunchAt, minGap, maxGap;
                    float farthest;
                    bool haveGoal, allWalk, allBit;
                    Movement::Vector3 lastGoal;
                };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("withinRadius=INVALID(spawn failed) | walksTheLurch=INVALID(spawn failed) | lurchCadence=INVALID(spawn failed)"); return; }
                Silence(a);
                Silence(k);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->launches = 0; st->lastLaunchAt = 0; st->minGap = 100000; st->maxGap = 0; st->farthest = 0.0f;
                st->haveGoal = false; st->allWalk = true; st->allBit = true;
                At(500, [this, g, gk, st]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    st->ax = a->Where().X();
                    st->ay = a->Where().Y();
                    a->SetConfused(true, gk, POLYMORPH, 0);
                    Log("confused at (%.1f, %.1f), radius %.1f, mt=%s", st->ax, st->ay, sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_CONFUSE_RADIUS), TypeName(a));
                });
                for (uint32 i = 1; i <= 60; ++i)
                {
                    At(500 + i * 100, [this, g, st, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const uint32 t = i * 100;
                        // A launch is counted by its destination, whether the spline is still
                        // running or already finalised (skip only while none was ever laid): a
                        // leg short enough to end inside one 100 ms sampling window is still
                        // seen this way, where a Finalized()-gated read would miss it. The gait
                        // and the bit are only meaningful while a leg is actually running.
                        if (!a->movespline->Initialized()) { return; }
                        const Movement::Vector3 goal = a->movespline->FinalDestination();
                        const bool running = !a->movespline->Finalized();
                        if (running)
                        {
                            if (!a->IsWalking()) { st->allWalk = false; }
                            if (!a->GetMotionMaster()->Latches().confusedLeg) { st->allBit = false; }
                        }
                        if (st->haveGoal && SameGoal(goal, st->lastGoal)) { return; }
                        // A stop spline ends where the unit stands (the confuse's activation stops
                        // a mover): not a lurch, so it is remembered and not counted.
                        if (Dist2(goal.x, goal.y, a->Where().X(), a->Where().Y()) < 0.1f && !running)
                        {
                            st->haveGoal = true;
                            st->lastGoal = goal;
                            return;
                        }
                        // A fresh lurch: a destination the last one did not have.
                        const float d = Dist2(st->ax, st->ay, goal.x, goal.y);
                        if (d > st->farthest) { st->farthest = d; }
                        if (st->launches > 0)
                        {
                            const uint32 gap = t - st->lastLaunchAt;
                            if (gap < st->minGap) { st->minGap = gap; }
                            if (gap > st->maxGap) { st->maxGap = gap; }
                        }
                        ++st->launches;
                        st->lastLaunchAt = t;
                        st->haveGoal = true;
                        st->lastGoal = goal;
                        Log("+%4ums lurch %u: goal (%.1f, %.1f), %.1f yd from the anchor, walk=%d move=%d mt=%s", t, st->launches, goal.x, goal.y, d, a->IsWalking() ? 1 : 0, a->GetMotionMaster()->Latches().confusedLeg ? 1 : 0, TypeName(a));
                    });
                }
                At(6600, [this, st]()
                {
                    const float radius = sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_CONFUSE_RADIUS);
                    char within[128], gait[96], cadence[160];
                    if (st->launches < 2)
                    {
                        snprintf(within, sizeof(within), "BUG(%u lurches in 6 s)", st->launches);
                        snprintf(gait, sizeof(gait), "INVALID(%u lurches)", st->launches);
                    }
                    else
                    {
                        snprintf(within, sizeof(within), "%s(%u lurches, the farthest goal %.1f yd of the %.1f yd envelope)", st->farthest <= radius + 0.6f ? "OK" : "BUG", st->launches, st->farthest, radius);
                        snprintf(gait, sizeof(gait), "%s(walk=%d move=%d on every leg)", (st->allWalk && st->allBit) ? "OK" : "BUG", st->allWalk ? 1 : 0, st->allBit ? 1 : 0);
                    }
                    if (st->launches == 0)
                    {
                        snprintf(cadence, sizeof(cadence), "INVALID(no lurch)");
                    }
                    else if (st->launches < 3)
                    {
                        // The stagger of 800-1500 ms yields four to seven launches in 6 s, three
                        // even with one unseen: fewer is a stagger that stalled, not a run too
                        // short to measure.
                        snprintf(cadence, sizeof(cadence), "BUG(%u lurches in 6 s)", st->launches);
                    }
                    else
                    {
                        // A pick within the driver's 0.5 yd relay gate of the lurch still being
                        // walked is kept as that leg, and a fresh leg under 0.25 yd at the walk
                        // ends inside one 100 ms sampling window: at a small envelope a launch
                        // can go unseen, so one doubled gap is the sampler's, not the stagger's.
                        snprintf(cadence, sizeof(cadence), "%s(launch to launch %u-%u ms)", (st->minGap >= 700 && st->maxGap <= 3600) ? "OK" : "BUG", st->minGap, st->maxGap);
                    }
                    std::string text = std::string("withinRadius=") + within + " | walksTheLurch=" + gait + " | lurchCadence=" + cadence;
                    Verdict(text);
                });
            }
        };

        /// A corpse frightens: the kobold is killed before the fear lands, and the flee still
        /// resolves it through the port (ObjectLookup::GetUnit has no alive test, design §6.7)
        /// and bolts away from where it lies.
        class FearFromACorpse : public Scenario
        {
        public:
            FearFromACorpse() : Scenario("fear-from-a-corpse", 56) {}

            void Prepare() override
            {
                struct St
                {
                    float cx, cy;                          // the corpse's position, recorded at the kill
                    bool  haveCorpse;                      // ...which only happens if the kobold was still resolvable then
                    bool  haveFirst, legRan, haveEnd, haveSecond;
                    bool  rerouted;                        // a fresh goal while the wolf was nowhere near the first bolt's endpoint
                    float rerouteYd;
                    float bx0, by0, bx1, by1;               // the wolf's position when each bolt was first seen running
                    Movement::Vector3 goal0, goal1;
                    Motion::Kind mtAfter;
                };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("fleeStarts=INVALID(spawn failed) | corpseFrightens=INVALID(spawn failed)"); return; }
                Silence(a);
                Silence(k);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->haveFirst = st->legRan = st->haveEnd = st->haveSecond = false;
                st->haveCorpse = st->rerouted = false;
                // Not left to chance: if the kobold cannot be resolved at +300 ms the corpse's
                // position is never recorded, and every bearing this scenario computes would be
                // taken from an uninitialised one. haveCorpse makes that an INVALID verdict.
                st->cx = st->cy = st->rerouteYd = 0.0f;
                st->mtAfter = Motion::Kind::Idle;
                At(300, [this, gk, st]()
                {
                    Creature* k = Get(gk); if (!k) { return; }
                    st->cx = k->Where().X();
                    st->cy = k->Where().Y();
                    st->haveCorpse = true;
                    k->DealDamage(k, k->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
                    Log("the kobold killed at (%.1f, %.1f): alive=%d", st->cx, st->cy, k->IsAlive() ? 1 : 0);
                });
                At(500, [this, g, gk]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(true, gk, FEAR, 0, 0);
                    Log("feared by the corpse 6 yd east, mt=%s", TypeName(a));
                });
                At(700, [this, g, st]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    st->mtAfter = Type(a);
                });
                for (uint32 i = 1; i <= 40; ++i)
                {
                    At(500 + i * 100, [this, g, st, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const uint32 t = i * 100;
                        Movement::Vector3 goal;
                        const bool running = RunningGoal(a, goal);
                        if (running)
                        {
                            if (!st->haveFirst)
                            {
                                st->haveFirst = true;
                                st->bx0 = a->Where().X();
                                st->by0 = a->Where().Y();
                                st->goal0 = goal;
                                Log("+%4ums the first bolt: goal (%.1f, %.1f) from (%.1f, %.1f)", t, goal.x, goal.y, st->bx0, st->by0);
                            }
                            // A fresh destination, not "seen standing then running again": since
                            // 2026-09-21 about half the bolts are CHAINED into the next inside
                            // one 100 ms sampling window, and a detector that waits for the
                            // standing simply never finds a second bolt on those runs. But when
                            // the standing was never seen, the proof that the first bolt ENDED
                            // is the wolf standing on its endpoint (kLegArrivedYd); a fresh goal
                            // taken from anywhere else is a re-pick part-way through the bolt,
                            // and this scenario must report that rather than measure the
                            // bearing of a leg that never happened.
                            else if (!st->haveSecond && !SameGoal(goal, st->goal0))
                            {
                                const float fromEnd = Dist2(st->goal0.x, st->goal0.y, a->Where().X(), a->Where().Y());
                                if (!st->haveEnd && fromEnd > kLegArrivedYd)
                                {
                                    if (!st->rerouted)
                                    {
                                        st->rerouted = true;
                                        st->rerouteYd = fromEnd;
                                        Log("+%4ums RE-ROUTE: a fresh goal (%.1f, %.1f) while the wolf is %.1f yd from the first bolt's endpoint -- that bolt never finished",
                                            t, goal.x, goal.y, fromEnd);
                                    }
                                }
                                else
                                {
                                    st->haveSecond = true;
                                    st->bx1 = a->Where().X();
                                    st->by1 = a->Where().Y();
                                    st->goal1 = goal;
                                    Log("+%4ums the second bolt: goal (%.1f, %.1f) from (%.1f, %.1f)%s", t, goal.x, goal.y, st->bx1, st->by1,
                                        st->haveEnd ? "" : " (chained: never seen standing, and on the first bolt's endpoint)");
                                }
                            }
                            st->legRan = true;
                        }
                        else if (st->legRan && !st->haveEnd)
                        {
                            st->haveEnd = true;
                        }
                    });
                }
                At(4600, [this, st]()
                {
                    char starts[96], bolt[220];
                    snprintf(starts, sizeof(starts), "%s(mt=%s 200 ms after the fear)", st->mtAfter == Motion::Kind::Fear ? "OK" : "BUG", Motion::KindName(st->mtAfter));
                    if (!st->haveCorpse)
                    {
                        // Every bearing below is taken FROM the corpse; without its position
                        // there is nothing to measure, and a zeroed one would measure nonsense.
                        snprintf(bolt, sizeof(bolt), "INVALID(the corpse's position was never recorded)");
                    }
                    else if (st->rerouted)
                    {
                        snprintf(bolt, sizeof(bolt), "BUG(a fresh goal %.1f yd from the first bolt's endpoint: the bolt never finished, so its bearing from the corpse means nothing)", st->rerouteYd);
                    }
                    else if (!st->haveFirst || !st->haveSecond)
                    {
                        snprintf(bolt, sizeof(bolt), "INVALID(%s within 4 s)", st->haveFirst ? "only one bolt ran" : "no bolt ran");
                    }
                    else
                    {
                        const float bx[2] = { st->bx0, st->bx1 };
                        const float by[2] = { st->by0, st->by1 };
                        const Movement::Vector3 goal[2] = { st->goal0, st->goal1 };
                        bool pass[2];
                        for (uint32 i = 0; i < 2; ++i)
                        {
                            // The close band, from where THIS bolt launched (the wolf drifts
                            // between bolts): a launch inside minQuiet (28 yd) bolts away from the
                            // corpse within pi/8 (0.45 rad with the mesh's slack) for 0.4-1.3
                            // times the distance left to the band (half a yard of slack for the
                            // ground drop and the mesh, and the 30 yd path cap). A launch past
                            // minQuiet mills about in any direction or drifts back, so neither the
                            // bearing nor the length says anything about the corpse: it passes.
                            const float away = Bearing(st->cx, st->cy, bx[i], by[i]);
                            const float boltBearing = Bearing(bx[i], by[i], goal[i].x, goal[i].y);
                            const float off = AngleDiff(boltBearing, away);
                            const float distFromCorpse = Dist2(st->cx, st->cy, bx[i], by[i]);
                            if (distFromCorpse <= 28.0f)
                            {
                                const float dist = Dist2(bx[i], by[i], goal[i].x, goal[i].y);
                                const float lo = 0.4f * (28.0f - distFromCorpse) - 0.6f;
                                float hi = 1.3f * (28.0f - distFromCorpse) + 0.6f;
                                if (hi > 30.5f) { hi = 30.5f; }
                                pass[i] = off <= 0.45f && dist >= lo && dist <= hi;
                                Log("bolt %u: launched %.1f yd from the corpse, %.0f deg off away from it, %.1f yd of the %.1f-%.1f band", i + 1, distFromCorpse, off * 180.0f / M_PI_F, dist, lo, hi);
                            }
                            else
                            {
                                pass[i] = true;
                                Log("bolt %u launched %.1f yd from the corpse, past minQuiet: no band to test, %.0f deg off away from it", i + 1, distFromCorpse, off * 180.0f / M_PI_F);
                            }
                        }
                        if (pass[0] && pass[1]) { snprintf(bolt, sizeof(bolt), "OK(every bolt launched inside minQuiet heads away from the corpse for its band's length)"); }
                        else { snprintf(bolt, sizeof(bolt), "BUG(bolt %u: not within 0.45 rad of away from the corpse, or its length outside 0.4-1.3 x the distance left to the band)", pass[0] ? 2u : 1u); }
                    }
                    std::string text = std::string("fleeStarts=") + starts + " | corpseFrightens=" + bolt;
                    Verdict(text);
                });
            }
        };

        /// A refreshed aura of the same identity (the same spell, effect and caster) restarts the
        /// flee (design fact 5: the arbiter's in-place update binds the fresh native and retires
        /// the running one): a fresh bolt follows each refresh within 400 ms, the type stays
        /// FLEEING throughout, and the one claim's single release ends it.
        class FearRefreshSameClaim : public Scenario
        {
        public:
            FearRefreshSameClaim() : Scenario("fear-refresh-same-claim", 57) {}

            void Prepare() override
            {
                struct St
                {
                    uint32 refreshAt;
                    bool haveGoalBefore;
                    Movement::Vector3 goalBefore;
                    bool freshAfter[2];
                    uint32 refreshes;
                    bool typeHeld;
                    bool endedClean;
                    bool sampledEnd;
                };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("refreshRestarts=INVALID(spawn failed) | typeHeld=INVALID(spawn failed) | oneReleaseEnds=INVALID(spawn failed)"); return; }
                Silence(a);
                Silence(k);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->refreshAt = 0; st->haveGoalBefore = false; st->freshAfter[0] = st->freshAfter[1] = false;
                st->refreshes = 0; st->typeHeld = true; st->endedClean = true; st->sampledEnd = false;
                At(500, [this, g, gk]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(true, gk, FEAR, 0, 0);
                    Log("feared, mt=%s", TypeName(a));
                });
                auto refresh = [this, g, gk, st](uint32 at)
                {
                    Creature* a = Get(g); if (!a) { return; }
                    Movement::Vector3 goal;
                    st->haveGoalBefore = RunningGoal(a, goal);
                    st->goalBefore = goal;
                    st->refreshAt = at;
                    ++st->refreshes;
                    a->SetFeared(true, gk, FEAR, 0, 0);
                    Log("+%4ums the same fear applied again (%s a leg), mt=%s", at, st->haveGoalBefore ? "mid" : "between", TypeName(a));
                };
                At(2000, [refresh]() { refresh(2000); });
                At(3500, [refresh]() { refresh(3500); });
                for (uint32 i = 1; i <= 45; ++i)
                {
                    At(500 + i * 100, [this, g, st, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const uint32 t = 500 + i * 100;
                        if (Type(a) != Motion::Kind::Fear) { st->typeHeld = false; Log("+%4ums mt=%s", t, TypeName(a)); }
                        if (st->refreshAt && t > st->refreshAt && t <= st->refreshAt + 400 && st->refreshes <= 2 && !st->freshAfter[st->refreshes - 1])
                        {
                            Movement::Vector3 goal;
                            if (RunningGoal(a, goal) && (!st->haveGoalBefore || !SameGoal(goal, st->goalBefore)))
                            {
                                st->freshAfter[st->refreshes - 1] = true;
                                Log("+%4ums a fresh bolt %u ms after the refresh: goal (%.1f, %.1f)", t, t - st->refreshAt, goal.x, goal.y);
                            }
                        }
                    });
                }
                At(5000, [this, g, gk]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(false, gk, FEAR, 0, 0);
                    Log("the one claim released once, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 5; ++i)
                {
                    At(5000 + i * 100, [this, g, st]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        st->sampledEnd = true;
                        if (Type(a) == Motion::Kind::Fear || a->Blocked(Motion::ReasonFeared)) { st->endedClean = false; }
                    });
                }
                At(5600, [this, st]()
                {
                    char restart[128], held[64], ends[96];
                    snprintf(restart, sizeof(restart), "%s(fresh bolt after refresh 1: %d, after refresh 2: %d)", (st->freshAfter[0] && st->freshAfter[1]) ? "OK" : "BUG", st->freshAfter[0] ? 1 : 0, st->freshAfter[1] ? 1 : 0);
                    snprintf(held, sizeof(held), "%s(FLEEING through both refreshes)", st->typeHeld ? "OK" : "BUG");
                    snprintf(ends, sizeof(ends), "%s(no FLEEING within 500 ms of the single release)", (st->sampledEnd && st->endedClean) ? "OK" : "BUG");
                    std::string text = std::string("refreshRestarts=") + restart + " | typeHeld=" + held + " | oneReleaseEnds=" + ends;
                    Verdict(text);
                });
            }
        };

        /// The stagger in the air: the flying template confused takes the random point's air
        /// branch (a third draw for z, design fact 4) through the same port call; it lurches,
        /// stays CONFUSED, and keeps its goals within the envelope in the plane.
        class ConfuseInTheAir : public Scenario
        {
        public:
            ConfuseInTheAir() : Scenario("confuse-in-the-air", 58) {}

            void Prepare() override
            {
                struct St { float ax, ay; uint32 launches; float farthest, aboveGround; bool haveGoal; Movement::Vector3 lastGoal; bool typeHeld; };
                const float startZ = Ground(SE.x, SE.y, SE.z) + 10.0f;
                Creature* w = Spawn(FLYER, SE.x, SE.y, startZ, 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!w || !k) { Verdict("airLurches=INVALID(spawn failed) | withinRadius2d=INVALID(spawn failed)"); return; }
                if (!w->CanFly())
                {
                    Log("ERR entry %u does not fly on the bare map (CanFly()=0): pick another entry", FLYER);
                    Verdict("airLurches=INVALID(the template does not fly) | withinRadius2d=INVALID(the template does not fly)");
                    return;
                }
                Silence(w);
                Silence(k);
                const ObjectGuid g = w->GetObjectGuid(), gk = k->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->launches = 0; st->farthest = 0.0f; st->aboveGround = -1000.0f; st->haveGoal = false; st->typeHeld = true;
                At(500, [this, g, gk, st]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    st->ax = w->Where().X();
                    st->ay = w->Where().Y();
                    w->SetConfused(true, gk, POLYMORPH, 0);
                    Log("confused in the air at z=%.1f, mt=%s", w->Where().Z(), TypeName(w));
                });
                for (uint32 i = 1; i <= 50; ++i)
                {
                    At(500 + i * 100, [this, g, st, i]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        if (Type(w) != Motion::Kind::Confused) { st->typeHeld = false; }
                        Movement::Vector3 goal;
                        if (!RunningGoal(w, goal)) { return; }
                        if (st->haveGoal && SameGoal(goal, st->lastGoal)) { return; }
                        const float d = Dist2(st->ax, st->ay, goal.x, goal.y);
                        if (d > st->farthest) { st->farthest = d; }
                        const float above = goal.z - Ground(goal.x, goal.y, goal.z);
                        if (above > st->aboveGround) { st->aboveGround = above; }
                        ++st->launches;
                        st->haveGoal = true;
                        st->lastGoal = goal;
                        Log("+%4ums lurch %u: goal (%.1f, %.1f, %.1f), %.1f yd from the anchor in the plane, %.1f yd above ground, mt=%s", i * 100, st->launches, goal.x, goal.y, goal.z, d, above, TypeName(w));
                    });
                }
                At(5600, [this, st]()
                {
                    const float radius = sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_CONFUSE_RADIUS);
                    char lurches[128], within[128];
                    snprintf(lurches, sizeof(lurches), "%s(%u lurches, CONFUSED throughout: %d, highest clearance %.1f yd)", (st->launches >= 1 && st->typeHeld && st->aboveGround >= 1.0f) ? "OK" : "BUG", st->launches, st->typeHeld ? 1 : 0, st->aboveGround);
                    if (st->launches < 1) { snprintf(within, sizeof(within), "INVALID(no lurch)"); }
                    else { snprintf(within, sizeof(within), "%s(the farthest goal %.1f yd of the %.1f yd envelope)", st->farthest <= radius + 0.6f ? "OK" : "BUG", st->farthest, radius); }
                    std::string text = std::string("airLurches=") + lurches + " | withinRadius2d=" + within;
                    Verdict(text);
                });
            }
        };
    }

        /// S60 (the live test of 2026-09-20): the fear applied AS THE SPELL, not through the
        /// entry point every other scenario calls. Spell 5782's third effect is a Mod Root aura
        /// -- retail's way of stopping the CLIENT steering while the server drives the flee --
        /// and once root auras reached the kernel's block state (the root fix of 2026-09-14) it
        /// paused the very fear that brought it: the victim stood still with its fleeing
        /// animation running, on every live server, while every harness scenario passed because
        /// it called Unit::SetFeared directly and the spell's other effects never landed.
        /// This one casts the spell.
        class FearAuraMoves : public Scenario
        {
        public:
            FearAuraMoves() : Scenario("fear-aura-moves", 60) {}

            void Prepare() override
            {
                struct St
                {
                    float x0, y0;
                    float far2;        ///< the farthest the wolf got from where it was feared
                    bool  feared;      ///< the claim was held at the first sample
                    bool  rooted;      ///< the kernel called it rooted while feared
                };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("fearAuraMoves=INVALID(spawn failed) | ownRootNotHeld=INVALID(spawn failed)"); return; }
                Silence(a);
                Silence(k);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->x0 = st->y0 = st->far2 = 0.0f;
                st->feared = st->rooted = false;

                At(500, [this, g, gk, st]()
                {
                    Creature* a = Get(g); Creature* k = Get(gk);
                    if (!a || !k) { return; }
                    st->x0 = a->Where().X();
                    st->y0 = a->Where().Y();
                    k->CastSpell(a, FEAR, true);        // the spell, with every effect it carries
                    Log("the kobold casts %u on the wolf at (%.1f, %.1f)", FEAR, st->x0, st->y0);
                });
                At(700, [this, g, st]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    st->feared = a->Blocked(Motion::ReasonFeared);
                    st->rooted = a->IsRooted();
                    Log("+ 200ms after the cast: feared=%d rooted=%d mt=%s", st->feared ? 1 : 0, st->rooted ? 1 : 0, TypeName(a));
                });
                for (uint32 i = 1; i <= 16; ++i)
                {
                    At(700 + i * 250, [this, g, st, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const float d = Dist2(st->x0, st->y0, a->Where().X(), a->Where().Y());
                        if (d > st->far2) { st->far2 = d; }
                        if (i % 4 == 0)
                        {
                            Log("+%4ums %.1f yd from the spot it was feared at, farthest %.1f", i * 250, d, st->far2);
                        }
                    });
                }
                At(5000, [this, g, st]()
                {
                    // A bolt is 11-36 yd and the first one is under way well inside four seconds:
                    // ten yards is the line between fleeing and standing, not a measure of the geometry.
                    const char* moved = !st->feared ? "INVALID(the fear never held)"
                                                    : (st->far2 >= 10.0f ? "OK" : "BUG(stood still)");
                    const char* root = st->rooted ? "BUG(the fear's own root is in the kernel's block)" : "OK";
                    Verdict(std::string("fearAuraMoves=") + moved + " | ownRootNotHeld=" + root);
                });
            }
        };

    /// S64 (the cadence note of 2026-09-21, design/2026-09-20-fear-cadence-and-speed.md): the two
    /// numbers a feared unit's flee is measured by, against retail's own.
    ///
    /// CADENCE. Retail's 27 consecutive-leg gaps inside confirmed MOD_FEAR aura windows
    /// (Cataclysm 4.0.6a dumps, peer/retail-fear-movement-2026-09-20.md) read
    /// median +114 ms, p75 +1341, max +7791, with about 52% of them at or under 300 ms: retail
    /// CHAINS about half its legs straight on and rests after the other half. The throwaway
    /// diagnostic of 2026-09-20 measured ours over the same 20 s on the same bare map: 7 legs,
    /// 6 gaps, mean 1033 ms, and ZERO chained -- we rested after every single leg. This scenario
    /// is that diagnostic made permanent, so a change that flattens the cadence back to one mode
    /// reads BUG instead of passing unnoticed.
    ///
    /// SPEED. Retail sends `SMSG_SPLINE_SET_RUN_SPEED 6.9444 -> 8.6805` in the fear aura's own
    /// batch and puts it back at removal -- a flat x1.25 for the aura's life, which our core did
    /// not do at all. The check is a ratio, not a number, so it holds for any creature template,
    /// and it is taken again after a BARE RECALCULATION mid-flee (Unit::UpdateSpeed with nothing
    /// else changed, which is what any unrelated aura change triggers): that is the whole reason
    /// the quarter lives inside UpdateSpeed's own arithmetic rather than being poked in from
    /// Unit::SetFeared, and the only way to prove it from here.
    ///
    /// The fear is the direct entry point with no time limit, not the spell (S60 covers the
    /// spell's own effects): the cadence needs twenty uninterrupted seconds, and the aura's
    /// duration is not this scenario's subject.
    class FearCadenceAndSpeed : public Scenario
    {
    public:
        FearCadenceAndSpeed() : Scenario("fear-cadence-and-speed", 63) {}

        void Prepare() override
        {
            struct St
            {
                // The cadence
                bool     running;          ///< a leg was under way at the previous sample
                bool     haveEnd;          ///< the standing since the last leg ended was SEEN
                uint32   endAt;            ///< when it was first seen standing
                uint32   legs;             ///< legs counted
                uint32   gaps;             ///< gaps counted (legs - 1)
                uint32   chained;          ///< of those, at or under 300 ms
                uint32   gapSum;           ///< for the mean
                uint32   reroutes;         ///< fresh goals taken while the unit was NOT at the running leg's endpoint
                float    worstReroute;     ///< the farthest of them, for the verdict
                Movement::Vector3 goal;    ///< the goal of the leg under way
                // The speed
                float    before;           ///< the run speed before the fear landed
                float    minFeared;        ///< the least and the most it read while feared
                float    maxFeared;
                uint32   fearedSamples;
                float    afterRecalc;      ///< what it read on the sample after the bare UpdateSpeed
                bool     recalcSeen;
                float    after;            ///< the run speed once the fear had gone
                bool     afterSeen;
                bool     held;             ///< the claim was held on at least one sample
                bool     heldAtRelease;    ///< ReasonFeared, read in the release step BEFORE the release
                bool     auraAtRelease;    ///< IsFearedByAura(), likewise
            };
            Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
            if (!a || !k) { Verdict("chainedGaps=INVALID(spawn failed) | fearRunSpeed=INVALID(spawn failed) | speedRestored=INVALID(spawn failed)"); return; }
            Silence(a);
            Silence(k);
            const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->running = st->haveEnd = st->recalcSeen = st->afterSeen = st->held = false;
            st->heldAtRelease = st->auraAtRelease = false;
            st->endAt = st->legs = st->gaps = st->chained = st->gapSum = st->fearedSamples = st->reroutes = 0;
            st->before = st->after = st->afterRecalc = st->worstReroute = 0.0f;
            st->minFeared = 1.0e9f;
            st->maxFeared = -1.0e9f;

            At(500, [this, g, gk, st]()
            {
                Creature* a = Get(g); if (!a) { return; }
                st->before = a->GetSpeed(MOVE_RUN);
                a->SetFeared(true, gk, FEAR, 0, 0);
                Log("feared by the kobold 6 yd east: run speed %.4f -> %.4f yd/s (x%.3f), mt=%s",
                    st->before, a->GetSpeed(MOVE_RUN), st->before > 0.0f ? a->GetSpeed(MOVE_RUN) / st->before : 0.0f, TypeName(a));
            });
            // 100 ms for twenty seconds, the diagnostic's own cadence on the same bare map.
            for (uint32 i = 1; i <= 200; ++i)
            {
                At(500 + i * 100, [this, g, st, i]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    const uint32 t = i * 100;
                    if (!a->Blocked(Motion::ReasonFeared))
                    {
                        // The FEAR's cadence is what this measures, so the legs are counted only
                        // while the fear drives. Nothing should end it inside this window -- the
                        // claim is untimed and released at +20200 -- but if anything ever did,
                        // the home run and the wander that follow lay legs of their own, and
                        // counting those would put somebody else's cadence in the verdict.
                        return;
                    }
                    st->held = true;
                    ++st->fearedSamples;
                    {
                        const float s = a->GetSpeed(MOVE_RUN);
                        if (s < st->minFeared) { st->minFeared = s; }
                        if (s > st->maxFeared) { st->maxFeared = s; }
                    }
                    Movement::Vector3 goal;
                    const bool running = RunningGoal(a, goal);
                    if (!running)
                    {
                        if (st->running) { st->running = false; st->haveEnd = true; st->endAt = t; }
                        return;
                    }
                    // A leg is new when none ran at the last sample, or when the one that ran
                    // has a different destination -- the second half is what a CHAINED bolt
                    // looks like, laid inside one 100 ms window so the sampler never sees the
                    // unit standing. A goal kStopSplineYd from where the unit stands is the
                    // activation's stop spline, not a bolt: a stop ends exactly where the unit
                    // is, while the first sample of a real leg is at most 100 ms -- about a
                    // yard at the feared run -- along it.
                    if (st->running && SameGoal(goal, st->goal)) { return; }
                    if (Dist2(goal.x, goal.y, a->Where().X(), a->Where().Y()) <= kStopSplineYd) { return; }
                    // The soundness check this whole distribution rests on (kLegArrivedYd). A
                    // fresh goal while a leg was running is a CHAIN only if that leg finished,
                    // and the proof is the unit standing on its endpoint. Without this, a
                    // behaviour that re-picked every tick and never finished a leg -- exactly
                    // what happens if the traveling guard at ControlMoves.cpp:152-155 goes --
                    // would be scored as a healthy 40%-chained cadence, and the category would
                    // be emitting a number that looks like evidence and is not.
                    if (st->running)
                    {
                        const float fromEnd = Dist2(st->goal.x, st->goal.y, a->Where().X(), a->Where().Y());
                        if (fromEnd > kLegArrivedYd)
                        {
                            ++st->reroutes;
                            if (fromEnd > st->worstReroute) { st->worstReroute = fromEnd; }
                            if (st->reroutes <= 3)
                            {
                                Log("+%5ums RE-ROUTE %u: a fresh goal (%.1f, %.1f) while the unit is %.1f yd from the running leg's own endpoint -- that leg never finished",
                                    t, st->reroutes, goal.x, goal.y, fromEnd);
                            }
                            st->goal = goal;   // the leg that runs now; not a leg completed, not a gap
                            return;
                        }
                    }
                    uint32 gap = 0;
                    if (st->legs)
                    {
                        gap = st->haveEnd ? t - st->endAt : 0;   // never seen standing = chained inside the window
                        ++st->gaps;
                        st->gapSum += gap;
                        if (gap <= 300) { ++st->chained; }
                    }
                    ++st->legs;
                    st->running = true;
                    st->haveEnd = false;
                    st->goal = goal;
                    Log("+%5ums leg %u starts: goal (%.1f, %.1f), gap since the last leg %u ms%s", t, st->legs, goal.x, goal.y,
                        st->legs > 1 ? gap : 0u, (st->legs > 1 && gap <= 300) ? " (chained)" : "");
                });
            }
            // A bare recalculation mid-flee, with nothing else changed: the quarter must survive
            // it, because that is exactly what an unrelated aura landing on the unit would do.
            At(10500, [this, g, st]()
            {
                Creature* a = Get(g); if (!a) { return; }
                a->UpdateSpeed(MOVE_RUN, true);
                st->afterRecalc = a->GetSpeed(MOVE_RUN);
                st->recalcSeen = true;
                Log("+10000ms a bare UpdateSpeed(MOVE_RUN) mid-flee: run speed %.4f yd/s (x%.3f of the unfeared rate)",
                    st->afterRecalc, st->before > 0.0f ? st->afterRecalc / st->before : 0.0f);
            });
            At(20700, [this, g, gk, st]()
            {
                Creature* a = Get(g); if (!a) { return; }
                // Read BEFORE the release, and the verdict refuses to speak without them:
                // "unfeared and at base speed afterwards" is also what an aura that expired
                // early looks like, which is the very failure speedRestored exists to catch.
                st->heldAtRelease = a->Blocked(Motion::ReasonFeared);
                st->auraAtRelease = a->IsFearedByAura();
                a->SetFeared(false, gk, FEAR, 0, 0);
                Log("+20200ms the fear released (held just before it: feared=%d auraFeared=%d): feared=%d run speed %.4f yd/s, mt=%s",
                    st->heldAtRelease ? 1 : 0, st->auraAtRelease ? 1 : 0,
                    a->Blocked(Motion::ReasonFeared) ? 1 : 0, a->GetSpeed(MOVE_RUN), TypeName(a));
            });
            for (uint32 i = 1; i <= 5; ++i)
            {
                At(20700 + i * 100, [this, g, st]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    if (a->Blocked(Motion::ReasonFeared)) { return; }
                    st->after = a->GetSpeed(MOVE_RUN);
                    st->afterSeen = true;
                });
            }
            At(21400, [this, st]()
            {
                char cadence[280], speed[280], restored[200];
                // Retail: 52% of 27 gaps at or under 300 ms. Over a run this short the count is
                // a small sample of a one-in-two coin, so the band is wide on purpose: what it
                // must catch is a cadence with ONE mode again -- all rested (the state before
                // 2026-09-21: 0 of 6) or all chained.
                if (st->reroutes)
                {
                    // Legs that never finished: the share below would be computed over
                    // something that is not a cadence at all, so it is not computed.
                    snprintf(cadence, sizeof(cadence), "BUG(%u re-route(s), the farthest %.1f yd from the running leg's own endpoint: legs are not finishing, so there is no cadence to measure)",
                             st->reroutes, st->worstReroute);
                }
                else if (st->gaps < 4)
                {
                    snprintf(cadence, sizeof(cadence), "INVALID(only %u gaps over %u legs in 20 s)", st->gaps, st->legs);
                }
                else
                {
                    const float share = 100.0f * float(st->chained) / float(st->gaps);
                    snprintf(cadence, sizeof(cadence), "%s(%u of %u gaps at or under 300 ms = %.0f%%, retail 52%%; mean gap %u ms over %u legs, no re-routes)",
                             (share >= 20.0f && share <= 85.0f) ? "OK" : "BUG", st->chained, st->gaps, share, st->gapSum / st->gaps, st->legs);
                }
                if (!st->held || st->fearedSamples < 10 || st->before <= 0.0f)
                {
                    snprintf(speed, sizeof(speed), "INVALID(the fear never held: %u samples, unfeared speed %.4f)", st->fearedSamples, st->before);
                    snprintf(restored, sizeof(restored), "INVALID(the fear never held)");
                }
                else
                {
                    const float lo = st->minFeared / st->before, hi = st->maxFeared / st->before;
                    const float rc = st->recalcSeen ? st->afterRecalc / st->before : 0.0f;
                    const bool flat = lo > 1.2450f && hi < 1.2550f;
                    const bool survived = st->recalcSeen && rc > 1.2450f && rc < 1.2550f;
                    snprintf(speed, sizeof(speed), "%s(x%.3f-%.3f of the unfeared %.4f yd/s over %u samples, x%.3f after a bare recalculation; retail x1.250)",
                             (flat && survived) ? "OK" : "BUG", lo, hi, st->before, st->fearedSamples, rc);
                    if (!st->heldAtRelease || !st->auraAtRelease)
                    {
                        // The aura had already gone before the scheduled release, so whatever
                        // the rate reads afterwards cannot be attributed to the release: an
                        // aura that expired early would otherwise pass this category, and that
                        // is precisely what it is here to catch.
                        snprintf(restored, sizeof(restored), "INVALID(the fear was not held at the release: feared=%d auraFeared=%d just before it)",
                                 st->heldAtRelease ? 1 : 0, st->auraAtRelease ? 1 : 0);
                    }
                    else if (!st->afterSeen)
                    {
                        snprintf(restored, sizeof(restored), "INVALID(no sample after the release)");
                    }
                    else
                    {
                        const float back = st->after / st->before;
                        snprintf(restored, sizeof(restored), "%s(x%.3f of the unfeared rate within 500 ms of a release that lifted a held aura)",
                                 (back > 0.995f && back < 1.005f) ? "OK" : "BUG", back);
                    }
                }
                std::string text = std::string("chainedGaps=") + cadence + " | fearRunSpeed=" + speed + " | speedRestored=" + restored;
                Verdict(text);
            });
        }
    };

    /// S65 (the ruling of 2026-09-21): the OTHER half of the fear's x1.25 -- who must NOT get it.
    ///
    /// Creature::DoFleeToGetAssistance, the AI's own low-health runner, reaches Unit::SetFeared
    /// with no spell of its own and so raises Motion::ReasonFeared exactly as a fear aura does.
    /// It is not a fear EFFECT: retail's evidence for the quarter is entirely aura batches and
    /// the wiki's "All Fear effects", and we have none at all for a mob running away by itself.
    /// A gate on the reason would have given every low-health runner 0.66 x 1.25 = 0.825 of its
    /// rate -- and, because that flee is the TIMED variant and expires on its own without ever
    /// calling SetFeared(false), would have left the quarter hanging on it until something
    /// unrelated recalculated. The gate is the published auraFear instead, which reads the
    /// claim's own spell field and therefore goes when the claim goes, however it goes.
    ///
    /// Without this scenario the low-health path has no coverage at all and the next person to
    /// touch Unit::UpdateSpeed can put the reason back with nothing to stop them.
    class LowHealthFleeSpeed : public Scenario
    {
    public:
        LowHealthFleeSpeed() : Scenario("low-health-flee-speed", 64) {}

        void Prepare() override
        {
            /// The assistance cut Unit::UpdateSpeed applies to a creature that has searched for
            /// help ("best guessed value, so this will be 33% reduction"). It is what a
            /// low-health runner's rate SHOULD be; 0.66 x 1.25 = 0.825 is the regression.
            const float kAssistCut = 0.66f;
            struct St
            {
                float  before;         ///< the run speed before any of this
                float  minFlee;        ///< the least and the most it read while the flee's claim was held
                float  maxFlee;
                uint32 fleeSamples;    ///< samples with the claim held
                bool   auraEver;       ///< IsFearedByAura() on any of them (it must never be true)
                float  afterFlee;      ///< the rate on the first sample after the claim went, with NO recalculation in between
                bool   afterSeen;
                float  lifted;         ///< the rate once the assistance cut is lifted and the speed recalculated
                bool   liftedSeen;
                bool   victim;         ///< the wolf had a victim when the flee was asked for
            };
            Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
            if (!w || !k) { Verdict("aiFleeNoBoost=INVALID(spawn failed) | aiFleeRestores=INVALID(spawn failed)"); return; }
            w->SetMaxHealth(500000); w->SetHealth(500000); k->SetMaxHealth(500000); k->SetHealth(500000);
            Silence(w);
            Silence(k);
            const ObjectGuid g = w->GetObjectGuid(), gk = k->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->before = st->afterFlee = st->lifted = 0.0f;
            st->minFlee = 1.0e9f;
            st->maxFlee = -1.0e9f;
            st->fleeSamples = 0;
            st->auraEver = st->afterSeen = st->liftedSeen = st->victim = false;

            At(500, [this, g, gk, st]()
            {
                Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                st->before = w->GetSpeed(MOVE_RUN);
                w->Attack(k, true);
                w->AddThreat(k, 1000.0f);
                Log("Attack + AddThreat on the kobold 20 yd east: victim=%d run speed %.4f yd/s", w->getVictim() ? 1 : 0, st->before);
            });
            At(1000, [this, g, st]()
            {
                Creature* w = Get(g); if (!w) { return; }
                st->victim = w->getVictim() != NULL;
                // The AI's own entry point, not Unit::SetFeared: the whole point is that this
                // path reaches SetFeared by itself, with no spell, and must be told apart there.
                w->DoFleeToGetAssistance();
                Log("DoFleeToGetAssistance: feared=%d auraFeared=%d run speed %.4f yd/s (x%.3f), mt=%s",
                    w->Blocked(Motion::ReasonFeared) ? 1 : 0, w->IsFearedByAura() ? 1 : 0, w->GetSpeed(MOVE_RUN),
                    st->before > 0.0f ? w->GetSpeed(MOVE_RUN) / st->before : 0.0f, TypeName(w));
            });
            // The flee lasts CreatureFamilyFleeDelay (7000 ms by default), so it ends around
            // +8000; the sampling runs past that to catch the rate with the claim gone.
            for (uint32 i = 1; i <= 79; ++i)
            {
                At(1000 + i * 100, [this, g, st, i]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    const uint32 t = i * 100;
                    const float s = w->GetSpeed(MOVE_RUN);
                    if (w->Blocked(Motion::ReasonFeared))
                    {
                        ++st->fleeSamples;
                        if (w->IsFearedByAura()) { st->auraEver = true; }
                        if (s < st->minFlee) { st->minFlee = s; }
                        if (s > st->maxFlee) { st->maxFlee = s; }
                    }
                    else if (st->fleeSamples && !st->afterSeen)
                    {
                        // The first reading after the claim went, and nothing has recalculated
                        // the speed in between: a quarter left hanging would still be here.
                        st->afterSeen = true;
                        st->afterFlee = s;
                        Log("+%5ums the flee's claim has gone: run speed %.4f yd/s (x%.3f), mt=%s", t, s,
                            st->before > 0.0f ? s / st->before : 0.0f, TypeName(w));
                    }
                    if (i % 20 == 0)
                    {
                        Log("+%5ums feared=%d auraFeared=%d run speed %.4f yd/s (x%.3f)", t, w->Blocked(Motion::ReasonFeared) ? 1 : 0,
                            w->IsFearedByAura() ? 1 : 0, s, st->before > 0.0f ? s / st->before : 0.0f);
                    }
                });
            }
            At(9100, [this, g, st]()
            {
                Creature* w = Get(g); if (!w) { return; }
                // Lift the assistance cut and recalculate: with nothing of the flee left, the
                // rate must be exactly what it was before any of this.
                w->SetNoSearchAssistance(false);
                w->UpdateSpeed(MOVE_RUN, true);
                st->lifted = w->GetSpeed(MOVE_RUN);
                st->liftedSeen = true;
                Log("+ 8100ms the assistance cut lifted and the speed recalculated: %.4f yd/s (x%.3f)",
                    st->lifted, st->before > 0.0f ? st->lifted / st->before : 0.0f);
            });
            At(9500, [this, st, kAssistCut]()
            {
                char noBoost[300], restores[260];
                if (!st->victim || st->fleeSamples < 10 || st->before <= 0.0f)
                {
                    // No victim, or an assistant was found within 30 yd and MoveSeekAssistance
                    // was taken instead: the fear path never ran and there is nothing to read.
                    snprintf(noBoost, sizeof(noBoost), "INVALID(the low-health flee never held: victim=%d, %u samples, base %.4f)",
                             st->victim ? 1 : 0, st->fleeSamples, st->before);
                    snprintf(restores, sizeof(restores), "INVALID(the low-health flee never held)");
                }
                else
                {
                    const float lo = st->minFlee / st->before, hi = st->maxFlee / st->before;
                    const bool flat = lo > kAssistCut - 0.01f && hi < kAssistCut + 0.01f;
                    snprintf(noBoost, sizeof(noBoost),
                             "%s(x%.3f-%.3f of the unfeared %.4f yd/s over %u samples with ReasonFeared held, auraFeared %s; the assistance cut x%.2f, NOT x%.3f)",
                             (flat && !st->auraEver) ? "OK" : "BUG", lo, hi, st->before, st->fleeSamples,
                             st->auraEver ? "TRUE" : "never true", kAssistCut, kAssistCut * 1.25f);
                    if (!st->afterSeen || !st->liftedSeen)
                    {
                        snprintf(restores, sizeof(restores), "INVALID(the flee had not ended by +8000ms: afterSeen=%d liftedSeen=%d)",
                                 st->afterSeen ? 1 : 0, st->liftedSeen ? 1 : 0);
                    }
                    else
                    {
                        const float after = st->afterFlee / st->before, back = st->lifted / st->before;
                        const bool nothingStuck = after > kAssistCut - 0.01f && after < kAssistCut + 0.01f;
                        const bool restored = back > 0.995f && back < 1.005f;
                        snprintf(restores, sizeof(restores),
                                 "%s(x%.3f with the claim gone and nothing recalculated, x%.3f once the assistance cut is lifted)",
                                 (nothingStuck && restored) ? "OK" : "BUG", after, back);
                    }
                }
                Verdict(std::string("aiFleeNoBoost=") + noBoost + " | aiFleeRestores=" + restores);
            });
        }
    };

    /// S66 (the possession ruling of 2026-09-21): the fear's x1.25 must follow the CLAIM, by
    /// every route the claim can end, not only the two that go through Unit::SetFeared.
    ///
    /// The case that prompted it: Unit::TakePossessOf, when a player takes his own pet, calls
    /// `possessed->GetMotionMaster()->CancelControl(Motion::Kind::Fear)` (Unit.cpp:7162). That
    /// ends the claim without ever reaching SetFeared(false), so the published auraFear drops
    /// and, before the fix, nothing recalculated the run speed: the quarter stayed on the unit
    /// until something unrelated happened to recompute it.
    ///
    /// Patching that one call was refused, and rightly: it is merely the third route anyone has
    /// thought of (round 1 gated on the wrong flag and missed the AI flee, round 2 fixed
    /// SetFeared's two routes and missed this). The speed now follows the published flag --
    /// MotionMaster::Publish recalculates MOVE_RUN whenever auraFear CHANGES -- so Release,
    /// CancelControl, Clear(true), the death and anything nobody has enumerated are all covered
    /// by construction. This scenario proves two routes that Unit::SetFeared does not own.
    ///
    /// WHAT IT DOES NOT REACH, and no longer cannot: the real pet-possession path needs a Player
    /// possessing HIS OWN pet (Unit.cpp:7157 `ownPet`), and a creature possessing a creature,
    /// which S34 exercises, never enters that branch. This scenario makes the same facade call
    /// TakePossessOf makes, on the same arbiter, rather than building a pet for it -- the call
    /// under test is identical; only its preconditions are not -- and the note used to add that
    /// the harness could not build one. It can, since 2026-09-21: S73
    /// player-owned-pet-possession (order 907) builds the pet and enters the branch itself. The
    /// division of labour is deliberate and the two do not overlap: S73 asks whether the take
    /// ENDS the claim, and this one asks what the RUN SPEED does when a claim ends by a route
    /// Unit::SetFeared does not own, which is a question about MotionMaster::Publish and needs no
    /// pet at all.
    class FearSpeedFollowsTheClaim : public Scenario
    {
    public:
        FearSpeedFollowsTheClaim() : Scenario("fear-speed-follows-the-claim", 65) {}

        void Prepare() override
        {
            struct St
            {
                float  before;        ///< the run speed with no fear at all
                float  boostedA;      ///< with the first fear held
                float  afterCancel;   ///< the first reading after CancelControl, with nothing else touched
                float  worstCancel;   ///< the highest reading over the whole window after it
                bool   auraA;         ///< IsFearedByAura() just before the cancel
                bool   cancelSeen;
                float  boostedB;      ///< with the second fear held
                float  afterDeath;    ///< the first reading after the death, likewise untouched
                float  worstDeath;
                bool   auraB;         ///< IsFearedByAura() just before the death
                bool   deathSeen;
                bool   diedClean;     ///< the wolf really did die
            };
            Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
            if (!w || !k) { Verdict("possessTakeRestores=INVALID(spawn failed) | deathRouteRestores=INVALID(spawn failed)"); return; }
            Silence(w);
            Silence(k);
            const ObjectGuid g = w->GetObjectGuid(), gk = k->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->before = st->boostedA = st->afterCancel = st->boostedB = st->afterDeath = 0.0f;
            st->worstCancel = st->worstDeath = 0.0f;
            st->auraA = st->auraB = st->cancelSeen = st->deathSeen = st->diedClean = false;

            At(500, [this, g, gk, st]()
            {
                Creature* w = Get(g); if (!w) { return; }
                st->before = w->GetSpeed(MOVE_RUN);
                w->SetFeared(true, gk, FEAR, 0, 0);
                Log("feared: run speed %.4f -> %.4f yd/s, auraFeared=%d", st->before, w->GetSpeed(MOVE_RUN), w->IsFearedByAura() ? 1 : 0);
            });
            // ROUTE 1: the possession take's own call, on the same facade, mid-fear.
            At(1000, [this, g, st]()
            {
                Creature* w = Get(g); if (!w) { return; }
                st->boostedA = w->GetSpeed(MOVE_RUN);
                st->auraA = w->IsFearedByAura();
                w->GetMotionMaster()->CancelControl(Motion::Kind::Fear);
                Log("+ 500ms CancelControl(Fear), as TakePossessOf calls it: auraFeared %d -> %d, run speed %.4f -> %.4f yd/s",
                    st->auraA ? 1 : 0, w->IsFearedByAura() ? 1 : 0, st->boostedA, w->GetSpeed(MOVE_RUN));
            });
            for (uint32 i = 1; i <= 5; ++i)
            {
                At(1000 + i * 100, [this, g, st]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    // Nothing in this window touches the unit: no aura lands, no speed is set,
                    // no recalculation is asked for. Whatever the rate reads here is what the
                    // cancel left behind.
                    const float s = w->GetSpeed(MOVE_RUN);
                    if (!st->cancelSeen) { st->cancelSeen = true; st->afterCancel = s; }
                    if (s > st->worstCancel) { st->worstCancel = s; }
                });
            }
            // ROUTE 2: the death, which ends every claim through Arbiter::Die and likewise never
            // reaches Unit::SetFeared. A fresh fear first, so there is a quarter to give back.
            At(2000, [this, g, gk, st]()
            {
                Creature* w = Get(g); if (!w) { return; }
                w->SetFeared(true, gk, FEAR, 0, 0);
                Log("+1500ms feared again: run speed %.4f yd/s, auraFeared=%d", w->GetSpeed(MOVE_RUN), w->IsFearedByAura() ? 1 : 0);
            });
            At(2500, [this, g, st]()
            {
                Creature* w = Get(g); if (!w) { return; }
                st->boostedB = w->GetSpeed(MOVE_RUN);
                st->auraB = w->IsFearedByAura();
                w->DealDamage(w, w->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
                st->diedClean = !w->IsAlive();
                Log("+2000ms killed mid-fear: alive=%d auraFeared %d -> %d, run speed %.4f -> %.4f yd/s",
                    w->IsAlive() ? 1 : 0, st->auraB ? 1 : 0, w->IsFearedByAura() ? 1 : 0, st->boostedB, w->GetSpeed(MOVE_RUN));
            });
            for (uint32 i = 1; i <= 5; ++i)
            {
                At(2500 + i * 100, [this, g, st]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    const float s = w->GetSpeed(MOVE_RUN);
                    if (!st->deathSeen) { st->deathSeen = true; st->afterDeath = s; }
                    if (s > st->worstDeath) { st->worstDeath = s; }
                });
            }
            At(3400, [this, st]()
            {
                char cancel[300], death[300];
                if (st->before <= 0.0f || !st->auraA || st->boostedA < st->before * 1.2f || !st->cancelSeen)
                {
                    // Without a boost to give back there is nothing to test: say so rather than
                    // pass because the rate happened to be right all along.
                    snprintf(cancel, sizeof(cancel), "INVALID(no aura fear to cancel: base %.4f, auraFeared=%d, boosted %.4f, samples=%d)",
                             st->before, st->auraA ? 1 : 0, st->boostedA, st->cancelSeen ? 1 : 0);
                }
                else
                {
                    const float held = st->boostedA / st->before, back = st->afterCancel / st->before, worst = st->worstCancel / st->before;
                    snprintf(cancel, sizeof(cancel), "%s(x%.3f while held, x%.3f on the first reading after CancelControl and never above x%.3f, with nothing else touched)",
                             (back > 0.995f && back < 1.005f && worst < 1.005f) ? "OK" : "BUG", held, back, worst);
                }
                if (!st->auraB || !st->diedClean || st->boostedB < st->before * 1.2f || !st->deathSeen)
                {
                    snprintf(death, sizeof(death), "INVALID(no aura fear to end by the death: auraFeared=%d died=%d boosted %.4f samples=%d)",
                             st->auraB ? 1 : 0, st->diedClean ? 1 : 0, st->boostedB, st->deathSeen ? 1 : 0);
                }
                else
                {
                    const float held = st->boostedB / st->before, back = st->afterDeath / st->before, worst = st->worstDeath / st->before;
                    snprintf(death, sizeof(death), "%s(x%.3f while held, x%.3f on the first reading after the death and never above x%.3f)",
                             (back > 0.995f && back < 1.005f && worst < 1.005f) ? "OK" : "BUG", held, back, worst);
                }
                Verdict(std::string("possessTakeRestores=") + cancel + " | deathRouteRestores=" + death);
            });
        }
    };

    /// S66 (the live capture of 2026-09-20, `server-release/mvcapture.log`): a feared unit that
    /// has reached the edge of the quiet band lays a CASCADE of legs too short to be legs at
    /// all, and every one of them is an SMSG_MONSTER_MOVE to every observer.
    ///
    /// Two episodes of creature 3123/251013 in that capture, decoded off the wire (the layout
    /// is confirmed against the 15595 binary, peer/monster-move-wire-decompile-2026-09-21.md):
    ///
    ///     1.868 yd/144 ms  1.386/107  0.472/37  0.108/9  0.058/5  0.085/7   then 15.582/1197
    ///     3.451 yd/266 ms  3.324/256  1.693/131 [9.345/718] 1.681/130  0.364/28  0.178/14  0.013/2
    ///
    /// Each leg starts exactly where the last one ended and moves the unit RADIALLY AWAY from
    /// the fright by its own length, converging on a fixed radius -- the signature of
    /// FearBehaviour::PickFleePoint's close branch, whose draw is
    /// `Frand(0.4, 1.3) * (minQuiet - distFromCaster)` and therefore collapses geometrically to
    /// nothing as the unit approaches minQuiet. Retail never does this: the 27 flee legs
    /// measured across 11 MOD_FEAR episodes (peer/retail-fear-movement-2026-09-20.md §3) run
    /// 2.62-38.03 yd and 313-5245 ms, with NOTHING below 2.62 yd.
    ///
    /// The fixture is the user's own case: a creature that is chasing when the fear lands, and
    /// that is handed back to the chase when it lifts. Thirty seconds is long enough for the
    /// wolf to bolt out of the close band and settle against it, which is where the cascade
    /// lives; six seconds of fear (S54's window) never gets there.
    ///
    /// Legs are counted by the spline's OWN id, not by sampling a running goal: a 5 ms leg is
    /// finalized long before the next 100 ms sample and a goal-watcher cannot see it at all.
    /// That is why S63's cadence counter reports eight legs over a window that puts dozens on
    /// the wire, and why this scenario had to be written instead of extending it.
    class FearMicroLegs : public Scenario
    {
    public:
        FearMicroLegs() : Scenario("fear-micro-legs", 66) {}

        void Prepare() override
        {
            /// Retail's shortest measured flee leg is 2.62 yd (§3 above). A leg under this is
            /// not a bolt; it is a packet.
            const float kMinBolt = 2.5f;
            const uint32 kFearAt = 700;
            const uint32 kSamples = 300;            // 30 s of fear at the 100 ms cadence
            const uint32 kReleaseAt = kFearAt + kSamples * 100 + 100;
            const uint32 kAfterSamples = 30;        // 3 s of whatever follows the fear

            struct St
            {
                uint32 lastId;         ///< the spline id at the previous sample
                bool   haveId;
                uint32 legs;           ///< flee legs laid while the fear held
                uint32 shortLegs;      ///< of those, under kMinBolt
                uint32 burst;          ///< the run of short legs under way
                uint32 worstBurst;     ///< the longest such run
                float  shortest;       ///< the shortest leg seen, in yards
                int32  shortestMs;     ///< and its duration
                float  shortestAt;     ///< how far the unit stood from the fright when it was laid
                uint32 logged;         ///< short legs printed (the first dozen)
                bool   released;
                uint32 afterLegs;      ///< legs laid after the release
                float  firstAfter;     ///< the first one's length
                int32  firstAfterMs;
                Motion::Kind afterKind;
            };

            Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
            if (!w || !k) { Verdict("fearLegLengths=INVALID(spawn failed) | chaseAfterFear=INVALID(spawn failed)"); return; }
            Silence(w);
            Silence(k);
            const ObjectGuid g = w->GetObjectGuid(), gk = k->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->lastId = 0;
            st->haveId = st->released = false;
            st->legs = st->shortLegs = st->burst = st->worstBurst = st->logged = st->afterLegs = 0;
            st->shortest = 1.0e9f;
            st->shortestMs = 0;
            st->shortestAt = 0.0f;
            st->firstAfter = 0.0f;
            st->firstAfterMs = 0;
            st->afterKind = Motion::Kind::Idle;

            At(300, [this, g, gk]()
            {
                Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                // Ranged, as the tracking family does it: the victim the chase needs, without
                // handing the melee bit out before the wolf is in reach.
                w->Attack(k, false);
                w->AddThreat(k, 1000.0f);
                w->GetMotionMaster()->MoveChase(k);
                Log("chasing the kobold from %.1f yd, mt=%s", Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y()), TypeName(w));
            });
            At(kFearAt, [this, g, gk]()
            {
                Creature* w = Get(g); if (!w) { return; }
                w->SetFeared(true, gk, FEAR, 0, 0);
                Log("feared by the kobold it was chasing: mt=%s, run %.4f yd/s", TypeName(w), w->GetSpeed(MOVE_RUN));
            });
            // The leg detector. A launched spline takes a fresh id, so a new id at a sample is
            // a leg that was laid since the last one -- whether or not it is still running. Its
            // LENGTH is the wire's own: Duration() x Velocity(), the two numbers the
            // SMSG_MONSTER_MOVE carries. A stop is not a leg (zero duration, born cut).
            for (uint32 i = 1; i <= kSamples + kAfterSamples + 2; ++i)
            {
                At(kFearAt + i * 100, [this, g, gk, st, i, kMinBolt]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    const uint32 t = i * 100;
                    Movement::MoveSpline const& s = *w->movespline;
                    if (!s.Initialized()) { return; }
                    const uint32 id = s.GetId();
                    if (st->haveId && id == st->lastId) { return; }
                    st->haveId = true;
                    st->lastId = id;
                    const int32 ms = s.Duration();
                    if (ms <= 0 || s.Cut()) { return; }              // a stop, not a bolt
                    const float len = float(ms) * s.Velocity() / 1000.0f;
                    if (!st->released)
                    {
                        if (!w->Blocked(Motion::ReasonFeared)) { return; }   // only the flee's own legs
                        ++st->legs;
                        if (len < kMinBolt)
                        {
                            ++st->shortLegs;
                            ++st->burst;
                            if (st->burst > st->worstBurst) { st->worstBurst = st->burst; }
                            const float from = Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y());
                            if (len < st->shortest) { st->shortest = len; st->shortestMs = ms; st->shortestAt = from; }
                            if (++st->logged <= 12)
                            {
                                Log("+%5ums MICRO LEG %u: %.3f yd in %d ms, laid %.1f yd from the fright", t, st->shortLegs, len, ms, from);
                            }
                        }
                        else
                        {
                            st->burst = 0;
                        }
                        return;
                    }
                    if (!st->afterLegs)
                    {
                        st->firstAfter = len;
                        st->firstAfterMs = ms;
                        st->afterKind = Type(w);
                        Log("+%5ums the first leg after the fear: %.1f yd in %d ms, mt=%s", t, len, ms, TypeName(w));
                    }
                    ++st->afterLegs;
                });
            }
            At(kReleaseAt, [this, g, gk, st]()
            {
                Creature* w = Get(g); if (!w) { return; }
                w->SetFeared(false, gk, FEAR, 0, 0);
                st->released = true;
                Log("the fear released after %u flee legs (%u of them under the retail floor), mt=%s", st->legs, st->shortLegs, TypeName(w));
            });
            At(kReleaseAt + kAfterSamples * 100 + 200, [this, st, kMinBolt]()
            {
                char legs[360], after[300];
                if (st->legs < 8)
                {
                    snprintf(legs, sizeof(legs), "INVALID(only %u flee legs in 30 s: the fear never ran)", st->legs);
                }
                else if (st->shortLegs)
                {
                    snprintf(legs, sizeof(legs),
                             "BUG(%u of %u flee legs under %.1f yd, the longest run %u back to back; the shortest %.3f yd in %d ms, laid %.1f yd from the fright -- retail's shortest of 27 measured is 2.62 yd)",
                             st->shortLegs, st->legs, kMinBolt, st->worstBurst, st->shortest, st->shortestMs, st->shortestAt);
                }
                else
                {
                    snprintf(legs, sizeof(legs), "OK(none of %u flee legs under %.1f yd; retail's 27 measured run 2.62-38.03 yd)", st->legs, kMinBolt);
                }
                if (!st->afterLegs)
                {
                    snprintf(after, sizeof(after), "INVALID(no leg in the 3 s after the release)");
                }
                else
                {
                    // Retail hands a fear back to the chase as ONE long face-target leg
                    // (peer/retail-fear-movement-2026-09-20.md Case A: type=3, 31.11 yd at
                    // 6.64 yd/s). What must not happen is the handback stuttering.
                    const bool ok = st->afterKind == Motion::Kind::Chase && st->firstAfter >= 5.0f;
                    snprintf(after, sizeof(after), "%s(mt=%s, %.1f yd in %d ms, %u leg(s) in the 3 s after the release)",
                             ok ? "OK" : "BUG", Motion::KindName(st->afterKind), st->firstAfter, st->firstAfterMs, st->afterLegs);
                }
                Verdict(std::string("fearLegLengths=") + legs + " | chaseAfterFear=" + after);
            });
        }
    };


    namespace
    {
        /// Whether the player is his own mover, read where the handoff actually writes it:
        /// the session's authority set, which WorldSession::GrantMover adds the guid to and
        /// RevokeMover drops it from, and the unit's own mover session, which the same pair
        /// sets and clears (WorldSession.cpp:145-167). Both halves, because RevokeMover clears
        /// the pointer only while it is this session's to clear -- reading either alone would
        /// call a half-done handoff finished. Nothing else is consulted: the packet the
        /// transition sends goes nowhere on a socketless session, and UNIT_FLAG_FLEEING answers
        /// a different question.
        bool OwnMover(Player* p)
        {
            WorldSession* s = p->GetSession();
            return s && s->Movers().IsMember(p->GetObjectGuid().GetRawValue()) && p->MoverSession() == s;
        }

        /// player-fear's two fixed moments, as the TIMELINE counts them: absolute offsets from
        /// the scenario's own start, which is what Scenario::At takes.
        ///
        /// ONE TIME BASE IN THE LOG, and it is not this one. Every "+Nms" line the scenario
        /// prints is N ms AFTER THE CAST, because that is the event every reading is about; the
        /// sampler's own `t` has always been that, and a step that logs an absolute moment must
        /// subtract the cast. Mixing the two is how the record came to carry a line reading
        /// "+4000ms the aura pulled" for something that happened 3500 ms after the cast.
        const uint32 kFearCastAt = 500;
        const uint32 kFearPullAt = 4000;
    }

    /// S63 (the harness's first player): the fear's CONTROL HANDOFF, the half of Unit::SetFeared
    /// no creature can exercise. A fear landing on a player revokes the client's authority over
    /// him before the flee leg is laid (UnitSpeed.cpp:305-311) and the last one going gives it
    /// back (UnitSpeed.cpp:357-364); until today the only witness to either was a human in a
    /// game client. A kobold casts 5782 -- the spell, as S60 does, not the entry point every
    /// other scenario calls -- at a session-less harness player, the scenario samples for six
    /// seconds and pulls the aura 3.5 s after the cast (kFearPullAt, absolute +4 s) so the
    /// handback falls inside that window rather than waiting on the spell's own duration.
    class PlayerFear : public Scenario
    {
    public:
        /// Order 900, not 63. Player scenarios take the 900 block precisely so they always sort
        /// last: the runner refuses `MVTEST all` when a player scenario is queued before one
        /// that holds no player (Harness.cpp Start), and with contiguous orders 1..63 the very
        /// next family registered would take 64, fire that guard and break `all` until somebody
        /// renumbered. A reserved high block makes the ordering promise hold by construction, so
        /// the creature families can go on growing from 64 without ever colliding with it.
        PlayerFear() : Scenario("player-fear", 900) {}

        /// The runner owes a player scenario two things: the last place in the queue and the
        /// map's grids reset behind it. A player in world promotes the grids around him to full
        /// state and changes Map::Update's own visitation order, and no scenario that holds none
        /// may read that.
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                float  x0, y0;            ///< where he stood when the fear landed
                float  far2;              ///< the farthest he got from there
                bool   selfBefore;        ///< his own mover before the cast (the spawn's grant held)
                bool   auraLanded;        ///< the 5782 holder was on him 100 ms after the cast
                uint32 fearedSamples;     ///< samples with the fear's claim held
                uint32 afterSamples;      ///< samples since the claim was LAST seen held
                bool   selfWhileFeared;   ///< still his own mover on one of those first samples
                bool   allSelfAfter;      ///< his own mover on every one of the second
                bool   taxi;              ///< IsTaxiFlying() on any sample
                uint32 taxiSamples;       ///< how many times it was read at all: zero is not "false"
                uint32 endedAt;           ///< when the claim went for the last time
                bool   pulled;            ///< the scenario's own RemoveAurasDueToSpell has run
                uint32 flickers;          ///< times the claim went unpublished BEFORE the pull and came back
                uint32 discarded;         ///< samples those re-anchorings took back out of the after-window
                uint32 backAfterPull;     ///< samples with the claim published AFTER the pull: a failure, never a flicker
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* k = p ? Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f) : NULL;
            if (!p || !k)
            {
                Verdict("controlTaken=INVALID(spawn failed) | playerFlees=INVALID(spawn failed) | controlReturned=INVALID(spawn failed) | notHeldByTaxi=INVALID(spawn failed)");
                return;
            }
            Silence(k);
            const ObjectGuid g = p->GetObjectGuid(), gk = k->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->x0 = st->y0 = st->far2 = 0.0f;
            st->selfBefore = st->auraLanded = st->selfWhileFeared = st->taxi = false;
            st->allSelfAfter = true;
            st->fearedSamples = st->afterSamples = st->taxiSamples = st->endedAt = 0;
            st->pulled = false;
            st->flickers = st->discarded = st->backAfterPull = 0;
            Log("the player %s stands at (%.1f, %.1f), the kobold 6 yd east", g.GetString().c_str(), p->Where().X(), p->Where().Y());

            At(kFearCastAt, [this, g, gk, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* k = Get(gk);
                if (!p || !k) { return; }
                st->x0 = p->Where().X();
                st->y0 = p->Where().Y();
                // Read before the cast, because it is the whole meaning of the revoke that
                // follows: a player who was never his own mover cannot have it taken from him,
                // and a scenario that skipped this would pass on a server that never granted.
                st->selfBefore = OwnMover(p);
                if (p->IsTaxiFlying()) { st->taxi = true; }
                ++st->taxiSamples;
                k->CastSpell(p, FEAR, true);
                Log("the kobold casts %u on the player at (%.1f, %.1f): his own mover before it=%d", FEAR, st->x0, st->y0, st->selfBefore ? 1 : 0);
            });
            At(kFearCastAt + 100, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->auraLanded = p->HasAura(FEAR);
                Log("+ 100ms after the cast: aura=%d feared=%d rooted=%d his own mover=%d", st->auraLanded ? 1 : 0,
                    p->Blocked(Motion::ReasonFeared) ? 1 : 0, p->IsRooted() ? 1 : 0, OwnMover(p) ? 1 : 0);
            });
            // Registered before the sampler so it runs first at its own moment (the timeline
            // orders a tie by insertion): the sample at +3500 after the cast is then already an
            // after-sample.
            At(kFearPullAt, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                p->RemoveAurasDueToSpell(FEAR);
                // The moment that divides the scenario in two for the sampler below: before it,
                // a gap in the claim is a flicker inside a fear that is still meant to be
                // running; after it, the claim coming back at all is a failure.
                st->pulled = true;
                Log("+%4ums after the cast the aura pulled: aura=%d feared=%d his own mover=%d taxi=%d",
                    kFearPullAt - kFearCastAt, p->HasAura(FEAR) ? 1 : 0,
                    p->Blocked(Motion::ReasonFeared) ? 1 : 0, OwnMover(p) ? 1 : 0, p->IsTaxiFlying() ? 1 : 0);
            });
            for (uint32 i = 1; i <= 60; ++i)
            {
                At(kFearCastAt + i * 100, [this, g, st, i]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    const uint32 t = i * 100;
                    const float d = Dist2(st->x0, st->y0, p->Where().X(), p->Where().Y());
                    if (d > st->far2) { st->far2 = d; }
                    // Sampled, not assumed, and COUNTED: the handback at UnitSpeed.cpp:361 is
                    // refused under a flight, so a taxi anywhere in the run would be an
                    // alternative explanation for every other reading here -- and a run that
                    // never read the flag at all has not established that it was false, which
                    // is why the verdict below asks how many of these there were.
                    if (p->IsTaxiFlying()) { st->taxi = true; }
                    ++st->taxiSamples;
                    const bool self = OwnMover(p);
                    const bool claimHeld = p->Blocked(Motion::ReasonFeared);
                    if (claimHeld && !st->pulled)
                    {
                        ++st->fearedSamples;
                        if (self) { st->selfWhileFeared = true; }
                        // THE AFTER-WINDOW IS ANCHORED TO THE END OF THE FEAR, not to the first
                        // gap in it. A claim that reads unpublished on one sample and published
                        // again on the next -- BEFORE the scenario pulls the aura, while the
                        // fear is still meant to be running -- was a flicker, not the handback,
                        // and everything counted since that gap belongs to the fear's own
                        // window, where the control is CORRECTLY revoked. Counted as
                        // after-samples they would read "not his own mover after the aura went"
                        // and this category would print BUG on a working server. So the window
                        // re-anchors here, and what it discards is counted and said.
                        if (st->afterSamples)
                        {
                            ++st->flickers;
                            st->discarded += st->afterSamples;
                        }
                        st->afterSamples = 0;
                        st->endedAt = 0;
                        st->allSelfAfter = true;
                    }
                    else if (claimHeld)
                    {
                        // The claim is published AFTER the scenario pulled the aura. That is not
                        // a flicker in anything: the fear was ended, on purpose, at a moment this
                        // scenario chose, so a claim standing again is a failure in its own right
                        // and is reported as one. Above all it must NOT re-anchor -- the failing
                        // control samples gathered since the pull are exactly the evidence a
                        // re-anchoring would erase, turning a genuine BUG into OK, which is the
                        // worse direction to be wrong in: a test that cries wolf gets looked at,
                        // one that sleeps does not.
                        // Nor does it touch fearedSamples or selfWhileFeared. Those two answer
                        // controlTaken, which asks about the revoke at ONSET; a stale claim long
                        // after the pull is controlReturned's finding, and letting it reach the
                        // other category would make one event print BUG twice, once falsely.
                        ++st->backAfterPull;
                    }
                    else if (st->fearedSamples)
                    {
                        if (!st->endedAt) { st->endedAt = t; }
                        ++st->afterSamples;
                        if (!self) { st->allSelfAfter = false; }
                    }
                    if (i % 10 == 0)
                    {
                        Log("+%4ums %.1f yd out (farthest %.1f), feared=%d his own mover=%d", t, d, st->far2,
                            p->Blocked(Motion::ReasonFeared) ? 1 : 0, self ? 1 : 0);
                    }
                });
            }
            At(kFearCastAt + 6100, [this, st]()
            {
                char taken[200], flees[96], returned[288], taxi[112];
                if (!st->fearedSamples)
                {
                    snprintf(taken, sizeof(taken), "INVALID(the fear never held: aura=%d)", st->auraLanded ? 1 : 0);
                    snprintf(flees, sizeof(flees), "INVALID(the fear never held)");
                    snprintf(returned, sizeof(returned), "INVALID(the fear never held)");
                }
                else
                {
                    if (!st->selfBefore)
                    {
                        snprintf(taken, sizeof(taken), "BUG(he was not his own mover before the cast, so the revoke had nothing to take)");
                    }
                    else
                    {
                        snprintf(taken, sizeof(taken), "%s(his own mover before the cast, the session's authority %s him on all %u samples while feared)",
                                 st->selfWhileFeared ? "BUG" : "OK", st->selfWhileFeared ? "still on" : "off", st->fearedSamples);
                    }
                    // Ten yards is the line between fleeing and standing, as S60 reads it: a
                    // bolt is 11-36 yd and the first is under way well inside four seconds.
                    snprintf(flees, sizeof(flees), "%s(%.1f yd from where he was feared)", st->far2 >= 10.0f ? "OK" : "BUG", st->far2);
                    // The flicker note, appended to whichever branch fires below. A claim that
                    // went unpublished mid-fear and came back is worth seeing even though the
                    // window no longer counts it -- it is a finding about the claim, just not
                    // this category's -- and on a healthy run there is nothing to say, so the
                    // line reads as it always has.
                    char flicker[112];
                    if (st->flickers)
                    {
                        snprintf(flicker, sizeof(flicker), "; the claim flickered %u time(s) before the pull, re-anchoring the window past %u sample(s)",
                                 st->flickers, st->discarded);
                    }
                    else
                    {
                        flicker[0] = '\0';
                    }
                    // First, and ahead of the sample-count INVALID: a claim standing again after
                    // the aura was pulled is a hard failure, and reporting "too few samples"
                    // over the top of it would hide the one thing that went wrong.
                    if (st->backAfterPull)
                    {
                        snprintf(returned, sizeof(returned), "BUG(the fear's claim was published again on %u sample(s) AFTER the aura was pulled, so the fear did not end when it was ended; of the %u control sample(s) since the pull he was his own mover on %s%s)",
                                 st->backAfterPull, st->afterSamples, st->allSelfAfter ? "all" : "not all", flicker);
                    }
                    else if (st->afterSamples < 5)
                    {
                        snprintf(returned, sizeof(returned), "INVALID(only %u samples after the aura went%s)", st->afterSamples, flicker);
                    }
                    else if (st->allSelfAfter)
                    {
                        snprintf(returned, sizeof(returned), "OK(his own mover again from the first of the %u samples after the aura went, %u ms after the cast%s)",
                                 st->afterSamples, st->endedAt, flicker);
                    }
                    else
                    {
                        snprintf(returned, sizeof(returned), "BUG(not his own mover on every one of the %u samples after the aura went, %u ms after the cast%s)",
                                 st->afterSamples, st->endedAt, flicker);
                    }
                }
                // Sampled, so it is allowed to say it was never sampled. A run whose player went
                // unresolvable reads nothing here rather than the false comfort of "false
                // throughout", which is what an unconditional OK would have printed over zero
                // readings. player-confuse has said it this way since it was written; this
                // scenario is the older of the two and carried the unconditional form until now.
                if (!st->taxiSamples) { snprintf(taxi, sizeof(taxi), "INVALID(IsTaxiFlying() was never read)"); }
                else { snprintf(taxi, sizeof(taxi), "%s(IsTaxiFlying() %s over %u samples)", st->taxi ? "BUG" : "OK", st->taxi ? "held" : "false throughout", st->taxiSamples); }
                std::string text = std::string("controlTaken=") + taken + " | playerFlees=" + flees + " | controlReturned=" + returned + " | notHeldByTaxi=" + taxi;
                Verdict(text);
            });
        }
    };


    namespace
    {
        /// player-confuse's two fixed moments, as the TIMELINE counts them: absolute offsets
        /// from the scenario's own start, which is what Scenario::At takes. They are its own
        /// rather than shared with player-fear's pair on purpose -- the two scenarios have no
        /// reason to move together, and a shared constant would make one of them the other's
        /// hostage.
        ///
        /// ONE TIME BASE IN THE LOG, and it is not this one, for the same reason it is not in
        /// player-fear: every "+Nms" line this scenario prints is N ms AFTER THE CAST, because
        /// that is the event every reading is about, and a step that logs an absolute moment
        /// subtracts the cast.
        const uint32 kConfuseCastAt = 500;
        const uint32 kConfusePullAt = 4000;

        /// How much of the confuse's own envelope the player must cover before playerWanders
        /// will call it movement.
        ///
        /// A confuse is NOT a flee, so player-fear's ten yards would read BUG on a perfect one:
        /// the stagger lurches inside Movement.ConfuseRadius -- 2 yd by default -- of the spot
        /// the unit was confused at, and none of it is meant to travel anywhere. So the
        /// threshold is a SHARE of the configured envelope, as S55's verdict is, and still says
        /// what it means on a server that has moved the radius.
        ///
        /// A quarter, and not less, because a quarter is already far outside anything a
        /// standing player can produce: nothing writes a standing player's position, so he reads
        /// 0.0 exactly and fails at any positive threshold at all, and the margin is there for
        /// the ground under the lurches rather than for noise.
        ///
        /// A quarter, and not more, because the reading is the PLAYER's own position, not the
        /// goal, and a confuse is under no obligation to reach either. Each lurch's destination
        /// is drawn with its radial distance uniform on [0, radius)
        /// (Map::GetReachableRandomPointOnGround: `range = rand_norm_f() * radius`), so one
        /// lurch alone clears a quarter of the envelope three times in four and the six-second
        /// window holds four to seven of them at the 800-1500 ms stagger -- but the stagger runs
        /// MID-LEG (ControlMoves.cpp Tick), so a lurch drawn near the rim can be superseded
        /// before it is walked, and the farthest the player is actually SEEN is well short of
        /// the farthest goal. A half or a whole envelope would be measuring the draw's luck.
        const float kWanderShareOfEnvelope = 0.25f;
    }

    /// S67 (the harness's second player): the confuse's CONTROL HANDOFF, the half of
    /// Unit::SetConfused no creature can exercise. player-fear proved the pair of calls for the
    /// fear (UnitSpeed.cpp:355-359, 427-430); this proves the confuse's own
    /// (UnitSpeed.cpp:458-462 on apply, UnitSpeed.cpp:500-503 on removal) -- a second copy of
    /// the rule, in a second function, which nothing but a player reaches and which until today
    /// only a human in a game client had ever watched work.
    ///
    /// The kobold casts 118 -- the spell, as S60 and player-fear do, not the entry point S55 and
    /// S58 call. Its two effects in 4.3.4 are Mod Confuse at index 0 (which is why every other
    /// scenario keys its claim on `POLYMORPH, 0`) and the sheep Transform at index 1; the sheep
    /// is along for the ride and changes nothing here, and taking the spell rather than the
    /// entry point is what puts Aura::HandleModConfuse on the path. The scenario samples for six
    /// seconds and pulls the aura 3.5 s after the cast (kConfusePullAt, absolute +4 s), so the
    /// handback falls inside that window instead of waiting on the spell's own 50 s.
    class PlayerConfuse : public Scenario
    {
    public:
        /// Order 901, in the reserved player block behind player-fear's 900, for the reason
        /// that block exists: the runner refuses `MVTEST all` when a player scenario is queued
        /// before one that holds no player (Harness.cpp Start), so player scenarios take high
        /// contiguous orders of their own and the creature families go on growing from 64
        /// without ever colliding with them.
        PlayerConfuse() : Scenario("player-confuse", 901) {}

        /// The runner owes a player scenario two things: the last place in the queue and the
        /// map's grids reset behind it. A player in world promotes the grids around him to full
        /// state and changes Map::Update's own visitation order, and no scenario that holds none
        /// may read that.
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                float  x0, y0;             ///< where he stood when the confuse landed -- the stagger's anchor too
                float  far2;               ///< the farthest he got from there
                bool   selfBefore;         ///< his own mover before the cast (the spawn's grant held)
                bool   auraLanded;         ///< the 118 holder was on him 100 ms after the cast
                uint32 confusedSamples;    ///< samples with the confuse's claim held
                uint32 afterSamples;       ///< samples since the claim was LAST seen held
                bool   selfWhileConfused;  ///< still his own mover on one of those first samples
                bool   allSelfAfter;       ///< his own mover on every one of the second
                uint32 taxiSamples;        ///< samples IsTaxiFlying() was actually read on
                bool   taxi;               ///< ...and true on any of them
                uint32 endedAt;            ///< when the claim went for the last time
                bool   pulled;             ///< the scenario's own RemoveAurasDueToSpell has run
                uint32 flickers;           ///< times the claim went unpublished BEFORE the pull and came back
                uint32 discarded;          ///< samples those re-anchorings took back out of the after-window
                uint32 backAfterPull;      ///< samples with the claim published AFTER the pull: a failure, never a flicker
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* k = p ? Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f) : NULL;
            if (!p || !k)
            {
                Verdict("controlTaken=INVALID(spawn failed) | playerWanders=INVALID(spawn failed) | controlReturned=INVALID(spawn failed) | notHeldByTaxi=INVALID(spawn failed)");
                return;
            }
            Silence(k);
            const ObjectGuid g = p->GetObjectGuid(), gk = k->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->x0 = st->y0 = st->far2 = 0.0f;
            st->selfBefore = st->auraLanded = st->selfWhileConfused = st->taxi = false;
            st->allSelfAfter = true;
            st->confusedSamples = st->afterSamples = st->taxiSamples = st->endedAt = 0;
            st->pulled = false;
            st->flickers = st->discarded = st->backAfterPull = 0;
            Log("the player %s stands at (%.1f, %.1f), the kobold 6 yd east, envelope %.1f yd", g.GetString().c_str(),
                p->Where().X(), p->Where().Y(), sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_CONFUSE_RADIUS));

            At(kConfuseCastAt, [this, g, gk, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* k = Get(gk);
                if (!p || !k) { return; }
                st->x0 = p->Where().X();
                st->y0 = p->Where().Y();
                // Read before the cast, because it is the whole meaning of the revoke that
                // follows: a player who was never his own mover cannot have it taken from him,
                // and a scenario that skipped this would pass on a server that never granted.
                st->selfBefore = OwnMover(p);
                if (p->IsTaxiFlying()) { st->taxi = true; }
                ++st->taxiSamples;
                k->CastSpell(p, POLYMORPH, true);
                Log("the kobold casts %u on the player at (%.1f, %.1f): his own mover before it=%d", POLYMORPH, st->x0, st->y0, st->selfBefore ? 1 : 0);
            });
            At(kConfuseCastAt + 100, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->auraLanded = p->HasAura(POLYMORPH);
                Log("+ 100ms after the cast: aura=%d confused=%d rooted=%d his own mover=%d", st->auraLanded ? 1 : 0,
                    p->Blocked(Motion::ReasonConfused) ? 1 : 0, p->IsRooted() ? 1 : 0, OwnMover(p) ? 1 : 0);
            });
            // Registered before the sampler so it runs first at its own moment (the timeline
            // orders a tie by insertion): the sample at +3500 after the cast is then already an
            // after-sample.
            At(kConfusePullAt, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                p->RemoveAurasDueToSpell(POLYMORPH);
                // The moment that divides the scenario in two for the sampler below: before it,
                // a gap in the claim is a flicker inside a confuse that is still meant to be
                // running; after it, the claim coming back at all is a failure.
                st->pulled = true;
                Log("+%4ums after the cast the aura pulled: aura=%d confused=%d his own mover=%d taxi=%d",
                    kConfusePullAt - kConfuseCastAt, p->HasAura(POLYMORPH) ? 1 : 0,
                    p->Blocked(Motion::ReasonConfused) ? 1 : 0, OwnMover(p) ? 1 : 0, p->IsTaxiFlying() ? 1 : 0);
            });
            for (uint32 i = 1; i <= 60; ++i)
            {
                At(kConfuseCastAt + i * 100, [this, g, st, i]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    const uint32 t = i * 100;
                    const float d = Dist2(st->x0, st->y0, p->Where().X(), p->Where().Y());
                    if (d > st->far2) { st->far2 = d; }
                    // Sampled, not assumed, and COUNTED: the handback at UnitSpeed.cpp:500 is
                    // refused under a flight, so a taxi anywhere in the run would be an
                    // alternative explanation for every other reading here -- and a run that
                    // never read the flag at all has not established that it was false, which
                    // is why the verdict below asks how many of these there were.
                    if (p->IsTaxiFlying()) { st->taxi = true; }
                    ++st->taxiSamples;
                    const bool self = OwnMover(p);
                    const bool claimHeld = p->Blocked(Motion::ReasonConfused);
                    if (claimHeld && !st->pulled)
                    {
                        ++st->confusedSamples;
                        if (self) { st->selfWhileConfused = true; }
                        // THE AFTER-WINDOW IS ANCHORED TO THE END OF THE CONFUSE, not to the
                        // first gap in it -- player-fear's own rule, and inherited here in its
                        // corrected form rather than rediscovered. A claim that reads
                        // unpublished on one sample and published again on the next -- BEFORE
                        // the scenario pulls the aura, while the confuse is still meant to be
                        // running -- was a flicker, not the handback, and everything counted
                        // since that gap belongs to the confuse's own window, where the control
                        // is CORRECTLY revoked. Counted as after-samples they would read "not
                        // his own mover after the aura went" and this category would print BUG
                        // on a working server. So the window re-anchors here, and what it
                        // discards is counted and said.
                        if (st->afterSamples)
                        {
                            ++st->flickers;
                            st->discarded += st->afterSamples;
                        }
                        st->afterSamples = 0;
                        st->endedAt = 0;
                        st->allSelfAfter = true;
                    }
                    else if (claimHeld)
                    {
                        // The claim is published AFTER the scenario pulled the aura. That is not
                        // a flicker in anything: the confuse was ended, on purpose, at a moment
                        // this scenario chose, so a claim standing again is a failure in its own
                        // right and is reported as one. Above all it must NOT re-anchor -- the
                        // failing control samples gathered since the pull are exactly the
                        // evidence a re-anchoring would erase, turning a genuine BUG into OK,
                        // which is the worse direction to be wrong in.
                        // Nor does it touch confusedSamples or selfWhileConfused. Those two
                        // answer controlTaken, which asks about the revoke at ONSET; a stale
                        // claim long after the pull is controlReturned's finding, and letting it
                        // reach the other category would make one event print BUG twice, once
                        // falsely.
                        ++st->backAfterPull;
                    }
                    else if (st->confusedSamples)
                    {
                        if (!st->endedAt) { st->endedAt = t; }
                        ++st->afterSamples;
                        if (!self) { st->allSelfAfter = false; }
                    }
                    if (i % 10 == 0)
                    {
                        Log("+%4ums %.1f yd out (farthest %.1f), confused=%d his own mover=%d", t, d, st->far2,
                            p->Blocked(Motion::ReasonConfused) ? 1 : 0, self ? 1 : 0);
                    }
                });
            }
            At(kConfuseCastAt + 6100, [this, st]()
            {
                const float radius = sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_CONFUSE_RADIUS);
                char taken[200], wanders[144], returned[296], taxi[112];
                if (!st->confusedSamples)
                {
                    snprintf(taken, sizeof(taken), "INVALID(the confuse never held: aura=%d)", st->auraLanded ? 1 : 0);
                    snprintf(wanders, sizeof(wanders), "INVALID(the confuse never held)");
                    snprintf(returned, sizeof(returned), "INVALID(the confuse never held)");
                }
                else
                {
                    if (!st->selfBefore)
                    {
                        snprintf(taken, sizeof(taken), "BUG(he was not his own mover before the cast, so the revoke had nothing to take)");
                    }
                    else
                    {
                        snprintf(taken, sizeof(taken), "%s(his own mover before the cast, the session's authority %s him on all %u samples while confused)",
                                 st->selfWhileConfused ? "BUG" : "OK", st->selfWhileConfused ? "still on" : "off", st->confusedSamples);
                    }
                    // A share of the envelope, not a distance (kWanderShareOfEnvelope): the
                    // stagger is not going anywhere, and a standing player reads 0.0.
                    snprintf(wanders, sizeof(wanders), "%s(%.1f yd from where he was confused, of the %.1f yd envelope; the line is %.1f)",
                             st->far2 >= kWanderShareOfEnvelope * radius ? "OK" : "BUG", st->far2, radius, kWanderShareOfEnvelope * radius);
                    // The flicker note, appended to whichever branch fires below. A claim that
                    // went unpublished mid-confuse and came back is worth seeing even though the
                    // window no longer counts it -- it is a finding about the claim, just not
                    // this category's -- and on a healthy run there is nothing to say.
                    char flicker[112];
                    if (st->flickers)
                    {
                        snprintf(flicker, sizeof(flicker), "; the claim flickered %u time(s) before the pull, re-anchoring the window past %u sample(s)",
                                 st->flickers, st->discarded);
                    }
                    else
                    {
                        flicker[0] = '\0';
                    }
                    // First, and ahead of the sample-count INVALID: a claim standing again after
                    // the aura was pulled is a hard failure, and reporting "too few samples"
                    // over the top of it would hide the one thing that went wrong.
                    if (st->backAfterPull)
                    {
                        snprintf(returned, sizeof(returned), "BUG(the confuse's claim was published again on %u sample(s) AFTER the aura was pulled, so the confuse did not end when it was ended; of the %u control sample(s) since the pull he was his own mover on %s%s)",
                                 st->backAfterPull, st->afterSamples, st->allSelfAfter ? "all" : "not all", flicker);
                    }
                    else if (st->afterSamples < 5)
                    {
                        snprintf(returned, sizeof(returned), "INVALID(only %u samples after the aura went%s)", st->afterSamples, flicker);
                    }
                    else if (st->allSelfAfter)
                    {
                        snprintf(returned, sizeof(returned), "OK(his own mover again from the first of the %u samples after the aura went, %u ms after the cast%s)",
                                 st->afterSamples, st->endedAt, flicker);
                    }
                    else
                    {
                        snprintf(returned, sizeof(returned), "BUG(not his own mover on every one of the %u samples after the aura went, %u ms after the cast%s)",
                                 st->afterSamples, st->endedAt, flicker);
                    }
                }
                // Sampled, so it is allowed to say it was never sampled. A run whose player went
                // unresolvable reads nothing here rather than the false comfort of "false
                // throughout", which is what an unconditional OK would have printed over zero
                // readings.
                if (!st->taxiSamples) { snprintf(taxi, sizeof(taxi), "INVALID(IsTaxiFlying() was never read)"); }
                else { snprintf(taxi, sizeof(taxi), "%s(IsTaxiFlying() %s over %u samples)", st->taxi ? "BUG" : "OK", st->taxi ? "held" : "false throughout", st->taxiSamples); }
                std::string text = std::string("controlTaken=") + taken + " | playerWanders=" + wanders + " | controlReturned=" + returned + " | notHeldByTaxi=" + taxi;
                Verdict(text);
            });
        }
    };


    namespace
    {
        /// player-feign's three fixed moments, as the TIMELINE counts them: absolute offsets
        /// from the scenario's own start, which is what Scenario::At takes. Its own pair rather
        /// than player-confuse's, for the reason player-confuse gives for not taking
        /// player-fear's: a shared constant would make one scenario the other's hostage.
        ///
        /// ONE TIME BASE IN THE LOG, and it is not this one: every "+Nms" line this scenario
        /// prints is N ms AFTER THE CAST, because the cast is the event every reading is about,
        /// and a step that logs an absolute moment subtracts it.
        ///
        /// The combat opens 200 ms BEFORE the cast rather than inside the cast's own step, and
        /// that gap is load-bearing twice. It lets two world updates pass with the player in
        /// combat, so combatEnded reads a state that had settled instead of one set in the same
        /// breath as the feign that ends it; and it is what gives the hostile reference below
        /// something to do, because a player in combat whom nothing hates is swept back out of
        /// it by Unit::Update's own combat timer (Unit.cpp:482-499) on the very next tick.
        const uint32 kFeignCombatAt = 300;
        const uint32 kFeignCastAt = 500;
        const uint32 kFeignPullAt = 4000;

        /// What the scenario writes into the player's movement flags just before the cast, so
        /// that the line under test (UnitSpeed.cpp:536) has something to clear. A session-less
        /// player's flag word is otherwise MOVEFLAG_NONE already, and a clear that clears
        /// nothing proves nothing.
        ///
        /// TWO bits, not one, because the line sets the WHOLE WORD to MOVEFLAG_NONE and a single
        /// bit could not tell that from a targeted clear: the creature side of the same `if`
        /// reaches MoveSplineInit::Stop, which removes MOVEFLAG_FORWARD and only that
        /// (MoveSplineInit.cpp:261). These two, because they are what a client driving a running
        /// character actually sends, which is the state the branch exists to erase.
        ///
        /// Nothing else on this path writes either of them. MoveSplineInit::Launch adds
        /// MOVEFLAG_FORWARD when a spline starts (MoveSplineInit.cpp:167) and this player never
        /// gets one -- he must not, since Player::SetPosition removes SPELL_AURA_FEIGN_DEATH
        /// outright on any relocation (Player.cpp:3589), so a player who moved would end his own
        /// feign. And both sit in movementFlagsMask, so Player::isMoving() reads true while they
        /// are on: that is deliberate, it is what a driving player looks like, and it cannot
        /// refuse the cast -- SpellChecks.cpp:199 turns a moving player's cast down only for an
        /// autorepeat spell or one carrying AURA_INTERRUPT_FLAG_NOT_SEATED, and 5384's aura
        /// interrupt flags are 0x3c3c, which is neither.
        const MovementFlags kDrivingFlags = MovementFlags(MOVEFLAG_FORWARD | MOVEFLAG_STRAFE_LEFT);
    }

    /// S68 (the harness's third player): the feign's PLAYER BRANCH, the half of
    /// Unit::SetFeignDeath no creature can exercise. A creature that feigns is STOPPED
    /// (UnitSpeed.cpp:532); a player has his MOVEMENT FLAGS CLEARED instead
    /// (UnitSpeed.cpp:536), because he is the one driving and the server cannot simply halt a
    /// spline he owns. Every feign the harness had run until today was a creature's --
    /// ScenariosBlock's feign-keeps-follow calls Creature::SetFeignDeath -- so that `else` is
    /// the one side of the `if` no scenario had ever taken.
    ///
    /// The player casts 5384 ON HIMSELF, and there is no choice about that: feign death's one
    /// effect carries implicit target 1, the caster, so a kobold casting it at the player the
    /// way player-fear and player-confuse cast theirs would put the aura on the KOBOLD. It is
    /// still the SPELL and not the entry point ScenariosBlock calls, which is the point: casting
    /// is what puts Aura::HandleFeignDeath (SpellAuraControl.cpp:391) on the path, and a
    /// self-cast is besides the only shape that takes the caster==target branch at
    /// UnitSpeed.cpp:552-555 at all. Taken, though with nothing to finish: the harness's cast is
    /// TRIGGERED and a triggered spell is never put in a current-spell slot (Spell::Prepare), so
    /// the FinishSpell inside that branch finds no generic spell to end here. A hunter pressing
    /// the button casts it untriggered, and there it has one.
    ///
    /// Around that branch sit two behaviours shared with the creature side that no player had
    /// been watched under either: the kernel's Dead inhibition (raised at UnitSpeed.cpp:546,
    /// lifted at 570) and the CombatStop() that ends his fight (UnitSpeed.cpp:547). The
    /// ExpireCombat() beside it (UnitSpeed.cpp:548) ends a chase in the Combat layer, and this
    /// player holds none, so there is nothing here for it to show; it is named so that the next
    /// reader does not go hunting for a category that was never available to write.
    ///
    /// The scenario samples for six seconds and pulls the aura 3.5 s after the cast
    /// (kFeignPullAt, absolute +4 s), so the lift falls inside that window instead of waiting on
    /// the spell's own six minutes.
    class PlayerFeign : public Scenario
    {
    public:
        /// Order 902, the next in the reserved player block behind player-fear's 900 and
        /// player-confuse's 901, for the reason that block exists: the runner refuses
        /// `MVTEST all` when a player scenario is queued before one that holds no player
        /// (Harness.cpp Start), so player scenarios take high contiguous orders of their own and
        /// the creature families go on growing from 64 without ever colliding with them.
        PlayerFeign() : Scenario("player-feign", 902) {}

        /// The runner owes a player scenario two things: the last place in the queue and the
        /// map's grids reset behind it. A player in world promotes the grids around him to full
        /// state and changes Map::Update's own visitation order, and no scenario that holds none
        /// may read that.
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   castRan;         ///< the cast step resolved the player and really cast
                uint32 flagsBefore;     ///< the driving flags, read BACK after the scenario wrote them and before the cast
                uint32 flagsAtApply;    ///< the same word the instant the cast returned
                bool   auraAtApply;     ///< the 5384 holder was on him the instant the cast returned
                bool   feignBefore;     ///< already feigning before the cast: published, or the kernel's own Dead
                bool   victimBefore;    ///< the kobold was his victim before the cast
                bool   combatBefore;    ///< ...and UNIT_FLAG_IN_COMBAT was on him
                bool   refsBefore;      ///< ...and something still hated him (the kobold's threat list)
                bool   victimAtApply;   ///< a victim the instant the cast returned
                bool   combatAtApply;   ///< in combat the instant the cast returned
                bool   refsAtApply;     ///< anything hating him the instant the cast returned
                uint32 auraSamples;     ///< samples before the pull with the 5384 holder on him
                uint32 unblocked;       ///< ...on which the published feign was NOT held
                uint32 disagreed;       ///< ...on which the published feign and the kernel's Dead inhibition differed
                uint32 feignSamples;    ///< samples with the block published
                uint32 afterSamples;    ///< samples since the block was LAST seen published
                bool   allClearAfter;   ///< the kernel's Dead inhibition was down on every one of those
                uint32 endedAt;         ///< when the block went for the last time
                bool   pulled;          ///< the scenario's own RemoveAurasDueToSpell has run
                uint32 flickers;        ///< times the block went unpublished BEFORE the pull and came back
                uint32 discarded;       ///< samples those re-anchorings took back out of the after-window
                uint32 backAfterPull;   ///< samples with the block published AFTER the pull: a failure, never a flicker
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* k = p ? Spawn(KOBOLD, SE.x + 3.0f, SE.y, Ground(SE.x + 3.0f, SE.y, SE.z), 3.1f) : NULL;
            if (!p || !k)
            {
                Verdict("flagsCleared=INVALID(spawn failed) | blockHeld=INVALID(spawn failed) | blockLifted=INVALID(spawn failed) | combatEnded=INVALID(spawn failed)");
                return;
            }
            Silence(k);
            const ObjectGuid g = p->GetObjectGuid(), gk = k->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->castRan = st->auraAtApply = st->feignBefore = false;
            st->victimBefore = st->combatBefore = st->refsBefore = false;
            st->victimAtApply = st->combatAtApply = st->refsAtApply = false;
            st->flagsBefore = st->flagsAtApply = 0;
            st->auraSamples = st->unblocked = st->disagreed = 0;
            st->allClearAfter = true;
            st->feignSamples = st->afterSamples = st->endedAt = 0;
            st->pulled = false;
            st->flickers = st->discarded = st->backAfterPull = 0;
            Log("the player %s stands at (%.1f, %.1f), the kobold 3 yd east", g.GetString().c_str(),
                p->Where().X(), p->Where().Y());

            At(kFeignCombatAt, [this, g, gk, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* k = Get(gk);
                if (!p || !k) { return; }
                // A victim without a swing. Unit::Attack is the row itself -- it sets m_attacking
                // and nothing more, combat state included -- so the two halves are asked for
                // separately, and the melee half is left out on purpose: a swinging player would
                // be dealing damage through the whole window, and every reading below would then
                // have the fight's own progress as an alternative explanation.
                p->Attack(k, false);
                p->SetInCombatWith(k);
                // The kobold's threat list, not the player's: a player cannot have one
                // (Unit::CanHaveThreatList refuses anything but a creature), and what this call
                // really builds is the HostileReference on the far side, which registers itself
                // in the PLAYER's HostileRefManager. That reference is the only reason the
                // combat opened above survives to the cast at all (Unit.cpp:482-499), and it is
                // also what UnitSpeed.cpp:557 deletes.
                k->AddThreat(p, 1000.0f);
                Log("+%4ums before the cast: victim=%d in combat=%d anything hating him=%d",
                    kFeignCastAt - kFeignCombatAt, p->getVictim() ? 1 : 0, p->IsInCombat() ? 1 : 0,
                    p->GetHostileRefManager().isEmpty() ? 0 : 1);
            });
            At(kFeignCastAt, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // Written, then READ BACK, and the read is what the verdict uses. The write
                // could be undone between here and the cast by anything that touched the word,
                // and a category that assumed its own setup held would report the feign's
                // success over a state the feign never saw.
                p->m_movementInfo.SetMovementFlags(kDrivingFlags);
                st->flagsBefore = uint32(p->m_movementInfo.GetMovementFlags());
                // Read before the cast, because it is the whole meaning of the block that
                // follows: a player already feigning cannot have a feign raised on him, and a
                // scenario that skipped this would pass on a server that was never clear.
                st->feignBefore = p->IsFeigningDeath() || p->GetMotionMaster()->Inhibited(Motion::Inhibition::Dead);
                st->victimBefore = p->getVictim() != NULL;
                st->combatBefore = p->IsInCombat();
                st->refsBefore = !p->GetHostileRefManager().isEmpty();
                SelfCast(p, FEIGN);
                // EVERY READING BELOW IS TAKEN HERE, in the cast's own step, and combatEnded
                // depends on that. A triggered instant spell applies its aura inside this call
                // (Spell::Prepare -> cast(true)), so this is the state SetFeignDeath left behind
                // and nothing else has run yet. Sampled 100 ms later instead, the combat reading
                // would be worthless: Unit::Update's combat timer ends the combat of a player
                // nobody hates on the next tick anyway (UnitSpeed.cpp:557 empties that list), so
                // a CombatStop() that never happened and one that did look identical from the
                // next step onwards. Only the synchronous read tells them apart -- measured, not
                // assumed: with CombatStop() removed the flag still reads ON here and OFF at the
                // very next sample.
                //
                // And of the two combat readings, THE FLAG IS THE ONE THAT DISCRIMINATES. The
                // victim is over-determined on this spell: 5384 carries
                // SPELL_ATTR_STOP_ATTACK_TARGET (its Attributes are 0x02150100, and the bit is
                // 0x00100000), so Spell::finish calls AttackStop() on the caster of its own
                // accord (SpellCast.cpp:1184) after the effects and before this line runs.
                // getVictim() is therefore NULL here whether CombatStop() ran or not, and it is
                // kept only because a victim standing after both of those would be a real
                // finding. Nobody should read "no victim" in this verdict as evidence of
                // CombatStop(); UNIT_FLAG_IN_COMBAT is what carries that.
                st->castRan = true;
                st->flagsAtApply = uint32(p->m_movementInfo.GetMovementFlags());
                st->auraAtApply = p->HasAura(FEIGN);
                st->victimAtApply = p->getVictim() != NULL;
                st->combatAtApply = p->IsInCombat();
                st->refsAtApply = !p->GetHostileRefManager().isEmpty();
                Log("the player casts %u on himself: flags 0x%08x -> 0x%08x, aura=%d feigning=%d dead-inhibited=%d victim=%d in combat=%d hated=%d",
                    FEIGN, st->flagsBefore, st->flagsAtApply, st->auraAtApply ? 1 : 0,
                    p->IsFeigningDeath() ? 1 : 0, p->GetMotionMaster()->Inhibited(Motion::Inhibition::Dead) ? 1 : 0,
                    st->victimAtApply ? 1 : 0, st->combatAtApply ? 1 : 0, st->refsAtApply ? 1 : 0);
            });
            At(kFeignCastAt + 100, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Log("+ 100ms after the cast: aura=%d feigning=%d dead-inhibited=%d flags 0x%08x in combat=%d",
                    p->HasAura(FEIGN) ? 1 : 0, p->IsFeigningDeath() ? 1 : 0,
                    p->GetMotionMaster()->Inhibited(Motion::Inhibition::Dead) ? 1 : 0,
                    uint32(p->m_movementInfo.GetMovementFlags()), p->IsInCombat() ? 1 : 0);
            });
            // Registered before the sampler so it runs first at its own moment (the timeline
            // orders a tie by insertion): the sample at +3500 after the cast is then already an
            // after-sample.
            At(kFeignPullAt, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                p->RemoveAurasDueToSpell(FEIGN);
                // The moment that divides the scenario in two for the sampler below: before it,
                // a gap in the block is a flicker inside a feign that is still meant to be
                // running; after it, the block coming back at all is a failure.
                st->pulled = true;
                Log("+%4ums after the cast the aura pulled: aura=%d feigning=%d dead-inhibited=%d",
                    kFeignPullAt - kFeignCastAt, p->HasAura(FEIGN) ? 1 : 0, p->IsFeigningDeath() ? 1 : 0,
                    p->GetMotionMaster()->Inhibited(Motion::Inhibition::Dead) ? 1 : 0);
            });
            for (uint32 i = 1; i <= 60; ++i)
            {
                At(kFeignCastAt + i * 100, [this, g, st, i]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    const uint32 t = i * 100;
                    // Three readings of one thing, and they are not interchangeable. The aura is
                    // the CAUSE and keys blockHeld's window; the published feign
                    // (MotionMaster::PublishedState::feign) is the shell's view of the block and
                    // is what player-confuse's Blocked() reads for its own claim; the arbiter's
                    // Dead inhibition is the kernel's live answer. A category that read only one
                    // of the last two could not see the two disagree, which is the failure a
                    // published state exists to be able to have.
                    const bool auraNow = p->HasAura(FEIGN);
                    const bool heldNow = p->IsFeigningDeath();
                    const bool kernelNow = p->GetMotionMaster()->Inhibited(Motion::Inhibition::Dead);
                    if (auraNow && !st->pulled)
                    {
                        ++st->auraSamples;
                        if (!heldNow) { ++st->unblocked; }
                        if (heldNow != kernelNow) { ++st->disagreed; }
                    }
                    if (heldNow && !st->pulled)
                    {
                        ++st->feignSamples;
                        // THE AFTER-WINDOW IS ANCHORED TO THE END OF THE FEIGN, not to the first
                        // gap in it -- player-fear's rule, corrected on player-confuse, and
                        // inherited here rather than rediscovered. A block that reads unpublished
                        // on one sample and published again on the next -- BEFORE the scenario
                        // pulls the aura, while the feign is still meant to be running -- was a
                        // flicker, not the lift, and everything counted since that gap belongs to
                        // the feign's own window, where the block is CORRECTLY up. Counted as
                        // after-samples they would read "still blocked after the aura went" and
                        // this category would print BUG on a working server. So the window
                        // re-anchors here, and what it discards is counted and said.
                        if (st->afterSamples)
                        {
                            ++st->flickers;
                            st->discarded += st->afterSamples;
                        }
                        st->afterSamples = 0;
                        st->endedAt = 0;
                        st->allClearAfter = true;
                    }
                    else if (heldNow)
                    {
                        // The block is published AFTER the scenario pulled the aura. That is not
                        // a flicker in anything: the feign was ended, on purpose, at a moment
                        // this scenario chose, so a block standing again is a failure in its own
                        // right and is reported as one. Above all it must NOT re-anchor -- the
                        // failing samples gathered since the pull are exactly the evidence a
                        // re-anchoring would erase, turning a genuine BUG into OK, which is the
                        // worse direction to be wrong in.
                        // Nor does it touch auraSamples or unblocked. Those answer blockHeld,
                        // which asks about the block at ONSET; a stale block long after the pull
                        // is blockLifted's finding, and letting it reach the other category would
                        // make one event print BUG twice, once falsely.
                        ++st->backAfterPull;
                    }
                    else if (st->feignSamples)
                    {
                        if (!st->endedAt) { st->endedAt = t; }
                        ++st->afterSamples;
                        // The published block is down on every sample that reaches here, by the
                        // branch above; the kernel's own is the reading this window is left to
                        // take, and the one a lift that forgot to Uninhibit would fail.
                        if (kernelNow) { st->allClearAfter = false; }
                    }
                    if (i % 10 == 0)
                    {
                        Log("+%4ums aura=%d feigning=%d dead-inhibited=%d flags 0x%08x victim=%d in combat=%d", t,
                            auraNow ? 1 : 0, heldNow ? 1 : 0, kernelNow ? 1 : 0,
                            uint32(p->m_movementInfo.GetMovementFlags()), p->getVictim() ? 1 : 0, p->IsInCombat() ? 1 : 0);
                    }
                });
            }
            At(kFeignCastAt + 6100, [this, st]()
            {
                char flags[224], held[272], lifted[320], combat[288];
                // Every branch below is reachable, and the INVALIDs come first wherever a
                // reading could not be taken at all: a step that never ran, a spell that never
                // landed, a setup the feign never saw. An OK printed over any of those would be
                // the one failure a harness cannot afford -- a scenario that passes without an
                // actor.
                if (!st->castRan)
                {
                    snprintf(flags, sizeof(flags), "INVALID(the cast step never ran: the player went unresolvable)");
                    snprintf(held, sizeof(held), "INVALID(the cast step never ran)");
                    snprintf(combat, sizeof(combat), "INVALID(the cast step never ran)");
                }
                else
                {
                    if (!st->flagsBefore)
                    {
                        snprintf(flags, sizeof(flags), "INVALID(the driving flags did not stay on him up to the cast: 0x%08x, so the feign had nothing to clear)", st->flagsBefore);
                    }
                    else if (!st->auraAtApply)
                    {
                        snprintf(flags, sizeof(flags), "INVALID(no %u holder on him when the cast returned, so Aura::HandleFeignDeath was never on the path)", FEIGN);
                    }
                    else
                    {
                        snprintf(flags, sizeof(flags), "%s(0x%08x on him before the cast, 0x%08x the instant it returned)",
                                 st->flagsAtApply == uint32(MOVEFLAG_NONE) ? "OK" : "BUG", st->flagsBefore, st->flagsAtApply);
                    }
                    if (!st->auraSamples)
                    {
                        snprintf(held, sizeof(held), "INVALID(the feign aura never held: on him when the cast returned=%d)", st->auraAtApply ? 1 : 0);
                    }
                    else if (st->feignBefore)
                    {
                        snprintf(held, sizeof(held), "BUG(he was already feigning before the cast, so the block had nothing to raise)");
                    }
                    else if (st->unblocked)
                    {
                        snprintf(held, sizeof(held), "BUG(the block was down on %u of the %u samples the aura was on him)", st->unblocked, st->auraSamples);
                    }
                    else if (st->disagreed)
                    {
                        snprintf(held, sizeof(held), "BUG(the published feign and the kernel's Dead inhibition differed on %u of the %u samples the aura was on him)", st->disagreed, st->auraSamples);
                    }
                    else
                    {
                        snprintf(held, sizeof(held), "OK(not feigning before the cast, the published feign and the kernel's Dead inhibition both held on all %u samples the aura was on him)", st->auraSamples);
                    }
                    if (!st->victimBefore || !st->combatBefore)
                    {
                        snprintf(combat, sizeof(combat), "INVALID(he was not in combat with a victim before the cast: victim=%d in combat=%d hated=%d, so there was nothing for CombatStop to end)",
                                 st->victimBefore ? 1 : 0, st->combatBefore ? 1 : 0, st->refsBefore ? 1 : 0);
                    }
                    else if (st->victimAtApply || st->combatAtApply)
                    {
                        snprintf(combat, sizeof(combat), "BUG(the instant the cast returned he still had victim=%d in combat=%d; hated=%d)",
                                 st->victimAtApply ? 1 : 0, st->combatAtApply ? 1 : 0, st->refsAtApply ? 1 : 0);
                    }
                    else
                    {
                        // The flag is named first and named as the flag, because it is the half
                        // only CombatStop() can clear here (see the cast step): the victim is
                        // gone either way on a spell carrying SPELL_ATTR_STOP_ATTACK_TARGET.
                        snprintf(combat, sizeof(combat), "OK(in combat with a victim before the cast, UNIT_FLAG_IN_COMBAT and the victim both gone the instant it returned; hated %d -> %d)",
                                 st->refsBefore ? 1 : 0, st->refsAtApply ? 1 : 0);
                    }
                }
                if (!st->feignSamples)
                {
                    snprintf(lifted, sizeof(lifted), "INVALID(the block never held)");
                }
                else
                {
                    // The flicker note, appended to whichever branch fires below. A block that
                    // went unpublished mid-feign and came back is worth seeing even though the
                    // window no longer counts it -- it is a finding about the block, just not
                    // this category's -- and on a healthy run there is nothing to say.
                    char flicker[128];
                    if (st->flickers)
                    {
                        snprintf(flicker, sizeof(flicker), "; the block flickered %u time(s) before the pull, re-anchoring the window past %u sample(s)",
                                 st->flickers, st->discarded);
                    }
                    else
                    {
                        flicker[0] = '\0';
                    }
                    // First, and ahead of the sample-count INVALID: a block standing again after
                    // the aura was pulled is a hard failure, and reporting "too few samples" over
                    // the top of it would hide the one thing that went wrong.
                    if (st->backAfterPull)
                    {
                        snprintf(lifted, sizeof(lifted), "BUG(the block was published again on %u sample(s) AFTER the aura was pulled, so the feign did not end when it was ended; of the %u sample(s) since the pull the kernel's Dead inhibition was down on %s%s)",
                                 st->backAfterPull, st->afterSamples, st->allClearAfter ? "all" : "not all", flicker);
                    }
                    else if (st->afterSamples < 5)
                    {
                        snprintf(lifted, sizeof(lifted), "INVALID(only %u samples after the block went%s)", st->afterSamples, flicker);
                    }
                    else if (st->allClearAfter)
                    {
                        snprintf(lifted, sizeof(lifted), "OK(the published block and the kernel's Dead inhibition were both down from the first of the %u samples after it went, %u ms after the cast%s)",
                                 st->afterSamples, st->endedAt, flicker);
                    }
                    else
                    {
                        snprintf(lifted, sizeof(lifted), "BUG(the kernel's Dead inhibition was still held on at least one of the %u samples after the published block went, %u ms after the cast%s)",
                                 st->afterSamples, st->endedAt, flicker);
                    }
                }
                std::string text = std::string("flagsCleared=") + flags + " | blockHeld=" + held + " | blockLifted=" + lifted + " | combatEnded=" + combat;
                Verdict(text);
            });
        }
    };


    namespace
    {
        /// player-stun's five fixed moments, as the TIMELINE counts them: absolute offsets from
        /// the scenario's own start, which is what Scenario::At takes. Its own set rather than
        /// player-feign's, for the reason player-confuse gives for not taking player-fear's: a
        /// shared constant would make one scenario the other's hostage.
        ///
        /// ONE TIME BASE IN THE LOG, and it is not this one: every "+Nms" line this scenario
        /// prints is N ms AFTER THE CAST, because the cast is the event every reading is about,
        /// and a step that logs an absolute moment subtracts it.
        ///
        /// The possession is taken 300 ms BEFORE the cast rather than inside the cast's own
        /// step, and that gap is load-bearing. Unit::TakePossessOf ends in
        /// Creature::AIM_Initialize (Unit.cpp:7192), which runs MotionMaster::Initialize on the
        /// body -- StopMoving, a full arbiter Clear, a fresh factory native -- so a take in the
        /// same breath as the stun would leave every reading about that body with the
        /// re-initialisation as an alternative explanation. Three world updates pass with the
        /// possession settled before anything is cast.
        ///
        /// The pull is 2.5 s after the cast, not 3.5 s as the earlier player slices used: 76216
        /// runs 6 s flat, and the after-window wants room for the whole tail rather than the
        /// last second of it.
        const uint32 kStunPossessAt = 200;
        const uint32 kStunCastAt    = 500;
        const uint32 kStunPullAt    = 3000;   ///< +2500 after the cast, inside the aura's own 6 s
        const uint32 kStunReleaseAt = 6600;   ///< the possession handed back, after the last sample
        const uint32 kStunVerdictAt = 6700;

        /// The stun this scenario casts, and it is deliberately NOT the harness's usual 5211
        /// Bash. 76216 "Self Stun - 6 seconds" carries ONE effect -- SPELL_EFFECT_APPLY_AURA
        /// with SPELL_AURA_MOD_STUN, implicit target 1 (the caster), 6000 ms flat from duration
        /// index 32 -- and nothing beside it: Attributes 0x00000080 with AttributesEx A..G all
        /// zero, no SpellCategories row at all (so DmgClass, Mechanic, Category and
        /// StartRecoveryCategory every one of them read 0), no SpellInterrupts row (aura
        /// interrupt flags 0), no SpellAuraRestrictions, no SpellShapeshift, no
        /// SpellTargetRestrictions. Physical school, so the frost branch at the head of
        /// HandleAuraModStun (SpellAuraControl.cpp:493) cannot fire either, and with no mechanic
        /// there is no diminishing group, so the duration is the flat 6 s on a player as well.
        ///
        /// BASH WAS REJECTED WITH A NUMBER. 5211's SpellCategories row gives DefenseType 2
        /// (SPELL_DAMAGE_CLASS_MELEE), so every application of it runs
        /// Unit::MeleeSpellHitResult (UnitCombat.cpp:640): a ~5% miss roll, and dodge and parry
        /// after it, because 5211 does not carry SPELL_ATTR_IMPOSSIBLE_DODGE_PARRY_BLOCK. This
        /// scenario applies its stun THREE times in one run and all six of its categories are
        /// keyed on the aura being on, so one unlucky roll takes the whole scenario out -- and
        /// the harness's RNG is seeded, so it would take it out on every run rather than
        /// occasionally, which is the worse of the two failures. 76216's damage class is NONE
        /// and Unit::SpellHitResult returns SPELL_MISS_NONE for that without rolling anything
        /// (UnitCombat.cpp:968-970). (Bash also wants bear form -- SpellShapeshift 67, stance
        /// mask 16 -- and only the triggered cast's skip of the shapeshift check keeps that out
        /// of the way; nothing here relies on that skip.)
        ///
        /// The one attribute 76216 does carry, SPELL_ATTR_UNK7 (0x80), is read in exactly two
        /// places in this tree: GetErrorAtShapeshiftedCast (SpellMgr.h:702), which a triggered
        /// cast never reaches, and a four-attribute conjunction at DBCStores.cpp:780 that one
        /// bit alone does not satisfy. Inert for everything asserted below.
        const uint32 STUN_SELF = 76216;

        /// THE SELF-CAST IS NOT A CONVENIENCE. Spell::DoSpellHitOnUnit stands a sitting target
        /// up itself (SpellHit.cpp:419-422), and it does so BEFORE the effects are applied, so
        /// its own `!Blocked(Motion::ReasonStunned)` guard is still open when it runs. That
        /// whole block sits under `if (realCaster && realCaster != unit)` (SpellHit.cpp:376): a
        /// stun cast by somebody else would reset the stand state on the spell-hit path whether
        /// HandleAuraModStun did or not, and moverBranch below would then read OK over a
        /// handler that had been gutted. With caster == target that path is not taken at all and
        /// the handler's SetStandState (SpellAuraControl.cpp:509) is the only one left.
        /// 76216's implicit target is the caster in any case, so the self-cast is the only shape
        /// its target map has.
        ///
        /// What the scenario writes into the plain player's movement-flag word just before the
        /// cast, so the wipe under test (SpellAuraControl.cpp:508) has something to clear: a
        /// session-less player's word is MOVEFLAG_NONE already, and a clear that clears nothing
        /// proves nothing.
        ///
        /// TWO bits, and the second is the one that carries the claim. The `else` half of the
        /// same `if` reaches Unit::StopMoving, whose spline stop removes MOVEFLAG_FORWARD and
        /// only that (MoveSplineInit.cpp:261) -- and on a unit whose spline is already finalized,
        /// as this standing player's is, StopMoving returns before even that
        /// (Unit.cpp:5821-5826). MOVEFLAG_STRAFE_LEFT therefore survives every route but the
        /// whole-word wipe, which is exactly what makes "the clientMover branch was taken"
        /// falsifiable. Both sit in movementFlagsMask, so Player::isMoving() reads true while
        /// they are on, and that cannot refuse the cast: SpellChecks.cpp:199 turns a moving
        /// player's cast down only for an autorepeat spell or one carrying
        /// AURA_INTERRUPT_FLAG_NOT_SEATED, and 76216 has no SpellInterrupts row at all.
        const MovementFlags kStunDrivingFlags = MovementFlags(MOVEFLAG_FORWARD | MOVEFLAG_STRAFE_LEFT);

        /// The kernel's own answer to "was the root DECIDED?", read off the change pipeline
        /// rather than off the flag word. Motion::State::Desired() is what the facade committed
        /// the instant ProjectClientRoot called SetRoot, and Pending() holds the entry a
        /// client-driven mover's change waits in until an ack that never comes for a player with
        /// no client (the Root row has an ack layout, PacketMatrix.cpp:53, so Apply really does
        /// open one). Both are taken through a const Unit*: the non-const MotionState() asserts
        /// the map phase, and a scenario step runs after the map's update rather than inside it.
        bool RootDecided(Unit const* u) { return u->MotionState().Desired().root; }
        bool RootInFlight(Unit const* u) { return u->MotionState().Pending().Has(Motion::ChangeType::Root); }
    }

    /// S69 (the harness's fourth player): the STUN's clientMover branch, and at its centre the
    /// ORDERING inside that branch which until today only a comment asserted.
    ///
    /// Aura::HandleAuraModStun (SpellAuraControl.cpp:482) wipes the movement-flag word and
    /// resets the stand state for a clientMover -- a player, OR a unit whose charmer is a
    /// player -- and it does so BEFORE Inhibit(Stunned). The comment labelled M1 says why: run
    /// it after and the wipe "erases MOVEFLAG_ROOT right back off", because Inhibit's projection
    /// (MotionMaster::ProjectClientRoot, MotionMaster.cpp:1695) roots a stunned clientMover.
    /// Aura::HandleAuraModRoot (SpellAuraControl.cpp:880) has the OPPOSITE order -- Inhibit
    /// first, then the wipe -- and its own comment names the asymmetry.
    ///
    /// The asymmetry is real, and it is not about the plain player. Player::SetRoot
    /// (PlayerMovement.cpp:85) never touches m_movementInfo: it applies a Motion::FlagChange and
    /// sends it, so a player's root is a desired flag awaiting his ack and a wipe running after
    /// Inhibit would have nothing of his to erase. Creature::SetRoot (CreatureMovement.cpp:267)
    /// DOES write MOVEFLAG_ROOT into m_movementInfo. So M1's stated rationale bites for exactly
    /// one shape -- the PLAYER-CHARMED CREATURE, the half of clientMover no scenario had ever
    /// built. S34 possessed a body with a CREATURE charmer, which the projection deliberately
    /// does not root; the three player scenarios of the last two days possessed nothing.
    ///
    /// Three subjects in one run, therefore, each self-casting 76216 at the same moment:
    ///   - a plain player: the branch is taken (his driving flags wiped whole, his stand state
    ///     reset), and his m_movementInfo carries no MOVEFLAG_ROOT, because his root is in
    ///     flight;
    ///   - a wolf THAT SAME PLAYER possesses: MOVEFLAG_ROOT is in its m_movementInfo the instant
    ///     the cast returns, which it can only be if the wipe ran first;
    ///   - a plain wolf: stopped, not rooted -- the projection never asks for its root at all.
    /// Of those, only the second is M1 itself; the first and third are the contrast that gives
    /// it its meaning, and without them "the flag is there" would say nothing about ordering.
    ///
    /// WHAT THE SCENARIO DOES NOT CLAIM: that the plain creature was STOPPED. Unit::StopMoving
    /// at SpellAuraControl.cpp:520 is over-determined here twice over -- the Inhibit two lines
    /// above it already blocks the body through the arbiter, and a creature standing still has a
    /// finalized spline, on which StopMoving returns without doing anything (Unit.cpp:5821-5826).
    /// "Not rooted" is the half of that sentence a scenario can own, and it is the half the
    /// projection decides.
    ///
    /// The scenario samples for six seconds and pulls all three auras 2.5 s after the cast
    /// (kStunPullAt, absolute +3 s) so every lift falls inside the window rather than arriving
    /// with the spell's own expiry at the very end of it.
    class PlayerStun : public Scenario
    {
    public:
        /// Order 903, the next in the reserved player block behind player-fear's 900,
        /// player-confuse's 901 and player-feign's 902, for the reason that block exists: the
        /// runner refuses `MVTEST all` when a player scenario is queued before one that holds no
        /// player (Harness.cpp Start), so player scenarios take high contiguous orders of their
        /// own and the creature families go on growing from 64 without ever colliding with them.
        PlayerStun() : Scenario("player-stun", 903) {}

        /// The runner owes a player scenario two things: the last place in the queue and the
        /// map's grids reset behind it. A player in world promotes the grids around him to full
        /// state and changes Map::Update's own visitation order, and no scenario that holds none
        /// may read that.
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                // --- the plain player
                bool   castRan;          ///< the cast step resolved every actor and really cast
                uint32 pFlagsBefore;     ///< the driving flags, read BACK after the scenario wrote them
                uint8  pStandBefore;     ///< ...and the stand state, likewise read back
                bool   pTargetBefore;    ///< ...and a non-empty UNIT_FIELD_TARGET to be cleared
                bool   pStunnedBefore;   ///< already stunned before the cast: the flag, or the kernel's own
                bool   pAuraAtApply;     ///< the 76216 holder was on him the instant the cast returned
                uint32 pFlagsAtApply;    ///< the same word that instant
                uint8  pStandAtApply;    ///< ...and the same stand state
                uint32 pAuraSamples;     ///< samples before the pull with the holder on him
                uint32 pUnflagged;       ///< ...on which UNIT_FLAG_STUNNED was off
                uint32 pUninhibited;     ///< ...on which the kernel's Stunned inhibition was down
                uint32 pTargeted;        ///< ...on which a target guid was back on him
                uint32 pRootInWord;      ///< ...on which MOVEFLAG_ROOT sat in his m_movementInfo
                uint32 pRootUndecided;   ///< ...on which the kernel had not decided his root
                uint32 pRootPending;     ///< ...on which a Root change was still awaiting his ack
                // the lift, anchored to the KERNEL's Stunned (see the sampler)
                uint32 pHeldSamples;     ///< samples with that inhibition up
                uint32 pAfterSamples;    ///< samples since it was LAST seen up
                uint32 pEndedAt;         ///< when it went for the last time
                bool   pFlagAfter;       ///< UNIT_FLAG_STUNNED seen on one of those
                bool   pTargetNotBack;   ///< his victim's guid was NOT back on one of those
                bool   pulled;           ///< the scenario's own RemoveAurasDueToSpell has run
                uint32 pFlickers;        ///< times the inhibition went BEFORE the pull and came back
                uint32 pDiscarded;       ///< samples those re-anchorings took back out of the after-window
                uint32 pBackAfterPull;   ///< samples with the inhibition up AFTER the pull: a failure, never a flicker
                // --- the wolf the player possesses
                bool   took;             ///< TakePossessOf returned true
                bool   bCharmedBefore;   ///< ...and the player really was its charmer at the cast
                uint32 bFlagsBefore;     ///< its flag word before the cast: MOVEFLAG_ROOT must NOT be in it
                bool   bAuraAtApply;     ///< the holder was on it the instant the cast returned
                uint32 bFlagsAtApply;    ///< the same word that instant: M1's reading
                uint32 bSamples;         ///< samples with the holder on it AND the player still its charmer
                uint32 bRootMissing;     ///< ...on which MOVEFLAG_ROOT was gone from its word
                // --- the plain wolf
                bool   cAuraAtApply;
                uint32 cFlagsBefore;
                uint32 cFlagsAtApply;
                bool   cStunnedAtApply;  ///< the kernel's Stunned inhibition was up on it
                uint32 cSamples;
                uint32 cRootInWord;      ///< samples on which MOVEFLAG_ROOT sat in its word
                uint32 cRootWanted;      ///< ...on which it was rooted or its root had been decided
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            // The body 4 yd east, the plain wolf 16 yd east and the mark 8 yd west: far enough
            // apart that nothing is in melee reach of anything, which matters because the take
            // gives the body the PLAYER's faction and its own AI back (see the possess step).
            Creature* body  = p     ? Spawn(WOLF,   SE.x + 4.0f,  SE.y, Ground(SE.x + 4.0f,  SE.y, SE.z), 3.1f) : NULL;
            Creature* plain = body  ? Spawn(WOLF,   SE.x + 16.0f, SE.y, Ground(SE.x + 16.0f, SE.y, SE.z), 3.1f) : NULL;
            Creature* mark  = plain ? Spawn(KOBOLD, SE.x - 8.0f,  SE.y, Ground(SE.x - 8.0f,  SE.y, SE.z), 0.0f) : NULL;
            if (!p || !body || !plain || !mark)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            // All three, and the body among them although the take is about to undo it:
            // AIM_Initialize hands the body a fresh factory AI, so this Silence only covers the
            // 200 ms before the take -- but those are 200 ms with a player standing 4 yd from
            // two hostile beasts, and an aggro there would put a melee swing (and with it
            // Unit::DealDamage's own stand-state reset, Unit.cpp:903) on the path of every
            // reading below. After the take the body is quiet for different reasons: it holds
            // the player's faction, its charm info is REACT_PASSIVE/COMMAND_STAY, and the
            // Possessed inhibition refuses it movement (S34).
            Silence(body);
            Silence(plain);
            Silence(mark);
            const ObjectGuid g = p->GetObjectGuid(), gbody = body->GetObjectGuid(),
                             gplain = plain->GetObjectGuid(), gmark = mark->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->castRan = st->pTargetBefore = st->pStunnedBefore = st->pAuraAtApply = false;
            st->pFlagsBefore = st->pFlagsAtApply = 0;
            st->pStandBefore = st->pStandAtApply = UNIT_STAND_STATE_STAND;
            st->pAuraSamples = st->pUnflagged = st->pUninhibited = st->pTargeted = 0;
            st->pRootInWord = st->pRootUndecided = st->pRootPending = 0;
            st->pHeldSamples = st->pAfterSamples = st->pEndedAt = 0;
            st->pFlagAfter = st->pTargetNotBack = st->pulled = false;
            st->pFlickers = st->pDiscarded = st->pBackAfterPull = 0;
            st->took = st->bCharmedBefore = st->bAuraAtApply = false;
            st->bFlagsBefore = st->bFlagsAtApply = st->bSamples = st->bRootMissing = 0;
            st->cAuraAtApply = st->cStunnedAtApply = false;
            st->cFlagsBefore = st->cFlagsAtApply = st->cSamples = st->cRootInWord = st->cRootWanted = 0;
            Log("the player %s stands at (%.1f, %.1f); the body 4 yd east, the plain wolf 16 yd east, his mark 8 yd west",
                g.GetString().c_str(), p->Where().X(), p->Where().Y());

            At(kStunPossessAt, [this, g, gbody, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* b = Get(gbody);
                if (!p || !b) { return; }
                // Unit::TakePossessOf(Unit*) and nothing hand-rolled beside it: it is the call
                // the possess effect makes, and it is what sets the charmer guid the stun
                // handler's clientMover test and ProjectClientRoot both read. A player charmer
                // takes a longer path through it than S34's creature one -- the camera, the
                // client-control grant that makes the body client-driven, the possess action
                // bar -- and every packet on that path goes to a socketless session and is
                // dropped by WorldSession::SendPacket's first line.
                st->took = p->TakePossessOf(b);
                Log("the player possesses the wolf: %d, charmer=%s possessed=%d rooted=%d flags 0x%08x",
                    st->took ? 1 : 0, b->GetCharmerGuid().GetString().c_str(),
                    b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0,
                    b->IsRooted() ? 1 : 0, uint32(b->m_movementInfo.GetMovementFlags()));
            });
            At(kStunCastAt, [this, g, gbody, gplain, gmark, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* b = Get(gbody);
                Creature* c = Get(gplain);
                Creature* m = Get(gmark);
                if (!p || !b || !c || !m) { return; }
                // Written, then READ BACK, and the read is what the verdict uses. The write
                // could be undone between here and the cast by anything that touched the word,
                // the stand state or the target field, and a category that assumed its own setup
                // held would report the stun's success over a state the stun never saw.
                p->m_movementInfo.SetMovementFlags(kStunDrivingFlags);
                p->SetStandState(UNIT_STAND_STATE_SIT);
                // A victim without a swing, as player-feign took one: Unit::Attack is what
                // writes UNIT_FIELD_TARGET (Unit.cpp:3063), and the melee half is left out so
                // that no damage is dealt anywhere in the run -- Unit::DealDamage stands a
                // sitting player up (Unit.cpp:903), which would be an alternative explanation
                // for the stand state, and it is also the one thing that could end a stun early.
                p->Attack(m, false);
                st->pFlagsBefore = uint32(p->m_movementInfo.GetMovementFlags());
                st->pStandBefore = p->getStandState();
                st->pTargetBefore = !p->GetTargetGuid().IsEmpty();
                // Read before the cast, because it is the whole meaning of the state that
                // follows: a player already stunned cannot have a stun raised on him, and a
                // scenario that skipped this would pass on a server that was never clear.
                st->pStunnedBefore = p->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED) ||
                                     p->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                st->bCharmedBefore = b->GetCharmerGuid() == g;
                st->bFlagsBefore = uint32(b->m_movementInfo.GetMovementFlags());
                st->cFlagsBefore = uint32(c->m_movementInfo.GetMovementFlags());
                // Three self-casts, one step, no world update between them: the body first,
                // because it is the reading the scenario exists for and it should not be able to
                // blame anything the other two did.
                SelfCast(b, STUN_SELF);
                SelfCast(c, STUN_SELF);
                SelfCast(p, STUN_SELF);
                // EVERY READING BELOW IS TAKEN HERE, in the cast's own step. A triggered instant
                // spell applies its aura inside the call (Spell::Prepare -> cast(true)), so this
                // is the state HandleAuraModStun left behind and nothing else has run yet -- and
                // for the body's flag word that is the point, since the very next thing the
                // scenario could do would be indistinguishable from the handler's own ordering.
                st->castRan = true;
                st->pAuraAtApply = p->HasAura(STUN_SELF);
                st->pFlagsAtApply = uint32(p->m_movementInfo.GetMovementFlags());
                st->pStandAtApply = p->getStandState();
                st->bAuraAtApply = b->HasAura(STUN_SELF);
                st->bFlagsAtApply = uint32(b->m_movementInfo.GetMovementFlags());
                st->cAuraAtApply = c->HasAura(STUN_SELF);
                st->cFlagsAtApply = uint32(c->m_movementInfo.GetMovementFlags());
                st->cStunnedAtApply = c->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                Log("the cast returns: player aura=%d flags 0x%08x -> 0x%08x stand %u -> %u target=%d stunned=%d root decided=%d",
                    st->pAuraAtApply ? 1 : 0, st->pFlagsBefore, st->pFlagsAtApply,
                    uint32(st->pStandBefore), uint32(st->pStandAtApply),
                    p->GetTargetGuid().IsEmpty() ? 0 : 1,
                    p->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED) ? 1 : 0, RootDecided(p) ? 1 : 0);
                Log("the cast returns: body aura=%d charmer=%d flags 0x%08x -> 0x%08x (MOVEFLAG_ROOT %s) | plain aura=%d stunned=%d flags 0x%08x -> 0x%08x root decided=%d",
                    st->bAuraAtApply ? 1 : 0, st->bCharmedBefore ? 1 : 0, st->bFlagsBefore, st->bFlagsAtApply,
                    (st->bFlagsAtApply & MOVEFLAG_ROOT) ? "held" : "GONE",
                    st->cAuraAtApply ? 1 : 0, st->cStunnedAtApply ? 1 : 0, st->cFlagsBefore, st->cFlagsAtApply,
                    RootDecided(c) ? 1 : 0);
            });
            At(kStunCastAt + 100, [this, g, gbody, gplain, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Creature* b = Get(gbody); Creature* c = Get(gplain);
                Log("+ 100ms after the cast: player aura=%d stunned=%d kernel=%d flags 0x%08x root decided=%d in flight=%d | body flags 0x%08x | plain flags 0x%08x",
                    p->HasAura(STUN_SELF) ? 1 : 0, p->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED) ? 1 : 0,
                    p->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0,
                    uint32(p->m_movementInfo.GetMovementFlags()), RootDecided(p) ? 1 : 0, RootInFlight(p) ? 1 : 0,
                    b ? uint32(b->m_movementInfo.GetMovementFlags()) : 0,
                    c ? uint32(c->m_movementInfo.GetMovementFlags()) : 0);
            });
            // Registered before the sampler so it runs first at its own moment (the timeline
            // orders a tie by insertion): the sample at +2500 after the cast is then already an
            // after-sample.
            At(kStunPullAt, [this, g, gbody, gplain, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Creature* b = Get(gbody); Creature* c = Get(gplain);
                p->RemoveAurasDueToSpell(STUN_SELF);
                if (b) { b->RemoveAurasDueToSpell(STUN_SELF); }
                if (c) { c->RemoveAurasDueToSpell(STUN_SELF); }
                // The moment that divides the scenario in two for the sampler below: before it,
                // a gap in the stun is a flicker inside a stun that is still meant to be
                // running; after it, the stun coming back at all is a failure.
                st->pulled = true;
                Log("+%4ums after the cast the auras pulled: player aura=%d stunned=%d kernel=%d target=%s | body flags 0x%08x | plain flags 0x%08x",
                    kStunPullAt - kStunCastAt, p->HasAura(STUN_SELF) ? 1 : 0,
                    p->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED) ? 1 : 0,
                    p->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0,
                    p->GetTargetGuid().GetString().c_str(),
                    b ? uint32(b->m_movementInfo.GetMovementFlags()) : 0,
                    c ? uint32(c->m_movementInfo.GetMovementFlags()) : 0);
            });
            for (uint32 i = 1; i <= 60; ++i)
            {
                At(kStunCastAt + i * 100, [this, g, gbody, gplain, gmark, st, i]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    Creature* b = Get(gbody);
                    Creature* c = Get(gplain);
                    const uint32 t = i * 100;
                    const bool auraNow = p->HasAura(STUN_SELF);
                    const bool flagNow = p->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
                    const bool kernelNow = p->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                    const uint32 wordNow = uint32(p->m_movementInfo.GetMovementFlags());
                    if (auraNow && !st->pulled)
                    {
                        // Three readings of one state and they are not interchangeable: the
                        // client flag is what the game shows, the kernel's inhibition is what the
                        // movement gates read, and UNIT_FIELD_TARGET is the third thing the apply
                        // writes. A category that read only one could not see two of them
                        // disagree, which is the failure a projected state exists to be able to
                        // have.
                        ++st->pAuraSamples;
                        if (!flagNow) { ++st->pUnflagged; }
                        if (!kernelNow) { ++st->pUninhibited; }
                        if (!p->GetTargetGuid().IsEmpty()) { ++st->pTargeted; }
                        if (wordNow & MOVEFLAG_ROOT) { ++st->pRootInWord; }
                        if (!RootDecided(p)) { ++st->pRootUndecided; }
                        if (RootInFlight(p)) { ++st->pRootPending; }
                    }
                    // THE AFTER-WINDOW IS ANCHORED TO THE END OF THE STUN, not to the first gap
                    // in it -- player-fear's rule, corrected on player-confuse, inherited here
                    // rather than rediscovered -- and it is anchored on the KERNEL's inhibition
                    // rather than on the client flag ON PURPOSE. The two categories must be able
                    // to fail apart: a handler that forgot to raise UNIT_FLAG_STUNNED is
                    // stunHeld's finding, and anchoring this window on the flag would turn that
                    // one bug into "the stun never held" here as well, reporting one fault twice.
                    if (kernelNow && !st->pulled)
                    {
                        ++st->pHeldSamples;
                        if (st->pAfterSamples)
                        {
                            ++st->pFlickers;
                            st->pDiscarded += st->pAfterSamples;
                        }
                        st->pAfterSamples = 0;
                        st->pEndedAt = 0;
                        st->pFlagAfter = false;
                        st->pTargetNotBack = false;
                    }
                    else if (kernelNow)
                    {
                        // The inhibition is up AFTER the scenario pulled the aura. That is not a
                        // flicker in anything: the stun was ended, on purpose, at a moment this
                        // scenario chose, so a block standing again is a failure in its own right
                        // and is reported as one. Above all it must NOT re-anchor -- the failing
                        // samples gathered since the pull are exactly the evidence a re-anchoring
                        // would erase, turning a genuine BUG into OK.
                        ++st->pBackAfterPull;
                    }
                    else if (st->pHeldSamples)
                    {
                        if (!st->pEndedAt) { st->pEndedAt = t; }
                        ++st->pAfterSamples;
                        if (flagNow) { st->pFlagAfter = true; }
                        // The removal restores the victim's guid (SpellAuraControl.cpp:588-594),
                        // which is why the player was given a victim at all: without one that
                        // branch runs and writes nothing, and "the target came back" would be a
                        // sentence about an empty field.
                        if (p->GetTargetGuid() != gmark) { st->pTargetNotBack = true; }
                    }
                    // The body's window is keyed on the possession as well as on the aura: the
                    // release at the end of the run drops the charmer, and with it the
                    // projection's clientMover, so a sample taken after that would read a body
                    // the kernel had correctly unrooted as a missing root.
                    if (b && !st->pulled && b->HasAura(STUN_SELF) && b->GetCharmerGuid() == g)
                    {
                        ++st->bSamples;
                        if (!(uint32(b->m_movementInfo.GetMovementFlags()) & MOVEFLAG_ROOT)) { ++st->bRootMissing; }
                    }
                    if (c && !st->pulled && c->HasAura(STUN_SELF))
                    {
                        ++st->cSamples;
                        if (uint32(c->m_movementInfo.GetMovementFlags()) & MOVEFLAG_ROOT) { ++st->cRootInWord; }
                        if (c->IsRooted() || RootDecided(c)) { ++st->cRootWanted; }
                    }
                    if (i % 10 == 0)
                    {
                        Log("+%4ums player aura=%d stunned=%d kernel=%d flags 0x%08x target=%d | body flags 0x%08x | plain flags 0x%08x rooted=%d", t,
                            auraNow ? 1 : 0, flagNow ? 1 : 0, kernelNow ? 1 : 0, wordNow,
                            p->GetTargetGuid().IsEmpty() ? 0 : 1,
                            b ? uint32(b->m_movementInfo.GetMovementFlags()) : 0,
                            c ? uint32(c->m_movementInfo.GetMovementFlags()) : 0,
                            c && c->IsRooted() ? 1 : 0);
                    }
                });
            }
            At(kStunReleaseAt, [this, g, gbody]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // After the last sample, and before the runner's teardown rather than left to
                // it: the teardown despawns the scenario's creatures BEFORE it ends its players
                // (Harness.cpp), so a body still charmed at that point would be freed under a
                // live charmer. ResetControlState is the call the possess aura's own removal
                // makes, and the body is a creature, which keeps it clear of the unconditional
                // Creature* cast in it that a possessed PLAYER would walk into.
                p->ResetControlState(false);
                Creature* b = Get(gbody);
                Log("the possession released: charmer=%s body flags 0x%08x possessed=%d",
                    b ? b->GetCharmerGuid().GetString().c_str() : "gone",
                    b ? uint32(b->m_movementInfo.GetMovementFlags()) : 0,
                    b && b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0);
            });
            At(kStunVerdictAt, [this, st]()
            {
                char branch[288], held[304], lifted[336], m1[352], inflight[304], plainroot[320];
                // Every branch below is reachable, and the INVALIDs come first wherever a reading
                // could not be taken at all: a step that never ran, a spell that never landed, a
                // setup the stun never saw, a possession that was refused. An OK printed over any
                // of those would be the one failure a harness cannot afford -- a scenario that
                // passes without an actor.
                if (!st->castRan)
                {
                    snprintf(branch, sizeof(branch), "INVALID(the cast step never ran: an actor went unresolvable)");
                    snprintf(held, sizeof(held), "INVALID(the cast step never ran)");
                    snprintf(m1, sizeof(m1), "INVALID(the cast step never ran)");
                    snprintf(inflight, sizeof(inflight), "INVALID(the cast step never ran)");
                    snprintf(plainroot, sizeof(plainroot), "INVALID(the cast step never ran)");
                }
                else
                {
                    // --- moverBranch: the clientMover half of the `if` was taken for the player.
                    // The driving bits are masked rather than the whole word compared to
                    // MOVEFLAG_NONE, and that is deliberate: the very Inhibit this branch runs
                    // before may legitimately put MOVEFLAG_ROOT into the word of a unit whose
                    // SetRoot writes one, which is what charmedRootSurvives is about. A
                    // whole-word test here would make this category flip on that, and the two
                    // claims must be able to fail apart.
                    if (!st->pAuraAtApply)
                    {
                        snprintf(branch, sizeof(branch), "INVALID(no %u holder on him when the cast returned, so Aura::HandleAuraModStun was never on the path)", STUN_SELF);
                    }
                    else if ((st->pFlagsBefore & uint32(kStunDrivingFlags)) != uint32(kStunDrivingFlags))
                    {
                        snprintf(branch, sizeof(branch), "INVALID(the driving flags did not stay on him up to the cast: 0x%08x, so the wipe had nothing to clear)", st->pFlagsBefore);
                    }
                    else if (st->pStandBefore == UNIT_STAND_STATE_STAND)
                    {
                        snprintf(branch, sizeof(branch), "INVALID(he was already standing before the cast: stand state %u, so the reset had nothing to do)", uint32(st->pStandBefore));
                    }
                    else
                    {
                        const bool wiped = (st->pFlagsAtApply & uint32(kStunDrivingFlags)) == 0;
                        const bool stood = st->pStandAtApply == UNIT_STAND_STATE_STAND;
                        snprintf(branch, sizeof(branch), "%s(the driving flags 0x%08x -> 0x%08x and the stand state %u -> %u the instant the cast returned; StopMoving clears neither on a finalized spline)",
                                 wiped && stood ? "OK" : "BUG", st->pFlagsBefore, st->pFlagsAtApply,
                                 uint32(st->pStandBefore), uint32(st->pStandAtApply));
                    }
                    // --- stunHeld
                    if (!st->pAuraSamples)
                    {
                        snprintf(held, sizeof(held), "INVALID(the stun aura never held on him: on him when the cast returned=%d)", st->pAuraAtApply ? 1 : 0);
                    }
                    else if (st->pStunnedBefore)
                    {
                        snprintf(held, sizeof(held), "BUG(he was already stunned before the cast, so the stun had nothing to raise)");
                    }
                    else if (!st->pTargetBefore)
                    {
                        snprintf(held, sizeof(held), "INVALID(he held no target guid before the cast, so the clear at SpellAuraControl.cpp:512 had nothing to clear)");
                    }
                    else if (st->pUnflagged)
                    {
                        snprintf(held, sizeof(held), "BUG(UNIT_FLAG_STUNNED was off on %u of the %u samples the aura was on him)", st->pUnflagged, st->pAuraSamples);
                    }
                    else if (st->pUninhibited)
                    {
                        snprintf(held, sizeof(held), "BUG(the kernel's Stunned inhibition was down on %u of the %u samples the aura was on him)", st->pUninhibited, st->pAuraSamples);
                    }
                    else if (st->pTargeted)
                    {
                        snprintf(held, sizeof(held), "BUG(his target guid was back on %u of the %u samples the aura was on him)", st->pTargeted, st->pAuraSamples);
                    }
                    else
                    {
                        snprintf(held, sizeof(held), "OK(not stunned and holding a target guid before the cast; UNIT_FLAG_STUNNED and the kernel's Stunned inhibition both held and the target guid stayed empty on all %u samples the aura was on him)", st->pAuraSamples);
                    }
                    // --- charmedRootSurvives: M1 itself.
                    if (!st->took)
                    {
                        snprintf(m1, sizeof(m1), "INVALID(TakePossessOf refused, so no player-charmed creature was built)");
                    }
                    else if (!st->bCharmedBefore)
                    {
                        snprintf(m1, sizeof(m1), "INVALID(the wolf's charmer was not the player when the cast went out, so the clientMover branch was never its path)");
                    }
                    else if (st->bFlagsBefore & MOVEFLAG_ROOT)
                    {
                        snprintf(m1, sizeof(m1), "INVALID(MOVEFLAG_ROOT was already in its m_movementInfo before the cast: 0x%08x, so surviving proves nothing)", st->bFlagsBefore);
                    }
                    else if (!st->bAuraAtApply)
                    {
                        snprintf(m1, sizeof(m1), "INVALID(no %u holder on it when the cast returned)", STUN_SELF);
                    }
                    else if (!(st->bFlagsAtApply & MOVEFLAG_ROOT))
                    {
                        snprintf(m1, sizeof(m1), "BUG(MOVEFLAG_ROOT was not in its m_movementInfo the instant the cast returned: 0x%08x -> 0x%08x, so the wipe ran AFTER Inhibit and erased the projection's root)",
                                 st->bFlagsBefore, st->bFlagsAtApply);
                    }
                    else if (!st->bSamples)
                    {
                        snprintf(m1, sizeof(m1), "INVALID(no samples with the aura on it while the player was still its charmer)");
                    }
                    else if (st->bRootMissing)
                    {
                        snprintf(m1, sizeof(m1), "BUG(MOVEFLAG_ROOT was gone from its m_movementInfo on %u of the %u samples the aura was on it under the possession)", st->bRootMissing, st->bSamples);
                    }
                    else
                    {
                        snprintf(m1, sizeof(m1), "OK(0x%08x -> 0x%08x the instant the cast returned and MOVEFLAG_ROOT held on all %u samples under the possession: the wipe ran BEFORE Inhibit)",
                                 st->bFlagsBefore, st->bFlagsAtApply, st->bSamples);
                    }
                    // --- playerCarriesNoRoot: the contrast that gives M1 its meaning.
                    if (!st->pAuraSamples)
                    {
                        snprintf(inflight, sizeof(inflight), "INVALID(the stun aura never held on him)");
                    }
                    else if (st->pRootUndecided)
                    {
                        snprintf(inflight, sizeof(inflight), "BUG(the kernel had not decided his root on %u of the %u samples the aura was on him, so a stunned player's mover was never rooted at all)", st->pRootUndecided, st->pAuraSamples);
                    }
                    else if (st->pRootInWord)
                    {
                        snprintf(inflight, sizeof(inflight), "BUG(MOVEFLAG_ROOT sat in his m_movementInfo on %u of the %u samples the aura was on him: a player's root belongs in the change pipeline, not the word)", st->pRootInWord, st->pAuraSamples);
                    }
                    else
                    {
                        snprintf(inflight, sizeof(inflight), "OK(the kernel wanted his root on all %u samples the aura was on him and it was still awaiting his ack on %u of them, and MOVEFLAG_ROOT was in his m_movementInfo on none)",
                                 st->pAuraSamples, st->pRootPending);
                    }
                    // --- plainCreatureNotRooted
                    if (!st->cAuraAtApply)
                    {
                        snprintf(plainroot, sizeof(plainroot), "INVALID(no %u holder on the plain wolf when the cast returned)", STUN_SELF);
                    }
                    else if (!st->cStunnedAtApply)
                    {
                        snprintf(plainroot, sizeof(plainroot), "INVALID(the kernel's Stunned inhibition was not up on it when the cast returned, so there was no stun for the projection to read)");
                    }
                    else if (st->cFlagsBefore & MOVEFLAG_ROOT)
                    {
                        snprintf(plainroot, sizeof(plainroot), "INVALID(MOVEFLAG_ROOT was already in its m_movementInfo before the cast: 0x%08x)", st->cFlagsBefore);
                    }
                    else if (st->cFlagsAtApply & MOVEFLAG_ROOT)
                    {
                        snprintf(plainroot, sizeof(plainroot), "BUG(MOVEFLAG_ROOT was in its m_movementInfo the instant the cast returned: 0x%08x -> 0x%08x, so a stun alone rooted a creature no client moves)", st->cFlagsBefore, st->cFlagsAtApply);
                    }
                    else if (!st->cSamples)
                    {
                        snprintf(plainroot, sizeof(plainroot), "INVALID(no samples with the aura on it)");
                    }
                    else if (st->cRootInWord || st->cRootWanted)
                    {
                        snprintf(plainroot, sizeof(plainroot), "BUG(it was rooted under the stun: MOVEFLAG_ROOT in its word on %u and the kernel's root wanted on %u of the %u samples the aura was on it)",
                                 st->cRootInWord, st->cRootWanted, st->cSamples);
                    }
                    else
                    {
                        snprintf(plainroot, sizeof(plainroot), "OK(stunned with no MOVEFLAG_ROOT in its m_movementInfo and no root decided, at the cast and on all %u samples the aura was on it)", st->cSamples);
                    }
                }
                // --- stunLifted, which does not need the cast step's readings: it is entirely
                // the sampler's, anchored on the kernel's own inhibition.
                if (!st->pHeldSamples)
                {
                    snprintf(lifted, sizeof(lifted), "INVALID(the kernel's Stunned inhibition never held on him)");
                }
                else
                {
                    // The flicker note, appended to whichever branch fires below. An inhibition
                    // that went mid-stun and came back is worth seeing even though the window no
                    // longer counts it -- it is a finding about the block, just not this
                    // category's -- and on a healthy run there is nothing to say.
                    char flicker[144];
                    if (st->pFlickers)
                    {
                        snprintf(flicker, sizeof(flicker), "; the inhibition flickered %u time(s) before the pull, re-anchoring the window past %u sample(s)",
                                 st->pFlickers, st->pDiscarded);
                    }
                    else
                    {
                        flicker[0] = '\0';
                    }
                    // First, and ahead of the sample-count INVALID: a stun standing again after
                    // the aura was pulled is a hard failure, and reporting "too few samples" over
                    // the top of it would hide the one thing that went wrong.
                    if (st->pBackAfterPull)
                    {
                        snprintf(lifted, sizeof(lifted), "BUG(the kernel's Stunned inhibition was up again on %u sample(s) AFTER the aura was pulled, so the stun did not end when it was ended%s)",
                                 st->pBackAfterPull, flicker);
                    }
                    else if (st->pAfterSamples < 5)
                    {
                        snprintf(lifted, sizeof(lifted), "INVALID(only %u samples after the inhibition went%s)", st->pAfterSamples, flicker);
                    }
                    else if (st->pFlagAfter)
                    {
                        snprintf(lifted, sizeof(lifted), "BUG(UNIT_FLAG_STUNNED was still on for at least one of the %u samples after the inhibition went, %u ms after the cast%s)",
                                 st->pAfterSamples, st->pEndedAt, flicker);
                    }
                    else if (st->pTargetNotBack)
                    {
                        snprintf(lifted, sizeof(lifted), "BUG(his victim's guid was not back in UNIT_FIELD_TARGET on at least one of the %u samples after the inhibition went, %u ms after the cast%s)",
                                 st->pAfterSamples, st->pEndedAt, flicker);
                    }
                    else
                    {
                        snprintf(lifted, sizeof(lifted), "OK(UNIT_FLAG_STUNNED off and his victim's guid back in UNIT_FIELD_TARGET from the first of the %u samples after the inhibition went, %u ms after the cast%s)",
                                 st->pAfterSamples, st->pEndedAt, flicker);
                    }
                }
                std::string text = std::string("moverBranch=") + branch + " | stunHeld=" + held + " | stunLifted=" + lifted +
                                   " | charmedRootSurvives=" + m1 + " | playerCarriesNoRoot=" + inflight +
                                   " | plainCreatureNotRooted=" + plainroot;
                Verdict(text);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "moverBranch=" + w + " | stunHeld=" + w + " | stunLifted=" + w +
                   " | charmedRootSurvives=" + w + " | playerCarriesNoRoot=" + w + " | plainCreatureNotRooted=" + w;
        }
    };


    namespace
    {
        /// THE CHARM THE FIRST OF THE THREE CASTS, and it is chosen the way player-stun chose
        /// 76216: by its DBC row, not by its name. 21835 "Gizlock's Dummy Charm Effect" carries
        /// ONE effect -- SPELL_EFFECT_APPLY_AURA with SPELL_AURA_MOD_CHARM, base points 100,
        /// implicit target 25 (TARGET_DUELVSPLAYER, which resolves to the unit target whether it
        /// is friendly or hostile, SpellTargeting.cpp:1119) -- and has no SpellCategories row at
        /// all, so its damage class, mechanic, category and start-recovery category all read 0.
        ///
        /// DAMAGE CLASS NONE IS THE LOAD-BEARING HALF. Unit::SpellHitResult has no self case and
        /// no charm case: a spell whose class is MELEE, RANGED or MAGIC draws a roll against its
        /// target (UnitCombat.cpp:969-977), and under the harness's fixed seed a bad one fails
        /// every run. Scenario::SelfCast warns about exactly this for a self-cast; the charm is
        /// cast AT the wolf rather than at the caster, so the warning would not cover it and the
        /// spell is picked to need no cover. Mechanic 0 matters for the same reason from the
        /// other end: MECHANIC_CHARM would put the aura in a diminishing group.
        ///
        /// The two obvious alternatives were rejected on their rows. 24261 "Brain Wash" and
        /// 35120 "Charm" both carry SPELL_ATTR_EX_CHANNELED_1, and a channel is a second thing
        /// that can end the aura for reasons that have nothing to do with this scenario; 35120
        /// also carries mechanic 1. 21835's only attribute is SPELL_ATTR_EX_UNK28 (0x10000000 of
        /// AttributesEx), which is declared in SharedDefines.h and read nowhere in this tree.
        /// Its duration index 21 is -1, an infinite aura, so the charm holds until the scenario
        /// takes it off -- which it does, before the runner's teardown, for player-stun's reason:
        /// the teardown despawns the scenario's creatures before it ends its players.
        const uint32 CHARM = 21835;

        /// One marker bit written into the subject's m_movementInfo just before the stun, and
        /// the whole falsifiability of "the word was not wiped" rests on which bit it is.
        ///
        /// MOVEFLAG_STRAFE_LEFT survives every route out of Aura::HandleAuraModStun EXCEPT the
        /// whole-word wipe. The `else` half of the handler's `if` reaches Unit::StopMoving, whose
        /// spline stop removes MOVEFLAG_FORWARD and only that (MoveSplineInit.cpp:261), and on a
        /// finalized spline StopMoving returns before even that (Unit.cpp:5823-5826). So the bit
        /// being gone afterwards can mean one thing only, and the bit being there says the
        /// clientMover branch was not taken.
        ///
        /// ADDED to the word rather than assigned over it: these subjects are creatures that may
        /// be mid-wander when the marker goes in (a wolf's default movement type is random, so
        /// MotionMaster::Initialize installs a wander on it), and assigning the word whole would
        /// take the walk and forward bits of a leg that is actually running off a unit the
        /// movement code still believes is moving. The marker is read BACK after it is written,
        /// and a category whose marker did not survive its own setup reads INVALID.
        const MovementFlags kMoverMarker = MOVEFLAG_STRAFE_LEFT;

        /// charm-stun-not-client-driven's moments, absolute offsets from the scenario's start as
        /// Scenario::At takes them; its "+Nms" log lines count from the CAST, as player-stun's do.
        ///
        /// The charm is taken 300 ms before the cast for the reason player-stun leaves 300 ms
        /// between its possession and its cast: Aura::HandleModCharm runs Creature::AIM_Initialize
        /// (SpellAuraControl.cpp:250), which re-initialises the MotionMaster -- StopMoving, a full
        /// arbiter clear, a fresh factory native -- and a charm in the same breath as the stun
        /// would leave every reading below with that re-initialisation as an alternative
        /// explanation. Three world updates pass with the charm settled before anything is cast.
        const uint32 kCharmStunCharmAt   = 200;
        const uint32 kCharmStunCastAt    = 500;
        const uint32 kCharmStunPullAt    = 3000;   ///< +2500 after the cast, inside 76216's own 6 s
        const uint32 kCharmStunUncharmAt = 3400;   ///< after the last sample: no body may be freed under a live charmer
        const uint32 kCharmStunVerdictAt = 3600;

        /// stun-then-possess-roots' moments. The stun goes on FIRST and the take follows it, which
        /// is the whole point: Unit::TakePossessOf raises the Possessed inhibition at Unit.cpp:7144
        /// -- and with it the client-root projection -- 23 lines BEFORE SetClientControl grants the
        /// mover at :7167, so at the moment of the projection the body is not yet a client mover
        /// and only WorldSession::GrantMover's own recompute can root it.
        const uint32 kLateTakeStunAt    = 200;
        const uint32 kLateTakeAt        = 500;
        const uint32 kLateTakePullAt    = 3000;
        const uint32 kLateTakeReleaseAt = 3400;
        const uint32 kLateTakeVerdictAt = 3600;

        /// release-possession-while-stunned's moments. The release falls INSIDE the stun (76216
        /// runs 6 s flat from 500, so it is still on at 3000 and still on at the verdict), because
        /// a release after the stun has already gone proves nothing about the root: the stun's own
        /// Uninhibit would have taken it off.
        const uint32 kEarlyReleaseTakeAt  = 200;
        const uint32 kEarlyReleaseCastAt  = 500;
        const uint32 kEarlyReleaseAt      = 3000;
        const uint32 kEarlyReleaseVerdict = 5200;
    }

    /// S70 (order 904): THE FIX. A creature mind-controlled through SPELL_AURA_MOD_CHARM has a
    /// player charmer and no client at all, and the stun must treat it as the server-driven unit
    /// it is.
    ///
    /// Aura::HandleModCharm (SpellAuraControl.cpp:203) sets the charmer guid and the caster's
    /// charm, re-initialises the creature's AI, gives it REACT_DEFENSIVE and ends the player
    /// branch at Player::CharmSpellInitialize -- the PET BAR. It never calls SetClientControl, so
    /// WorldSession::GrantMover never runs, MoverSession() stays NULL and the kernel's mode stays
    /// ServerDriven. Unit::TakePossessOf does call it (Unit.cpp:7167); that is the difference
    /// between the two, and it is the reason this scenario charms rather than possesses. Built on
    /// a possession it would pass either way and prove nothing.
    ///
    /// Until 2026-09-21 both readers of "does a client drive this unit" asked instead whether its
    /// charmer was a player, and this creature answered yes to that. Stunned, it therefore:
    ///   1. had its m_movementInfo zeroed WHOLE by Aura::HandleAuraModStun -- the word, not a
    ///      mask -- with nothing to put it back, because the only restoring path is
    ///      WorldSession::HandleMoverRelocation's `else // creature charmed` branch
    ///      (MovementHandler.cpp:983-988) and it is reached from a movement packet or an ack,
    ///      neither of which a unit with no client will ever send;
    ///   2. was client-rooted by MotionMaster::ProjectClientRoot, which for a unit the server
    ///      drives means nothing: a stunned plain creature is STOPPED, not rooted.
    /// Unit::IsClientMover answers the question the kernel's own mode answers, and both readers
    /// now ask it.
    ///
    /// FOUR CATEGORIES AND THE FIRST IS THE SETUP. charmGrantsNoMover is what makes the other
    /// three about a charm: it fails if the charm did not take, and it fails if the charm turned
    /// out to hand the body a mover session after all -- in which case this scenario would be a
    /// second, worse copy of player-stun's possession and every reading below would be
    /// meaningless. The stun's own block is asserted beside the two things that must NOT happen,
    /// because "nothing happened" is not the finding: the stun must still stun.
    ///
    /// WHAT IT DOES NOT CLAIM: that the wolf was STOPPED. Unit::StopMoving at
    /// SpellAuraControl.cpp:520 is over-determined -- the Inhibit two lines above already blocks
    /// the body through the arbiter -- and player-stun's plainCreatureNotRooted gives the reason
    /// in full. "Not rooted" is the half the projection decides, and it is the half asserted here.
    class CharmStunNotClientDriven : public Scenario
    {
    public:
        /// Order 904, the next in the reserved player block behind player-stun's 903: the runner
        /// refuses `MVTEST all` when a player scenario is queued before one that holds no player
        /// (Harness.cpp Start), so player scenarios take high contiguous orders of their own.
        CharmStunNotClientDriven() : Scenario("charm-stun-not-client-driven", 904) {}

        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   charmRan;         ///< the charm step resolved both actors and cast
                bool   charmed;          ///< ...and the player really was the wolf's charmer afterwards
                bool   moverAfterCharm;  ///< the charm handed it a mover session: this scenario is then not about a charm
                bool   clientAfterCharm; ///< Unit::IsClientMover, the predicate under test, the instant the charm returned
                bool   castRan;          ///< the cast step resolved both actors and cast
                bool   charmedAtCast;    ///< the player was STILL its charmer when the stun went out
                bool   moverAtCast;      ///< ...and it STILL had no mover session
                uint32 wordBefore;       ///< its word with the marker in it, read BACK after the write
                uint32 wordAtApply;      ///< the same word the instant the cast returned
                bool   auraAtApply;      ///< the 76216 holder was on it that instant
                bool   stunnedAtApply;   ///< the kernel's Stunned inhibition was up that instant
                bool   rootedAtApply;    ///< MOVEFLAG_ROOT was in its word that instant
                bool   rootDecidedAtApply; ///< ...or the kernel had decided its root
                uint32 samples;          ///< samples with the holder on it AND the charm still held
                uint32 markerGone;       ///< ...on which the marker bit had left its word
                uint32 rootInWord;       ///< ...on which MOVEFLAG_ROOT sat in its word
                uint32 rootDecided;      ///< ...on which the kernel had decided its root
                uint32 uninhibited;      ///< ...on which the kernel's Stunned inhibition was down
                uint32 gainedMover;      ///< ...on which a mover session had appeared under it
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            // 4 yd east, as player-stun's body: inside the charm's range (RangeIndex 6) and out
            // of melee reach of anything, which matters for the 200 ms before the charm -- after
            // it the wolf holds the player's own faction.
            Creature* victim = p ? Spawn(WOLF, SE.x + 4.0f, SE.y, Ground(SE.x + 4.0f, SE.y, SE.z), 3.1f) : NULL;
            if (!p || !victim)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            // Only for the 200 ms before the charm: Aura::HandleModCharm runs AIM_Initialize,
            // which hands the wolf a fresh factory AI in place of this decorator. Those 200 ms
            // are still worth buying -- they are 200 ms with a player standing 4 yd from a
            // hostile beast, and an aggro there would put a melee swing on the path of every
            // reading below. After the charm the wolf is quiet for a different reason: it holds
            // the player's faction and nothing hostile stands on the map.
            Silence(victim);
            const ObjectGuid g = p->GetObjectGuid(), gv = victim->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->charmRan = st->charmed = st->moverAfterCharm = st->clientAfterCharm = false;
            st->castRan = st->charmedAtCast = st->moverAtCast = false;
            st->wordBefore = st->wordAtApply = 0;
            st->auraAtApply = st->stunnedAtApply = st->rootedAtApply = st->rootDecidedAtApply = false;
            st->samples = st->markerGone = st->rootInWord = st->rootDecided = st->uninhibited = st->gainedMover = 0;
            Log("the player %s stands at (%.1f, %.1f); the wolf he will mind-control 4 yd east",
                g.GetString().c_str(), p->Where().X(), p->Where().Y());

            At(kCharmStunCharmAt, [this, g, gv, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* v = Get(gv);
                if (!p || !v) { return; }
                // The spell, through Unit::CastSpell, and not a hand-rolled SetCharmerGuid beside
                // it: the point of the scenario is the path Aura::HandleModCharm takes, and a
                // scenario that set the guid itself would be asserting over its own setup rather
                // than over the handler.
                p->CastSpell(v, CHARM, true);
                st->charmRan = true;
                st->charmed = v->GetCharmerGuid() == g;
                st->moverAfterCharm = v->MoverSession() != NULL;
                st->clientAfterCharm = v->IsClientMover();
                Log("the charm %u returns: aura=%d charmer=%s mover session=%d client mover=%d flags 0x%08x mt=%s",
                    CHARM, v->HasAura(CHARM) ? 1 : 0, v->GetCharmerGuid().GetString().c_str(),
                    st->moverAfterCharm ? 1 : 0, st->clientAfterCharm ? 1 : 0,
                    uint32(v->m_movementInfo.GetMovementFlags()), TypeName(v));
            });
            At(kCharmStunCastAt, [this, g, gv, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* v = Get(gv);
                if (!p || !v) { return; }
                // Written, then READ BACK, and the read is what the verdict uses: a marker that
                // did not survive its own setup would have the category report the stun's
                // restraint over a word the stun never saw.
                v->m_movementInfo.AddMovementFlag(kMoverMarker);
                st->wordBefore = uint32(v->m_movementInfo.GetMovementFlags());
                st->charmedAtCast = v->GetCharmerGuid() == g;
                st->moverAtCast = v->MoverSession() != NULL;
                SelfCast(v, STUN_SELF);
                // EVERY READING BELOW IS TAKEN HERE, in the cast's own step: a triggered instant
                // spell applies its aura inside the call (Spell::Prepare -> cast(true)), so this
                // is the state Aura::HandleAuraModStun left behind and nothing else has run yet.
                st->castRan = true;
                st->auraAtApply = v->HasAura(STUN_SELF);
                st->wordAtApply = uint32(v->m_movementInfo.GetMovementFlags());
                st->stunnedAtApply = v->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                st->rootedAtApply = (st->wordAtApply & MOVEFLAG_ROOT) != 0;
                st->rootDecidedAtApply = RootDecided(v);
                Log("the cast returns: aura=%d charmer=%d mover session=%d flags 0x%08x -> 0x%08x (marker %s, MOVEFLAG_ROOT %s) stunned=%d root decided=%d",
                    st->auraAtApply ? 1 : 0, st->charmedAtCast ? 1 : 0, st->moverAtCast ? 1 : 0,
                    st->wordBefore, st->wordAtApply,
                    (st->wordAtApply & uint32(kMoverMarker)) ? "held" : "GONE",
                    st->rootedAtApply ? "SET" : "clear",
                    st->stunnedAtApply ? 1 : 0, st->rootDecidedAtApply ? 1 : 0);
            });
            // Registered before the sampler so it runs first at its own moment (the timeline
            // orders a tie by insertion).
            At(kCharmStunPullAt, [this, gv]()
            {
                Creature* v = Get(gv); if (!v) { return; }
                v->RemoveAurasDueToSpell(STUN_SELF);
                Log("+%4ums the stun pulled: stunned=%d flags 0x%08x mt=%s", kCharmStunPullAt - kCharmStunCastAt,
                    v->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0,
                    uint32(v->m_movementInfo.GetMovementFlags()), TypeName(v));
            });
            for (uint32 i = 1; i <= 25; ++i)
            {
                At(kCharmStunCastAt + i * 100, [this, g, gv, st, i]()
                {
                    Creature* v = Get(gv); if (!v) { return; }
                    const uint32 t = i * 100;
                    const uint32 word = uint32(v->m_movementInfo.GetMovementFlags());
                    // The window is keyed on the CHARM as well as on the aura: the uncharm at the
                    // end of the run drops the charmer, and a sample taken after it would be
                    // about an ordinary creature and would say nothing about this fix.
                    if (v->HasAura(STUN_SELF) && v->GetCharmerGuid() == g)
                    {
                        ++st->samples;
                        if (!(word & uint32(kMoverMarker))) { ++st->markerGone; }
                        if (word & MOVEFLAG_ROOT) { ++st->rootInWord; }
                        if (RootDecided(v)) { ++st->rootDecided; }
                        if (!v->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned)) { ++st->uninhibited; }
                        if (v->MoverSession() != NULL) { ++st->gainedMover; }
                    }
                    if (i % 10 == 0)
                    {
                        Log("+%4ums aura=%d charmer=%d stunned=%d flags 0x%08x root decided=%d mt=%s", t,
                            v->HasAura(STUN_SELF) ? 1 : 0, v->GetCharmerGuid() == g ? 1 : 0,
                            v->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0,
                            word, RootDecided(v) ? 1 : 0, TypeName(v));
                    }
                });
            }
            At(kCharmStunUncharmAt, [this, gv]()
            {
                Creature* v = Get(gv); if (!v) { return; }
                // Before the runner's teardown rather than left to it, for player-stun's reason:
                // the teardown despawns the scenario's creatures BEFORE it ends its players
                // (Harness.cpp), so a body still charmed at that point would be freed under a
                // live charmer. Removing the aura is the charm's own door out -- it runs
                // Aura::HandleModCharm's `apply == false` half, which is what the charm's expiry
                // or a dispel would run.
                v->RemoveAurasDueToSpell(CHARM);
                Log("the charm removed: charmer=%s flags 0x%08x mt=%s",
                    v->GetCharmerGuid().GetString().c_str(),
                    uint32(v->m_movementInfo.GetMovementFlags()), TypeName(v));
            });
            At(kCharmStunVerdictAt, [this, st]()
            {
                char setup[352], word[352], root[352], held[320];
                // --- charmGrantsNoMover: the setup, and the reason the other three are about a
                // charm at all. Every INVALID here is a scenario that could not be run; the two
                // BUGs are a charm that behaved like a possession, which is a finding in itself.
                if (!st->charmRan)
                {
                    snprintf(setup, sizeof(setup), "INVALID(the charm step never ran: an actor went unresolvable)");
                }
                else if (!st->charmed)
                {
                    snprintf(setup, sizeof(setup), "INVALID(%u did not charm the wolf: it held no charmer guid when the cast returned, so no player-charmed creature was built)", CHARM);
                }
                else if (st->moverAfterCharm)
                {
                    snprintf(setup, sizeof(setup), "BUG(the charm handed the wolf a mover session, so Aura::HandleModCharm granted a mover after all and this is a possession, not a charm)");
                }
                else if (st->clientAfterCharm)
                {
                    snprintf(setup, sizeof(setup), "BUG(Unit::IsClientMover read true for a charmed creature with no mover session: the predicate is still asking who owns the unit, not who drives it)");
                }
                else
                {
                    snprintf(setup, sizeof(setup), "OK(the charm gave the wolf a player charmer and NO mover session, and Unit::IsClientMover reads false: the body is server-driven and no client will ever resend its word)");
                }
                // --- wordSurvives, notClientRooted and stunHeld all need the cast step.
                if (!st->castRan)
                {
                    snprintf(word, sizeof(word), "INVALID(the cast step never ran: an actor went unresolvable)");
                    snprintf(root, sizeof(root), "INVALID(the cast step never ran)");
                    snprintf(held, sizeof(held), "INVALID(the cast step never ran)");
                }
                else if (!st->charmedAtCast || st->moverAtCast)
                {
                    // Said once, in all three, because it is one fact: the subject was not the
                    // shape the scenario is about when the stun went out.
                    char w[288];
                    snprintf(w, sizeof(w), "INVALID(when the stun went out the wolf was charmed=%d and held a mover session=%d, so it was not a client-less player-charmed creature)",
                             st->charmedAtCast ? 1 : 0, st->moverAtCast ? 1 : 0);
                    snprintf(word, sizeof(word), "%s", w);
                    snprintf(root, sizeof(root), "%s", w);
                    snprintf(held, sizeof(held), "%s", w);
                }
                else if (!st->auraAtApply)
                {
                    char w[224];
                    snprintf(w, sizeof(w), "INVALID(no %u holder on it when the cast returned, so Aura::HandleAuraModStun was never on the path)", STUN_SELF);
                    snprintf(word, sizeof(word), "%s", w);
                    snprintf(root, sizeof(root), "%s", w);
                    snprintf(held, sizeof(held), "%s", w);
                }
                else
                {
                    // --- wordSurvives: the whole-word wipe at SpellAuraControl.cpp:508 did not
                    // run. The marker bit, not the whole word compared to something, because the
                    // very handler under test legitimately removes MOVEFLAG_FORWARD through
                    // StopMoving and the two claims must be able to fail apart.
                    if (!(st->wordBefore & uint32(kMoverMarker)))
                    {
                        snprintf(word, sizeof(word), "INVALID(the marker bit did not survive its own setup: the word read back 0x%08x, so the wipe would have had nothing of ours to clear)", st->wordBefore);
                    }
                    else if (!(st->wordAtApply & uint32(kMoverMarker)))
                    {
                        snprintf(word, sizeof(word), "BUG(the marker was gone from its m_movementInfo the instant the cast returned: 0x%08x -> 0x%08x, so the stun zeroed the authoritative word of a unit with no client to resend it)",
                                 st->wordBefore, st->wordAtApply);
                    }
                    else if (!st->samples)
                    {
                        snprintf(word, sizeof(word), "INVALID(no samples with the stun on it while the charm still held)");
                    }
                    else if (st->markerGone)
                    {
                        snprintf(word, sizeof(word), "BUG(the marker had left its m_movementInfo on %u of the %u samples the stun was on it under the charm)", st->markerGone, st->samples);
                    }
                    else
                    {
                        snprintf(word, sizeof(word), "OK(0x%08x -> 0x%08x the instant the cast returned and the marker held on all %u samples under the charm: the clientMover branch was not taken)",
                                 st->wordBefore, st->wordAtApply, st->samples);
                    }
                    // --- notClientRooted: the projection left it alone. A stunned server-driven
                    // unit is stopped, not rooted; player-stun's plainCreatureNotRooted is the
                    // same claim about a creature nobody charmed.
                    if (st->rootedAtApply)
                    {
                        snprintf(root, sizeof(root), "BUG(MOVEFLAG_ROOT was in its m_movementInfo the instant the cast returned: 0x%08x -> 0x%08x, so the projection client-rooted a unit the server drives)",
                                 st->wordBefore, st->wordAtApply);
                    }
                    else if (st->rootDecidedAtApply)
                    {
                        snprintf(root, sizeof(root), "BUG(the kernel had decided its root the instant the cast returned, so MotionMaster::ProjectClientRoot asked for a root no client will ever ack)");
                    }
                    else if (!st->samples)
                    {
                        snprintf(root, sizeof(root), "INVALID(no samples with the stun on it while the charm still held)");
                    }
                    else if (st->rootInWord || st->rootDecided)
                    {
                        snprintf(root, sizeof(root), "BUG(it was client-rooted under the stun: MOVEFLAG_ROOT in its word on %u and the kernel's root decided on %u of the %u samples under the charm)",
                                 st->rootInWord, st->rootDecided, st->samples);
                    }
                    else
                    {
                        snprintf(root, sizeof(root), "OK(no MOVEFLAG_ROOT in its m_movementInfo and no root decided, at the cast and on all %u samples the stun was on it under the charm)", st->samples);
                    }
                    // --- stunHeld: the fix took a wipe and a root away and must have taken
                    // nothing else. A stun that stopped stunning would pass both categories above.
                    if (!st->stunnedAtApply)
                    {
                        snprintf(held, sizeof(held), "BUG(the kernel's Stunned inhibition was not up when the cast returned, so the stun raised no block at all)");
                    }
                    else if (!st->samples)
                    {
                        snprintf(held, sizeof(held), "INVALID(no samples with the stun on it while the charm still held)");
                    }
                    else if (st->uninhibited)
                    {
                        snprintf(held, sizeof(held), "BUG(the kernel's Stunned inhibition was down on %u of the %u samples the stun was on it under the charm)", st->uninhibited, st->samples);
                    }
                    else if (st->gainedMover)
                    {
                        snprintf(held, sizeof(held), "BUG(a mover session appeared under it on %u of the %u samples, so the body stopped being the client-less creature the other categories were read over)", st->gainedMover, st->samples);
                    }
                    else
                    {
                        snprintf(held, sizeof(held), "OK(the kernel's Stunned inhibition was up at the cast and on all %u samples the stun was on it, with no mover session under it on any of them)", st->samples);
                    }
                }
                std::string text = std::string("charmGrantsNoMover=") + setup + " | wordSurvives=" + word +
                                   " | notClientRooted=" + root + " | stunHeld=" + held;
                Verdict(text);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "charmGrantsNoMover=" + w + " | wordSurvives=" + w + " | notClientRooted=" + w + " | stunHeld=" + w;
        }
    };

    /// S71 (order 905): the ordering the grant's recompute exists for. A creature stunned BEFORE
    /// it is possessed must come out of the take client-rooted, and under Unit::IsClientMover it
    /// can only do so if WorldSession::GrantMover recomputes the projection.
    ///
    /// Unit::TakePossessOf sets the charmer at Unit.cpp:7137 and raises the Possessed inhibition
    /// at :7144 -- and MotionMaster::Inhibit projects the client root as its last act -- but
    /// SetClientControl does not grant the mover until :7167. The old predicate read the charmer,
    /// which was already set at :7144, so the projection rooted the body there. The new one reads
    /// the authority, which is not handed over until :7167, so the projection at :7144 correctly
    /// decides nothing and the ROOT arrives with the grant instead. Take the recompute out of
    /// GrantMover and this scenario's rootAtTake goes BUG while nothing else moves.
    ///
    /// The contrast with player-stun is the whole design of the pair: there the body is possessed
    /// first and stunned after, so the stun's own Inhibit does the projecting and the grant has
    /// long since run. Here the two are swapped, and that is the only ordering in which the
    /// grant's recompute is the only thing that can root the body.
    class StunThenPossessRoots : public Scenario
    {
    public:
        StunThenPossessRoots() : Scenario("stun-then-possess-roots", 905) {}

        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   stunRan;           ///< the stun step resolved the wolf and cast
                bool   auraBefore;        ///< the 76216 holder was on it when that cast returned
                bool   stunnedBefore;     ///< the kernel's Stunned inhibition was up before the take
                uint32 wordBefore;        ///< its word before the take
                bool   rootedBefore;      ///< MOVEFLAG_ROOT was in it: a plain stunned creature must NOT be rooted
                bool   rootDecidedBefore; ///< ...or the kernel had decided its root
                bool   takeRan;           ///< the take step resolved both actors
                bool   took;              ///< TakePossessOf returned true
                bool   charmedAtTake;     ///< the player was its charmer the instant the take returned
                bool   moverAtTake;       ///< ...and a mover session was under it
                bool   clientAtTake;      ///< ...and Unit::IsClientMover read true
                bool   possessedAtTake;   ///< ...and the Possessed inhibition was up
                bool   stunnedAtTake;     ///< ...and the Stunned inhibition still was
                uint32 wordAtTake;        ///< the word that instant: the reading the scenario exists for
                uint32 samples;           ///< samples with the stun on it AND the possession still held
                uint32 rootMissing;       ///< ...on which MOVEFLAG_ROOT was gone from its word
                uint32 moverLost;         ///< ...on which the mover session had gone from under it
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* body = p ? Spawn(WOLF, SE.x + 4.0f, SE.y, Ground(SE.x + 4.0f, SE.y, SE.z), 3.1f) : NULL;
            if (!p || !body)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            Silence(body);
            const ObjectGuid g = p->GetObjectGuid(), gb = body->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->stunRan = st->auraBefore = st->stunnedBefore = st->rootedBefore = st->rootDecidedBefore = false;
            st->takeRan = st->took = st->charmedAtTake = st->moverAtTake = false;
            st->clientAtTake = st->possessedAtTake = st->stunnedAtTake = false;
            st->wordBefore = st->wordAtTake = 0;
            st->samples = st->rootMissing = st->moverLost = 0;
            Log("the player %s stands at (%.1f, %.1f); the wolf he will stun and THEN possess 4 yd east",
                g.GetString().c_str(), p->Where().X(), p->Where().Y());

            At(kLateTakeStunAt, [this, gb, st]()
            {
                Creature* b = Get(gb); if (!b) { return; }
                SelfCast(b, STUN_SELF);
                st->stunRan = true;
                st->auraBefore = b->HasAura(STUN_SELF);
                Log("the stun returns on the plain wolf: aura=%d stunned=%d flags 0x%08x root decided=%d",
                    st->auraBefore ? 1 : 0, b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0,
                    uint32(b->m_movementInfo.GetMovementFlags()), RootDecided(b) ? 1 : 0);
            });
            At(kLateTakeAt, [this, g, gb, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* b = Get(gb);
                if (!p || !b) { return; }
                // Read IMMEDIATELY before the take and used by the verdict: a body already rooted
                // before the take would make "it came out of the take rooted" prove nothing, and a
                // body no longer stunned would make it prove something else entirely.
                st->stunnedBefore = b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                st->wordBefore = uint32(b->m_movementInfo.GetMovementFlags());
                st->rootedBefore = (st->wordBefore & MOVEFLAG_ROOT) != 0;
                st->rootDecidedBefore = RootDecided(b);
                // Unit::TakePossessOf and nothing hand-rolled beside it: it is the call the
                // possess effect makes, and its internal ordering -- the charmer, the inhibition,
                // then twenty-three lines later the grant -- is the thing under test.
                st->took = p->TakePossessOf(b);
                st->takeRan = true;
                st->wordAtTake = uint32(b->m_movementInfo.GetMovementFlags());
                st->charmedAtTake = b->GetCharmerGuid() == g;
                st->moverAtTake = b->MoverSession() == p->GetSession();
                st->clientAtTake = b->IsClientMover();
                st->possessedAtTake = b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed);
                st->stunnedAtTake = b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                Log("the take returns: took=%d charmer=%d mover session=%d client mover=%d possessed=%d stunned=%d flags 0x%08x -> 0x%08x (MOVEFLAG_ROOT %s)",
                    st->took ? 1 : 0, st->charmedAtTake ? 1 : 0, st->moverAtTake ? 1 : 0, st->clientAtTake ? 1 : 0,
                    st->possessedAtTake ? 1 : 0, st->stunnedAtTake ? 1 : 0, st->wordBefore, st->wordAtTake,
                    (st->wordAtTake & MOVEFLAG_ROOT) ? "SET" : "MISSING");
            });
            At(kLateTakePullAt, [this, gb]()
            {
                Creature* b = Get(gb); if (!b) { return; }
                b->RemoveAurasDueToSpell(STUN_SELF);
                Log("+%4ums the stun pulled: stunned=%d flags 0x%08x", kLateTakePullAt - kLateTakeAt,
                    b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0,
                    uint32(b->m_movementInfo.GetMovementFlags()));
            });
            for (uint32 i = 1; i <= 24; ++i)
            {
                At(kLateTakeAt + i * 100, [this, g, gb, st, i]()
                {
                    Player* p = sPlayerRegistry.Find(g);
                    Creature* b = Get(gb); if (!b) { return; }
                    const uint32 t = i * 100;
                    const uint32 word = uint32(b->m_movementInfo.GetMovementFlags());
                    if (b->HasAura(STUN_SELF) && b->GetCharmerGuid() == g)
                    {
                        ++st->samples;
                        if (!(word & MOVEFLAG_ROOT)) { ++st->rootMissing; }
                        if (!p || b->MoverSession() != p->GetSession()) { ++st->moverLost; }
                    }
                    if (i % 10 == 0)
                    {
                        Log("+%4ums aura=%d charmer=%d stunned=%d possessed=%d flags 0x%08x", t,
                            b->HasAura(STUN_SELF) ? 1 : 0, b->GetCharmerGuid() == g ? 1 : 0,
                            b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0,
                            b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0, word);
                    }
                });
            }
            At(kLateTakeReleaseAt, [this, g, gb]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // After the last sample and before the runner's teardown: a body still charmed
                // when the teardown despawns it would be freed under a live charmer. The wolf is
                // a creature, which keeps it clear of the unconditional Creature* cast in
                // Unit::ResetControlState that a possessed PLAYER would walk into.
                p->ResetControlState(false);
                Creature* b = Get(gb);
                Log("the possession released: charmer=%s flags 0x%08x possessed=%d",
                    b ? b->GetCharmerGuid().GetString().c_str() : "gone",
                    b ? uint32(b->m_movementInfo.GetMovementFlags()) : 0,
                    b && b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0);
            });
            At(kLateTakeVerdictAt, [this, st]()
            {
                char before[320], rooted[352], granted[320];
                // --- stunnedNotRootedBeforeTake: the setup, and it is also player-stun's
                // plainCreatureNotRooted read once more at a different moment -- deliberately, so
                // that "it came out of the take rooted" cannot be satisfied by a body that was
                // already rooted going in.
                if (!st->stunRan)
                {
                    snprintf(before, sizeof(before), "INVALID(the stun step never ran: the wolf went unresolvable)");
                }
                else if (!st->auraBefore)
                {
                    snprintf(before, sizeof(before), "INVALID(no %u holder on it when the first cast returned)", STUN_SELF);
                }
                else if (!st->takeRan)
                {
                    snprintf(before, sizeof(before), "INVALID(the take step never ran: an actor went unresolvable)");
                }
                else if (!st->stunnedBefore)
                {
                    snprintf(before, sizeof(before), "INVALID(the kernel's Stunned inhibition was down again before the take, so there was no stun for the take to find)");
                }
                else if (st->rootedBefore || st->rootDecidedBefore)
                {
                    snprintf(before, sizeof(before), "BUG(the plain stunned wolf was already client-rooted before the take: word 0x%08x, root decided=%d -- a stun alone rooted a unit the server drives)",
                             st->wordBefore, st->rootDecidedBefore ? 1 : 0);
                }
                else
                {
                    snprintf(before, sizeof(before), "OK(stunned and NOT client-rooted going into the take: word 0x%08x, no MOVEFLAG_ROOT and no root decided)", st->wordBefore);
                }
                // --- rootAtTake and moverGranted both need the take.
                if (!st->takeRan)
                {
                    snprintf(rooted, sizeof(rooted), "INVALID(the take step never ran: an actor went unresolvable)");
                    snprintf(granted, sizeof(granted), "INVALID(the take step never ran)");
                }
                else if (!st->took)
                {
                    snprintf(rooted, sizeof(rooted), "INVALID(TakePossessOf refused, so no possession was built)");
                    snprintf(granted, sizeof(granted), "INVALID(TakePossessOf refused)");
                }
                else
                {
                    // --- moverGranted: the authority, which is what the projection now reads.
                    // Said before rootAtTake in the verdict line's order of thought, though it
                    // prints second: a take that granted no mover would make rootAtTake a
                    // question about the old predicate instead.
                    if (!st->charmedAtTake)
                    {
                        snprintf(granted, sizeof(granted), "INVALID(the wolf held no charmer guid when the take returned)");
                    }
                    else if (!st->possessedAtTake)
                    {
                        snprintf(granted, sizeof(granted), "INVALID(the Possessed inhibition was not up when the take returned, so the take did not complete)");
                    }
                    else if (!st->moverAtTake)
                    {
                        snprintf(granted, sizeof(granted), "BUG(no mover session under the body when the take returned: SetClientControl's grant did not reach it, so nothing client-drives it)");
                    }
                    else if (!st->clientAtTake)
                    {
                        snprintf(granted, sizeof(granted), "BUG(Unit::IsClientMover read false over a live mover session)");
                    }
                    else if (!st->samples)
                    {
                        snprintf(granted, sizeof(granted), "INVALID(no samples with the stun on it while the possession still held)");
                    }
                    else if (st->moverLost)
                    {
                        snprintf(granted, sizeof(granted), "BUG(the mover session had gone from under the body on %u of the %u samples the possession held)", st->moverLost, st->samples);
                    }
                    else
                    {
                        snprintf(granted, sizeof(granted), "OK(the take handed the body the possessor's session and Unit::IsClientMover read true, and the session stayed under it on all %u samples)", st->samples);
                    }
                    // --- rootAtTake: the finding. The grant's recompute is the only thing that
                    // can have put MOVEFLAG_ROOT there -- the projection at Unit.cpp:7144 ran
                    // before the body had an authority at all.
                    if (!st->stunnedAtTake)
                    {
                        snprintf(rooted, sizeof(rooted), "INVALID(the kernel's Stunned inhibition was gone the instant the take returned, so there was no stun left for the projection to read)");
                    }
                    else if (!st->moverAtTake)
                    {
                        snprintf(rooted, sizeof(rooted), "INVALID(the take granted no mover session, so the recompute under test had no authority to read)");
                    }
                    else if (!(st->wordAtTake & MOVEFLAG_ROOT))
                    {
                        snprintf(rooted, sizeof(rooted), "BUG(MOVEFLAG_ROOT was not in its m_movementInfo the instant the take returned: 0x%08x -> 0x%08x -- the projection ran at Unit.cpp:7144 before the grant and nothing recomputed it after)",
                                 st->wordBefore, st->wordAtTake);
                    }
                    else if (!st->samples)
                    {
                        snprintf(rooted, sizeof(rooted), "INVALID(no samples with the stun on it while the possession still held)");
                    }
                    else if (st->rootMissing)
                    {
                        snprintf(rooted, sizeof(rooted), "BUG(MOVEFLAG_ROOT was gone from its m_movementInfo on %u of the %u samples the stun was on it under the possession)", st->rootMissing, st->samples);
                    }
                    else
                    {
                        snprintf(rooted, sizeof(rooted), "OK(0x%08x -> 0x%08x the instant the take returned and MOVEFLAG_ROOT held on all %u samples under the possession: the grant recomputed the projection)",
                                 st->wordBefore, st->wordAtTake, st->samples);
                    }
                }
                std::string text = std::string("stunnedNotRootedBeforeTake=") + before + " | rootAtTake=" + rooted +
                                   " | moverGranted=" + granted;
                Verdict(text);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "stunnedNotRootedBeforeTake=" + w + " | rootAtTake=" + w + " | moverGranted=" + w;
        }
    };

    /// S72 (order 906): the mirror, and the revoke's half of the same recompute. A possession
    /// released while the body is still stunned must lose the client root at the release, because
    /// the body reverts to server-driven and a stunned server-driven creature is stopped, not
    /// rooted -- and it must lose ONLY the root: the stun's own block is nobody's to end here.
    ///
    /// Unit::ResetControlState clears the charmer at Unit.cpp:7229 and lifts the Possessed
    /// inhibition at :7235, and the projection that runs there still sees a live mover session,
    /// because SetClientControl does not revoke until :7242. So under Unit::IsClientMover the
    /// body is still a client mover at the Uninhibit and stays rooted through it; only
    /// WorldSession::RevokeMover's own recompute can take the root off. Take it out and this
    /// scenario's rootOffAtRelease goes BUG while nothing else moves.
    ///
    /// WHAT THIS IS NOT, measured rather than argued: it is not a defect that was already there.
    /// Under the OLD predicate the root came off here anyway, because ResetControlState clears
    /// the charmer at :7229 BEFORE the Uninhibit at :7235, so the projection at the Uninhibit
    /// read a plain creature and unrooted it on the spot. A build carrying the old predicate and
    /// no revoke recompute was run against this scenario on 2026-09-21 and its rootOffAtRelease
    /// reads OK. The recompute is therefore keeping the NEW predicate from introducing a
    /// regression, not repairing an old one -- which is why it is not optional, and why this
    /// scenario exists.
    ///
    /// The release falls INSIDE the stun on purpose. Released after it, the stun's own Uninhibit
    /// would have unrooted the body and the category would read OK over a path that has nothing
    /// to do with the revoke.
    class ReleasePossessionWhileStunned : public Scenario
    {
    public:
        ReleasePossessionWhileStunned() : Scenario("release-possession-while-stunned", 906) {}

        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   took;              ///< TakePossessOf returned true
                bool   castRan;           ///< the cast step resolved the body and cast
                bool   auraAtApply;       ///< the 76216 holder was on it when the cast returned
                bool   charmedAtCast;     ///< the player was its charmer then
                bool   moverAtCast;       ///< ...and a mover session was under it
                uint32 wordAtApply;       ///< the word that instant: MOVEFLAG_ROOT must be in it
                uint32 heldSamples;       ///< samples before the release with the stun on it AND the possession held
                uint32 heldRootMissing;   ///< ...on which MOVEFLAG_ROOT was gone
                bool   releaseRan;        ///< the release step resolved the player and ran
                uint32 wordAtRelease;     ///< the word the instant ResetControlState returned
                bool   moverAtRelease;    ///< a mover session was STILL under it that instant
                bool   clientAtRelease;   ///< ...and Unit::IsClientMover still read true
                bool   stunnedAtRelease;  ///< the kernel's Stunned inhibition was still up that instant
                bool   possessedAtRelease;///< ...and the Possessed inhibition was down
                uint32 afterSamples;      ///< samples after the release with the stun still on it
                uint32 afterRooted;       ///< ...on which MOVEFLAG_ROOT was back (or never left)
                uint32 afterUninhibited;  ///< ...on which the Stunned inhibition had gone with it
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* body = p ? Spawn(WOLF, SE.x + 4.0f, SE.y, Ground(SE.x + 4.0f, SE.y, SE.z), 3.1f) : NULL;
            if (!p || !body)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            Silence(body);
            const ObjectGuid g = p->GetObjectGuid(), gb = body->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->took = st->castRan = st->auraAtApply = st->charmedAtCast = st->moverAtCast = false;
            st->releaseRan = st->moverAtRelease = st->clientAtRelease = false;
            st->stunnedAtRelease = st->possessedAtRelease = false;
            st->wordAtApply = st->wordAtRelease = 0;
            st->heldSamples = st->heldRootMissing = st->afterSamples = st->afterRooted = st->afterUninhibited = 0;
            Log("the player %s stands at (%.1f, %.1f); the wolf he will possess, stun and release under the stun 4 yd east",
                g.GetString().c_str(), p->Where().X(), p->Where().Y());

            At(kEarlyReleaseTakeAt, [this, g, gb, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* b = Get(gb);
                if (!p || !b) { return; }
                st->took = p->TakePossessOf(b);
                Log("the player possesses the wolf: %d, charmer=%s mover session=%d possessed=%d flags 0x%08x",
                    st->took ? 1 : 0, b->GetCharmerGuid().GetString().c_str(),
                    b->MoverSession() == p->GetSession() ? 1 : 0,
                    b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0,
                    uint32(b->m_movementInfo.GetMovementFlags()));
            });
            At(kEarlyReleaseCastAt, [this, g, gb, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* b = Get(gb);
                if (!p || !b) { return; }
                st->charmedAtCast = b->GetCharmerGuid() == g;
                st->moverAtCast = b->MoverSession() == p->GetSession();
                SelfCast(b, STUN_SELF);
                st->castRan = true;
                st->auraAtApply = b->HasAura(STUN_SELF);
                st->wordAtApply = uint32(b->m_movementInfo.GetMovementFlags());
                Log("the cast returns on the possessed body: aura=%d charmer=%d mover session=%d flags 0x%08x (MOVEFLAG_ROOT %s) stunned=%d",
                    st->auraAtApply ? 1 : 0, st->charmedAtCast ? 1 : 0, st->moverAtCast ? 1 : 0, st->wordAtApply,
                    (st->wordAtApply & MOVEFLAG_ROOT) ? "SET" : "MISSING",
                    b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned) ? 1 : 0);
            });
            // Registered before the sampler so it runs first at its own moment: the sample at
            // +2500 after the cast is then already an after-sample.
            At(kEarlyReleaseAt, [this, g, gb, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* b = Get(gb);
                if (!p || !b) { return; }
                // Read IMMEDIATELY before the release: the release is only about the revoke if
                // the body is still a client mover going into it, and only about a stunned body
                // if the stun is still on.
                st->moverAtRelease = b->MoverSession() == p->GetSession();
                st->clientAtRelease = b->IsClientMover();
                // Unit::ResetControlState(false), the call the possess aura's own removal makes;
                // false so the body is not turned on its former charmer, which would put an
                // AIM_Initialize and an AttackedBy on the path of every reading after it.
                p->ResetControlState(false);
                st->releaseRan = true;
                st->wordAtRelease = uint32(b->m_movementInfo.GetMovementFlags());
                st->stunnedAtRelease = b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                st->possessedAtRelease = !b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed);
                Log("+%4ums the possession released under the stun: charmer=%s mover session now=%d flags 0x%08x (MOVEFLAG_ROOT %s) stunned=%d possessed=%d",
                    kEarlyReleaseAt - kEarlyReleaseCastAt, b->GetCharmerGuid().GetString().c_str(),
                    b->MoverSession() != NULL ? 1 : 0, st->wordAtRelease,
                    (st->wordAtRelease & MOVEFLAG_ROOT) ? "STILL SET" : "gone",
                    st->stunnedAtRelease ? 1 : 0,
                    b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0);
            });
            for (uint32 i = 1; i <= 45; ++i)
            {
                At(kEarlyReleaseCastAt + i * 100, [this, g, gb, st, i]()
                {
                    Creature* b = Get(gb); if (!b) { return; }
                    const uint32 t = i * 100;
                    const uint32 word = uint32(b->m_movementInfo.GetMovementFlags());
                    const bool stunned = b->GetMotionMaster()->Inhibited(Motion::Inhibition::Stunned);
                    if (!b->HasAura(STUN_SELF))
                    {
                        // Past the aura's own 6 s, or pulled: neither window says anything then.
                    }
                    else if (!st->releaseRan)
                    {
                        // The held window is keyed on the possession as well as on the aura, so a
                        // release that ran early cannot be counted here as a missing root.
                        if (b->GetCharmerGuid() == g)
                        {
                            ++st->heldSamples;
                            if (!(word & MOVEFLAG_ROOT)) { ++st->heldRootMissing; }
                        }
                    }
                    else
                    {
                        ++st->afterSamples;
                        if (word & MOVEFLAG_ROOT) { ++st->afterRooted; }
                        if (!stunned) { ++st->afterUninhibited; }
                    }
                    if (i % 10 == 0)
                    {
                        Log("+%4ums aura=%d charmer=%d mover session=%d stunned=%d possessed=%d flags 0x%08x", t,
                            b->HasAura(STUN_SELF) ? 1 : 0, b->GetCharmerGuid() == g ? 1 : 0,
                            b->MoverSession() != NULL ? 1 : 0, stunned ? 1 : 0,
                            b->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0, word);
                    }
                });
            }
            At(kEarlyReleaseVerdict, [this, st]()
            {
                char held[352], off[352], stun[352];
                // --- rootedWhileHeld: the setup. Everything after it is about a root coming off,
                // and a root that was never on cannot come off.
                if (!st->took)
                {
                    snprintf(held, sizeof(held), "INVALID(TakePossessOf refused, so no possession was built)");
                }
                else if (!st->castRan)
                {
                    snprintf(held, sizeof(held), "INVALID(the cast step never ran: an actor went unresolvable)");
                }
                else if (!st->charmedAtCast || !st->moverAtCast)
                {
                    snprintf(held, sizeof(held), "INVALID(when the stun went out the body was charmed=%d and held the possessor's session=%d, so it was not a client-driven possessed body)",
                             st->charmedAtCast ? 1 : 0, st->moverAtCast ? 1 : 0);
                }
                else if (!st->auraAtApply)
                {
                    snprintf(held, sizeof(held), "INVALID(no %u holder on it when the cast returned)", STUN_SELF);
                }
                else if (!(st->wordAtApply & MOVEFLAG_ROOT))
                {
                    snprintf(held, sizeof(held), "BUG(MOVEFLAG_ROOT was not in its m_movementInfo the instant the cast returned: 0x%08x, so the stun never client-rooted the possessed body)", st->wordAtApply);
                }
                else if (!st->heldSamples)
                {
                    snprintf(held, sizeof(held), "INVALID(no samples with the stun on it before the release while the possession held)");
                }
                else if (st->heldRootMissing)
                {
                    snprintf(held, sizeof(held), "BUG(MOVEFLAG_ROOT was gone from its m_movementInfo on %u of the %u samples before the release)", st->heldRootMissing, st->heldSamples);
                }
                else
                {
                    snprintf(held, sizeof(held), "OK(client-rooted from the instant the cast returned, 0x%08x, and on all %u samples up to the release)", st->wordAtApply, st->heldSamples);
                }
                // --- rootOffAtRelease and stunSurvivesRelease both need the release.
                if (!st->releaseRan)
                {
                    snprintf(off, sizeof(off), "INVALID(the release step never ran: an actor went unresolvable)");
                    snprintf(stun, sizeof(stun), "INVALID(the release step never ran)");
                }
                else if (!st->moverAtRelease || !st->clientAtRelease)
                {
                    char w[320];
                    snprintf(w, sizeof(w), "INVALID(going into the release the body held the possessor's session=%d and Unit::IsClientMover read %d, so there was no authority for the revoke to take away)",
                             st->moverAtRelease ? 1 : 0, st->clientAtRelease ? 1 : 0);
                    snprintf(off, sizeof(off), "%s", w);
                    snprintf(stun, sizeof(stun), "%s", w);
                }
                else if (!st->stunnedAtRelease)
                {
                    char w[288];
                    snprintf(w, sizeof(w), "INVALID(the kernel's Stunned inhibition was already down when the release returned, so the release did not fall inside the stun)");
                    snprintf(off, sizeof(off), "%s", w);
                    snprintf(stun, sizeof(stun), "%s", w);
                }
                else
                {
                    // --- rootOffAtRelease: the finding.
                    if (!st->possessedAtRelease)
                    {
                        snprintf(off, sizeof(off), "INVALID(the Possessed inhibition was still up when the release returned, so the release did not complete)");
                    }
                    else if (st->wordAtRelease & MOVEFLAG_ROOT)
                    {
                        snprintf(off, sizeof(off), "BUG(MOVEFLAG_ROOT was still in its m_movementInfo the instant the release returned: 0x%08x -- the body reverted to server-driven and nothing recomputed the projection)", st->wordAtRelease);
                    }
                    else if (st->afterSamples < 5)
                    {
                        snprintf(off, sizeof(off), "INVALID(only %u samples with the stun still on it after the release)", st->afterSamples);
                    }
                    else if (st->afterRooted)
                    {
                        snprintf(off, sizeof(off), "BUG(MOVEFLAG_ROOT was back in its m_movementInfo on %u of the %u samples with the stun still on it after the release)", st->afterRooted, st->afterSamples);
                    }
                    else
                    {
                        snprintf(off, sizeof(off), "OK(MOVEFLAG_ROOT gone the instant the release returned, 0x%08x, and still gone on all %u samples with the stun still on it: the revoke recomputed the projection)",
                                 st->wordAtRelease, st->afterSamples);
                    }
                    // --- stunSurvivesRelease: and ONLY the root came off. A release that ended
                    // the stun would satisfy rootOffAtRelease for entirely the wrong reason.
                    if (st->afterSamples < 5)
                    {
                        snprintf(stun, sizeof(stun), "INVALID(only %u samples with the stun still on it after the release)", st->afterSamples);
                    }
                    else if (st->afterUninhibited)
                    {
                        snprintf(stun, sizeof(stun), "BUG(the kernel's Stunned inhibition was down on %u of the %u samples after the release while the 76216 holder was still on it, so the revoke ended the stun and not just the root)",
                                 st->afterUninhibited, st->afterSamples);
                    }
                    else
                    {
                        snprintf(stun, sizeof(stun), "OK(the kernel's Stunned inhibition was up when the release returned and on all %u samples after it: the revoke took the root and left the block)", st->afterSamples);
                    }
                }
                std::string text = std::string("rootedWhileHeld=") + held + " | rootOffAtRelease=" + off +
                                   " | stunSurvivesRelease=" + stun;
                Verdict(text);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "rootedWhileHeld=" + w + " | rootOffAtRelease=" + w + " | stunSurvivesRelease=" + w;
        }
    };


    namespace
    {
        /// THE PET'S TEMPLATE, and it is read out of the database rather than picked by name.
        /// 416 "Imp" is the warlock's own SUMMON_PET: `pet_levelstats` covers it from level 1 to
        /// 85, so Pet::InitStatsForLevel takes its real branch instead of the "'Weakifying' pet
        /// and giving it mana to make it obvious" fallback (Pet.cpp:760) that an entry with no
        /// rows would land in; creature_template.MechanicImmuneMask is 0, so no immunity stands
        /// between the fear and the claim under test; UnitFlags is 0 and MovementType is 0. Read
        /// from mangos3.creature_template and mangos3.pet_levelstats on 2026-09-21.
        ///
        /// Its Expansion column is -1 -- as it is for every entry `pet_levelstats` covers on this
        /// database -- so InitStatsForLevel logs one "SUMMON_PET creature_template not finished
        /// (expansion field = -1)" errorDb line per run and falls back to the template's own melee
        /// damage. That is a data gap far older than this scenario, it touches nothing any
        /// category below reads, and it is named here so the next reader of the log does not go
        /// hunting for a fault in the harness.
        const uint32 IMP = 416;

        /// THE CLAIMS ARE TAKEN THROUGH Unit::SetFeared AND Unit::SetConfused, not by casting
        /// 5782 and 118 at the pet, AND THAT IS A MEASUREMENT, not a convenience.
        ///
        /// Both spells draw a hit roll against the pet. 5782's Spell.dbc row points at
        /// SpellCategories row 764, whose DefenseType is 1 -- SPELL_DAMAGE_CLASS_MAGIC -- with
        /// mechanic 5 (MECHANIC_FEAR); 118's points at row 53, DefenseType 1, mechanic 17. (Read
        /// out of server-release/dbc/Spell.dbc column 35 and SpellCategories.dbc on 2026-09-21,
        /// the same way player-stun read 76216's absent row and 5211's DefenseType 2.) A class
        /// other than NONE sends Unit::SpellHitResult to MagicSpellHitResult
        /// (UnitCombat.cpp:969-977), which rolls; the harness's seed is fixed, so a bad roll would
        /// fail this scenario on EVERY run rather than occasionally -- and a pet is exactly the
        /// shape that gets the roll, since it is neither the caster nor immune. Both mechanics are
        /// also diminishing groups, and diminishing returns apply to a player's pet.
        ///
        /// The entry point costs nothing this scenario needs and buys something it wants. It is
        /// the call Aura::HandleModFear and Aura::HandleModConfuse make one line below the aura
        /// (S55, S58 and S65 -- the scenario that carries the asterisk this one removes -- all
        /// take it), the claim it stamps is identical, and with `time` 0 the claim carries NO
        /// clock: there is no aura to expire and no timer to run out, so nothing in the world can
        /// end it except the take. "The claim was gone the instant the take returned" therefore
        /// has exactly one explanation available to it.
        ///
        /// WHAT IS LOST BY IT, said plainly: the aura's own removal path is not exercised here,
        /// and this scenario does not claim it is. player-fear and player-confuse cast the real
        /// spells at a player and own that half.

        /// player-owned-pet-possession's moments, absolute offsets from the scenario's start as
        /// Scenario::At takes them.
        ///
        /// TWO TIME BASES WOULD BE ONE TOO MANY, so this scenario's log lines carry the ABSOLUTE
        /// timeline moment and each one names its phase. Every player scenario before it had a
        /// single event every reading was about and counted "+Nms" from it; this one has two takes
        /// with a release between them, and a "+Nms" that silently changed anchor half way down
        /// the log would be worse than no anchor at all.
        ///
        /// The fear is given 1.1 s before the first take: a flee bolt is 11-36 yd at 7.5 yd/s, so
        /// the pet is unmistakably running by then and still well inside the 90 yd at which
        /// Unit::ResetControlState would dismiss an out-of-range pet instead of handing it back.
        /// The release is 1.3 s after the take, the confuse 500 ms after the release (the follow
        /// the release lays has settled), and the second take 900 ms after the confuse.
        const uint32 kOwnPetFearAt    = 500;
        const uint32 kOwnPetTakeAt    = 1600;
        const uint32 kOwnPetReleaseAt = 2900;
        const uint32 kOwnPetConfuseAt = 3400;
        const uint32 kOwnPetTake2At   = 4300;
        const uint32 kOwnPetCleanupAt = 5700;   ///< the possession handed back and the pet gone, before the runner's teardown
        const uint32 kOwnPetVerdictAt = 6000;

        /// How far the flee must have carried the pet before fleeStopsAtTake will call it running.
        /// Five yards, from the same arithmetic S60 uses for its ten: the shortest bolt this
        /// geometry draws is 8 yd and the pet runs it at 7.5 yd/s, so 1.1 s of fleeing covers
        /// about 8 yd. Five leaves room for the first tick's latency and for the ground under the
        /// goal, and is still ten times anything a standing unit can produce -- nothing writes a
        /// standing creature's position, so a pet that never fled reads 0.0 exactly.
        const float kOwnPetFleeYd = 5.0f;

        /// And how far it may drift after the take before "stopped" stops meaning it. Half a yard,
        /// the same figure the flee family uses to tell a stop spline from a bolt (kStopSplineYd):
        /// a feared unit runs 8.75 yd/s (7.0 base x the fear's 1.25), so ONE sample of a surviving
        /// bolt covers 0.88 yd and already clears this.
        const float kOwnPetStillYd = 0.5f;

        /// ...measured from the fifth sample after the take, not from the take itself, and the
        /// gap is the harness reading the world between its beats rather than anything moving.
        /// A scenario step runs after the map's update, so the position it reads is the last one
        /// Unit::UpdateSplineMovement relocated; Unit::StopMoving ends the spline on the
        /// INTERPOLATED point (MoveSplineInit::Stop computes it), so the take's own stop lands as
        /// a relocation on a later update and reads as a jump of a yard or two that no behaviour
        /// authored. Anchoring on the take instant would count that jump as movement; anchoring
        /// half a second later cannot miss a bolt, which would have carried the pet four yards
        /// further by then. What the stop carried is measured anyway and printed in the verdict,
        /// so it is bounded rather than waved away.
        const uint32 kOwnPetSettleSamples = 5;
    }

    /// S73 (order 907, the harness's eighth player and its first pet): the ownPet branch of
    /// Unit::TakePossessOf (Unit.cpp:7155-7176) -- the one player-only path in the movement kernel
    /// that no scenario has ever entered, and the asterisk on S65's own header.
    ///
    /// The branch is gated on three things at once: the charmer is a PLAYER, the possessed body
    /// IsPet(), and its guid is the charmer's own GetPetGuid(). A creature possessing a creature
    /// (S34) fails the first, and the seven player scenarios of the last two days possessed a
    /// plain wolf, which fails the other two. Inside the gate the take does two things nothing has
    /// observed: BEFORE the grant it ends the body's control episodes --
    /// `CancelControl(Motion::Kind::Fear)` and `CancelControl(Motion::Kind::Confused)`, commented
    /// "the grant below needs the flee and confuse states gone" -- and AFTER the grant it stops the
    /// body, clears the stack, installs the idle and returns, skipping the whole non-pet tail.
    /// S65 could reach neither: "the harness cannot build a player-owned pet, so the scenario makes
    /// the identical facade call on the identical arbiter instead" (peer/baselines/README-58a0ffc6c.md).
    /// It can now. The pet is a real Pet object built in memory the way Spell::DoSummonPet builds a
    /// fresh warlock minion, minus the single line of that function the harness may never run.
    ///
    /// THE CANCEL IS NOT OVER-DETERMINED, and that was settled by reading the arbiter before a line
    /// of this was written:
    ///   - Arbiter::Inhibit (Arbiter.cpp:433) moves the mobility word and reconciles the block. It
    ///     finishes NO entry, so the Possessed inhibition the take raises twenty lines earlier
    ///     cannot be what ended the claim.
    ///   - Arbiter::Clear(false) (Arbiter.cpp:595) takes the command layers and the combat entry,
    ///     and takes the CLAIMS only `if (all)` -- which the take's own `Clear(false)` is not. So
    ///     the tail's clear cannot be what ended it either.
    /// Delete the two CancelControl lines and nothing else, and fearEndsAtTake and
    /// confuseEndsAtTake go BUG while every other category in this file stands.
    ///
    /// AND THE FLEE WOULD REALLY GO ON RUNNING. Motion::Decide (Mobility.cpp:84) blocks a
    /// possessed body's behaviours only `if ((reasons & ReasonPossessed) && selected !=
    /// Selected::Control)`: a selected fear or confuse plays THROUGH a possession, by design and
    /// by the retail reference (15.4.1) -- the possessor is the one locked out. That is the whole
    /// reason the branch exists, and it is why fleeStopsAtTake is a finding and not a tautology:
    /// without the cancel the bolt keeps playing under the grant and the owner's client is handed
    /// a body running somewhere on its own.
    ///
    /// WHAT THIS SCENARIO DOES NOT CLAIM. Not which of the tail's three calls stops the body:
    /// StopMoving, Clear(false) and MoveIdle all bear on it and the scenario measures the state
    /// the tail leaves, not the authorship of it. Not that the stagger was moving before the
    /// second take: a confuse lurches inside Movement.ConfuseRadius (2 yd) and rests 800-1500 ms
    /// between lurches, so a 900 ms window sees movement or does not by the draw, and only the
    /// FEAR phase asserts that something was running. Not the aura removal path (see the entry
    /// point's note above).
    class PlayerOwnedPetPossession : public Scenario
    {
    public:
        /// Order 907, the next in the reserved player block behind the mover-authority trio's
        /// 904-906: the runner refuses `MVTEST all` when a player scenario is queued before one
        /// that holds no player (Harness.cpp Start), so player scenarios take high contiguous
        /// orders of their own and the creature families go on growing from 64 without ever
        /// colliding with them.
        PlayerOwnedPetPossession() : Scenario("player-owned-pet-possession", 907) {}

        /// The runner owes a player scenario two things: the last place in the queue and the
        /// map's grids reset behind it. A player in world promotes the grids around him to full
        /// state and changes Map::Update's own visitation order, and no scenario that holds none
        /// may read that.
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            /// One take's worth of readings. The two phases are the same shape -- a control
            /// episode running, a take, a window after it -- so they are the same struct twice
            /// rather than two flat sets of fields whose names would have to be told apart by a
            /// suffix.
            struct Half
            {
                uint32 heldSamples;   ///< samples before the take with the claim held
                bool   kindSeen;      ///< ActiveKind() read the control's own kind on one of them
                bool   takeRan;       ///< the take step resolved both actors and ran
                bool   claimBefore;   ///< the claim was held the instant before the take
                bool   auraBefore;    ///< the published auraFear with it (the fear half only)
                bool   gateIsPet;     ///< Creature::IsPet() the instant before the take
                bool   gateOwner;     ///< ...and its owner guid was the player
                bool   gatePetGuid;   ///< ...and the player's own GetPetGuid() was this pet
                bool   took;          ///< TakePossessOf returned true
                bool   splineBefore;  ///< a spline was PLAYING the instant before the take
                bool   claimAfter;    ///< the claim the instant the take returned: must be false
                Motion::Kind kindAfter;   ///< ...and the selected kind then: must be Idle
                bool   splineAfter;   ///< ...and the spline was finalized: nothing left playing
                bool   charm;         ///< the player was its charmer the instant the take returned
                bool   mover;         ///< ...and his session was under it
                bool   possessed;     ///< ...and the Possessed inhibition was up
                bool   alive;         ///< ...and the pet was alive
                bool   stillPet;      ///< ...and still a Pet, still his
                float  x, y;          ///< where it stood the instant the take returned
                bool   anchored;      ///< the settled anchor below has been taken
                float  ax, ay;        ///< where it stood kOwnPetSettleSamples later: the stillness anchor
                float  settle;        ///< how far the take's own stop carried it to get there
                float  drift;         ///< the farthest it got from the anchor afterwards
                uint32 after;         ///< samples after the take, before the phase ended
                uint32 back;          ///< ...on which the claim was held again
                uint32 splineRunning; ///< ...on which a spline was playing again
                uint32 lost;          ///< ...on which the possession was no longer whole
                uint32 gone;          ///< ...on which it was no longer his live pet
            };
            struct St
            {
                bool   built;         ///< the pet exists, in world, and the player owns it
                float  fearX, fearY;  ///< where it stood when the fear was taken
                float  fled;          ///< the farthest it got from there before the first take
                bool   released;      ///< the release step ran between the two phases
                Half   fear;
                Half   confuse;
            };

            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            // The fright. Eight yards east of the player and four east of the pet, so the flee
            // runs WEST, past its owner rather than away from him: a pet that bolts out of the
            // 90 yd visibility band is one Unit::ResetControlState dismisses (Unit.cpp:7287)
            // instead of handing back, and the release below is not the thing under test.
            Creature* k = p ? Spawn(KOBOLD, SE.x + 8.0f, SE.y, Ground(SE.x + 8.0f, SE.y, SE.z), 3.1f) : NULL;
            Pet* pet = (p && k) ? BuildPet(p) : NULL;
            if (!p || !k || !pet)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            Silence(k);
            const ObjectGuid g = p->GetObjectGuid(), gk = k->GetObjectGuid(), gp = pet->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->built = true;
            st->fearX = st->fearY = st->fled = 0.0f;
            st->released = false;
            Zero(st->fear);
            Zero(st->confuse);
            Log("the player %s stands at (%.1f, %.1f); his own pet (entry %u, %s) 4 yd east, the kobold 8 yd east; IsPet=%d his pet guid=%d",
                g.GetString().c_str(), p->Where().X(), p->Where().Y(), IMP, gp.GetString().c_str(),
                pet->IsPet() ? 1 : 0, p->GetPetGuid() == gp ? 1 : 0);

            // ---- phase 1: the fear -------------------------------------------------------
            At(kOwnPetFearAt, [this, gk, gp, st]()
            {
                Pet* pet = FindPet(gp); if (!pet) { return; }
                st->fearX = pet->Where().X();
                st->fearY = pet->Where().Y();
                pet->SetFeared(true, gk, FEAR, 0, 0);
                Log("%4ums the pet is feared by the kobold: claim=%d auraFear=%d mt=%s",
                    kOwnPetFearAt, pet->GetMotionMaster()->HoldsControl(Motion::Kind::Fear) ? 1 : 0,
                    pet->IsFearedByAura() ? 1 : 0, Motion::KindName(pet->GetMotionMaster()->ActiveKind()));
            });
            // Registered before its sampler so it runs first at its own moment (the timeline
            // orders a tie by insertion): the sample at kOwnPetTakeAt is then already an
            // after-sample, taken against the anchor the take itself wrote.
            At(kOwnPetTakeAt, [this, g, gp, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Pet* pet = FindPet(gp);
                if (!p || !pet) { return; }
                Before(*p, *pet, st->fear, true);
                // The FARTHEST it got, as the sampler tracks it, not the reading at this instant:
                // a bolt that has turned back would otherwise report less flee than it ran.
                const float d = Dist2(st->fearX, st->fearY, pet->Where().X(), pet->Where().Y());
                if (d > st->fled) { st->fled = d; }
                st->fear.took = p->TakePossessOf(pet);
                After(*p, *pet, st->fear, true);
                Log("%4ums THE FIRST TAKE (his own pet, feared): gate isPet=%d owner=%d petGuid=%d | fear claim %d -> %d, auraFear %d -> %d, mt %s, ran %.1f yd | took=%d charmer=%d mover=%d possessed=%d alive=%d",
                    kOwnPetTakeAt, st->fear.gateIsPet ? 1 : 0, st->fear.gateOwner ? 1 : 0, st->fear.gatePetGuid ? 1 : 0,
                    st->fear.claimBefore ? 1 : 0, st->fear.claimAfter ? 1 : 0,
                    st->fear.auraBefore ? 1 : 0, pet->IsFearedByAura() ? 1 : 0,
                    Motion::KindName(st->fear.kindAfter), st->fled,
                    st->fear.took ? 1 : 0, st->fear.charm ? 1 : 0, st->fear.mover ? 1 : 0,
                    st->fear.possessed ? 1 : 0, st->fear.alive ? 1 : 0);
            });
            for (uint32 t = kOwnPetFearAt + 100; t < kOwnPetReleaseAt; t += 100)
            {
                At(t, [this, g, gp, st, t]()
                {
                    Player* p = sPlayerRegistry.Find(g);
                    Pet* pet = FindPet(gp);
                    Sample(p, pet, st->fear, Motion::Kind::Fear, st->fearX, st->fearY, &st->fled);
                    // EVERY sample of the after-window is logged, not one in five: it is the
                    // window fleeStopsAtTake turns on, and the settling relocation the take's own
                    // stop leaves behind is a thing a reader should be able to watch land rather
                    // than take on the verdict's word.
                    if (st->fear.takeRan || t % 500 == 0)
                    {
                        Log("%4ums fear phase: claim=%d mt=%s spline=%s charmer=%d mover=%d possessed=%d at (%.2f, %.2f) ran %.1f yd drift %.2f yd",
                            t, pet && pet->GetMotionMaster()->HoldsControl(Motion::Kind::Fear) ? 1 : 0,
                            pet ? Motion::KindName(pet->GetMotionMaster()->ActiveKind()) : "gone",
                            pet ? (pet->movespline->Finalized() ? "done" : "PLAYING") : "-",
                            pet && p && pet->GetCharmerGuid() == p->GetObjectGuid() ? 1 : 0,
                            pet && p && pet->MoverSession() == p->GetSession() ? 1 : 0,
                            pet && pet->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0,
                            pet ? pet->Where().X() : 0.0f, pet ? pet->Where().Y() : 0.0f,
                            st->fled, st->fear.drift);
                    }
                });
            }
            At(kOwnPetReleaseAt, [this, g, gp, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Pet* pet = FindPet(gp);
                if (!p || !pet) { return; }
                // Unit::ResetControlState(false), the call the possess aura's own removal makes;
                // false so the body is not turned on its former charmer. Its own pet branch
                // (Unit.cpp:7284-7295) is a path no scenario had entered either -- it lays the
                // follow back on -- and the second phase needs the possession handed back, since
                // a player has exactly one GetPetGuid() and therefore one own pet to take.
                p->ResetControlState(false);
                st->released = true;
                Log("%4ums the possession handed back: charmer=%s his pet guid still this pet=%d mt=%s",
                    kOwnPetReleaseAt, pet->GetCharmerGuid().GetString().c_str(),
                    p->GetPetGuid() == pet->GetObjectGuid() ? 1 : 0,
                    Motion::KindName(pet->GetMotionMaster()->ActiveKind()));
            });

            // ---- phase 2: the confuse ----------------------------------------------------
            At(kOwnPetConfuseAt, [this, gk, gp]()
            {
                Pet* pet = FindPet(gp); if (!pet) { return; }
                pet->SetConfused(true, gk, POLYMORPH, 0);
                Log("%4ums the pet is confused: claim=%d mt=%s", kOwnPetConfuseAt,
                    pet->GetMotionMaster()->HoldsControl(Motion::Kind::Confused) ? 1 : 0,
                    Motion::KindName(pet->GetMotionMaster()->ActiveKind()));
            });
            At(kOwnPetTake2At, [this, g, gp, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Pet* pet = FindPet(gp);
                if (!p || !pet) { return; }
                Before(*p, *pet, st->confuse, false);
                st->confuse.took = p->TakePossessOf(pet);
                After(*p, *pet, st->confuse, false);
                Log("%4ums THE SECOND TAKE (his own pet, confused): gate isPet=%d owner=%d petGuid=%d | confuse claim %d -> %d, mt %s | took=%d charmer=%d mover=%d possessed=%d alive=%d",
                    kOwnPetTake2At, st->confuse.gateIsPet ? 1 : 0, st->confuse.gateOwner ? 1 : 0,
                    st->confuse.gatePetGuid ? 1 : 0, st->confuse.claimBefore ? 1 : 0,
                    st->confuse.claimAfter ? 1 : 0, Motion::KindName(st->confuse.kindAfter),
                    st->confuse.took ? 1 : 0, st->confuse.charm ? 1 : 0, st->confuse.mover ? 1 : 0,
                    st->confuse.possessed ? 1 : 0, st->confuse.alive ? 1 : 0);
            });
            for (uint32 t = kOwnPetConfuseAt + 100; t < kOwnPetCleanupAt; t += 100)
            {
                At(t, [this, g, gp, st, t]()
                {
                    Player* p = sPlayerRegistry.Find(g);
                    Pet* pet = FindPet(gp);
                    Sample(p, pet, st->confuse, Motion::Kind::Confused, 0.0f, 0.0f, NULL);
                    if (t % 500 == 0)
                    {
                        Log("%4ums confuse phase: claim=%d mt=%s charmer=%d mover=%d possessed=%d drift %.2f yd",
                            t, pet && pet->GetMotionMaster()->HoldsControl(Motion::Kind::Confused) ? 1 : 0,
                            pet ? Motion::KindName(pet->GetMotionMaster()->ActiveKind()) : "gone",
                            pet && p && pet->GetCharmerGuid() == p->GetObjectGuid() ? 1 : 0,
                            pet && p && pet->MoverSession() == p->GetSession() ? 1 : 0,
                            pet && pet->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed) ? 1 : 0,
                            st->confuse.drift);
                    }
                });
            }

            At(kOwnPetCleanupAt, [this, g, gp]()
            {
                EndPet(sPlayerRegistry.Find(g), FindPet(gp));
            });
            At(kOwnPetVerdictAt, [this, st]()
            {
                Verdict(Read(*st));
            });
        }

    private:
        // ---- the pet ---------------------------------------------------------------------

        /// Map::GetCreature answers only the HIGHGUID_UNIT store (Map.cpp:3063), so
        /// Scenario::Get cannot resolve a pet; HIGHGUID_PET lives in its own one, which
        /// Pet::AddToWorld inserts into. Every step re-resolves through here, as every other
        /// scenario re-resolves its actors by guid.
        Pet* FindPet(ObjectGuid guid) const
        {
            Map* map = GetMap();
            return map ? map->GetPet(guid) : NULL;
        }

        /**
         * A player-owned pet, in memory, with no row in `character_pet` and no write to the
         * character database -- the machinery S65 said the harness did not have.
         *
         * IT IS Spell::DoSummonPet's OWN RECIPE (SpellEffectSummonLock.cpp:937-1011), in its
         * order, MINUS exactly one line. That function's player branch ends
         * `spawnCreature->SavePetToDB(PET_SAVE_AS_CURRENT)`; everything before it is pure memory,
         * and this builder stops there. The pet it makes is therefore a SUMMON_PET -- the type
         * DoSummonPet gives a fresh minion when no stored row answers -- and not a faked creature
         * dressed up to satisfy the gate: Pet::IsPet() is the Creature subtype its constructor
         * passes, GetPetGuid() is set by Unit::SetPet, and Pet::Update runs its whole controlled
         * -pet loop over it every tick.
         *
         * THE ORDER IS LOAD-BEARING IN THREE PLACES:
         *   1. SetOwnerGuid comes before AIM_Initialize, because MotionMaster::Initialize reads
         *      it: a creature whose owner guid is a player takes the IDLE factory default
         *      whatever its template's MovementType says ("A player's pet has no factory
         *      default", MotionMaster.cpp:735). Set it after, and the pet would carry whatever
         *      default its entry names and "the fear moved it" would have a second explanation.
         *   2. SetOwnerGuid also comes before InitStatsForLevel, which resolves GetOwner() for
         *      the owner's class bonus (Pet.cpp:700-717) and logs an error without one.
         *   3. SetActiveObjectState comes before Map::Add, as Scenario::Spawn does it: the map
         *      ticks the cells around players plus the active list, and the registration point
         *      for a new object is the add.
         *
         * WHAT OWNS IT: this scenario, start to finish. Nothing else in the server made it and
         * nothing else will free it -- the runner's End() sweep despawns TemporarySummons from
         * Spawned() and hands Find'd creatures back, and a Pet is neither -- so EndPet below runs
         * as the scenario's last act but one, before the verdict and well before the teardown.
         */
        Pet* BuildPet(Player* owner)
        {
            Map* map = GetMap();
            CreatureInfo const* cinfo = ObjectMgr::GetCreatureTemplate(IMP);
            if (!map || !owner || !cinfo)
            {
                Log("ERR pet: no map, no owner, or no creature template %u", IMP);
                return NULL;
            }
            const float x = owner->Where().X() + 4.0f;
            const float y = owner->Where().Y();
            Load(x, y);
            Pet* pet = new Pet(SUMMON_PET);
            CreatureCreatePos pos(map, x, y, Ground(x, y, owner->Where().Z()), 0.0f, 1);
            const uint32 petNumber = sObjectMgr.GeneratePetNumber();
            if (!pet->Create(map->GenerateLocalLowGuid(HIGHGUID_PET), pos, cinfo, petNumber))
            {
                delete pet;
                Log("ERR pet: Pet::Create failed for entry %u", IMP);
                return NULL;
            }
            pet->SetSpawn(pos);
            pet->SetOwnerGuid(owner->GetObjectGuid());     // (1) and (2) above
            pet->SetCreatorGuid(owner->GetObjectGuid());
            pet->setFaction(owner->getFaction());          // so the kobold is its enemy, as the owner's
            pet->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, 0);
            pet->InitStatsForLevel(owner->getLevel());
            pet->GetCharmInfo()->SetPetNumber(petNumber, pet->isControlled());
            pet->GetCharmInfo()->SetReactState(REACT_DEFENSIVE);
            pet->InitPetCreateSpells();                    // memory only: the action bar, the family passives, the owner's pet auras
            pet->SetActiveObjectState(true);               // (3) above
            map->Add((Creature*)pet);
            pet->AIM_Initialize();
            // The factory AI dropped from under a recording decorator, as Scenario::Silence does
            // it for a spawned actor -- which cannot be used here, because Silence refuses
            // anything Spawn did not hand out and a Pet is not a TemporarySummon. It is not
            // optional: PetAI::UpdateAI draws from urand for its autocast pick (PetAI.cpp:399),
            // re-lays a follow on its owner and selects hostile targets, so a live one would both
            // perturb the seeded stream every other scenario shares and fight the claims under
            // test for the wheel.
            pet->SetAI(new HarnessAI(pet, pet->AI(), this));
            if (HarnessAI* recording = dynamic_cast<HarnessAI*>(pet->AI()))
            {
                delete recording->Release();
            }
            owner->SetPet(pet);                            // UNIT_FIELD_SUMMON: the gate's third conjunct
            return pet;
        }

        /**
         * The pet's end, and THE ONE PLACE THIS SCENARIO COULD HAVE WRITTEN TO THE CHARACTER
         * DATABASE. Pet::Unsummon ends in `SavePetToDB(mode)` unconditionally (Pet.cpp:390), and
         * a SUMMON_PET passes that function's first two gates -- it has an entry and
         * isControlled() is true -- so a pet with a live player owner would have a row INSERTed
         * for a character that does not exist.
         *
         * The owner guid is therefore cleared FIRST, before anything that can route into
         * Unsummon. That closes SavePetToDB's third gate (`GetOwnerGuid().IsPlayer()`,
         * PetDatabase.cpp:407) for every path out of here at once, including the two that are not
         * obvious: Unit::ResetControlState dismisses an out-of-range pet with
         * RemovePet(PET_SAVE_REAGENTS) (Unit.cpp:7289), and Pet::Update unsummons a pet whose
         * owner it cannot resolve. Both then find their save refused instead of relying on the
         * pet having stayed close enough, or on this step having run at all.
         *
         * Then the charm, then the pet guid, then the removal. Ordered that way because
         * ResetControlState reads GetPetGuid() to choose its own pet branch, and clearing the
         * guid first would send it down the wrong one.
         */
        void EndPet(Player* p, Pet* pet)
        {
            if (!pet)
            {
                Log("ERR cleanup: the pet was already gone");
                return;
            }
            pet->SetOwnerGuid(ObjectGuid());
            if (p)
            {
                if (p->GetCharmGuid() == pet->GetObjectGuid())
                {
                    p->ResetControlState(false);
                }
                if (p->GetPetGuid() == pet->GetObjectGuid())
                {
                    p->SetPet(NULL);
                }
            }
            pet->Unsummon(PET_SAVE_NOT_IN_SLOT);
            Log("%4ums the pet released and unsummoned: charmer=%d his pet guid=%d",
                kOwnPetCleanupAt, p && p->GetCharmGuid() ? 1 : 0, p && p->GetPetGuid() ? 1 : 0);
        }

        // ---- the readings ----------------------------------------------------------------

        /// The fields are named rather than value-initialised so that adding one to Half without
        /// adding it here is a compile error rather than a field that silently reads zero.
        template <class H>
        static void Zero(H& h)
        {
            h.heldSamples = h.after = h.back = h.splineRunning = h.lost = h.gone = 0;
            h.kindSeen = h.takeRan = h.claimBefore = h.auraBefore = false;
            h.gateIsPet = h.gateOwner = h.gatePetGuid = false;
            h.took = h.splineBefore = h.claimAfter = h.splineAfter = false;
            h.kindAfter = Motion::Kind::Idle;
            h.charm = h.mover = h.possessed = h.alive = h.stillPet = false;
            h.anchored = false;
            h.x = h.y = h.ax = h.ay = h.settle = h.drift = 0.0f;
        }

        /// Read IMMEDIATELY before a take and used by every category: the conjunct that makes
        /// each of them falsifiable. A claim that was not held going in cannot be ended by the
        /// take, and a body that did not satisfy the gate never entered the branch at all.
        template <class H>
        void Before(Player& p, Pet& pet, H& h, bool isFear) const
        {
            h.takeRan     = true;
            h.claimBefore = pet.GetMotionMaster()->HoldsControl(isFear ? Motion::Kind::Fear : Motion::Kind::Confused);
            h.auraBefore  = pet.IsFearedByAura();
            // The kernel's own answer to "is a leg running", and the reading the stillness
            // categories turn on: a fear whose claim survived the take would still be playing one.
            h.splineBefore = !pet.movespline->Finalized();
            h.gateIsPet   = pet.IsPet();
            h.gateOwner   = pet.GetOwnerGuid() == p.GetObjectGuid();
            h.gatePetGuid = p.GetPetGuid() == pet.GetObjectGuid();
        }

        /// And IMMEDIATELY after it returns, in one breath with the call: the claim, the selected
        /// kind, the possession the take built, and the pet itself.
        template <class H>
        void After(Player& p, Pet& pet, H& h, bool isFear) const
        {
            h.claimAfter = pet.GetMotionMaster()->HoldsControl(isFear ? Motion::Kind::Fear : Motion::Kind::Confused);
            h.kindAfter  = pet.GetMotionMaster()->ActiveKind();
            h.splineAfter = pet.movespline->Finalized();
            h.charm      = pet.GetCharmerGuid() == p.GetObjectGuid();
            h.mover      = pet.MoverSession() == p.GetSession();
            h.possessed  = pet.GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed);
            h.alive      = pet.IsAlive();
            h.stillPet   = pet.IsPet() && p.GetPetGuid() == pet.GetObjectGuid();
            h.x          = pet.Where().X();
            h.y          = pet.Where().Y();
        }

        /// One 100 ms sample of a phase. Before the take it counts the episode; after it, the
        /// window the take anchors -- and the window is anchored to the TAKE, a deliberate end at
        /// a moment this scenario chose, so a claim held again inside it is not a flicker to
        /// re-anchor past but a failure in its own right, counted and reported as one.
        /// `fled` is NULL for a phase that measures no travel of its own -- the confuse's, whose
        /// stagger stays inside Movement.ConfuseRadius and rests between lurches, so a window
        /// this short would be measuring the draw rather than the behaviour.
        template <class H>
        void Sample(Player* p, Pet* pet, H& h, Motion::Kind kind, float x0, float y0, float* fled) const
        {
            if (!pet)
            {
                if (h.takeRan) { ++h.after; ++h.gone; }
                return;
            }
            const bool held = pet->GetMotionMaster()->HoldsControl(kind);
            if (!h.takeRan)
            {
                if (held)
                {
                    ++h.heldSamples;
                    if (pet->GetMotionMaster()->ActiveKind() == kind) { h.kindSeen = true; }
                }
                if (fled)
                {
                    const float d = Dist2(x0, y0, pet->Where().X(), pet->Where().Y());
                    if (d > *fled) { *fled = d; }
                }
                return;
            }
            const uint32 n = h.after;   // this sample's index in the after-window, from 0
            ++h.after;
            if (held) { ++h.back; }
            if (!pet->movespline->Finalized()) { ++h.splineRunning; }
            if (!p || pet->GetCharmerGuid() != p->GetObjectGuid() ||
                pet->MoverSession() != p->GetSession() ||
                !pet->GetMotionMaster()->Inhibited(Motion::Inhibition::Possessed))
            {
                ++h.lost;
            }
            if (!pet->IsAlive() || !pet->IsPet() || !p || p->GetPetGuid() != pet->GetObjectGuid())
            {
                ++h.gone;
            }
            if (n == kOwnPetSettleSamples)
            {
                h.ax = pet->Where().X();
                h.ay = pet->Where().Y();
                h.settle = Dist2(h.x, h.y, h.ax, h.ay);
                h.anchored = true;
            }
            else if (h.anchored)
            {
                const float d = Dist2(h.ax, h.ay, pet->Where().X(), pet->Where().Y());
                if (d > h.drift) { h.drift = d; }
            }
        }

        template <class S>
        std::string Read(S& st) const
        {
            char gate[416], fear[448], flee[416], conf[448], poss[416], survive[416];
            // --- ownPetGateHeld: the branch's own precondition, and the reason this scenario
            // exists at all. Everything below is about what the take did INSIDE the gate, so a
            // gate that did not hold makes the rest a report about the ordinary tail.
            if (!st.built || !st.fear.takeRan || !st.confuse.takeRan)
            {
                snprintf(gate, sizeof(gate), "INVALID(the pet was not built, or a take step never ran: built=%d first take=%d second take=%d)",
                         st.built ? 1 : 0, st.fear.takeRan ? 1 : 0, st.confuse.takeRan ? 1 : 0);
            }
            else if (!st.fear.gateIsPet || !st.confuse.gateIsPet)
            {
                snprintf(gate, sizeof(gate), "BUG(Creature::IsPet() read false before a take: first=%d second=%d -- the body was not a Pet and Unit.cpp:7157 could not have been entered)",
                         st.fear.gateIsPet ? 1 : 0, st.confuse.gateIsPet ? 1 : 0);
            }
            else if (!st.fear.gateOwner || !st.confuse.gateOwner || !st.fear.gatePetGuid || !st.confuse.gatePetGuid)
            {
                snprintf(gate, sizeof(gate), "BUG(it was not HIS pet before a take: owner guid first=%d second=%d, his GetPetGuid() first=%d second=%d)",
                         st.fear.gateOwner ? 1 : 0, st.confuse.gateOwner ? 1 : 0,
                         st.fear.gatePetGuid ? 1 : 0, st.confuse.gatePetGuid ? 1 : 0);
            }
            else if (!st.fear.took || !st.confuse.took)
            {
                snprintf(gate, sizeof(gate), "BUG(TakePossessOf refused a take it had every precondition for: first=%d second=%d)",
                         st.fear.took ? 1 : 0, st.confuse.took ? 1 : 0);
            }
            else
            {
                snprintf(gate, sizeof(gate), "OK(all three conjuncts of Unit.cpp:7157 held at both takes: a player charmer, Creature::IsPet(), and a body whose guid was his own GetPetGuid())");
            }
            // --- fearEndsAtTake and confuseEndsAtTake: the finding, twice.
            Episode(st.fear, "fear", "the 5782 claim", st.built, fear, sizeof(fear));
            Episode(st.confuse, "confuse", "the 118 claim", st.built && st.released, conf, sizeof(conf));
            // --- fleeStopsAtTake: what the branch's tail leaves behind. Its "was running" half
            // is what keeps it from passing over a pet that never went anywhere.
            if (!st.built || !st.fear.takeRan)
            {
                snprintf(flee, sizeof(flee), "INVALID(the pet was not built, or the first take step never ran)");
            }
            else if (!st.fear.took || !st.fear.claimBefore)
            {
                snprintf(flee, sizeof(flee), "INVALID(no fear was running into the take: claim held=%d, take returned %d)",
                         st.fear.claimBefore ? 1 : 0, st.fear.took ? 1 : 0);
            }
            else if (st.fled < kOwnPetFleeYd || !st.fear.splineBefore)
            {
                snprintf(flee, sizeof(flee), "INVALID(no flee leg was playing into the take: it had covered %.1f yd of the %.1f yd this reads as running and its spline was %s, so there was nothing for the stop to end)",
                         st.fled, kOwnPetFleeYd, st.fear.splineBefore ? "playing" : "already finalized (the flee was resting between bolts)");
            }
            else if (st.fear.kindAfter != Motion::Kind::Idle)
            {
                snprintf(flee, sizeof(flee), "BUG(the selected behaviour the instant the take returned was %s, not Idle: the tail's Clear(false) and MoveIdle did not leave the body idle after it had run %.1f yd)",
                         Motion::KindName(st.fear.kindAfter), st.fled);
            }
            else if (!st.fear.splineAfter)
            {
                snprintf(flee, sizeof(flee), "BUG(a spline was still playing the instant the take returned, after %.1f yd of flee: the tail's StopMoving did not end the leg and the bolt runs on under the grant)",
                         st.fled);
            }
            else if (!st.fear.anchored || st.fear.after < kOwnPetSettleSamples + 5)
            {
                snprintf(flee, sizeof(flee), "INVALID(only %u samples after the first take, too few to leave %u for the stop to land and five to read)",
                         st.fear.after, kOwnPetSettleSamples);
            }
            else if (st.fear.splineRunning)
            {
                snprintf(flee, sizeof(flee), "BUG(a spline was playing again on %u of the %u samples after the take)", st.fear.splineRunning, st.fear.after);
            }
            else if (st.fear.drift > kOwnPetStillYd)
            {
                snprintf(flee, sizeof(flee), "BUG(it moved %.2f yd over the %u samples from the settled anchor, past the %.2f yd a stopped body allows, with no spline playing on any of them)",
                         st.fear.drift, st.fear.after - kOwnPetSettleSamples, kOwnPetStillYd);
            }
            else
            {
                snprintf(flee, sizeof(flee), "OK(a leg playing and %.1f yd run from where it was feared; the take returned with the selected behaviour Idle and the spline finalized, the stop carried it a further %.2f yd, and it then moved %.2f yd over the remaining %u samples with no spline playing on any of the %u)",
                         st.fled, st.fear.settle, st.fear.drift, st.fear.after - kOwnPetSettleSamples, st.fear.after);
            }
            // --- possessionSurvivesTake: the first refusal of a hollow pass. A take that ended
            // the possession would end the claims with it and every finding above would be true
            // for the wrong reason.
            const uint32 after = st.fear.after + st.confuse.after;
            const uint32 lost = st.fear.lost + st.confuse.lost;
            if (!st.built || !st.fear.takeRan || !st.confuse.takeRan)
            {
                snprintf(poss, sizeof(poss), "INVALID(the pet was not built, or a take step never ran)");
            }
            else if (!st.fear.charm || !st.confuse.charm || !st.fear.mover || !st.confuse.mover ||
                     !st.fear.possessed || !st.confuse.possessed)
            {
                snprintf(poss, sizeof(poss), "BUG(a take returned without the possession it is supposed to build: charmer first=%d second=%d, his session under it first=%d second=%d, Possessed inhibition first=%d second=%d)",
                         st.fear.charm ? 1 : 0, st.confuse.charm ? 1 : 0,
                         st.fear.mover ? 1 : 0, st.confuse.mover ? 1 : 0,
                         st.fear.possessed ? 1 : 0, st.confuse.possessed ? 1 : 0);
            }
            else if (after < 10)
            {
                snprintf(poss, sizeof(poss), "INVALID(only %u samples after the two takes together)", after);
            }
            else if (lost)
            {
                snprintf(poss, sizeof(poss), "BUG(the possession was not whole on %u of the %u samples after the takes: the charm, the mover session or the Possessed inhibition had gone)",
                         lost, after);
            }
            else
            {
                snprintf(poss, sizeof(poss), "OK(both takes returned with the pet charmed by him, his session under it and the Possessed inhibition up, and all three held on all %u samples after them)", after);
            }
            // --- petSurvivesTake: the second refusal. A take that killed or unsummoned the pet
            // would also have ended its flee, and the claim would read gone for no better reason.
            const uint32 gone = st.fear.gone + st.confuse.gone;
            if (!st.built || !st.fear.takeRan || !st.confuse.takeRan)
            {
                snprintf(survive, sizeof(survive), "INVALID(the pet was not built, or a take step never ran)");
            }
            else if (!st.fear.alive || !st.confuse.alive || !st.fear.stillPet || !st.confuse.stillPet)
            {
                snprintf(survive, sizeof(survive), "BUG(a take returned with the pet no longer his live pet: alive first=%d second=%d, still his Pet first=%d second=%d)",
                         st.fear.alive ? 1 : 0, st.confuse.alive ? 1 : 0,
                         st.fear.stillPet ? 1 : 0, st.confuse.stillPet ? 1 : 0);
            }
            else if (after < 10)
            {
                snprintf(survive, sizeof(survive), "INVALID(only %u samples after the two takes together)", after);
            }
            else if (gone)
            {
                snprintf(survive, sizeof(survive), "BUG(the pet was gone, dead, or no longer his on %u of the %u samples after the takes: the take ended the pet and not just the flee)",
                         gone, after);
            }
            else
            {
                snprintf(survive, sizeof(survive), "OK(it was still in the map, alive, a Pet and his own on all %u samples after both takes)", after);
            }
            return std::string("ownPetGateHeld=") + gate + " | fearEndsAtTake=" + fear +
                   " | fleeStopsAtTake=" + flee + " | confuseEndsAtTake=" + conf +
                   " | possessionSurvivesTake=" + poss + " | petSurvivesTake=" + survive;
        }

        /// One control episode's verdict text: held right up to the take, gone the instant it
        /// returned, and never held again inside the window the take anchors.
        template <class H>
        void Episode(H const& h, char const* name, char const* what, bool ready, char* out, size_t size) const
        {
            if (!ready || !h.takeRan)
            {
                snprintf(out, size, "INVALID(the %s phase never reached its take: the pet or the player went unresolvable)", name);
                return;
            }
            if (!h.took)
            {
                snprintf(out, size, "INVALID(TakePossessOf refused, so no take was made for %s to be ended by)", what);
                return;
            }
            if (!h.claimBefore)
            {
                snprintf(out, size, "INVALID(%s was not held the instant before the take, so the take had nothing to cancel)", what);
                return;
            }
            if (!h.kindSeen || h.heldSamples < 3)
            {
                snprintf(out, size, "INVALID(%s was held on only %u sample(s) before the take and was the selected behaviour on %s of them)",
                         what, h.heldSamples, h.kindSeen ? "one" : "none");
                return;
            }
            if (h.claimAfter)
            {
                snprintf(out, size, "BUG(%s was STILL held the instant the take returned, on all %u samples before it: the ownPet branch's CancelControl did not run, and Motion::Decide lets a selected Control play through a possession)",
                         what, h.heldSamples);
                return;
            }
            if (h.after < 5)
            {
                snprintf(out, size, "INVALID(only %u samples after the %s take)", h.after, name);
                return;
            }
            if (h.back)
            {
                snprintf(out, size, "BUG(%s was held again on %u of the %u samples after the take, so the take did not end the episode it cancelled)",
                         what, h.back, h.after);
                return;
            }
            snprintf(out, size, "OK(%s was held on all %u samples up to the take and the instant before it, gone the instant it returned, and never held again on any of the %u samples after it)",
                     what, h.heldSamples, h.after);
        }

        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "ownPetGateHeld=" + w + " | fearEndsAtTake=" + w + " | fleeStopsAtTake=" + w +
                   " | confuseEndsAtTake=" + w + " | possessionSurvivesTake=" + w + " | petSurvivesTake=" + w;
        }
    };

    /**
     * S74 (order 912): A POSSESSED BODY MUST BE STANDING (live test 2026-09-22, B1 bonus).
     *
     * Mind-control a creature that is SITTING -- an AFK player's body, or any creature whose
     * script sat it down -- and the possessor gets the camera, the mover and the pet bar, but
     * cannot right-click-turn it: the client will not turn a unit whose stand state is anything
     * but STAND. Unit::TakePossessOf never touched the stand state, so whatever the body was
     * doing when it was taken, it went on doing.
     *
     * THE FIX IS ONE LINE and the precedent is SpellAuraControl.cpp:509, where a stun stands its
     * victim up for the same reason. It sits at the take, in front of the client-control grant,
     * so the body is already standing on the first update block the possessor's client is given
     * rather than standing up a moment later.
     *
     * WHAT IS MEASURED. The stand state the instant TakePossessOf returns -- which is what the
     * grant hands over -- and then every 100 ms for 2.4 s, because a stand state that is right
     * for one tick and wrong afterwards would leave the same bug on screen. Remove the line and
     * bodySittingStandsAtTheTake goes BUG with UNIT_STAND_STATE_SIT printed against it, while
     * the sit itself (sitTookBeforeTheTake) still reads OK -- so the scenario cannot pass by
     * failing to sit the body down in the first place.
     */
    class PossessionStandsTheBodyUp : public Scenario
    {
    public:
        PossessionStandsTheBodyUp() : Scenario("possession-stands-the-body-up", 912) {}

        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   sitRan = false;
                uint8  stateAfterSit = UNIT_STAND_STATE_STAND;
                bool   takeRan = false;
                bool   took = false;
                bool   charmedAtTake = false;
                uint8  stateAtTake = UNIT_STAND_STATE_SIT;   ///< THE reading: the instant the take returns
                uint32 samples = 0;      ///< samples with the possession still held
                uint32 notStanding = 0;  ///< ...on which the body was not STAND after all
                uint8  worstAfter = UNIT_STAND_STATE_STAND;
            };
            Player* p = SpawnPlayer(SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
            Creature* body = p ? Spawn(WOLF, SE.x + 4.0f, SE.y, Ground(SE.x + 4.0f, SE.y, SE.z), 3.1f) : NULL;
            if (!p || !body)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            Silence(body);
            const ObjectGuid g = p->GetObjectGuid(), gb = body->GetObjectGuid();
            auto st = std::make_shared<St>();
            Log("the player %s stands at (%.1f, %.1f); the wolf he will sit down and then possess 4 yd east, stand state %u",
                g.GetString().c_str(), p->Where().X(), p->Where().Y(), body->getStandState());

            At(200, [this, gb, st]()
            {
                Creature* b = Get(gb); if (!b) { return; }
                b->SetStandState(UNIT_STAND_STATE_SIT);
                st->sitRan = true;
                st->stateAfterSit = b->getStandState();
                Log("200ms the wolf sat down: stand state %u (SIT is %u)", st->stateAfterSit, uint32(UNIT_STAND_STATE_SIT));
            });
            At(500, [this, g, gb, st]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Creature* b = Get(gb);
                if (!p || !b) { return; }
                // Unit::TakePossessOf itself, which is the call the possess effect makes; the
                // reading is taken the instant it returns, i.e. with the client-control grant
                // inside it already done.
                st->took = p->TakePossessOf(b);
                st->takeRan = true;
                st->stateAtTake = b->getStandState();
                st->charmedAtTake = b->GetCharmerGuid() == g;
                Log("500ms the take returns: took=%d charmer=%d stand state %u (STAND is %u), client mover=%d",
                    st->took ? 1 : 0, st->charmedAtTake ? 1 : 0, st->stateAtTake,
                    uint32(UNIT_STAND_STATE_STAND), b->IsClientMover() ? 1 : 0);
            });
            for (uint32 i = 1; i <= 24; ++i)
            {
                At(500 + i * 100, [this, g, gb, st, i]()
                {
                    Creature* b = Get(gb); if (!b) { return; }
                    if (b->GetCharmerGuid() != g) { return; }
                    ++st->samples;
                    const uint8 state = b->getStandState();
                    if (state != UNIT_STAND_STATE_STAND)
                    {
                        ++st->notStanding;
                        if (state > st->worstAfter) { st->worstAfter = state; }
                    }
                    if (i % 8 == 0)
                    {
                        Log("+%4ums stand state %u, charmer=%d", i * 100, state, b->GetCharmerGuid() == g ? 1 : 0);
                    }
                });
            }
            At(3100, [this, g, gb]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // The same release S71 uses, and for the same reason: a body still charmed when
                // the runner despawns it would be freed under a live charmer.
                p->ResetControlState(false);
                Creature* b = Get(gb);
                Log("3100ms the possession released: charmer=%s stand state %u",
                    b ? b->GetCharmerGuid().GetString().c_str() : "gone",
                    b ? uint32(b->getStandState()) : 0);
            });
            At(3400, [this, st]()
            {
                char sat[288], stood[384];
                if (!st->sitRan)
                {
                    snprintf(sat, sizeof(sat), "INVALID(the sit step never ran)");
                }
                else if (st->stateAfterSit != UNIT_STAND_STATE_SIT)
                {
                    snprintf(sat, sizeof(sat), "INVALID(SetStandState(SIT) left the wolf in stand state %u, so there was never a sitting body to take)", st->stateAfterSit);
                }
                else
                {
                    snprintf(sat, sizeof(sat), "OK(the wolf was in stand state %u, SIT, going into the take)", st->stateAfterSit);
                }

                if (!st->takeRan)
                {
                    snprintf(stood, sizeof(stood), "INVALID(the take step never ran)");
                }
                else if (!st->took || !st->charmedAtTake)
                {
                    snprintf(stood, sizeof(stood), "INVALID(TakePossessOf returned %d with charmer=%d, so there was no possession to read)", st->took ? 1 : 0, st->charmedAtTake ? 1 : 0);
                }
                else if (st->stateAfterSit != UNIT_STAND_STATE_SIT)
                {
                    snprintf(stood, sizeof(stood), "INVALID(the body was not sitting going in, so standing afterwards proves nothing)");
                }
                else if (st->stateAtTake != UNIT_STAND_STATE_STAND)
                {
                    snprintf(stood, sizeof(stood), "BUG(the take returned with the body still in stand state %u: the possessor holds a sitting body, which his client will not turn)", st->stateAtTake);
                }
                else if (st->samples < 12)
                {
                    snprintf(stood, sizeof(stood), "INVALID(only %u samples while the possession was held)", st->samples);
                }
                else if (st->notStanding)
                {
                    snprintf(stood, sizeof(stood), "BUG(the body stood up at the take but was back in stand state %u on %u of the %u samples after it)", st->worstAfter, st->notStanding, st->samples);
                }
                else
                {
                    snprintf(stood, sizeof(stood), "OK(a body taken while in stand state %u came out of TakePossessOf in stand state %u, STAND, and was still standing on all %u samples over the 2.4 s the possession was held)",
                             uint32(UNIT_STAND_STATE_SIT), st->stateAtTake, st->samples);
                }
                Verdict(std::string("sitTookBeforeTheTake=") + sat +
                        " | bodySittingStandsAtTheTake=" + stood);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "sitTookBeforeTheTake=" + w + " | bodySittingStandsAtTheTake=" + w;
        }
    };


    void RegisterControlScenarios(Runner& r)
    {
        r.Register(new FearBoltsAway());
        r.Register(new ConfuseLurchesNearAnchor());
        r.Register(new FearFromACorpse());
        r.Register(new FearRefreshSameClaim());
        r.Register(new ConfuseInTheAir());
        r.Register(new FearAuraMoves());
        r.Register(new FearCadenceAndSpeed());
        r.Register(new LowHealthFleeSpeed());
        r.Register(new FearSpeedFollowsTheClaim());
        // The flee's leg LENGTHS (order 66, 2026-09-22): the micro-leg cascade the live
        // capture of 2026-09-20 put on the wire, which no goal-watching sampler can see.
        r.Register(new FearMicroLegs());
        r.Register(new PlayerFear());
        r.Register(new PlayerConfuse());
        r.Register(new PlayerFeign());
        r.Register(new PlayerStun());
        // The mover-authority trio (orders 904-906, 2026-09-21): the charm that grants no mover,
        // the grant's own recompute on a body stunned before the take, and the revoke's on one
        // released while still stunned.
        r.Register(new CharmStunNotClientDriven());
        r.Register(new StunThenPossessRoots());
        r.Register(new ReleasePossessionWhileStunned());
        // The last player-only path (order 907, 2026-09-21): the ownPet branch of
        // Unit::TakePossessOf, which needs a player possessing HIS OWN PET and which S65's
        // header had to record as unreachable.
        r.Register(new PlayerOwnedPetPossession());
        // Order 912, behind the taxi family's 908-911 (live test 2026-09-22, B1 bonus): the
        // stand state a possessed body is handed over in, which decides whether the possessor
        // can turn it at all.
        r.Register(new PossessionStandsTheBodyUp());
    }
}
