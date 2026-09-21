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
#include "Arbiter.h"

#include <string>
#include <vector>

class Creature;
class Map;
class Player;

namespace Harness
{
    /// A sampled point (x, y, z): shared by every family that records where an actor
    /// stood over a run (ScenariosFlee.cpp's original, moved here so a later family
    /// does not carry its own copy).
    struct Pt { float x, y, z; };

    /// The distance from the first sample to the farthest one: how far the unit got.
    float Spread(std::vector<Pt> const& samples);

    /// One event the recording AI saw: a MovementInform (the kind and the id the native gave),
    /// an external path's WaypointPathInform, the home reached, or the death; where the creature
    /// stood, and whose.
    /// kind, id, pathId and pathEvent are meaningful only for the event named in each field's doc; a
    /// ReachedHome or Died record carries their defaults (kind reads Idle).
    struct Inform
    {
        enum class Event : uint8 { Inform, PathInform, ReachedHome, Died };
        Event  event = Event::Inform;
        Motion::Kind kind = Motion::Kind::Idle;   ///< Inform: the kind that informed
        uint32 id = 0;                    ///< Inform: the id; PathInform: the node
        uint32 pathId = 0;                ///< PathInform: the external path's id
        Motion::PathEvent pathEvent = Motion::PathEvent::NodeReached;   ///< PathInform: what happened
        float  x = 0.0f;
        float  y = 0.0f;
        uint32 guidLow = 0;
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
     * MotionMaster facade from a step timeline, samples positions and behaviour
     * kinds, and prints its verdict. The runner owns the map and the clock; the
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
        /// Every guid SpawnPlayer handed out. Kept apart from Spawned() because a player
        /// leaves by a different door: the runner unregisters him, removes him from the map
        /// and deletes his session, where a creature is simply unsummoned.
        std::vector<ObjectGuid> const& SpawnedPlayers() const { return m_players; }
        /// True for a scenario that puts a player on the map (SpawnPlayer). A player
        /// promotes the grids around it to full state and changes Map::Update's own
        /// visitation order for as long as he is in world, so the runner requires every
        /// scenario answering true here to run after every scenario that does not, and
        /// resets the map's grids behind it once it ends. Default false; a scenario that
        /// calls SpawnPlayer overrides it to true.
        virtual bool UsesPlayer() const { return false; }
        /// Every creature Find resolved and activated: the runner hands each back
        /// whole at the end (a Find'd creature is the world's own; it is never
        /// despawned).
        std::vector<FoundActor> const& Found() const { return m_found; }
        std::vector<Inform>& Informs() { return m_informs; }
        /// A MovementInform the recording AI just saw, delivered synchronously from inside
        /// the inform: a scenario that must act while the behaviour's Tick is still on
        /// the stack overrides this. The default records nothing more.
        virtual void OnInform(Creature* /*creature*/, Motion::Kind /*kind*/, uint32 /*id*/) {}
        /// An external waypoint path's progress, as MovementInform above but for the second hook.
        virtual void OnPathInform(Creature* /*creature*/, uint32 /*pathId*/, Motion::PathEvent /*event*/, uint32 /*node*/) {}
        void Reset();

    protected:
        void At(uint32 ms, std::function<void()> step) { m_timeline.At(ms, step); }
        uint32 Now() const { return m_timeline.Now(); }
        /// A TemporarySummon with no summoner, on the harness map, the grid loaded,
        /// the recording AI installed; NULL (and a logged ERR) when the template is
        /// missing or the create fails.
        Creature* Spawn(uint32 entry, float x, float y, float z, float o);
        /// A Player with no client behind him, on the harness map: a real Player object on a
        /// real WorldSession whose socket and mailbox are null, holding a guid out of the
        /// harness's own reserved block and never written to the database. He exists so the
        /// player-only halves of the kernel - the control handoff above all - get a scenario
        /// instead of a manual live test. NULL when there is no map, when the block is spent,
        /// or when the create fails.
        Player* SpawnPlayer(float x, float y, float z, float o);
        /// A creature from the world database (S8's patroller), on the harness map.
        Creature* Find(uint32 lowGuid, uint32 entry);
        Creature* Get(ObjectGuid guid) const;
        /// Drops the factory AI from under the recording decorator: the creature still records
        /// its informs and its home arrival, but never reacts on its own again - no AttackStart
        /// when something hits it, no evade when a leg goes unreachable, no threat selection
        /// turning it to face an attacker. A scenario that SCRIPTS where an actor goes needs
        /// that, or the AI takes the wheel back the moment a scripted leg ends. Spawned actors
        /// only: the runner despawns those, and hands a Find'd creature its own AI back.
        void Silence(Creature* c);
        void Load(float x, float y);
        /// The map's height at (x, y) near z; the caller's z when the map has none.
        float Ground(float x, float y, float z) const;
        /// The selected kind of the creature's facade (ActiveKind()), Idle for NULL.
        Motion::Kind Type(Creature* c) const;
        /// Motion::KindName(Type(c)): the label the verdicts print (mt=...).
        char const* TypeName(Creature* c) const { return Motion::KindName(Type(c)); }
        /// The current waypoint node when the selected behaviour is a patrol, else 0.
        uint32 Node(Creature* c) const;
        /// The selected tracking native's re-lay counters (routine, cut, partial, blocked,
        /// finished, first), else NULL: nothing selected, a legacy binding, or a native that
        /// counts nothing. Only the chase and the follow keep them.
        Motion::RelayCounts const* Relays(Creature* c) const;
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
        std::vector<ObjectGuid> m_players;
        std::vector<FoundActor> m_found;
        std::vector<Inform>     m_informs;
    };
}

#endif
