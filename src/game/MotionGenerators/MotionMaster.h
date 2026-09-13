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

#ifndef MANGOS_MOTIONMASTER_H
#define MANGOS_MOTIONMASTER_H

#include "Platform/Define.h"
#include "Arbiter.h"
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

struct Position;

class Unit;
class MovementGenerator;
class MotionBehaviour;
class WaypointMovementGenerator;
class FlightPathMovementGenerator;
struct EffectLaunch;

// Creature Entry ID used for waypoints show, visible only for GMs
#define VISUAL_WAYPOINT 1

/**
 * @brief Movement generator type enumeration
 *
 * Values 0 ... MAX_DB_MOTION_TYPE-1 used in database.
 * Values MAX_DB_MOTION_TYPE and above cannot be set in database.
 */
enum MovementGeneratorType
{
    IDLE_MOTION_TYPE = 0,                  ///< Idle movement (IdleMovementGenerator.h)
    RANDOM_MOTION_TYPE = 1,                ///< Random movement (RandomMovementGenerator.h)
    WAYPOINT_MOTION_TYPE = 2,              ///< Waypoint movement (WaypointMovementGenerator.h)
    MAX_DB_MOTION_TYPE = 3,                ///< Maximum database motion type (values below this can be set in DB)

    CONFUSED_MOTION_TYPE = 4,              ///< Confused movement (ConfusedMovementGenerator.h)
    CHASE_MOTION_TYPE = 5,                 ///< Chase movement (TargetedMovementGenerator.h)
    HOME_MOTION_TYPE = 6,                  ///< Return home movement (HomeMovementGenerator.h)
    FLIGHT_MOTION_TYPE = 7,                ///< Flight movement (WaypointMovementGenerator.h)
    POINT_MOTION_TYPE = 8,                 ///< Point movement (PointMovementGenerator.h)
    FLEEING_MOTION_TYPE = 9,               ///< Fleeing movement (FleeingMovementGenerator.h)
    DISTRACT_MOTION_TYPE = 10,             ///< Distract movement (IdleMovementGenerator.h)
    ASSISTANCE_MOTION_TYPE = 11,           ///< Assistance movement (PointMovementGenerator.h - first part of flee for assistance)
    ASSISTANCE_DISTRACT_MOTION_TYPE = 12,  ///< Assistance distract (IdleMovementGenerator.h - second part of flee for assistance)
    TIMED_FLEEING_MOTION_TYPE = 13,        ///< Timed fleeing (FleeingMovementGenerator.h - alternative second part of flee for assistance)
    FOLLOW_MOTION_TYPE = 14,               ///< Follow movement (TargetedMovementGenerator.h)
    EFFECT_MOTION_TYPE = 15,               ///< Effect movement

    EXTERNAL_WAYPOINT_MOVE = 256,          ///< External waypoint move (used in CreatureAI::MovementInform when waypoint reached)
    EXTERNAL_WAYPOINT_MOVE_START = 512,    ///< External waypoint move start (used in CreatureAI::MovementInform when waypoint started)
    EXTERNAL_WAYPOINT_FINISHED_LAST = 1024 ///< External waypoint finished last (used in CreatureAI::MovementInform when last waypoint wait time finished)
};

/**
 * The movement facade and, since P3-B, the kernel's Controller shell (design
 * 2026-09-13-movement-p3b-controller-design.md): one Motion::Arbiter decides which
 * held behaviour runs, one adapted legacy generator behaves per held entry, and
 * every public call is an arbiter transaction whose events reach the behaviours
 * through the hook matrix when the outermost call ends. Only the selected
 * behaviour ticks. The entry points the vendored scripts call are the ones the
 * shim gate pins; the rest is the core's.
 */
class MotionMaster
{
    public:
        explicit MotionMaster(Unit* unit);
        ~MotionMaster();

        /// The factory default: clear everything, install the creature's default movement (idle for players).
        void Initialize();
        /// The selected behaviour's generator; NULL before Initialize.
        MovementGenerator const* GetCurrent() const;
        /// One tick of the selected behaviour; nothing under UNIT_STAT_CAN_NOT_MOVE.
        void UpdateMotion(uint32 diff);
        /// Every command, claim and combat finish; the pushed default too when `all`; the survivor resets when `reset && !all`.
        void Clear(bool reset = true, bool all = false);
        /// The selected behaviour finishes; the exposed one resets when `reset` and nothing was pushed over it.
        void MovementExpired(bool reset = true);

        void MoveIdle();
        void MoveRandomAroundPoint(float x, float y, float z, float radius, float verticalZ = 0.0f);
        void MoveTargetedHome();
        void MoveFollow(Unit* target, float dist, float angle);
        void MoveChase(Unit* target, float dist = 0.0f, float angle = 0.0f);
        void MoveConfused();
        void MoveFleeing(Unit* enemy, uint32 timeLimit = 0);
        void MovePoint(uint32 id, float x, float y, float z, bool generatePath = true);
        void MoveSeekAssistance(float x, float y, float z);
        void MoveSeekAssistanceDistract(uint32 timer);
        void MoveWaypoint(int32 id = 0, uint32 source = 0, uint32 initialDelay = 0, uint32 overwriteEntry = 0);
        /// Holds a waypoint patrol where it stands; true when the selected behaviour was a patrol and took the pause.
        bool PauseWaypoints(int32 ms);
        void MoveTaxiFlight(uint32 path, uint32 pathnode);
        void MoveDistract(uint32 timeLimit);
        void MoveJump(float x, float y, float z, float horizontalSpeed, float max_height, uint32 id = 0);
        void MoveJump(Position& pos, float horizontalSpeed, float max_height, uint32 id = 0);
        /// A jump that ends facing a target, or a given orientation: a raw spline, no behaviour (P5).
        void MoveDestination(float x, float y, float z, float o, float horizontalSpeed, float max_height, Unit* target = NULL);
        void MoveFall();
        void MoveFlyOrLand(uint32 id, float x, float y, float z, bool liftOff);

        MovementGeneratorType GetCurrentMovementGeneratorType() const;
        void PropagateSpeedChange();
        bool SetNextWaypoint(uint32 pointId);
        uint32 getLastReachedWaypoint() const;
        void GetWaypointPathInformation(std::ostringstream& oss) const;
        bool GetDestination(float& x, float& y, float& z);

        /// Death: every behaviour finishes Died while the unit still reads alive, then the idle default.
        void Die();
        /// Release the control claims of this kind (the aura handlers' form until P4).
        void CancelControl(Motion::Kind kind);
        /// A near teleport: suspend the selection, relocate, resume it with a reset.
        void RelocateSelected(float x, float y, float z, float o);
        /// True iff this generator belongs to the selected behaviour (replaces MovementGenerator::IsActive).
        bool IsSelected(MovementGenerator const* generator) const;
        /// A chase (the Combat entry) or a follow (a Default-layer kind), selected or masked --
        /// what an effect's finalizer asks before starting a fresh chase; P3-C's typed queries replace it.
        bool HoldsChaseOrFollow() const;
        /// True when this sequence has a binding and that binding has been activated.
        bool IsActivated(uint32 seq) const;
        /// The held patrol generator wherever it sits (default slot, masked or not), else NULL.
        WaypointMovementGenerator* HeldWaypoint();
        WaypointMovementGenerator const* HeldWaypoint() const;
        /// The held taxi flight, else NULL.
        FlightPathMovementGenerator* HeldFlight();

        /// One held entry for a listing.
        struct HeldView
        {
            MovementGenerator const* generator;
            bool selected;
        };
        /// Every held behaviour in arrival order, the selected one marked.
        std::vector<HeldView> Held() const;
        /// Allocate the arbiter's decision ring (Movement.DecisionRing).
        void EnableDecisionRing();
        /// The model, for the GM dump.
        Motion::Arbiter const& Arbiter() const { return m_arbiter; }

    private:
        /// One held entry's behaviour, keyed by the arbiter's sequence.
        struct Bound
        {
            uint32 seq;
            std::unique_ptr<MotionBehaviour> behaviour;
            bool activated;
            Bound(uint32 s, std::unique_ptr<MotionBehaviour> b);
            Bound(Bound&& other) noexcept;
            Bound& operator=(Bound&& other) noexcept;
            ~Bound();
        };
        /// The stack's reset latch: consumed once at the outermost commit.
        enum class PendingReset : uint8 { None, WhenExposed, Always };

        class Scope;   ///< the transaction guard (MotionMaster.cpp)

        void Request(Motion::MoveRequest const& request, MovementGenerator* generator, bool owned, EffectLaunch const& launch);
        void Request(Motion::MoveRequest const& request, MovementGenerator* generator, bool owned);
        void InstallFactory(Motion::Kind kind, MovementGenerator* generator, bool owned);
        bool Bind(Motion::Kind kind, uint32 seqBefore, MovementGenerator* generator, bool owned, EffectLaunch const& launch);
        void SweepStale(Motion::Kind kind);
        void Commit(std::optional<Motion::Transaction>& transaction);
        void DeliverEvents();
        void Deliver(Motion::Event const& event);
        void Reconcile();
        bool IsHeld(uint32 seq) const;
        size_t IndexOf(uint32 seq) const;
        Bound* Find(uint32 seq);
        Bound const* Find(uint32 seq) const;
        Bound* SelectedBound();
        Bound const* SelectedBound() const;
        void Retire(size_t index, Motion::FinishReason reason);

        Unit*              m_owner;
        Motion::Arbiter    m_arbiter;
        std::vector<Bound> m_bound;
        std::vector<std::unique_ptr<MotionBehaviour> > m_retired; ///< finished behaviours, destroyed at the end of the outermost commit (a generator ticking when its hook finished it must outlive its own Update)
        uint32             m_depth;          ///< open scopes
        Motion::TransactionKind m_scopeKind; ///< the kind the outermost commit runs under; a nested death raises it to Death
        PendingReset       m_pendingReset;
        uint32             m_exposedSeq;     ///< WhenExposed: the entry an expiry exposed
};

#endif // MANGOS_MOTIONMASTER_H
