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

        /// The flee's first bolt: a wolf feared by a kobold 6 yd east bolts within pi/8 of due
        /// west for 0.4-1.3 times the 22 yd to the quiet band, with the fear's leg latched and the run
        /// gait on the leg (design §4.1: the close band, the bit with the leg, SetWalk(false));
        /// then rests 800-1500 ms standing (measured 700-1700 at the sampler's cadence) before
        /// the next bolt (the rest counts only standing).
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
                            else if (st->haveEnd && !st->haveSecond)
                            {
                                st->haveSecond = true;
                                st->secondAt = t;
                                Log("+%4ums the second bolt, %u ms after the first ended", t, t - st->endAt);
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
                        if (!st->haveEnd) { snprintf(rest, sizeof(rest), "INVALID(the first bolt never ended)"); }
                        else if (!st->haveSecond) { snprintf(rest, sizeof(rest), "BUG(no second bolt after the first ended at +%ums)", st->endAt); }
                        else
                        {
                            const uint32 gap = st->secondAt - st->endAt;
                            snprintf(rest, sizeof(rest), "%s(%u ms standing between the bolts)", (gap >= 700 && gap <= 1700) ? "OK" : "BUG", gap);
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
                    bool  haveFirst, legRan, haveEnd, haveSecond;
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
                st->mtAfter = Motion::Kind::Idle;
                At(300, [this, gk, st]()
                {
                    Creature* k = Get(gk); if (!k) { return; }
                    st->cx = k->Where().X();
                    st->cy = k->Where().Y();
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
                            else if (st->haveEnd && !st->haveSecond)
                            {
                                st->haveSecond = true;
                                st->bx1 = a->Where().X();
                                st->by1 = a->Where().Y();
                                st->goal1 = goal;
                                Log("+%4ums the second bolt: goal (%.1f, %.1f) from (%.1f, %.1f)", t, goal.x, goal.y, st->bx1, st->by1);
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
                    if (!st->haveFirst || !st->haveSecond)
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
    }

    /// S63 (the harness's first player): the fear's CONTROL HANDOFF, the half of Unit::SetFeared
    /// no creature can exercise. A fear landing on a player revokes the client's authority over
    /// him before the flee leg is laid (UnitSpeed.cpp:305-311) and the last one going gives it
    /// back (UnitSpeed.cpp:357-364); until today the only witness to either was a human in a
    /// game client. A kobold casts 5782 -- the spell, as S60 does, not the entry point every
    /// other scenario calls -- at a session-less harness player, the scenario samples for six
    /// seconds and pulls the aura at +4 s so the handback falls inside that window rather than
    /// waiting on the spell's own duration.
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
                uint32 afterSamples;      ///< samples after it went
                bool   selfWhileFeared;   ///< still his own mover on one of those first samples
                bool   allSelfAfter;      ///< his own mover on every one of the second
                bool   taxi;              ///< IsTaxiFlying() on any sample
                uint32 endedAt;           ///< when the claim was first seen gone
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
            st->fearedSamples = st->afterSamples = st->endedAt = 0;
            Log("the player %s stands at (%.1f, %.1f), the kobold 6 yd east", g.GetString().c_str(), p->Where().X(), p->Where().Y());

            At(500, [this, g, gk, st]()
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
                k->CastSpell(p, FEAR, true);
                Log("the kobold casts %u on the player at (%.1f, %.1f): his own mover before it=%d", FEAR, st->x0, st->y0, st->selfBefore ? 1 : 0);
            });
            At(600, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->auraLanded = p->HasAura(FEAR);
                Log("+ 100ms after the cast: aura=%d feared=%d rooted=%d his own mover=%d", st->auraLanded ? 1 : 0,
                    p->Blocked(Motion::ReasonFeared) ? 1 : 0, p->IsRooted() ? 1 : 0, OwnMover(p) ? 1 : 0);
            });
            // Registered before the sampler so it runs first at its own moment (the timeline
            // orders a tie by insertion): the sample at +4000 is then already an after-sample.
            At(4000, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                p->RemoveAurasDueToSpell(FEAR);
                Log("+4000ms the aura pulled: aura=%d feared=%d his own mover=%d taxi=%d", p->HasAura(FEAR) ? 1 : 0,
                    p->Blocked(Motion::ReasonFeared) ? 1 : 0, OwnMover(p) ? 1 : 0, p->IsTaxiFlying() ? 1 : 0);
            });
            for (uint32 i = 1; i <= 60; ++i)
            {
                At(500 + i * 100, [this, g, st, i]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    const uint32 t = i * 100;
                    const float d = Dist2(st->x0, st->y0, p->Where().X(), p->Where().Y());
                    if (d > st->far2) { st->far2 = d; }
                    // Sampled, not assumed: the handback at UnitSpeed.cpp:361 is refused under a
                    // flight, so a taxi anywhere in the run would be an alternative explanation
                    // for every other reading here.
                    if (p->IsTaxiFlying()) { st->taxi = true; }
                    const bool self = OwnMover(p);
                    if (p->Blocked(Motion::ReasonFeared))
                    {
                        ++st->fearedSamples;
                        if (self) { st->selfWhileFeared = true; }
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
            At(6600, [this, st]()
            {
                char taken[200], flees[96], returned[176], taxi[80];
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
                    if (st->afterSamples < 5)
                    {
                        snprintf(returned, sizeof(returned), "INVALID(only %u samples after the aura went)", st->afterSamples);
                    }
                    else if (st->allSelfAfter)
                    {
                        snprintf(returned, sizeof(returned), "OK(his own mover again from the first of the %u samples after the aura went, %u ms after the cast)",
                                 st->afterSamples, st->endedAt);
                    }
                    else
                    {
                        snprintf(returned, sizeof(returned), "BUG(not his own mover on every one of the %u samples after the aura went, %u ms after the cast)",
                                 st->afterSamples, st->endedAt);
                    }
                }
                snprintf(taxi, sizeof(taxi), "%s(IsTaxiFlying() %s throughout)", st->taxi ? "BUG" : "OK", st->taxi ? "held" : "false");
                std::string text = std::string("controlTaken=") + taken + " | playerFlees=" + flees + " | controlReturned=" + returned + " | notHeldByTaxi=" + taxi;
                Verdict(text);
            });
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
        r.Register(new PlayerFear());
    }
}
