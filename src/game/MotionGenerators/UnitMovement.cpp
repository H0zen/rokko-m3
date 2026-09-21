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
#include "Player.h"
#include "CreatureAI.h"
#include "WaypointManager.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "ObjectLookup.h"
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

void UnitMovement::UseDefault()
{
    // WHAT A CREATURE DOES WHEN NOTHING ELSE IS ASKED OF IT.
    //
    // Read from the spawn, once, and turned into one of the shapes. It is not a generator
    // parked at the bottom of a stack for the life of the unit: a creature whose default is
    // Idle ends up holding nothing at all, costs no wake-ups and owns no behaviour object.
    Stop();

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
            Wander(centre.x, centre.y, centre.z, radius);
            return;
        }
        case CREATURE_MOVEMENT_WAYPOINT:
            WalkPath(0, PATH_NO_PATH, 0);
            return;
        case CREATURE_MOVEMENT_IDLE:
        default:
            return;
    }
}

bool UnitMovement::PositionNow(Geometry::Vector3& out) const
{
    if (!m_movement.InFlight().Running())
    {
        return false;
    }
    out = m_movement.InFlight().At(getMSTime());
    return true;
}

void UnitMovement::Tick(uint32 /*diff*/)
{
    if (!m_unit || !m_unit->IsInWorld())
    {
        return;
    }

    const uint32 now = getMSTime();

    // The route we sent has run out. This is the only thing that happens on its own: the
    // client walks the polyline alone and there is no packet in which it says so, which is
    // exactly why the moment is computed rather than waited for. The route itself knows
    // when it ends, so there is no second copy of that instant to keep in step.
    Move::Route const& route = m_movement.InFlight();
    if (route.Running() && route.Arrived(now))
    {
        Advance(true, false);
        return;
    }

    const uint32 due = m_movement.DueAt();
    if (due != 0 && int32(now - due) >= 0)
    {
        Advance(false, false);
        return;
    }

    // Nothing was due. This is what the whole scheduling idea buys: the visit cost two
    // integer comparisons and no behaviour was asked anything.
    MoveStats::Visited();
}

void UnitMovement::Advance(bool legEnded, bool cut)
{
    MoveStats::Decided();
    const uint32 now = getMSTime();
    MoveWorld world(*m_unit);
    Move::Plan plan;

    if (legEnded)
    {
        // The route that just ended is no longer in flight. Clearing it before the
        // behaviour answers means "still running" cannot outlive the leg it described --
        // and if a new leg is laid below, it replaces this cleanly.
        m_movement.InFlight().Clear();
        m_movement.Ended(now, cut, world, plan);
    }
    else
    {
        m_movement.Wake(now, world, plan);
    }

    // What the behaviour asked the game to do. The engine never learns what any of these
    // mean; it names them and the game performs them.
    Creature* creature = m_unit->GetTypeId() == TYPEID_UNIT ? static_cast<Creature*>(m_unit) : NULL;
    Player* player = m_unit->GetTypeId() == TYPEID_PLAYER ? static_cast<Player*>(m_unit) : NULL;

    for (size_t i = 0; i < plan.acts.size(); ++i)
    {
        const Move::Plan::Act& act = plan.acts[i];
        switch (act.what)
        {
            case Move::ACT_ARRIVED:
                if (creature && creature->AI())
                {
                    creature->AI()->MovementInform(Motion::Kind(act.extra), act.id);
                }
                break;

            case Move::ACT_NODE_REACHED:
                if (creature && creature->AI())
                {
                    creature->AI()->MovementInform(Motion::Kind::Patrol, act.id);
                }
                break;

            case Move::ACT_LANDED:
                // A flight that ran to its end. The passenger drives again from here, and
                // the handover is announced rather than assumed: until the client answers,
                // nothing it says about where it is can be believed.
                if (act.extra == uint32(Move::Kind::Taxi))
                {
                    GiveControl();
                    if (player)
                    {
                        player->TaxiAbort();
                    }
                    m_movement.Drop(Move::Kind::Taxi);
                }
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

    Move::Kind shape = Move::Kind::Count;
    m_movement.Running(shape);

    if (!MoveSend::Leg(*m_unit, written, plan.facing, shape))
    {
        // Refused. The client would have crashed on it or teleported the mover, so nothing
        // was sent and nothing about the behaviour's state changed. It asks again shortly.
        m_movement.InFlight().Clear();
        return;
    }

    // THE ROUTE IS RE-LAUNCHED FROM WHAT ACTUALLY WENT OUT, not from what the behaviour
    // asked for. The writer strips points the client would choke on and re-times what is
    // left, so the polyline it produced is the one the client walks -- and if the route
    // held the behaviour's version instead, PositionNow() would describe a leg nobody is
    // walking. One description, and it is the one on the wire.
    std::vector<Geometry::Vector3> sent;
    sent.reserve(written.middle.size() + 2);
    sent.push_back(written.start);
    for (size_t i = 0; i < written.middle.size(); ++i)
    {
        sent.push_back(written.middle[i]);
    }
    sent.push_back(written.destination);
    m_movement.InFlight().Launch(&sent[0], uint16(sent.size()), written.speed, now);

    m_sentFlags = written.flags;
    m_sentFacing = plan.facing;
    ++m_sentId;
}

void UnitMovement::Stop()
{
    const bool wasMoving = IsMoving();
    m_movement.Clear();
    if (wasMoving)
    {
        MoveSend::Halt(*m_unit);
    }
}

void UnitMovement::StopAndDefault()
{
    Stop();
    UseDefault();
}

void UnitMovement::Finish()
{
    Move::Kind running = Move::Kind::Count;
    if (m_movement.Running(running))
    {
        m_movement.Drop(running);
    }
    Advance(false, false);
}

void UnitMovement::GoTo(uint32 id, float x, float y, float z, bool /*routed*/, float speed)
{
    Move::GoToPoint* leg = new Move::GoToPoint(Move::Kind::Point, Geometry::Vector3(x, y, z), id);
    if (speed > 0.0f)
    {
        leg->AtSpeed(speed);
    }
    m_movement.Take(leg);
    Advance(false, false);
}

void UnitMovement::GoHome()
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
    Advance(false, false);
}

void UnitMovement::Wander(float x, float y, float z, float radius)
{
    const Geometry::Vector3 centre(x, y, z);
    Creature* creature = (m_unit && m_unit->GetTypeId() == TYPEID_UNIT)
        ? static_cast<Creature*>(m_unit) : NULL;

    if (creature && creature->CanFly())
    {
        m_movement.Take(new Move::Orbit(Move::Kind::Wander, centre, radius));
    }
    else
    {
        m_movement.Take(new Move::Scatter(Move::Kind::Wander, centre, radius, 3000, 9000));
    }
    Advance(false, false);
}

void UnitMovement::Chase(Unit* target, float dist, float angle)
{
    if (!target)
    {
        return;
    }
    if (!m_sighting)
    {
        m_sighting = new MoveSighting(*m_unit);
    }
    Move::Pursue* chase = new Move::Pursue(Move::Kind::Chase,
                                           target->GetObjectGuid().GetRawValue(),
                                           *m_sighting, dist > 0.0f ? dist : 1.0f);
    if (angle != 0.0f)
    {
        chase->HoldSlot(angle);
    }
    m_movement.Take(chase);
    Advance(false, false);
}

void UnitMovement::Follow(Unit* target, float distance, float angle)
{
    if (!target)
    {
        return;
    }
    if (!m_sighting)
    {
        m_sighting = new MoveSighting(*m_unit);
    }
    Move::Pursue* follow = new Move::Pursue(Move::Kind::Follow,
                                            target->GetObjectGuid().GetRawValue(),
                                            *m_sighting, distance > 0.0f ? distance : 2.0f);
    follow->HoldSlot(angle);
    m_movement.Take(follow);
    Advance(false, false);
}

void UnitMovement::FleeFrom(Unit* enemy, uint32 /*timeLimitMs*/, uint64 source)
{
    if (!enemy)
    {
        return;
    }
    // The source goes in first: the reason is what keeps the behaviour alive, so a fear
    // whose aura is already counted survives the ending of any other.
    m_blocks.Inhibit(Motion::Inhibition::Feared, source);
    m_movement.Take(new Move::FleeFrom(Move::Kind::Fear, enemy->Where().Pos(), 30.0f));
    Advance(false, false);
}

void UnitMovement::Confused(uint64 source)
{
    m_blocks.Inhibit(Motion::Inhibition::Confused, source);
    m_movement.Take(new Move::Scatter(Move::Kind::Confused, m_unit->Where().Pos(),
                                      6.0f, 500, 1500));
    Advance(false, false);
}

void UnitMovement::RunAskingHelp(float x, float y, float z)
{
    m_movement.Take(new Move::GoToPoint(Move::Kind::AssistRun, Geometry::Vector3(x, y, z)));
    Advance(false, false);
}

void UnitMovement::WalkPath(int32 pathId, uint32 source, uint32 overwriteEntry)
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

    m_pathId = pathId;
    m_pathOrigin = origin;
    m_movement.Take(new Move::WalkNodes(nodes));
    Advance(false, false);
}

Motion::Kind UnitMovement::Doing() const
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

void UnitMovement::FlyTo(uint32 id, float x, float y, float z)
{
    m_movement.Take(new Move::GoToPoint(Move::Kind::FlyLand, Geometry::Vector3(x, y, z), id));
    Advance(false, false);
}

void UnitMovement::ChargePoint(float x, float y, float z, float speed)
{
    Move::GoToPoint* charge = new Move::GoToPoint(Move::Kind::Effect, Geometry::Vector3(x, y, z));
    charge->AtSpeed(speed);
    m_movement.Take(charge);
    Advance(false, false);
}

void UnitMovement::ChargeTarget(Unit* target, float speed)
{
    if (!target)
    {
        return;
    }
    Geometry::Vector3 const& at = target->Where().Pos();
    ChargePoint(at.x, at.y, at.z, speed);
}

bool UnitMovement::JumpTo(float x, float y, float z, float speed, float apex, uint32 /*id*/)
{
    m_movement.Take(new Move::Ballistic(Geometry::Vector3(x, y, z), speed, apex));
    Advance(false, false);
    return true;
}

void UnitMovement::Fall()
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
    Advance(false, false);
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
    Advance(false, false);
    return true;
}

uint32 UnitMovement::ReachedNode() const
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Patrol);
    return held ? static_cast<Move::WalkNodes*>(held)->Reached() : 0;
}

uint32 UnitMovement::NextNode() const
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Patrol);
    return held ? static_cast<Move::WalkNodes*>(held)->Heading() : 0;
}

bool UnitMovement::Destination(Geometry::Vector3& out) const
{
    // IsMoving(), not merely "a route exists": between a leg ending and the route being
    // cleared there is still one there, and its last point is a place the unit has already
    // arrived at.
    if (!IsMoving())
    {
        return false;
    }
    out = m_movement.InFlight().End();
    return true;
}

bool UnitMovement::FacingNow(float& out) const
{
    if (!IsMoving())
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

void UnitMovement::StopRoute()
{
    if (IsMoving())
    {
        MoveSend::Halt(*m_unit);
    }
    m_movement.InFlight().Clear();
}


// ---------------------------------------------------------------- what is forbidden

void UnitMovement::Forbid(Motion::Inhibition what, uint64 source)
{
    if (!m_blocks.Inhibit(what, source))
    {
        return;   // the reason already held: another source, nothing changes
    }
    StopIfForbidden();
}

void UnitMovement::Allow(Motion::Inhibition what, uint64 source)
{
    m_blocks.Uninhibit(what, source);
    if (!m_blocks.Inhibited(what))
    {
        EndWhatItWasRunning(what);
    }
}

void UnitMovement::AllowAll(Motion::Inhibition what)
{
    // Every source at once. Taking the list by value first: releasing a source edits the
    // very list being walked.
    std::vector<uint64> const held = m_blocks.Sources(what);
    for (size_t i = 0; i < held.size(); ++i)
    {
        m_blocks.Uninhibit(what, held[i]);
    }
    EndWhatItWasRunning(what);
}

void UnitMovement::EndWhatItWasRunning(Motion::Inhibition what)
{
    // A reason that has a behaviour of its own cannot outlive it, nor it the reason: they
    // are the same fact, so they end together and in one place. The other reasons -- rooted,
    // stunned, dead, possessed -- only forbid; there is nothing of theirs to stop.
    Move::Kind shape = Move::Kind::Count;
    switch (what)
    {
        case Motion::Inhibition::Feared:   shape = Move::Kind::Fear;     break;
        case Motion::Inhibition::Confused: shape = Move::Kind::Confused; break;
        default: return;
    }

    if (!m_movement.Held(shape))
    {
        return;
    }
    m_movement.Drop(shape);
    Advance(false, false);
}

void UnitMovement::ReleaseAllFrom(Motion::SourceDomain domain)
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
            // Fear and confusion are NOT here any more: they are counted by source like
            // every other reason, so asking what is running would answer a question that
            // already has an owner -- and would answer it wrongly while a second aura still
            // holds a unit whose first one has ended.
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

void UnitMovement::StopIfForbidden()
{
    // Whatever was walking is not allowed to any more, and the leg in flight is already on
    // its way to every client that can see it. Stopping is not optional and not deferred:
    // a rooted unit that keeps walking is the defect this class exists to make impossible.
    if (!MayMove())
    {
        StopRoute();
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
    m_pending = 0;
    m_authority = RestingAuthority(m_unit);
}


// -------------------------------------------------------------------- who is driving

namespace
{
    /// Where control goes when nothing has taken it: a player drives itself, a creature is
    /// driven by the server, and that never changes for either.
    Authority RestingAuthority(Unit const* unit)
    {
        return (unit && unit->GetTypeId() == TYPEID_PLAYER) ? Authority::Client : Authority::Server;
    }

    uint32 NextCounter(uint32& seed)
    {
        const uint32 issued = seed++;
        if (seed == 0)
        {
            seed = 1;   // zero means "nothing outstanding"
        }
        return issued;
    }
}

uint32 UnitMovement::TakeControl()
{
    m_authority = Authority::Seized;
    m_pendingTo = Authority::Seized;
    m_pending = NextCounter(m_nextCounter);
    return m_pending;
}

uint32 UnitMovement::GiveControl()
{
    m_pendingTo = RestingAuthority(m_unit);
    m_pending = NextCounter(m_nextCounter);
    return m_pending;
}

bool UnitMovement::Confirmed(uint32 counter)
{
    // A counter that is not the outstanding one is a late answer to a handover that has
    // already ended. Honouring it would settle the NEXT one on the strength of the last.
    if (m_pending == 0 || counter != m_pending)
    {
        return false;
    }
    m_pending = 0;
    m_authority = m_pendingTo;
    return true;
}

void UnitMovement::SpeedChanged()
{
    // The packet spelled a speed as a duration over a length. Change the speed and that
    // duration now describes a different one, so the leg is laid again from where the mover
    // actually is -- which is what the route already knows.
    if (!IsMoving())
    {
        return;
    }
    StopRoute();
    Advance(false, false);
}


void UnitMovement::StopChasing()
{
    m_movement.Drop(Move::Kind::Chase);
    Advance(false, false);
}

Unit* UnitMovement::ChaseTarget() const
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Chase);
    if (!held || !m_unit)
    {
        return NULL;
    }
    return ObjectLookup::GetUnit(*m_unit, ObjectGuid(static_cast<Move::Pursue*>(held)->Target()));
}

Unit* UnitMovement::FollowTarget() const
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Follow);
    if (!held || !m_unit)
    {
        return NULL;
    }
    return ObjectLookup::GetUnit(*m_unit, ObjectGuid(static_cast<Move::Pursue*>(held)->Target()));
}

bool UnitMovement::PauseWaypoints(int32 ms)
{
    Move::Behaviour* held = m_movement.Held(Move::Kind::Patrol);
    if (!held || ms <= 0)
    {
        return false;
    }
    StopRoute();
    static_cast<Move::WalkNodes*>(held)->WaitFor(uint32(ms));
    return true;
}

bool UnitMovement::AddToSelectedPatrolPause(int32 ms)
{
    return PauseWaypoints(ms);
}

bool UnitMovement::CurrentWalkPath(int32& pathId, WaypointPathOrigin& origin) const
{
    if (!m_movement.Held(Move::Kind::Patrol))
    {
        return false;
    }
    pathId = m_pathId;
    origin = m_pathOrigin;
    return true;
}

void UnitMovement::DescribeWalkPath(std::ostringstream& oss) const
{
    int32 pathId = 0;
    WaypointPathOrigin origin = PATH_NO_PATH;
    if (!CurrentWalkPath(pathId, origin))
    {
        oss << "no waypoint path";
        return;
    }
    oss << "path " << pathId << " (origin " << uint32(origin) << "), heading for node "
        << NextNode() << ", last reached " << ReachedNode();
}

bool UnitMovement::FlyRoute(std::vector<uint32> const& route, uint32 startNode, float speed)
{
    if (route.size() < 2 || !m_unit)
    {
        return false;
    }

    // Every hop of the route, laid end to end: the client is told the whole thing as a few
    // long legs rather than one packet per node, and hears nothing from us in between.
    std::vector<Move::Flight::Node> nodes;
    for (size_t hop = 0; hop + 1 < route.size(); ++hop)
    {
        uint32 path = 0;
        uint32 cost = 0;
        sObjectMgr.GetTaxiPath(route[hop], route[hop + 1], path, cost);
        if (!path || path >= sTaxiPathNodesByPath.size())
        {
            continue;
        }

        TaxiPathNodeList const& leg = sTaxiPathNodesByPath[path];
        for (size_t i = 0; i < leg.size(); ++i)
        {
            TaxiPathNodeEntry const& entry = leg[i];

            // The first hop begins at the node the flight was joined from, not at the start
            // of its path: a player boarding halfway does not fly backwards first.
            if (hop == 0 && nodes.empty() && startNode != 0 && i < startNode)
            {
                continue;
            }

            Move::Flight::Node node;
            node.at = Geometry::Vector3(entry.Loc_0, entry.Loc_1, entry.Loc_2);
            node.delayMs = entry.Delay * IN_MILLISECONDS;
            node.arriveEvent = entry.ArrivalEventID;
            node.departEvent = entry.DepartureEventID;
            nodes.push_back(node);
        }
    }

    if (nodes.size() < 2)
    {
        return false;
    }

    // THE PASSENGER STOPS DRIVING HERE. Until the client answers, its own movement packets
    // describe a world it has not been told it left, and the route we are about to send is
    // the only truth about where it is.
    TakeControl();

    m_movement.Take(new Move::Flight(nodes, speed));
    Advance(false, false);
    return true;
}
