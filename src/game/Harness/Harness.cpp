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

#include "Harness.h"
#include "Scenario.h"
#include "Ownership.h"
#include "HarnessAI.h"
#include "MapManager.h"
#include "Map.h"
#include "Creature.h"
#include "TemporarySummon.h"
#include "WaypointManager.h"
#include "Log.h"
#include "WorldClock.h"
#include "RNGen.h"
#include "World.h"
#include "Player.h"
#include "PlayerRegistry.h"
#include "WorldSession.h"
#include "GameTime.h"
#include "Database/DatabaseEnv.h"

#include <algorithm>
#include <cstdio>

namespace Harness
{
    void RegisterJumpScenarios(Runner& r);
    void RegisterPointScenarios(Runner& r);
    void RegisterHomeScenarios(Runner& r);
    void RegisterFollowScenarios(Runner& r);
    void RegisterPatrolScenarios(Runner& r);
    void RegisterFleeScenarios(Runner& r);
    void RegisterWanderScenarios(Runner& r);
    void RegisterBlockScenarios(Runner& r);
    void RegisterCoverageScenarios(Runner& r);
    void RegisterSimpleScenarios(Runner& r);
    void RegisterDefaultScenarios(Runner& r);
    void RegisterTrackingScenarios(Runner& r);
    void RegisterControlScenarios(Runner& r);

    namespace
    {
        const uint32 kCadenceMs = 100;
        const uint32 kSettleMs = 1000;
        const uint32 kMapId = 1;   ///< Kalimdor: the old scenarios' Mulgore plains
    }

    Runner::Runner() : m_index(0), m_elapsed(0), m_settle(0), m_sinceTick(0), m_verdicts(0), m_seedBase(kSeedBase), m_map(NULL)
    {
        // Every family registers its scenarios with their place in the old harness's
        // run order (S1=1, S2=2, S3=3, S5=4, S6=5, S7=6, S8=7, S9=8, S10=9, S11=10,
        // S12=11, S13=12, S15=13, S17=14, S19=15, S4=16); Start("all") sorts by it, so
        // the call order here does not matter.
        // The coverage family (RegisterCoverageScenarios) is orders 32-35: a vehicle
        // seat, a death and respawn, a possession, two feigns. The simple-move family
        // (RegisterSimpleScenarios) is orders 36-41: the refused arc, the arc under a
        // stun, and the charge (P5-B family 1). The default-moves family
        // (RegisterDefaultScenarios) is orders 45-47: the welded internal patrol, a
        // waiting node's facing versus a pass-through node's travel facing, and a
        // MOVE_START hook that redirects the next node (P5-B family 2 Task 5). The
        // tracking family (RegisterTrackingScenarios) is orders 48-53: the chase's re-lay
        // budget and where it stops, the follow's pace, band and facings, and the evade
        // that waits under a root (P5-B family 3 Task 5). The control family
        // (RegisterControlScenarios) is orders 54-58: the flee's first bolt and its rest,
        // the stagger's envelope and gait, a corpse as a fright, a refreshed aura restarting
        // the flee, and the stagger in the air (P5-B family 4 Task 5).
        RegisterJumpScenarios(*this);
        RegisterPointScenarios(*this);
        RegisterHomeScenarios(*this);
        RegisterFollowScenarios(*this);
        RegisterFleeScenarios(*this);
        RegisterWanderScenarios(*this);
        RegisterBlockScenarios(*this);
        RegisterCoverageScenarios(*this);
        RegisterSimpleScenarios(*this);
        RegisterPatrolScenarios(*this);
        RegisterDefaultScenarios(*this);
        RegisterTrackingScenarios(*this);
        RegisterControlScenarios(*this);
    }

    /// The registry, and DELIBERATELY NOTHING ELSE -- not `End`'s five-step player teardown,
    /// not the settle, not `ResetGrids`.
    ///
    /// This runs too late to do any of it. `sHarness` is a function-local static
    /// (MaNGOS::Singleton), so this destructor runs at static destruction, after `main`
    /// returns -- and `Master::ShutdownWorld()` has by then already run `sMapMgr.UnloadAll()`
    /// (Master.cpp:391), which unloads every map and then DELETES it
    /// (MapManager.cpp:453-462), before `Master::Run` ever returns to `main`. So `m_map` is a
    /// freed pointer here, every actor that stood on it is gone with it, and the whole
    /// teardown -- `GetCreature`, `Map::Remove`, `AddToActive` -- would be a use-after-free
    /// on a map that no longer exists. Adding it here would not close a leak; it would turn
    /// a harmless one into a crash on the way out.
    ///
    /// AND THERE IS NOTHING LEFT TO CLOSE. A process killed mid-scenario leaks the scenario's
    /// Player, its WorldSession and (907) its Pet: the grid unloader only visits
    /// GridTypeMapContainer -- GameObject, Creature-except-pets, DynamicObject, Corpse
    /// (GridDefines.h:72) -- so a Player or a Pet, which live in the WorldTypeMapContainer
    /// beside it, is unlinked with its grid rather than deleted. Every one of those
    /// allocations dies with the address space a moment later, and the three things the
    /// teardown's order exists to protect -- the next scenario's reuse of the reserved guid,
    /// the mover-authority totals, and ~Unit's "still had a mover session" -- all belong to a
    /// process that goes on running. None of them has a next tick here.
    ///
    /// Whoever wants a mid-run kill to tear down cleanly must hook it BEFORE the maps go, in
    /// `Master::ShutdownWorld` ahead of the UnloadAll, and not in this destructor.
    Runner::~Runner()
    {
        for (size_t i = 0; i < m_registry.size(); ++i)
        {
            delete m_registry[i];
        }
    }

    void Runner::ResetGrids()
    {
        // Boot force-loads the grids of map 1's always-active creatures
        // (ObjectMgr::LoadActiveEntities); a bare map skips their spawns but still loads
        // their terrain, vmap and mmap tiles, and those grids' unload timers start in real
        // time at boot. A run starting after a real-time delay that differs between two
        // launches would then see a boot-loaded grid near the scenario area unload at a
        // different virtual moment each time, so terrain and vmap queries at its edge would
        // answer differently. A bare map holds no objects yet, so unloading every grid is
        // safe: from here every grid loads on demand at a deterministic virtual moment (a
        // scenario's `Load` call or an actor's spawn) and its unload timer counts from there.
        // The terrain caches' reclaim passes were phased the same way -- the fused tile
        // cache's sweep and the navmesh purge both fell at boot-phased virtual moments -- so
        // RestartTerrainCleanUp reclaims every unheld tile now and restarts both, so the
        // passes count from here too.
        if (!m_map->IsBare())
        {
            // A live GM may still run scenarios on a full map; only the launcher's
            // headless, stepped runs need the map bare to read alike twice (P0-D).
            sLog.outString("MVTEST WARN: map %u carries the world's spawns; two runs will not read alike (the launcher sets Movement.HarnessBareMap = %u)", kMapId, kMapId);
        }
        else if (m_map->HavePlayers())
        {
            // UnloadAll(true) force-deletes a player's own NGridType, so a GM logged in on
            // the bare map keeps its grids instead.
            sLog.outString("MVTEST WARN: map %u has players; grids kept, two runs will not read alike", kMapId);
        }
        else
        {
            m_map->UnloadAll(true);
            m_map->RestartTerrainCleanUp();
            sLog.outString("MVTEST map %u grids reset: every grid loads at a scenario's own moment, terrain reclaim restarted", kMapId);
        }
    }

    bool Runner::Start(std::string const& what, uint32 seedBase)
    {
        if (Running() || m_settle)
        {
            sLog.outString("MVTEST refused: a run is in progress (%s)", Status().c_str());
            return false;
        }
        m_queue.clear();
        m_index = 0;
        m_verdicts = 0;
        if (what == "all")
        {
            m_queue = m_registry;
            std::sort(m_queue.begin(), m_queue.end(), [](Scenario const* a, Scenario const* b) { return a->Order() < b->Order(); });
            if (m_queue.empty())
            {
                sLog.outString("MVTEST refused: no scenarios registered");
                return false;
            }
        }
        else
        {
            for (size_t i = 0; i < m_registry.size(); ++i)
            {
                if (what == m_registry[i]->Name())
                {
                    m_queue.push_back(m_registry[i]);
                }
            }
            if (m_queue.empty())
            {
                sLog.outString("MVTEST refused: no scenario named %s", what.c_str());
                return false;
            }
        }
        // A player promotes the grids around it to full state and changes Map::Update's
        // own visitation order for as long as he is in world (F4). End resets the map
        // behind a player scenario once it ends (below), but that reset cannot help a
        // scenario the queue already ran before it -- by then the promoted grids and
        // their expiry phases were already read. The queue is the one place that can see
        // every scenario's order at once, so it is the one place that can promise this
        // rather than leave it for a reader to remember when registering a new family.
        Scenario* lastPlayerScenario = NULL;
        for (size_t i = 0; i < m_queue.size(); ++i)
        {
            if (m_queue[i]->UsesPlayer())
            {
                lastPlayerScenario = m_queue[i];
            }
            else if (lastPlayerScenario)
            {
                sLog.outString("MVTEST refused: %s (a player scenario) is queued before %s; a player scenario must run after every scenario that does not, so the grids it promotes are never read by one that follows it",
                               lastPlayerScenario->Name(), m_queue[i]->Name());
                m_queue.clear();
                return false;
            }
        }
        if (uint32 n = sWorld.GetActiveSessionCount())
        {
            sLog.outString("MVTEST refused: %u session(s) online; a run steps the world and its seconds, and a client's respawn and aura stamps would straddle the step back (run from the console on an empty realm)", n);
            m_queue.clear();
            return false;
        }
        // A scenario's player takes a guid straight out of the harness's reserved block, and a
        // guid that a real character already owns would have the run write over him the first
        // time anything saved. The registry cannot answer this: an offline character is invisible
        // to it, so the characters table is the only witness.
        // Asked only of a queue that actually holds a player scenario, which `lastPlayerScenario`
        // above is non-NULL for exactly when it does. The 62-plus scenarios that hold no player
        // draw nothing from the block, and giving every one of them a new refusal path for a
        // character-database outage would be a failure mode bought for nothing. Nor can a
        // scenario slip a player past the gate: SpawnPlayer refuses outright unless the scenario
        // declares UsesPlayer() (Scenario.cpp), which is the same flag this reads.
        if (lastPlayerScenario)
        {
            QueryResult* taken = CharacterDatabase.PQuery(
                "SELECT COUNT(*) FROM `characters` WHERE `guid` BETWEEN %u AND %u",
                kHarnessPlayerGuidFirst, kHarnessPlayerGuidFirst + kHarnessPlayerGuidCount - 1);
            // A guard that fails open is not a guard: a lost connection or a missing table returns
            // NULL, and reading that as "the block is free" is exactly the case where the answer is
            // unknown and a real character may be standing in it. Refuse instead.
            if (!taken)
            {
                sLog.outString("MVTEST refused: the harness guid block %u..%u could not be checked against `characters` (no result: connection or schema)",
                               kHarnessPlayerGuidFirst, kHarnessPlayerGuidFirst + kHarnessPlayerGuidCount - 1);
                m_queue.clear();
                return false;
            }
            const uint32 rows = taken->Fetch()[0].GetUInt32();
            delete taken;
            if (rows)
            {
                sLog.outString("MVTEST refused: %u character(s) occupy the harness guid block %u..%u",
                               rows, kHarnessPlayerGuidFirst, kHarnessPlayerGuidFirst + kHarnessPlayerGuidCount - 1);
                m_queue.clear();
                return false;
            }
        }
        m_map = sMapMgr.CreateMap(kMapId, NULL);
        if (!m_map)
        {
            sLog.outString("MVTEST refused: map %u could not be created", kMapId);
            m_queue.clear();
            return false;
        }
        sLog.outString("MVTEST map %u bare=%d", kMapId, m_map->IsBare() ? 1 : 0);
        ResetGrids();
        // The chicken's square (S7, S19), the old runner's template rows, as an
        // external path under the harness's own path id: id 0 is the one a script
        // would use for entry 621's external path, and AddExternalNode keys by
        // (entry << 8) + pathId with no collision check.
        static bool pathAdded = false;
        if (!pathAdded)
        {
            if (!sWaypointMgr.AddExternalNode(621, kExternalPath, 1, -3122.6f, -261.3f, 46.0f, 100.0f, 0))
            {
                sLog.outString("MVTEST %s", "ERR external node 1 not added");
            }
            if (!sWaypointMgr.AddExternalNode(621, kExternalPath, 2, -3152.6f, -261.3f, 46.0f, 100.0f, 0))
            {
                sLog.outString("MVTEST %s", "ERR external node 2 not added");
            }
            if (!sWaypointMgr.AddExternalNode(621, kExternalPath, 3, -3152.6f, -231.3f, 46.0f, 100.0f, 0))
            {
                sLog.outString("MVTEST %s", "ERR external node 3 not added");
            }
            if (!sWaypointMgr.AddExternalNode(621, kExternalPath, 4, -3122.6f, -231.3f, 46.0f, 100.0f, 0))
            {
                sLog.outString("MVTEST %s", "ERR external node 4 not added");
            }
            // Mouse's own four nodes (creature_movement guid 261361, entry 6271, read
            // 2026-09-15), mirrored as an external path so patrol-lifted can spawn its
            // own patroller on them: the harness map is bare (P0-D), so the world's
            // Mouse is not there to Find.
            // Nodes 3 and 4 coincide, as in the world's rows (creature_movement id
            // 261361, points 2 and 3 share -2995.64 -338.986 53.5518): the path mirrors
            // Mouse's exactly.
            if (!sWaypointMgr.AddExternalNode(6271, kMousePath, 1, -2986.64f, -329.723f, 54.0748f, 0.0f, 0)) { sLog.outString("MVTEST %s", "ERR mouse node 1 not added"); }
            if (!sWaypointMgr.AddExternalNode(6271, kMousePath, 2, -2985.8f, -329.178f, 54.0748f, 0.0f, 0)) { sLog.outString("MVTEST %s", "ERR mouse node 2 not added"); }
            if (!sWaypointMgr.AddExternalNode(6271, kMousePath, 3, -2995.64f, -338.986f, 53.5518f, 0.0f, 0)) { sLog.outString("MVTEST %s", "ERR mouse node 3 not added"); }
            if (!sWaypointMgr.AddExternalNode(6271, kMousePath, 4, -2995.64f, -338.986f, 53.5518f, 0.0f, 0)) { sLog.outString("MVTEST %s", "ERR mouse node 4 not added"); }
            // The chicken's square again, but an entry-origin path (P5-B family 2 Task 5,
            // patrol-welded): PatrolBehaviour only welds an internal-origin leg, never an
            // external one, so this scenario needs its own in-memory path.
            if (!sWaypointMgr.AddEntryNode(621, kWeldPath, 1, -3122.6f, -261.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR weld node 1 not added"); }
            if (!sWaypointMgr.AddEntryNode(621, kWeldPath, 2, -3152.6f, -261.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR weld node 2 not added"); }
            if (!sWaypointMgr.AddEntryNode(621, kWeldPath, 3, -3152.6f, -231.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR weld node 3 not added"); }
            if (!sWaypointMgr.AddEntryNode(621, kWeldPath, 4, -3122.6f, -231.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR weld node 4 not added"); }
            // The chicken's square, external, with node 2 waiting (faces 1.5 rad) and node 3
            // passed through (faces 4.7 rad, ignored: patrol-orients-at-a-waiting-node).
            if (!sWaypointMgr.AddExternalNode(621, kFacePath, 1, -3122.6f, -261.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR face node 1 not added"); }
            if (!sWaypointMgr.AddExternalNode(621, kFacePath, 2, -3152.6f, -261.3f, 46.0f, 1.5f, 3000)) { sLog.outString("MVTEST %s", "ERR face node 2 not added"); }
            if (!sWaypointMgr.AddExternalNode(621, kFacePath, 3, -3152.6f, -231.3f, 46.0f, 4.7f, 0)) { sLog.outString("MVTEST %s", "ERR face node 3 not added"); }
            if (!sWaypointMgr.AddExternalNode(621, kFacePath, 4, -3122.6f, -231.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR face node 4 not added"); }
            // The chicken's plain square again, external (patrol-hook-sets-next-node).
            if (!sWaypointMgr.AddExternalNode(621, kHookPath, 1, -3122.6f, -261.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR hook node 1 not added"); }
            if (!sWaypointMgr.AddExternalNode(621, kHookPath, 2, -3152.6f, -261.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR hook node 2 not added"); }
            if (!sWaypointMgr.AddExternalNode(621, kHookPath, 3, -3152.6f, -231.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR hook node 3 not added"); }
            if (!sWaypointMgr.AddExternalNode(621, kHookPath, 4, -3122.6f, -231.3f, 46.0f, 100.0f, 0)) { sLog.outString("MVTEST %s", "ERR hook node 4 not added"); }
            pathAdded = true;
        }
        sLog.outString("MVTEST start: %u scenario(s) on map %u", uint32(m_queue.size()), kMapId);
        m_seedBase = seedBase;
        WorldClock::EnterStepped();
        sMapMgr.ResetUpdateTimer();   // the next map update lands exactly two ticks after the start, every run
        sMapMgr.SetBeforeMapUpdateHook([this](Map& map) { if (&map == m_map) { SeedMapUpdate(); } });   // the harness map's own update draws from the scenario's seed; the other maps' creatures ahead of it in the pass no longer shift its stream
        sLog.outString("MVTEST stepped: seed base %u, map phase pinned", m_seedBase);
        Begin(m_queue[0]);
        return true;
    }

    std::string Runner::Status() const
    {
        if (m_settle)
        {
            return "settling";
        }
        if (!Running())
        {
            return "idle";
        }
        char text[128];
        snprintf(text, sizeof(text), "%s (%u/%u) at +%u ms", m_queue[m_index]->Name(), uint32(m_index + 1), uint32(m_queue.size()), m_elapsed);
        return text;
    }

    void Runner::SeedMapUpdate()
    {
        // During the settle the actors are being despawned and Begin reseeds, so the map
        // update draws from whatever the generator holds; only a running scenario's draws are pinned.
        if (!Running() || m_settle)
        {
            return;
        }
        Scenario* s = m_queue[m_index];
        RNG::Seed(TickSeed(m_seedBase, s->Order(), m_elapsed));
    }

    void Runner::Begin(Scenario* s)
    {
        m_elapsed = 0;
        m_sinceTick = 0;
        s->Reset();
        RNG::Seed(SeedFor(m_seedBase, s->Order()));
        sLog.outString("MVTEST %s start seed=%u", s->Name(), SeedFor(m_seedBase, s->Order()));
        s->Prepare();
    }

    void Runner::End(Scenario* s)
    {
        std::vector<ObjectGuid> const& spawned = s->Spawned();
        for (size_t i = 0; i < spawned.size(); ++i)
        {
            if (Creature* c = m_map->GetCreature(spawned[i]))
            {
                if (TemporarySummon* t = dynamic_cast<TemporarySummon*>(c))
                {
                    t->UnSummon();
                }
            }
        }
        // A Find'd creature (S8's patroller) is the world's own: never despawned,
        // never written back to the database, only handed back whole - its own
        // factory AI restored in place of the recording decorator, and deactivated
        // again if Find is what activated it.
        std::vector<FoundActor> const& found = s->Found();
        for (size_t i = 0; i < found.size(); ++i)
        {
            if (Creature* c = m_map->GetCreature(found[i].guid))
            {
                if (HarnessAI* h = dynamic_cast<HarnessAI*>(c->AI()))
                {
                    c->SetAI(h->Release());
                    delete h;
                }
                if (!found[i].wasActive)
                {
                    c->SetActiveObjectState(false);
                }
                // SetActiveObjectState(false) above drops the creature from
                // m_activeNonPlayers too; a camera (Camera::SetView) can have listed
                // it there directly, with the flag never set, so hand that listing
                // back as well - AddToActive re-takes the grid lock RemoveFromActive
                // just released, keeping the pair balanced.
                if (found[i].wasListed && !found[i].wasActive)
                {
                    m_map->AddToActive(c);
                }
            }
        }
        // The scenario's players go last, after every actor that could still be pointing at one
        // has left, and they are torn down from the scenario's OWN record of each of them --
        // the Player and the WorldSession it allocated under him -- and not from a lookup. The
        // player registry indexes logged-in players; it is not an ownership table, and using it
        // as one loses the session the moment anything ends the player by the normal door:
        // Map::Remove(player, true) ends in Map::DeleteFromWorld, which unregisters and deletes
        // him (Map.cpp:479-483) and never looks at a session allocated separately, so the lookup
        // would miss, and the session -- which nothing else in the server frees -- would dangle
        // over freed memory with the next scenario about to build a second Player on the same
        // reserved guid.
        //
        // THE ORDER BELOW IS EXACT, and every step of it was paid for:
        //
        // 1. RevokeAllMovers while the player is still alive and still his session's _player.
        //    Nothing else does it for him: Unit::RemoveFromWorld skips the revoke for a player
        //    on purpose ("revoked by LogoutPlayer", Unit.cpp:4965-4969) and the harness never
        //    logs anybody out. Left undone, ~Unit reports "still had a mover session" once per
        //    player run, and ~WorldSession folds an added/removed pair that never balanced into
        //    the process-wide authority totals -- poisoning the one signal (added != removed)
        //    the campaign reads to spot a leaked mover. Both neighbouring placements are wrong:
        //    after `delete session` it is a use-after-free, since RevokeAllMovers dereferences
        //    _player (WorldSession.cpp:168-180) and the removal has already deleted him, and it
        //    also resolves the other members through ObjectLookup, which reads the registry;
        //    after SetPlayer(NULL) it is a silent no-op, with `removed` still never counted.
        //    `now` is the same clock every other revoke passes -- LogoutPlayer
        //    (WorldSession.cpp:806) and Unit::RemoveFromWorld both pass
        //    GameTime::GetGameTimeMS() -- and it stays deterministic here because GameMSTime is
        //    refreshed from WorldClock, which only Step() moves while the harness runs.
        // 2. The registry removal, while the guid still resolves to him.
        // 3. Map::Remove(player, true), which deletes the player itself.
        // 4. SetPlayer(NULL): ~WorldSession runs LogoutPlayer(true) when a player is still
        //    attached (WorldSession.cpp:293-298), which would drive the whole logout cascade --
        //    the online flag, the group and guild broadcasts -- against a character that never
        //    existed, through a pointer that by now is freed.
        // 5. delete session.
        //
        // Steps 4 and 5 are NOT unconditional. They are right for the player this teardown
        // ended itself, and right for one it finds already destroyed, and WRONG for one that
        // may still be alive -- each branch below says which it is and why.
        std::vector<OwnedPlayer> const& players = s->SpawnedPlayers();
        for (size_t i = 0; i < players.size(); ++i)
        {
            OwnedPlayer const& owned = players[i];
            // Asked before anything follows the pointer, because by here it may be freed
            // memory: the registry is the witness that the object is still alive, and identity
            // is what is compared rather than presence. Ownership.h carries the argument for
            // both, and the inWorld = false lookup is part of it.
            const Ownership state = ClassifyOwnership(owned.player, sPlayerRegistry.Find(owned.guid, false));
            if (state == Ownership::Held)
            {
                if (owned.session)
                {
                    owned.session->RevokeAllMovers(GameTime::GetGameTimeMS());
                }
                sPlayerRegistry.Remove(owned.player);
                // FindMap, not GetMap: the classification above does not filter on IsInWorld, so
                // the record can still be Held for a player whose map reference has already been
                // cleared (Map::Remove ends in ResetMap), and GetMap asserts on that. Off every
                // map there is no removal left to make -- delete him here, as
                // Map::DeleteFromWorld would have.
                if (Map* map = owned.player->FindMap())
                {
                    map->Remove(owned.player, true);
                }
                else
                {
                    sLog.outString("MVTEST ERR %s: harness player %s held no map at teardown; deleted without a map removal",
                                   s->Name(), owned.guid.GetString().c_str());
                    delete owned.player;
                }
                // Steps 4 and 5, and only on this branch: the player we owned is gone by our
                // own hand, so nothing can be reaching for the session any more.
                if (owned.session)
                {
                    owned.session->SetPlayer(NULL);
                    delete owned.session;
                }
            }
            else if (state == Ownership::Destroyed)
            {
                // Nothing answers the guid, and in this tree only one thing ever unregisters a
                // player: Map::DeleteFromWorld (Map.cpp:481, the sole caller of
                // PlayerRegistry::Remove outside this teardown), whose next statement deletes
                // him. So the object really is gone, no live player can still be reaching for
                // this session, and closing it is the correct end rather than a leak. SetPlayer
                // is a plain assignment (WorldSession.h:480-483), so it is safe over a _player
                // that is already freed, and it is what keeps ~WorldSession's LogoutPlayer(true)
                // off that freed memory.
                sLog.outString("MVTEST ERR %s: harness player %s was destroyed by something else before the teardown reached him; he is not touched, and the session the scenario allocated is closed here",
                               s->Name(), owned.guid.GetString().c_str());
                if (owned.session)
                {
                    owned.session->SetPlayer(NULL);
                    delete owned.session;
                }
            }
            else
            {
                // Replaced: another object answers the guid. That says our player was replaced
                // IN THE REGISTRY -- it does NOT say he died. He may be alive and in the map
                // this instant, and Map::Update calls plr->GetSession()->Update(...) for every
                // in-world player on it (Map.cpp:913-923), so deleting this session would hand
                // a live player a freed one on the very next tick. A leaked session is bad; a
                // live player holding a freed session is worse, so THE SESSION IS LEFT ALONE,
                // deliberately, and the line below says so in as many words.
                //
                // Not detached either. SetPlayer(NULL) would not crash -- a harness session's
                // Update is a no-op whatever _player holds, since the packet loop and
                // UpdateSecondStream both return on the null socket and MapSessionFilter never
                // runs the logout -- but it would buy nothing and cost something. Nothing will
                // ever delete this session now, so there is no ~WorldSession to protect from
                // the pointer; and the player's own m_session cannot be cleared from here
                // without touching a player we have just decided we may not touch, so detaching
                // would leave a live player whose session denies him, which any code doing
                // GetSession()->GetPlayer() would then read as nobody.
                //
                // The leak is visible twice over: this line, and the mover authority totals,
                // which a session that is never destroyed never folds back, so the campaign's
                // added != removed signal will also be off by this player's grants.
                sLog.outString("MVTEST ERR %s: harness player %s was replaced in the registry by another player object before the teardown reached him; he is not touched and HIS SESSION IS LEAKED ON PURPOSE -- he may still be alive and in the map, and Map::Update would call straight into a freed session. Do not 'fix' this into a delete. The classification is a best effort: it compares the pointer this scenario recorded with what the registry answers now, and an address can be handed out twice",
                               s->Name(), owned.guid.GetString().c_str());
            }
        }
        // The player himself is gone now, above, but a player promotes the grids around
        // him to full state and changes Map::Update's own visitation order for as long as
        // he was in world (F4); the grids he touched and their expiry phases are still
        // whatever he left them at. Whatever runs next must not inherit that, so the same
        // reset Start makes runs again here -- the same call, so the two cannot drift apart.
        // The condition trusts the flag OR the evidence: SpawnPlayer now refuses a scenario
        // that has not declared UsesPlayer(), but a player that got onto the map some other
        // way still gets his grids reset behind him rather than leaving the next scenario to
        // inherit them.
        if (s->UsesPlayer() || !s->SpawnedPlayers().empty())
        {
            ResetGrids();
        }
        ++m_verdicts;
        ++m_index;
        m_settle = kSettleMs;
    }

    void Runner::Update(uint32 diff)
    {
        if (m_settle)
        {
            m_settle = diff >= m_settle ? 0 : m_settle - diff;
            if (m_settle)
            {
                return;
            }
            if (Running())
            {
                Begin(m_queue[m_index]);
            }
            else
            {
                sLog.outString("MVTEST DONE %u scenarios, %u verdict lines", uint32(m_queue.size()), m_verdicts);
                WorldClock::LeaveStepped();
                sMapMgr.SetBeforeMapUpdateHook(MapManager::BeforeMapUpdateHook());
                sLog.outString("MVTEST clock offset %u ms", WorldClock::OffsetMs());
                m_queue.clear();
                m_index = 0;
            }
            return;
        }
        if (!Running())
        {
            return;
        }
        m_elapsed += diff;
        m_sinceTick += diff;
        if (m_sinceTick < kCadenceMs)
        {
            return;
        }
        m_sinceTick = 0;
        Scenario* s = m_queue[m_index];
        // The step below runs after every map's update (World::Update calls it after
        // sMapMgr.Update), so it reseeds for the same reason SeedMapUpdate does.
        RNG::Seed(StepSeed(m_seedBase, s->Order(), m_elapsed));
        s->Tick(m_elapsed);
        if (!s->Finished() && s->Idle())
        {
            s->Abandon();
        }
        if (!s->Finished() && m_elapsed > kScenarioMaxMs)
        {
            sLog.outString("MVTEST %s abandoned after %u ms (no verdict)", s->Name(), m_elapsed);
            char text[64];
            snprintf(text, sizeof(text), "timeout=INVALID(abandoned after %u s)", kScenarioMaxMs / 1000);
            s->Abandon(text);
        }
        if (s->Finished())
        {
            End(s);
        }
    }
}
