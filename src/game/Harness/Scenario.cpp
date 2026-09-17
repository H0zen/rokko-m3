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

#include "Scenario.h"
#include "Harness.h"
#include "HarnessAI.h"
#include "Creature.h"
#include "TemporarySummon.h"
#include "ObjectMgr.h"
#include "Map.h"
#include "GridMap.h"
#include "Log.h"

#include <cstdarg>
#include <cstdio>

namespace
{
    /// sLog names the global class Log, and Scenario::Log hides that name inside the
    /// class's own scope, so the macro cannot be written in a member of Scenario. This
    /// free function keeps the lookup out of it.
    void Out(std::string const& line)
    {
        sLog.outString("%s", line.c_str());
    }
}

namespace Harness
{
    float Spread(std::vector<Pt> const& samples)
    {
        float spread = 0.0f;
        for (size_t k = 1; k < samples.size(); ++k)
        {
            const float d = Dist2(samples[0].x, samples[0].y, samples[k].x, samples[k].y);
            if (d > spread) { spread = d; }
        }
        return spread;
    }

    void Scenario::Reset()
    {
        m_timeline = Timeline();
        m_finished = false;
        m_spawned.clear();
        m_found.clear();
        m_informs.clear();
    }

    Map* Scenario::GetMap() const
    {
        return sHarness.GetMap();
    }

    void Scenario::Load(float x, float y)
    {
        if (Map* map = GetMap())
        {
            if (!map->IsLoaded(x, y))
            {
                map->ForceLoadGrid(x, y);
            }
        }
    }

    float Scenario::Ground(float x, float y, float z) const
    {
        Map* map = GetMap();
        if (!map)
        {
            return z;
        }
        const float h = map->GetHeight(1, x, y, z);
        return h > INVALID_HEIGHT + 1.0f ? h : z;
    }

    Creature* Scenario::Spawn(uint32 entry, float x, float y, float z, float o)
    {
        Map* map = GetMap();
        CreatureInfo const* cinfo = ObjectMgr::GetCreatureTemplate(entry);
        if (!map || !cinfo)
        {
            Log("ERR spawn %u: no map or template", entry);
            return NULL;
        }
        Load(x, y);
        TemporarySummon* c = new TemporarySummon(ObjectGuid());
        CreatureCreatePos pos(map, x, y, z, o, 1);
        if (!c->Create(map->GenerateLocalLowGuid(cinfo->GetHighGuid()), pos, cinfo))
        {
            delete c;
            Log("ERR spawn %u at %.1f %.1f failed", entry, x, y);
            return NULL;
        }
        c->SetSpawn(pos);
        // No player stands on the harness map, and Map::Update ticks only the cells
        // around players plus the active objects: without this the actor is in the
        // world but never updated, so its movement generators never run at all. The
        // flag is set before the add because Map::Add is the single registration
        // point for a new object; setting it afterwards would register through the
        // setter instead - either works, one is enough.
        c->SetActiveObjectState(true);
        c->Summon(TEMPSPAWN_MANUAL_DESPAWN, 0);   // adds it to the map; despawned by the runner's sweep
        c->SetAI(new HarnessAI(c, c->AI(), this));
        m_spawned.push_back(c->GetObjectGuid());
        return c;
    }

    Creature* Scenario::Find(uint32 lowGuid, uint32 entry)
    {
        Map* map = GetMap();
        if (!map)
        {
            return NULL;
        }
        Creature* c = map->GetCreature(ObjectGuid(HIGHGUID_UNIT, entry, lowGuid));
        if (c)
        {
            if (!dynamic_cast<HarnessAI*>(c->AI()))
            {
                c->SetAI(new HarnessAI(c, c->AI(), this));
            }
            // A found creature is the world's own and is not ticked without a player
            // nearby unless it is on the map's active list; the runner hands it back
            // whole when the scenario ends (its factory AI restored, and deactivated
            // again if it was Find that activated it) instead of despawning it.
            FoundActor fa;
            fa.guid = c->GetObjectGuid();
            fa.wasActive = c->IsActiveObject();
            fa.wasListed = map->IsActive(c);
            c->SetActiveObjectState(true);
            m_found.push_back(fa);
        }
        return c;
    }

    Creature* Scenario::Get(ObjectGuid guid) const
    {
        Map* map = GetMap();
        return map ? map->GetCreature(guid) : NULL;
    }

    void Scenario::Silence(Creature* c)
    {
        if (!c)
        {
            return;
        }
        // A Find'd creature is the world's own and the runner hands it back by installing the AI
        // this decorator was wrapping; deleting that AI here would hand the world a creature with
        // none at all. Spawned actors only, and the runner despawns those.
        bool spawned = false;
        for (size_t i = 0; i < m_spawned.size() && !spawned; ++i)
        {
            spawned = m_spawned[i] == c->GetObjectGuid();
        }
        if (!spawned)
        {
            Log("ERR Silence refused for guid %u: not an actor this scenario spawned", c->GetGUIDLow());
            return;
        }
        if (HarnessAI* recording = dynamic_cast<HarnessAI*>(c->AI()))
        {
            delete recording->Release();   // the decorator stays; the AI it forwarded to is gone
        }
    }

    MovementGeneratorType Scenario::Type(Creature* c) const
    {
        // The binding's projection, not a generator's: the simple moves are natives now.
        return c ? c->GetMotionMaster()->GetCurrentMovementGeneratorType() : IDLE_MOTION_TYPE;
    }

    uint32 Scenario::Node(Creature* c) const
    {
        return c ? c->GetMotionMaster()->SelectedPatrolNode() : 0;
    }

    Motion::RelayCounts const* Scenario::Relays(Creature* c) const
    {
        return c ? c->GetMotionMaster()->SelectedRelays() : NULL;
    }

    void Scenario::Log(char const* fmt, ...) const
    {
        char text[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        Out(std::string("MVTEST ") + m_name + " " + text);
    }

    void Scenario::Verdict(std::string const& body)
    {
        if (m_finished)
        {
            return;
        }
        Out("MVTEST " + VerdictLine(m_name, body));
        m_finished = true;
    }
}
