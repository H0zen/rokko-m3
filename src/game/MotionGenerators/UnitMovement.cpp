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

#include "UnitMovement.h"
#include "MoveWorld.h"
#include "MoveSend.h"
#include "Move/Behaviours.h"
#include "Move/MoveWriter.h"
#include "Unit.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "WaypointManager.h"
#include "ObjectMgr.h"
#include "Timer.h"
#include "MoveStats.h"

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

UnitMovement::~UnitMovement()
{
    // The movement goes first: a pursuit holds a reference to the sighting for its life, so
    // the behaviours have to be gone before the thing they read through is.
    m_movement.Clear();
    delete m_sighting;
}

void UnitMovement::Initialize()
{
    // WHAT A CREATURE DOES WHEN NOTHING ELSE IS ASKED OF IT.
    //
    // Read from the spawn, once, and turned into one of the shapes. It is not a generator
    // parked at the bottom of a stack for the life of the unit: a creature whose default is
    // Idle ends up holding nothing at all, costs no wake-ups and owns no behaviour object.
    Clear();

    if (!m_unit || m_unit->GetTypeId() != TYPEID_UNIT)
    {
        return;
    }
    Creature* creature = static_cast<Creature*>(m_unit);

    switch (creature->GetDefaultMovementType())
    {
        case CREATURE_MOVEMENT_RANDOM:
        {
            const float radius = creature->GetRespawnRadius();
            if (radius <= 0.0f)
            {
                return;
            }
            Geometry::Vector3 centre = creature->Where().Pos();
            if (CreatureData const* spawn = sObjectMgr.GetCreatureData(creature->GetGUIDLow()))
            {
                centre = Geometry::Vector3(spawn->posX, spawn->posY, spawn->posZ);
            }
            m_movement.Take(new Move::Scatter(Move::Kind::Wander, centre, radius, 3000, 9000));
            Serve(false, false);
            return;
        }
        case CREATURE_MOVEMENT_WAYPOINT:
            MoveWaypoint(0, PATH_NO_PATH, 0, 0);
            return;
        case CREATURE_MOVEMENT_IDLE:
        default:
            return;
    }
}

bool UnitMovement::LivePosition(Geometry::Vector3& out) const
{
    if (!m_movement.InFlight().Running())
    {
        return false;
    }
    out = m_movement.InFlight().At(getMSTime());
    return true;
}

void UnitMovement::UpdateMotion(uint32 /*diff*/)
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
        return;
    }

    // Nothing was due. This is what the whole scheduling idea buys: the visit cost two
    // integer comparisons and no behaviour was asked anything.
    MoveStats::Visited();
}

void UnitMovement::Serve(bool legEnded, bool cut)
{
    MoveStats::Decided();
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
    // A charge names its own speed and the behaviour does not know it, so the request
    // carries it here rather than through a field on a shape that has no use for one.
    float pace = plan.speed > 0.0f ? plan.speed : world.Pace(plan.gait);
    if (m_chargeSpeed > 0.0f)
    {
        pace = m_chargeSpeed;
    }
    const Move::Written written = Move::MoveWriter::Write(
        plan.points, plan.count, pace, m_unit->GetSpeed(MOVE_RUN), flags);

    Move::Kind shape = Move::Kind::Count;
    m_movement.Running(shape);

    if (!MoveSend::Leg(*m_unit, written, plan.facing, shape))
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
    m_sentFlags = written.flags;
    m_sentDuration = written.duration;
    m_sentFacing = plan.facing;
    ++m_sentId;
}

void UnitMovement::Clear(bool /*reset*/, bool /*all*/)
{
    m_movement.Clear();
    if (m_legRunning)
    {
        MoveSend::Halt(*m_unit);
        m_legRunning = false;
    }
}

void UnitMovement::MovementExpired(bool /*reset*/)
{
    Move::Kind running = Move::Kind::Count;
    if (m_movement.Running(running))
    {
        m_movement.Drop(running);
    }
    Serve(false, false);
}

void UnitMovement::MoveIdle()
{
    Clear();
}

void UnitMovement::MovePoint(uint32 id, float x, float y, float z, bool /*generatePath*/)
{
    m_movement.Take(new Move::GoToPoint(Move::Kind::Point, Geometry::Vector3(x, y, z), id));
    Serve(false, false);
}

void UnitMovement::MoveTargetedHome()
{
    // WHERE HOME IS, in the order the answers are actually trustworthy.
    //
    // 1. The combat anchor, when one was recorded. Unit::Attack writes the creature's
    //    position at the moment a NEW battle starts, so it is where the pull interrupted
    //    it -- a point on its patrol path, or a spot inside its wander leash. Returning
    //    there lets a patrol resume where it left off instead of walking back to node zero.
    //
    // 2. The spawn row, when there is no anchor. A script may send a creature home without
    //    it ever having fought, and the anchor is a zero sentinel in that case; falling
    //    through to the current position would make "go home" mean "stay where you are",
    //    which is the one answer that is always wrong.
    //
    // 3. Its own position, for anything with neither: a summon has no database row and
    //    nothing better is known about it.
    Geometry::Vector3 home = m_unit->Where().Pos();
    if (m_unit->GetTypeId() == TYPEID_UNIT)
    {
        Creature* creature = static_cast<Creature*>(m_unit);
        Geometry::Vector3 const& anchor = creature->CombatAnchor();
        if (anchor.x != 0.0f || anchor.y != 0.0f || anchor.z != 0.0f)
        {
            home = anchor;
        }
        else if (CreatureData const* spawn = sObjectMgr.GetCreatureData(creature->GetGUIDLow()))
        {
            home = Geometry::Vector3(spawn->posX, spawn->posY, spawn->posZ);
        }
    }

    m_movement.Take(new Move::GoToPoint(Move::Kind::Home, home));
    Serve(false, false);
}

void UnitMovement::MoveRandomAroundPoint(float x, float y, float z, float radius, float /*verticalZ*/)
{
    m_movement.Take(new Move::Scatter(Move::Kind::Wander, Geometry::Vector3(x, y, z),
                                      radius, 3000, 9000));
    Serve(false, false);
}

void UnitMovement::MoveChase(Unit* target, float dist, float /*angle*/)
{
    if (!target)
    {
        return;
    }
    if (!m_sighting)
    {
        m_sighting = new MoveSighting(*m_unit);
    }
    m_movement.Take(new Move::Pursue(Move::Kind::Chase, target->GetObjectGuid().GetRawValue(),
                                     *m_sighting, dist > 0.0f ? dist : 1.0f));
    Serve(false, false);
}

void UnitMovement::MoveFollow(Unit* target, float dist, float /*angle*/)
{
    if (!target)
    {
        return;
    }
    if (!m_sighting)
    {
        m_sighting = new MoveSighting(*m_unit);
    }
    m_movement.Take(new Move::Pursue(Move::Kind::Follow, target->GetObjectGuid().GetRawValue(),
                                     *m_sighting, dist > 0.0f ? dist : 2.0f));
    Serve(false, false);
}

void UnitMovement::MoveFleeing(Unit* enemy, uint32 /*timeLimit*/, uint64)
{
    if (!enemy)
    {
        return;
    }
    m_movement.Take(new Move::FleeFrom(Move::Kind::Fear, enemy->Where().Pos(), 30.0f));
    Serve(false, false);
}

void UnitMovement::MoveConfused(uint64)
{
    m_movement.Take(new Move::Scatter(Move::Kind::Confused, m_unit->Where().Pos(),
                                      6.0f, 500, 1500));
    Serve(false, false);
}

void UnitMovement::MoveSeekAssistance(float x, float y, float z)
{
    m_movement.Take(new Move::GoToPoint(Move::Kind::AssistRun, Geometry::Vector3(x, y, z)));
    Serve(false, false);
}

void UnitMovement::MoveWaypoint(int32 pathId, uint32 source, uint32 /*initialDelay*/,
                                uint32 overwriteEntry)
{
    if (m_unit->GetTypeId() != TYPEID_UNIT)
    {
        return;
    }
    Creature* creature = static_cast<Creature*>(m_unit);

    const uint32 entry = overwriteEntry ? overwriteEntry : creature->GetEntry();
    WaypointPathOrigin origin = WaypointPathOrigin(source);

    // No origin named: take whichever path the creature actually has, its own row first and
    // its template's second, which is what a spawn set to WAYPOINT means by "its path".
    WaypointPath* path = NULL;
    if (origin == PATH_NO_PATH)
    {
        path = sWaypointMgr.GetPathFromOrigin(entry, creature->GetGUIDLow(), 0, PATH_FROM_GUID);
        if (!path || path->empty())
        {
            path = sWaypointMgr.GetPathFromOrigin(entry, creature->GetGUIDLow(), 0, PATH_FROM_ENTRY);
        }
    }
    else
    {
        path = sWaypointMgr.GetPathFromOrigin(entry, creature->GetGUIDLow(), pathId, origin);
    }
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

Motion::Kind UnitMovement::ActiveKind() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) ? ToGame(running) : Motion::Kind::Idle;
}

bool UnitMovement::IsPatrolling() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) && running == Move::Kind::Patrol;
}

bool UnitMovement::IsChasing() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) && running == Move::Kind::Chase;
}

bool UnitMovement::IsFollowing() const
{
    Move::Kind running = Move::Kind::Count;
    return m_movement.Running(running) && running == Move::Kind::Follow;
}

void UnitMovement::MoveFlyOrLand(uint32 id, float x, float y, float z, bool /*liftOff*/)
{
    m_movement.Take(new Move::GoToPoint(Move::Kind::FlyLand, Geometry::Vector3(x, y, z), id));
    Serve(false, false);
}

void UnitMovement::MoveCharge(float x, float y, float z, float speed)
{
    Move::GoToPoint* charge = new Move::GoToPoint(Move::Kind::Effect, Geometry::Vector3(x, y, z));
    m_movement.Take(charge);
    m_chargeSpeed = speed;
    Serve(false, false);
    m_chargeSpeed = 0.0f;
}

void UnitMovement::MoveCharge(Unit* target, float speed)
{
    if (!target)
    {
        return;
    }
    Geometry::Vector3 const& at = target->Where().Pos();
    MoveCharge(at.x, at.y, at.z, speed);
}

bool UnitMovement::MoveJump(float x, float y, float z, float horizontalSpeed, float maxHeight,
                            uint32 /*id*/)
{
    m_movement.Take(new Move::Ballistic(Geometry::Vector3(x, y, z), horizontalSpeed, maxHeight));
    Serve(false, false);
    return true;
}

void UnitMovement::MoveFall()
{
    // A fall is a jump with no forward speed and no arc: the client computes the descent
    // from gravity alone once the falling flag is set, so the server only names the floor.
    MoveWorld world(*m_unit);
    Geometry::Vector3 down = m_unit->Where().Pos();
    if (!world.Floor(down, down.z))
    {
        return;
    }
    m_movement.Take(new Move::Ballistic(down, 0.0f, 0.0f));
    Serve(false, false);
}

bool UnitMovement::SetNextWaypoint(uint32 pointId)
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Patrol);
    if (!held)
    {
        return false;
    }
    if (!static_cast<Move::WalkNodes*>(held)->SetNext(pointId))
    {
        return false;
    }
    Serve(false, false);
    return true;
}

uint32 UnitMovement::getLastReachedWaypoint() const
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Patrol);
    return held ? static_cast<Move::WalkNodes*>(held)->Reached() : 0;
}

uint32 UnitMovement::SelectedPatrolNode() const
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Patrol);
    return held ? static_cast<Move::WalkNodes*>(held)->Heading() : 0;
}

bool UnitMovement::GetDestination(float& x, float& y, float& z)
{
    if (!m_movement.InFlight().Running())
    {
        return false;
    }
    Geometry::Vector3 const& end = m_movement.InFlight().End();
    x = end.x;
    y = end.y;
    z = end.z;
    return true;
}

bool UnitMovement::LiveFacing(float& out) const
{
    if (!m_legRunning || !m_movement.InFlight().Running())
    {
        return false;
    }
    out = m_movement.InFlight().FacingAt(getMSTime());
    return true;
}

void UnitMovement::FaceTo(float orientation)
{
    // No travel and no route: the mover turns where it stands. A spline would be a leg of
    // no length, which is the one shape the client cannot decode.
    MoveSend::Face(*m_unit, orientation);
}

void UnitMovement::Halt()
{
    if (m_legRunning)
    {
        MoveSend::Halt(*m_unit);
        m_legRunning = false;
    }
    m_movement.InFlight().Clear();
}

void UnitMovement::MoveAtSpeed(float x, float y, float z, float speed, bool routed)
{
    MoveWorld world(*m_unit);
    std::vector<Geometry::Vector3> points;
    const Geometry::Vector3 to(x, y, z);

    if (!routed || !world.Route(world.Here(), to, points) || points.size() < 2)
    {
        points.clear();
        points.push_back(world.Here());
        points.push_back(to);
    }

    const Move::Written written = Move::MoveWriter::Write(
        &points[0], uint16(points.size()), speed, m_unit->GetSpeed(MOVE_RUN));

    if (!MoveSend::Leg(*m_unit, written, Move::Facing(), Move::Kind::Point))
    {
        return;
    }
    m_movement.InFlight().Launch(&points[0], uint16(points.size()), written.speed, getMSTime());
    m_legEndsAt = getMSTime() + written.duration;
    m_legRunning = true;
    m_sentFlags = written.flags;
    m_sentDuration = written.duration;
    m_sentFacing = Move::Facing();
    ++m_sentId;
}

// ---------------------------------------------------------------- what is forbidden

void UnitMovement::Inhibit(Motion::Inhibition what, uint64 source)
{
    if (!m_blocks.Inhibit(what, source))
    {
        return;   // the reason already held: another source, nothing changes
    }
    Forbidden();
}

void UnitMovement::Uninhibit(Motion::Inhibition what, uint64 source)
{
    m_blocks.Uninhibit(what, source);
}

void UnitMovement::DropDomain(Motion::SourceDomain domain)
{
    m_blocks.DropDomain(domain);
}

uint8 UnitMovement::Reasons() const
{
    // The counted sources, plus the reasons that ARE a running behaviour. A fear is not
    // something a caster registered against this unit; it is a fear holding the Control
    // layer, and asking the behaviour is asking the only thing that knows.
    uint8 reasons = m_blocks.Reasons();

    Move::Kind running = Move::Kind::Count;
    if (m_movement.Running(running))
    {
        switch (running)
        {
            case Move::Kind::Fear:     reasons |= Motion::ReasonFeared;     break;
            case Move::Kind::Confused: reasons |= Motion::ReasonConfused;   break;
            case Move::Kind::Distract: reasons |= Motion::ReasonDistracted; break;
            case Move::Kind::Taxi:     reasons |= Motion::ReasonOnTaxi;     break;
            default: break;
        }
    }
    return reasons;
}

bool UnitMovement::Feigning() const
{
    // A death holds the Dead reason from one known source. Any OTHER source of it is a unit
    // pretending, which is why the sentinel exists: the two are indistinguishable by their
    // effect and have to be told apart by who asked.
    std::vector<uint64> const& sources = m_blocks.Sources(Motion::Inhibition::Dead);
    for (size_t i = 0; i < sources.size(); ++i)
    {
        if (sources[i] != Motion::kDeathSource)
        {
            return true;
        }
    }
    return false;
}

void UnitMovement::Forbidden()
{
    // Whatever was walking is not allowed to any more, and the leg in flight is already on
    // its way to every client that can see it. Stopping is not optional and not deferred:
    // a rooted unit that keeps walking is the defect this class exists to make impossible.
    if (!MayMove())
    {
        Halt();
        m_movement.Clear();
    }
}

void UnitMovement::ReleaseEveryRestriction()
{
    // Every source of every reason, by domain, because a respawn or a revive is the one
    // moment when nothing that held this unit before still applies -- not the aura that
    // rooted it, not the death, not the seat it was sitting in.
    m_blocks.DropDomain(Motion::SourceDomain::Aura);
    m_blocks.DropDomain(Motion::SourceDomain::Death);
    m_blocks.DropDomain(Motion::SourceDomain::Possession);
    m_blocks.DropDomain(Motion::SourceDomain::Seat);
    m_blocks.DropDomain(Motion::SourceDomain::FixedVehicle);
    m_blocks.DropDomain(Motion::SourceDomain::Script);
    Release();
}

std::vector<UnitMovement::HeldView> UnitMovement::Held() const
{
    std::vector<HeldView> held;
    Move::Kind running = Move::Kind::Count;
    if (m_movement.Running(running))
    {
        HeldView view;
        view.kind = ToGame(running);
        view.selected = true;
        held.push_back(view);
    }
    return held;
}

// -------------------------------------------------------------------- who is driving

uint32 UnitMovement::Seize()
{
    m_authority = Authority::Seized;
    m_seizeAck = m_seizeNext++;
    if (m_seizeNext == 0)
    {
        m_seizeNext = 1;   // zero means "nothing outstanding"
    }
    return m_seizeAck;
}

bool UnitMovement::Released(uint32 counter)
{
    // A counter that is not the one being waited for is a late answer to a seizure that has
    // already ended. Accepting it would hand control back in the middle of the NEXT one.
    if (m_seizeAck == 0 || counter != m_seizeAck)
    {
        return false;
    }
    Release();
    return true;
}

void UnitMovement::Release()
{
    m_seizeAck = 0;
    if (m_unit && m_unit->GetTypeId() == TYPEID_PLAYER)
    {
        m_authority = Authority::Client;
    }
    else
    {
        m_authority = Authority::Server;
    }
}
