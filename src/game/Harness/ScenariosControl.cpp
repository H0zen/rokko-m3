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
        /// west for 0.4-1.3 times the 22 yd to the quiet band, with FLEEING_MOVE set and the run
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
                                st->bitOnLeg = a->hasUnitState(UNIT_STAT_FLEEING_MOVE);
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
                            Log("+%4ums the first bolt ended, move=%d", t, a->hasUnitState(UNIT_STAT_FLEEING_MOVE) ? 1 : 0);
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
        /// the spot the wolf was confused at, at a walk with CONFUSED_MOVE set, launched every
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
                            if (!a->hasUnitState(UNIT_STAT_CONFUSED_MOVE)) { st->allBit = false; }
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
                        Log("+%4ums lurch %u: goal (%.1f, %.1f), %.1f yd from the anchor, walk=%d move=%d mt=%s", t, st->launches, goal.x, goal.y, d, a->IsWalking() ? 1 : 0, a->hasUnitState(UNIT_STAT_CONFUSED_MOVE) ? 1 : 0, TypeName(a));
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
                    MovementGeneratorType mtAfter;
                };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("fleeStarts=INVALID(spawn failed) | corpseFrightens=INVALID(spawn failed)"); return; }
                Silence(a);
                Silence(k);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->haveFirst = st->legRan = st->haveEnd = st->haveSecond = false;
                st->mtAfter = IDLE_MOTION_TYPE;
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
                    snprintf(starts, sizeof(starts), "%s(mt=%s 200 ms after the fear)", st->mtAfter == FLEEING_MOTION_TYPE ? "OK" : "BUG", Harness::TypeName(uint32(st->mtAfter)));
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
                        if (Type(a) != FLEEING_MOTION_TYPE) { st->typeHeld = false; Log("+%4ums mt=%s", t, TypeName(a)); }
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
                        if (Type(a) == FLEEING_MOTION_TYPE || a->hasUnitState(UNIT_STAT_FLEEING)) { st->endedClean = false; }
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
                        if (Type(w) != CONFUSED_MOTION_TYPE) { st->typeHeld = false; }
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

    void RegisterControlScenarios(Runner& r)
    {
        r.Register(new FearBoltsAway());
        r.Register(new ConfuseLurchesNearAnchor());
        r.Register(new FearFromACorpse());
        r.Register(new FearRefreshSameClaim());
        r.Register(new ConfuseInTheAir());
    }
}
