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

#include "Move/Movement.h"

namespace Move
{
    namespace
    {
        /// Which priority each kind holds. One table, read twice, so that a kind's
        /// priority is stated in exactly one place instead of at every call that requests
        /// it. The order of Layer is the whole content of the arbitration.
        struct Rank { Layer layer; Policy policy; };

        const Rank RANK[uint8_t(Kind::Count)] =
        {
            { Layer::Default,  Policy::Supersede },  // Idle
            { Layer::Default,  Policy::Supersede },  // Wander
            { Layer::Default,  Policy::Supersede },  // Patrol
            { Layer::Default,  Policy::Supersede },  // Follow
            { Layer::Combat,   Policy::Supersede },  // Chase
            { Layer::Scripted, Policy::Suspend   },  // Point
            { Layer::Scripted, Policy::Suspend   },  // FlyLand
            { Layer::Scripted, Policy::Override  },  // Home
            { Layer::Scripted, Policy::Suspend   },  // AssistRun
            { Layer::Distract, Policy::Suspend   },  // Distract
            { Layer::Control,  Policy::Suspend   },  // Fear
            { Layer::Control,  Policy::Suspend   },  // Confused
            { Layer::Forced,   Policy::Suspend   },  // Effect
            { Layer::Taxi,     Policy::Override  }   // Taxi
        };
    }

    Layer LayerOf(Kind kind)
    {
        return kind < Kind::Count ? RANK[uint8_t(kind)].layer : Layer::Count;
    }

    Policy PolicyOf(Kind kind)
    {
        return kind < Kind::Count ? RANK[uint8_t(kind)].policy : Policy::Supersede;
    }

    Movement::Movement()
    {
        for (uint8_t i = 0; i < uint8_t(Layer::Count); ++i)
        {
            m_behaviour[i] = 0;
        }
    }

    Movement::~Movement()
    {
        Clear();
    }

    void Movement::Take(Behaviour* behaviour)
    {
        if (!behaviour)
        {
            return;
        }
        const Kind kind = behaviour->What();
        const Layer layer = LayerOf(kind);
        if (layer >= Layer::Count)
        {
            delete behaviour;
            return;
        }
        const Policy policy = PolicyOf(kind);

        // An Override empties the slots beneath, so those behaviours are gone for good and
        // this is where they are destroyed. Selection decides WHICH slots that is; it holds
        // no pointers, so the deletion has to shadow it here rather than inside it.
        if (policy == Policy::Override)
        {
            for (uint8_t i = 0; i < uint8_t(layer); ++i)
            {
                delete m_behaviour[i];
                m_behaviour[i] = 0;
            }
        }

        delete m_behaviour[uint8_t(layer)];
        m_behaviour[uint8_t(layer)] = behaviour;
        m_who.Request(layer, policy, uint8_t(kind));

        // Whatever was walking is no longer the answer, so the route it laid is no longer
        // ours to finish. The map hears about it as a halt in the next Plan.
        m_due = 0;
    }

    void Movement::Drop(Kind kind)
    {
        const Layer layer = LayerOf(kind);
        if (layer >= Layer::Count)
        {
            return;
        }
        if (!m_who.IsHeld(layer) || m_who.At(layer).kind != uint8_t(kind))
        {
            return;
        }
        delete m_behaviour[uint8_t(layer)];
        m_behaviour[uint8_t(layer)] = 0;
        m_who.Finish(layer);
        m_due = 0;
    }

    void Movement::Clear()
    {
        for (uint8_t i = 0; i < uint8_t(Layer::Count); ++i)
        {
            delete m_behaviour[i];
            m_behaviour[i] = 0;
        }
        m_who.Clear();
        m_route.Clear();
        m_routeLayer = uint8_t(Layer::Count);
        m_due = 0;
    }

    bool Movement::Running(Kind& out) const
    {
        Layer at = Layer::Count;
        if (!m_who.Selected(at))
        {
            return false;
        }
        out = Kind(m_who.At(at).kind);
        return true;
    }

    Behaviour* Movement::Held(Kind kind) const
    {
        const Layer layer = LayerOf(kind);
        if (layer >= Layer::Count || !m_who.IsHeld(layer))
        {
            return 0;
        }
        return m_who.At(layer).kind == uint8_t(kind) ? m_behaviour[uint8_t(layer)] : 0;
    }

    Behaviour* Movement::Selected() const
    {
        Layer at = Layer::Count;
        return m_who.Selected(at) ? m_behaviour[uint8_t(at)] : 0;
    }

    uint32_t Movement::Wake(uint32_t nowMs, World& world, Plan& out)
    {
        return Ask(nowMs, false, false, false, world, out);
    }

    uint32_t Movement::Ended(uint32_t nowMs, bool cut, World& world, Plan& out)
    {
        return Ask(nowMs, false, true, cut, world, out);
    }

    uint32_t Movement::Ask(uint32_t nowMs, bool, bool ended, bool cut, World& world, Plan& out)
    {
        Layer at = Layer::Count;
        if (!m_who.Selected(at))
        {
            // Nothing is held. The mover stands, and nothing needs to wake it until
            // something is requested -- which is a call, not a moment in time.
            m_due = 0;
            return 0;
        }

        Behaviour* running = m_behaviour[uint8_t(at)];
        if (!running)
        {
            m_due = 0;
            return 0;
        }

        // A behaviour that did not lay the route in flight is being asked for the first
        // time, or for the first time since it lost and regained the answer. Either way it
        // must not assume the mover is where it left it.
        const bool restart = (m_routeLayer != uint8_t(at));

        // Only the behaviour that laid the route hears that it ended. For anyone else the
        // route belonged to someone who is no longer the answer, and the news is noise.
        const uint32_t next = (ended && !restart)
            ? running->Arrived(nowMs, cut, world, out)
            : running->Decide(nowMs, restart, world, out);

        if (out.send && out.count >= 2)
        {
            m_route.Launch(out.points, out.count,
                           out.speed > 0.0f ? out.speed : world.Pace(out.gait), nowMs);
            m_routeLayer = uint8_t(at);
        }
        else if (out.halt)
        {
            m_route.Clear();
            m_routeLayer = uint8_t(Layer::Count);
        }

        m_due = next;
        return next;
    }
}
