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
#include "Utilities/MathDefines.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ===========================================================================================
// The chase against a target that really moves (orders 68-71).
//
// The chase had no moving-target net at all: `chase-relay-budget` (order 48) moves its target
// by teleport steps, and WALKS it through the phase its re-lay budget is read on. These four
// put a creature on a scripted path at run speed and watch the chase hold it through a straight
// run, a dead stop, a mid-leg 180 and a 12 yd ring.
//
// WHAT THEY WERE BUILT FOR, AND WHAT THEY DECIDED. `Movement.ChaseLead` shipped default-off,
// aiming the chase half a second ahead of a trusted velocity, with a conf note refusing it the
// production aim "until the numbers beat retail's cadence without overshooting a stop, a
// reversal or a circle". These scenarios produced those numbers -- each motion run twice over
// the same ground, lead off then on -- and the lead became the aim, unconditionally, with the
// switch deleted (ChaseBehaviour::AimCentre carries the full table and the reasoning, including
// the pre-registered rule it was overruled against). So there is one aim now and one pass each,
// and what stays behind is the net.
//
// The headline the flip was made on, and the reason `chase-moving-steady` asserts CONTACT:
// from a 10 yd abeam start the un-led chase never got inside the melee band in thirty seconds,
// while the led one was in contact at 2.9 s. That number is the behaviour this family exists to
// protect, so it is a gate and not a printed aside.
//
// "Beat retail" meant "beat lead-off" -- lead-off was retail's aim, and no retail moving-target
// chase capture exists. The corpus holds one long leg at a stationary-ish target
// (peer/retail-fear-movement-2026-09-20.md) and nothing against a runner, so every delta that
// decided this was measured against this same core with the flag off, and nothing was ever
// measured against Blizzard's server.
//
// The four motions:
//
//   steady    a 30 s straight run; the chaser opens 10 yd abeam, so there is an approach to time.
//   stop      10 s of the same run, then the leg ends and the target stands 10 s.
//   reversal  10 s of it, then a 180 deg turn mid-leg (the new leg replaces the running one,
//             so there is no standing moment in between) and 10 s back.
//   circle    a 12 yd ring for 30 s, re-aimed 24 deg ahead every 700 ms.
//
// THE PAIR, AND WHY ITS SPEEDS ARE SET. The target is a WOLF and the chaser a KOBOLD, and both
// creature templates happen to run at 6.00 yd/s -- equal speeds, which is no chase at all: the
// first cut of this family measured a chaser that could never close and sat 8.3 yd behind for
// thirty seconds. The chaser is therefore given the client's own base run rate
// (SetSpeedRate(MOVE_RUN, 1.0f) = 7.00 yd/s) and the target keeps its template's 6.00, which is
// retail's own margin between a mob and a running player, and the smallest margin that makes
// the aim -- rather than the speed deficit -- the thing being measured. Both are Silenced and
// Parked, as everywhere in the tracking family, so nothing but this file moves them. The
// corridor is long-point's (ScenariosPoint.cpp A4 -> B4): 372 yd a wolf is proven to cross in a
// single leg, rolling by about 10 yd over the 180 the straight run needs.
//
// OVERSHOOT, per motion, in the frame that makes it mean something. The measure is the chaser
// going PAST and having to come back, and it is taken against the point the target stopped or
// turned at rather than against the live target -- because after a reversal the target runs back
// past its chaser BY DESIGN, and a live-target test would score that unavoidable crossing as a
// bug:
//
//   steady    the chaser's projection past the target along the run direction.
//   stop      the chaser's projection past the POINT THE TARGET STOPPED AT.
//   reversal  the chaser's projection past the POINT THE TARGET TURNED AT.
//   circle    a freshly laid leg goal farther from the ring's centre than radius + band.
//
// A sample -- for the circle, a leg -- counts as an overshoot when the measure exceeds the
// pair's client melee range, which is the contact band the chase itself is built on. The worst
// measure is reported beside the count either way. The half-second lead overshoots none of
// them: it crosses a dead stop by 0.65 yd and a turn point by 1.89, both well inside the 5 yd
// band, because this kernel's target velocity is never stale about the PRESENT -- a stop
// finalizes the spline (untrusted, the lead disengages that tick) and a turn replaces it (the
// chord flips the same tick).
//
// THERE IS NO PATH RATIO HERE, ON PURPOSE. An earlier cut scored laid path over target path.
// Over a thirty-second straight run a chaser's displacement IS its target's, so that ratio sat
// at 1.013 whatever the aim did -- a quantity whose best case is a tie is a metric that can only
// be lost, and it very nearly retired an aim that was winning everywhere else. The two path
// lengths are still printed as evidence of wasted motion; they are not scored. What IS scored
// is what a chase is for: the re-lay budget, the overshoot, and whether it gets its target.
//
// Each scenario's verdict is two categories:
//   relayBudget  design v2 section 11's number: at most one ROUTINE re-lay per second over the
//                moving phase, with every cause and the total rate printed beside it.
//   aim          zero overshoot samples, contact made inside the melee band within the motion's
//                own deadline, and the chase holding its victim and its selection throughout;
//                the gap, the time outside the band and the two path lengths ride in the text.
// ===========================================================================================

namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;      ///< the target: 6.00 yd/s at its template's run rate
        const uint32 KOBOLD = 6;     ///< the chaser, at the base run rate: 7.00 yd/s

        /// long-point's corridor (ScenariosPoint.cpp): 372 yd of ground a wolf is proven to
        /// cross in a single leg.
        const Pt CORR_A = { -3257.5f, -351.6f, 47.8f };
        const Pt CORR_B = { -2891.0f, -416.7f, 47.9f };

        const float kLaneIn = 40.0f;       ///< how far in from the corridor's end the line starts
        const float kRadius = 12.0f;       ///< the ring the circle motion runs
        const float kAbeam = 10.0f;        ///< the steady motion's opening offset, across the run
        const float kBehind = 3.0f;        ///< the other three motions' opening gap, inside melee range

        /// A planar direction: the corridor's own, and the perpendicular the abeam start sits on.
        struct Dir { float x, y; };

        float Dot(Dir const& d, float x, float y) { return (d.x * x) + (d.y * y); }

        /// Three dimensions, as the engage effect measures its reach (NativeBehaviour.cpp).
        float Dist3(Creature* a, Creature* b)
        {
            const float dx = a->Where().X() - b->Where().X();
            const float dy = a->Where().Y() - b->Where().Y();
            const float dz = a->Where().Z() - b->Where().Z();
            return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
        }

        /// The client's own melee range for the pair: the chase's contact band.
        float MeleeRange(Creature* a, Creature* b)
        {
            const float reachSum = a->GetFloatValue(UNIT_FIELD_COMBATREACH) + b->GetFloatValue(UNIT_FIELD_COMBATREACH);
            return std::max(reachSum + 4.0f / 3.0f, 5.0f);
        }

        /// "routine 12, cut 1, partial 0, blocked 0, finished 3, first 1 (total 17)".
        std::string Causes(Motion::RelayCounts const& c)
        {
            char text[160];
            snprintf(text, sizeof(text), "routine %u, cut %u, partial %u, blocked %u, finished %u, first %u (total %u)",
                     c.routine, c.cut, c.partial, c.blocked, c.finished, c.first, c.Total());
            return text;
        }

        /// A creature's own default movement is a random wander around its spawn, so the moment a
        /// scripted leg ends it heads back there. Naming the default idle and re-initialising
        /// makes the factory native IdleBehaviour, and nothing at all moves the actor except this
        /// file. ScenariosTracking.cpp has the same three lines behind its own anonymous
        /// namespace, which this file cannot reach.
        void Park(Creature* c)
        {
            c->SetDefaultMovementType(CREATURE_MOVEMENT_IDLE);
            c->GetMotionMaster()->Initialize();
        }

        /// The gate the chase's lead hangs off, so the share of the phase it holds for a
        /// scripted point path is worth reading even though nothing here can change it.
        ///
        /// This used to be a hand copy of TargetKinematics' rule and it went stale the moment
        /// the kernel learned to trust a curve's own heading: with smooth ground paths on, every
        /// one of these targets runs a Catmull-Rom leg, and the copy reported the lead engaged
        /// on 0% of the samples while the kernel was in fact leading on all of them. It asks the
        /// kernel now (Scenario.h).
        bool WouldTrust(Creature* t)
        {
            return TargetMotionOf(*t).trusted;
        }

        /// Which motion a scenario runs.
        enum class Shape { Steady, Stop, Reversal, Circle };

        /// Everything the 100 ms samples add up over the phase.
        struct Run
        {
            ObjectGuid chaser, target;
            bool   started = false;
            // the ground both bodies covered -- printed as evidence, never scored (see the header)
            bool   havePrev = false;
            Pt     prevC = { 0.0f, 0.0f, 0.0f }, prevT = { 0.0f, 0.0f, 0.0f };
            float  chaserPath = 0.0f, targetPath = 0.0f;
            float  zMin = 1.0e9f, zMax = -1.0e9f;
            uint32 samples = 0, trusted = 0;
            // contact
            float  melee = 0.0f;
            bool   contacted = false;
            uint32 contactAt = 0;           ///< ms into the phase
            uint32 outsideBand = 0;         ///< samples over the phase with the gap outside the band
            double gapSum = 0.0;
            float  gapWorst = 0.0f;
            bool   victimKept = true, chaseKept = true;
            // the counters
            Motion::RelayCounts atMoveStart, atMoveEnd, atEnd;
            bool   haveMoveStart = false, haveMoveEnd = false, haveEnd = false;
            uint32 lastTotal = 0;
            bool   haveTotal = false;
            // overshoot
            uint32 overshoot = 0;
            float  worstPast = 0.0f;
            bool   havePast = false;
            // the motion's own moments and marks
            bool   stopped = false;
            uint32 stoppedAt = 0;
            Pt     mark = { 0.0f, 0.0f, 0.0f };   ///< the ring's centre, or the stop or turn point
            bool   turned = false;
            uint32 circleAt = 0;
            float  ringWorst = 0.0f;        ///< the chaser's own worst radial excursion, reported
            uint32 ringLegs = 0;
        };

        /// The four motions, one scenario each: the shape decides the geometry, the target's
        /// script, the overshoot frame and the contact deadline, and nothing else differs.
        class ChaseMovingMotion : public Scenario
        {
        public:
            ChaseMovingMotion(char const* name, int order, Shape shape)
                : Scenario(name, order), m_shape(shape) {}

            void Prepare() override;

        private:
            /// The whole phase, the moving part included.
            uint32 PhaseMs() const { return (m_shape == Shape::Steady || m_shape == Shape::Circle) ? 30000 : 20000; }
            /// The part of it the target is supposed to be moving for: the budget's own window.
            uint32 MovingMs() const { return (m_shape == Shape::Stop) ? 10000 : PhaseMs(); }
            /// By when the chase must have been inside the melee band at least once. The steady
            /// motion opens 10 yd abeam and has to close that at one yard a second of speed
            /// margin -- the led aim does it in 2.9 s and the un-led one never did in thirty, so
            /// 5 s both proves the closure and leaves room for the ground to roll. The other
            /// three open inside the band already, so anything but immediate is a regression.
            uint32 ContactByMs() const { return m_shape == Shape::Steady ? 5000 : 1000; }
            char const* ShapeName() const
            {
                switch (m_shape)
                {
                    case Shape::Steady:   return "a 30 s straight run";
                    case Shape::Stop:     return "10 s of run, then 10 s stopped dead";
                    case Shape::Reversal: return "10 s of run, a 180 deg turn mid-leg, 10 s back";
                    default:              return "a 12 yd ring for 30 s";
                }
            }
            Shape m_shape;
        };

        void ChaseMovingMotion::Prepare()
        {
            // The corridor, and the axis the abeam start is measured on.
            const float cdx = CORR_B.x - CORR_A.x, cdy = CORR_B.y - CORR_A.y;
            const float clen = std::sqrt((cdx * cdx) + (cdy * cdy));
            const Dir u = { cdx / clen, cdy / clen };
            const Dir p = { -u.y, u.x };
            const Shape shape = m_shape;
            const uint32 kPhase = PhaseMs();
            const uint32 kMoving = MovingMs();
            const uint32 kContactBy = ContactByMs();
            const uint32 kMove = 1500;
            const uint32 kEnd = kMove + kPhase;
            const uint32 kTicks = (kEnd + 500) / 100;

            auto st = std::make_shared<Run>();

            // The line, kLaneIn yards in from the corridor's end so a 180 yd run and a 12 yd
            // ring both stay on ground long-point already crosses.
            Pt anchor;
            anchor.x = CORR_A.x + (kLaneIn * u.x);
            anchor.y = CORR_A.y + (kLaneIn * u.y);
            anchor.z = CORR_A.z;

            // Where the two bodies open. The steady motion opens abeam, so there is an approach
            // to time; the other three open inside the melee band, because a stop, a reversal and
            // a ring can only be overshot by a chaser already ON its target.
            Pt wantT = anchor, wantC = anchor;
            if (shape == Shape::Circle)
            {
                wantT.x += kRadius * u.x;             wantT.y += kRadius * u.y;
                wantC.x += (kRadius + kBehind) * u.x; wantC.y += (kRadius + kBehind) * u.y;
            }
            else if (shape == Shape::Steady)
            {
                wantC.x -= kAbeam * p.x;  wantC.y -= kAbeam * p.y;
            }
            else
            {
                wantC.x -= kBehind * u.x; wantC.y -= kBehind * u.y;
            }

            Load(anchor.x, anchor.y);
            Load(anchor.x + (200.0f * u.x), anchor.y + (200.0f * u.y));

            Creature* t = Spawn(WOLF, wantT.x, wantT.y, Ground(wantT.x, wantT.y, wantT.z), 0.0f);
            Creature* c = Spawn(KOBOLD, wantC.x, wantC.y, Ground(wantC.x, wantC.y, wantC.z), 0.0f);
            if (!t || !c)
            {
                Verdict("relayBudget=INVALID(spawn failed) | aim=INVALID(spawn failed)");
                return;
            }
            t->SetMaxHealth(500000); t->SetHealth(500000);
            c->SetMaxHealth(500000); c->SetHealth(500000);
            t->setFaction(14);        // the chaser's victim, as everywhere in this family
            Silence(t); Silence(c);   // nothing but this file moves either body
            Park(t); Park(c);
            // The one yard a second that makes this a chase at all: the base run rate on the
            // chaser (7.00 yd/s), the template's own on the target (6.00). See the header.
            c->SetSpeedRate(MOVE_RUN, 1.0f, true);
            c->SetWalk(false, false);
            st->chaser = c->GetObjectGuid();
            st->target = t->GetObjectGuid();
            // The ring's centre: the circle's target opened kRadius out along the corridor from
            // the anchor, and the anchor is the request while this is the ground's own answer.
            // For the other three shapes `mark` is written when the motion's own moment arrives
            // and is never read before it.
            st->mark.x = t->Where().X() - (kRadius * u.x);
            st->mark.y = t->Where().Y() - (kRadius * u.y);
            st->mark.z = t->Where().Z();

            // ---- the chase, and the target's own script ------------------------------------
            // The chase is requested at the very moment the target starts, and not a second
            // earlier: a head start would let the chaser close the whole abeam offset before the
            // approach this scenario means to time had begun -- and that approach is now a gate.
            auto begin = [this, st, shape, u]()
            {
                Creature* c = Get(st->chaser); Creature* t = Get(st->target);
                if (!c || !t) { return; }
                // RANGED, as orders 48 and 49: a melee Attack would set UNIT_STAT_MELEE_ATTACKING
                // here with no range test at all, and the contact time -- which this family gates
                // on -- would be the scenario's own doing. A ranged attack sets the victim the
                // chase needs and leaves the melee bit to the chase's own EngageInReach effect.
                c->Attack(t, false);
                c->AddThreat(t, 1000.0f);
                c->GetMotionMaster()->MoveChase(t);
                st->melee = MeleeRange(c, t);
                st->started = true;
                Log("the chase opens %.2f yd out, melee range %.2f; target %.2f yd/s, chaser %.2f yd/s",
                    Dist3(c, t), st->melee, t->GetSpeed(MOVE_RUN), c->GetSpeed(MOVE_RUN));
                if (shape == Shape::Circle)
                {
                    st->circleAt = 0;   // the ring is handed out by the ticker, 24 deg at a time
                    Log("the target runs %s", ShapeName());
                    return;
                }
                // Every straight motion gets a leg LONGER than its window can walk, so the target
                // holds one velocity throughout and never stands between legs: 200 yd at 6 yd/s
                // is 33 s, and the longest straight window here is 30. The stop is the exception:
                // its leg is exactly the 60 yd of ten seconds, and ENDING is the event.
                const float reach = shape == Shape::Stop ? 60.0f : 200.0f;
                const float gx = t->Where().X() + (reach * u.x), gy = t->Where().Y() + (reach * u.y);
                t->GetMotionMaster()->MovePoint(1, gx, gy, Ground(gx, gy, t->Where().Z()), true);
                Log("the target runs %s (%.0f yd of leg)", ShapeName(), reach);
            };

            // A point on the ring 24 degrees ahead of where the target stands, re-aimed every
            // 700 ms: the 4.99 yd chord takes 0.83 s at 6 yd/s, so the next point always goes
            // out before the target reaches the last one and it never stands between legs.
            //
            // THE CADENCE IS NOT FREE, AND 700 ms IS WHY IT IS THIS. A scripted MovePoint costs
            // about one map tick between the request and the leg actually launching, and the
            // first cut of this scenario re-aimed every 300 ms: a hundred re-lays over the
            // thirty seconds, ten of those seconds spent standing, and the target walked 118 yd
            // of a ring it should have run 180 yd of -- at 3.9 yd/s, not run speed, with its
            // velocity trustworthy on only 66% of the samples, which is the very gate the
            // chase's lead hangs off. At 700 ms the stall costs about 4 s of the 30, the ring is
            // run at about 5.1 yd/s and the velocity is trusted 86% of the time. The 24 deg
            // chord cuts 0.26 yd inside the ring and leaves it 12 deg off the true tangent.
            auto ringStep = [this, st](uint32 now)
            {
                Creature* t = Get(st->target);
                if (!t) { return; }
                if (st->circleAt && now - st->circleAt < 700) { return; }
                st->circleAt = now;
                const float bx = t->Where().X() - st->mark.x, by = t->Where().Y() - st->mark.y;
                const float a = std::atan2(by, bx) + (24.0f * M_PI_F / 180.0f);
                const float gx = st->mark.x + (kRadius * std::cos(a));
                const float gy = st->mark.y + (kRadius * std::sin(a));
                t->GetMotionMaster()->MovePoint(3, gx, gy, Ground(gx, gy, st->mark.z), true);
            };

            // ---- one 100 ms sample -----------------------------------------------------------
            auto sample = [this, st, shape, u, kMoving, kPhase](uint32 into)
            {
                Creature* c = Get(st->chaser); Creature* t = Get(st->target);
                if (!c || !t) { return; }
                Motion::RelayCounts const* rc = Relays(c);
                if (!rc && into >= 1000) { st->chaseKept = false; }   // a tick or two to select
                if (c->getVictim() != t) { st->victimKept = false; }
                if (rc) { st->atEnd = *rc; st->haveEnd = true; }
                if (rc && !st->haveMoveStart) { st->atMoveStart = *rc; st->haveMoveStart = true; }
                if (rc && into <= kMoving) { st->atMoveEnd = *rc; st->haveMoveEnd = true; }

                const float gap = Dist3(c, t);
                if (st->melee <= 0.0f) { st->melee = MeleeRange(c, t); }
                // Contact is the EngageInReach effect's own doing: the melee bit, which nothing
                // else in this scenario ever sets.
                if (!st->contacted && c->hasUnitState(UNIT_STAT_MELEE_ATTACKING))
                {
                    st->contacted = true;
                    st->contactAt = into;
                }
                if (gap > st->melee) { ++st->outsideBand; }
                st->gapSum += gap;
                st->gapWorst = std::max(st->gapWorst, gap);

                // The ground both bodies covered, and how level the line is under the target.
                const Pt hereC = { c->Where().X(), c->Where().Y(), c->Where().Z() };
                const Pt hereT = { t->Where().X(), t->Where().Y(), t->Where().Z() };
                if (st->havePrev)
                {
                    st->chaserPath += Dist2(hereC.x, hereC.y, st->prevC.x, st->prevC.y);
                    st->targetPath += Dist2(hereT.x, hereT.y, st->prevT.x, st->prevT.y);
                }
                st->prevC = hereC; st->prevT = hereT; st->havePrev = true;
                st->zMin = std::min(st->zMin, hereT.z);
                st->zMax = std::max(st->zMax, hereT.z);
                ++st->samples;
                if (WouldTrust(t)) { ++st->trusted; }

                // A freshly laid leg. Fresh is the NATIVE's own counter, so a goal the driver
                // declined to re-lay under its 0.5 yd floor reads one floor stale.
                const uint32 total = rc ? rc->Total() : 0;
                const bool fresh = rc && st->haveTotal && total > st->lastTotal;
                if (rc) { st->lastTotal = total; st->haveTotal = true; }
                if (fresh && shape == Shape::Circle && !c->movespline->Finalized())
                {
                    const Movement::Vector3 goal = c->movespline->FinalDestination();
                    ++st->ringLegs;
                    const float out = Dist2(goal.x, goal.y, st->mark.x, st->mark.y) - kRadius;
                    if (!st->havePast || out > st->worstPast) { st->worstPast = out; st->havePast = true; }
                    if (out > st->melee) { ++st->overshoot; }
                }

                // Overshoot, in each motion's own frame (see the header).
                if (shape == Shape::Steady)
                {
                    const float past = Dot(u, hereC.x - hereT.x, hereC.y - hereT.y);
                    if (!st->havePast || past > st->worstPast) { st->worstPast = past; st->havePast = true; }
                    if (past > st->melee) { ++st->overshoot; }
                }
                else if (shape == Shape::Stop)
                {
                    if (!st->stopped && into >= 1000 && t->movespline->Finalized())
                    {
                        st->stopped = true;
                        st->stoppedAt = into;
                        st->mark = hereT;
                        Log("the target stopped dead at +%u ms, %.2f yd from its chaser", into, gap);
                    }
                    if (st->stopped)
                    {
                        const float past = Dot(u, hereC.x - st->mark.x, hereC.y - st->mark.y);
                        if (!st->havePast || past > st->worstPast) { st->worstPast = past; st->havePast = true; }
                        if (past > st->melee) { ++st->overshoot; }
                    }
                }
                else if (shape == Shape::Reversal)
                {
                    if (!st->turned && into >= kPhase / 2)
                    {
                        // Mid-leg, so the running spline is REPLACED and the target never stands.
                        st->turned = true;
                        st->mark = hereT;
                        const float gx = hereT.x - (120.0f * u.x), gy = hereT.y - (120.0f * u.y);
                        t->GetMotionMaster()->MovePoint(2, gx, gy, Ground(gx, gy, hereT.z), true);
                        Log("the target turned 180 deg at +%u ms, %.2f yd from its chaser", into, gap);
                    }
                    if (st->turned)
                    {
                        const float past = Dot(u, hereC.x - st->mark.x, hereC.y - st->mark.y);
                        if (!st->havePast || past > st->worstPast) { st->worstPast = past; st->havePast = true; }
                        if (past > st->melee) { ++st->overshoot; }
                    }
                }
                else
                {
                    const float ring = std::fabs(Dist2(hereC.x, hereC.y, st->mark.x, st->mark.y) - kRadius);
                    st->ringWorst = std::max(st->ringWorst, ring);
                }

                if (into % 5000 == 0)
                {
                    Log("+%5u ms: gap %.2f yd, %s", into, gap,
                        rc ? Causes(*rc).c_str() : "(the chase is not selected)");
                }
            };

            // ---- the verdict -----------------------------------------------------------------
            auto verdict = [this, st, kMoving, kPhase, kContactBy, shape]()
            {
                char text[1000];
                const bool valid = st->started && st->haveMoveStart && st->haveMoveEnd && st->samples > 10;
                if (!valid)
                {
                    Verdict("relayBudget=INVALID(the chase did not run) | aim=INVALID(the chase did not run)");
                    return;
                }
                const float span = float(kMoving) / 1000.0f;
                const uint32 routine = st->atMoveEnd.routine - st->atMoveStart.routine;
                const float routinePerSec = span > 0.0f ? float(routine) / span : 0.0f;
                const uint32 total = st->haveEnd ? st->atEnd.Total() : 0;
                const float totalPerSec = float(total) / (float(kPhase) / 1000.0f);
                const float meanGap = float(st->gapSum / double(st->samples));
                const float bandS = float(st->outsideBand) * 0.1f;
                const float trustPct = 100.0f * float(st->trusted) / float(st->samples);

                snprintf(text, sizeof(text), "relayBudget=%s(%.2f routine re-lays per second over %s: %u over %.0f s of the moving phase, against design v2 section 11's ceiling of one; every cause over the whole phase: %s, %.2f re-lays per second in all)",
                         routinePerSec <= 1.0f ? "OK" : "BUG", routinePerSec, ShapeName(), routine, span,
                         st->haveEnd ? Causes(st->atEnd).c_str() : "(the chase was not selected)", totalPerSec);
                const std::string relayBudget = text;

                std::string aim;
                if (!st->victimKept || !st->chaseKept)
                {
                    snprintf(text, sizeof(text), "aim=INVALID(the chase did not hold: victim kept %d, chase selected %d)",
                             st->victimKept ? 1 : 0, st->chaseKept ? 1 : 0);
                    aim = text;
                }
                else
                {
                    // Contact is the gate the predictive aim was adopted for: from the steady
                    // motion's 10 yd abeam start the un-led chase never made it inside the band
                    // in thirty seconds, and the led one is there in under three.
                    const bool onTime = st->contacted && st->contactAt <= kContactBy;
                    const bool ok = st->overshoot == 0 && onTime;
                    char contact[48];
                    if (!st->contacted) { snprintf(contact, sizeof(contact), "NEVER"); }
                    else { snprintf(contact, sizeof(contact), "%.1f s", float(st->contactAt) / 1000.0f); }
                    snprintf(text, sizeof(text), "aim=%s(%u overshoot sample(s) over %s, the worst %+.2f yd past the mark against a %.2f yd band; first contact %s against a %.1f s deadline; the gap averaged %.2f yd and peaked at %.2f, %.1f s of the phase outside the band; the chaser laid %.1f yd of ground over the target's %.1f -- evidence, not a score, since over a long straight run a chaser's path IS its target's -- and the target's velocity was trustworthy on %.0f%% of the samples)",
                             ok ? "OK" : "BUG", st->overshoot, ShapeName(), st->worstPast, st->melee,
                             contact, float(kContactBy) / 1000.0f, meanGap, st->gapWorst, bandS,
                             st->chaserPath, st->targetPath, trustPct);
                    aim = text;
                }

                Log("the line's own height range under the target is %.2f yd", st->zMax - st->zMin);
                if (shape == Shape::Circle)
                {
                    Log("the chaser's own worst radial excursion off the %.0f yd ring: %.2f yd over %u legs",
                        kRadius, st->ringWorst, st->ringLegs);
                }
                Verdict(relayBudget + " | " + aim);
            };

            // ---- the timeline ------------------------------------------------------------------
            At(kMove, begin);
            for (uint32 i = 1; i <= kTicks; ++i)
            {
                const uint32 now = i * 100;
                At(now, [sample, ringStep, now, kMove, kEnd, kMoving, shape]()
                {
                    if (now < kMove || now > kEnd) { return; }
                    const uint32 into = now - kMove;
                    if (shape == Shape::Circle && into <= kMoving) { ringStep(now); }
                    sample(into);
                });
            }
            At(kEnd + 400, verdict);
        }
    }

    void RegisterChaseMovingScenarios(Runner& r)
    {
        r.Register(new ChaseMovingMotion("chase-moving-steady", 68, Shape::Steady));
        r.Register(new ChaseMovingMotion("chase-moving-stop", 69, Shape::Stop));
        r.Register(new ChaseMovingMotion("chase-moving-reversal", 70, Shape::Reversal));
        r.Register(new ChaseMovingMotion("chase-moving-circle", 71, Shape::Circle));
    }
}
