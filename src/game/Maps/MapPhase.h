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

#ifndef MANGOS_MAPPHASE_H
#define MANGOS_MAPPHASE_H

#include "Platform/Define.h"

class Map;

/**
 * Who may touch a unit's movement kernel right now (design v2 §10.4, F21): the
 * map phase flag is the world's (Begin/End around sMapMgr.Update), but which
 * map currently owns the kernel is set by Map::Update itself, through Scope --
 * so a vessel's deck, whose TransportMap::Update runs Map::Update nested
 * inside the world map's own tick on that same thread, is right by
 * construction: the nested Scope hands the deck the ownership and, on return,
 * hands the world map back. Outside any map's Update (the session phase, the
 * console) the world thread owns everything, because the phases never
 * overlap. The guard witnesses that barrier: a violation is counted
 * process-wide (and asserted under MANGOS_DEBUG), never silently allowed.
 */
namespace MapPhase
{
    void Begin();                  ///< World::Update: the maps are about to run
    void End();                    ///< every map is done (after MapUpdater::wait)

    /// RAII: `map` owns the kernel for the scope's lifetime on this thread. The
    /// constructor saves whatever map (if any) the thread already owned and
    /// installs `map`; the destructor restores what it saved -- so a transport
    /// deck's nested Map::Update sets itself as owner and hands the thread back
    /// to the world map it interrupted when its own Update returns.
    class Scope
    {
    public:
        explicit Scope(Map const* map);
        ~Scope();

    private:
        Map const* m_previous;

        Scope(Scope const&) = delete;
        Scope& operator=(Scope const&) = delete;
    };

    bool Active();
    bool Owns(Map const* map);     ///< !Active(), or this thread is updating `map`
    uint32 Violations();           ///< process-wide, since start
    void Violation(char const* unit);   ///< counts; the first ten are logged
}

#endif
