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

#include "Move/Behaviours.h"

#include <cmath>

namespace Move
{
    namespace
    {
        /// How long to wait before trying again after a route that could not be found.
        /// Short enough that a creature does not look stuck, long enough that a genuinely
        /// unreachable destination is not retried every tick.
        const uint32_t RETRY_MS = 500;

        /// After this many consecutive failures on the same destination, walk straight at
        /// it instead of asking the router. The destination is NEVER abandoned: a creature
        /// that gives up on a node is the defect this rule exists to prevent.
        const uint8_t STRAIGHT_AFTER = 3;

        bool Reached(const Vector3& here, const Vector3& there, float within)
        {
            return (there - here).squaredMagnitude() <= within * within;
        }
    }

    // ---------------------------------------------------------------- GoToPoint

    uint32_t GoToPoint::Decide(uint32_t nowMs, bool, World& world, Plan& out)
    {
        if (m_done)
        {
            return 0;
        }

        const Vector3 here = world.Here();
        if (Reached(here, m_to, 0.5f))
        {
            m_done = true;
            out.acts.push_back(Plan::Act{ ACT_ARRIVED, m_id, uint32_t(m_kind) });
            return 0;
        }

        m_points.clear();
        const bool straight = m_failures >= STRAIGHT_AFTER;
        if (!straight && !world.Route(here, m_to, m_points))
        {
            ++m_failures;
            return nowMs + RETRY_MS;
        }
        if (straight)
        {
            m_points.clear();
            m_points.push_back(here);
            m_points.push_back(m_to);
        }

        m_failures = 0;
        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        out.gait = m_walk ? GAIT_WALK : GAIT_RUN;
        return 0;
    }

    uint32_t GoToPoint::Arrived(uint32_t nowMs, bool cut, World& world, Plan& out)
    {
        if (cut)
        {
            // Something ended the leg early. That is not an arrival, so nothing is reported
            // and the destination is unchanged: ask again.
            return nowMs;
        }
        return Decide(nowMs, false, world, out);
    }

    // ---------------------------------------------------------------- WalkNodes

    size_t WalkNodes::IndexOf(uint32_t nodeId) const
    {
        for (size_t i = 0; i < m_nodes.size(); ++i)
        {
            if (m_nodes[i].id == nodeId)
            {
                return i;
            }
        }
        return m_nodes.size();
    }

    bool WalkNodes::SetNext(uint32_t nodeId)
    {
        const size_t at = IndexOf(nodeId);
        if (at >= m_nodes.size())
        {
            return false;
        }
        m_target = at;
        m_failures = 0;
        m_run.clear();
        return true;
    }

    void WalkNodes::Replace(std::vector<Node> nodes)
    {
        m_nodes = nodes;
        if (m_target >= m_nodes.size())
        {
            m_target = 0;
        }
        m_run.clear();
    }

    bool WalkNodes::Anchor(Vector3& pos, float& facing) const
    {
        const size_t at = IndexOf(m_reached);
        if (at >= m_nodes.size())
        {
            return false;
        }
        pos = m_nodes[at].at;
        facing = m_nodes[at].hasFacing ? m_nodes[at].facing : 0.0f;
        return true;
    }

    uint32_t WalkNodes::Decide(uint32_t nowMs, bool, World& world, Plan& out)
    {
        return Lay(nowMs, world, out);
    }

    uint32_t WalkNodes::Lay(uint32_t nowMs, World& world, Plan& out)
    {
        if (m_nodes.empty())
        {
            return 0;
        }
        if (m_target >= m_nodes.size())
        {
            m_target = 0;
        }

        m_points.clear();
        m_run.clear();

        // Weld consecutive nodes into one leg for as long as nothing asks us to stop. This
        // is the whole reason a patrol is cheap: the client walks the lot without the server
        // hearing from it once.
        Vector3 from = world.Here();
        size_t at = m_target;
        const bool straight = m_failures >= STRAIGHT_AFTER;

        for (size_t step = 0; step < m_nodes.size(); ++step)
        {
            std::vector<Vector3> leg;
            if (straight && step == 0)
            {
                leg.push_back(from);
                leg.push_back(m_nodes[at].at);
            }
            else if (!world.Route(from, m_nodes[at].at, leg))
            {
                break;
            }

            if (m_points.empty())
            {
                m_points.push_back(leg.front());
            }
            for (size_t i = 1; i < leg.size(); ++i)
            {
                m_points.push_back(leg[i]);
            }
            m_run.push_back(at);

            if (m_nodes[at].stops || m_nodes[at].waitMs != 0)
            {
                break;
            }

            from = m_nodes[at].at;
            const size_t next = at + 1 >= m_nodes.size() ? 0 : at + 1;
            if (!m_loops && at + 1 >= m_nodes.size())
            {
                break;
            }
            if (next == m_target)
            {
                break;   // a whole lap: do not weld the path onto itself
            }
            at = next;
        }

        // Nothing usable. The target node is UNCHANGED -- that is the rule this whole class
        // is shaped around. Try again shortly, and after a few tries walk straight at it.
        if (m_points.size() < 2 || m_run.empty())
        {
            ++m_failures;
            return nowMs + RETRY_MS;
        }

        m_failures = 0;
        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        out.gait = m_walk ? GAIT_WALK : GAIT_RUN;

        const Node& last = m_nodes[m_run.back()];
        if (last.hasFacing && last.waitMs != 0)
        {
            out.facing = Facing::At(last.facing);
        }
        return 0;
    }

    uint32_t WalkNodes::Arrived(uint32_t nowMs, bool cut, World& world, Plan& out)
    {
        if (cut || m_run.empty())
        {
            // Not an arrival. No node is marked reached and the target does not move.
            return nowMs;
        }

        // Every node the leg covered was passed, in order, so each one is reported. The
        // last of them is where the creature now stands.
        for (size_t i = 0; i < m_run.size(); ++i)
        {
            const Node& node = m_nodes[m_run[i]];
            m_reached = node.id;
            out.acts.push_back(Plan::Act{ ACT_NODE_REACHED, node.id, 0 });
        }

        const Node& last = m_nodes[m_run.back()];
        size_t next = m_run.back() + 1;
        if (next >= m_nodes.size())
        {
            if (!m_loops)
            {
                m_run.clear();
                return 0;
            }
            next = 0;
        }
        m_target = next;
        m_run.clear();

        if (last.waitMs != 0)
        {
            return nowMs + last.waitMs;
        }
        return Lay(nowMs, world, out);
    }

    // ------------------------------------------------------------------ Scatter

    bool Scatter::Anchor(Vector3& pos, float& facing) const
    {
        pos = m_centre;
        facing = 0.0f;
        return true;
    }

    uint32_t Scatter::Decide(uint32_t nowMs, bool, World& world, Plan& out)
    {
        Vector3 spot;
        if (!world.Scatter(m_centre, m_radius, spot))
        {
            return nowMs + RETRY_MS;
        }

        m_points.clear();
        if (!world.Route(world.Here(), spot, m_points) || m_points.size() < 2)
        {
            return nowMs + RETRY_MS;
        }

        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        out.gait = m_walk ? GAIT_WALK : GAIT_RUN;
        return 0;
    }

    uint32_t Scatter::Arrived(uint32_t nowMs, bool, World& world, Plan&)
    {
        const uint32_t rest = m_restMax > m_restMin
            ? world.Urand(m_restMin, m_restMax)
            : m_restMin;
        return nowMs + (rest == 0 ? 1 : rest);
    }

    // ------------------------------------------------------------------- Pursue

    uint32_t Pursue::DriftDeadline(const Quarry& quarry, float slack)
    {
        // The target can be anywhere within topSpeed * (t - reportedAt) of where it was
        // reported. The uncertainty about where it is NOW and the uncertainty about where
        // it will be are the same quantity, so the deadline is measured from the report and
        // not from the present moment.
        if (quarry.topSpeed <= 0.0f)
        {
            return quarry.reportedAtMs + 60000;
        }
        const float seconds = slack / quarry.topSpeed;
        uint32_t ms = uint32_t(seconds * 1000.0f);
        if (ms < 1)
        {
            ms = 1;
        }
        return quarry.reportedAtMs + ms;
    }

    uint32_t Pursue::Decide(uint32_t nowMs, bool, World& world, Plan& out)
    {
        const Quarry quarry = m_sighting->Look(m_target);
        if (!quarry.known)
        {
            // The target is gone. Say so once; who decides what that means is the game's,
            // not this class's.
            out.acts.push_back(Plan::Act{ ACT_TARGET_LOST, 0, uint32_t(m_kind) });
            return 0;
        }

        const Vector3 here = world.Here();
        const float stop = m_stopAt + quarry.reach;

        // Aim short of the target rather than at it, so the leg ends where the creature
        // should stand and not inside the thing it is chasing.
        Vector3 aim = quarry.at;
        const Vector3 toward = quarry.at - here;
        const float span = toward.magnitude();
        if (span > stop && span > 0.0001f)
        {
            aim = here + toward * ((span - stop) / span);
        }

        uint32_t deadline = DriftDeadline(quarry, m_slack);
        if (int32_t(deadline - nowMs) < 1)
        {
            deadline = nowMs + 1;
        }

        if (span <= stop)
        {
            // Already close enough. Nothing to send; come back when the target could have
            // drifted out of tolerance, which is the only thing that can change this.
            m_aim = here;
            m_aimed = true;
            out.facing = Facing::Upon(m_target);
            return deadline;
        }

        m_points.clear();
        if (!world.Route(here, aim, m_points) || m_points.size() < 2)
        {
            return nowMs + RETRY_MS;
        }

        m_aim = aim;
        m_aimed = true;
        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        out.gait = GAIT_RUN;
        out.facing = Facing::Upon(m_target);

        // Wake at whichever comes first: the leg ending, or the target having had time to
        // drift too far from where we aimed. The map takes the leg's end for us, so only
        // the drift deadline is ours to ask for.
        return deadline;
    }

    uint32_t Pursue::Arrived(uint32_t nowMs, bool, World& world, Plan& out)
    {
        return Decide(nowMs, false, world, out);
    }

    // ----------------------------------------------------------------- FleeFrom

    uint32_t FleeFrom::Decide(uint32_t nowMs, bool, World& world, Plan& out)
    {
        const Vector3 here = world.Here();
        Vector3 away = here - m_from;
        away.z = 0.0f;
        const float span = away.magnitude();
        if (span < 0.0001f)
        {
            // Standing on the thing being fled. Any direction will do, so take one rather
            // than dividing by nothing.
            const float bearing = world.Frand(0.0f, 6.2831853f);
            away = Vector3(std::cos(bearing), std::sin(bearing), 0.0f);
        }
        else
        {
            away = away * (1.0f / span);
        }

        Vector3 goal = here + away * m_distance;
        if (!world.Floor(goal, goal.z))
        {
            return nowMs + RETRY_MS;
        }

        m_points.clear();
        if (!world.Route(here, goal, m_points) || m_points.size() < 2)
        {
            return nowMs + RETRY_MS;
        }

        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        out.gait = GAIT_RUN;
        return 0;
    }

    uint32_t FleeFrom::Arrived(uint32_t nowMs, bool, World& world, Plan& out)
    {
        return Decide(nowMs, false, world, out);
    }

    // ---------------------------------------------------------------- Ballistic

    uint32_t Ballistic::Decide(uint32_t, bool, World&, Plan& out)
    {
        if (m_landed)
        {
            return 0;
        }
        m_points.clear();
        m_points.push_back(Vector3());   // filled by the caller with the mover's own spot
        m_points.push_back(m_to);
        out.send = true;
        out.points = &m_points[0];
        out.count = 2;
        out.speed = m_speed;
        out.gait = GAIT_STRAIGHT;
        return 0;
    }

    uint32_t Ballistic::Arrived(uint32_t, bool, World&, Plan& out)
    {
        m_landed = true;
        out.acts.push_back(Plan::Act{ ACT_LANDED, 0, 0 });
        return 0;
    }
}
