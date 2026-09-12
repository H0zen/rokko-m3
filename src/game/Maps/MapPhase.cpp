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

#include "MapPhase.h"
#include "Log.h"

#include <atomic>

namespace
{
    std::atomic<bool>   s_active(false);
    std::atomic<uint32> s_violations(0);
    thread_local Map const* t_current = NULL;
}

namespace MapPhase
{
    void Begin() { s_active.store(true); }
    void End() { s_active.store(false); }
    bool Active() { return s_active.load(); }
    bool Owns(Map const* map) { return !s_active.load() || t_current == map; }
    uint32 Violations() { return s_violations.load(); }

    Scope::Scope(Map const* map) : m_previous(t_current)
    {
        t_current = map;
    }

    Scope::~Scope()
    {
        t_current = m_previous;
    }

    void Violation(char const* unit)
    {
        const uint32 n = ++s_violations;
        if (n <= 10)
        {
            sLog.outError("MapPhase: the movement kernel of %s touched from outside its map's update (violation %u)", unit, n);
        }
    }
}
