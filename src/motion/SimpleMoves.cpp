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

#include "SimpleMoves.h"

namespace Motion
{
    namespace
    {
        bool Displacing(FinishReason why)
        {
            return why == FinishReason::Superseded || why == FinishReason::Overridden || why == FinishReason::Cancelled;
        }
    }

    // ---- Point --------------------------------------------------------------------

    PointBehaviour::PointBehaviour(Params const& p) : m_p(p) {}

    Step PointBehaviour::Activate(Sight const& sight)
    {
        // The generator's Initialize: under CAN_NOT_REACT | NOT_MOVE it returned at once;
        // otherwise StopMoving, both roaming bits, and the leg laid on the first tick.
        Step s;
        if (!sight.canReact || !sight.canMove)
        {
            return s;
        }
        s.stop = true;
        s.roaming = Roaming::SetBoth;
        s.resetLeg = true;
        m_haveLaid = false;
        return s;
    }

    Step PointBehaviour::Suspend()
    {
        // The generator's Interrupt: cut the spline, clear both bits, forget the leg.
        Step s;
        s.interrupt = true;
        s.roaming = Roaming::ClearBoth;
        s.resetLeg = true;
        m_haveLaid = false;
        return s;
    }

    Step PointBehaviour::Resume(Sight const& sight, bool reset)
    {
        return reset ? Activate(sight) : Step::None();   // the generator's Reset is its Initialize
    }

    Step PointBehaviour::Tick(Sight const& sight, uint32 diff)
    {
        if (!sight.canMove)
        {
            Step s = Step::Of(MoveIntent::Hold());
            s.roaming = Roaming::ClearMove;
            return s;
        }
        const bool tracking = m_p.target != 0;
        if (tracking && !sight.hasTarget)
        {
            m_end = FinishReason::TargetLost;   // the charge's target is gone: no inform, no leg
            return Step::Of(MoveIntent::Done());
        }
        if (sight.status.arrived && tracking && m_haveLaid)
        {
            // The leg ended where the target WAS. A target that walked on past the tolerance
            // gets a fresh leg at once: the re-lay budget paces a live leg, not an ended one.
            const Vector3 drift = sight.targetPoint - m_laid;
            if (drift.squaredLength() > m_p.relayDrift * m_p.relayDrift)
            {
                m_laid = sight.targetPoint;
                m_sinceRelay = 0;
                ++m_relays;
                Step s = Step::Of(MoveIntent::Move(m_laid, m_p.flags).AtSpeed(m_p.speed));
                s.roaming = Roaming::SetBoth;
                return s;
            }
        }
        if (sight.status.arrived || sight.status.blocked)
        {
            m_done = true;
            m_end = sight.status.blocked ? FinishReason::Blocked : FinishReason::Arrived;
            return Step::Of(MoveIntent::Done());
        }
        if (sight.status.cut)
        {
            // Stopped from outside: over, and the script is told anyway (the point is wherever we stand).
            m_done = true;
            m_end = FinishReason::Cut;
            return Step::Of(MoveIntent::Done());
        }

        Vector3 goal = m_p.goal;
        if (tracking)
        {
            // The charge follows its target's contact point, re-laid when it drifted past the
            // tolerance and not more often than the budget (retail re-targets a moving target).
            m_sinceRelay += diff;
            const bool first = !m_haveLaid;
            const Vector3 drift = sight.targetPoint - m_laid;
            const bool drifted = first || drift.squaredLength() > m_p.relayDrift * m_p.relayDrift;
            if (drifted && (first || m_sinceRelay >= m_p.relayEveryMs))
            {
                m_laid = sight.targetPoint;
                m_haveLaid = true;
                m_sinceRelay = 0;
                if (!first)
                {
                    ++m_relays;
                }
            }
            goal = m_laid;
        }

        Step s = Step::Of(MoveIntent::Move(goal, m_p.flags).AtSpeed(m_p.speed));
        s.roaming = Roaming::SetBoth;
        return s;
    }

    Outcome PointBehaviour::Finish(FinishReason why, Sight const& sight)
    {
        Outcome o;
        o.roaming = Roaming::ClearBoth;
        if (Displacing(why))
        {
            o.interrupt = true;   // the shell skips it for a behaviour already suspended
            return o;             // no finalizer for a displaced point: no inform, no assistance
        }
        if (m_p.kind == Motion::Kind::AssistRun)
        {
            // The assistance finisher replaces the point's: no inform, ever.
            o.effects.push_back(Effect(Effect::CallAssistance));
            if (sight.alive)
            {
                o.effects.push_back(Effect(Effect::SeekAssistDistract));
            }
            return o;
        }
        if (m_done && m_p.informs)
        {
            o.effects.push_back(Effect(Effect::Inform, m_p.kind, m_p.id));
            o.effects.push_back(Effect(Effect::SummonedInform, m_p.kind, m_p.id));
        }
        return o;
    }

    // ---- Distract -----------------------------------------------------------------

    Step DistractBehaviour::Tick(Sight const& /*sight*/, uint32 diff)
    {
        if (diff > m_timer)   // strict, as the generator: equality leaves it alive at zero
        {
            return Step::Of(MoveIntent::Done());
        }
        m_timer -= diff;
        return Step::None();   // the generator touched nothing; the facing was the caller's
    }

    Outcome DistractBehaviour::Finish(FinishReason /*why*/, Sight const& /*sight*/)
    {
        Outcome o;
        if (m_kind == Motion::Kind::AssistDistract)
        {
            o.effects.push_back(Effect(Effect::AttackVictim));   // the shell checks victim and alive
        }
        return o;
    }

    // ---- Effect -------------------------------------------------------------------

    Step EffectBehaviour::Activate(Sight const& /*sight*/)
    {
        if (m_launched || m_launch.kind == EffectLaunch::None)
        {
            return Step::None();
        }
        m_launched = true;
        return Step::Of(MoveIntent::Launch(m_launch));   // once
    }

    Step EffectBehaviour::Tick(Sight const& sight, uint32 /*diff*/)
    {
        // `traveling`, not `arrived`: a spline never launched must end at once.
        if (sight.status.traveling)
        {
            return Step::Of(MoveIntent::Hold());
        }
        m_done = true;
        return Step::Of(MoveIntent::Done());
    }

    Outcome EffectBehaviour::Finish(FinishReason why, Sight const& sight)
    {
        Outcome o;
        if (Displacing(why) && !sight.landed)
        {
            return o;   // a spline still flying, or cut short by what replaces us: nothing happened
        }
        if (m_done || sight.landed)
        {
            o.effects.push_back(Effect(Effect::Inform, Motion::Kind::Effect, m_id));
        }
        o.effects.push_back(Effect(Effect::ReengageVictim));   // independent of the inform; the shell reads its predicate live, after the inform
        return o;
    }
}
