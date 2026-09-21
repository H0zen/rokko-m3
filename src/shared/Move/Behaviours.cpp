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
#include "Move/ClientRules.h"

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

        /// The length of a polyline, which is what the client will time the leg by.
        float Span(const std::vector<Vector3>& points)
        {
            float length = 0.0f;
            for (size_t i = 1; i < points.size(); ++i)
            {
                length += (points[i] - points[i - 1]).magnitude();
            }
            return length;
        }

        /// A leg nobody should ask for: too short for the client to interpolate, so the
        /// writer would refuse it and the shape would learn nothing from being told no.
        bool GoesNowhere(const std::vector<Vector3>& points)
        {
            return points.size() < 2 || Span(points) < POINTLESS_LEG;
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
        if (Reached(here, m_to, POINTLESS_LEG))
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

        // The router came back with a path that covers no ground -- the destination was
        // reachable but is where we already are. That is an arrival, not a leg.
        if (GoesNowhere(m_points))
        {
            m_done = true;
            out.acts.push_back(Plan::Act{ ACT_ARRIVED, m_id, uint32_t(m_kind) });
            return 0;
        }

        m_failures = 0;
        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        out.gait = m_walk ? GAIT_WALK : GAIT_RUN;
        out.speed = m_speed;
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

        // Somebody outside asked this patrol to stand still. The node it was walking to is
        // untouched -- a pause is not a reason to give up on a destination.
        if (m_waitUntilMs != 0)
        {
            const uint32_t wait = m_waitUntilMs;
            m_waitUntilMs = 0;
            return nowMs + wait;
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

            // Standing on the node we were heading to. It is reached, not skipped: the
            // creature is there, so say so and carry on to the next one rather than asking
            // for a leg of no length that the writer would refuse.
            if (leg.size() < 2 || Span(leg) < POINTLESS_LEG)
            {
                if (m_points.empty())
                {
                    m_run.push_back(at);
                    m_reachedWithoutWalking = true;
                }
                if (m_nodes[at].stops || m_nodes[at].waitMs != 0)
                {
                    break;
                }
                from = m_nodes[at].at;
                const size_t after = at + 1 >= m_nodes.size() ? 0 : at + 1;
                if (!m_loops && at + 1 >= m_nodes.size())
                {
                    break;
                }
                if (after == m_target)
                {
                    break;
                }
                at = after;
                continue;
            }

            const size_t before = m_points.size();
            if (m_points.empty())
            {
                m_points.push_back(leg.front());
            }
            for (size_t i = 1; i < leg.size(); ++i)
            {
                m_points.push_back(leg[i]);
            }

            // THE WELD HAS A REACH. Every point between the first and the last travels as an
            // offset from their midpoint, quantised into signed 11/11/10 bit fields; one
            // yard past the edge the field changes sign and the creature walks to the far
            // side of the map. So a node that would push the run past that reach does not
            // join it -- the run ends here and the next one starts from this node.
            if (before >= 2 && !Client::PacksWithoutWrapping(&m_points[0], uint16_t(m_points.size())))
            {
                m_points.resize(before);
                break;
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

        // Standing on the nodes the run covered -- they are reached, but there is no leg to
        // send. Report them and come back shortly from the node after.
        //
        // THIS MUST NOT CALL Arrived. Arrived lays the next leg, and laying it can land here
        // again: a path whose nodes all sit within a stride of each other recurses between
        // the two until the stack runs out, which is exactly how this crashed the server
        // once already. A wake-up is bounded; a recursive call is not.
        if (m_points.size() < 2 && m_reachedWithoutWalking)
        {
            m_reachedWithoutWalking = false;
            const uint32_t wait = Reap(out);
            return nowMs + (wait ? wait : RETRY_MS);
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

    uint32_t WalkNodes::Reap(Plan& out)
    {
        if (m_run.empty())
        {
            return 0;
        }

        // Every node the run covered was passed, in order, so each one is reported. The last
        // of them is where the creature now stands.
        for (size_t i = 0; i < m_run.size(); ++i)
        {
            const Node& node = m_nodes[m_run[i]];
            m_reached = node.id;
            out.acts.push_back(Plan::Act{ ACT_NODE_REACHED, node.id, 0 });
        }

        const Node& last = m_nodes[m_run.back()];
        const uint32_t wait = last.waitMs;

        size_t next = m_run.back() + 1;
        if (next >= m_nodes.size())
        {
            next = m_loops ? 0 : m_run.back();
        }
        m_target = next;
        m_run.clear();
        return wait;
    }

    uint32_t WalkNodes::Arrived(uint32_t nowMs, bool cut, World& world, Plan& out)
    {
        if (cut || m_run.empty())
        {
            // Not an arrival. No node is marked reached and the target does not move.
            return nowMs;
        }

        const bool wasLast = !m_loops && m_run.back() + 1 >= m_nodes.size();
        const uint32_t wait = Reap(out);

        if (wasLast)
        {
            return 0;
        }
        if (wait != 0)
        {
            return nowMs + wait;
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
        if (!world.Route(world.Here(), spot, m_points))
        {
            return nowMs + RETRY_MS;
        }

        // The draw landed where the creature already stands. That is not a failure and not
        // a leg: it is a turn of standing still, so rest and draw again.
        if (GoesNowhere(m_points))
        {
            const uint32_t rest = m_restMax > m_restMin ? world.Urand(m_restMin, m_restMax) : m_restMin;
            return nowMs + (rest == 0 ? RETRY_MS : rest);
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

    // -------------------------------------------------------------------- Orbit

    namespace
    {
        /// How much of the circle one packet carries. A whole turn in one leg would be
        /// cheapest, but the polyline has to stay inside the packed reach and the client
        /// must have points close enough together that the chords it walks look like a
        /// circle rather than a polygon.
        const float ORBIT_ARC = 1.5707963f;   // a quarter turn
        const uint16_t ORBIT_POINTS = 9;      // eight chords over that quarter

        /// How far the circle rises and falls, as a share of its radius. The slope this
        /// produces -- asin(0.25), about fourteen degrees -- is the whole reason for the
        /// number, and it does not depend on how big the circle is.
        const float ORBIT_BAND_SHARE = 0.25f;
    }

    Orbit::Orbit(Kind kind, const Vector3& centre, float radius)
        : m_kind(kind), m_centre(centre), m_radius(radius < 1.0f ? 1.0f : radius),
          m_verticalBand(m_radius * ORBIT_BAND_SHARE)
    {
    }

    float Orbit::SteepestPitch() const
    {
        return std::asin(m_verticalBand / m_radius);
    }

    Vector3 Orbit::At(float angle) const
    {
        return Vector3(m_centre.x + m_radius * std::cos(angle),
                       m_centre.y + m_radius * std::sin(angle),
                       m_centre.z + m_verticalBand * std::sin(angle));
    }

    bool Orbit::Anchor(Vector3& pos, float& facing) const
    {
        pos = m_centre;
        facing = 0.0f;
        return true;
    }

    uint32_t Orbit::Decide(uint32_t, bool restart, World& world, Plan& out)
    {
        // Coming back after something else held the flier: pick up the circle at the point
        // nearest to where it actually is, so it does not swing across the middle to rejoin.
        if (restart)
        {
            const Vector3 here = world.Here();
            m_angle = std::atan2(here.y - m_centre.y, here.x - m_centre.x);
        }

        m_points.clear();
        m_points.reserve(ORBIT_POINTS);
        for (uint16_t i = 0; i < ORBIT_POINTS; ++i)
        {
            const float step = ORBIT_ARC * float(i) / float(ORBIT_POINTS - 1);
            m_points.push_back(At(m_angle + step));
        }

        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        // No routing and no ground: it flies, so the navmesh has nothing to say about it.
        out.gait = GAIT_FLY | GAIT_STRAIGHT;
        return 0;
    }

    uint32_t Orbit::Arrived(uint32_t nowMs, bool cut, World& world, Plan& out)
    {
        if (cut)
        {
            return nowMs;
        }
        m_angle += ORBIT_ARC;
        if (m_angle > 6.2831853f)
        {
            m_angle -= 6.2831853f;
        }
        return Decide(nowMs, false, world, out);
    }

    // ---------------------------------------------------------------- HoldStill

    uint32_t HoldStill::Decide(uint32_t nowMs, bool, World&, Plan& out)
    {
        if (!m_started)
        {
            m_started = true;
            m_untilMs = nowMs + m_ms;
        }

        if (int32_t(nowMs - m_untilMs) < 0)
        {
            return m_untilMs;   // still holding; nothing goes on the wire, ever
        }

        // Over. Saying so is the whole of ending: the game takes this off its layer and
        // what was suspended beneath it is asked again.
        out.acts.push_back(Plan::Act{ ACT_EXPIRED, 0, uint32_t(m_kind) });
        return 0;
    }

    uint32_t HoldStill::Arrived(uint32_t nowMs, bool, World& world, Plan& out)
    {
        // This shape lays no route, so the only way here is someone else's leg ending
        // underneath it. Nothing about the hold changed.
        return Decide(nowMs, false, world, out);
    }

    // ------------------------------------------------------------------- Flight

    uint32_t Flight::Decide(uint32_t nowMs, bool, World& world, Plan& out)
    {
        if (m_landed || m_at >= m_nodes.size())
        {
            return 0;
        }

        m_points.clear();
        m_run.clear();

        // The leg starts where the passenger is, because the client will put its own
        // position in front of ours otherwise and fly a different line than we timed.
        m_points.push_back(world.Here());

        for (size_t i = m_at; i < m_nodes.size(); ++i)
        {
            const size_t before = m_points.size();
            m_points.push_back(m_nodes[i].at);

            // The packed offsets reach a bounded distance from the midpoint of the ends, and
            // a flight path is long: this is where one packet stops and the next begins.
            if (before >= 2 && !Client::PacksWithoutWrapping(&m_points[0], uint16_t(m_points.size())))
            {
                m_points.resize(before);
                break;
            }

            m_run.push_back(i);

            // A node that pauses, or that fires an event, is somewhere the server has to be
            // present again -- so the leg ends there.
            if (m_nodes[i].delayMs != 0 || m_nodes[i].arriveEvent != 0 || m_nodes[i].departEvent != 0)
            {
                break;
            }
        }

        if (m_points.size() < 2 || m_run.empty())
        {
            // Nowhere to go from here: the passenger is already standing on the next node.
            // Count it and come back, rather than asking for a leg of no length.
            if (m_at < m_nodes.size())
            {
                ++m_at;
            }
            return nowMs + 1;
        }

        out.send = true;
        out.points = &m_points[0];
        out.count = uint16_t(m_points.size());
        out.speed = m_speed;
        out.gait = GAIT_FLY | GAIT_STRAIGHT;
        return 0;
    }

    uint32_t Flight::Arrived(uint32_t nowMs, bool cut, World& world, Plan& out)
    {
        if (cut || m_run.empty())
        {
            // A flight that was cut is not a flight that landed. The game decides what that
            // means; this shape only refuses to pretend it arrived.
            return nowMs;
        }

        for (size_t i = 0; i < m_run.size(); ++i)
        {
            const Node& node = m_nodes[m_run[i]];
            if (node.arriveEvent)
            {
                out.acts.push_back(Plan::Act{ ACT_NODE_REACHED, node.arriveEvent, 0 });
            }
        }

        const Node& last = m_nodes[m_run.back()];
        m_at = m_run.back() + 1;
        m_run.clear();

        if (m_at >= m_nodes.size())
        {
            m_landed = true;
            out.acts.push_back(Plan::Act{ ACT_LANDED, 0, uint32_t(Kind::Taxi) });
            return 0;
        }

        if (last.delayMs != 0)
        {
            return nowMs + last.delayMs;
        }
        return Decide(nowMs, false, world, out);
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

        uint32_t deadline = DriftDeadline(quarry, m_slack);
        if (int32_t(deadline - nowMs) < 1)
        {
            deadline = nowMs + 1;
        }

        // WHERE TO STAND. A follow has a slot -- a place round the target, measured from
        // the target's own facing, which turns when the target turns. A chase has only a
        // distance, so it closes from wherever it already is.
        Vector3 aim;
        if (m_hasSlot)
        {
            const float bearing = quarry.facing + m_angle;
            aim = Vector3(quarry.at.x + std::cos(bearing) * stop,
                          quarry.at.y + std::sin(bearing) * stop,
                          quarry.at.z);
        }
        else
        {
            const Vector3 toward = quarry.at - here;
            const float span = toward.magnitude();

            // Already nearer than the stop distance: that is the goal, not a problem. The
            // ratio below goes NEGATIVE when span is under stop, which would put the aim
            // behind the pursuer and walk it backwards away from what it is chasing.
            aim = (span > stop && span > 0.0001f)
                ? here + toward * ((span - stop) / span)
                : here;
        }

        // CLOSE ENOUGH IS A RANGE, NOT A LINE. Being already at the place is the goal, so
        // the only reason to move is being further from it than is worth walking. Without
        // that second term every pursuer sitting a hand's breadth out asked for a leg of a
        // few centimetres, which the writer refuses -- that was every refusal the live
        // server logged once the other shapes went quiet.
        if ((aim - here).magnitude() <= POINTLESS_LEG)
        {
            m_aim = here;
            m_aimed = true;
            out.facing = Facing::Upon(m_target);
            return deadline;
        }

        m_points.clear();
        if (!world.Route(here, aim, m_points) || GoesNowhere(m_points))
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
