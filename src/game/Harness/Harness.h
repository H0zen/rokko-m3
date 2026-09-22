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
#include "Timeline.h"

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
    /// MoveWaypoint calls. A non-zero path id makes the patrol report through
    /// WaypointPathInform(pathId, …) instead of MovementInform, but
    /// patrol-square and stun-mid-patrol read Node() for their verdicts, not the
    /// inform type, so this is unobserved.
    static const int32 kExternalPath = 250;

    /// The world patroller's (Mouse, entry 6271, guid 261361) four nodes, mirrored as
    /// an external path for S8's own spawn (Movement.HarnessBareMap leaves the map
    /// with no world creature to find). Same id scheme as kExternalPath, one slot up.
    static const int32 kMousePath = 251;

    /// The chicken's square again, but an ENTRY path (WaypointManager::AddEntryNode, no
    /// database row): the welding only runs for an internal-origin patrol (an external
    /// path's script may replace it under us at any node, so PatrolBehaviour never welds
    /// one), so patrol-welded loads this one instead of kExternalPath.
    static const int32 kWeldPath = 252;

    /// The chicken's square as an external path with two of its four nodes' orientation set:
    /// node 2 has a 3 s delay and faces 1.5 rad (a waiting node's orientation is honoured);
    /// node 3 has no delay and faces 4.7 rad (a pass-through node's orientation is ignored,
    /// so the chicken keeps its travel facing there instead). patrol-orients-at-a-waiting-node.
    static const int32 kFacePath = 253;

    /// The chicken's plain square again, external, for patrol-hook-sets-next-node's
    /// MOVE_START hook (SetNextWaypoint from inside the inform).
    static const int32 kHookPath = 254;

    /// patrol-zero-length-legs' two degenerate paths, both of them shapes the world
    /// database really holds. They are registered against entry 6271 rather than the
    /// chicken, whose slots below 0xFF are full: AddExternalNode keys by
    /// (entry << 8) + pathId, so the mouse's 252 and 253 are free.
    ///
    /// ONE node, at the point the walker is standing on: creature_movement id 127332
    /// (entry 3296, an Orgrimmar Grunt) is exactly this, a single row, and it emitted
    /// 2 427 zero-length SMSG_MONSTER_MOVEs in one unbroken run in the user's capture.
    static const int32 kStandstillPath = 252;

    /// Four nodes of which the last two are the SAME point -- creature_movement ids
    /// 318624 (entry 51346) and 236808 (entry 42548), whose points 2 and 3 coincide, and
    /// kMousePath's own 3 and 4. The coincident node waits 3 s here so that the leg laid
    /// for it stays the newest spline across a whole sampling window; the world rows wait
    /// 0, which changes when the next leg replaces it, not whether the leg is laid.
    static const int32 kCoincidentPath = 253;

    /// A scenario still running after this much virtual time is abandoned, so
    /// MVTEST DONE always comes (long-follow needs about four).
    static const uint32 kScenarioMaxMs = 300000;

    /// The harness's own player guids. NOT GeneratePlayerLowGuid(): that advances a real counter,
    /// so two runs would differ and the record would stop being byte-identical.
    /// They live here rather than in Harness.cpp's anonymous namespace because the runner owns
    /// the refusal that reserves the block and Scenario::SpawnPlayer hands the guids out.
    static const uint32 kHarnessPlayerGuidFirst = 0x00F00000;
    static const uint32 kHarnessPlayerGuidCount = 8;
    /// One account id for every harness session. It is never written anywhere.
    static const uint32 kHarnessAccountId = 0x00F00000;

    /**
     * The GM harness runner (design v2 §12): the registry of scenarios in the old
     * harness's order, the map they run on (Kalimdor, Mulgore), the clock, and the
     * MVTEST log. Ticked from World::Update after the maps, outside the map phase,
     * where console commands run; one scenario at a time; every actor a scenario
     * spawned is despawned when it ends.
     *
     * A run steps the world (movement P0-D): from `Start` to `MVTEST DONE` the world
     * loop advances the clock in fixed 50 ms ticks without sleeping, every map updates
     * on the world thread, and each scenario starts from `SeedFor(seedBase, order)` and
     * reseeds again right before the harness map's own update (`SeedMapUpdate`) and its
     * own step, and, on a bare map, from no loaded grids; a run refuses to start while
     * any session is online -- the mode exists for the headless launcher.
     *
     * A run started from a chat command is processed in UpdateSessions, before the maps'
     * update of its tick, and one from the console after it, so a GM-started run pins a
     * different tick phase and is not bit-comparable with a launcher run.
     */
    class Runner
    {
    public:
        Runner();
        ~Runner();

        /// `all`, or one scenario's name; `seedBase` is the console's optional second
        /// argument. False when unknown or a run is in progress.
        bool Start(std::string const& what, uint32 seedBase = kSeedBase);
        std::string Status() const;
        void Update(uint32 diff);
        void SeedMapUpdate();   ///< right before the harness map's update, while a scenario is running: the world thread's generator takes TickSeed(seedBase, order, elapsed)
        Map* GetMap() const { return m_map; }
        void Register(Scenario* scenario) { m_registry.push_back(scenario); }
        bool Running() const { return m_index < m_queue.size(); }

    private:
        void Begin(Scenario* s);
        void End(Scenario* s);
        /// The harness map's grids put back to a known state, and the ONE place that decision
        /// is written: Start makes it before the first scenario and End again behind a player
        /// scenario, and the two have to agree to the word -- the same three branches and the
        /// same three log lines -- or a reader comparing one run's log with another is
        /// comparing two different rules. It was two copies once; this is what that cost.
        void ResetGrids();

        std::vector<Scenario*> m_registry;
        std::vector<Scenario*> m_queue;
        size_t                 m_index;
        uint32                 m_elapsed;       ///< ms since the current scenario started
        uint32                 m_settle;        ///< ms of pause left before the next
        uint32                 m_sinceTick;
        uint32                 m_verdicts;
        uint32                 m_seedBase;      ///< the run's seed base (Start's second argument): each scenario seeds from SeedFor(m_seedBase, order)
        Map*                   m_map;
    };
}

#define sHarness MaNGOS::Singleton<Harness::Runner>::Instance()

#endif
