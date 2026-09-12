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

    namespace
    {
        const uint32 kCadenceMs = 100;
        const uint32 kSettleMs = 1000;
        const uint32 kMapId = 1;   ///< Kalimdor: the old scenarios' Mulgore plains
    }

    Runner::Runner() : m_index(0), m_elapsed(0), m_settle(0), m_sinceTick(0), m_verdicts(0), m_map(NULL)
    {
        // Every family registers its scenarios with their place in the old harness's
        // run order (S1=1, S2=2, S3=3, S5=4, S6=5, S7=6, S8=7, S9=8, S10=9, S11=10,
        // S12=11, S13=12, S15=13, S17=14, S19=15, S4=16); Start("all") sorts by it, so
        // the call order here does not matter. Tasks 3-5 add their Register calls.
        RegisterJumpScenarios(*this);
        RegisterPointScenarios(*this);
        RegisterHomeScenarios(*this);
        RegisterFollowScenarios(*this);
        RegisterFleeScenarios(*this);
        RegisterWanderScenarios(*this);
        RegisterPatrolScenarios(*this);
    }

    Runner::~Runner()
    {
        for (size_t i = 0; i < m_registry.size(); ++i)
        {
            delete m_registry[i];
        }
    }

    bool Runner::Start(std::string const& what)
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
        m_map = sMapMgr.CreateMap(kMapId, NULL);
        if (!m_map)
        {
            sLog.outString("MVTEST refused: map %u could not be created", kMapId);
            m_queue.clear();
            return false;
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
            pathAdded = true;
        }
        sLog.outString("MVTEST start: %u scenario(s) on map %u", uint32(m_queue.size()), kMapId);
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

    void Runner::Begin(Scenario* s)
    {
        m_elapsed = 0;
        m_sinceTick = 0;
        s->Reset();
        sLog.outString("MVTEST %s start", s->Name());
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
        s->Tick(m_elapsed);
        if (!s->Finished() && s->Idle())
        {
            s->Abandon();
        }
        if (s->Finished())
        {
            End(s);
        }
    }
}
