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
#include "MotionMaster.h"
#include "WaypointManager.h"
#include "Utilities/MathDefines.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// The default-moves family (P5-B family 2 Task 5, orders 45-47): three claims made native
// by Tasks 1-4 (welding, a waiting node's orientation, SetNextWaypoint from inside a hook)
// that the old harness never exercised, because the shell used to own all three. Every
// scenario spawns its own chicken (entry 621) at the patrol family's square and lets the
// runner despawn it. Pt, Dist2 and the square's corners follow ScenariosPatrol.cpp's naming.
namespace Harness
{
    namespace
    {
        const uint32 CHICKEN = 621;

        const Pt P0     = { -3122.6f, -261.3f, 46.0f };   // node 1: the template square's near corner
        const Pt P0_N2  = { -3152.6f, -261.3f, 46.0f };   // node 2
        const Pt P0_FAR = { -3152.6f, -231.3f, 46.0f };   // node 3: the square's far (diagonal) corner
        const Pt P0_N4  = { -3122.6f, -231.3f, 46.0f };   // node 4

        /// The nodes a sample recorded, comma-joined, as ScenariosPatrol.cpp's JoinNodes does
        /// (copied here rather than shared: each family's copy stays a one-line function).
        std::string JoinNodes(std::vector<uint32> const& nodes)
        {
            std::string out;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (i)
                {
                    out += ",";
                }
                char buf[16];
                snprintf(buf, sizeof(buf), "%u", nodes[i]);
                out += buf;
            }
            return out;
        }

        /// The bearing from one point to another, normalized into [0, 2*pi) the way
        /// Geometry::Placement::Face stores every facing, so AngleDiff can compare a sampled
        /// facing against it directly.
        float Bearing(Pt const& from, Pt const& to)
        {
            const float a = std::atan2(to.y - from.y, to.x - from.x);
            return (a >= 0.0f) ? a : 2.0f * M_PI_F + a;
        }

        /// The shorter angular distance between two facings (both already in [0, 2*pi), as
        /// Geometry::Placement::Face normalizes every write): wraps at pi, so a facing near
        /// 0/2*pi still compares correctly against a target near the other end.
        float AngleDiff(float a, float b)
        {
            float d = std::fabs(a - b);
            if (d > M_PI_F)
            {
                d = 2.0f * M_PI_F - d;
            }
            return d;
        }

        /// Task 5 / patrol-welded (45): the chicken's square as an ENTRY path (kWeldPath,
        /// WaypointManager::AddEntryNode -- no database row), so PatrolBehaviour welds its
        /// legs (an external path never does, since a script may replace it under us at any
        /// node). `welded` reads the facade's new SelectedPatrolLegPoints at +1.5 s;
        /// `informsInOrder` and `lapContinues` read the ordinary WAYPOINT informs.
        class PatrolWelded : public Scenario
        {
        public:
            PatrolWelded() : Scenario("patrol-welded", 45) {}

            void Prepare() override
            {
                Creature* a = Spawn(CHICKEN, P0.x, P0.y, P0.z, 0.0f);
                if (!a)
                {
                    Verdict("welded=INVALID(spawn failed) | informsInOrder=INVALID(spawn failed) | lapContinues=INVALID(spawn failed)");
                    return;
                }
                Load(P0_FAR.x, P0_FAR.y);
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                const size_t mark = Informs().size();
                auto reached = std::make_shared<std::vector<uint32> >();
                auto lastLegPoints = std::make_shared<size_t>(size_t(-1));   // an impossible count: the first sample always logs
                auto legAtOneFive = std::make_shared<size_t>(0);
                At(500, [this, g, low]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveWaypoint(kWeldPath, PATH_FROM_ENTRY);
                    Log("chicken guid=%u MoveWaypoint on the welded entry square, mt=%s", low, TypeName(a));
                });
                At(1500, [this, g, legAtOneFive]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    const size_t n = a->GetMotionMaster()->SelectedPatrolLegPoints();
                    *legAtOneFive = n;
                    Log("+1.5s selected patrol leg point count = %u", uint32(n));
                });
                for (uint32 i = 1; i <= 60; ++i)
                {
                    At(500 + i * 1000, [this, g, reached, lastLegPoints, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const float px = a->Where().X(), py = a->Where().Y();
                        const uint32 wp = Node(a);
                        if (reached->empty() || reached->back() != wp)
                        {
                            reached->push_back(wp);
                            Log("+%3us at %.1f %.1f mt=%s at node %u", i, px, py, TypeName(a), wp);
                        }
                        const size_t legPoints = a->GetMotionMaster()->SelectedPatrolLegPoints();
                        if (legPoints != *lastLegPoints)
                        {
                            Log("+%3us leg point count = %u", i, uint32(legPoints));
                            *lastLegPoints = legPoints;
                        }
                    });
                }
                At(61500, [this, low, mark, legAtOneFive, reached]()
                {
                    std::vector<uint32> ids;
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.kind == Motion::Kind::Patrol) { ids.push_back(r.id); }
                    }
                    char welded[80];
                    if (*legAtOneFive > 2) { snprintf(welded, sizeof(welded), "welded=OK(%u points at +1.5 s)", uint32(*legAtOneFive)); }
                    else { snprintf(welded, sizeof(welded), "welded=BUG(%u points: no weld)", uint32(*legAtOneFive)); }

                    const bool inOrder = ids.size() >= 4 && ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && ids[3] == 4;
                    std::string informsInOrder;
                    if (inOrder)
                    {
                        informsInOrder = "informsInOrder=OK";
                    }
                    else
                    {
                        std::vector<uint32> head(ids.begin(), ids.begin() + std::min<size_t>(ids.size(), 8));
                        informsInOrder = "informsInOrder=BUG(ids: " + JoinNodes(head) + ")";
                    }

                    bool lapContinues = false;
                    int fourIdx = -1;
                    for (size_t k = 0; k < ids.size(); ++k) { if (ids[k] == 4) { fourIdx = int(k); break; } }
                    if (fourIdx >= 0)
                    {
                        for (size_t k = size_t(fourIdx) + 1; k < ids.size(); ++k) { if (ids[k] == 1) { lapContinues = true; break; } }
                    }

                    std::string body = std::string(welded) + " | " + informsInOrder + " | lapContinues=" +
                                        (lapContinues ? std::string("OK")
                                                      : "BUG(no second lap in 60 s; informed ids: " + JoinNodes(ids) + ")") +
                                        " (nodes in order: " + JoinNodes(*reached) + ")";
                    Verdict(body);
                });
            }
        };

        /// Task 5 / patrol-orients-at-a-waiting-node (46): kFacePath, an external path where
        /// node 2 waits 3 s facing 1.5 rad and node 3 (no delay) is given an orientation
        /// (4.7 rad) the native must ignore, keeping the travel facing instead. `facesAtWait`
        /// polls the wait at node 2, which holds for 3 s -- ample time for a 250 ms sample to
        /// land inside it. `travelFacingElse` cannot poll the same way: node 3 has no delay,
        /// so Node() reads 3 for at most the native's own PrepareInform round (one tick,
        /// proven on the seeded run: even a 1.5 yd position window never sampled it), too
        /// narrow a window for a 250 ms cadence to hit reliably. It reads the facing from
        /// inside the arrival's own inform instead, where the moment is exact rather than a
        /// sampling bet.
        class PatrolOrientsAtAWaitingNode : public Scenario
        {
        public:
            PatrolOrientsAtAWaitingNode() : Scenario("patrol-orients-at-a-waiting-node", 46) {}

            /// The recording hook (Scenario.h): fires synchronously at the exact arrival, so
            /// the facing read here is the leg's true final facing, not whatever a later poll
            /// might catch after the driver has already moved on to the next leg.
            void OnPathInform(Creature* creature, uint32 pathId, Motion::PathEvent event, uint32 node) override
            {
                if (creature->GetGUIDLow() == m_low && pathId == uint32(kFacePath) && event == Motion::PathEvent::NodeReached && node == 3)
                {
                    const float facing = creature->Where().Facing();
                    m_atNode3.push_back(facing);
                    Log("node 3 informed: facing=%.3f", facing);
                }
            }

            void Prepare() override
            {
                m_atNode3.clear();
                m_low = 0;
                Creature* a = Spawn(CHICKEN, P0.x, P0.y, P0.z, 0.0f);
                if (!a)
                {
                    Verdict("facesAtWait=INVALID(spawn failed) | travelFacingElse=INVALID(spawn failed)");
                    return;
                }
                Load(P0_FAR.x, P0_FAR.y);
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                m_low = low;   // the hook below fires for every recording creature: only ours counts
                auto atNode2 = std::make_shared<std::vector<float> >();
                At(500, [this, g, low]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveWaypoint(kFacePath, PATH_FROM_EXTERNAL);
                    Log("chicken guid=%u MoveWaypoint on the facing path, mt=%s", low, TypeName(a));
                });
                for (uint32 i = 1; i <= 240; ++i)   // 250 ms * 240 = 60 s
                {
                    At(500 + i * 250, [this, g, atNode2, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const uint32 node = Node(a);
                        const float x = a->Where().X(), y = a->Where().Y();
                        const float facing = a->Where().Facing();
                        if (node == 2 && Dist2(x, y, P0_N2.x, P0_N2.y) <= 0.5f)
                        {
                            atNode2->push_back(facing);
                            Log("+%5ums at node 2 (%.1f %.1f) facing=%.3f", i * 250, x, y, facing);
                        }
                    });
                }
                At(61500, [this, atNode2]()
                {
                    std::string facesAtWait;
                    if (atNode2->empty())
                    {
                        facesAtWait = "facesAtWait=INVALID(never waited at node 2)";
                    }
                    else
                    {
                        bool matched = false;
                        float example = atNode2->front();
                        for (size_t k = 0; k < atNode2->size(); ++k)
                        {
                            if (AngleDiff((*atNode2)[k], 1.5f) <= 0.1f) { matched = true; example = (*atNode2)[k]; break; }
                        }
                        char buf[64];
                        if (matched) { snprintf(buf, sizeof(buf), "facesAtWait=OK(facing %.3f at node 2)", example); }
                        else { snprintf(buf, sizeof(buf), "facesAtWait=BUG(facing %.3f)", atNode2->back()); }
                        facesAtWait = buf;
                    }
                    std::string travelFacingElse;
                    if (m_atNode3.empty())
                    {
                        travelFacingElse = "travelFacingElse=INVALID(node 3 never sampled)";
                    }
                    else
                    {
                        // Not the node's own 4.7 is only half the claim: the facing must be the
                        // travel direction, which for the 2->3 leg is the bearing between the two
                        // nodes (pi/2 here, computed rather than written down).
                        const float travel = Bearing(P0_N2, P0_FAR);
                        bool allFar = true;
                        bool allTravel = true;
                        float culprit = m_atNode3.front();
                        float strayed = m_atNode3.front();
                        for (size_t k = 0; k < m_atNode3.size(); ++k)
                        {
                            if (AngleDiff(m_atNode3[k], 4.7f) <= 0.3f) { allFar = false; culprit = m_atNode3[k]; break; }
                        }
                        for (size_t k = 0; k < m_atNode3.size(); ++k)
                        {
                            if (AngleDiff(m_atNode3[k], travel) > 0.3f) { allTravel = false; strayed = m_atNode3[k]; break; }
                        }
                        char buf[160];
                        if (allFar && allTravel)
                        {
                            snprintf(buf, sizeof(buf), "travelFacingElse=OK(facing %.3f at node 3: %.3f rad from the node's 4.7, %.3f rad from the 2->3 bearing %.3f)",
                                     m_atNode3.front(), AngleDiff(m_atNode3.front(), 4.7f), AngleDiff(m_atNode3.front(), travel), travel);
                        }
                        else if (!allFar) { snprintf(buf, sizeof(buf), "travelFacingElse=BUG(faced the node's orientation %.3f)", culprit); }
                        else { snprintf(buf, sizeof(buf), "travelFacingElse=BUG(facing %.3f is %.3f rad off the 2->3 bearing %.3f)", strayed, AngleDiff(strayed, travel), travel); }
                        travelFacingElse = buf;
                    }
                    Verdict(facesAtWait + " | " + travelFacingElse);
                });
            }

        private:
            std::vector<float> m_atNode3;   ///< facings OnInform captured at node 3's arrival
            uint32             m_low = 0;   ///< this scenario's own chicken, for the hook's filter
        };

        /// Task 5 / patrol-hook-sets-next-node (47): kHookPath, a plain external square. The
        /// hook reacts to the MOVE_START inform for node 2 (fired right after node 1) by
        /// calling SetNextWaypoint(4) from inside the inform, the same reentrant pattern
        /// family 1's InformReentersFacade (ScenariosSimple.cpp) uses for a different facade
        /// call. `hookHonoured` reads the next arrival off the redirected node; `noStall`
        /// proves the patrol keeps moving afterwards.
        class PatrolHookSetsNextNode : public Scenario
        {
        public:
            PatrolHookSetsNextNode() : Scenario("patrol-hook-sets-next-node", 47) {}

            /// The recording hook (Scenario.h): called synchronously from inside the native's
            /// MOVE_START inform, before the driver prepares the leg toward the named node --
            /// installing SetNextWaypoint here IS reentering the facade from inside the inform.
            void OnPathInform(Creature* creature, uint32 pathId, Motion::PathEvent event, uint32 node) override
            {
                if (creature->GetGUIDLow() == m_low && pathId == uint32(kHookPath) && event == Motion::PathEvent::NodeLeft && node == 2 && !m_hooked)
                {
                    const bool ok = creature->GetMotionMaster()->SetNextWaypoint(4);
                    m_hooked = true;
                    m_hookMark = Informs().size();
                    Log("hook: MOVE_START for node 2 -> SetNextWaypoint(4) returned %d", ok ? 1 : 0);
                }
            }

            void Prepare() override
            {
                m_hooked = false;
                m_hookMark = 0;
                m_low = 0;
                Creature* a = Spawn(CHICKEN, P0.x, P0.y, P0.z, 0.0f);
                if (!a)
                {
                    Verdict("hookHonoured=INVALID(spawn failed) | noStall=INVALID(spawn failed)");
                    return;
                }
                Load(P0_FAR.x, P0_FAR.y);
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                m_low = low;   // the hook and the scan below both count only our own chicken
                auto reached = std::make_shared<std::vector<uint32> >();
                At(500, [this, g, low]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveWaypoint(kHookPath, PATH_FROM_EXTERNAL);
                    Log("chicken guid=%u MoveWaypoint on the hook path, mt=%s", low, TypeName(a));
                });
                for (uint32 i = 1; i <= 40; ++i)
                {
                    At(500 + i * 1000, [this, g, reached, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const float px = a->Where().X(), py = a->Where().Y();
                        const uint32 wp = Node(a);
                        if (reached->empty() || reached->back() != wp)
                        {
                            reached->push_back(wp);
                            Log("+%3us at %.1f %.1f mt=%s at node %u", i, px, py, TypeName(a), wp);
                        }
                    });
                }
                At(41500, [this]()
                {
                    if (!m_hooked)
                    {
                        Verdict("hookHonoured=INVALID(hook never fired) | noStall=INVALID(hook never fired)");
                        return;
                    }
                    uint32 nextArrival = 0;
                    bool found = false;
                    uint32 arrivals = 0;
                    for (size_t k = m_hookMark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow != m_low || r.event != Inform::Event::PathInform || r.pathId != uint32(kHookPath) || r.pathEvent != Motion::PathEvent::NodeReached) { continue; }
                        ++arrivals;
                        if (!found && r.id != 1) { found = true; nextArrival = r.id; }
                    }
                    char hookHonoured[96];
                    if (found && nextArrival == 4) { snprintf(hookHonoured, sizeof(hookHonoured), "hookHonoured=OK(after node 1 the next arrival was 4)"); }
                    else { snprintf(hookHonoured, sizeof(hookHonoured), "hookHonoured=BUG(next arrival was %u)", nextArrival); }
                    char noStall[64];
                    if (arrivals >= 3) { snprintf(noStall, sizeof(noStall), "noStall=OK(%u arrivals after the hook)", arrivals); }
                    else { snprintf(noStall, sizeof(noStall), "noStall=BUG(%u arrivals after the hook)", arrivals); }
                    Verdict(std::string(hookHonoured) + " | " + noStall);
                });
            }

        private:
            bool   m_hooked;
            size_t m_hookMark;
            uint32 m_low = 0;    ///< this scenario's own chicken, for the hook's and the scan's filter
        };
    }

    void RegisterDefaultScenarios(Runner& r)
    {
        r.Register(new PatrolWelded());
        r.Register(new PatrolOrientsAtAWaitingNode());
        r.Register(new PatrolHookSetsNextNode());
    }
}
