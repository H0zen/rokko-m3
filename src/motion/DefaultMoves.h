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

#ifndef MANGOS_MOTION_DEFAULTMOVES_H
#define MANGOS_MOTION_DEFAULTMOVES_H

#include "BehaviourModel.h"

// The two default behaviours a creature falls back to when nothing else claims it, as pure
// kernel policy (P5-B family 2): WanderBehaviour replaces RandomMovementGenerator, and
// PatrolBehaviour replaces WaypointMovementGenerator together with its WaypointSmoothing
// helper -- all three deleted on this branch, the smoothing moved into the kernel as
// PatrolWelding.h. Everything the shell used to do around them (the draws, the routes, the
// live unit reads) is a Services call now.

namespace Motion
{
    /// The idle wander (design §4.1): a hop to a random point in the leash, a rest, again;
    /// a flier orbits an inclined ellipse instead. Every draw is the generator's, in its order.
    /// Whether it flies is not a parameter: the tick asks Services::CanFly() live, as the
    /// generator's Airborne(owner) did on every Intent.
    class WanderBehaviour : public Behaviour
    {
        public:
            struct Params
            {
                Vector3 centre;
                float   radius = 0.1f;      ///< clamped at 0.1 by the caller
                /// The vertical band of the orbit; > 0 offers the flight, but only a unit whose
                /// live Services::CanFly() says so actually orbits (the generator's rule).
                float   verticalZ = 0.0f;
            };
            explicit WanderBehaviour(Params const& p) : m_p(p) {}
            Motion::Kind Kind() const override { return Motion::Kind::Wander; }
            Step Activate(Sight const& sight, Services& svc) override;
            Step Suspend() override;
            Step Resume(Sight const& sight, Services& svc, bool reset) override;
            Step Tick(Sight const& sight, Services& svc, uint32 diff) override;
            FinishReason EndReason(Sight const&) const override { return FinishReason::Expired; }
            Outcome Finish(FinishReason why, Sight const& sight, Services& svc) override;
        private:
            Vector3 NextOrbitPoint(Services& svc);
            uint32  RetryDelay();
            Params  m_p;
            float   m_orbitTilt = 0.0f;
            float   m_orbitAngle = 0.0f;
            int32   m_rest = 0;            ///< ms left standing; <= 0 = passed
            bool    m_haveHop = false;
            Vector3 m_hop;
            uint32  m_retries = 0;
            bool    m_lastRunning = false; ///< Suspend() has no Sight, so the walk restore reads the last tick's running state; equivalent to the generator's live read since the wander's own leg never sets UNIT_STAT_RUNNING, and a chase/fear sets it only after the wander is suspended.
    };

    /// A waypoint patrol (design §4.2): the generator's node loop as continuation steps.
    class PatrolBehaviour : public Behaviour
    {
        public:
            struct Node
            {
                uint32  id = 0;
                Vector3 pos;
                float   orientation = 100.0f;   ///< 100 = none
                uint32  delay = 0;
                uint32  scriptId = 0;
                uint32  emote = 0;
                uint32  spell = 0;
                std::vector<int32> textIds;     ///< the leading non-zero text ids (the generator's text branch fires on textid[0])
                bool    textAnywhere = false;   ///< any of the five text ids non-zero (the generator's WaypointBehavior::isEmpty() rule)
                uint32  model1 = 0;
                uint32  model2 = 0;
                /// The generator's `behavior != nullptr && !behavior->isEmpty()`: emote, spell, either model, or any text id.
                bool HasBehavior() const { return emote || spell || model1 || model2 || textAnywhere; }
            };
            struct Params
            {
                int32  pathId = 0;
                uint32 origin = 0;         ///< the shell's WaypointPathOrigin value, opaque here
                bool   external = false;   ///< origin == PATH_FROM_EXTERNAL && pathId > 0: the informs go out as PathInform(pathId, ...) instead of Inform(Patrol, node)
                bool   externalOrigin = false; ///< origin == PATH_FROM_EXTERNAL (any pathId): no welding, the path may be replaced under us
                std::vector<Node> nodes;   ///< in path order (ascending id)
                uint32 initialDelay = 0;
                uint32 revision = 0;       ///< opaque: the shell's path revision the nodes were read at
                uint32 entry = 0;          ///< opaque: the entry the shell resolved the path with, so a refresh resolves the same path
            };
            explicit PatrolBehaviour(Params const& p);
            Motion::Kind Kind() const override { return Motion::Kind::Patrol; }
            Step Activate(Sight const& sight, Services& svc) override;
            Step Suspend() override;
            Step Resume(Sight const& sight, Services& svc, bool reset) override;
            Step Tick(Sight const& sight, Services& svc, uint32 diff) override;
            FinishReason EndReason(Sight const&) const override { return FinishReason::Expired; }
            Outcome Finish(FinishReason why, Sight const& sight, Services& svc) override;
            bool ResetPosition(Sight const& sight, Services& svc, Vector3& pos, float& o) const override;
            // The facade's reads and commands (MotionMaster::HeldPatrol)
            Step Pause(int32 ms);                       ///< the generator's Pause: stop, the segment cleared, the wait started unless one runs
            void AddToPauseTime(int32 diff);
            bool SetNextWaypoint(uint32 pointId);       ///< true when the node exists; the caller resets the driver's leg
            uint32 CurrentNode() const { return m_currentNode; }
            uint32 LastReached() const { return m_lastReached; }
            int32  PathId() const { return m_p.pathId; }
            uint32 Origin() const { return m_p.origin; }
            uint32 Revision() const { return m_p.revision; }   ///< the shell's path revision these nodes were read at
            uint32 Entry() const { return m_p.entry; }         ///< the entry the shell resolved the path with
            /// Takes a freshly read copy of the path: the generator held a live pointer at the
            /// manager's node map, so a GM's waypoint edit reached a running patrol at once.
            /// Only the nodes and the revision are replaced -- the current node, the last
            /// reached one, the wait, the pending node delay, the segment and the leg in flight
            /// are the patrol's own progress and stay. The next prepare reads the new nodes
            /// through IndexOf; a current node that no longer exists takes the "not found ->
            /// Hold" path, and an empty replacement makes HasPath() false.
            void ReplaceNodes(std::vector<Node> const& nodes, uint32 revision) { m_p.nodes = nodes; m_p.revision = revision; }
            bool   HasPath() const { return !m_p.nodes.empty(); }
            size_t LegPointCount() const { return m_legPoints.size(); }   ///< the harness's welding measurement
        private:
            enum class Phase : uint8 { Fresh, Arrivals, PrepareInform };
            struct SegmentWaypoint { uint32 pointId; size_t pathPointIndex; };
            Node const* Find(uint32 id) const;
            size_t IndexOf(uint32 id) const;             ///< nodes.size() when absent
            bool Stopped(Services& svc) const { return m_wait > 0 || svc.WaypointPaused(); }
            bool CanMove(Services& svc, uint32 diff);
            void Stop(int32 ms) { m_wait = ms; }
            /// The tracked segment goes, and with it every arrival it still owed: the
            /// generator's SetNextWaypoint/Pause cleared m_segment, which ended
            /// ProcessSegmentProgress's while loop at once, so the nodes the spline had passed
            /// were never informed. Only the finalized branch's latch-guarded trailing
            /// OnArrived still ran afterwards -- the trailing `{0, false}` entry stays.
            void ClearSegment();
            Step ArrivalStep(Services& svc);              ///< one node's state and effects, in the generator's order
            Step StartPrepare(Sight const& sight, Services& svc);
            Step PrepareLeg(Sight const& sight, Services& svc, size_t currIndex);
            Step WalkPreparedLeg() const;
            void BuildSmoothPath(Services& svc, Vector3 const& moverPos, size_t startIndex);
            bool AppendLeg(Services& svc, Vector3 const& start, Node const& end);
            void CollectArrivals(int32 pathIndex);       ///< the nodes the spline passed since the last tick, in order
            struct Arrival
            {
                uint32 pointId;     ///< the node reached; unused (0) when !fromSegment -- the latch alone decides the trailing arrival
                bool   fromSegment; ///< ProcessSegmentProgress's arrivals clear the latch first
            };
            Params m_p;                                  ///< the loaded path and its origin/inform types
            uint32 m_currentNode = 0;                    ///< the generator's m_currentNode: reached, or being walked to
            uint32 m_lastReached = 0;                    ///< the generator's m_lastReachedWaypoint
            int32  m_wait = 0;                            ///< the generator's m_nextMoveTime (TimeTracker), in raw milliseconds
            bool   m_arrivalDone = false;                ///< the generator's m_isArrivalDone
            std::vector<SegmentWaypoint> m_segment;      ///< the generator's m_segment: the smoothed leg's tracked waypoints, in order
            size_t m_segmentArrivals = 0;                ///< the generator's m_segmentArrivals: how many of m_segment have been passed
            PointsArray m_legPoints;                     ///< owned; stable for the leg's life (Along is non-owning)
            Vector3 m_legEnd;                            ///< the generator's m_legEnd
            Facing  m_legFacing;                         ///< the generator's m_legFacing
            bool    m_legWalk = true;                    ///< the generator's m_legWalk
            bool    m_haveLeg = false;                   ///< the generator's m_haveLeg
            uint32  m_deadNodes = 0;                     ///< the generator's m_deadNodes: unreachable nodes skipped in a row
            bool    m_forceNextLeg = false;              ///< the generator's m_forceNextLeg: a whole lap was unreachable, walk the next leg unrouted
            bool    m_approached = false;                ///< the generator's m_approached: a partial leg already got as close as it gets
            // the continuation
            Phase   m_phase = Phase::Fresh;              ///< no generator counterpart: the generator ran OnArrived/PrepareMove to completion inline
            std::vector<Arrival> m_pendingArrivals;      ///< nodes to arrive at, in order
            bool    m_finalizedSegment = false;          ///< the arrivals came from a finalized spline: prepare after them
            bool    m_reachedLast = false;                ///< the generator's local `reachedLast` in PrepareMove, kept across the external inform's round
            uint32  m_nextAfterInform = 0;               ///< the node the prepare inform named
            uint32  m_nodeBeforeInform = 0;              ///< m_currentNode when the prepare inform fired: a hook's SetNextWaypoint shows as a change
            int32   m_delayAfterEffects = 0;             ///< the arrived node's delay, waiting to be started: the generator's OnArrived ran Stop(node.delay) as its LAST line, after the inform hook
            bool    m_delayPending = false;              ///< an arrival that reached its effects owes that Stop; the next round starts it before anything else, so the node's delay overwrites whatever the hook installed, and it survives Suspend/Resume as the generator's timer did
            bool    m_lastRunning = false;               ///< Suspend() has no Sight: the last tick's running state, as WanderBehaviour's m_lastRunning
    };
}

#endif
