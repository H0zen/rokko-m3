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
        // the call order here does not matter. Tasks 3-5 add their Register calls.
        // The coverage family (RegisterCoverageScenarios) is orders 32-35: a vehicle
        // seat, a death and respawn, a possession, two feigns. The simple-move family
        // (RegisterSimpleScenarios) is orders 36-41: the refused arc, the arc under a
        // stun, and the charge (P5-B family 1).
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
    }

    Runner::~Runner()
    {
        for (size_t i = 0; i < m_registry.size(); ++i)
        {
            delete m_registry[i];
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
        if (uint32 n = sWorld.GetActiveSessionCount())
        {
            sLog.outString("MVTEST refused: %u session(s) online; a run steps the world and its seconds, and a client's respawn and aura stamps would straddle the step back (run from the console on an empty realm)", n);
            m_queue.clear();
            return false;
        }
        m_map = sMapMgr.CreateMap(kMapId, NULL);
        if (!m_map)
        {
            sLog.outString("MVTEST refused: map %u could not be created", kMapId);
            m_queue.clear();
            return false;
        }
        sLog.outString("MVTEST map %u bare=%d", kMapId, m_map->IsBare() ? 1 : 0);
        if (!m_map->IsBare())
        {
            // A live GM may still run scenarios on a full map; only the launcher's
            // headless, stepped runs need the map bare to read alike twice (P0-D).
            sLog.outString("MVTEST WARN: map %u carries the world's spawns; two runs will not read alike (the launcher sets Movement.HarnessBareMap = %u)", kMapId, kMapId);
        }
        else
        {
            // Boot force-loads the grids of map 1's always-active creatures
            // (ObjectMgr::LoadActiveEntities); a bare map skips their spawns but
            // still loads their terrain, vmap and mmap tiles, and those grids'
            // unload timers start in real time at boot. A run starting after a
            // real-time delay that differs between two launches would then see a
            // boot-loaded grid near the scenario area unload at a different
            // virtual moment each time, so terrain and vmap queries at its edge
            // would answer differently. A bare map holds no objects yet, so
            // unloading every grid here is safe: from here every grid loads on
            // demand at a deterministic virtual moment (a scenario's `Load` call
            // or an actor's spawn) and its unload timer counts from there. The
            // terrain caches' reclaim passes were phased the same way -- the fused
            // tile cache's sweep and the navmesh purge both fell at boot-phased
            // virtual moments -- so RestartTerrainCleanUp below reclaims every
            // unheld tile now and restarts both, so the passes count from here too.
            // UnloadAll(true) force-deletes a player's own NGridType, so a GM
            // logged in on the bare map keeps its grids instead.
            if (m_map->HavePlayers())
            {
                sLog.outString("MVTEST WARN: map %u has players; grids kept, two runs will not read alike", kMapId);
            }
            else
            {
                m_map->UnloadAll(true);
                m_map->RestartTerrainCleanUp();
                sLog.outString("MVTEST map %u grids reset: every grid loads at a scenario's own moment, terrain reclaim restarted", kMapId);
            }
        }
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
