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

#include "ControlMoves.h"

#include <algorithm>
#include <cmath>

namespace Motion
{
    // ---- Fear -----------------------------------------------------------------------------

    Step FearBehaviour::Activate(Sight const&, Services&)
    {
        // The generator's Initialize: +FLEEING_MOVE, then StopMoving() -- which clears the moving
        // mask, that bit included, so the pair nets to a CLEARED bit and no latch belongs
        // here; a creature runs and drops its target; the rest cleared, no point, the leg
        // forgotten. The timed clock is untouched: only construction sets it.
        m_rest = 0;
        m_havePoint = false;
        Step s;
        s.stop = true;
        s.effects.push_back(Effect::Walk(false));
        s.effects.push_back(Effect(Effect::ClearTarget));
        s.resetLeg = true;
        return s;
    }

    Step FearBehaviour::Suspend()
    {
        // The generator's Interrupt: InterruptMoving, the leg latch alone cleared (the flee state
        // is the shell's published block and outlives a suspension), no point, the leg forgotten.
        m_havePoint = false;
        Step s;
        s.interrupt = true;
        s.effects.push_back(Effect::Latch(0, LatchLeg));
        s.resetLeg = true;
        return s;
    }

    Step FearBehaviour::Resume(Sight const& sight, Services& svc, bool reset)
    {
        return reset ? Activate(sight, svc) : Step::None();   // the generator's Reset is its Initialize: a fresh bearing from the current spot
    }

    bool FearBehaviour::PickFleePoint(Sight const& sight, Services& svc, Vector3& out)
    {
        FearGeometry const& g = m_p.geometry;

        // Draw 1, always: the bearing a fright that cannot be found leaves in force.
        float distFromCaster = 0.0f;
        float angleToCaster = svc.Frand(0, 2 * M_PI_F);

        Vector3 frightPosition;
        float frightDistance = 0.0f;
        if (svc.Fright(m_p.fright, frightPosition, frightDistance))
        {
            // The DISTANCE needs no correction: a rigid transform preserves lengths. The
            // BEARING does, so it is taken between frame positions (sight.position stands in
            // for the generator's MoverPosition, family 2's rule).
            distFromCaster = frightDistance;
            if (distFromCaster > 0.2f)
            {
                angleToCaster = AngleFromTo(frightPosition, sight.position);
            }
        }

        float dist, angle;
        if (distFromCaster < g.minQuiet)
        {
            // Too close: bolt more or less straight away from it.
            dist = svc.Frand(g.closeFactorMin, g.closeFactorMax) * (g.minQuiet - distFromCaster);
            angle = angleToCaster + svc.Frand(-g.closeJitter, g.closeJitter);
        }
        else if (distFromCaster > g.maxQuiet)
        {
            // Further than the panic band: drift back toward it (toward the caster is bearing + pi).
            dist = svc.Frand(g.farFactorMin, g.farFactorMax) * (g.maxQuiet - g.minQuiet);
            angle = angleToCaster + M_PI_F + svc.Frand(-g.farJitter, g.farJitter);
        }
        else
        {
            // Inside the band: mill about in any direction.
            dist = svc.Frand(g.bandFactorMin, g.bandFactorMax) * (g.maxQuiet - g.minQuiet);
            angle = svc.Frand(0, 2 * M_PI_F);
        }

        Vector3 const& from = sight.position;
        // Unqualified cos/sin with <cmath>, exactly as the generator wrote them: the same overload
        // resolution keeps the record's flee points byte-identical.
        const Vector3 guess(from.x + dist * cos(angle),
                            from.y + dist * sin(angle),
                            from.z + 0.5f);

        // The frame drops the guess onto whatever it considers ground and, for a player, pulls
        // it back to the first obstruction on the way there (the port's GroundPoint).
        return svc.GroundPoint(guess, out);
    }

    Step FearBehaviour::Tick(Sight const& sight, Services& svc, uint32 diff)
    {
        // The timed variant's clock first, before any draw: the generator's subclass updated it
        // and returned Done before the base tick ran.
        if (m_p.timeLimitMs)
        {
            if (m_totalLeft > 0)
            {
                m_totalLeft -= int32(diff);
            }
            if (m_totalLeft <= 0)
            {
                return Step::Of(MoveIntent::Done());
            }
        }
        if (!sight.alive)
        {
            return Step::Of(MoveIntent::Done());
        }
        // No mobility guard: MotionMaster::UpdateMotion withholds a blocked Control selection's
        // tick (Mobility::Decide), so a tick that arrives may move (design fact 6).

        // Nowhere to run THAT way: pick a different bearing in a moment -- and fall through, as
        // the generator did: a tick whose diff passes the retry picks again at once.
        if (sight.status.blocked)
        {
            m_havePoint = false;
            m_rest = int32(m_p.geometry.retryMs);
        }

        if (sight.status.traveling && m_havePoint)
        {
            return Step::Of(MoveIntent::Move(m_point, MOVE_REQUIRE_PATH));
        }

        // Standing: catch a breath before the next bolt (the rest counts only here).
        m_rest -= int32(diff);
        if (m_rest > 0)
        {
            return Step::Of(MoveIntent::Hold());
        }

        Vector3 point;
        if (!PickFleePoint(sight, svc, point))
        {
            m_rest = int32(m_p.geometry.retryMs);
            return Step::Of(MoveIntent::Hold());
        }

        m_point = point;
        m_havePoint = true;
        // Two modes, not one band (the cadence note of 2026-09-21). Retail rests after about
        // every OTHER leg and chains the rest straight on; we rested after every one. The coin
        // is drawn through the port like every other draw -- never rand() -- so the harness's
        // seeded record stays reproducible, and it is drawn FIRST and unconditionally, so a
        // bolt's place in the stream does not depend on the branch it takes. A LOW roll rests:
        // that keeps the rested branch the one a Services stub answering its minimum exercises.
        const uint32 restedPercent = m_p.geometry.chainPercent < 100 ? 100 - m_p.geometry.chainPercent : 0;
        m_rest = svc.Urand(1, 100) <= restedPercent
                     ? int32(svc.Urand(m_p.geometry.restMin, m_p.geometry.restMax))
                     : 0;   // chained: the next bolt is laid as this one ends

        Step s = Step::Of(MoveIntent::Move(m_point, MOVE_REQUIRE_PATH).WithinLength(m_p.geometry.legLimit));
        s.effects.push_back(Effect::Latch(LatchLeg, 0));   // the generator added the bit before its Move
        return s;
    }

    Outcome FearBehaviour::Finish(FinishReason why, Sight const& sight, Services& svc)
    {
        Outcome o;
        if (Displacing(why))
        {
            // The generator's Interrupt carried the move bit's clear, and the adapter skipped
            // Interrupt for a behaviour it had already suspended -- whose Suspend() cleared the
            // bit itself, so a bit set since then belongs to the claim that drives now and this
            // finish must leave it alone (the Sight carries that: the shell fills `suspended`
            // from its own Suspend()/Resume() bookkeeping). LegacyBehaviour's cleanup gait
            // restore is unconditional on suspension, though, and runs LIVE, after the clear,
            // unless another fear claim survives and keeps the run. The arbiter has erased the
            // finishing claim already, so ClaimHeld answers for a survivor.
            o.interrupt = true;
            if (!sight.suspended)
            {
                o.effects.push_back(Effect::Latch(0, LatchLeg));
            }
            if (sight.isCreature && !svc.ClaimHeld(Motion::Kind::Fear))
            {
                o.effects.push_back(Effect(Effect::RestoreGait));
            }
            return o;
        }
        if (m_p.timeLimitMs)
        {
            // The timed variant's Finalize (Expired, Died, Cleared): the move bit cleared, the
            // client-visible flag dropped when no fear claim remains (the low-health flee has
            // no aura to clear it), the gait restored unconditionally (design §6.5: the
            // generator left the run), and the panic over, back to whatever frightened us.
            o.effects.push_back(Effect::Latch(0, LatchLeg));
            if (!svc.ClaimHeld(Motion::Kind::Fear))
            {
                o.effects.push_back(Effect(Effect::ClearFleeingFlag));
            }
            if (sight.isCreature)
            {
                o.effects.push_back(Effect(Effect::RestoreGait));
            }
            o.effects.push_back(Effect(Effect::AttackVictim));
            return o;
        }
        // The untimed Finalize: a creature's gait read BEFORE the clear (a death mid-leg keeps
        // the run, as the generator's order did), a player stopped, then the move bit cleared.
        if (sight.isCreature)
        {
            o.effects.push_back(Effect(Effect::RestoreGait));
        }
        else
        {
            o.stop = true;
        }
        o.effects.push_back(Effect::Latch(0, LatchLeg));
        return o;
    }

    // ---- Confused -------------------------------------------------------------------------

    Step ConfusedBehaviour::Activate(Sight const& sight, Services&)
    {
        // Anchored to wherever the unit stood when it lost its wits (sight.position stands in
        // for the generator's MoverPosition, family 2's rule); then the generator's Initialize.
        m_anchor = sight.position;
        return Restart(sight);
    }

    Step ConfusedBehaviour::Restart(Sight const& sight)
    {
        m_stagger = 0;
        m_haveLurch = false;
        Step s;
        s.resetLeg = true;
        if (!sight.alive || sight.notMove)
        {
            return s;   // the generator returned before its stop and its bit (UNIT_STAT_NOT_MOVE: root, stun, death, a distract's stand)
        }
        // The stop first, then the latch: a stop's ClearMovingLatches never clears the confuse's leg, so it ends SET.
        s.stop = true;
        s.effects.push_back(Effect::Latch(LatchLeg, 0));
        return s;
    }

    Step ConfusedBehaviour::Suspend()
    {
        // The generator's Interrupt: InterruptMoving, the leg latch cleared (the confused state
        // is the shell's published block and outlives a suspension), no lurch, the leg forgotten.
        m_haveLurch = false;
        Step s;
        s.interrupt = true;
        s.effects.push_back(Effect::Latch(0, LatchLeg));
        s.resetLeg = true;
        return s;
    }

    Step ConfusedBehaviour::Resume(Sight const& sight, Services&, bool reset)
    {
        return reset ? Restart(sight) : Step::None();   // the generator's Reset: the anchor is NOT re-captured
    }

    uint32 ConfusedBehaviour::RetryDelay()
    {
        // Every failed attempt doubles the wait, up to the shortest normal stagger, so a spot
        // that never routes is retried a few times a second, not twenty. A lurch that gets laid
        // resets the count (the tick's arrived/traveling rule).
        const uint32 delay = std::min<uint32>(m_p.retryMs << m_retries, m_p.staggerMin);
        if (delay < m_p.staggerMin)
        {
            ++m_retries;
        }
        return delay;
    }

    Step ConfusedBehaviour::Tick(Sight const& sight, Services& svc, uint32 diff)
    {
        // No mobility guard (design fact 6). The leg latch is re-asserted every tick: a wipe from
        // elsewhere may clear it, and the generator wrote its bit back at the top of each Intent.
        Step s;
        s.effects.push_back(Effect::Latch(LatchLeg, 0));

        // A refused leg: no lurch, the retry wait -- and fall through, as the generator did: a
        // tick whose diff passes it picks again at once, and a refused pick in that same tick
        // advances the count a second time.
        if (sight.status.blocked)
        {
            m_haveLurch = false;
            m_stagger = int32(RetryDelay());
        }

        // A lurch that got laid -- seen traveling, or already run out -- ends the failure streak.
        if (sight.status.arrived || (sight.status.traveling && m_haveLurch))
        {
            m_retries = 0;
        }

        // The timer runs even mid-leg, which is the point: when it fires early the new
        // destination supersedes the one being walked to and the leg is cut off part-way.
        m_stagger -= int32(diff);
        if (m_stagger > 0)
        {
            s.apply = true;
            s.intent = (sight.status.traveling && m_haveLurch)
                ? MoveIntent::Move(m_lurch, MOVE_WALK)
                : MoveIntent::Hold();
            return s;
        }

        Vector3 lurch;
        if (!svc.RandomPoint(m_anchor, m_p.radius, lurch))   // the port's one call carries the generator's draws: two on the ground, two or three in the air or under water
        {
            m_stagger = int32(RetryDelay());
            s.apply = true;
            s.intent = MoveIntent::Hold();
            return s;
        }

        m_lurch = lurch;
        m_haveLurch = true;
        m_stagger = int32(svc.Urand(m_p.staggerMin, m_p.staggerMax));

        s.apply = true;
        s.intent = MoveIntent::Move(m_lurch, MOVE_WALK);
        return s;
    }

    Outcome ConfusedBehaviour::Finish(FinishReason why, Sight const& sight, Services& /*svc*/)
    {
        Outcome o;
        if (Displacing(why))
        {
            // The generator's Interrupt carried the move bit's clear (the moving mask does not
            // hold it, so this was its only write), and the adapter skipped Interrupt for a
            // behaviour it had already suspended -- whose Suspend() cleared the bit itself, so a
            // bit set since then belongs to the claim that drives now and this finish must leave
            // it alone (the Sight carries that). Its cleanup did nothing else.
            o.interrupt = true;
            if (!sight.suspended)
            {
                o.effects.push_back(Effect::Latch(0, LatchLeg));
            }
            return o;
        }
        // The Finalize (Expired, Died, Cleared): the move bit cleared; a player is left where it
        // stands with its client told to stop (the forced stop), a creature's spline simply
        // abandoned to whatever takes over. The generator cleared before it stopped; the Outcome
        // stops before its effects: benign, StopMoving neither reads nor writes this bit.
        o.effects.push_back(Effect::Latch(0, LatchLeg));
        if (!sight.isCreature)
        {
            o.stopForced = true;
        }
        return o;
    }
}
