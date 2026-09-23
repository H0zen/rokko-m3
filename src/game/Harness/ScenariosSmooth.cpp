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
#include "Map.h"
#include "MotionMaster.h"
#include "MotionFrame.h"
#include "Log.h"
#include "movement/MoveSpline.h"
#include "Utilities/MathDefines.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

/**
 * The smooth-ground-splines family (the spec of 2026-09-22, design v2 §11): a routed ground
 * leg the pathfinder BENT -- three or more points -- goes out as a Catmull-Rom curve instead
 * of a chain of straight segments, so the unit rounds its corners instead of pivoting on each.
 *
 * What the risk table asks this to prove, and how it is proved here:
 *
 *  - CORNER CUTTING. A Catmull-Rom interpolates every control point, so it cannot cut a corner
 *    off; what it does is swing WIDE of the segments either side of one, by 2L/27 of a leg of
 *    length L at a right angle (MotionWriters_the_curve_strays_from_the_polyline_in_proportion
 *    _to_the_leg pins that law exactly). The pathfinder's own points are SMOOTH_PATH_STEP_SIZE
 *    = 4 yd apart, so the stray is centimetres -- but only a real route over the real mesh can
 *    say so, which is what this does: it samples the spline's own interpolated position every
 *    100 ms and measures how far each sample sits from the polyline the pathfinder drew.
 *
 *    The bare harness map (Mulgore) is not a city and has no doorway or pillar to name up
 *    front, so rather than hard-code a corner that might be a straight line on this mesh the
 *    scenario FINDS one: it routes a fan of candidate goals around the spawn and takes the
 *    route that turns hardest. The verdict prints the turn it found, so a reader can see
 *    whether the corner was worth the name. Alongside the stray it also asks the map itself:
 *    every consecutive pair of samples must have line of sight, so a curve that swung through
 *    a rock or a wall between two samples is caught as geometry and not as a tolerance.
 *
 *  - ARRIVAL DRIFT. The leg must still end ON the point and inform there, and the zero-length
 *    guard (NOWHERE_DISTANCE, PR #115) must not then swallow the NEXT leg as "already there".
 *    So a second point is ordered after the first arrives, and the unit has to really travel
 *    to it.
 */
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;

        /// The Mulgore spot the point family spawns at; open enough to route in every
        /// direction and inside the grids the harness already loads.
        const float SX = -3122.6f;
        const float SY = -261.3f;
        const float SZ = 46.0f;

        /// The absolute ceiling on how far the curve may put the unit off the polyline the
        /// router drew, over and above the per-leg bound the verdict computes. A Catmull-Rom
        /// cannot stray further than (4/27)(|m_in - d| + |m_out - d|) on any segment, which for
        /// points a step s apart is at most 8s/27; the pathfinder smooths at
        /// SMOOTH_PATH_STEP_SIZE = 4 yd (PathFinder.h:48), so 8 x 4 / 27 = 1.19 yd is the most
        /// any route of its making can reach and 1.25 is that with a little float headroom.
        /// The number is therefore a statement about the ROUTER'S STEP, not a tolerance chosen
        /// to fit an answer: widen the step and this is the assertion that fires.
        const float kStrayCeiling = 1.25f;

        /// A leg is only a corner worth measuring if it turns at least this much somewhere.
        const float kCornerDegrees = 45.0f;

        float Dist2(float x1, float y1, float x2, float y2)
        {
            const float dx = x1 - x2, dy = y1 - y2;
            return sqrtf(dx * dx + dy * dy);
        }

        /// The distance from p to the segment ab, in the horizontal plane. A footprint is
        /// horizontal, and the z of a routed point is the ground's.
        float DistToSegment2D(Movement::Vector3 const& p, Movement::Vector3 const& a, Movement::Vector3 const& b)
        {
            const float vx = b.x - a.x, vy = b.y - a.y;
            const float wx = p.x - a.x, wy = p.y - a.y;
            const float len2 = vx * vx + vy * vy;
            float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
            if (t < 0.0f) { t = 0.0f; }
            if (t > 1.0f) { t = 1.0f; }
            const float dx = wx - t * vx, dy = wy - t * vy;
            return sqrtf(dx * dx + dy * dy);
        }

        /// The sharpest turn anywhere along a polyline, in degrees: 0 is straight on,
        /// 180 a hairpin.
        float SharpestTurn(Motion::PointsArray const& p)
        {
            float worst = 0.0f;
            for (size_t i = 1; i + 1 < p.size(); ++i)
            {
                const float ax = p[i].x - p[i - 1].x, ay = p[i].y - p[i - 1].y;
                const float bx = p[i + 1].x - p[i].x, by = p[i + 1].y - p[i].y;
                const float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
                if (la < 0.01f || lb < 0.01f) { continue; }
                float cosine = (ax * bx + ay * by) / (la * lb);
                if (cosine > 1.0f) { cosine = 1.0f; }
                if (cosine < -1.0f) { cosine = -1.0f; }
                const float turn = acosf(cosine) * 180.0f / M_PI_F;
                if (turn > worst) { worst = turn; }
            }
            return worst;
        }

        /// smooth-ground-corner: the routed corner, its curve, and what the curve costs.
        class SmoothGroundCorner : public Scenario
        {
        public:
            SmoothGroundCorner() : Scenario("smooth-ground-corner", 72) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SX, SY, Ground(SX, SY, SZ), 0.0f);
                if (!a)
                {
                    Verdict("route=INVALID(spawn failed) | smooth=INVALID(spawn failed) | corner=INVALID(spawn failed)"
                            " | clearance=INVALID(spawn failed) | arrival=INVALID(spawn failed) | nextLeg=INVALID(spawn failed)");
                    return;
                }
                Silence(a);                       // no wander of its own under the scripted legs

                // The probe reaches 110 yd; load the grids it can touch so the mmap tiles
                // under them are in the mesh before anything is routed.
                for (int gx = -1; gx <= 1; ++gx)
                {
                    for (int gy = -1; gy <= 1; ++gy)
                    {
                        Load(SX + gx * 110.0f, SY + gy * 110.0f);
                    }
                }

                struct St
                {
                    bool   found;
                    float  gx, gy, gz;
                    size_t points;
                    float  turn;
                    bool   sawLeg;
                    bool   legEnded;
                    uint32 splineId;                   ///< the POINT 77 leg's own spline: nothing else is sampled
                    bool   smooth;
                    Motion::PointsArray guarded;       ///< the leg's point array AS THE SPLINE HOLDS IT, both guards included
                    std::vector<Pt> samples;           ///< the spline's interpolated position, every 100 ms
                    float  stray;
                    float  bound;                      ///< the most a Catmull-Rom over THESE points can stray
                    float  longestStep;                ///< the longest gap between two routed points
                    bool   losOk;
                    bool   losTested;
                    float  arrivedAt;                  ///< how far from the point the unit stood WHEN THE LEG ENDED
                    bool   informed;
                    float  informedAt;
                    float  secondTravelled;
                    bool   secondOrdered;
                    float  secondFromX, secondFromY;
                };
                auto st = std::make_shared<St>();
                st->found = st->sawLeg = st->legEnded = st->smooth = false;
                st->informed = st->secondOrdered = false;
                st->losOk = true;
                st->losTested = false;
                st->points = 0;
                st->splineId = 0;
                st->turn = st->stray = st->bound = st->longestStep = st->secondTravelled = 0.0f;
                st->gx = st->gy = st->gz = 0.0f;
                st->arrivedAt = st->informedAt = -1.0f;
                st->secondFromX = st->secondFromY = 0.0f;

                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                const size_t mark = Informs().size();

                // --- the probe: which way does this mesh make a leg bend? ------------------
                At(500, [this, g, st]()
                {
                    Creature* c = Get(g); if (!c) { return; }
                    std::unique_ptr<Motion::IPathQuery> q = Motion::FrameFor(*c).CreatePathQuery(*c);
                    if (!q) { return; }

                    const Motion::Vector3 from = Motion::FrameFor(*c).MoverPosition(*c);
                    const float radii[4] = { 30.0f, 50.0f, 75.0f, 100.0f };
                    for (int r = 0; r < 4; ++r)
                    {
                        for (int dir = 0; dir < 16; ++dir)
                        {
                            const float angle = float(dir) * 2.0f * M_PI_F / 16.0f;
                            Motion::Vector3 to;
                            to.x = from.x + cosf(angle) * radii[r];
                            to.y = from.y + sinf(angle) * radii[r];
                            to.z = Ground(to.x, to.y, from.z);
                            if (!q->Calculate(from, to, false, 0.0f) || !q->Routed()) { continue; }
                            if (q->Points().size() < 3) { continue; }

                            const float turn = SharpestTurn(q->Points());
                            if (turn > st->turn)
                            {
                                st->found = true;
                                st->turn = turn;
                                st->points = q->Points().size();
                                st->gx = to.x; st->gy = to.y; st->gz = to.z;
                            }
                        }
                    }
                    if (st->found)
                    {
                        Log("the mesh bends hardest toward (%.1f, %.1f): %u points, sharpest turn %.0f deg",
                            st->gx, st->gy, uint32(st->points), st->turn);
                    }
                    else
                    {
                        Log("ERR no routed polyline of three or more points anywhere in the probe");
                    }
                });

                // --- the leg ---------------------------------------------------------------
                At(1000, [this, g, st]()
                {
                    Creature* c = Get(g); if (!c || !st->found) { return; }
                    c->GetMotionMaster()->MovePoint(77, st->gx, st->gy, st->gz, true);
                    Log("MovePoint(77) to the bent route, mt=%s", TypeName(c));
                });

                // --- the samples: the POINT 77 spline's own interpolated position, every 100 ms
                //     The wolf's own default motion is a random wander and it takes the wheel
                //     the moment the point is reached, so the leg is pinned by its SPLINE ID:
                //     nothing laid after it is ever sampled or measured.
                for (uint32 i = 1; i <= 400; ++i)
                {
                    At(1000 + i * 100, [this, g, st]()
                    {
                        Creature* c = Get(g); if (!c || !st->found || st->legEnded) { return; }
                        const bool live = c->movespline->Initialized() && !c->movespline->Finalized();

                        if (!st->sawLeg)
                        {
                            if (!live) { return; }
                            st->sawLeg = true;
                            st->splineId = c->movespline->GetId();
                            st->smooth = c->movespline->isSmooth();
                            // The leg's point array exactly as the spline holds it: index 0 is
                            // the leading virtual guard and the last a duplicate of the end.
                            Movement::MoveSpline::MySpline::ControlArray const& all = c->movespline->PathPoints();
                            for (size_t k = 0; k < all.size(); ++k)
                            {
                                st->guarded.push_back(all[k]);
                            }
                            for (size_t k = 2; k + 1 < all.size(); ++k)
                            {
                                const float step = (all[k] - all[k - 1]).length();
                                if (step > st->longestStep) { st->longestStep = step; }
                            }
                            Log("leg live: smooth=%d, %u routed points, longest router step %.2f yd, %d ms",
                                st->smooth ? 1 : 0, uint32(all.size() - 2), st->longestStep,
                                c->movespline->Duration());
                        }

                        if (!live || c->movespline->GetId() != st->splineId)
                        {
                            // The leg is over. Read the arrival HERE, before the wander moves the
                            // wolf, and order the next point from where it really stands: that is
                            // what asks the zero-length guard (NOWHERE_DISTANCE, PR #115) whether
                            // it thinks a unit fresh off a smooth leg is already at its next one.
                            st->legEnded = true;
                            st->arrivedAt = Dist2(c->Where().X(), c->Where().Y(), st->gx, st->gy);
                            st->secondFromX = c->Where().X();
                            st->secondFromY = c->Where().Y();
                            st->secondOrdered = true;
                            c->GetMotionMaster()->MovePoint(78, st->gx + 20.0f, st->gy,
                                                            Ground(st->gx + 20.0f, st->gy, st->gz), true);
                            Log("the leg ended %.2f yd from the point over %u samples; MovePoint(78) 20 yd on",
                                st->arrivedAt, uint32(st->samples.size()));
                            return;
                        }

                        Movement::Location const at = c->movespline->ComputePosition();
                        Pt p; p.x = at.x; p.y = at.y; p.z = at.z;
                        st->samples.push_back(p);
                    });
                }

                // --- the measurement, then the verdict -------------------------------------
                At(1000 + 401 * 100, [this, g, st, low, mark]()
                {
                    Creature* c = Get(g); if (!c) { return; }

                    // The polyline the router drew: the leg's points with the two guards off.
                    Motion::PointsArray route;
                    for (size_t k = 1; k + 1 < st->guarded.size(); ++k)
                    {
                        route.push_back(st->guarded[k]);
                    }

                    // What a Catmull-Rom over exactly these points is ABLE to stray, which is
                    // the number the tolerance should be and not one picked to fit the answer.
                    // Writing the segment from c[i] to c[i+1] in Hermite form and subtracting
                    // the straight line between the same two points leaves
                    //     P(t) - L(t) = h10(t) (m_i - d) + h11(t) (m_i+1 - d)
                    // with d = c[i+1] - c[i] and m the Catmull-Rom tangents (half the chord
                    // between a point's two neighbours). |h10| and |h11| both peak at 4/27, so
                    // the stray on that segment can never exceed (4/27)(|m_i - d| + |m_i+1 - d|)
                    // -- a quantity in the SPACING of the router's points and not in the length
                    // of the leg. That is the whole safety argument for this feature, and it is
                    // why nothing here may ever widen the pathfinder's 4 yd step.
                    for (size_t i = 1; i + 2 < st->guarded.size(); ++i)
                    {
                        Movement::Vector3 const d = st->guarded[i + 1] - st->guarded[i];
                        Movement::Vector3 const mIn = (st->guarded[i + 1] - st->guarded[i - 1]) * 0.5f;
                        Movement::Vector3 const mOut = (st->guarded[i + 2] - st->guarded[i]) * 0.5f;
                        const float worst = (4.0f / 27.0f) * ((mIn - d).length() + (mOut - d).length());
                        if (worst > st->bound) { st->bound = worst; }
                    }

                    // How far the curve ever actually put the unit from that polyline, and
                    // whether the map had anything between two consecutive samples -- the
                    // second is the footprint question asked of the geometry itself and not
                    // of a tolerance.
                    for (size_t i = 0; i < st->samples.size(); ++i)
                    {
                        Movement::Vector3 const at(st->samples[i].x, st->samples[i].y, st->samples[i].z);
                        float nearest = 1e9f;
                        for (size_t k = 1; k < route.size(); ++k)
                        {
                            const float d = DistToSegment2D(at, route[k - 1], route[k]);
                            if (d < nearest) { nearest = d; }
                        }
                        if (route.size() >= 2 && nearest > st->stray) { st->stray = nearest; }

                        if (i > 0)
                        {
                            Pt const& prev = st->samples[i - 1];
                            st->losTested = true;
                            if (!c->GetMap()->IsInLineOfSight(prev.x, prev.y, prev.z + 1.0f,
                                                              at.x, at.y, at.z + 1.0f, c->GetPhaseMask()))
                            {
                                st->losOk = false;
                            }
                        }
                    }

                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.kind == Motion::Kind::Point && r.id == 77)
                        {
                            st->informed = true;
                            st->informedAt = Dist2(r.x, r.y, st->gx, st->gy);
                        }
                    }
                    if (st->secondOrdered)
                    {
                        st->secondTravelled = Dist2(c->Where().X(), c->Where().Y(), st->secondFromX, st->secondFromY);
                    }
                    Log("curve: %u samples, worst stray %.2f yd of the %.2f this curve could reach, LOS %s",
                        uint32(st->samples.size()), st->stray, st->bound, st->losOk ? "clear" : "BROKEN");

                    char text[256];
                    std::string route_v;
                    if (!st->found)
                    {
                        route_v = "INVALID(no routed polyline of three or more points in the probe)";
                    }
                    else if (st->turn < kCornerDegrees)
                    {
                        snprintf(text, sizeof(text), "INVALID(the sharpest route on this mesh turns only %.0f deg)", st->turn);
                        route_v = text;
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "OK(%u points, turns %.0f deg)", uint32(st->points), st->turn);
                        route_v = text;
                    }

                    std::string smooth;
                    if (!st->sawLeg)
                    {
                        smooth = "INVALID(no leg was ever live)";
                    }
                    else if (st->smooth)
                    {
                        smooth = "OK(Catmull-Rom)";
                    }
                    else
                    {
                        smooth = "BUG(the bent route still went out linear)";
                    }

                    std::string corner;
                    if (st->samples.empty() || route.size() < 3)
                    {
                        corner = "INVALID(nothing sampled)";
                    }
                    else if (st->stray <= st->bound && st->stray <= kStrayCeiling)
                    {
                        snprintf(text, sizeof(text), "OK(%.2f yd off the polyline, under this curve's own %.2f and the %.2f ceiling; longest router step %.2f yd)",
                                 st->stray, st->bound, kStrayCeiling, st->longestStep);
                        corner = text;
                    }
                    else if (st->stray > st->bound)
                    {
                        snprintf(text, sizeof(text), "BUG(%.2f yd off the polyline, past the %.2f a Catmull-Rom over these points can reach: the unit is not on the curve at all)",
                                 st->stray, st->bound);
                        corner = text;
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "BUG(%.2f yd off the polyline, past the %.2f ceiling: the router's step %.2f yd is too coarse to smooth)",
                                 st->stray, kStrayCeiling, st->longestStep);
                        corner = text;
                    }

                    std::string clearance;
                    if (!st->losTested)
                    {
                        clearance = "INVALID(nothing sampled)";
                    }
                    else
                    {
                        clearance = st->losOk ? "OK(no geometry between any two samples)"
                                              : "BUG(the curve passed through geometry)";
                    }

                    std::string arrival;
                    if (!st->legEnded)
                    {
                        arrival = "INVALID(the leg never ended)";
                    }
                    else if (!st->informed)
                    {
                        arrival = "BUG(POINT 77 never informed)";
                    }
                    else if (st->arrivedAt < 6.0f && st->informedAt < 6.0f)
                    {
                        snprintf(text, sizeof(text), "OK(stood %.2f yd from the point as the leg ended, informed %.2f yd from it)",
                                 st->arrivedAt, st->informedAt);
                        arrival = text;
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "BUG(stood %.2f yd away as the leg ended, informed %.2f yd short)",
                                 st->arrivedAt, st->informedAt);
                        arrival = text;
                    }

                    std::string nextLeg;
                    if (!st->secondOrdered)
                    {
                        nextLeg = "INVALID(the second point was never ordered)";
                    }
                    else if (st->secondTravelled > 10.0f)
                    {
                        snprintf(text, sizeof(text), "OK(travelled %.1f yd for the next point)", st->secondTravelled);
                        nextLeg = text;
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "BUG(moved only %.1f yd: the zero-length guard swallowed the leg)",
                                 st->secondTravelled);
                        nextLeg = text;
                    }

                    Verdict("route=" + route_v + " | smooth=" + smooth + " | corner=" + corner +
                            " | clearance=" + clearance + " | arrival=" + arrival + " | nextLeg=" + nextLeg);
                });
            }
        };
    }

    void RegisterSmoothScenarios(Runner& r)
    {
        r.Register(new SmoothGroundCorner());
    }
}
