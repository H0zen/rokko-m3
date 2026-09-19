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

#include "DefaultMoves.h"
#include "PatrolWelding.h"

#include <algorithm>
#include <cmath>

namespace Motion
{
    namespace
    {
        constexpr int    CHANCE_NO_BREAK = 30;
        constexpr uint32 REST_AFTER_HOP_MIN = 3000;
        constexpr uint32 REST_AFTER_HOP_MAX = 10000;
        constexpr uint32 RETRY_DELAY = 50;
        constexpr float  MIN_FLIGHT_CLEARANCE = 0.5f;
        constexpr float  ORBIT_STEP_MIN = 0.45f;
        constexpr float  ORBIT_STEP_MAX = 1.15f;
        constexpr float  ORBIT_RADIUS_MIN_FACTOR = 0.55f;
        constexpr float  TWO_PI = 6.28318530718f;
    }

    Step WanderBehaviour::Activate(Sight const& sight, Services& svc)
    {
        // The generator's Initialize: ROAMING, both orbit draws (always), the rest cleared, no hop.
        m_lastRunning = sight.runningState;
        m_orbitTilt = svc.Frand(0.0f, TWO_PI);
        m_orbitAngle = svc.Frand(0.0f, TWO_PI);
        m_rest = 0;
        m_haveHop = false;
        // m_retries is untouched: the generator's Initialize never reset it either (git history: the deleted wander generator's Initialize).
        Step s;
        s.roaming = Roaming::SetRoam;   // ROAMING alone; ROAMING_MOVE follows the first hop
        s.resetLeg = true;
        return s;
    }

    Step WanderBehaviour::Suspend()
    {
        // The generator's Interrupt: InterruptMoving, Finalize (bits cleared, walk restored), the hop forgotten, the leg reset.
        Step s;
        s.interrupt = true;
        s.roaming = Roaming::ClearBoth;
        s.effects.push_back(Effect::Walk(!m_lastRunning));
        s.resetLeg = true;
        m_haveHop = false;
        return s;
    }

    Step WanderBehaviour::Resume(Sight const& sight, Services& svc, bool reset)
    {
        return reset ? Activate(sight, svc) : Step::None();   // the generator's Reset is its Initialize
    }

    uint32 WanderBehaviour::RetryDelay()
    {
        const uint32 delay = std::min<uint32>(RETRY_DELAY << m_retries, REST_AFTER_HOP_MIN);
        if (delay < REST_AFTER_HOP_MIN)
        {
            ++m_retries;
        }
        return delay;
    }

    Vector3 WanderBehaviour::NextOrbitPoint(Services& svc)
    {
        m_orbitAngle += svc.Frand(ORBIT_STEP_MIN, ORBIT_STEP_MAX);
        if (m_orbitAngle > TWO_PI)
        {
            m_orbitAngle -= TWO_PI;
        }
        const float r = m_p.radius * svc.Frand(ORBIT_RADIUS_MIN_FACTOR, 1.0f);
        Vector3 p(m_p.centre.x + r * std::cos(m_orbitAngle),
                  m_p.centre.y + r * std::sin(m_orbitAngle),
                  m_p.centre.z + m_p.verticalZ * std::sin(m_orbitAngle - m_orbitTilt));
        float floorZ = 0.0f;
        if (svc.Ground(p, floorZ))
        {
            p.z = std::max(p.z, floorZ + MIN_FLIGHT_CLEARANCE);
        }
        return p;
    }

    Step WanderBehaviour::Tick(Sight const& sight, Services& svc, uint32 diff)
    {
        m_lastRunning = sight.runningState;
        // The generator asked Airborne(owner) here, at the top of every Intent, and it re-read
        // CanFly() each time: a leash laid while the unit could fly is walked on foot the moment
        // it cannot, and the other way round. The port answers live, so this does the same.
        const bool airborne = m_p.verticalZ > 0.0f && svc.CanFly();
        const uint32 hopFlags = airborne ? (MOVE_FLY | MOVE_STRAIGHT) : MOVE_WALK;

        if (!sight.alive || !sight.canMove)
        {
            m_rest = 0;
            Step s = Step::Of(MoveIntent::Hold());
            s.roaming = Roaming::ClearMove;
            return s;
        }
        if (sight.status.blocked)
        {
            m_haveHop = false;
            m_rest = int32(RetryDelay());
        }
        if (sight.status.arrived || (sight.status.traveling && m_haveHop))
        {
            m_retries = 0;
        }
        if (sight.status.traveling && m_haveHop)
        {
            return Step::Of(MoveIntent::Move(m_hop, hopFlags));
        }
        m_rest -= int32(diff);
        if (m_rest > 0)
        {
            return Step::Of(MoveIntent::Hold());
        }
        if (airborne)
        {
            m_hop = NextOrbitPoint(svc);
        }
        else
        {
            Vector3 hop;
            if (!svc.RandomPoint(m_p.centre, m_p.radius, hop))
            {
                m_rest = int32(RetryDelay());
                return Step::Of(MoveIntent::Hold());
            }
            m_hop = hop;
        }
        m_haveHop = true;
        // The rest after THIS hop is decided now; a flier never rests (its arcs join).
        m_rest = int32((airborne || svc.Irand(0, 99) < CHANCE_NO_BREAK)
            ? RETRY_DELAY
            : svc.Urand(REST_AFTER_HOP_MIN, REST_AFTER_HOP_MAX));
        Step s = Step::Of(MoveIntent::Move(m_hop, hopFlags));
        s.roaming = Roaming::SetMove;   // the generator adds ROAMING_MOVE here (ROAMING was set at Initialize)
        return s;
    }

    Outcome WanderBehaviour::Finish(FinishReason why, Sight const& sight, Services& /*svc*/)
    {
        // Finalize runs for every reason (ClearBoth, the walk restored); a displacing finish
        // is the generator's Interrupt, the stop first -- the shell skips it when already suspended.
        Outcome o;
        o.roaming = Roaming::ClearBoth;
        o.interrupt = Displacing(why);
        o.effects.push_back(Effect::Walk(!sight.runningState));
        return o;
    }

    namespace
    {
        constexpr int32 SKIP_DEAD_NODE_DELAY = 50;
        constexpr float TWO_PI_F = 6.28318530718f;
    }

    PatrolBehaviour::PatrolBehaviour(Params const& p) : m_p(p)
    {
        if (!m_p.nodes.empty())
        {
            m_currentNode = m_p.nodes.front().id;
        }
        m_wait = int32(m_p.initialDelay);   // InitializeWaypointPath's m_nextMoveTime.Reset(initialDelay)
    }

    PatrolBehaviour::Node const* PatrolBehaviour::Find(uint32 id) const
    {
        const size_t i = IndexOf(id);
        return i < m_p.nodes.size() ? &m_p.nodes[i] : nullptr;
    }

    size_t PatrolBehaviour::IndexOf(uint32 id) const
    {
        for (size_t i = 0; i < m_p.nodes.size(); ++i) { if (m_p.nodes[i].id == id) { return i; } }
        return m_p.nodes.size();
    }

    Step PatrolBehaviour::Activate(Sight const& sight, Services&)
    {
        // Initialize: ROAMING, the paused bit cleared, no leg.
        m_lastRunning = sight.runningState;
        m_haveLeg = false;
        Step s;
        s.roaming = Roaming::SetRoam;
        s.effects.push_back(Effect(Effect::ClearWaypointPaused));
        s.resetLeg = true;
        return s;
    }

    Step PatrolBehaviour::Resume(Sight const& sight, Services&, bool reset)
    {
        if (!reset) { return Step::None(); }
        // Reset: ROAMING, no leg; the paused bit untouched.
        m_lastRunning = sight.runningState;
        m_haveLeg = false;
        Step s;
        s.roaming = Roaming::SetRoam;
        s.resetLeg = true;
        return s;
    }

    Step PatrolBehaviour::Suspend()
    {
        // Interrupt: InterruptMoving, Finalize (segment cleared, no leg, bits cleared, walk restored), the leg reset.
        ClearSegment();
        m_haveLeg = false;
        m_phase = Phase::Fresh;
        m_pendingArrivals.clear();
        Step s;
        s.interrupt = true;
        s.roaming = Roaming::ClearBoth;
        s.effects.push_back(Effect::Walk(!m_lastRunning));
        s.resetLeg = true;
        return s;
    }

    Outcome PatrolBehaviour::Finish(FinishReason why, Sight const& sight, Services&)
    {
        // Finalize for every reason; a displacing finish (Superseded/Overridden/Cancelled) was the
        // generator's Interrupt, which stopped the mover first (the shell skips the stop when suspended).
        ClearSegment();
        m_haveLeg = false;
        Outcome o;
        o.interrupt = Displacing(why);   // Motion::Displacing, hoisted into BehaviourModel.h by Task 2's fix round
        o.roaming = Roaming::ClearBoth;
        o.effects.push_back(Effect::Walk(!sight.runningState));
        return o;
    }

    Step PatrolBehaviour::Pause(int32 ms)
    {
        ClearSegment();
        m_haveLeg = false;
        if (m_wait <= 0) { Stop(ms); }
        Step s;
        s.stop = true;
        return s;
    }

    void PatrolBehaviour::AddToPauseTime(int32 diff)
    {
        m_wait -= diff;
        if (m_wait <= 0) { m_wait = 0; }
    }

    bool PatrolBehaviour::SetNextWaypoint(uint32 pointId)
    {
        if (!Find(pointId)) { return false; }
        m_wait = 1;
        m_arrivalDone = false;
        ClearSegment();
        m_haveLeg = false;
        m_currentNode = pointId;
        return true;
    }

    bool PatrolBehaviour::CanMove(Services& svc, uint32 diff)
    {
        m_wait -= int32(diff);
        if (m_wait <= 0 && svc.WaypointPaused())
        {
            m_wait = 1;
        }
        return m_wait <= 0 && !svc.WaypointPaused();
    }

    void PatrolBehaviour::ClearSegment()
    {
        m_segment.clear();
        m_segmentArrivals = 0;
        // The arrivals the segment still owed go with it: the generator's SetNextWaypoint and
        // Pause cleared m_segment, and ProcessSegmentProgress's while loop -- the only thing
        // that informed a passed node -- ended on the spot. The trailing `{0, false}` entry is
        // the finalized branch's own latch-guarded OnArrived, which ran regardless, so it stays.
        m_pendingArrivals.erase(std::remove_if(m_pendingArrivals.begin(), m_pendingArrivals.end(),
                                               [](Arrival const& a) { return a.fromSegment; }),
                                m_pendingArrivals.end());
    }

    void PatrolBehaviour::CollectArrivals(int32 pathIndex)
    {
        while (m_segmentArrivals < m_segment.size() &&
               HasReachedWaypointEndpoint(pathIndex, m_segment[m_segmentArrivals].pathPointIndex))
        {
            m_pendingArrivals.push_back({m_segment[m_segmentArrivals].pointId, true});
            ++m_segmentArrivals;
        }
    }

    Step PatrolBehaviour::ArrivalStep(Services& svc)
    {
        // One node, the generator's OnArrived in its order: the state first (lastReached, the
        // latch, ROAMING_MOVE cleared), then script, emote, spell, display, text, inform, the wait.
        const Arrival a = m_pendingArrivals.front();
        m_pendingArrivals.erase(m_pendingArrivals.begin());
        if (a.fromSegment)
        {
            m_currentNode = a.pointId;
            m_arrivalDone = false;   // ProcessSegmentProgress cleared the latch before each OnArrived
        }
        Step s;
        s.again = true;
        m_lastReached = m_currentNode;
        m_deadNodes = 0;
        if (m_arrivalDone) { return s; }
        s.roaming = Roaming::ClearMove;
        m_arrivalDone = true;
        Node const* node = Find(m_currentNode);
        if (!node) { return s; }
        if (node->scriptId) { s.effects.push_back(Effect(Effect::RunScript, Motion::Kind::Patrol, node->scriptId)); }
        if (node->emote) { s.effects.push_back(Effect(Effect::Emote, Motion::Kind::Patrol, node->emote)); }
        if (node->spell) { s.effects.push_back(Effect(Effect::CastSpell, Motion::Kind::Patrol, node->spell)); }
        if (node->model1) { s.effects.push_back(Effect(Effect::SetDisplay, Motion::Kind::Patrol, node->model1)); }
        if (!node->textIds.empty())
        {
            int32 textId = node->textIds[0];
            if (node->textIds.size() > 1)
            {
                textId = node->textIds[svc.Urand(0, uint32(node->textIds.size() - 1))];
            }
            s.effects.push_back(Effect(Effect::Say, Motion::Kind::Patrol, uint32(textId)));
        }
        s.effects.push_back(m_p.external ? Effect::Path(uint32(m_p.pathId), PathEvent::NodeReached, m_currentNode)
                                         : Effect(Effect::Inform, Motion::Kind::Patrol, m_currentNode));
        // The generator's Stop(node.delay) was OnArrived's last line -- it ran AFTER the inform
        // hook, so it overwrote whatever that hook installed (a SetNextWaypoint's 1 ms, a Pause's
        // own timer). The shell performs this step's effects only after it returns, so the Stop
        // is owed rather than done: the next round starts it before anything else.
        m_delayAfterEffects = int32(node->delay);
        m_delayPending = true;
        return s;
    }
    // (The generator re-added ROAMING_MOVE after each arrival while the spline still ran and nothing stopped
    //  it (ProcessSegmentProgress); the bit's state after the last arrival is what matters, and the hook of
    //  every arrival saw it cleared. The step that ends the Arrivals phase in Tick carries that re-add.)

    Step PatrolBehaviour::StartPrepare(Sight const& sight, Services& svc)
    {
        // PrepareMove up to its hook.
        m_haveLeg = false;
        m_approached = false;
        m_legPoints.clear();
        if (m_p.nodes.empty() || Stopped(svc)) { return Step::Of(MoveIntent::Hold()); }
        // The generator re-read IsAlive/UNIT_STAT_NOT_MOVE here, live, after the node's effects
        // had run: the spell an arrival cast may have rooted or stunned the unit since the
        // Sight was taken. Death after the effects is the shell's guard, which drops this
        // round's intent outright; the movement bit is the port's to answer.
        if (!sight.alive || !svc.CanMove()) { return Step::Of(MoveIntent::Hold()); }
        size_t curr = IndexOf(m_currentNode);
        if (curr >= m_p.nodes.size()) { return Step::Of(MoveIntent::Hold()); }
        Step s;
        Node const& at = m_p.nodes[curr];
        if (at.HasBehavior())
        {
            if (at.model2) { s.effects.push_back(Effect(Effect::SetDisplay, Motion::Kind::Patrol, at.model2)); }
            // The generator cleared the emote state here (line 496) before PrepareLeg's ROAMING_MOVE
            // add; the shell performs a step's roaming write before its effects, so the order is
            // reversed on this internal path -- unobservable, since nothing runs between the two in
            // the same tick. The external path, the only one with a hook in between, keeps the
            // generator's actual order (the hook ran before ROAMING_MOVE there too): ROAMING_MOVE is
            // not set until the PrepareInform continuation calls PrepareLeg, after the hook.
            s.effects.push_back(Effect(Effect::ClearEmoteState));
        }
        if (m_arrivalDone)
        {
            m_reachedLast = false;
            size_t next = curr + 1;
            if (next >= m_p.nodes.size()) { m_reachedLast = true; next = 0; }
            if (m_p.external)
            {
                // The external start/last inform ends the round: its hook may replace us (the
                // shell's own re-check after every round's effects catches that) or set the
                // next node, which the PrepareInform continuation below honours.
                m_nodeBeforeInform = m_currentNode;
                m_nextAfterInform = m_p.nodes[next].id;
                s.effects.push_back(Effect::Path(uint32(m_p.pathId), m_reachedLast ? PathEvent::LastWaitEnded : PathEvent::NodeLeft, m_p.nodes[next].id));
                s.again = true;
                m_phase = Phase::PrepareInform;
                return s;
            }
            curr = next;
            m_currentNode = m_p.nodes[curr].id;
        }
        Step leg = PrepareLeg(sight, svc, curr);
        leg.effects.insert(leg.effects.begin(), s.effects.begin(), s.effects.end());
        return leg;
    }
    // (The generator's PrepareMove: WaypointMovementGenerator.cpp:471-566. Its m_isArrivalDone advance, the
    //  external inform, the re-check and the hook's node all live here and in the PrepareInform continuation.)

    Step PatrolBehaviour::PrepareLeg(Sight const& sight, Services& svc, size_t currIndex)
    {
        m_arrivalDone = false;
        // sight.position stands in for the generator's frame.MoverPosition(creature); for the
        // world frame they coincide, and a patrol on a transport was outside the generator's
        // scope too.
        BuildSmoothPath(svc, sight.position, currIndex);
        Node const* finalNode = &m_p.nodes[currIndex];
        if (!m_legPoints.empty())
        {
            // The id always exists (BuildSmoothPath only ever committed real node ids from
            // m_p.nodes); the fallback to the node we started from is a kernel type never
            // aborting the server, not a path this should ever actually take.
            if (Node const* welded = Find(m_segment.back().pointId))
            {
                finalNode = welded;
            }
        }
        m_legEnd = finalNode->pos;
        m_legFacing = (finalNode->orientation != 100.0f && finalNode->delay != 0)
            ? Facing::ToAngle(finalNode->orientation)
            : Facing();
        m_haveLeg = true;
        m_legWalk = !sight.runningState && !sight.levitating;
        Step s = WalkPreparedLeg();
        s.roaming = Roaming::SetMove;
        s.effects.push_back(Effect::Walk(m_legWalk));
        return s;
    }

    Step PatrolBehaviour::WalkPreparedLeg() const
    {
        uint32 flags = m_forceNextLeg ? MOVE_NONE : MOVE_REQUIRE_PATH;
        if (m_legWalk) { flags |= MOVE_WALK; }
        MoveIntent intent = MoveIntent::Move(m_legEnd, flags, m_legFacing);
        if (!m_legPoints.empty()) { intent.Along(m_legPoints); }
        return Step::Of(intent);
    }

    bool PatrolBehaviour::AppendLeg(Services& svc, Vector3 const& start, Node const& end)
    {
        PointsArray leg;
        const RouteResult r = svc.Route(start, end.pos, leg);
        if (!r.usable || !r.routed) { return false; }
        if (leg.size() < 2) { return false; }
        if (!m_legPoints.empty() && (leg.front() - m_legPoints.back()).length() >= WAYPOINT_SMOOTHING_MIN_SEGMENT_LENGTH) { return false; }
        if (m_legPoints.empty()) { m_legPoints.push_back(leg.front()); }
        for (size_t i = 1; i < leg.size(); ++i)
        {
            if ((leg[i] - m_legPoints.back()).length() >= WAYPOINT_SMOOTHING_MIN_SEGMENT_LENGTH) { m_legPoints.push_back(leg[i]); }
        }
        return true;
    }

    void PatrolBehaviour::BuildSmoothPath(Services& svc, Vector3 const& moverPos, size_t startIndex)
    {
        // The generator's BuildSmoothPath (WaypointMovementGenerator.cpp:352-449) over the route port.
        ClearSegment();
        m_legPoints.clear();
        // An externally-scripted path is walked node by node: its script may replace the path
        // under us at any node, so welding ahead through it is not safe.
        if (m_p.externalOrigin) { return; }
        // The generator built its router here, one per pass, and the legs of the pass shared it.
        // The mesh router is stateful (it reuses the poly path of its previous Calculate), so a
        // pass that inherited the last one's state would not weld the same geometry.
        svc.ResetRoute();
        Vector3 start = moverPos;
        WaypointSmoothingBounds bounds;
        size_t curr = startIndex;
        for (size_t segment = 0; segment < WAYPOINT_SMOOTHING_MAX_LOOKAHEAD; ++segment)
        {
            Node const& node = m_p.nodes[curr];
            const size_t committed = m_legPoints.size();
            if (!AppendLeg(svc, start, node))
            {
                // The FIRST leg failing means nothing is usable: a plain routed leg instead. A later
                // one failing keeps the chunk built so far.
                if (committed == 0)
                {
                    ClearSegment();
                    m_legPoints.clear();
                    return;
                }
                break;
            }
            // The first waypoint is always accepted (a one-waypoint chunk is dropped below); each
            // subsequent one only while the whole path stays within the packable budget.
            WaypointSmoothingBounds trial = bounds;
            for (size_t i = committed; i < m_legPoints.size(); ++i)
            {
                AddWaypointSmoothingPoint(trial, m_legPoints[i].x, m_legPoints[i].y, m_legPoints[i].z);
            }
            if (committed != 0 && !IsWaypointSmoothingWithinBudget(trial))
            {
                m_legPoints.resize(committed);
                break;
            }
            bounds = trial;
            m_segment.push_back({node.id, m_legPoints.size() - 1});
            // A node that pauses, emotes or runs a script is where the creature must stop: the weld ends there.
            WaypointSmoothingNode smoothing;
            smoothing.hasDelay = node.delay != 0;
            smoothing.hasScript = node.scriptId != 0;
            smoothing.hasBehavior = node.HasBehavior();
            if (!IsWaypointSmoothingSafe(smoothing)) { break; }
            size_t next = curr + 1;
            if (next >= m_p.nodes.size()) { next = 0; }
            // A full lap: stop before welding the path onto itself.
            if (next == startIndex) { break; }
            start = node.pos;
            curr = next;
        }
        // A chunk of one waypoint is not a smoothed segment; the driver routes it.
        if (m_segment.size() <= 1 || m_legPoints.size() < 2)
        {
            ClearSegment();
            m_legPoints.clear();
        }
    }

    Step PatrolBehaviour::Tick(Sight const& sight, Services& svc, uint32 diff)
    {
        m_lastRunning = sight.runningState;
        // The Stop the last arrival owed (ArrivalStep), started here because the generator ran it
        // after the node's effects and their hooks: a hook's own wait loses to the node's delay.
        if (m_delayPending)
        {
            Stop(m_delayAfterEffects);
            m_delayPending = false;
        }
        if (m_phase == Phase::Arrivals)
        {
            if (!m_pendingArrivals.empty()) { return ArrivalStep(svc); }
            m_phase = Phase::Fresh;
            if (m_finalizedSegment)
            {
                ClearSegment();
                if (Stopped(svc)) { m_haveLeg = false; return Step::Of(MoveIntent::Hold()); }
                return StartPrepare(sight, svc);
            }
            Step s = m_haveLeg ? WalkPreparedLeg() : Step::Of(MoveIntent::Hold());
            if (!Stopped(svc))
            {
                s.roaming = Roaming::SetMove;   // the generator's re-add after the arrivals of a running spline
            }
            return s;
        }
        if (m_phase == Phase::PrepareInform)
        {
            // The inform's hook has run and the shell found us still selected. The generator
            // compared m_currentNode before and after the inform
            // (nodeBefore): a SetNextWaypoint made inside the hook wins over the named next node.
            m_phase = Phase::Fresh;
            size_t curr = (m_currentNode != m_nodeBeforeInform) ? IndexOf(m_currentNode) : IndexOf(m_nextAfterInform);
            if (curr >= m_p.nodes.size()) { return Step::Of(MoveIntent::Hold()); }
            m_currentNode = m_p.nodes[curr].id;
            // (a SetNextWaypoint also set m_wait = 1 and cleared the segment, as the generator's did;
            //  PrepareMove went on to build the leg regardless, and so does this.)
            return PrepareLeg(sight, svc, curr);
        }
        // A fresh tick: the generator's Intent in its order.
        if (sight.status.cut) { ClearSegment(); m_haveLeg = false; }
        // The generator gated on UNIT_STAT_NOT_MOVE, which also holds UNIT_STAT_DISTRACTED; that
        // bit only mirrors an active Distract command, which masks the patrol at the arbiter
        // before it is ever ticked -- the narrower canMove is equivalent here.
        if (!sight.canMove || m_p.nodes.empty())
        {
            Step s = Step::Of(MoveIntent::Hold());
            s.roaming = Roaming::ClearMove;
            return s;
        }
        if (svc.Casting()) { return Step::Of(MoveIntent::Hold()); }
        if (sight.status.blocked && !m_approached)
        {
            ClearSegment();
            m_arrivalDone = true;
            m_haveLeg = false;
            m_forceNextLeg = false;
            Stop(SKIP_DEAD_NODE_DELAY);
            if (++m_deadNodes >= m_p.nodes.size()) { m_deadNodes = 0; m_forceNextLeg = true; }
        }
        if (Stopped(svc))
        {
            return CanMove(svc, diff) ? StartPrepare(sight, svc) : Step::Of(MoveIntent::Hold());
        }
        if (!m_haveLeg && !sight.status.traveling) { return StartPrepare(sight, svc); }
        if (sight.status.partial && m_haveLeg) { m_approached = true; return WalkPreparedLeg(); }
        const bool asCloseAsItGets = sight.status.blocked && m_approached;
        m_finalizedSegment = asCloseAsItGets || !sight.status.traveling;
        m_pendingArrivals.clear();
        CollectArrivals(sight.status.pathIndex);
        if (m_finalizedSegment)
        {
            // The generator's trailing OnArrived, latch-guarded in ArrivalStep; the id is unused
            // there when !fromSegment, so 0 rather than m_currentNode makes that explicit.
            m_pendingArrivals.push_back({0, false});
        }
        // A weld of up to WAYPOINT_SMOOTHING_MAX_LOOKAHEAD nodes crossed in one tick costs one
        // continuation round per arrival, plus one for the trailing entry above and one for the
        // prepare that drains the phase: the shell's own round cap (its kMaxContinuation, not the
        // kernel's) is sized to cover a full weld, so one tick drains whatever a weld can queue,
        // as the generator's own ProcessSegmentProgress loop did.
        if (!m_pendingArrivals.empty())
        {
            m_phase = Phase::Arrivals;
            return ArrivalStep(svc);
        }
        if (m_finalizedSegment)
        {
            ClearSegment();
            if (Stopped(svc)) { m_haveLeg = false; return Step::Of(MoveIntent::Hold()); }
            return StartPrepare(sight, svc);
        }
        return m_haveLeg ? WalkPreparedLeg() : Step::Of(MoveIntent::Hold());
    }

    bool PatrolBehaviour::ResetPosition(Sight const& sight, Services& svc, Vector3& pos, float& o) const
    {
        // The generator's GetResetPosition, WaypointMovementGenerator.cpp:673-746, over the
        // Services/Sight ports: the combat anchor first, then the last reached node, then the
        // leg that arrived at it. A path lookup miss that the generator asserted never happens
        // is a plain false here: a kernel type never aborts the server.
        if (m_p.nodes.empty()) { return false; }
        Vector3 anchor;
        if (svc.Anchor(anchor))
        {
            pos = anchor;
            o = sight.facing;
            // Face the waypoint it was heading for, so it keeps moving forward on resume.
            Node const* next = Find(m_currentNode);
            if (next)
            {
                const float dx = next->pos.x - pos.x;
                const float dy = next->pos.y - pos.y;
                if (dx != 0.0f || dy != 0.0f)
                {
                    o = std::atan2(dy, dx);
                    o = (o >= 0.0f) ? o : TWO_PI_F + o;
                }
            }
            return true;
        }
        const size_t lastIdx = IndexOf(m_lastReached);
        if (lastIdx >= m_p.nodes.size()) { return false; }
        Node const& curWP = m_p.nodes[lastIdx];
        pos = curWP.pos;
        if (curWP.orientation != 100.0f)
        {
            o = curWP.orientation;
            return true;
        }
        // No orientation on the node: face along the leg that arrived at it.
        const size_t prevIdx = (lastIdx != 0) ? (lastIdx - 1) : (m_p.nodes.size() - 1);
        Node const& prevWP = m_p.nodes[prevIdx];
        o = std::atan2(pos.y - prevWP.pos.y, pos.x - prevWP.pos.x); // returns -Pi..Pi
        o = (o >= 0.0f) ? o : TWO_PI_F + o;
        return true;
    }
}
