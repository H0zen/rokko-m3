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
#include "BehaviourModel.h"   // Motion::RelayCounts by value: MotionMaster.h only forward-declares it
#include "movement/MoveSpline.h"
#include "World.h"        // the TargetPosRecalculateRange the follow's drift edge is built on
#include "Utilities/MathDefines.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The tracking-move scenarios (P5-B family 3, orders 48-53): what the chase, the follow and
// the home natives claim and no older family ever asked. The chase's counted re-lay budget
// against a target that walks a straight line, stops, reverses and then circles, and where
// it comes to rest against one that cannot move at all; the follow's pace and band behind a
// running leader, the leader's facing copied at rest and the travel facing kept on a leg,
// and a leader that changes the KIND of its movement under the follower; the evade that
// waits under a root and runs its arrival recipe exactly once. Every scenario spawns its own
// actors at the block family's spot and the runner despawns them. Pt, Spread and Dist2 are
// the shared ones (Scenario.h).
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;
        const uint32 CHICKEN = 621;     // a walker: the follow's gait mirror wants a leader that walks
        const Pt SE = { -3200.0f, -300.0f, 47.0f };   // the block family's spot (ScenariosBlock.cpp)
        const uint32 ROOT = 745;        // Web: a plain root aura, about 5 s, no damage

        /// Pet.h's PET_FOLLOW_DIST and PET_FOLLOW_ANGLE, copied rather than included: the
        /// harness has no other reason to pull the pet header in, and a pet's own numbers are
        /// what a follow is asked for everywhere in the shell.
        const float FOLLOW_DIST = 1.0f;
        const float FOLLOW_ANGLE = (M_PI_F / 4.00f) * 3.50f;

        /// Three dimensions, unlike the shared planar Dist2: a chase's stop is measured the
        /// way the engage effect measures its reach (NativeBehaviour.cpp), in all three.
        float Dist3(float x1, float y1, float z1, float x2, float y2, float z2)
        {
            const float dx = x1 - x2, dy = y1 - y2, dz = z1 - z2;
            return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
        }

        float Dist3(Creature* a, Creature* b)
        {
            return Dist3(a->Where().X(), a->Where().Y(), a->Where().Z(),
                         b->Where().X(), b->Where().Y(), b->Where().Z());
        }

        /// The bearing from one point to another in [0, 2*pi), as Geometry::Placement::Face
        /// stores every facing (ScenariosDefault.cpp's own copy of the same one-liner).
        float Bearing(float fx, float fy, float tx, float ty)
        {
            const float a = std::atan2(ty - fy, tx - fx);
            return (a >= 0.0f) ? a : 2.0f * M_PI_F + a;
        }

        /// The shorter angular distance between two facings, both already normalised.
        float AngleDiff(float a, float b)
        {
            float d = std::fabs(a - b);
            if (d > M_PI_F)
            {
                d = 2.0f * M_PI_F - d;
            }
            return d;
        }

        /// The pair's combat reaches, the chase's band is built on (design §6.1).
        float ReachSum(Creature* a, Creature* b)
        {
            return a->GetFloatValue(UNIT_FIELD_COMBATREACH) + b->GetFloatValue(UNIT_FIELD_COMBATREACH);
        }

        /// The client's own melee range for the pair: the chase's re-approach edge.
        float MeleeRange(Creature* a, Creature* b)
        {
            return std::max(ReachSum(a, b) + 4.0f / 3.0f, 5.0f);
        }

        /// The two bounding radii a follow's standing distance folds in beside its offset.
        float ExtentSum(Creature* a, Creature* b)
        {
            return a->Where().Extent() + b->Where().Extent();
        }

        /// "routine 12, cut 1, partial 0, blocked 0, finished 3, first 1 (total 17)".
        std::string Causes(Motion::RelayCounts const& c)
        {
            char text[160];
            snprintf(text, sizeof(text), "routine %u, cut %u, partial %u, blocked %u, finished %u, first %u (total %u)",
                     c.routine, c.cut, c.partial, c.blocked, c.finished, c.first, c.Total());
            return text;
        }

        /// A creature's own default movement is a random wander around its spawn, so the moment
        /// a scripted leg ends it heads back there -- 56 yd of it, on the first run of order 48,
        /// which is also why a scenario watching for "the leg finished" never saw a finalized
        /// spline: the wander laid the next leg on the same tick. MoveIdle is NOT the answer,
        /// because an Idle request over a non-empty stack parks a SCRIPTED-layer idle
        /// (Arbiter::RequestDefault), which outranks the very chase being measured. Naming the
        /// default idle and re-initialising is: the factory native becomes IdleBehaviour and
        /// nothing at all moves the actor except this file.
        void Park(Creature* c)
        {
            c->SetDefaultMovementType(CREATURE_MOVEMENT_IDLE);
            c->GetMotionMaster()->Initialize();
        }

        /// A point leg always runs: MotionMaster::MovePoint asks for no gait at all and the
        /// driver reads MOVE_WALK off the intent alone (MotionDriver.cpp: init.SetWalk(intent)),
        /// so setting the walk flag on a scripted target would make it LOOK like a walker while
        /// covering ground at 5.4 yd/s. Holding its run speed down to its own walk speed makes
        /// the flag and the pace agree, which is what a follower mirroring a gait is owed.
        void WalkPace(Creature* c)
        {
            const float rate = c->GetSpeedRate(MOVE_RUN);
            const float unrated = rate > 0.0f ? c->GetSpeed(MOVE_RUN) / rate : 0.0f;
            if (unrated > 0.0f)
            {
                c->SetSpeedRate(MOVE_RUN, c->GetSpeed(MOVE_WALK) / unrated, true);
            }
            c->SetWalk(true, false);
        }

        /// Undoes WalkPace: the creature's own run rate back, and the walk flag cleared.
        void RunPace(Creature* c)
        {
            c->SetSpeedRate(MOVE_RUN, 1.0f, true);
            c->SetWalk(false, false);
        }

        /// The band a follower can hold behind its leader, derived from the native's own
        /// parameters rather than guessed (FollowBehaviour, MakeFollowParams, MotionDriver):
        ///   the standing spot it is aiming at           offset + both extents
        /// + the drift it is allowed before a re-lay     the native's own edge
        /// + the driver's own floor under a re-lay       MIN_RELAY_DISTANCE, 0.5 yd (a live leg whose
        ///                                               goal moved less than that is left alone)
        /// + one re-lay cadence of the leader's travel   0.4 s x its speed (MakeFollowParams routineMs)
        /// + one world update of it                      0.1 s x its speed
        /// The drift edge is FollowBehaviour::Drifted's own arithmetic, read back rather than
        /// restated: `allowed = recalcRange - target extent + (own + target extent)`, tested through
        /// `DriftedBeyond(sight, allowed + target extent)`, so the edge is
        /// `recalcRange + own extent + target extent`, and recalcRange is the live config, not a
        /// literal. The `offset > FOLLOW_DIST_GAP_FOR_DIST_FACTOR` slop branch is deliberately left
        /// out: it only applies past a 3 yd follow distance and every follow here asks for 1.0.
        /// `terms` is filled with the same numbers for the verdict text, so the band is read as
        /// a derivation and not as a fit.
        float FollowBand(Creature* f, Creature* leader, float leaderSpeed, std::string& terms)
        {
            const float oe = f->Where().Extent(), te = leader->Where().Extent();
            const float standing = FOLLOW_DIST + oe + te;
            const float recalcRange = sWorld.getConfig(CONFIG_FLOAT_RATE_TARGET_POS_RECALCULATION_RANGE);
            const float recalc = recalcRange + oe + te;
            const float floorTerm = 0.5f;                 // MotionDriver's MIN_RELAY_DISTANCE
            const float cadence = 0.4f * leaderSpeed;
            const float tick = 0.1f * leaderSpeed;
            char buf[224];
            snprintf(buf, sizeof(buf), "standing %.2f + drift edge %.2f (recalc range %.2f) + re-lay floor %.2f + one 400 ms cadence %.2f + one 100 ms update %.2f",
                     standing, recalc, recalcRange, floorTerm, cadence, tick);
            terms = buf;
            return standing + recalc + floorTerm + cadence + tick;
        }

        /// A point on the circle of `radius` around `centre`, at octant `idx` of eight.
        Pt Octant(Pt const& centre, uint32 idx, float radius)
        {
            const float a = 2.0f * M_PI_F * (float(idx % 8) / 8.0f);
            Pt p = { centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), centre.z };
            return p;
        }

        /// Order 48: the chase's re-lay budget, its engage and its band against a target that
        /// keeps changing what it does. The kobold walks a 60 yd straight line, stands 3 s, RUNS
        /// a 15 yd reversal and stops, then RUNS two laps of a 7 yd ring around a fixed point --
        /// four kinds of drift against one 1 Hz routine cadence (design §5,
        /// §6.1). `routineBudget` is the design's own number: one routine re-lay per second is
        /// the ceiling the cadence sets, and the generator's 100 ms poll could not have held it.
        /// `engages` reads the EngageInReach effect where it can be read at all -- the wolf opens
        /// with a RANGED attack, so the melee bit is the effect's own doing and nothing else's,
        /// and it must not appear before the wolf is inside the client's melee range. `reacquires`
        /// is the reversal: a target that outruns the cadence gets away, and the chase has to come
        /// back. `noOrbit` is where the chase AIMS over the ring: every leg it lays must go to the
        /// side of the target it already stands on, never around it to the far side.
        class ChaseRelayBudget : public Scenario
        {
        public:
            ChaseRelayBudget() : Scenario("chase-relay-budget", 48) {}

            void Prepare() override
            {
                struct Second { uint32 t; int phase; uint32 routine; bool have; float dist; };
                struct State
                {
                    int    phase = 1;          ///< 1 the walk, 2 the stand, 3 the running reversal, 4 the re-acquire window, 5 the circle, 6 done
                    uint32 phaseAt = 0;        ///< when the current phase began
                    uint32 circleAt = 0;       ///< when the last circle point was handed out
                    uint32 circleIdx = 0;
                    Pt     aEnd = { 0.0f, 0.0f, 0.0f };
                    Pt     goal = { 0.0f, 0.0f, 0.0f };   ///< where the phase's own point leg was sent
                    float  melee = 0.0f;
                    // engages: the melee bit against the reach that is supposed to grant it
                    bool   reached = false;    ///< the wolf has been inside melee range at least once
                    uint32 reachAt = 0;
                    float  reachGap = 0.0f;
                    bool   bitEarly = false;   ///< the bit was set at a sample before the wolf was ever in reach
                    float  bitEarlyGap = 0.0f;
                    bool   bitSet = false;
                    uint32 bitAt = 0;
                    bool   victimKept = true;  ///< the chase never lost its victim (it would hold for good if it had)
                    // reacquires: the gap the running reversal opened, and how fast it closed again
                    uint32 stopAt = 0;         ///< when the reversal's own leg ended
                    float  stopGap = 0.0f;
                    float  awayPeak = 0.0f;    ///< the widest the gap ever got over the reversal
                    bool   reacquired = false;
                    uint32 reacquireMs = 0;
                    // noOrbit: where the chase AIMS, relative to the line from the target to
                    // itself; plus the winding and the ground, which are reported, not gated
                    Pt     ringCentre = { 0.0f, 0.0f, 0.0f };   ///< fixed for the whole phase: what the target runs around
                    uint32 ringHanded = 0;     ///< how many of the ring's points have been handed out
                    float  sideWorst = 0.0f;   ///< the worst angle between "the spot it laid" and "the side it stands on"
                    uint32 sideChecks = 0;
                    uint32 lastTotal = 0;
                    bool   haveTotal = false;
                    float  winding = 0.0f;
                    float  lastBearing = 0.0f;
                    bool   haveBearing = false;
                    float  circleGap = 0.0f;
                    float  circleGround = 0.0f;
                    Pt     lastW = { 0.0f, 0.0f, 0.0f };
                    bool   haveLastW = false;
                    Motion::RelayCounts end;
                    bool   haveEnd = false;
                };
                const uint32 kPhaseAStart = 1000;
                const float kLine = 60.0f, kBack = 15.0f, kRadius = 7.0f;
                const uint32 kRingPoints = 16;   // two full laps of eight octants

                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 6.0f, SE.y, Ground(SE.x + 6.0f, SE.y, SE.z), 3.1f);
                if (!w || !k)
                {
                    Verdict("routineBudget=INVALID(spawn failed) | reacquires=INVALID(spawn failed) | noOrbit=INVALID(spawn failed) | engages=INVALID(spawn failed)");
                    return;
                }
                w->SetMaxHealth(500000); w->SetHealth(500000);
                k->SetMaxHealth(500000); k->SetHealth(500000);
                k->setFaction(14);
                // Both AIs go: the kobold's would take the wheel back the moment a scripted leg
                // ended (the wolf's melee wakes it, and a leg it cannot path arms the no-path
                // evade, which walked it all the way home mid-phase on the first run of this
                // scenario), and the wolf's SelectHostileTarget would turn it to face its victim
                // every tick, which is a facing the chase is supposed to be asking for itself.
                Silence(w); Silence(k);
                // A creature default is a random wander around its spawn, so the moment a
                // scripted point leg ends it walks all the way back there (it did, 56 yd, on the
                // first run of this scenario). Parked.
                Park(w); Park(k);
                WalkPace(k);   // the target walks: the drift the cadence must catch is slow enough to read
                Load(SE.x + kLine + 10.0f, SE.y);
                const ObjectGuid g = w->GetObjectGuid(), h = k->GetObjectGuid();
                auto st = std::make_shared<State>();
                auto secs = std::make_shared<std::vector<Second> >();
                st->phaseAt = kPhaseAStart;

                At(500, [this, g, h]()
                {
                    Creature* w = Get(g); Creature* k = Get(h); if (!w || !k) { return; }
                    // RANGED, deliberately. Attack(k, true) would set UNIT_STAT_MELEE_ATTACKING
                    // here, with no range test of any kind, and `engages` would then be true from
                    // its first sample whatever the kernel did. A ranged attack sets the victim --
                    // which the chase needs, since TrackingBehaviour holds the whole tick while
                    // the target is not the victim -- and leaves the melee bit for the chase's own
                    // EngageInReach effect to grant when the wolf is actually in reach.
                    w->Attack(k, false);
                    w->AddThreat(k, 1000.0f);
                    w->GetMotionMaster()->MoveChase(k);
                    Log("ranged Attack + MoveChase from %.1f yd, mt=%s, melee bit=%d, reaches %.2f + %.2f, melee range %.2f; the wolf runs at %.2f yd/s, the kobold walks at %.2f",
                        Dist3(w, k), TypeName(w), w->hasUnitState(UNIT_STAT_MELEE_ATTACKING) ? 1 : 0,
                        w->GetFloatValue(UNIT_FIELD_COMBATREACH), k->GetFloatValue(UNIT_FIELD_COMBATREACH),
                        MeleeRange(w, k), w->GetSpeed(MOVE_RUN), k->GetSpeed(MOVE_RUN));
                });
                At(kPhaseAStart, [this, h, st, kLine]()
                {
                    Creature* k = Get(h); if (!k) { return; }
                    const float tx = k->Where().X() + kLine, ty = k->Where().Y();
                    st->goal.x = tx; st->goal.y = ty; st->goal.z = SE.z;
                    k->GetMotionMaster()->MovePoint(1, tx, ty, Ground(tx, ty, SE.z), true);
                    Log("phase A: the kobold walks %.0f yd straight (+x) to %.1f %.1f", kLine, tx, ty);
                });

                // The const locals in these capture lists look redundant to clang, which reports
                // -Wunused-lambda-capture for them, but MSVC refuses to compile the lambda without
                // them (C3493, "cannot be implicitly captured because no default capture mode has
                // been specified") -- even for a constexpr one, as RedirectStreamTest's PER_THREAD
                // shows. The captures stay and the clang warning is turned off in the build flags.
                auto verdict = [this, st, secs, kPhaseAStart, kRadius]()
                {
                    std::string routineBudget, reacquires, noOrbit, engages;
                    char text[512];
                    // routineBudget: the routine re-lays the straight walk cost, from the second
                    // second of the phase (the first carries the very first spot and whatever the
                    // approach latched) to its last.
                    std::vector<Second> line;
                    for (size_t i = 0; i < secs->size(); ++i)
                    {
                        Second const& s = (*secs)[i];
                        if (s.phase == 1 && s.have && s.t >= kPhaseAStart + 2000) { line.push_back(s); }
                    }
                    if (line.size() < 3)
                    {
                        snprintf(text, sizeof(text), "routineBudget=INVALID(%u seconds of the straight walk sampled)", uint32(line.size()));
                        routineBudget = text;
                    }
                    else
                    {
                        const float span = float(line.back().t - line.front().t) / 1000.0f;
                        const uint32 laid = line.back().routine - line.front().routine;
                        const float rate = laid / span;
                        snprintf(text, sizeof(text), "routineBudget=%s(%.2f routine re-lays per second: %u over %.1f s of the straight walk)",
                                 rate <= 1.0f ? "OK" : "BUG", rate, laid, span);
                        routineBudget = text;
                    }
                    // engages: the melee bit is the effect's alone. It must be clear for as long as
                    // the wolf has never been inside the melee range, and set within 500 ms of the
                    // first sample that is.
                    if (!st->victimKept)
                    {
                        engages = "engages=INVALID(the chase lost its victim: the ranged attack did not hold)";
                    }
                    else if (!st->reached)
                    {
                        engages = "engages=INVALID(the wolf was never sampled inside the melee range)";
                    }
                    else if (st->bitEarly)
                    {
                        snprintf(text, sizeof(text), "engages=BUG(the melee bit was already set %.2f yd out, before the wolf was ever inside the %.2f yd melee range)",
                                 st->bitEarlyGap, st->melee);
                        engages = text;
                    }
                    else if (!st->bitSet)
                    {
                        snprintf(text, sizeof(text), "engages=BUG(the wolf reached %.2f yd, inside the %.2f yd melee range, and the melee bit never appeared)",
                                 st->reachGap, st->melee);
                        engages = text;
                    }
                    else
                    {
                        const uint32 lag = st->bitAt - st->reachAt;
                        snprintf(text, sizeof(text), "engages=%s(clear until the wolf was inside the %.2f yd melee range at %.2f yd, then set %u ms later)",
                                 lag <= 500 ? "OK" : "BUG", st->melee, st->reachGap, lag);
                        engages = text;
                    }
                    // reacquires: the running reversal outruns the cadence and opens a gap; the
                    // chase has to close it again within 3 s of the target coming to a stop.
                    if (!st->stopAt)
                    {
                        reacquires = "reacquires=INVALID(the running reversal never finished)";
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "reacquires=%s(the reversal opened the gap to %.2f yd, %.2f at its stop; back inside the %.2f yd melee range %u ms after it)",
                                 st->reacquired ? "OK" : "BUG", st->awayPeak, st->stopGap, st->melee, st->reacquireMs);
                        reacquires = text;
                    }
                    // noOrbit. The winding of the bearing between the two bodies is the PAIR's
                    // relative revolution, and this phase scripts the target to revolve around the
                    // chaser, so it winds whatever the chase does: the run that first measured it
                    // logged the wolf standing at one spot for six straight seconds while the kobold
                    // walked right round it, and the winding still came to 4.56 rad. A chaser that
                    // never moves at all scores a full turn here. So the winding is printed as
                    // evidence and the gate is the thing that actually distinguishes an orbit: where
                    // the chase AIMS. Every leg it lays must go to a spot on the side of the target
                    // it already stands on; a spot on the far side is the walk-around, and is what a
                    // chase deriving its angle from the target's facing would produce. The gap is
                    // the second half: it must never be left a whole melee range outside the circle.
                    std::vector<Second> circle;
                    for (size_t i = 0; i < secs->size(); ++i)
                    {
                        if ((*secs)[i].phase == 5) { circle.push_back((*secs)[i]); }
                    }
                    if (circle.size() < 3)
                    {
                        snprintf(text, sizeof(text), "noOrbit=INVALID(%u seconds of the ring sampled)", uint32(circle.size()));
                        noOrbit = text;
                    }
                    else if (!st->sideChecks)
                    {
                        snprintf(text, sizeof(text), "noOrbit=INVALID(the chase laid no leg at all over %u s of the ring: nothing to read an aim off)", uint32(circle.size()));
                        noOrbit = text;
                    }
                    else
                    {
                        const float ceiling = kRadius + st->melee;
                        const bool ok = st->sideWorst <= M_PI_F / 2.0f && st->circleGap <= ceiling;
                        snprintf(text, sizeof(text), "noOrbit=%s(%u legs laid over %u s of the %.0f yd ring, the worst %.0f deg off the side the chaser already stood on; it covered %.1f yd of ground and the gap peaked at %.2f yd against the %.2f the ring and the melee range allow; the pair's bearing wound %.2f rad, which is not gated because it is the PAIR's relative revolution and this ring scripts the target to revolve)",
                                 ok ? "OK" : "BUG", st->sideChecks, uint32(circle.size()), kRadius,
                                 st->sideWorst * 180.0f / M_PI_F, st->circleGround, st->circleGap, ceiling, st->winding);
                        noOrbit = text;
                    }
                    std::string tail = st->haveEnd ? (" (re-lays: " + Causes(st->end) + ")") : " (re-lays: the chase was not selected at the end)";
                    Log("totals: %s", st->haveEnd ? Causes(st->end).c_str() : "the chase was not selected at the end");
                    Verdict(routineBudget + " | " + reacquires + " | " + noOrbit + " | " + engages + tail);
                };

                for (uint32 i = 1; i <= 400; ++i)
                {
                    At(kPhaseAStart + i * 250, [this, g, h, st, secs, verdict, i, kPhaseAStart, kBack, kRadius, kRingPoints]()
                    {
                        if (st->phase >= 6) { return; }
                        Creature* w = Get(g); Creature* k = Get(h); if (!w || !k) { return; }
                        const uint32 t = kPhaseAStart + i * 250;
                        Motion::RelayCounts const* rc = Relays(w);
                        const float d = Dist3(w, k);
                        const float melee = MeleeRange(w, k);
                        st->melee = melee;
                        if (rc) { st->end = *rc; st->haveEnd = true; }
                        if (w->getVictim() != k) { st->victimKept = false; }

                        // engages: the reach the effect tests, and the bit it alone may grant.
                        const bool bit = w->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
                        if (!st->reached && d <= melee)
                        {
                            st->reached = true; st->reachAt = t; st->reachGap = d;
                        }
                        if (bit && !st->bitSet)
                        {
                            st->bitSet = true; st->bitAt = t;
                            if (!st->reached) { st->bitEarly = true; st->bitEarlyGap = d; }
                            Log("+%5ums the melee bit appeared at %.2f yd (melee range %.2f, first inside it at %s)",
                                t, d, melee, st->reached ? "this sample or earlier" : "NEVER YET");
                        }

                        // reacquires: how far the running reversal got away, and when the gap closed.
                        if (st->phase == 3)
                        {
                            st->awayPeak = std::max(st->awayPeak, d);
                        }
                        if (st->phase == 4 && !st->reacquired)
                        {
                            st->awayPeak = std::max(st->awayPeak, d);
                            if (d <= melee)
                            {
                                st->reacquired = true;
                                st->reacquireMs = t - st->stopAt;
                            }
                        }

                        // noOrbit: orbiting is a chase that walks AROUND its target to reach a spot
                        // on the far side, so the spot is what is read -- every freshly laid leg's
                        // goal, against the line from the target to where the chaser already stands.
                        // A head-on chase (TrackingBehaviour::Bearing for angle 0 is
                        // AngleFromTo(centre, mover): approach from where the mover already is) puts
                        // it at ~0 deg; a chase that derived its spot from the target's FACING
                        // instead would send it round the far side, which is the bug. The winding and
                        // the ground below are reported beside it but cannot be the gate -- see the
                        // verdict's comment.
                        if (st->phase == 5)
                        {
                            const uint32 total = rc ? rc->Total() : 0;
                            const bool fresh = rc && st->haveTotal && total > st->lastTotal;
                            if (rc) { st->lastTotal = total; st->haveTotal = true; }
                            // "Fresh" is the NATIVE's counter, which TrackingBehaviour::Derive
                            // increments before the driver has decided anything: if the new spot is
                            // under MotionDriver's MIN_RELAY_DISTANCE from the leg already running,
                            // ReconcileMove keeps that leg and FinalDestination() is one 0.5 yd
                            // re-lay floor stale. Bounded by that half yard, and at a 7 yd ring it
                            // cannot move the side angle across the 90 deg gate.
                            if (fresh && !w->movespline->Finalized())
                            {
                                const Movement::Vector3 goal = w->movespline->FinalDestination();
                                const float toGoal = Bearing(k->Where().X(), k->Where().Y(), goal.x, goal.y);
                                const float toChaser = Bearing(k->Where().X(), k->Where().Y(), w->Where().X(), w->Where().Y());
                                const float off = AngleDiff(toGoal, toChaser);
                                st->sideWorst = std::max(st->sideWorst, off);
                                ++st->sideChecks;
                                Log("+%5ums the chase laid a fresh leg while the target circled: the spot is %.0f deg off the side it already stands on", t, off * 180.0f / M_PI_F);
                            }
                            const float b = Bearing(k->Where().X(), k->Where().Y(), w->Where().X(), w->Where().Y());
                            if (st->haveBearing)
                            {
                                float step = b - st->lastBearing;
                                while (step > M_PI_F) { step -= 2.0f * M_PI_F; }
                                while (step < -M_PI_F) { step += 2.0f * M_PI_F; }
                                st->winding += step;
                            }
                            st->lastBearing = b; st->haveBearing = true;
                            const Pt here = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                            if (st->haveLastW) { st->circleGround += Dist2(here.x, here.y, st->lastW.x, st->lastW.y); }
                            st->lastW = here; st->haveLastW = true;
                            st->circleGap = std::max(st->circleGap, d);
                        }

                        if (i % 4 == 0)
                        {
                            Second s;
                            s.t = t; s.phase = st->phase; s.have = rc != NULL;
                            s.routine = rc ? rc->routine : 0;
                            s.dist = d;
                            secs->push_back(s);
                            Log("+%5ums phase %d re-lays %s | gap %.2f yd | melee bit %d | wolf %.1f %.1f | kobold %.1f %.1f",
                                t, s.phase, rc ? Causes(*rc).c_str() : "(the chase is not selected)", s.dist, bit ? 1 : 0,
                                w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y());
                        }
                        switch (st->phase)
                        {
                            case 1:
                                if (t - st->phaseAt >= 2000 && (k->movespline->Finalized() || Dist2(k->Where().X(), k->Where().Y(), st->goal.x, st->goal.y) <= 1.5f))
                                {
                                    st->aEnd.x = k->Where().X(); st->aEnd.y = k->Where().Y(); st->aEnd.z = k->Where().Z();
                                    st->phase = 2; st->phaseAt = t;
                                    Log("phase B: the kobold arrived at %.1f %.1f after %u ms and stands 3 s", st->aEnd.x, st->aEnd.y, t - kPhaseAStart);
                                }
                                break;
                            case 2:
                                if (t - st->phaseAt >= 3000)
                                {
                                    // The reversal RUNS. A 2.5 yd/s walker never leaves a 6 yd/s
                                    // chaser's melee range, so a walked reversal asks the chase
                                    // nothing; a runner outpaces the one-second cadence and gets
                                    // away, which is the only way to watch it come back.
                                    RunPace(k);
                                    const float tx = st->aEnd.x - kBack, ty = st->aEnd.y;
                                    st->goal.x = tx; st->goal.y = ty; st->goal.z = SE.z;
                                    k->GetMotionMaster()->MovePoint(2, tx, ty, Ground(tx, ty, SE.z), true);
                                    st->phase = 3; st->phaseAt = t;
                                    Log("phase C: the kobold turns 180 deg and RUNS %.0f yd back to %.1f %.1f at %.2f yd/s (the wolf is %.2f yd away)",
                                        kBack, tx, ty, k->GetSpeed(MOVE_RUN), Dist3(w, k));
                                }
                                break;
                            case 3:
                                if (t - st->phaseAt >= 1000 && (k->movespline->Finalized() || Dist2(k->Where().X(), k->Where().Y(), st->goal.x, st->goal.y) <= 1.5f))
                                {
                                    st->phase = 4; st->phaseAt = t; st->stopAt = t; st->stopGap = d;
                                    Log("the kobold stopped %.2f yd from the wolf after the run; the chase has 3 s to be back inside %.2f", d, melee);
                                }
                                break;
                            case 4:
                                if (t - st->phaseAt >= 3000)
                                {
                                    // The target RUNS the ring, and the ring's centre is FIXED where
                                    // the wolf stands as the phase opens. A walker cannot reach an
                                    // octant of a 7 yd ring before the next one is handed out, and a
                                    // centre that follows the chaser around keeps the target pinned
                                    // on the melee edge: the first cut of this phase did both, and
                                    // the chase laid exactly ONE leg over the whole eight seconds.
                                    // A runner on a fixed ring leaves the 5 yd edge on every octant,
                                    // so the chase has to derive a fresh spot each time -- which is
                                    // the only way the aim's-side gate gets anything to read.
                                    RunPace(k);
                                    st->phase = 5; st->phaseAt = t; st->circleAt = t;
                                    st->ringCentre.x = w->Where().X(); st->ringCentre.y = w->Where().Y(); st->ringCentre.z = w->Where().Z();
                                    const float b = Bearing(st->ringCentre.x, st->ringCentre.y, k->Where().X(), k->Where().Y());
                                    st->circleIdx = (uint32((b / (2.0f * M_PI_F)) * 8.0f + 0.5f) % 8) + 1;   // the octant after the one it stands in
                                    const Pt p = Octant(st->ringCentre, st->circleIdx, kRadius);
                                    k->GetMotionMaster()->MovePoint(3, p.x, p.y, Ground(p.x, p.y, p.z), true);
                                    st->ringHanded = 1;
                                    Log("phase D: the kobold RUNS a %.0f yd ring, %u octants (two laps), around the fixed point %.1f %.1f at %.2f yd/s",
                                        kRadius, kRingPoints, st->ringCentre.x, st->ringCentre.y, k->GetSpeed(MOVE_RUN));
                                }
                                break;
                            case 5:
                            {
                                // The next octant goes out when the target has reached the last one,
                                // or after 900 ms, whichever comes first: a 7 yd ring's octant chord
                                // is 5.36 yd, which a 7 yd/s runner covers in 0.77 s. The 500 ms
                                // floor is there because the leg is laid on the map tick AFTER the
                                // request, so a poll right after one is handed out would still see a
                                // finalized spline and skip an octant.
                                const bool arrived = t - st->circleAt >= 500 && k->movespline->Finalized();
                                if (st->ringHanded < kRingPoints && (arrived || t - st->circleAt >= 900))
                                {
                                    st->circleAt = t;
                                    ++st->circleIdx;
                                    ++st->ringHanded;
                                    const Pt p = Octant(st->ringCentre, st->circleIdx, kRadius);
                                    k->GetMotionMaster()->MovePoint(3, p.x, p.y, Ground(p.x, p.y, p.z), true);
                                }
                                else if (st->ringHanded >= kRingPoints && (arrived || t - st->circleAt >= 900))
                                {
                                    st->phase = 6;
                                    Log("phase D done: %u octants handed out over %u ms", st->ringHanded, t - st->phaseAt);
                                    At(500, verdict);
                                }
                                if (st->phase == 5 && t - st->phaseAt >= 30000)   // the ring never finished: say what was measured
                                {
                                    st->phase = 6;
                                    At(500, verdict);
                                }
                                break;
                            }
                            default:
                                break;
                        }
                    });
                }
                At(kPhaseAStart + 401 * 250, verdict);   // the phases never completed: whatever was measured, said plainly
            }
        };

        /// Order 49: where the chase comes to rest, and where its engage may first fire. The wolf
        /// opens with a RANGED attack, so the melee bit is the EngageInReach effect's alone, and
        /// `attacks` can say when it appeared relative to the reach that grants it instead of
        /// restating the Attack call the scenario itself made. The kobold is held by a Web for the whole
        /// measurement, because a chase target that walks into its chaser on its own (the
        /// wolf's melee wakes the kobold's AI, as root-mid-chase already records) would close
        /// the band from the other side and what is measured would be the pair's stop, not the
        /// chase's. `stopDistance` is the band itself (design §6.1: the spot is asked for at
        /// offset + 0.5 + the two reaches, and the free-spot search adds the two bounding
        /// radii on top, so the answer sits between the contact gap and the client's melee
        /// range); `noRelayWhileStanding` is the whole point of the one-second cadence -- a
        /// target that does not move costs nothing at all; `attacks` reads the engage.
        class ChaseStopsInsideReach : public Scenario
        {
        public:
            ChaseStopsInsideReach() : Scenario("chase-stops-inside-reach", 49) {}

            void Prepare() override
            {
                struct State
                {
                    bool   ran = false;         ///< the wolf's spline has been seen running since the chase began
                    bool   stopped = false;
                    uint32 stopAt = 0;
                    float  stop = 0.0f;         ///< the 3D gap at the stop
                    float  reachSum = 0.0f;
                    float  melee = 0.0f;
                    uint32 relaysAtStop = 0;
                    uint32 relaysAfter = 0;
                    bool   measured = false;    ///< the 5 s standing window has been read
                    bool   haveRelays = false;
                    // attacks: the melee bit against the reach that is supposed to grant it
                    bool   reached = false;
                    uint32 reachAt = 0;
                    float  reachGap = 0.0f;
                    bool   bitEarly = false;
                    float  bitEarlyGap = 0.0f;
                    bool   bitSet = false;
                    uint32 bitAt = 0;
                    bool   victimKept = true;
                };
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!w || !k)
                {
                    Verdict("stopDistance=INVALID(spawn failed) | noRelayWhileStanding=INVALID(spawn failed) | attacks=INVALID(spawn failed)");
                    return;
                }
                w->SetMaxHealth(500000); w->SetHealth(500000);
                k->SetMaxHealth(500000); k->SetHealth(500000);
                k->setFaction(14);
                Silence(w); Silence(k);   // a silent kobold really does stand: nothing walks it into its chaser
                Park(w); Park(k);   // and neither wanders back to its spawn between legs
                const ObjectGuid g = w->GetObjectGuid(), h = k->GetObjectGuid();
                auto st = std::make_shared<State>();
                At(700, [this, g, h]()
                {
                    Creature* w = Get(g); Creature* k = Get(h); if (!w || !k) { return; }
                    // Ranged, as order 48: the victim the chase needs, without the melee bit
                    // that would make `attacks` true before the wolf had moved an inch.
                    w->Attack(k, false);
                    w->AddThreat(k, 1000.0f);
                    w->GetMotionMaster()->MoveChase(k);
                    Log("ranged Attack + MoveChase from %.2f yd at a standing target, mt=%s, melee bit=%d, melee range %.2f",
                        Dist3(w, k), TypeName(w), w->hasUnitState(UNIT_STAT_MELEE_ATTACKING) ? 1 : 0, MeleeRange(w, k));
                });
                for (uint32 i = 1; i <= 56; ++i)
                {
                    At(1000 + i * 250, [this, g, h, st, i]()
                    {
                        Creature* w = Get(g); Creature* k = Get(h); if (!w || !k) { return; }
                        const uint32 t = 1000 + i * 250;
                        Motion::RelayCounts const* rc = Relays(w);
                        const bool running = !w->movespline->Finalized();
                        if (running) { st->ran = true; }
                        const float gap = Dist3(w, k);
                        const float melee = MeleeRange(w, k);
                        if (w->getVictim() != k) { st->victimKept = false; }
                        if (!st->reached && gap <= melee) { st->reached = true; st->reachAt = t; st->reachGap = gap; }
                        if (w->hasUnitState(UNIT_STAT_MELEE_ATTACKING) && !st->bitSet)
                        {
                            st->bitSet = true; st->bitAt = t;
                            if (!st->reached) { st->bitEarly = true; st->bitEarlyGap = gap; }
                            Log("+%5ums the melee bit appeared at %.2f yd (melee range %.2f)", t, gap, melee);
                        }
                        if (!st->stopped && st->ran && !running)
                        {
                            st->stopped = true;
                            st->stopAt = t;
                            st->stop = Dist3(w, k);
                            st->reachSum = ReachSum(w, k);
                            st->melee = MeleeRange(w, k);
                            st->haveRelays = rc != NULL;
                            st->relaysAtStop = rc ? rc->Total() : 0;
                            Log("+%5ums the chase came to rest %.2f yd away (reaches %.2f, melee range %.2f), re-lays %s",
                                t, st->stop, st->reachSum, st->melee, rc ? Causes(*rc).c_str() : "(the chase is not selected)");
                        }
                        if (st->stopped && !st->measured && t >= st->stopAt + 5000)
                        {
                            st->measured = true;
                            st->relaysAfter = rc ? rc->Total() : 0;
                            if (!rc) { st->haveRelays = false; }
                            Log("+%5ums 5 s of standing later: re-lays %s, gap %.2f yd, the kobold has moved %.2f yd",
                                t, rc ? Causes(*rc).c_str() : "(the chase is not selected)", Dist3(w, k),
                                Dist2(k->Where().X(), k->Where().Y(), k->Spawn().X(), k->Spawn().Y()));
                        }
                    });
                }
                At(15500, [this, st]()
                {
                    if (!st->stopped)
                    {
                        Verdict("stopDistance=INVALID(the chase never came to rest) | noRelayWhileStanding=INVALID(the chase never came to rest) | attacks=INVALID(the chase never came to rest)");
                        return;
                    }
                    const float low = 0.5f + st->reachSum;
                    char text[512];
                    const bool inBand = st->stop >= low && st->stop <= st->melee;
                    snprintf(text, sizeof(text), "stopDistance=%s(%.2f yd, band [%.2f, %.2f])", inBand ? "OK" : "BUG", st->stop, low, st->melee);
                    std::string body = text;
                    if (!st->measured || !st->haveRelays)
                    {
                        body += " | noRelayWhileStanding=INVALID(the standing window was never read)";
                    }
                    else
                    {
                        snprintf(text, sizeof(text), " | noRelayWhileStanding=%s(%u re-lays at the stop, %u after 5 s of standing)",
                                 st->relaysAfter == st->relaysAtStop ? "OK" : "BUG", st->relaysAtStop, st->relaysAfter);
                        body += text;
                    }
                    if (!st->victimKept)
                    {
                        body += " | attacks=INVALID(the chase lost its victim: the ranged attack did not hold)";
                    }
                    else if (!st->reached)
                    {
                        body += " | attacks=INVALID(the wolf was never sampled inside the melee range)";
                    }
                    else if (st->bitEarly)
                    {
                        snprintf(text, sizeof(text), " | attacks=BUG(the melee bit was already set %.2f yd out, before the wolf was ever inside the %.2f yd melee range)",
                                 st->bitEarlyGap, st->melee);
                        body += text;
                    }
                    else if (!st->bitSet)
                    {
                        snprintf(text, sizeof(text), " | attacks=BUG(the wolf reached %.2f yd, inside the %.2f yd melee range, and the melee bit never appeared)",
                                 st->reachGap, st->melee);
                        body += text;
                    }
                    else
                    {
                        const uint32 lag = st->bitAt - st->reachAt;
                        snprintf(text, sizeof(text), " | attacks=%s(clear until the wolf was inside the %.2f yd melee range at %.2f yd, then set %u ms later)",
                                 lag <= 500 ? "OK" : "BUG", st->melee, st->reachGap, lag);
                        body += text;
                    }
                    Verdict(body);
                });
            }
        };

        /// Order 50: the follow's pace and band behind a running leader. The follower aims one
        /// 400 ms cadence ahead of a trusted velocity (design §6.2, Movement.FollowHorizonMs),
        /// which is what lets it hold a band at all against a leader of its own speed. The band
        /// is FollowBand's derivation from the native's own parameters, at the leader's measured
        /// speed -- not a constant, which would only ever be a bet on how fast the leader is.
        class FollowKeepsPace : public Scenario
        {
        public:
            FollowKeepsPace() : Scenario("follow-keeps-pace", 50) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float x, y, z; float dist; bool leaderMoving; };
                struct State
                {
                    uint32 leaderStop = 0;
                    bool   stopped = false;
                    float  band = 0.0f;      ///< FollowBand's derivation, at the leader's speed
                    std::string terms;       ///< and its terms, for the verdict text
                    float  rest = 0.0f;      ///< 1.0 + the two extents + 1.5
                    float  runSpeed = 0.0f;
                    float  endGap = -1.0f;
                    bool   endRead = false;
                };
                const float kLeg = 70.0f;
                Creature* leader = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                const float fx = SE.x - 2.0f, fy = SE.y + 0.8f;
                Creature* f = Spawn(WOLF, fx, fy, Ground(fx, fy, SE.z), 0.0f);
                if (!leader || !f)
                {
                    Verdict("closingPace=INVALID(spawn failed) | holdsBand=INVALID(spawn failed) | endsBehind=INVALID(spawn failed)");
                    return;
                }
                Silence(leader); Silence(f);     // neither wolf decides anything on its own: the legs here are the script's
                Park(leader); Park(f);           // no random wander back to the spawn when a leg ends
                leader->SetWalk(false, false);   // the leader runs; the follower mirrors its gait
                Load(SE.x + kLeg + 10.0f, SE.y);
                const ObjectGuid gl = leader->GetObjectGuid(), gf = f->GetObjectGuid();
                auto st = std::make_shared<State>();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(300, [this, gl, gf]()
                {
                    Creature* leader = Get(gl); Creature* f = Get(gf); if (!leader || !f) { return; }
                    f->GetMotionMaster()->MoveFollow(leader, FOLLOW_DIST, FOLLOW_ANGLE);
                    Log("MoveFollow(%.1f, %.3f) from %.2f yd, mt=%s", FOLLOW_DIST, FOLLOW_ANGLE, Dist3(f, leader), TypeName(f));
                });
                At(500, [this, gl, kLeg]()
                {
                    Creature* leader = Get(gl); if (!leader) { return; }
                    const float tx = SE.x + kLeg, ty = SE.y;
                    leader->GetMotionMaster()->MovePoint(1, tx, ty, Ground(tx, ty, SE.z), true);
                    Log("the leader runs %.0f yd (+x) at %.2f yd/s", kLeg, leader->GetSpeed(MOVE_RUN));
                });
                for (uint32 i = 1; i <= 70; ++i)
                {
                    At(500 + i * 500, [this, gl, gf, st, samples, i]()
                    {
                        Creature* leader = Get(gl); Creature* f = Get(gf); if (!leader || !f) { return; }
                        const uint32 t = 500 + i * 500;
                        Sample s;
                        s.t = t;
                        s.x = f->Where().X(); s.y = f->Where().Y(); s.z = f->Where().Z();
                        s.dist = Dist3(f, leader);
                        s.leaderMoving = !leader->movespline->Finalized();
                        samples->push_back(s);
                        // Derived from the native's own parameters, not fitted to the run: see
                        // FollowBand. A flat constant is not a band at all -- it is a bet on the
                        // leader's speed, and the re-lay cadence is 400 ms, not a second.
                        st->band = FollowBand(f, leader, leader->GetSpeed(MOVE_RUN), st->terms);
                        st->rest = FOLLOW_DIST + ExtentSum(f, leader) + 1.5f;
                        st->runSpeed = f->GetSpeed(MOVE_RUN);
                        if (!st->stopped && t >= 2000 && !s.leaderMoving)
                        {
                            st->stopped = true;
                            st->leaderStop = t;
                            Log("+%5ums the leader's leg finished %.1f yd from the start, the follower %.2f yd behind",
                                t, Dist2(leader->Where().X(), leader->Where().Y(), SE.x, SE.y), s.dist);
                        }
                        if (st->stopped && !st->endRead && t >= st->leaderStop + 2000)
                        {
                            st->endRead = true;
                            st->endGap = s.dist;
                            Log("+%5ums 2 s after the leader stopped: the follower rests %.2f yd behind (spline running=%d)",
                                t, s.dist, f->movespline->Finalized() ? 0 : 1);
                        }
                        if (i % 4 == 0)
                        {
                            Log("+%5ums gap %.2f yd, leader moving=%d, follower at %.1f %.1f", t, s.dist, s.leaderMoving ? 1 : 0, s.x, s.y);
                        }
                    });
                }
                At(36000, [this, st, samples]()
                {
                    if (samples->size() < 20)
                    {
                        Verdict("closingPace=INVALID(no samples) | holdsBand=INVALID(no samples) | endsBehind=INVALID(no samples)");
                        return;
                    }
                    // closingPace: the ground the follower actually covered over the middle
                    // six seconds of the leader's run, against its own run speed.
                    float travelled = 0.0f;
                    uint32 from = 0, to = 0;
                    for (size_t i = 0; i < samples->size(); ++i)
                    {
                        Sample const& s = (*samples)[i];
                        if (s.t < 3000 || s.t > 9000) { continue; }
                        if (!from) { from = s.t; }
                        else { travelled += Dist3(s.x, s.y, s.z, (*samples)[i - 1].x, (*samples)[i - 1].y, (*samples)[i - 1].z); }
                        to = s.t;
                    }
                    std::string body;
                    char text[512];
                    if (to <= from || st->runSpeed <= 0.0f)
                    {
                        body = "closingPace=INVALID(the pace window held no samples)";
                    }
                    else
                    {
                        const float pace = travelled / (float(to - from) / 1000.0f);
                        const float share = pace / st->runSpeed;
                        snprintf(text, sizeof(text), "closingPace=%s(%.2f yd/s over %.1f s, %.0f %% of the %.2f yd/s run speed)",
                                 share >= 0.90f ? "OK" : "BUG", pace, float(to - from) / 1000.0f, share * 100.0f, st->runSpeed);
                        body = text;
                    }
                    // holdsBand: from the third second until the leader stops.
                    float worst = 0.0f;
                    uint32 counted = 0;
                    for (size_t i = 0; i < samples->size(); ++i)
                    {
                        Sample const& s = (*samples)[i];
                        if (s.t < 3000 || (st->stopped && s.t > st->leaderStop)) { continue; }
                        worst = std::max(worst, s.dist);
                        ++counted;
                    }
                    if (!counted)
                    {
                        body += " | holdsBand=INVALID(the band window held no samples)";
                    }
                    else
                    {
                        snprintf(text, sizeof(text), " | holdsBand=%s(the gap peaked at %.2f yd over %u samples, band %.2f yd = %s)",
                                 worst <= st->band ? "OK" : "BUG", worst, counted, st->band, st->terms.c_str());
                        body += text;
                    }
                    if (!st->endRead)
                    {
                        body += " | endsBehind=INVALID(the leader never stopped)";
                    }
                    else
                    {
                        snprintf(text, sizeof(text), " | endsBehind=%s(%.2f yd behind 2 s after the leader stopped, rest band %.2f yd)",
                                 st->endGap <= st->rest ? "OK" : "BUG", st->endGap, st->rest);
                        body += text;
                    }
                    Verdict(body);
                });
            }
        };

        /// Order 51: the two facings a follow owns (design §6.2). At rest it copies the leader's
        /// -- the generator's own rule, and the one a pet's idle pose depends on -- and while a
        /// leg runs it asks for none at all, so the spline's travel facing stands. The second half
        /// is read through MotionMaster::SelectedLegFacingMode, because a sampled heading cannot
        /// tell the two apart: a walker faces its travel direction whether the leg asked for no
        /// facing or had the leader's own heading baked into it, and both predict the same
        /// samples. The heading is still measured, as the text, but the MODE is the claim.
        class FollowCopiesFacing : public Scenario
        {
        public:
            FollowCopiesFacing() : Scenario("follow-copies-facing", 51) {}

            void Prepare() override
            {
                struct State
                {
                    bool   faced = false;
                    uint32 facedAt = 0;
                    uint32 lastMoving = 0;    ///< the last sample at which the follower's spline ran
                    bool   haveRest = false;
                    float  restFacing = 0.0f;
                    uint32 restAt = 0;
                    uint32 legChecks = 0;      ///< samples taken while the follower's own spline ran
                    uint32 legMisses = 0;      ///< of those, the ones whose leg asked for a facing
                    Motion::Facing::Mode legWorstMode = Motion::Facing::Mode::None;
                    float  legWorst = 0.0f;    ///< and, for the text, how far its heading was off its travel
                    float  legWorstFacing = 0.0f;
                    float  legWorstTravel = 0.0f;
                    float  px = 0.0f, py = 0.0f;
                    bool   havePrev = false;
                    uint32 restChecks = 0;     ///< samples taken on the hold after the last leg
                    uint32 restAngle = 0;      ///< of those, the ones holding the leader's heading
                    uint32 turnSamples = 0;    ///< samples on a spline that turned the unit without moving it
                    Motion::Facing::Mode restMode = Motion::Facing::Mode::None;
                    float  restLeaderFacing = 0.0f;
                };
                // The four modes, for a verdict that reads as words.
                auto modeName = [](Motion::Facing::Mode m) -> char const*
                {
                    switch (m)
                    {
                        case Motion::Facing::Mode::None:   return "None";
                        case Motion::Facing::Mode::Angle:  return "Angle";
                        case Motion::Facing::Mode::Target: return "Target";
                        case Motion::Facing::Mode::Spot:   return "Spot";
                    }
                    return "?";
                };
                const float kLeg = 15.0f, kFace = 1.2f;
                Creature* leader = Spawn(CHICKEN, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                // Twelve yards back along the follow angle, not three: a follower that starts
                // inside its own band never lays a leg long enough to read a travel facing off
                // (the first cut of this scenario got four usable samples out of a whole run).
                const float fx = SE.x + 12.0f * std::cos(FOLLOW_ANGLE), fy = SE.y + 12.0f * std::sin(FOLLOW_ANGLE);
                Creature* f = Spawn(WOLF, fx, fy, Ground(fx, fy, SE.z), 0.0f);
                if (!leader || !f)
                {
                    Verdict("facesLikeTheLeader=INVALID(spawn failed) | travelFacingOnLegs=INVALID(spawn failed)");
                    return;
                }
                Silence(leader); Silence(f);
                Park(leader); Park(f);
                WalkPace(leader);   // a walker in both senses: the flag the follower mirrors and the pace it mirrors
                const ObjectGuid gl = leader->GetObjectGuid(), gf = f->GetObjectGuid();
                auto st = std::make_shared<State>();
                At(300, [this, gl, gf]()
                {
                    Creature* leader = Get(gl); Creature* f = Get(gf); if (!leader || !f) { return; }
                    f->GetMotionMaster()->MoveFollow(leader, FOLLOW_DIST, FOLLOW_ANGLE);
                    Log("MoveFollow behind the chicken from %.2f yd, mt=%s", Dist3(f, leader), TypeName(f));
                });
                At(500, [this, gl, kLeg]()
                {
                    Creature* leader = Get(gl); if (!leader) { return; }
                    const float tx = SE.x + kLeg, ty = SE.y;
                    leader->GetMotionMaster()->MovePoint(1, tx, ty, Ground(tx, ty, SE.z), true);
                    Log("the chicken walks %.0f yd (+x) at %.2f yd/s", kLeg, leader->GetSpeed(MOVE_WALK));
                });
                for (uint32 i = 1; i <= 100; ++i)
                {
                    At(500 + i * 250, [this, gl, gf, st, i, kFace, modeName]()
                    {
                        Creature* leader = Get(gl); Creature* f = Get(gf); if (!leader || !f) { return; }
                        const uint32 t = 500 + i * 250;
                        if (!st->faced && t >= 1500 && leader->movespline->Finalized())
                        {
                            leader->SetFacingTo(kFace);
                            st->faced = true;
                            st->facedAt = t;
                            Log("+%5ums the chicken arrived and turned to %.3f rad (its facing reads %.3f)", t, kFace, leader->Where().Facing());
                        }
                        const bool moving = !f->movespline->Finalized();
                        const float x = f->Where().X(), y = f->Where().Y();
                        const float facing = f->Where().Facing();
                        // What the follow ASKED the driver for, not what the spline happens to look
                        // like: a walker faces where it is going either way, so a sampled heading
                        // cannot tell "no facing on the leg" from "the leader's heading baked into
                        // it". The facade read can.
                        const Motion::Facing::Mode mode = f->GetMotionMaster()->SelectedLegFacingMode();
                        if (moving)
                        {
                            st->lastMoving = t;
                            // A running spline is not necessarily a LEG: the hold's own facing is
                            // applied by launching a spline that turns the unit without moving it,
                            // and that one is supposed to carry mode Angle. A leg sample is one
                            // whose spline still has somewhere to go. The "and it covered ground
                            // since the last sample" half of this test is gone: it was only ever
                            // there because the driver used to record the facing of an intent it
                            // had not acted on, so the tick a leg ended on read the hold that had
                            // already replaced it. The driver records what it LAUNCHED now, so
                            // having somewhere to go is the whole test.
                            const Movement::Vector3 goal = f->movespline->FinalDestination();
                            const bool covered = Dist2(goal.x, goal.y, x, y) >= 0.5f;
                            if (!covered)
                            {
                                ++st->turnSamples;   // the hold's own turn-in-place spline
                            }
                            else if (covered)
                            {
                                ++st->legChecks;
                                if (mode != Motion::Facing::Mode::None)
                                {
                                    ++st->legMisses;
                                    st->legWorstMode = mode;
                                }
                                // The heading against its own travel is still reported, and THAT one
                                // does need two samples half a yard apart to have a bearing at all.
                                if (st->havePrev && Dist2(x, y, st->px, st->py) >= 0.5f)
                                {
                                    const float travel = Bearing(st->px, st->py, x, y);
                                    const float off = AngleDiff(facing, travel);
                                    if (off > st->legWorst) { st->legWorst = off; st->legWorstFacing = facing; st->legWorstTravel = travel; }
                                    Log("+%5ums on a leg: facing mode %s, facing %.3f, travelling %.3f, off by %.3f rad", t, modeName(mode), facing, travel, off);
                                }
                            }
                            st->px = x; st->py = y; st->havePrev = true;
                        }
                        else
                        {
                            st->havePrev = false;
                            if (st->faced && t >= st->facedAt + 2000 && st->lastMoving && t >= st->lastMoving + 2000)
                            {
                                st->haveRest = true;
                                st->restFacing = facing;
                                st->restAt = t;
                                st->restLeaderFacing = leader->Where().Facing();
                                st->restMode = mode;
                                ++st->restChecks;
                                if (mode == Motion::Facing::Mode::Angle) { ++st->restAngle; }
                            }
                        }
                    });
                }
                At(26500, [this, st, modeName]()
                {
                    std::string body;
                    char text[512];
                    if (!st->haveRest)
                    {
                        body = "facesLikeTheLeader=INVALID(the follower never rested 2 s after the leader turned)";
                    }
                    else
                    {
                        // Against the leader's LIVE heading, not the constant the script asked for:
                        // the claim is "it copies the leader", and only the leader can say what
                        // that is.
                        const float off = AngleDiff(st->restFacing, st->restLeaderFacing);
                        snprintf(text, sizeof(text), "facesLikeTheLeader=%s(facing %.3f at +%u ms, %.3f rad from the leader's live %.3f)",
                                 off <= 0.26f ? "OK" : "BUG", st->restFacing, st->restAt, off, st->restLeaderFacing);
                        body = text;
                    }
                    if (!st->legChecks || !st->restChecks)
                    {
                        snprintf(text, sizeof(text), " | travelFacingOnLegs=INVALID(%u samples on a leg, %u on the hold after it)",
                                 st->legChecks, st->restChecks);
                        body += text;
                    }
                    else
                    {
                        const bool ok = st->legMisses == 0 && st->restAngle == st->restChecks;
                        snprintf(text, sizeof(text), " | travelFacingOnLegs=%s(the leg asked for facing mode None at %u of %u samples whose spline still had somewhere to go, and mode Angle at %u of %u on the hold after it, last read %s; %u further samples were the hold's own turn-in-place spline; the heading was at most %.3f rad off its travel, %.3f against %.3f)",
                                 ok ? "OK" : "BUG", st->legChecks - st->legMisses, st->legChecks,
                                 st->restAngle, st->restChecks, modeName(st->restMode), st->turnSamples,
                                 st->legWorst, st->legWorstFacing, st->legWorstTravel);
                        body += text;
                    }
                    Verdict(body);
                });
            }
        };

        /// Order 52: the evade that waits. A root lands on a creature sent home, and the home
        /// native's first tick -- the one that clears the dynamic states -- must not come until
        /// the root lifts (design §6.5), because that clear would otherwise erase the block's
        /// own mirrors. Then exactly one leg carries it home and the arrival recipe runs once.
        /// A second wolf is displaced under the same root (MoveIdle while it still holds): its
        /// home never arrived, so JustReachedHome must never be called for it at all.
        class RootedEvadeWaits : public Scenario
        {
        public:
            RootedEvadeWaits() : Scenario("rooted-evade-waits", 52) {}

            void Prepare() override
            {
                struct Wolf
                {
                    bool   out = false;        ///< the 30 yd leg finished and the root + home went in
                    uint32 homeAt = 0;
                    bool   idled = false;
                    bool   lifted = false;
                    uint32 liftAt = 0;
                    size_t liftMark = 0;       ///< how many informs the run had seen when the root lifted
                    uint32 rootSamples = 0;
                    uint32 auraSamples = 0;
                    uint32 mirrorMisses = 0;
                    uint32 legsUnderRoot = 0;
                    std::vector<Pt> underRoot;
                    bool   wasRunning = false;
                    uint32 legsAfter = 0;
                    float  homeGap = 999.0f;
                };
                const float kOut = 30.0f;
                const Pt SEB = { SE.x, SE.y + 40.0f, SE.z };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(WOLF, SEB.x, SEB.y, Ground(SEB.x, SEB.y, SEB.z), 0.0f);
                if (!a || !b)
                {
                    Verdict("noLegUnderRoot=INVALID(spawn failed) | rootMirrorKept=INVALID(spawn failed) | oneLegAfterLift=INVALID(spawn failed) | reachedHomeOnce=INVALID(spawn failed) | noRecipeOnDisplacement=INVALID(spawn failed)");
                    return;
                }
                Silence(a); Silence(b);   // the home arrival still records: HarnessAI books it before it forwards
                Park(a); Park(b);   // the default is a wander around the spawn; the home must be the only thing that goes there
                Load(SE.x + kOut + 10.0f, SE.y);
                const ObjectGuid ga = a->GetObjectGuid(), gb = b->GetObjectGuid();
                const uint32 lowA = a->GetGUIDLow(), lowB = b->GetGUIDLow();
                auto wa = std::make_shared<Wolf>();
                auto wb = std::make_shared<Wolf>();
                const size_t mark = Informs().size();
                At(400, [this, ga, gb, kOut, SEB]()
                {
                    Creature* a = Get(ga); Creature* b = Get(gb); if (!a || !b) { return; }
                    a->GetMotionMaster()->MovePoint(1, SE.x + kOut, SE.y, Ground(SE.x + kOut, SE.y, SE.z), true);
                    b->GetMotionMaster()->MovePoint(1, SEB.x + kOut, SEB.y, Ground(SEB.x + kOut, SEB.y, SEB.z), true);
                    Log("both wolves walk %.0f yd out of their spawn", kOut);
                });
                for (uint32 i = 1; i <= 125; ++i)   // 200 ms * 125 = 25 s
                {
                    At(400 + i * 200, [this, ga, gb, wa, wb, i]()
                    {
                        const uint32 t = 400 + i * 200;
                        Creature* a = Get(ga); Creature* b = Get(gb); if (!a || !b) { return; }
                        struct Pair { Creature* c; std::shared_ptr<Wolf> w; char const* name; bool idles; };
                        Pair pairs[2] = { { a, wa, "A", false }, { b, wb, "B", true } };
                        for (uint32 p = 0; p < 2; ++p)
                        {
                            Creature* c = pairs[p].c;
                            Wolf& s = *pairs[p].w;
                            const bool running = !c->movespline->Finalized();
                            if (!s.out)
                            {
                                if (t >= 1400 && !running)
                                {
                                    s.out = true;
                                    s.homeAt = t;
                                    c->CastSpell(c, ROOT, true);
                                    c->GetMotionMaster()->MoveTargetedHome();
                                    Log("+%5ums %s arrived %.1f yd out: Web cast and MoveTargetedHome in the same step (rooted=%d, mt=%s)",
                                        t, pairs[p].name, Dist2(c->Where().X(), c->Where().Y(), c->Spawn().X(), c->Spawn().Y()),
                                        c->Blocked(Motion::ReasonRooted) ? 1 : 0, TypeName(c));
                                }
                                s.wasRunning = running;
                                continue;
                            }
                            if (pairs[p].idles && !s.idled && t >= s.homeAt + 1000)
                            {
                                s.idled = true;
                                c->GetMotionMaster()->MoveIdle();
                                Log("+%5ums B goes idle under the root: its home is displaced, never arrived (rooted=%d, mt=%s)",
                                    t, c->Blocked(Motion::ReasonRooted) ? 1 : 0, TypeName(c));
                            }
                            const bool rooted = c->Blocked(Motion::ReasonRooted);
                            const bool aura = c->HasAura(ROOT);
                            if (aura)
                            {
                                ++s.auraSamples;
                                if (!rooted) { ++s.mirrorMisses; }
                            }
                            if (rooted)
                            {
                                ++s.rootSamples;
                                Pt here = { c->Where().X(), c->Where().Y(), c->Where().Z() };
                                s.underRoot.push_back(here);
                                if (running) { ++s.legsUnderRoot; }
                            }
                            else if (!s.lifted && s.rootSamples)
                            {
                                s.lifted = true;
                                s.liftAt = t;
                                s.liftMark = Informs().size();
                                Log("+%5ums %s: the root lifted after %u samples, mt=%s, %.1f yd from home",
                                    t, pairs[p].name, s.rootSamples, TypeName(c),
                                    Dist2(c->Where().X(), c->Where().Y(), c->Spawn().X(), c->Spawn().Y()));
                            }
                            if (s.lifted)
                            {
                                if (running && !s.wasRunning) { ++s.legsAfter; }
                                s.homeGap = std::min(s.homeGap, Dist2(c->Where().X(), c->Where().Y(), c->Spawn().X(), c->Spawn().Y()));
                            }
                            s.wasRunning = running;
                            if (i % 5 == 0)
                            {
                                Log("+%5ums %s rooted=%d aura=%d spline=%d mt=%s at %.1f %.1f",
                                    t, pairs[p].name, rooted ? 1 : 0, aura ? 1 : 0, running ? 1 : 0, TypeName(c), c->Where().X(), c->Where().Y());
                            }
                        }
                    });
                }
                At(26000, [this, wa, wb, lowA, lowB, mark]()
                {
                    std::string body;
                    char text[512];
                    if (!wa->rootSamples)
                    {
                        body = "noLegUnderRoot=INVALID(A was never rooted) | rootMirrorKept=INVALID(A was never rooted) | oneLegAfterLift=INVALID(A was never rooted) | reachedHomeOnce=INVALID(A was never rooted)";
                    }
                    else
                    {
                        const float spread = Spread(wa->underRoot);
                        snprintf(text, sizeof(text), "noLegUnderRoot=%s(stood within %.2f yd over %u rooted samples, %u of them with a spline running)",
                                 (spread < 0.5f && !wa->legsUnderRoot) ? "OK" : "BUG", spread, wa->rootSamples, wa->legsUnderRoot);
                        body = text;
                        snprintf(text, sizeof(text), " | rootMirrorKept=%s(the root state held at %u of the %u samples the Web was on, about %.1f s)",
                                 wa->mirrorMisses ? "BUG" : "OK", wa->auraSamples - wa->mirrorMisses, wa->auraSamples, wa->auraSamples * 0.2f);
                        body += text;
                        if (!wa->lifted)
                        {
                            body += " | oneLegAfterLift=INVALID(the root never lifted) | reachedHomeOnce=INVALID(the root never lifted)";
                        }
                        else
                        {
                            snprintf(text, sizeof(text), " | oneLegAfterLift=%s(%u leg%s after the lift, closing to %.2f yd from home)",
                                     (wa->legsAfter == 1 && wa->homeGap < 2.0f) ? "OK" : "BUG", wa->legsAfter, wa->legsAfter == 1 ? "" : "s", wa->homeGap);
                            body += text;
                            uint32 before = 0, after = 0, atHome = 0;
                            for (size_t i = mark; i < Informs().size(); ++i)
                            {
                                Inform const& r = Informs()[i];
                                if (r.guidLow != lowA || r.event != Inform::Event::ReachedHome) { continue; }
                                if (i < wa->liftMark) { ++before; }
                                else { ++after; }
                                if (Dist2(r.x, r.y, SE.x, SE.y) <= 2.0f) { ++atHome; }
                            }
                            snprintf(text, sizeof(text), " | reachedHomeOnce=%s(%u home inform%s after the lift, %u before it, %u of them within 2 yd of home)",
                                     (after == 1 && !before && atHome == 1) ? "OK" : "BUG", after, after == 1 ? "" : "s", before, atHome);
                            body += text;
                        }
                    }
                    uint32 bHome = 0;
                    for (size_t i = mark; i < Informs().size(); ++i)
                    {
                        Inform const& r = Informs()[i];
                        if (r.guidLow == lowB && r.event == Inform::Event::ReachedHome) { ++bHome; }
                    }
                    snprintf(text, sizeof(text), " | noRecipeOnDisplacement=%s(%u home informs for the wolf that went idle under the root)",
                             bHome ? "BUG" : "OK", bHome);
                    body += text;
                    Verdict(body);
                });
            }
        };

        /// Order 53: a leader that changes the KIND of its movement under the follower. A jump
        /// is a parabola, so its velocity is never trusted (TargetKinematics: an airborne spline
        /// is moving but untrusted) and the follow's horizon must switch itself off rather than
        /// lead a follower into thin air. The horizon is read where it shows: a spot led by
        /// 400 ms of a 7.5 yd/s jump would be laid about two yards IN FRONT of the leader, and
        /// an unled one is always behind it, whatever the lag. The slow leg that comes after is
        /// a plain linear one again, and the follower closes on it and holds the same derived band
        /// order 50 uses. `noWildGoal` reads the GOALS the follow derives, not the gap that happens
        /// to be open: a follower can be standing right behind its leader while the native lays a
        /// spot halfway across the map, and a gap measurement would never know.
        class FollowOnMovingTargetKinds : public Scenario
        {
        public:
            FollowOnMovingTargetKinds() : Scenario("follow-on-moving-target-kinds", 53) {}

            void Prepare() override
            {
                struct State
                {
                    // noWildGoal: the GOAL the follower laid, against where the leader actually is
                    float  worstGoal = 0.0f;
                    uint32 goalChecks = 0;
                    uint32 worstGoalAt = 0;
                    float  worstGap = 0.0f;      ///< the plain follower-to-leader gap, for the text
                    uint32 airSamples = 0;
                    uint32 airRelays = 0;         ///< how many fresh spots the follower derived while the leader was in the air
                    uint32 lastTotal = 0;
                    bool   haveTotal = false;
                    float  airAheadWorst = -999.0f;///< how far IN FRONT of the leader a FRESH leg goal was laid, along the jump's axis
                    float  airLead = 0.0f;        ///< what counts as led: the follow's own standing distance and a little slack
                    uint32 airGoalChecks = 0;
                    uint32 walkAt = 0;
                    bool   walkStopped = false;
                    uint32 walkStop = 0;
                    float  walkWorst = 0.0f;
                    uint32 walkChecks = 0;
                    float  walkBand = 0.0f;
                    std::string walkTerms;
                };
                const float kJump = 10.0f, kWalk = 20.0f;
                Creature* leader = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                const float fx = SE.x - 2.5f, fy = SE.y + 1.0f;
                Creature* f = Spawn(WOLF, fx, fy, Ground(fx, fy, SE.z), 0.0f);
                if (!leader || !f)
                {
                    Verdict("noWildGoal=INVALID(spawn failed) | horizonOffWhileAirborne=INVALID(spawn failed) | keepsUpOnTheWalk=INVALID(spawn failed)");
                    return;
                }
                Silence(leader); Silence(f);
                Park(leader); Park(f);
                leader->SetWalk(false, false);   // it runs until the walk phase, so the follower may close the jump's gap
                Load(SE.x + kJump + kWalk + 10.0f, SE.y);
                const ObjectGuid gl = leader->GetObjectGuid(), gf = f->GetObjectGuid();
                auto st = std::make_shared<State>();
                At(300, [this, gl, gf]()
                {
                    Creature* leader = Get(gl); Creature* f = Get(gf); if (!leader || !f) { return; }
                    f->GetMotionMaster()->MoveFollow(leader, FOLLOW_DIST, FOLLOW_ANGLE);
                    Log("MoveFollow from %.2f yd, mt=%s", Dist3(f, leader), TypeName(f));
                });
                At(500, [this, gl, kJump]()
                {
                    Creature* leader = Get(gl); if (!leader) { return; }
                    const float tx = SE.x + kJump, ty = SE.y;
                    leader->GetMotionMaster()->MoveJump(tx, ty, Ground(tx, ty, SE.z), 7.5f, 5.0f, 53);
                    Log("the leader jumps %.0f yd (+x)", kJump);
                });
                At(2500, [this, gl, st, kJump, kWalk]()
                {
                    Creature* leader = Get(gl); if (!leader) { return; }
                    const float tx = SE.x + kJump + kWalk, ty = SE.y;
                    WalkPace(leader);
                    leader->GetMotionMaster()->MovePoint(1, tx, ty, Ground(tx, ty, SE.z), true);
                    st->walkAt = 2500;
                    Log("the leader now walks %.0f yd (+x) at %.2f yd/s", kWalk, leader->GetSpeed(MOVE_WALK));
                });
                // Both goal reads run on the harness's own 100 ms tick and only on a FRESH leg --
                // the sample where the follower's re-lay total has just gone up, so the goal is at
                // most one tick old. That matters twice over. `noWildGoal` is about the goal the
                // follow DERIVES, not the gap that happens to be open: a gap can stay small while
                // the native lays a spot into the next postcode, and the plain gap could never have
                // caught that. And the horizon only shows on a fresh goal: one derived a second ago
                // has had the leader move 7.5 yd out from under it, which swamps the 3 yd a 400 ms
                // lead on a 7.5 yd/s jump would add. At one tick old the two cases do not overlap:
                // unled, the spot sits within the follow's standing distance of where the leader IS,
                // minus up to 0.75 yd of tick lag; led, it would sit 3 yd further along the jump, so
                // between 1.25 and 4 yd in front. The gate is the standing distance and half a yard
                // of slack for the free-spot search, which may rotate the spot around that ring.
                for (uint32 i = 1; i <= 250; ++i)
                {
                    At(500 + i * 100, [this, gl, gf, st, i]()
                    {
                        Creature* leader = Get(gl); Creature* f = Get(gf); if (!leader || !f) { return; }
                        const uint32 t = 500 + i * 100;
                        const bool leaderMoving = !leader->movespline->Finalized();
                        const bool airborne = leaderMoving && leader->movespline->Airborne();
                        if (airborne)
                        {
                            ++st->airSamples;
                            st->airLead = FOLLOW_DIST + ExtentSum(f, leader) + 0.5f;
                        }
                        Motion::RelayCounts const* rc = Relays(f);
                        if (!rc) { return; }
                        const uint32 total = rc->Total();
                        const bool fresh = st->haveTotal && total > st->lastTotal;
                        st->lastTotal = total; st->haveTotal = true;
                        // Same caveat as order 48's read: the counter is the native's, incremented in
                        // Derive before MotionDriver::ReconcileMove decides, so a spot under the
                        // driver's 0.5 yd MIN_RELAY_DISTANCE leaves the previous leg running and this
                        // goal one re-lay floor stale. Half a yard against a 15 yd ceiling and a
                        // 1.50 yd horizon gate: reported, no verdict effect.
                        if (!fresh || f->movespline->Finalized()) { return; }
                        // Where the leader IS: its spline position while one runs (mid-jump that is
                        // metres from where its placement was last written), its placement otherwise.
                        Movement::Location live(leader->Where().X(), leader->Where().Y(), leader->Where().Z(), leader->Where().Facing());
                        if (leaderMoving) { live = leader->movespline->ComputePosition(); }
                        const Movement::Vector3 goal = f->movespline->FinalDestination();
                        const float goalGap = Dist3(goal.x, goal.y, goal.z, live.x, live.y, live.z);
                        if (goalGap > st->worstGoal) { st->worstGoal = goalGap; st->worstGoalAt = t; }
                        ++st->goalChecks;
                        if (airborne)
                        {
                            ++st->airRelays;
                            // The jump runs along +x, so the signed x offset IS the along-axis one:
                            // positive means the goal was laid in FRONT of where the leader is.
                            const float ahead = goal.x - live.x;
                            st->airAheadWorst = std::max(st->airAheadWorst, ahead);
                            ++st->airGoalChecks;
                            Log("+%5ums the leader is airborne and the follower just laid a leg: the goal is %.2f yd along the jump from the leader's live spot (led would be %.2f or more), %.2f yd from it in all",
                                t, ahead, st->airLead, goalGap);
                        }
                    });
                }
                for (uint32 i = 1; i <= 100; ++i)
                {
                    At(500 + i * 250, [this, gl, gf, st, i]()
                    {
                        Creature* leader = Get(gl); Creature* f = Get(gf); if (!leader || !f) { return; }
                        const uint32 t = 500 + i * 250;
                        const float gap = Dist3(f, leader);
                        st->worstGap = std::max(st->worstGap, gap);
                        Motion::RelayCounts const* rc = Relays(f);
                        // From the slow leg's third second on: what the follower must hold once
                        // the leader is back on the ground and moving at a walking pace.
                        if (st->walkAt && t >= st->walkAt + 3000 && !st->walkStopped)
                        {
                            st->walkWorst = std::max(st->walkWorst, gap);
                            // The same derivation order 50 holds its running leader to, at the
                            // speed this one is actually travelling at.
                            st->walkBand = FollowBand(f, leader, leader->GetSpeed(MOVE_RUN), st->walkTerms);
                            ++st->walkChecks;
                        }
                        if (st->walkAt && !st->walkStopped && t >= st->walkAt + 2000 && leader->movespline->Finalized())
                        {
                            st->walkStopped = true;
                            st->walkStop = t;
                            Log("+%5ums the leader's walk finished, the follower %.2f yd behind", t, gap);
                        }
                        if (i % 4 == 0)
                        {
                            Log("+%5ums gap %.2f yd, leader walking=%d spline=%d, follower walking=%d re-lays %s",
                                t, gap, leader->IsWalking() ? 1 : 0, leader->movespline->Finalized() ? 0 : 1,
                                f->IsWalking() ? 1 : 0, rc ? Causes(*rc).c_str() : "(the follow is not selected)");
                        }
                    });
                }
                At(26500, [this, st]()
                {
                    char text[512];
                    std::string body;
                    if (!st->goalChecks)
                    {
                        body = "noWildGoal=INVALID(the follower laid no leg over the whole run)";
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "noWildGoal=%s(the farthest of %u freshly laid leg goals sat %.2f yd from where the leader was at +%u ms; the plain gap peaked at %.2f yd)",
                                 st->worstGoal <= 15.0f ? "OK" : "BUG", st->goalChecks, st->worstGoal, st->worstGoalAt, st->worstGap);
                        body = text;
                    }
                    if (!st->airSamples)
                    {
                        body += " | horizonOffWhileAirborne=INVALID(the leader was never sampled airborne)";
                    }
                    else if (!st->airGoalChecks)
                    {
                        body += " | horizonOffWhileAirborne=INVALID(no fresh leg goal was laid while the leader was airborne: nothing to read a horizon off)";
                    }
                    else
                    {
                        snprintf(text, sizeof(text), " | horizonOffWhileAirborne=%s(%u fresh leg goals over %u airborne ticks reached at most %.2f yd along the jump from the leader's live spot, under the %.2f yd a led spot would have needed)",
                                 st->airAheadWorst <= st->airLead ? "OK" : "BUG",
                                 st->airGoalChecks, st->airSamples, st->airAheadWorst, st->airLead);
                        body += text;
                    }
                    if (!st->walkChecks)
                    {
                        body += " | keepsUpOnTheWalk=INVALID(the walk phase held no samples)";
                    }
                    else
                    {
                        snprintf(text, sizeof(text), " | keepsUpOnTheWalk=%s(the gap peaked at %.2f yd over %u samples of the walking-pace leg, band %.2f yd = %s)",
                                 st->walkWorst <= st->walkBand ? "OK" : "BUG", st->walkWorst, st->walkChecks, st->walkBand, st->walkTerms.c_str());
                        body += text;
                    }
                    Verdict(body);
                });
            }
        };
    }

    void RegisterTrackingScenarios(Runner& r)
    {
        r.Register(new ChaseRelayBudget());
        r.Register(new ChaseStopsInsideReach());
        r.Register(new FollowKeepsPace());
        r.Register(new FollowCopiesFacing());
        r.Register(new RootedEvadeWaits());
        r.Register(new FollowOnMovingTargetKinds());
    }
}
