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

#include "State.h"

namespace Motion
{
    Kinematics::Kinematics()
        : root(false), canFly(false), waterWalk(false), featherFall(false), hover(false),
          gravityDisabled(false), canTransitionSwimFly(false), walk(false), swim(false), collisionHeight(0.0f)
    {
        for (int i = 0; i < 9; ++i) { speed[i] = 0.0f; }
    }

    bool Kinematics::operator==(Kinematics const& r) const
    {
        for (int i = 0; i < 9; ++i) { if (speed[i] != r.speed[i]) { return false; } }
        return root == r.root && canFly == r.canFly && waterWalk == r.waterWalk && featherFall == r.featherFall
            && hover == r.hover && gravityDisabled == r.gravityDisabled && canTransitionSwimFly == r.canTransitionSwimFly
            && walk == r.walk && swim == r.swim && collisionHeight == r.collisionHeight;
    }

    void Kinematics::Apply(Change const& change)
    {
        if (IsSpeed(change.type)) { speed[SpeedIndex(change.type)] = change.value; return; }
        switch (change.type)
        {
            case ChangeType::Root:                 root = change.apply; break;
            case ChangeType::CanFly:               canFly = change.apply; break;
            case ChangeType::WaterWalk:            waterWalk = change.apply; break;
            case ChangeType::FeatherFall:          featherFall = change.apply; break;
            case ChangeType::Hover:                hover = change.apply; break;
            case ChangeType::GravityDisabled:      gravityDisabled = change.apply; break;
            case ChangeType::CanTransitionSwimFly: canTransitionSwimFly = change.apply; break;
            case ChangeType::Gait:                 walk = change.apply; break;
            case ChangeType::Swim:                 swim = change.apply; break;
            case ChangeType::CollisionHeight:      collisionHeight = change.value; break;
            default: break;   // KnockBack, Teleport: events, not state
        }
    }

    State::State(Mode mode, TimeoutPolicy const& policy, Kinematics const& initial)
        : m_mode(mode), m_desired(initial), m_confirmed(initial), m_pending(policy), m_lastAck(AckResult::NoPending), m_kick(false)
    {
    }

    Emission State::Emit(EmissionKind kind, uint16 opcode, uint32 counter, Change const& change)
    {
        Emission e;
        e.kind = kind;
        e.opcode = opcode;
        e.counter = counter;
        e.change = change;
        ++m_counters.emitted;
        return e;
    }

    void State::SetMode(Mode mode, uint32 now)
    {
        if (mode == m_mode) { return; }
        NewEpoch(now);
        m_mode = mode;
        if (m_mode == Mode::ServerDriven) { m_confirmed = m_desired; }
        ++m_counters.modeChanges;
    }

    std::vector<Emission> State::Apply(Change const& change, uint32 now)
    {
        std::vector<Emission> out;
        MatrixRow const* row = RowFor(change.type, change.apply);
        const bool serverOnly = change.type == ChangeType::Gait || change.type == ChangeType::Swim;
        if (!row || (m_mode == Mode::ClientDriven && serverOnly))
        {
            ++m_counters.refused;
            return out;
        }
        ++m_counters.applied;
        m_desired.Apply(change);
        if (m_mode == Mode::ServerDriven)
        {
            m_confirmed = m_desired;
            if (row->spline) { out.push_back(Emit(EmissionKind::Spline, row->spline, 0, change)); }
            return out;
        }
        if (row->mover)
        {
            const uint32 counter = m_pending.Open(change, now);
            out.push_back(Emit(EmissionKind::Mover, row->mover, counter, change));
        }
        else
        {
            // Forward-compatibility for a future client-driven row with no mover form; no
            // such row exists today (Gait/Swim are server-driven only and refused above).
            m_confirmed.Apply(change);   // nothing to negotiate: confirmed at once
        }
        return out;
    }

    std::vector<Emission> State::Ack(ChangeType type, uint32 counter, AckPayload const& payload, uint32 now)
    {
        std::vector<Emission> out;
        ++m_counters.acked;
        if (m_mode == Mode::ServerDriven)
        {
            m_lastAck = AckResult::NoPending;
            return out;
        }
        AckOutcome const outcome = m_pending.Ack(type, counter, payload, now);
        m_lastAck = outcome.result;
        // A PayloadMismatch (or any other non-Matched result) leaves desired diverged from
        // confirmed with nothing pending; P2-C decides the recovery (resend or resync).
        if (outcome.result != AckResult::Matched) { return out; }
        Change const& change = outcome.change.change;
        m_confirmed.Apply(change);
        ++m_counters.confirmed;
        MatrixRow const* row = RowFor(change.type, change.apply);
        if (row && row->observer) { out.push_back(Emit(EmissionKind::Observer, row->observer, counter, change)); }
        return out;
    }

    std::vector<Emission> State::Tick(uint32 now)
    {
        std::vector<Emission> out;
        std::vector<TimeoutEvent> const events = m_pending.Tick(now);
        for (size_t i = 0; i < events.size(); ++i)
        {
            TimeoutEvent const& e = events[i];
            if (e.action == TimeoutAction::Resend)
            {
                PendingChange const* entry = m_pending.Get(e.type);
                if (!entry) { continue; }
                MatrixRow const* row = RowFor(entry->change.type, entry->change.apply);
                if (row && row->mover) { out.push_back(Emit(EmissionKind::Mover, row->mover, e.newCounter, entry->change)); }
            }
            else if (e.action == TimeoutAction::Kick)
            {
                m_kick = true;
                ++m_counters.kicks;
            }
        }
        return out;
    }

    void State::NewEpoch(uint32 now)
    {
        m_pending.NewEpoch(now);
        m_kick = false;
        ++m_counters.epochs;
    }
}
