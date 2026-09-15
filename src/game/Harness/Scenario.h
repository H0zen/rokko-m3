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

#ifndef MANGOS_HARNESS_SCENARIO_H
#define MANGOS_HARNESS_SCENARIO_H

#include "Timeline.h"
#include "ObjectGuid.h"
#include "MotionMaster.h"

#include <string>
#include <vector>

class Creature;
class Map;

namespace Harness
{
    /// One MovementInform the recording AI saw: the generator type, the id the
    /// script gave, where the creature stood, and whose.
    struct Inform
    {
        uint32 type;
        uint32 id;
        float  x;
        float  y;
        uint32 guidLow;
    };

    /// One creature Find resolved: its guid, whether the world already had it
    /// active before Find set the flag, and whether the map already listed it on
    /// m_activeNonPlayers before that (a camera can list a creature without ever
    /// flagging it active) - so the runner's sweep restores both exactly.
    struct FoundActor
    {
        ObjectGuid guid;
        bool       wasActive;
        bool       wasListed;
    };

    /**
     * One headless scenario (design v2 §12): it spawns its actors, drives the
     * MotionMaster facade from a step timeline, samples positions and generator
     * types, and prints its verdict. The runner owns the map and the clock; the
     * scenario re-resolves every actor by guid on each step, as the old Lua did.
     */
    class Scenario
    {
    public:
        /// `order` is the scenario's place in the old harness's run order (1-16).
        Scenario(char const* name, int order) : m_name(name), m_order(order), m_finished(false) {}
        virtual ~Scenario() {}

        char const* Name() const { return m_name; }
        int Order() const { return m_order; }
        /// Spawn what the first step needs and fill the timeline; called once at start.
        virtual void Prepare() = 0;
        void Tick(uint32 nowMs) { m_timeline.Advance(nowMs); }
        bool Finished() const { return m_finished; }
        /// True while the timeline has no pending steps left to run.
        bool Idle() const { return m_timeline.Idle(); }
        /// The timeline ran dry, or the runner's per-scenario ceiling fired, without a
        /// verdict: reads BROKEN (or `reason`, for the ceiling) instead of wedging the runner.
        void Abandon(char const* reason = "BROKEN(no verdict: the timeline ran dry)") { Verdict(reason); }
        /// Every guid Spawn handed out: the runner despawns them at the end.
        std::vector<ObjectGuid> const& Spawned() const { return m_spawned; }
        /// Every creature Find resolved and activated: the runner hands each back
        /// whole at the end (a Find'd creature is the world's own; it is never
        /// despawned).
        std::vector<FoundActor> const& Found() const { return m_found; }
        std::vector<Inform>& Informs() { return m_informs; }
        /// A MovementInform the recording AI just saw, delivered synchronously from inside
        /// the inform: a scenario that must act while the generator's Update is still on
        /// the stack overrides this. The default records nothing more.
        virtual void OnInform(Creature* /*creature*/, uint32 /*type*/, uint32 /*id*/) {}
        void Reset();

    protected:
        void At(uint32 ms, std::function<void()> step) { m_timeline.At(ms, step); }
        uint32 Now() const { return m_timeline.Now(); }
        /// A TemporarySummon with no summoner, on the harness map, the grid loaded,
        /// the recording AI installed; NULL (and a logged ERR) when the template is
        /// missing or the create fails.
        Creature* Spawn(uint32 entry, float x, float y, float z, float o);
        /// A creature from the world database (S8's patroller), on the harness map.
        Creature* Find(uint32 lowGuid, uint32 entry);
        Creature* Get(ObjectGuid guid) const;
        void Load(float x, float y);
        /// The map's height at (x, y) near z; the caller's z when the map has none.
        float Ground(float x, float y, float z) const;
        MovementGeneratorType Type(Creature* c) const;
        char const* TypeName(Creature* c) const { return Harness::TypeName(uint32(Type(c))); }
        /// The current waypoint node when the top generator is a patrol, else 0.
        uint32 Node(Creature* c) const;
        /// "MVTEST <name> " + the formatted text.
        void Log(char const* fmt, ...) const;
        /// Prints "MVTEST VERDICT <name> <body>" and ends the scenario.
        void Verdict(std::string const& body);
        Map* GetMap() const;

    private:
        char const*             m_name;
        int                     m_order;
        Timeline                m_timeline;
        bool                    m_finished;
        std::vector<ObjectGuid> m_spawned;
        std::vector<FoundActor> m_found;
        std::vector<Inform>     m_informs;
    };
}

#endif
