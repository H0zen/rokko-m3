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
    };

    /// The roaming pair the shell mirrors for the point family (UNIT_STAT_ROAMING | ROAMING_MOVE) until a later family retires it.
    enum class Roaming : uint8 { Keep, SetBoth, ClearMove, ClearBoth };

    /// One tick's or one hook's result: shell operations first, then the intent when `apply`.
    struct Step
    {
        bool       stop = false;      ///< Unit::StopMoving (a Point's activation)
        bool       interrupt = false; ///< Unit::InterruptMoving (a Point's suspend)
        bool       resetLeg = false;  ///< MotionDriver::ResetLeg
        Roaming    roaming = Roaming::Keep;
        bool       apply = false;     ///< hand `intent` to the driver (Move/Hold) or the launcher (Launch)
        MoveIntent intent;

        static Step None() { return Step(); }
        static Step Of(MoveIntent const& i) { Step s; s.apply = true; s.intent = i; return s; }
    };

    /// One shell operation of an Outcome, performed in order after the behaviour finished.
    /// Every effect is a creature's: the shell performs none of the recipe for a player
    /// (the generators returned before the inform and the re-engage for a non-creature).
    struct Effect
    {
        enum Kind : uint8
        {
            Inform,            ///< creature.AI()->MovementInform(projection of `who`, id)
            SummonedInform,    ///< a temporary summon's creature summoner: SummonedMovementInform(projection, id)
            ReengageVictim,    ///< live predicate: creature, alive, not confused/fleeing/no-combat-movement, not chasing/following, has a victim -> MoveChase(victim)
            CallAssistance,    ///< SetNoCallAssistance(false); CallAssistance()
            SeekAssistDistract,///< if alive: MoveSeekAssistanceDistract(the configured delay)
            AttackVictim       ///< if a victim and alive: AttackStop(true); AI()->AttackStart(victim)
        };
        Kind         kind;
        Motion::Kind who;
        uint32       id;
        Effect(Kind k, Motion::Kind w = Motion::Kind::Idle, uint32 i = 0) : kind(k), who(w), id(i) {}
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
            virtual Step Activate(Sight const& sight) = 0;              ///< first selection
            virtual Step Suspend() = 0;                                  ///< masked or blocked
            virtual Step Resume(Sight const& sight, bool reset) = 0;     ///< selected again; reset = re-lay from the unit's spot
            virtual Step Tick(Sight const& sight, uint32 diff) = 0;      ///< the selected behaviour's tick
            virtual FinishReason EndReason(Sight const& sight) const = 0; ///< after a Done tick
            virtual Outcome Finish(FinishReason why, Sight const& sight) = 0;
            virtual bool TracksTarget() const { return false; }         ///< the Sight needs `targetPoint`
            virtual uint64 Target() const { return 0; }                  ///< the tracked target's raw guid
    };
}

#endif
