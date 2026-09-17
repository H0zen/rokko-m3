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

#include "TrackingMoves.h"

#include <cmath>
#include "Utilities/MathDefines.h"

namespace Motion
{
    namespace
    {
        constexpr float CONTACT_DISTANCE = 0.5f;            ///< retail's contact gap, on top of the two reaches
        constexpr float FOLLOW_RECALCULATE_FACTOR = 1.0f;   ///< how much of the two radii counts toward "the follow spot is stale"
        constexpr float FOLLOW_DIST_GAP_FOR_DIST_FACTOR = 3.0f;///< a follow distance beyond this gets extra slop
        constexpr float FOLLOW_DIST_RECALCULATE_FACTOR = 1.0f; ///< how much of it

        /// The world bearing from one point to another, normalised to [0, 2pi).
        float AngleFromTo(Vector3 const& from, Vector3 const& to)
        {
            const float a = std::atan2(to.y - from.y, to.x - from.x);
            return a >= 0.0f ? a : 2 * M_PI_F + a;
        }
    }

    // ---- TrackingBehaviour ----------------------------------------------------------

    void TrackingBehaviour::ResetTracking()
    {
        m_haveDest = false;
        m_relayLatch = false;
        m_routine = 0;
    }

    void TrackingBehaviour::LatchRelay(Sight const& sight)
    {
        // The leg ran out at the far end of a partial route, was cut short by a stop, or was
        // refused outright by the driver (no route under REQUIRE_PATH, or a partial one that
        // makes no progress -- in which case the leg goal the drift test reads was never even
        // touched). Each means: go on from here rather than from a spot we never reached. The
        // edge is latched, because it may arrive on a tick that holds (a cast, a control
        // state), and it is spent by the next tick that may move. One cause wins a tick that
        // carries several, most specific first.
        if (sight.status.cut)          { m_relayLatch = true; m_latchCause = RelayCause::Cut; }
        else if (sight.status.partial) { m_relayLatch = true; m_latchCause = RelayCause::Partial; }
        else if (sight.status.blocked) { m_relayLatch = true; m_latchCause = RelayCause::Blocked; }
    }

    float TrackingBehaviour::Bearing(Sight const& sight, Vector3 const& centre) const
    {
        if (Kind() == Motion::Kind::Chase && m_p.angle == 0.0f)
        {
            return AngleFromTo(centre, sight.position);   // head-on: approach from where the mover already is
        }
        return sight.target.facing + m_p.angle;
    }

    Step TrackingBehaviour::Activate(Sight const& sight, Services& /*svc*/)
    {
        // Initialize: the kind's state bit (never the _MOVE one: that follows a laid leg), the tracking reset.
        ResetTracking();
        Step s;
        s.resetLeg = true;
        // The order is load-bearing: the bit BEFORE the kind's own activation effects. The
        // deleted FollowMovementGenerator::Initialize did addUnitState(UNIT_STAT_FOLLOW) and
        // only then SyncSpeedWithMaster, because UpdateSpeed's pet branch copies the owner's
        // rate only while that bit is set (UnitSpeed.cpp) -- a sync ahead of the bit reads the
        // pet's own rate and the follower never matches its master. The chase's Walk(false)
        // then trails its bit too, harmlessly: SetWalk reads no unit state.
        s.effects.push_back(Effect::State(m_p.stateSet, 0));
        OnActivate(sight, s);
        return s;
    }

    Step TrackingBehaviour::Resume(Sight const& sight, Services& svc, bool reset)
    {
        return reset ? Activate(sight, svc) : Step::None();   // Reset was Initialize
    }

    Step TrackingBehaviour::Suspend()
    {
        // Interrupt: InterruptMoving, both bits cleared, the tracking reset.
        ResetTracking();
        Step s;
        s.interrupt = true;
        s.resetLeg = true;
        s.effects.push_back(Effect::State(0, m_p.stateSet | m_p.stateMove));
        OnSuspendOrFinish(s.effects);
        return s;
    }

    Outcome TrackingBehaviour::Finish(FinishReason why, Sight const& /*sight*/, Services& /*svc*/)
    {
        Outcome o;
        o.interrupt = Displacing(why);   // the generator's Interrupt stopped the mover; Finalize did not
        o.effects.push_back(Effect::State(0, m_p.stateSet | m_p.stateMove));
        OnSuspendOrFinish(o.effects);
        return o;
    }

    FinishReason TrackingBehaviour::EndReason(Sight const& /*sight*/) const
    {
        return FinishReason::TargetLost;
    }

    bool TrackingBehaviour::DriftedBeyond(Sight const& sight, float edge) const
    {
        // A flier cares about height too, and so does a swimmer in the water column; anything
        // on the ground does not. The goal is the one the driver actually laid a leg to.
        const Vector3& goal = sight.status.legGoal;
        const Vector3& t = sight.target.position;
        float d2 = (goal.x - t.x) * (goal.x - t.x) + (goal.y - t.y) * (goal.y - t.y);
        if (sight.canFlyHint || sight.swimming) { d2 += (goal.z - t.z) * (goal.z - t.z); }
        return d2 > edge * edge;
    }

    void TrackingBehaviour::Derive(Sight const& sight, Services& svc, RelayCause why, Step& s)
    {
        const Vector3 centre = AimCentre(sight);
        Vector3 spot;
        if (!svc.StandingSpot(centre, StandingDistance(sight), Bearing(sight, centre), spot))
        {
            spot = centre;   // no free spot: the centre itself, as the generator's NearPoint fell back to the raw point
        }
        m_dest = spot;
        m_haveDest = true;
        m_relays.Count(why);
        s.effects.push_back(Effect::State(m_p.stateMove, 0));
    }

    Step TrackingBehaviour::Tick(Sight const& sight, Services& svc, uint32 diff)
    {
        // 1. The target is gone.
        if (!sight.target.valid) { return Step::Of(MoveIntent::Done()); }
        // 2. The mover is dead.
        if (!sight.alive) { return Step::Of(MoveIntent::Hold()); }
        // 3. A state holds it (the chase also under NO_COMBAT_MOVEMENT), or the chase lost its victim.
        const bool held = !sight.canMove || (UsesCombatMovement() && sight.combatMovementHeld) || LostTarget(sight);
        if (held)
        {
            LatchRelay(sight);
            Step s = Step::Of(MoveIntent::Hold());
            s.effects.push_back(Effect::State(0, m_p.stateMove));
            return s;
        }
        // 4. A cast with a cast time, or a channel: stop (every casting tick; the shell sends no packet once stopped), hold.
        if (svc.Casting())
        {
            LatchRelay(sight);
            Step s = Step::Of(MoveIntent::Hold());
            // Unconditional, as the generator's own gate was: it called StopMoving() whenever
            // !IsStopped(), and StopMoving clears the _MOVE bits BEFORE its finalized-spline
            // early return (Unit::StopMoving). A standing chaser that starts a cast must lose
            // its move bit too, so gating the stop on a live leg would leave that bit set for
            // the whole cast. Nothing goes on the wire for a spline that is already finalized.
            s.stop = true;
            return s;
        }
        // 5. The routine cadence: has the target drifted past the edge from the leg's goal?
        bool needDest = !m_haveDest;
        RelayCause cause = RelayCause::First;
        m_routine -= int32(diff);
        if (m_routine <= 0)
        {
            m_routine = int32(m_p.routineMs);
            if (m_haveDest && Drifted(sight)) { needDest = true; cause = RelayCause::Routine; }
        }
        // 6. The event recoveries: a cut, a partial or a refused leg (latched, consumed on a tick that moves).
        LatchRelay(sight);
        if (m_relayLatch)
        {
            needDest = true;
            if (m_haveDest) { cause = m_latchCause; }   // the very first spot is First, whatever edge shares its tick
            m_relayLatch = false;
        }
        // 7. A finished leg whose target has drifted (the driver reports arrived once). The
        //    generator had no such case: it caught the same drift on its next 100 ms poll,
        //    which this native's one-second cadence no longer offers, so the design counts
        //    the finished leg as a recovery cause of its own.
        if (!needDest && sight.status.arrived && Drifted(sight)) { needDest = true; cause = RelayCause::Finished; }
        Step s;
        if (needDest) { Derive(sight, svc, cause, s); }
        // 8. Standing still, whether or not a fresh spot was just derived: the kind's idle
        //    work (the chase engages). The generator's ReachTarget ran on the same condition,
        //    so a chase begun already inside contact attacks on its very first tick.
        if (!sight.status.traveling && !sight.status.partial) { OnIdle(sight, s); }
        // 9. Nothing changed: hold with the kind's facing.
        if (!needDest && !sight.status.traveling)
        {
            s.apply = true;
            s.intent = MoveIntent::Hold(FacingFor(sight, false));
            return s;
        }
        // 10. The leg.
        uint32 flags = MOVE_REQUIRE_PATH;
        if (Walks(sight)) { flags |= MOVE_WALK; }
        if (ForcesDestination(sight)) { flags |= MOVE_FORCE_DEST; }
        s.apply = true;
        s.intent = MoveIntent::Move(m_dest, flags, FacingFor(sight, true));
        return s;
    }

    // ---- ChaseBehaviour -------------------------------------------------------------

    float ChaseBehaviour::StandingDistance(Sight const& sight) const
    {
        return m_p.offset + CONTACT_DISTANCE + sight.target.reachSum;   // retail's stop, design §6.1
    }

    bool ChaseBehaviour::Drifted(Sight const& sight) const
    {
        // The re-approach edge is the client's own melee range from the leg's goal (design
        // §6.1); the band's asymmetry is deliberate, since closing enforces only the stop.
        return DriftedBeyond(sight, m_p.offset + sight.target.meleeRange);
    }

    Vector3 ChaseBehaviour::AimCentre(Sight const& sight) const
    {
        Vector3 c = sight.target.position;
        if (m_c.lead && sight.target.velocityTrusted)
        {
            c = c + sight.target.velocity * (float(m_c.leadMs) / 1000.0f);
        }
        return c;
    }

    Facing ChaseBehaviour::FacingFor(Sight const& /*sight*/, bool /*moving*/) const
    {
        return m_p.angle == 0.0f ? Facing::ToTarget(m_p.target) : Facing();
    }

    void ChaseBehaviour::OnActivate(Sight const& sight, Step& s)
    {
        if (sight.isCreature) { s.effects.push_back(Effect::Walk(false)); }   // a chase runs
    }

    void ChaseBehaviour::OnIdle(Sight const& /*sight*/, Step& s)
    {
        s.effects.push_back(Effect(Effect::EngageInReach));   // re-emitted every idle tick: a stale miss never latches
    }

    void ChaseBehaviour::OnSuspendOrFinish(std::vector<Effect>& /*effects*/) {}

    // ---- FollowBehaviour ------------------------------------------------------------

    float FollowBehaviour::StandingDistance(Sight const& sight) const
    {
        return m_p.offset + sight.extent + sight.target.extent;
    }

    bool FollowBehaviour::Drifted(Sight const& sight) const
    {
        // The generator's tolerance, with the config honoured: the bounding radii are folded
        // in exactly as WorldObject's own IsWithinDist2d/3d folds them.
        float allowed = m_f.recalcRange - sight.target.extent + FOLLOW_RECALCULATE_FACTOR * (sight.extent + sight.target.extent);
        if (m_p.offset > FOLLOW_DIST_GAP_FOR_DIST_FACTOR) { allowed += FOLLOW_DIST_RECALCULATE_FACTOR * m_p.offset; }
        return DriftedBeyond(sight, allowed + sight.target.extent);
    }

    Vector3 FollowBehaviour::AimCentre(Sight const& sight) const
    {
        Vector3 c = sight.target.position;
        if (sight.target.velocityTrusted) { c = c + sight.target.velocity * (float(m_f.horizonMs) / 1000.0f); }
        return c;
    }

    Facing FollowBehaviour::FacingFor(Sight const& sight, bool moving) const
    {
        return moving ? Facing() : Facing::ToAngle(sight.target.facing);   // the leader's facing at rest only
    }

    void FollowBehaviour::OnActivate(Sight const& /*sight*/, Step& s)
    {
        s.effects.push_back(Effect(Effect::SyncSpeed));
    }

    void FollowBehaviour::OnSuspendOrFinish(std::vector<Effect>& effects)
    {
        effects.push_back(Effect(Effect::SyncSpeed));
    }

    // ---- HomeBehaviour ---------------------------------------------------------------

    Step HomeBehaviour::Activate(Sight const& /*sight*/, Services& /*svc*/)
    {
        m_cleared = false;
        m_arrived = false;
        Step s;
        s.resetLeg = true;   // the clear waits for the first tick: under a block the mask would erase the block's own mirrors
        return s;
    }

    Step HomeBehaviour::Resume(Sight const& /*sight*/, Services& /*svc*/, bool /*reset*/)
    {
        return Step::None();   // Reset was a no-op
    }

    Step HomeBehaviour::Tick(Sight const& sight, Services& /*svc*/, uint32 /*diff*/)
    {
        Step s;
        if (!m_cleared)
        {
            m_cleared = true;
            s.effects.push_back(Effect::State(0, m_p.stateClear));
        }
        // A creature that could not be sent home at all still counts as home: evade must always terminate.
        if (sight.status.arrived || sight.status.blocked)
        {
            m_arrived = true;
            s.apply = true;
            s.intent = MoveIntent::Done();
            return s;
        }
        // A stop on the way (a stun, a root) is not an arrival: the leg is re-stated once the unit may move again.
        s.apply = true;
        s.intent = MoveIntent::Move(m_p.home, MOVE_FORCE_DEST, Facing::ToAngle(m_p.facing));
        return s;
    }

    Outcome HomeBehaviour::Finish(FinishReason why, Sight const& sight, Services& /*svc*/)
    {
        Outcome o;   // never an interrupt: the generator's Interrupt was a no-op
        if (Displacing(why) || !m_arrived) { return o; }
        o.effects.push_back(Effect(Effect::RestoreTemporaryFaction));
        o.effects.push_back(Effect::Walk(!sight.runningState && !sight.levitating));
        o.effects.push_back(Effect(Effect::LoadAddon));
        o.effects.push_back(Effect(Effect::JustReachedHome));
        return o;
    }
}
