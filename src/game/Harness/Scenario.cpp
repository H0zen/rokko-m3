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
#include "Player.h"
#include "PlayerRegistry.h"
#include "WorldSession.h"
#include "Auth/BigNumber.h"

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
        m_players.clear();
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
        // world but never updated, so its movement behaviours never run at all. The
        // flag is set before the add because Map::Add is the single registration
        // point for a new object; setting it afterwards would register through the
        // setter instead - either works, one is enough.
        c->SetActiveObjectState(true);
        c->Summon(TEMPSPAWN_MANUAL_DESPAWN, 0);   // adds it to the map; despawned by the runner's sweep
        c->SetAI(new HarnessAI(c, c->AI(), this));
        m_spawned.push_back(c->GetObjectGuid());
        return c;
    }

    Player* Scenario::SpawnPlayer(float x, float y, float z, float o)
    {
        // The determinism guarantee says so itself rather than waiting to be asked. The runner
        // reads UsesPlayer() to decide both where a scenario sorts in the queue and whether to
        // reset the map's grids behind it; a scenario that spawns a player without overriding
        // the flag would run mid-queue with no reset after it, and every scenario following it
        // would read grids it did not establish -- a silent baseline drift nobody could
        // attribute. Refused before the session exists, so there is nothing to unwind.
        if (!UsesPlayer())
        {
            // Scenario::Log already prefixes the scenario's own name, so the line names it.
            Log("ERR spawn player refused: this scenario calls SpawnPlayer but does not override UsesPlayer() to true; without it the runner neither sorts it last nor resets the map behind it");
            return NULL;
        }
        Map* map = GetMap();
        if (!map)
        {
            Log("ERR spawn player: no map");
            return NULL;
        }
        // The reserved block is what Runner::Start proved free of real characters; a guid past
        // its end was never checked, so it is not ours to hand out.
        if (m_players.size() >= kHarnessPlayerGuidCount)
        {
            Log("ERR spawn player: the harness guid block holds only %u", kHarnessPlayerGuidCount);
            return NULL;
        }
        const uint32 guidlow = kHarnessPlayerGuidFirst + uint32(m_players.size());

        // The body is a human warrior. That choice is load-bearing, so it is checked rather than
        // trusted: Create fires REPLACE INTO character_phase_data for any race/class whose
        // playercreateinfo row carries a phase map (Player.cpp:909-912), and this player has no
        // character row to own such a write. The human warrior's phaseMap is 0 on the database
        // this was written against, but playercreateinfo is a table a server owner may edit, so
        // the "the harness never writes to the character database" constraint enforces itself
        // here instead of resting on what one database happens to hold. Refused before the
        // session exists, so there is nothing to unwind.
        const uint8 race = RACE_HUMAN;
        const uint8 class_ = CLASS_WARRIOR;
        PlayerInfo const* pInfo = sObjectMgr.GetPlayerInfo(race, class_);
        if (!pInfo)
        {
            Log("ERR spawn player %u: no playercreateinfo for race %u class %u", guidlow, race, class_);
            return NULL;
        }
        if (pInfo->phaseMap != 0)
        {
            Log("ERR spawn player %u refused: race %u class %u has playercreateinfo.phaseMap %u, and Create would write character_phase_data for a character that does not exist",
                guidlow, race, class_, pInfo->phaseMap);
            return NULL;
        }

        // A null socket and a null mailbox are safe, but NOT because the session is left alone:
        // Map::Update calls pSession->Update(updater) for every in-world player on the map
        // (Map.cpp:913-924), so this session is updated from the tick its player is added. What
        // makes that harmless is the filter and the null socket, not the absence of the call.
        // MapSessionFilter::ProcessLogout() is false (WorldSession.h:284-287), so the logout
        // block at WorldSession.cpp:599-611 -- which logs out exactly a session whose socket is
        // gone -- is skipped; and with m_Socket null the packet loop (WorldSession.cpp:458) and
        // UpdateSecondStream (WorldSession.cpp:1545-1548) each return before doing anything.
        // The consequence for whoever builds on this: because that loop is gated on m_Socket, a
        // harness session can never dispatch a mailbox packet. Pushing a WorldPacket into
        // m_mailbox will NOT work -- drive the server through its own methods instead.
        WorldSession* session = new WorldSession(kHarnessAccountId, "harness", nullptr, nullptr,
                                                 SEC_PLAYER, EXPANSION_CATA, 0, LOCALE_enUS, BigNumber());

        Player* player = new Player(session);
        session->SetPlayer(player);                      // as login does (CharacterHandler.cpp:771)
        player->GetMotionMaster()->Initialize();         // as login does, before the player ever moves

        // The phase-map refusal above is what makes this call safe to make against any database:
        // with phaseMap 0 the REPLACE INTO character_phase_data at Player.cpp:909-912 cannot
        // fire, and nothing else in Create touches the character database.
        if (!player->Create(guidlow, "HarnessMover", race, class_, GENDER_MALE, 0, 0, 0, 0, 0, 0))
        {
            session->SetPlayer(NULL);
            delete player;
            delete session;
            Log("ERR spawn player %u: create failed", guidlow);
            return NULL;
        }

        player->SetSaveTimer(0xFFFFFFFF);                // never let Player::Update save a character
                                                         // that does not exist

        // Create put him on his race's start map at his race's start point; put him where the
        // scenario asked for, on the harness map, before Map::Add reads the placement.
        // SetMap alone carries the map identity here -- it writes the map and instance ids and
        // re-bases the placement frame (WorldObjectSummon.cpp:66-74), which is all the
        // SetLocationMapId the design named would have done, and that one is protected anyway.
        player->SetMap(map);
        player->Place().MoveTo(x, y, z, o);

        // An assertion, not a recovery. Map::Add(Player*) returns true unconditionally
        // (Map.cpp:687-719), and by the point it could return anything else the player has
        // already been linked into m_mapRefManager and added to his cell -- so the obvious
        // recovery, deleting him, would leave a freed player in the map's reference list and
        // in the grid, and be a worse bug than the failure it handled. There is no correct
        // unwind to write against a branch that cannot be taken; whoever gives Map::Add a real
        // failure path owes this call site a real unwind with it. MANGOS_ASSERT is fatal in
        // Release too and evaluates its condition exactly once (Errors.h:45-70), so the add
        // still happens.
        const bool added = map->Add(player);
        MANGOS_ASSERT(added);

        // Map::Add does NOT do this, and ObjectLookup resolves a player guid only through the
        // registry (ObjectLookup.cpp:37-49), so without it nothing -- a pet's owner least of all --
        // can find him.
        sPlayerRegistry.Add(player);

        // The initial self grant. RevokeMover flips a unit to ServerDriven only when its mover
        // session is this one (WorldSession.cpp:161-164), so a fear's revoke against an ungranted
        // player is a silent no-op and the scenario would pass while proving nothing.
        player->SetClientControl(player, 1);

        // Both halves, recorded together and now: from here on the scenario owns this player
        // and this session, and the runner's teardown works from this record rather than
        // rediscovering either of them (Scenario.h OwnedPlayer, Ownership.h). Recorded after
        // every refusal above, so a record only ever describes a player who exists.
        OwnedPlayer owned;
        owned.guid = player->GetObjectGuid();
        owned.player = player;
        owned.session = session;
        m_players.push_back(owned);
        return player;
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

    Motion::Kind Scenario::Type(Creature* c) const
    {
        // The selected kind: the kernel's own answer, the label the verdicts print.
        return c ? c->GetMotionMaster()->ActiveKind() : Motion::Kind::Idle;
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
