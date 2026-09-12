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

#ifndef MANGOS_HARNESS_H
#define MANGOS_HARNESS_H

#include "Platform/Define.h"
#include "Policies/Singleton.h"

#include <string>
#include <vector>

class Map;

namespace Harness
{
    class Scenario;

    /// The harness's own external path id for the chicken's template square.
    /// WaypointManager::AddExternalNode keys its external paths by
    /// (entry << 8) + pathId with no collision check, and path 0 is the id a
    /// script would use for entry 621's external path; this id (< 0xFF) is the
    /// harness's alone. Shared by Harness.cpp's runner and ScenariosPatrol.cpp's
    /// MoveWaypoint calls. A non-zero path id shifts the waypoint generator's
    /// MovementInform type to EXTERNAL_WAYPOINT_MOVE + kExternalPath, but
    /// patrol-square and stun-mid-patrol read Node() for their verdicts, not the
    /// inform type, so this is unobserved.
    static const int32 kExternalPath = 250;

    /**
     * The GM harness runner (design v2 §12): the registry of scenarios in the old
     * harness's order, the map they run on (Kalimdor, Mulgore), the clock, and the
     * MVTEST log. Ticked from World::Update after the maps, outside the map phase,
     * where console commands run; one scenario at a time; every actor a scenario
     * spawned is despawned when it ends.
     */
    class Runner
    {
    public:
        Runner();
        ~Runner();

        /// `all`, or one scenario's name. False when unknown or a run is in progress.
        bool Start(std::string const& what);
        std::string Status() const;
        void Update(uint32 diff);
        Map* GetMap() const { return m_map; }
        void Register(Scenario* scenario) { m_registry.push_back(scenario); }
        bool Running() const { return m_index < m_queue.size(); }

    private:
        void Begin(Scenario* s);
        void End(Scenario* s);

        std::vector<Scenario*> m_registry;
        std::vector<Scenario*> m_queue;
        size_t                 m_index;
        uint32                 m_elapsed;       ///< ms since the current scenario started
        uint32                 m_settle;        ///< ms of pause left before the next
        uint32                 m_sinceTick;
        uint32                 m_verdicts;
        Map*                   m_map;
    };
}

#define sHarness MaNGOS::Singleton<Harness::Runner>::Instance()

#endif
