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

#pragma once

// WHAT THE GAME ASKS OF MOVEMENT.
//
// The old engine -- the arbiter, the fifteen behaviour classes, the driver, the frame
// adapters and the nine-thousand-line scenario harness, about eighteen thousand lines in
// all -- is deleted. This is the whole of what replaced it on the game's side: a thin
// surface that turns a request into one of the six shapes in src/shared/Move and gets out
// of the way.
//
// The two thousand call sites are kept deliberately. A script asking a creature to walk to
// a point is a statement about the GAME, not about how movement works; those statements are
// the specification the new engine serves, and deleting them would have thrown away the
// requirement along with the implementation.
//
// Nothing here decides anything. Move::Movement holds which behaviour is selected and the
// route in flight; Move::MoveWriter decides what may go on the wire and refuses what would
// make the client teleport the mover; MoveSend puts it there. This file only translates
// vocabulary.
//
// Player movement never passed through here and still does not: that is the client telling
// the server where it went, handled by the protocol in src/motion -- acknowledgements,
// mover authority, time base -- which is not an engine and was kept.

#include "Platform/Define.h"
#include "Mobility.h"
#include "WaypointManager.h"
#include "Move/Movement.h"

#include <sstream>
#include <vector>

namespace Geometry { struct Position; }

class Unit;

// Creature Entry ID used for waypoints show, visible only for GMs
#define VISUAL_WAYPOINT 1

namespace Motion
{
    /// The kinds of movement the game asks for. Kept because callers compare against them
    /// to decide what they are doing; nothing here performs any of them.
    enum class Kind : uint8
    {
        Idle, Wander, Patrol, Follow,
        Chase,
        Point, FlyLand, Home, AssistRun,
        Distract, AssistDistract,
        Fear, Confused,
        Effect,
        Taxi,
        Count
    };

    /// An external waypoint path's progress, as the AI hook CreatureAI::WaypointPathInform
    /// names it. Nothing reports any of these any more.
    enum class PathEvent : uint8 { NodeReached, NodeLeft, LastWaitEnded };

    /// The name of a kind, for the messages that print what a unit is doing.
    inline char const* KindName(Kind kind)
    {
        static char const* const NAMES[] =
        {
            "Idle", "Wander", "Patrol", "Follow",
            "Chase",
            "Point", "FlyLand", "Home", "AssistRun",
            "Distract", "AssistDistract",
            "Fear", "Confused",
            "Effect",
            "Taxi"
        };
        return kind < Kind::Count ? NAMES[uint8(kind)] : "none";
    }

    /**
     * @brief The identity of a Control claim: the aura that holds it.
     * @param spellId The aura's spell (0 for the low-health flee and for a script's fear).
     * @param effIndex The aura's effect index (0 or 1 for the two spell-less cases).
     * @param casterCounter The caster's guid counter (the victim's for the low-health flee).
     * @return A non-zero identity; two applications of one aura share it.
     */
    inline uint64 ControlClaim(uint32 spellId, uint8 effIndex, uint32 casterCounter)
    {
        return (uint64(spellId) << 40) | (uint64(effIndex) << 32) | uint64(casterCounter);
    }
}

/// The requests the game makes of movement, translated into the six shapes and nothing more.
class MotionMaster
{
    public:
        explicit MotionMaster(Unit* unit) : m_unit(unit) {}
        ~MotionMaster();

        void Initialize();
        /// Called from the unit's own update. Costs one comparison for a mover with nothing
        /// due, which is almost all of them almost all of the time: a creature walking a
        /// forty-yard leg has nothing to do for the eight seconds it takes.
        void UpdateMotion(uint32 diff);
        void Clear(bool reset = true, bool all = false);
        void MovementExpired(bool reset = true);

        void MoveIdle();
        void MoveRandomAroundPoint(float x, float y, float z, float radius, float verticalZ = 0.0f);
        void MoveTargetedHome();
        void MoveFollow(Unit* target, float dist, float angle);
        void MoveChase(Unit* target, float dist = 0.0f, float angle = 0.0f);
        void MoveConfused(uint64 = 0);
        void MoveFleeing(Unit* enemy, uint32 timeLimit = 0, uint64 = 0);
        void MovePoint(uint32 id, float x, float y, float z, bool generatePath = true);
        void MoveSeekAssistance(float x, float y, float z);
        void MoveSeekAssistanceDistract(uint32) {}
        void MoveWaypoint(int32 pathId = 0, uint32 source = 0, uint32 initialDelay = 0, uint32 overwriteEntry = 0);
        bool PauseWaypoints(int32) { return false; }
        void MoveTaxiFlight(std::vector<uint32> const&, uint32, uint32) {}
        void TaxiContinue() {}
        void MoveDistract(uint32) {}
        bool MoveJump(float, float, float, float, float, uint32 = 0) { return false; }
        bool MoveJump(Geometry::Position&, float, float, uint32 = 0) { return false; }
        bool MoveJump(float, float, float, float, float, float, Unit*) { return false; }
        void MoveFall() {}
        void MoveFlyOrLand(uint32, float, float, float, bool) {}
        void MoveCharge(Unit*, float) {}
        void MoveCharge(float, float, float, float) {}

        void Inhibit(Motion::Inhibition, uint64) {}
        void Uninhibit(Motion::Inhibition, uint64) {}
        bool Inhibited(Motion::Inhibition) const { return false; }

        void PropagateSpeedChange() {}
        bool SetNextWaypoint(uint32) { return false; }
        uint32 getLastReachedWaypoint() const { return 0; }
        void GetWaypointPathInformation(std::ostringstream& oss) const { oss << "No movement."; }
        bool GetWaypointPathInformation(int32&, WaypointPathOrigin&) const { return false; }
        bool AddToSelectedPatrolPause(int32) { return false; }
        uint32 SelectedPatrolNode() const { return 0; }
        bool GetDestination(float&, float&, float&) { return false; }

        void Die() {}
        void CancelControl(Motion::Kind) {}
        void ExpireCombat() {}
        bool ReleaseControl(uint64) { return false; }
        bool HoldsControl(Motion::Kind) const { return false; }
        void RelocateSelected(float, float, float, float) {}

        /// The state the old engine published about a unit. Permanently empty: a unit that
        /// never moves is never rooted, feared or distracted by anything movement knows.
        struct PublishedState
        {
            uint8 reasons = 0;
            bool feign = false;
        };
        PublishedState const& Published() const { return m_published; }
        void ClearPublished() {}
        void PublishTaxiEnded() {}
        Motion::MobilityDecision Mobility() const { return Motion::MobilityDecision(); }

        /// Which legs were in flight. Nothing flies, so every one of these is false and
        /// `Moving()` is the answer the rest of the server reads: a unit is always stopped.
        struct LatchBank
        {
            bool chase = false;
            bool chaseLeg = false;
            bool follow = false;
            bool followLeg = false;
            bool roaming = false;
            bool roamingLeg = false;
            bool fearLeg = false;
            bool confusedLeg = false;
            bool Moving() const { return false; }
            bool RunningLeg() const { return false; }
        };
        LatchBank const& Latches() const { return m_latches; }
        void ClearMovingLatches() {}
        void ClearAllLatches() {}
        void WipeLatches() {}
        void WriteLatches(Motion::Kind, uint8, uint8) {}

        struct HeldView
        {
            Motion::Kind kind = Motion::Kind::Idle;
            bool selected = false;
            bool reachable = true;
            uint64 target = 0;
        };
        std::vector<HeldView> Held() const { return std::vector<HeldView>(); }

        Motion::Kind ActiveKind() const;
        bool IsChasing() const;
        Unit* ChaseTarget() const { return 0; }
        bool IsFollowing() const;
        Unit* FollowTarget() const { return 0; }
        bool IsPatrolling() const;
        bool IsOnTaxi() const { return false; }
        bool IsReachable() const { return true; }
        void CombatStarted() {}

        /// Where the mover is at this instant. While a route runs this is arithmetic over
        /// the polyline the client was sent -- the same arithmetic the client does -- so the
        /// two agree to the millisecond instead of to the last position write.
        bool LivePosition(Geometry::Vector3& out) const;
        bool IsMoving() const { return m_movement.InFlight().Running(); }

    private:
        /// Ask the selected behaviour, then send or refuse what it asked for.
        void Serve(bool legEnded, bool cut);

        Unit* m_unit;
        Move::Movement m_movement;
        /// A pursuit reads its target through this, and holds a reference for its life, so
        /// it outlives the call that made it. Raw rather than a smart pointer because the
        /// header must not need the definition to declare the member.
        class MoveSighting* m_sighting = 0;
        uint32 m_legEndsAt = 0;
        bool m_legRunning = false;
        PublishedState m_published;
        LatchBank m_latches;
};
