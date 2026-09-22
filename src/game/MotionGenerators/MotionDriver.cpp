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

#include "MotionDriver.h"
#include "ObjectLookup.h"
#include "Unit.h"
#include "movement/MoveSpline.h"
#include "movement/MoveSplineInit.h"

#include <cmath>

namespace
{
    /// A goal that moved less than this is treated as the same goal. Re-laying a leg
    /// for a sub-yard nudge floods the client with SMSG_MONSTER_MOVE and reads on
    /// screen as a foot-slide.
    constexpr float MIN_RELAY_DISTANCE = 0.5f;

    /// Below this the unit already faces where it was asked to; re-orienting would
    /// only cost a packet.
    constexpr float FACING_EPSILON = 0.01f;

    /// A leg whose whole geometry covers less ground than this moves the unit nowhere:
    /// it is the unit's own position, to the centimetre. NOT a minimum leg length -- a
    /// length floor was rejected in #114 because it would swallow real short legs (a
    /// patrol node a yard away, the chase's last step into contact) and starve the
    /// `arrived` they wait on. This is the degenerate case only, and arrival is still
    /// reported: a unit standing on its goal HAS arrived.
    constexpr float NOWHERE_DISTANCE = 0.01f;

    /// The ground a point array covers, start to end, through every point.
    float GroundCovered(Motion::PointsArray const& points)
    {
        float covered = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
        {
            covered += (points[i] - points[i - 1]).length();
        }
        return covered;
    }
}

void MotionDriver::ResetLeg()
{
    m_legGoal = Motion::Vector3();
    m_legFacing = Motion::Facing::Mode::None;
    m_haveLeg = false;
    m_partialLeg = false;
    m_blocked = false;
    m_speedChanged = false;
    m_wasTraveling = false;
    m_arrivedInPlace = false;
}

Motion::IPathQuery* MotionDriver::Query(Unit const& owner)
{
    Motion::IMotionFrame const& frame = Motion::FrameFor(owner);

    // A leg never spans two frames: if the mover changed frame since the last leg, the
    // old router speaks the wrong coordinate system. Nor two maps: the router binds the
    // map's mesh and the instance's query at construction, and a behaviour outlives a
    // teleport.
    if (!m_query || m_queryFrame != frame.Kind() ||
        m_queryMapId != owner.GetMapId() || m_queryInstanceId != owner.GetInstanceId())
    {
        m_query = frame.CreatePathQuery(owner);
        m_queryFrame = frame.Kind();
        m_queryMapId = owner.GetMapId();
        m_queryInstanceId = owner.GetInstanceId();
    }

    return m_query.get();
}

Motion::MoveStatus MotionDriver::BeginTick(Unit& owner)
{
    const bool traveling = !owner.movespline->Finalized();

    Motion::MoveStatus status;
    status.traveling = traveling;
    status.blocked = m_blocked;

    // A leg that just ended did so in one of three ways, and they must not be confused:
    // it ran out at its goal (arrived), something stopped or interrupted it (cut), or it
    // ran out at the far end of a route that only got partway (partial).
    if (m_wasTraveling && !traveling)
    {
        if (owner.movespline->Cut())
        {
            status.cut = true;
        }
        else if (m_partialLeg)
        {
            status.partial = true;
        }
        else
        {
            status.arrived = true;
        }
    }

    // The goal was underfoot and no leg was laid for it (LayLeg). The behaviour still gets
    // its arrival -- standing on the goal is the one case where "arrived" needs no travel --
    // it just does not cost an SMSG_MONSTER_MOVE to every observer to say so.
    if (m_arrivedInPlace)
    {
        status.arrived = true;
        m_arrivedInPlace = false;
    }

    status.pathIndex = owner.movespline->Initialized() ? owner.movespline->currentPathIdx() : 0;

    if (m_haveLeg)
    {
        status.legGoal = m_legGoal;
    }

    // Both edges are reported exactly once. A sticky `blocked` would starve every
    // behaviour whose answer to it is "reset my retry timer and try again in a moment":
    // it would reset the timer on every tick and never fire it.
    m_blocked = false;
    m_wasTraveling = traveling;

    return status;
}

bool MotionDriver::Apply(Unit& owner, Motion::MoveIntent const& intent)
{
    switch (intent.act)
    {
        case Motion::MoveIntent::Act::Done:
            return false;

        case Motion::MoveIntent::Act::Move:
            ReconcileMove(owner, intent);
            return true;

        case Motion::MoveIntent::Act::Hold:
            ReconcileHold(owner, intent);
            return true;

        case Motion::MoveIntent::Act::Launch:
            // An Effect's arc is launched by the shell adapter itself (NativeBehaviour::Launch,
            // once, at activation) and never reaches the driver; a Launch here is a caller's error.
            return false;
    }

    return true;
}

bool MotionDriver::ReconcileMove(Unit& owner, Motion::MoveIntent const& intent)
{
    // Lay a fresh leg when there is nothing to ride, or -- for a goal that tracks
    // something that moves -- when it has drifted past tolerance. A live leg whose goal
    // is still fresh is left alone: re-routing every tick reads as a foot-slide.
    bool relay = !m_haveLeg || owner.movespline->Finalized();

    // A speed change re-paces a routed leg, but must NOT re-lay an explicit one: that
    // geometry was built once, from the leg's START, and rebuilding it from a point
    // halfway along would walk the unit back to the beginning of its own path.
    // Nor a leg with its own speed override (the charge): the unit's pace is not what paces it.
    if (!relay && m_speedChanged && !intent.path && intent.speed <= 0.0f)
    {
        relay = true;
    }

    if (!relay)
    {
        const Motion::Vector3 drift = intent.goal - m_legGoal;
        relay = drift.squaredLength() > MIN_RELAY_DISTANCE * MIN_RELAY_DISTANCE;
    }

    return relay ? LayLeg(owner, intent) : false;
}

bool MotionDriver::LayLeg(Unit& owner, Motion::MoveIntent const& intent)
{
    Movement::MoveSplineInit init(owner);
    m_partialLeg = false;

    // How much ground the leg would actually cover. Measured per branch because only the
    // routed one knows its geometry before Launch; -1 means "not measured", which no
    // branch currently leaves but which reads as "do not judge it".
    float covered = -1.0f;

    if (intent.path && intent.path->size() >= 2)
    {
        // The behaviour dictated the exact geometry (the smoothed patrol).
        init.MovebyPath(*intent.path);
        covered = GroundCovered(*intent.path);
    }
    else if (intent.Has(Motion::MOVE_STRAIGHT))
    {
        // No routing at all: jumps, effects, forced moves.
        init.MoveTo(intent.goal.x, intent.goal.y, intent.goal.z, false);
        covered = (intent.goal - Motion::FrameFor(owner).MoverPosition(owner)).length();
    }
    else
    {
        // Route toward the goal through the mover's frame -- the one call behind which
        // collision, obstacle avoidance and any future deck live.
        Motion::IPathQuery* query = Query(owner);
        const Motion::Vector3 start = Motion::FrameFor(owner).MoverPosition(owner);

        const bool routed = query && query->Calculate(start, intent.goal,
                                                      intent.Has(Motion::MOVE_FORCE_DEST),
                                                      intent.pathLengthLimit);

        // Nothing usable at all, or the router failed and this movement kind refuses the
        // straight-line fallback. Either way no leg is laid, and the behaviour is told
        // so next tick so it can give up or pick somewhere else.
        // Likewise a partial route that gets no closer to the goal: laying it would walk
        // nowhere and re-lay itself from the same spot every tick.
        if (!routed || (intent.Has(Motion::MOVE_REQUIRE_PATH) && query->Failed()) ||
            (query->Partial() && !query->Progresses()))
        {
            m_blocked = true;
            return false;
        }

        init.MovebyPath(query->Points());
        covered = GroundCovered(query->Points());
        m_partialLeg = query->Partial();
    }

    // Nowhere to go. The goal is the ground the unit is standing on, so the leg would be a
    // 1 ms spline to the unit's own feet -- 9 364 of them in the user's capture, one
    // SMSG_MONSTER_MOVE each to every observer, for a step of nothing. Retail never sends
    // one: over the movement corpus (peer/retail-fear-movement-2026-09-20.md) it ends a
    // move with a type-1 stop and otherwise puts nothing on the wire when there is nowhere
    // to go. So neither do we -- and the behaviour is told it arrived, which is simply true.
    //
    // Only with no final facing to deliver: a zero-length leg that carries one is how a unit
    // turns on the spot, and that packet says something. Only with nothing running: a live
    // leg's real start is the spline's computed position, not the placement this measured
    // against, and cutting one short is never this function's business.
    if (covered >= 0.0f && covered <= NOWHERE_DISTANCE &&
        intent.facing.mode == Motion::Facing::Mode::None &&
        owner.movespline->Finalized() && !owner.PendingSplineCommit())
    {
        m_legGoal = intent.goal;
        m_legFacing = Motion::Facing::Mode::None;
        m_haveLeg = true;
        m_blocked = false;          // not blocked: the unit is AT the goal, it is not kept from it
        m_speedChanged = false;
        m_wasTraveling = false;
        m_arrivedInPlace = true;
        return false;
    }

    switch (intent.facing.mode)
    {
        case Motion::Facing::Mode::Angle:
            init.SetFacing(intent.facing.angle);
            break;

        case Motion::Facing::Mode::Spot:
            init.SetFacing(intent.facing.spot);
            break;

        case Motion::Facing::Mode::Target:
            if (Unit* target = ObjectLookup::GetUnit(owner, ObjectGuid(intent.facing.target)))
            {
                init.SetFacing(target);
            }
            break;

        case Motion::Facing::Mode::None:
            break;
    }

    init.SetWalk(intent.Has(Motion::MOVE_WALK));

    if (intent.Has(Motion::MOVE_FLY))
    {
        init.SetFly();
    }

    if (intent.Has(Motion::MOVE_SMOOTH))
    {
        // An uncompressed Catmull-Rom path: 4.3.4 reads float path points only with
        // UncompressedPath (SetSmooth sets it); the packed linear path wraps at +-255 yd,
        // which every taxi route exceeds (design v2 §5).
        init.SetSmooth();
    }

    // The velocity is left to MoveSplineInit, which resolves the unit's live
    // walk/run/swim/flight speed at Launch -- so a speed change re-paces the next leg
    // instead of a stale value being baked in here.
    if (intent.speed > 0.0f)
    {
        // a behaviour's speed override (the charge); otherwise MoveSplineInit resolves the live speed at Launch
        init.SetVelocity(intent.speed);
    }

    if (init.Launch() == 0)
    {
        // The spline refused the leg (Validate): nothing is running, and the next tick
        // must not judge whatever spline was there before.
        m_blocked = true;
        return false;
    }

    m_legGoal = intent.goal;
    m_legFacing = intent.facing.mode;   // the leg that was actually launched, for the facade read
    m_haveLeg = true;
    m_blocked = false;
    m_speedChanged = false;
    m_wasTraveling = !owner.movespline->Finalized();
    // A leg is running again, so whatever an earlier round of this same tick decided it had
    // arrived at in place is stale: the spline's own end will report this one.
    m_arrivedInPlace = false;

    return true;
}

void MotionDriver::ReconcileHold(Unit& owner, Motion::MoveIntent const& intent)
{
    // A running leg is deliberately NOT cut short: letting it finish is what stops an
    // arriving chase from stuttering a yard short of its victim. A behaviour that really
    // must halt calls Unit::StopMoving itself -- a unit-level action, not a decision
    // about the next leg.
    if (!owner.movespline->Finalized())
    {
        return;
    }

    // Past the guard the hold is what the driver is acting on, so its facing is what the facade
    // reports -- whether or not the switch below has to move anything (a unit already facing the
    // right way is still being held there). Before the guard nothing happened and the launched
    // leg's own mode stands.
    m_legFacing = intent.facing.mode;

    switch (intent.facing.mode)
    {
        case Motion::Facing::Mode::Target:
        {
            Unit* target = ObjectLookup::GetUnit(owner, ObjectGuid(intent.facing.target));
            if (target && !owner.Where().HasInArc(target->Where(), FACING_EPSILON))
            {
                owner.SetInFront(target);
            }
            break;
        }
        case Motion::Facing::Mode::Angle:
        {
            if (std::fabs(owner.Where().Facing() - intent.facing.angle) > FACING_EPSILON)
            {
                owner.SetFacingTo(intent.facing.angle);
            }
            break;
        }
        case Motion::Facing::Mode::Spot:
        {
            const float angle = owner.Where().BearingTo(Geometry::Vector2(intent.facing.spot.x, intent.facing.spot.y));
            if (std::fabs(owner.Where().Facing() - angle) > FACING_EPSILON)
            {
                owner.SetFacingTo(angle);
            }
            break;
        }
        case Motion::Facing::Mode::None:
            break;
    }
}
