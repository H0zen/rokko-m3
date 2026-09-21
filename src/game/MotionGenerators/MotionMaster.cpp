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

#include "MotionMaster.h"
#include "MoveWorld.h"
#include "MoveSend.h"
#include "Move/Behaviours.h"
#include "Move/MoveWriter.h"
#include "Unit.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "WaypointManager.h"
#include "Timer.h"

namespace
{
    /// The two vocabularies are nearly the same list and diverge at one entry, so the map is
    /// written out rather than cast. AssistDistract has no shape of its own: it is a wait,
    /// not a movement, and the engine has nothing to do for it.
    Motion::Kind ToGame(Move::Kind kind)
    {
        switch (kind)
        {
            case Move::Kind::Idle:      return Motion::Kind::Idle;
            case Move::Kind::Wander:    return Motion::Kind::Wander;
            case Move::Kind::Patrol:    return Motion::Kind::Patrol;
            case Move::Kind::Follow:    return Motion::Kind::Follow;
            case Move::Kind::Chase:     return Motion::Kind::Chase;
            case Move::Kind::Point:     return Motion::Kind::Point;
            case Move::Kind::FlyLand:   return Motion::Kind::FlyLand;
            case Move::Kind::Home:      return Motion::Kind::Home;
            case Move::Kind::AssistRun: return Motion::Kind::AssistRun;
            case Move::Kind::Distract:  return Motion::Kind::Distract;
            case Move::Kind::Fear:      return Motion::Kind::Fear;
            case Move::Kind::Confused:  return Motion::Kind::Confused;
            case Move::Kind::Effect:    return Motion::Kind::Effect;
            case Move::Kind::Taxi:      return Motion::Kind::Taxi;
            default:                    return Motion::Kind::Idle;
        }
    }

    uint32 SplineFlagsFor(uint32 gait)
    {
        uint32 flags = Move::SPLINE_NONE;
        if ((gait & Move::GAIT_WALK) != 0)
        {
            flags |= Move::SPLINE_WALK;
        }
        return flags;
    }
}

MotionMaster::~MotionMaster()
{
    // The movement goes first: a pursuit holds a reference to the sighting for its life, so
    // the behaviours have to be gone before the thing they read through is.
    m_movement.Clear();
    delete m_sighting;
}

void MotionMaster::Initialize()
{
    // Nothing is installed here. What a creature does when nothing else is asked of it is a
    // property of its spawn, read when the spawn asks for it -- not an object parked at the
    // bottom of a stack for the life of the unit.
}

bool MotionMaster::LivePosition(Geometry::Vector3& out) const
{
    if (!m_movement.InFlight().Running())
    {
        return false;
    }
    out = m_movement.InFlight().At(getMSTime());
    return true;
}

void MotionMaster::UpdateMotion(uint32 /*diff*/)
{
    if (!m_unit || !m_unit->IsInWorld())
    {
        return;
    }

    const uint32 now = getMSTime();

    // The leg we sent has run out. This is the only thing that happens on its own: the
    // client walks the polyline alone and there is no packet in which it says so, which is
    // exactly why the moment is computed instead of waited for.
    if (m_legRunning && int32(now - m_legEndsAt) >= 0)
    {
        m_legRunning = false;
        Serve(true, false);
        return;
    }

    const uint32 due = m_movement.DueAt();
    if (due != 0 && int32(now - due) >= 0)
    {
        Serve(false, false);
    }
}

void MotionMaster::Serve(bool legEnded, bool cut)
{
    const uint32 now = getMSTime();
    MoveWorld world(*m_unit);
    Move::Plan plan;

    if (legEnded)
    {
        m_movement.Ended(now, cut, world, plan);
    }
    else
    {
        m_movement.Wake(now, world, plan);
    }

    // What the behaviour asked the game to do. The engine never learns what any of these
    // mean; it names them and the game performs them.
    for (size_t i = 0; i < plan.acts.size(); ++i)
    {
        const Move::Plan::Act& act = plan.acts[i];
        Creature* creature = m_unit->GetTypeId() == TYPEID_UNIT ? static_cast<Creature*>(m_unit) : NULL;
        if (!creature || !creature->AI())
        {
            continue;
        }
        switch (act.what)
        {
            case Move::ACT_ARRIVED:
                creature->AI()->MovementInform(Motion::Kind(act.extra), act.id);
                break;
            case Move::ACT_NODE_REACHED:
                creature->AI()->MovementInform(Motion::Kind::Patrol, act.id);
                break;
            default:
                break;
        }
    }

    if (!plan.send || plan.count < 2)
    {
        return;
    }

    const uint32 flags = SplineFlagsFor(plan.gait);
    const float pace = plan.speed > 0.0f ? plan.speed : world.Pace(plan.gait);
    const Move::Written written = Move::MoveWriter::Write(
        plan.points, plan.count, pace, m_unit->GetSpeed(MOVE_RUN), flags);

    if (!MoveSend::Leg(*m_unit, written, plan.facing))
    {
        // Refused. The client would have crashed on it or teleported the mover, so nothing
        // was sent and nothing about the behaviour's state changed. It asks again shortly.
        m_movement.InFlight().Clear();
        m_legRunning = false;
        return;
    }

    // The duration the WRITER computed, not the one the route holds: it is the number the
    // client was given, and the client's own arithmetic starts from it.
    m_legEndsAt = now + written.duration;
    m_legRunning = true;
}

void MotionMaster::Clear(bool /*reset*/, bool /*all*/)
{
    m_movement.Clear();
    if (m_legRunning)
    {
        MoveSend::Halt(*m_unit);
        m_legRunning = false;
    }
}

void MotionMaster::MovementExpired(bool /*reset*/)
{
    Move::Kind running = Move::Kind::Count;
    if (m_movement.Running(running))
    {
        m_movement.Drop(running);
    }
    Serve(false, false);
}

void MotionMaster::MoveIdle()
{
    Clear();
}

void MotionMaster::MovePoint(uint32 id, float x, float y, float z, bool /*generatePath*/)
{
    m_movement.Take(new Move::GoToPoint(Move::Kind::Point, Geometry::Vector3(x, y, z), id));
    Serve(false, false);
}

void MotionMaster::MoveTargetedHome()
{
    // The place a creature returns to is the spot it was standing when it was pulled. A
    // spawn point read from the database would be wrong for anything summoned, escorted or
    // moved by a script, and the anchor is exactly the position the pull interrupted.
    Geometry::Vector3 home = m_unit->Where().Pos();
    if (m_unit->GetTypeId() == TYPEID_UNIT)
    {
        Geometry::Vector3 const& anchor = static_cast<Creature*>(m_unit)->CombatAnchor();
        if (anchor.x != 0.0f || anchor.y != 0.0f || anchor.z != 0.0f)
        {
            home = anchor;
        }
    }
    m_movement.Take(new Move::GoToPoint(Move::Kind::Home, home));
    Serve(false, false);
}

void MotionMaster::MoveRandomAroundPoint(float x, float y, float z, float radius, float /*verticalZ*/)
{
    m_movement.Take(new Move::Scatter(Move::Kind::Wander, Geometry::Vector3(x, y, z),
                                      radius, 3000, 9000));
    Serve(false, false);
}

void MotionMaster::MoveChase(Unit* target, float dist, float /*angle*/)
{
    if (!target)
    {
        return;
    }
    m_sighting.reset(new MoveSighting(*m_unit));
    m_movement.Take(new Move::Pursue(Move::Kind::Chase, target->GetObjectGuid().GetRawValue(),
                                     *m_sighting, dist > 0.0f ? dist : 1.0f));
    Serve(false, false);
}

void MotionMaster::MoveFollow(Unit* target, float dist, float /*angle*/)
{
    if (!target)
    {
        return;
    }
    m_sighting.reset(new MoveSighting(*m_unit));
    m_movement.Take(new Move::Pursue(Move::Kind::Follow, target->GetObjectGuid().GetRawValue(),
                                     *m_sighting, dist > 0.0f ? dist : 2.0f));
    Serve(false, false);
}

void MotionMaster::MoveFleeing(Unit* enemy, uint32 /*timeLimit*/, uint64)
{
    if (!enemy)
    {
        return;
    }
    m_movement.Take(new Move::FleeFrom(Move::Kind::Fear, enemy->Where().Pos(), 30.0f));
    Serve(false, false);
}

void MotionMaster::MoveConfused(uint64)
{
    m_movement.Take(new Move::Scatter(Move::Kind::Confused, m_unit->Where().Pos(),
                                      6.0f, 500, 1500));
    Serve(false, false);
}

void MotionMaster::MoveSeekAssistance(float x, float y, float z)
{
    m_movement.Take(new Move::GoToPoint(Move::Kind::AssistRun, Geometry::Vector3(x, y, z)));
    Serve(false, false);
}

void MotionMaster::MoveWaypoint(int32 pathId, uint32 source, uint32 /*initialDelay*/,
                                uint32 overwriteEntry)
{
    if (m_unit->GetTypeId() != TYPEID_UNIT)
    {
        return;
    }
    Creature* creature = static_cast<Creature*>(m_unit);

    uint32 entry = overwriteEntry ? overwriteEntry : creature->GetEntry();
    WaypointPathOrigin origin = WaypointPathOrigin(source);
    WaypointPath const* path = sWaypointMgr.GetPathFromOrigin(entry, creature->GetGUIDLow(),
                                                              pathId, origin);
    if (!path || path->empty())
    {
        return;
    }

    std::vector<Move::WalkNodes::Node> nodes;
    nodes.reserve(path->size());
    for (WaypointPath::const_iterator it = path->begin(); it != path->end(); ++it)
    {
        Move::WalkNodes::Node node;
        node.id = it->first;
        node.at = Geometry::Vector3(it->second.x, it->second.y, it->second.z);
        node.waitMs = it->second.delay;
        node.stops = it->second.script_id != 0;
        node.hasFacing = it->second.orientation != 100.0f;
        node.facing = it->second.orientation;
        nodes.push_back(node);
    }

    m_movement.Take(new Move::WalkNodes(nodes));
    Serve(false, false);
}

Motion::Kind MotionMaster::ActiveKind() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) ? ToGame(running) : Motion::Kind::Idle;
}

bool MotionMaster::IsPatrolling() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) && running == Move::Kind::Patrol;
}

bool MotionMaster::IsChasing() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) && running == Move::Kind::Chase;
}

bool MotionMaster::IsFollowing() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) && running == Move::Kind::Follow;
}
