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

#ifndef MANGOS_MOTION_BEHAVIOURMODEL_H
#define MANGOS_MOTION_BEHAVIOURMODEL_H

#include "Platform/Define.h"
#include "Arbiter.h"
#include "MoveIntent.h"

#include <vector>

/**
 * A native behaviour of the kernel (P5-B family 1 design §3): pure policy. It computes
 * from a Sight the shell fills, returns Steps (an intent plus the shell operations of the
 * moment) and, when it finishes, an Outcome (an ordered recipe with live predicates the
 * shell evaluates). It never touches a unit.
 */
namespace Motion
{
    /// A finish that replaces the behaviour while it is selected: Superseded, Overridden,
    /// Cancelled; the generators' Interrupt path.
    inline bool Displacing(FinishReason why)
    {
        return why == FinishReason::Superseded || why == FinishReason::Overridden || why == FinishReason::Cancelled;
    }

    /// A route's answer (Services::Route): `usable` when the query produced geometry to walk
    /// (a straight fallback included), `routed` only when it is a real route through the mesh.
    struct RouteResult
    {
        bool usable = false;
        bool routed = false;
        bool partial = false;    ///< the route ends short of the goal
        bool progresses = false; ///< the partial route still gets closer (worth walking, as opposed to one that ends where it starts)
    };

    /**
     * The world queries a behaviour may make, on demand, through the shell (design §3):
     * every draw and every route happens when and only when the native asks, so the
     * shared random stream and the query schedule are the native's, as the generators'
     * were. Tests fake it and record the calls.
     */
    class Services
    {
        public:
            virtual ~Services() {}
            /// The frame's mover-aware reachable random point (ground, air or water by the mover); every draw inside.
            virtual bool RandomPoint(Vector3 const& centre, float radius, Vector3& out) = 0;
            /// The floor under a point in the mover's frame.
            virtual bool Ground(Vector3 const& at, float& z) = 0;
            /// The shared RNG: the native's draw order and count are the stream.
            virtual float Frand(float min, float max) = 0;
            /// The shared RNG: the native's draw order and count are the stream.
            virtual uint32 Urand(uint32 min, uint32 max) = 0;
            /// The shared RNG: the native's draw order and count are the stream.
            virtual int32 Irand(int32 min, int32 max) = 0;
            /// A route in the mover's frame; `points` receives the geometry when usable.
            virtual RouteResult Route(Vector3 const& from, Vector3 const& to, PointsArray& points) = 0;
            /// Starts the next route from a fresh router: a welding pass begins here, as the
            /// generator built one router per pass; the mesh router is stateful and reuses a
            /// previous poly path.
            virtual void ResetRoute() = 0;
            /// Live: the unit may move (`!UNIT_STAT_CAN_NOT_MOVE`); the generator re-read it
            /// after a node's effects, which may have rooted or stunned the unit.
            virtual bool CanMove() const = 0;
            virtual bool Casting() const = 0;          ///< a non-melee spell in progress (the patrol holds)
            virtual bool WaypointPaused() const = 0;   ///< UNIT_STAT_WAYPOINT_PAUSED, a script's bit
            virtual bool Anchor(Vector3& out) const = 0; ///< the creature's combat anchor; false when zero
            /// Live: the unit is a creature that can fly (Creature::CanFly()), false for anything
            /// else; the wander generator re-read it on every tick, since a shapeshift, an aura or
            /// a levitate flips it under a leash that was laid long before.
            virtual bool CanFly() const = 0;
    };

    /// What a behaviour may know about its unit this tick.
    struct Sight
    {
        MoveStatus status;           ///< the driver's view of the live leg
        Vector3    position;         ///< world
        float      facing = 0.0f;
        bool       canReact = true;  ///< !(CAN_NOT_REACT | NOT_MOVE): the generators' Initialize guard
        bool       canMove = true;   ///< !CAN_NOT_MOVE
        bool       landed = false;   ///< the Effect's spline ran out uncut (Finalized && !Cut)
        bool       alive = true;
        bool       hasTarget = false; ///< a tracked target exists (the charge)
        Vector3    targetPoint;       ///< its contact point, world
        bool       runningState = false; ///< UNIT_STAT_RUNNING_STATE
        bool       levitating = false;   ///< Unit::IsLevitating()
    };

    /// The roaming pair the shell mirrors for the point family (UNIT_STAT_ROAMING | ROAMING_MOVE) until a later family retires it.
    /// SetRoam/SetMove are the generators' single-bit writes (Initialize/Reset, and the hop or the leg).
    enum class Roaming : uint8 { Keep, SetBoth, ClearMove, ClearBoth, SetRoam, SetMove };

    /// One shell operation of a Step or an Outcome, performed in order. Every effect is a
    /// creature's: the shell performs none of the recipe for a player (the generators
    /// returned before the inform and the re-engage for a non-creature).
    struct Effect
    {
        enum Kind : uint8
        {
            Inform,            ///< creature.AI()->MovementInform(projection of `who`, id)
            SummonedInform,    ///< a temporary summon's creature summoner: SummonedMovementInform(projection, id)
            ReengageVictim,    ///< live predicate: creature, alive, not confused/fleeing/no-combat-movement, not chasing/following, has a victim -> MoveChase(victim)
            CallAssistance,    ///< SetNoCallAssistance(false); CallAssistance()
            SeekAssistDistract,///< if alive: MoveSeekAssistanceDistract(the configured delay)
            AttackVictim,      ///< if a victim and alive: AttackStop(true); AI()->AttackStart(victim)
            InformRaw,         ///< creature.AI()->MovementInform(raw, id): the raw type is the shell's constant handed in as a parameter
            RunScript,         ///< creature.GetMap()->ScriptsStart(DBS_ON_CREATURE_MOVEMENT, id, &creature, &creature)
            Emote,             ///< creature.HandleEmote(id)
            CastSpell,         ///< creature.CastSpell(&creature, id, false)
            SetDisplay,        ///< creature.SetDisplayId(id)
            Say,               ///< creature.MonsterText(sObjectMgr.GetMangosStringLocale(int32(id))) when found, else the DB error as the generator logged it
            ClearEmoteState,   ///< creature.SetUInt32Value(UNIT_NPC_EMOTESTATE, 0)
            SetWalk,           ///< creature.SetWalk(flag, false)
            ClearWaypointPaused ///< clearUnitState(UNIT_STAT_WAYPOINT_PAUSED)
        };
        Kind         kind;
        Motion::Kind who;
        uint32       id;
        uint32       raw;    ///< InformRaw's type
        bool         flag;   ///< SetWalk's value
        Effect(Kind k, Motion::Kind w = Motion::Kind::Idle, uint32 i = 0) : kind(k), who(w), id(i), raw(0), flag(false) {}
        static Effect Raw(uint32 type, uint32 nodeId) { Effect e(InformRaw); e.raw = type; e.id = nodeId; return e; }
        static Effect Walk(bool walk) { Effect e(SetWalk); e.flag = walk; return e; }
    };

    /// One tick's or one hook's result: shell operations first, then the intent when `apply`.
    /// A tick's round needs no guard of its own: the shell re-checks alive/in-world/selected
    /// after every round's effects and drops the round's intent when a hook the effects fired
    /// has replaced or suspended the behaviour.
    struct Step
    {
        bool       stop = false;      ///< Unit::StopMoving (a Point's activation)
        bool       interrupt = false; ///< Unit::InterruptMoving (a Point's suspend)
        bool       resetLeg = false;  ///< MotionDriver::ResetLeg
        Roaming    roaming = Roaming::Keep;
        bool       apply = false;     ///< hand `intent` to the driver (Move/Hold) or the launcher (Launch)
        MoveIntent intent;
        std::vector<Effect> effects;  ///< performed by the shell after the stop/interrupt/roaming writes and before the intent, in order; creatures only
        bool       again = false;     ///< call Tick again at once (no elapsed time) instead of applying the intent; the round's Sight is one snapshot shared by every round of one Tick, but the Services reads (CanMove, Casting, WaypointPaused, Anchor) are live -- a native observes its own ClearWaypointPaused through the port, not the Sight

        static Step None() { return Step(); }
        static Step Of(MoveIntent const& i) { Step s; s.apply = true; s.intent = i; return s; }
    };

    /// A finish's recipe: the roaming write, an interrupt if the leg still runs, then the effects in order.
    struct Outcome
    {
        Roaming             roaming = Roaming::Keep;
        bool                interrupt = false; ///< performed by the shell only when it did not already suspend this behaviour; a suspended one was interrupted at its Suspend
        std::vector<Effect> effects;
    };

    class Behaviour
    {
        public:
            virtual ~Behaviour() {}
            virtual Motion::Kind Kind() const = 0;
            virtual Step Activate(Sight const& sight, Services& svc) = 0;              ///< first selection
            virtual Step Suspend() = 0;                                  ///< masked or blocked
            virtual Step Resume(Sight const& sight, Services& svc, bool reset) = 0;     ///< selected again; reset = re-lay from the unit's spot
            virtual Step Tick(Sight const& sight, Services& svc, uint32 diff) = 0;      ///< the selected behaviour's tick
            virtual FinishReason EndReason(Sight const& sight) const = 0; ///< after a Done tick
            virtual Outcome Finish(FinishReason why, Sight const& sight, Services& svc) = 0;
            virtual bool TracksTarget() const { return false; }         ///< the Sight needs `targetPoint`
            virtual uint64 Target() const { return 0; }                  ///< the tracked target's raw guid
            /// The home/reset position a default behaviour answers (the patrol); false when none.
            virtual bool ResetPosition(Sight const& /*sight*/, Services& /*svc*/, Vector3& /*pos*/, float& /*o*/) const { return false; }
    };
}

#endif
