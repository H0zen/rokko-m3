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

#include "TaxiMove.h"

#include <algorithm>

namespace Motion
{
    TaxiBehaviour::TaxiBehaviour(Params const& p)
        : m_p(p), m_node(p.startNode), m_legStart(p.startNode), m_legEnd(p.startNode), m_crossing(false), m_crossingConfirmed(false)
    {
        if (m_p.nodes.empty())
        {
            m_node = m_legStart = m_legEnd = 0;
        }
        else if (m_node >= m_p.nodes.size())
        {
            m_node = m_legStart = m_legEnd = m_p.nodes.size() - 1;
        }
    }

    size_t TaxiBehaviour::LegEnd(size_t from) const
    {
        if (from >= m_p.nodes.size())
        {
            return m_p.nodes.size();
        }
        const uint32 map = m_p.nodes[from].mapId;
        for (size_t i = from; i < m_p.nodes.size(); ++i)
        {
            if (m_p.nodes[i].mapId != map)
            {
                return i;
            }
        }
        return m_p.nodes.size();
    }

    Step TaxiBehaviour::LayLeg()
    {
        Step s;
        s.resetLeg = true;
        m_legPoints.clear();
        if (m_p.nodes.empty())
        {
            m_legStart = m_legEnd = 0;
            return s;
        }
        m_legStart = m_node;
        m_legEnd = LegEnd(m_node);
        // The leading slot: MoveSplineInit::Launch overwrites path[0] with the mover's real
        // position without changing the count. The generator pushed the nodes themselves and lost
        // the first under the position; a copy keeps node 0 as the first waypoint after the
        // mover's own position, which is retail's shape (the notes A.2).
        m_legPoints.push_back(m_p.nodes[m_legStart].pos);
        for (size_t i = m_legStart; i < m_legEnd; ++i)
        {
            m_legPoints.push_back(m_p.nodes[i].pos);
        }
        s.apply = true;
        s.intent = MoveIntent::Move(m_legPoints.back(), MOVE_FLY | MOVE_SMOOTH).Along(m_legPoints).AtSpeed(m_p.speed);
        return s;
    }

    Step TaxiBehaviour::Activate(Sight const& /*sight*/, Services& /*svc*/)
    {
        m_crossing = false;
        m_crossingConfirmed = false;
        Step s = LayLeg();
        // Before the leg: the shell's takeoff in retail's order (the stop, the control taken,
        // the mount display and the flags), then the flight spline.
        s.effects.insert(s.effects.begin(), Effect::Takeoff(m_p.mountDisplayId));
        return s;
    }

    Step TaxiBehaviour::Suspend()
    {
        return Step::None();
    }

    Step TaxiBehaviour::Resume(Sight const& /*sight*/, Services& /*svc*/, bool reset)
    {
        if (!reset)
        {
            return Step::None();
        }
        if (m_crossing)
        {
            if (!m_crossingConfirmed)
            {
                // A reset while the far teleport is in flight (nothing on the tree asks one: Clear
                // finishes a taxi and a taxi is never blocked): hold until the ack names the map.
                return Step::None();
            }
            // The worldport ack: the teleport put the mover ON the new map's first node, so the
            // leg starts from the one after it (the generator's SetCurrentNodeAfterTeleport +
            // SkipCurrentNode). Landed on the route's last node: nothing to fly, the next tick ends.
            m_crossing = false;
            m_crossingConfirmed = false;
            std::vector<Effect> seam;
            if (m_p.nodes[m_legEnd].seam)
            {
                seam.push_back(Effect::Seam());   // the node the teleport landed on is a hub the leg skips: the route still advances (finding 2)
            }
            m_node = m_legEnd + 1;
            if (m_node >= m_p.nodes.size())
            {
                m_node = m_p.nodes.empty() ? 0 : m_p.nodes.size() - 1;
                m_legPoints.clear();
                Step s;
                s.resetLeg = true;
                s.effects = seam;
                return s;
            }
            Step leg = LayLeg();
            leg.effects.insert(leg.effects.begin(), seam.begin(), seam.end());
            return leg;
        }
        return LayLeg();
    }

    void TaxiBehaviour::EventsUpTo(size_t target, std::vector<Effect>& effects)
    {
        // The generator's Update: while the driver's node exceeds ours, the departure of the node
        // left, then the arrival of the node reached, in alternation; the reached node's own
        // departure waits for the next advance and the last node's never fires. A seam node fires
        // its arrival only (the generator broke on its arrival and the next hop started at node 1)
        // and advances the route when it is left; a seam left across a map cut is fired by the
        // crossing or the resume instead (there is no tick that leaves it here).
        while (m_node < target && m_node + 1 < m_p.nodes.size())
        {
            Node const& left = m_p.nodes[m_node];
            if (left.seam)
            {
                effects.push_back(Effect::Seam());
            }
            else if (left.departureEvent)
            {
                effects.push_back(Effect::NodeEvent(left.departureEvent, true));
            }
            ++m_node;
            Node const& reached = m_p.nodes[m_node];
            if (reached.arrivalEvent)
            {
                effects.push_back(Effect::NodeEvent(reached.arrivalEvent, false));
            }
        }
    }

    Step TaxiBehaviour::Tick(Sight const& sight, Services& /*svc*/, uint32 /*diff*/)
    {
        Step s;
        s.apply = true;
        if (m_crossing)
        {
            s.intent = MoveIntent::Hold();   // the far teleport is in flight; the worldport ack resumes us
            return s;
        }
        if (m_p.nodes.empty() || sight.status.cut || sight.status.blocked)
        {
            s.intent = MoveIntent::Done();   // nothing to fly, or the leg was cut or refused: EndReason names it
            return s;
        }
        if (!m_legPoints.empty())
        {
            // The node reached, from the driver's index with the leading slot (design fact 2):
            // legStart + max(pathIndex - 1, 0), capped at the leg's last node.
            const size_t reached = m_legStart + size_t(std::max(sight.status.pathIndex - 1, 0));
            EventsUpTo(std::min(reached, m_legEnd - 1), s.effects);
        }
        if (m_node + 1 >= m_p.nodes.size())
        {
            s.intent = MoveIntent::Done();   // the route's last node: the landing is the finish's
            return s;
        }
        if (sight.status.arrived)
        {
            // The map's leg ran out short of the route: the crossing, once, then the hold until
            // the ack. The node advances on the resume, not here: a refused teleport must not skip it.
            m_crossing = true;
            if (m_p.nodes[m_legEnd - 1].seam)
            {
                s.effects.push_back(Effect::Seam());   // the leg's last node is a hub left across the teleport: the route advances before the crossing
            }
            Node const& first = m_p.nodes[m_legEnd];
            s.effects.push_back(Effect::Cross(first.mapId, first.pos, sight.facing));
            s.intent = MoveIntent::Hold();
            return s;
        }
        if (sight.status.traveling && !m_legPoints.empty())
        {
            // The leg re-stated: the same goal along the same points, which the driver keeps.
            s.intent = MoveIntent::Move(m_legPoints.back(), MOVE_FLY | MOVE_SMOOTH).Along(m_legPoints).AtSpeed(m_p.speed);
            return s;
        }
        s.intent = MoveIntent::Hold();
        return s;
    }

    FinishReason TaxiBehaviour::EndReason(Sight const& sight) const
    {
        if (sight.status.cut)
        {
            return FinishReason::Cut;
        }
        if (sight.status.blocked)
        {
            return FinishReason::Blocked;
        }
        return FinishReason::Arrived;
    }

    Outcome TaxiBehaviour::Finish(FinishReason why, Sight const& sight, Services& /*svc*/)
    {
        Outcome o;
        if (why == FinishReason::Arrived)
        {
            o.effects.push_back(Effect::Land(m_p.hasLanding, m_p.landing, sight.facing));
            return o;
        }
        o.interrupt = Displacing(why);   // a replaced flight's spline is cut; every other end cut or finished it already
        o.effects.push_back(Effect::Abort(why));
        return o;
    }

    bool TaxiBehaviour::ResetPosition(Sight const& sight, Services& /*svc*/, Vector3& pos, float& o) const
    {
        if (m_p.nodes.empty())
        {
            return false;
        }
        pos = m_p.nodes[m_node].pos;
        o = sight.facing;
        return true;
    }

    bool TaxiBehaviour::CrossingLandedOn(uint32 mapId)
    {
        const bool landed = m_crossing && m_legEnd < m_p.nodes.size() && m_p.nodes[m_legEnd].mapId == mapId;
        if (landed)
        {
            m_crossingConfirmed = true;
        }
        return landed;
    }
}
