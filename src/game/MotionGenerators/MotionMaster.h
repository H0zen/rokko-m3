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

// NOTHING MOVES.
//
// There is no movement engine. The arbitration, the fifteen behaviours, the driver, the
// frame adapters and the nine-thousand-line harness that existed to exercise them are
// deleted, not disabled -- about eighteen thousand lines. What remains here is the shape of
// the requests the game makes, and every one of them does nothing.
//
// This file is deliberately not an engine and must not grow into one. It exists so that
// the two thousand places that ask for movement still say what they want: a script asking
// a creature to walk to a point is a statement about the game, not about how movement
// works, and those statements are the specification the replacement will be written
// against. Deleting them would destroy the requirement along with the implementation.
//
// Consequently: every command returns without acting, every query answers "no" or zero,
// and the server moves nothing. A player walking still works: that is the client telling
// the server where it went, handled by the movement protocol in src/motion -- the
// acknowledgements, the mover authority and the time base -- which is not an engine and
// was kept.

#include "Platform/Define.h"
#include "Mobility.h"
#include "WaypointManager.h"

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

/// The requests the game makes of movement, and nothing behind them.
class MotionMaster
{
    public:
        explicit MotionMaster(Unit* unit) : m_unit(unit) {}

        void Initialize() {}
        void UpdateMotion(uint32 /*diff*/) {}
        void Clear(bool /*reset*/ = true, bool /*all*/ = false) {}
        void MovementExpired(bool /*reset*/ = true) {}

        void MoveIdle() {}
        void MoveRandomAroundPoint(float, float, float, float, float = 0.0f) {}
        void MoveTargetedHome() {}
        void MoveFollow(Unit*, float, float) {}
        void MoveChase(Unit*, float = 0.0f, float = 0.0f) {}
        void MoveConfused(uint64 = 0) {}
        void MoveFleeing(Unit*, uint32 = 0, uint64 = 0) {}
        void MovePoint(uint32, float, float, float, bool = true) {}
        void MoveSeekAssistance(float, float, float) {}
        void MoveSeekAssistanceDistract(uint32) {}
        void MoveWaypoint(int32 = 0, uint32 = 0, uint32 = 0, uint32 = 0) {}
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

        Motion::Kind ActiveKind() const { return Motion::Kind::Idle; }
        bool IsChasing() const { return false; }
        Unit* ChaseTarget() const { return 0; }
        bool IsFollowing() const { return false; }
        Unit* FollowTarget() const { return 0; }
        bool IsPatrolling() const { return false; }
        bool IsOnTaxi() const { return false; }
        bool IsReachable() const { return true; }
        void CombatStarted() {}

    private:
        Unit* m_unit;
        PublishedState m_published;
        LatchBank m_latches;
};
