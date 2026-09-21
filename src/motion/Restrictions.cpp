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

#include "Restrictions.h"

#include <algorithm>

namespace Motion
{
    uint8 ReasonBit(Inhibition what)
    {
        return what == Inhibition::Count ? 0 : uint8(1u << static_cast<unsigned>(what));
    }

    char const* InhibitionName(Inhibition what)
    {
        static char const* const names[] = { "Rooted", "Stunned", "Dead", "Possessed", "Feared", "Confused" };
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Inhibition::Count), "InhibitionName out of sync with Inhibition");
        const size_t index = static_cast<size_t>(what);
        return index < sizeof(names) / sizeof(names[0]) ? names[index] : "none";
    }

    bool Restrictions::Inhibit(Inhibition what, uint64 source)
    {
        std::vector<uint64>& sources = m_sources[static_cast<size_t>(what)];
        if (std::find(sources.begin(), sources.end(), source) != sources.end())
        {
            return false;
        }
        sources.push_back(source);
        return sources.size() == 1;
    }

    bool Restrictions::Uninhibit(Inhibition what, uint64 source)
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

    void Restrictions::DropDomain(SourceDomain domain)
    {
        for (size_t i = 0; i < static_cast<size_t>(Inhibition::Count); ++i)
        {
            std::vector<uint64>& sources = m_sources[i];
            sources.erase(std::remove_if(sources.begin(), sources.end(),
                [domain](uint64 source) { return static_cast<SourceDomain>(source >> 60) == domain; }),
                sources.end());
        }
    }

    bool Restrictions::Inhibited(Inhibition what) const
    {
        return !m_sources[static_cast<size_t>(what)].empty();
    }

    std::vector<uint64> const& Restrictions::Sources(Inhibition what) const
    {
        return m_sources[static_cast<size_t>(what)];
    }

    uint8 Restrictions::Reasons() const
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
