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
            /// Re-read at every call: the unit may move (the shell's `!Unit::CannotMove()`, the block as of the last commit); the generator re-read it
            /// after a node's effects, which may have rooted or stunned the unit.
            virtual bool CanMove() const = 0;
            virtual bool Casting() const = 0;          ///< a non-melee spell in progress (the patrol holds)
            virtual bool WaypointPaused() const = 0;   ///< UNIT_STAT_WAYPOINT_PAUSED, a script's bit
            virtual bool Anchor(Vector3& out) const = 0; ///< the creature's combat anchor; false when zero
            /// Live: the unit is a creature that can fly (Creature::CanFly()), false for anything
            /// else; the wander generator re-read it on every tick, since a shapeshift, an aura or
            /// a levitate flips it under a leash that was laid long before.
            virtual bool CanFly() const = 0;
            /// The frame's free-spot search around an explicit centre: the collision selector
            /// with the mover's extent as the searcher bounding, dropped onto the frame's ground.
            /// The centre is the native's -- the target's live position, or a led one -- rather
            /// than whatever the target object's placement last recorded.
            virtual bool StandingSpot(Vector3 const& center, float distance2d, float absAngle, Vector3& out) = 0;
            /// The fear source, resolved on demand at the pick (the generator's own schedule, not per
            /// tick): `position` is its placement in the mover's frame, `distance` the distance between
            /// the two placements. False when it cannot be found. A corpse resolves: it still frightens.
            virtual bool Fright(uint64 rawGuid, Vector3& position, float& distance) = 0;
            /// The frame's floor under a guess reached from the mover's own position, the WHOLE point:
            /// for a player the guess is pulled back to the first obstruction on the way, which moves
            /// x and y as well as z (the flee's wall rule); Ground() keeps only z and cannot carry it.
            virtual bool GroundPoint(Vector3 const& guess, Vector3& out) = 0;
            /// Live: a Control claim of this kind is held (HoldsControl). Read inside a finish, the
            /// arbiter has erased the finishing claim already, so it answers for a survivor.
            virtual bool ClaimHeld(Motion::Kind kind) const = 0;
    };

    /// One coherent observation of a tracked target in the mover's frame (design §3).
    struct TargetView
    {
        bool    valid = false;       ///< resolved this tick, alive, in the world, sharing the mover's frame
        Vector3 position;            ///< the live position: the running spline's, else the placement
        float   facing = 0.0f;       ///< its orientation, in the mover's frame
        float   extent = 0.0f;       ///< bounding radius
        float   reachSum = 0.0f;     ///< the two combat reaches, no offset
        float   meleeRange = 5.0f;   ///< max(reachSum + 4/3, 5): the client's own test
        bool    walking = false;     ///< it is in walk mode: a follower mirrors the gait
        bool    isVictim = false;    ///< the mover's current victim
        Vector3 velocity;            ///< frame yd/s, zero unless trusted
        bool    velocityTrusted = false;
    };

    /// Why a tracking native derived a fresh spot (design §3: counted apart, GM-dumpable).
    enum class RelayCause : uint8 { Routine, Cut, Partial, Blocked, Finished, First };
    struct RelayCounts
    {
        uint32 routine = 0, cut = 0, partial = 0, blocked = 0, finished = 0, first = 0;
        uint32 Total() const { return routine + cut + partial + blocked + finished + first; }
        void Count(RelayCause c);
    };
    inline void RelayCounts::Count(RelayCause c)
    {
        switch (c)
        {
            case RelayCause::Routine:  ++routine;  break;
            case RelayCause::Cut:      ++cut;      break;
            case RelayCause::Partial:  ++partial;  break;
            case RelayCause::Blocked:  ++blocked;  break;
            case RelayCause::Finished: ++finished; break;
            case RelayCause::First:    ++first;    break;
        }
    }

    /// What a behaviour may know about its unit this tick.
    struct Sight
    {
        MoveStatus status;           ///< the driver's view of the live leg
        Vector3    position;         ///< world
        float      facing = 0.0f;
        bool       canReact = true;  ///< not stunned, feared, confused, rooted, distracted or feigning (the old !(CAN_NOT_REACT | NOT_MOVE)): the generators' Initialize guard
        bool       canMove = true;   ///< !Unit::CannotMove() (the old !CAN_NOT_MOVE)
        bool       notMove = false;  ///< root, stun, a feign or a distract's stand (the old UNIT_STAT_NOT_MOVE): the confuse's activation guard, one bit more than !canMove
        bool       landed = false;   ///< the Effect's spline ran out uncut (Finalized && !Cut)
        bool       alive = true;
        bool       hasTarget = false; ///< a tracked target exists (the charge)
        Vector3    targetPoint;       ///< its contact point, world
        bool       runningState = false; ///< UNIT_STAT_RUNNING_STATE
        bool       levitating = false;   ///< Unit::IsLevitating()
        TargetView target;            ///< the tracked target, filled only for a TracksTarget() native
        float      extent = 0.0f;     ///< the mover's own bounding radius
        bool       isCreature = false;   ///< TYPEID_UNIT: every Effect is a creature's
        bool       isPet = false;        ///< Creature::IsPet()
        bool       combatMovementHeld = false; ///< UNIT_STAT_NO_COMBAT_MOVEMENT
        bool       swimming = false;     ///< MOVEFLAG_SWIMMING
        bool       canFlyHint = false;   ///< a creature's Creature::CanFly(): the drift test adds the height term for fliers, as the generator's did
        bool       suspended = false;   ///< Suspend() ran since the last Activate/Resume: the shell skips a finish's interrupt, and a native's displacing clear along with it (the generators' Interrupt carried the clear)
    };

    /// The roaming pair the shell mirrors for the point family (UNIT_STAT_ROAMING | ROAMING_MOVE) until a later family retires it.
    /// SetRoam/SetMove are the generators' single-bit writes (Initialize/Reset, and the hop or the leg).
    enum class Roaming : uint8 { Keep, SetBoth, ClearMove, ClearBoth, SetRoam, SetMove };

    /// One shell operation of a Step or an Outcome, performed in order. Every kind names its
    /// owners (Effect::Owners): a creature's kinds are skipped for a player owner (the generators
    /// returned before the inform and the re-engage for a non-creature), a player's for a
    /// creature, and the StateRaw masks are every owner's.
    struct Effect
    {
        enum Kind : uint8
        {
            Inform,            ///< creature.AI()->MovementInform(who, id)
            SummonedInform,    ///< a temporary summon's creature summoner: SummonedMovementInform(the summon, who, id)
            ReengageVictim,    ///< creature, alive, not confused/fleeing (the block as of the last commit: a fear the inform just applied is not yet seen) nor no-combat-movement, not chasing/following and with a victim (read live, after the inform) -> MoveChase(victim)
            CallAssistance,    ///< SetNoCallAssistance(false); CallAssistance()
            SeekAssistDistract,///< if alive: MoveSeekAssistanceDistract(the configured delay)
            AttackVictim,      ///< if a victim and alive: AttackStop(true); AI()->AttackStart(victim)
            PathInform,        ///< creature.AI()->WaypointPathInform(raw = the path id, event, id = the node): an external path's progress
            RunScript,         ///< creature.GetMap()->ScriptsStart(DBS_ON_CREATURE_MOVEMENT, id, &creature, &creature)
            Emote,             ///< creature.HandleEmote(id)
            CastSpell,         ///< creature.CastSpell(&creature, id, false)
            SetDisplay,        ///< creature.SetDisplayId(id)
            Say,               ///< creature.MonsterText(sObjectMgr.GetMangosStringLocale(int32(id))) when found, else the DB error as the generator logged it
            ClearEmoteState,   ///< creature.SetUInt32Value(UNIT_NPC_EMOTESTATE, 0)
            SetWalk,           ///< creature.SetWalk(flag, false)
            ClearWaypointPaused, ///< clearUnitState(UNIT_STAT_WAYPOINT_PAUSED)
            StateRaw,          ///< addUnitState(setMask) when non-zero, then clearUnitState(clearMask) when non-zero: the opaque unit-state masks a tracking native carries in its Params.
                               ///< Performed for EVERY owner (Effect::Owners: OwnerAny): a feared or confused player carries its move bit exactly as the generators wrote it; the
                               ///< shell's loop lifts the creature-only rule for this kind since P5-B family 4.
            SyncSpeed,         ///< a pet whose owner is the native's target: UpdateSpeed(MOVE_RUN/MOVE_WALK/MOVE_SWIM, true), the deleted SyncSpeedWithMaster
            EngageInReach,     ///< live predicate: the mover's live position against the target view's, 3D, within meleeRange -> Attack(target, true); re-emitted every idle tick, so a stale false never suppresses the attack. It skips only a target already being MELEED, not every victim: a ranged attacker's victim is upgraded to melee here, once (the deleted ReachTarget's own job), and Unit::Attack returns early afterwards
            RestoreTemporaryFaction, ///< if (GetTemporaryFactionFlags() & TEMPFACTION_RESTORE_REACH_HOME) ClearTemporaryFaction()
            LoadAddon,         ///< creature.LoadCreatureAddon(true)
            JustReachedHome,   ///< creature.AI()->JustReachedHome()
            ClearTarget,       ///< creature.SetTargetGuid(ObjectGuid()): the flee's creature initialisation
            ClearFleeingFlag,  ///< creature.RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING): the timed flee's end, when the native found no surviving fear claim
            RestoreGait,       ///< creature.SetWalk(!hasUnitState(UNIT_STAT_RUNNING_STATE), false), the mask read LIVE at the effect's place in the recipe: the generators' cleanup read it after the interrupt's clear and their Finalize before its own, and a Sight value sampled before the recipe would carry the finishing leg's own move bit and choose the run
            // ---- the taxi's six operations (P5-B family 5): a player's alone. The native says WHEN; the shell's Player::Taxi* say HOW, each in one place, in retail's order. ----
            TaxiTakeoff,       ///< Player::TaxiTakeoff(id = the mount display id): the stop, the client control revoked, the hostile references offline, the pet unsummoned, the mount display written without UNIT_FLAG_MOUNT, DISABLE_MOVE | TAXI_FLIGHT set
            TaxiEvent,         ///< StartEvents_Event(map, id, player, player, flag = departure): a path node's DBC event, in the generator's alternation
            TaxiSeam,          ///< Player::TaxiSeamPassed(): a route hop's shared node was left; the route advances (m_taxi.NextTaxiDestination) and a taxi cheater learns the hub
            TaxiCross,         ///< Player::TaxiCross(raw = the map id, point, angle): the far teleport onto the next map's first path node; refused -> the flight is expired (TaxiAbort)
            TaxiLand,          ///< Player::ScheduleTaxiLanding(flag = snap onto point, angle): performed by Player::Update once the teleport-deferral window has closed, in retail's order (design §6.4)
            TaxiAbort          ///< Player::TaxiAbort(): every non-landing end (reason names it): the flags and the mount display cleared, the pet back, the hostile references online, the route cleared, the client control returned
        };
        /// Who performs a kind: a creature, a player, or both (a mask).
        enum Owner : uint8 { OwnerCreature = 1, OwnerPlayer = 2, OwnerAny = 3 };
        Kind         kind;
        Motion::Kind who;
        uint32       id;
        uint32       raw;    ///< PathInform's path id; TaxiCross's map id
        PathEvent    event;  ///< PathInform's event
        bool         flag;   ///< SetWalk's value; TaxiEvent's departure; TaxiLand's snap
        uint32       setMask;   ///< StateRaw: the bits to add
        uint32       clearMask; ///< StateRaw: the bits to clear
        Vector3      point;     ///< TaxiCross: the node teleported onto; TaxiLand: the TaxiNodes position (world)
        float        angle;     ///< TaxiCross/TaxiLand: the facing the teleport keeps
        FinishReason reason;    ///< TaxiAbort: why the flight ended
        Effect(Kind k, Motion::Kind w = Motion::Kind::Idle, uint32 i = 0) : kind(k), who(w), id(i), raw(0), event(PathEvent::NodeReached), flag(false), setMask(0), clearMask(0), angle(0.0f), reason(FinishReason::Arrived) {}
        static Effect Path(uint32 pathId, PathEvent event, uint32 node) { Effect e(PathInform); e.raw = pathId; e.event = event; e.id = node; return e; }
        static Effect Walk(bool walk) { Effect e(SetWalk); e.flag = walk; return e; }
        static Effect State(uint32 set, uint32 clear) { Effect e(StateRaw); e.setMask = set; e.clearMask = clear; return e; }
        static Effect Takeoff(uint32 mountDisplayId) { Effect e(TaxiTakeoff); e.id = mountDisplayId; return e; }
        static Effect NodeEvent(uint32 eventId, bool departure) { Effect e(TaxiEvent); e.id = eventId; e.flag = departure; return e; }
        static Effect Seam() { return Effect(TaxiSeam); }
        static Effect Cross(uint32 mapId, Vector3 const& pos, float facing) { Effect e(TaxiCross); e.raw = mapId; e.point = pos; e.angle = facing; return e; }
        static Effect Land(bool snap, Vector3 const& pos, float facing) { Effect e(TaxiLand); e.flag = snap; e.point = pos; e.angle = facing; return e; }
        static Effect Abort(FinishReason why) { Effect e(TaxiAbort); e.reason = why; return e; }
        /// The owners of a kind. The StateRaw masks are a player's as much as a creature's (a
        /// feared player carries UNIT_STAT_FLEEING_MOVE exactly as the generators wrote it); the
        /// taxi's six are a player's alone; every other kind is a creature's.
        static uint8 Owners(Kind k)
        {
            switch (k)
            {
                case StateRaw:
                    return OwnerAny;
                case TaxiTakeoff:
                case TaxiEvent:
                case TaxiSeam:
                case TaxiCross:
                case TaxiLand:
                case TaxiAbort:
                    return OwnerPlayer;
                case Inform:
                case SummonedInform:
                case ReengageVictim:
                case CallAssistance:
                case SeekAssistDistract:
                case AttackVictim:
                case PathInform:
                case RunScript:
                case Emote:
                case CastSpell:
                case SetDisplay:
                case Say:
                case ClearEmoteState:
                case SetWalk:
                case ClearWaypointPaused:
                case SyncSpeed:
                case EngageInReach:
                case RestoreTemporaryFaction:
                case LoadAddon:
                case JustReachedHome:
                case ClearTarget:
                case ClearFleeingFlag:
                case RestoreGait:
                    return OwnerCreature;
            }
            return OwnerCreature;
        }
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
        std::vector<Effect> effects;  ///< performed by the shell after the stop/interrupt/roaming writes and before the intent, in order; each kind's owners per Effect::Owners
        bool       again = false;     ///< call Tick again at once (no elapsed time) instead of applying the intent; the round's Sight is one snapshot shared by every round of one Tick, but the Services reads (CanMove, Casting, WaypointPaused, Anchor) are live -- a native observes its own ClearWaypointPaused through the port, not the Sight

        static Step None() { return Step(); }
        static Step Of(MoveIntent const& i) { Step s; s.apply = true; s.intent = i; return s; }
    };

    /// A finish's recipe: the roaming write, an interrupt if the leg still runs, a stop, then the effects in order.
    struct Outcome
    {
        Roaming             roaming = Roaming::Keep;
        bool                interrupt = false; ///< performed by the shell only when it did not already suspend this behaviour; a suspended one was interrupted at its Suspend
        bool                stop = false;       ///< Unit::StopMoving(): the flee's player finish (nothing sent when no spline runs; an airborne one is left alone)
        bool                stopForced = false; ///< Unit::StopMoving(true): the confuse's player finish (the stop always sent). Both performed for every owner, after the interrupt and before the effects; when both are set the forced stop is performed (one StopMoving(true))
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
            virtual bool TracksTarget() const { return false; }         ///< the Sight needs `target` (and `targetPoint`, the charge's)
            virtual uint64 Target() const { return 0; }                  ///< the tracked target's raw guid
            /// A sub-type of the kind, 0 for every native but the timed flee's 1: the facade's SelectedVariant() reads it for the harness.
            virtual uint32 Variant() const { return 0; }
            /// The Sight needs `targetPoint`, the charge's contact point: a free-spot search per tick
            /// that only the point native's charge reads, so only it pays for it.
            virtual bool NeedsContactPoint() const { return false; }
            /// A tracking native's re-lay counters, by cause; NULL for one that keeps none.
            virtual RelayCounts const* Relays() const { return 0; }
            /// The home/reset position a default behaviour answers (the patrol); false when none.
            virtual bool ResetPosition(Sight const& /*sight*/, Services& /*svc*/, Vector3& /*pos*/, float& /*o*/) const { return false; }
    };
}

#endif
