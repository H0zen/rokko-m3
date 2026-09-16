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

#include "Mobility.h"

#include <algorithm>

namespace Motion
{
    uint8 ReasonBit(Inhibition what)
    {
        return what == Inhibition::Count ? 0 : uint8(1u << static_cast<unsigned>(what));
    }

    char const* InhibitionName(Inhibition what)
    {
        static char const* const names[] = { "Rooted", "Stunned", "Dead", "Possessed" };
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Inhibition::Count), "InhibitionName out of sync with Inhibition");
        const size_t index = static_cast<size_t>(what);
        return index < sizeof(names) / sizeof(names[0]) ? names[index] : "none";
    }

    MobilityDecision Decide(Selected selected, uint8 reasons)
    {
        MobilityDecision d;
        d.ticks = true;
        d.mayMove = true;
        d.mayTurn = true;
        d.dominant = Inhibition::Count;
        d.reasons = reasons;

        // Death ends everything (a real death has also finished every entry; a feign only pauses).
        if (reasons & ReasonDead)
        {
            d.ticks = false;
            d.mayMove = false;
            d.mayTurn = false;
            d.dominant = Inhibition::Dead;
            return d;
        }

        // A flight goes on under a root, a stun or a possession: nothing lands on a passenger
        // (reference 8.4) and a scripted flight on a stunned unit keeps flying (15.6.2).
        if (selected == Selected::Taxi)
        {
            return d;
        }

        // A stun stops movement and turning for every other class; a distract's clock still runs
        // (its tick moves nothing) so its 10 s run out meanwhile (reference 15.7).
        if (reasons & ReasonStunned)
        {
            d.ticks = selected == Selected::Distract;
            d.mayMove = false;
            d.mayTurn = false;
            d.dominant = Inhibition::Stunned;
            return d;
        }

        // The possessor's client or the pet AI moves the body: the server's own behaviours pause,
        // but a fear or confuse on the body plays and the possessor is the one locked out (15.4.1).
        if ((reasons & ReasonPossessed) && selected != Selected::Control)
        {
            d.ticks = selected == Selected::Distract;
            d.mayMove = false;
            d.mayTurn = false;
            d.dominant = Inhibition::Possessed;
            return d;
        }

        // A root stops translation and keeps turning, the state and the claims (reference 2.2, 2.5).
        if (reasons & ReasonRooted)
        {
            d.ticks = selected == Selected::Distract;
            d.mayMove = false;
            d.mayTurn = true;
            d.dominant = Inhibition::Rooted;
            return d;
        }

        return d;
    }

    bool Mobility::Inhibit(Inhibition what, uint64 source)
    {
        std::vector<uint64>& sources = m_sources[static_cast<size_t>(what)];
        if (std::find(sources.begin(), sources.end(), source) != sources.end())
        {
            return false;
        }
        sources.push_back(source);
        return sources.size() == 1;
    }

    bool Mobility::Uninhibit(Inhibition what, uint64 source)
    {
        std::vector<uint64>& sources = m_sources[static_cast<size_t>(what)];
        std::vector<uint64>::iterator it = std::find(sources.begin(), sources.end(), source);
        if (it == sources.end())
        {
            return false;
        }
        sources.erase(it);
        return sources.empty();
    }

    void Mobility::DropDomain(SourceDomain domain)
    {
        for (size_t i = 0; i < static_cast<size_t>(Inhibition::Count); ++i)
        {
            std::vector<uint64>& sources = m_sources[i];
            sources.erase(std::remove_if(sources.begin(), sources.end(),
                [domain](uint64 source) { return static_cast<SourceDomain>(source >> 60) == domain; }),
                sources.end());
        }
    }

    bool Mobility::Inhibited(Inhibition what) const
    {
        return !m_sources[static_cast<size_t>(what)].empty();
    }

    std::vector<uint64> const& Mobility::Sources(Inhibition what) const
    {
        return m_sources[static_cast<size_t>(what)];
    }

    uint8 Mobility::Reasons() const
    {
        uint8 bits = 0;
        for (size_t i = 0; i < static_cast<size_t>(Inhibition::Count); ++i)
        {
            if (!m_sources[i].empty())
            {
                bits |= ReasonBit(static_cast<Inhibition>(i));
            }
        }
        return bits;
    }
}
