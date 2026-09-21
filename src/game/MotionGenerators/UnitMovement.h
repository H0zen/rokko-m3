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

// ONE COMPONENT PER UNIT, AND IT OWNS EVERYTHING ABOUT ITS MOTION.
//
// A unit used to keep its movement in five places that had to agree with each other: what
// it was trying to do, the flags on the wire, the acknowledgements the client owed, the
// speeds, and a published copy of "what is forbidden" that the arbiter mirrored into the
// unit's state bits. Two of those were mirrors, and a mirror is a promise that two things
// stay equal. When the arbiter was deleted the mirror stayed behind, empty, and every
// reader of it -- CannotMove, CannotReact, LostControl, IsTaxiFlying -- quietly began
// answering "no" to everything. A root stopped rooting.
//
// So the rule this file exists to enforce is not "one object", it is ONE WRITER. Nothing is
// copied anywhere, so nothing can diverge. What is forbidden is counted here, by source,
// and read from here; there is no second copy to forget to update.
//
// AUTHORITY IS A STATE, NOT A TYPE. A creature is always driven by the server; a player
// drives itself -- until a taxi, a knockback, a fear or a root takes it over, and then the
// server drives it and must hand it back. That is not two classes: a player needs the whole
// server-driven machinery for the seized case anyway, and swapping the object mid-flight
// would lose the pending state exactly when it matters. It is three states and a counter.
//
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
#include "Mobility.h"

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

/// Who is driving a unit right now.
enum class Authority : uint8
{
    Server,   ///< a creature, always: the server decides and the client draws
    Client,   ///< a player driving itself: the server validates and relays
    Seized    ///< a player the server has taken over: a taxi, a knockback, a fear, a root
};

/// One unit's movement, whole.
class UnitMovement
{
    public:
        explicit UnitMovement(Unit* unit) : m_unit(unit) {}
        ~UnitMovement();

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
        void MoveFlyOrLand(uint32 id, float x, float y, float z, bool liftOff);
        void MoveCharge(Unit* target, float speed);
        void MoveCharge(float x, float y, float z, float speed);
        bool MoveJump(float x, float y, float z, float horizontalSpeed, float maxHeight, uint32 id = 0);
        void MoveFall();
        void MoveWaypoint(int32 pathId = 0, uint32 source = 0, uint32 initialDelay = 0, uint32 overwriteEntry = 0);
        bool PauseWaypoints(int32) { return false; }
        void MoveTaxiFlight(std::vector<uint32> const&, uint32, uint32) {}
        void TaxiContinue() {}
        void MoveDistract(uint32) {}
        bool MoveJump(Geometry::Position&, float, float, uint32 = 0) { return false; }
        bool MoveJump(float, float, float, float, float, float, Unit*) { return false; }

        // ---- WHAT IS FORBIDDEN, counted by source. A reason holds while any source holds
        // it, so two roots from two casters need two releases. Read straight from here:
        // there is no published copy, which is the whole point of this class.
        void Inhibit(Motion::Inhibition what, uint64 source);
        void Uninhibit(Motion::Inhibition what, uint64 source);
        bool Inhibited(Motion::Inhibition what) const { return m_blocks.Inhibited(what); }
        void DropDomain(Motion::SourceDomain domain);

        /// Every active reason as Reason bits: the counted inhibitions, plus the ones a held
        /// behaviour IS -- a fear is not a source anyone registered, it is a fear running.
        uint8 Reasons() const;
        bool MayMove() const { return (Reasons() & Motion::kCannotMoveReasons) == 0; }
        bool MayReact() const { return (Reasons() & Motion::kCannotReactReasons) == 0; }
        /// A Dead source that is not the death itself: a feign.
        bool Feigning() const;

        void PropagateSpeedChange() {}
        bool SetNextWaypoint(uint32 pointId);
        uint32 getLastReachedWaypoint() const;
        void GetWaypointPathInformation(std::ostringstream& oss) const { oss << "No movement."; }
        bool GetWaypointPathInformation(int32&, WaypointPathOrigin&) const { return false; }
        bool AddToSelectedPatrolPause(int32) { return false; }
        uint32 SelectedPatrolNode() const;
        bool GetDestination(float& x, float& y, float& z);

        // ---- WHO IS DRIVING
        Authority Driver() const { return m_authority; }
        void TakenByClient() { m_authority = Authority::Client; m_seizeAck = 0; }

        /// The server takes a player over. Answers the counter the client must echo; until
        /// that acknowledgement arrives the server's route is the truth and movement packets
        /// that contradict it are refused, because for that window the unit has two possible
        /// positions and only one of them is ours.
        uint32 Seize();
        /// The acknowledgement came back. A counter that is not the one being waited for is
        /// ignored: a late ack for a seizure that already ended must not release this one.
        bool Released(uint32 counter);
        /// Hand control back without waiting: the flight landed, the knockback ended.
        void Release();
        /// True while a seizure is announced but unacknowledged -- the window in which a
        /// client packet describes a world the server has already left.
        bool AwaitingHandover() const { return m_authority == Authority::Seized && m_seizeAck != 0; }
        /// Should a movement packet from this unit's client be believed right now?
        bool TrustsClient() const { return m_authority == Authority::Client; }

        void Die();
        void CancelControl(Motion::Kind) {}
        void ExpireCombat() {}
        bool ReleaseControl(uint64) { return false; }
        bool HoldsControl(Motion::Kind) const { return false; }
        void RelocateSelected(float, float, float, float) {}

        /// An outside wipe of a unit's state -- a respawn, a revive -- drops every source
        /// of every reason and hands authority back.
        void Wipe();

        struct HeldView
        {
            Motion::Kind kind = Motion::Kind::Idle;
            bool selected = false;
            bool reachable = true;
            uint64 target = 0;
        };
        std::vector<HeldView> Held() const;

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
        /// The heading the travel direction gives at this instant, which is what the client
        /// shows: it takes the facing from the spline's tangent by itself.
        bool LiveFacing(float& out) const;
        bool IsMoving() const { return m_legRunning && m_movement.InFlight().Running(); }

        /// Turn on the spot, with no travel. One packet, no route.
        void FaceTo(float orientation);
        /// A raw leg at an explicit speed: what a script means by "move there this fast".
        void MoveAtSpeed(float x, float y, float z, float speed, bool routed);
        /// Stop where the mover stands, and forget the route.
        void Halt();

        // ---- what a late observer must be told, so it sees the same leg as everyone else.
        Move::Route const& InFlight() const { return m_movement.InFlight(); }
        uint32 SentFlags() const { return m_sentFlags; }
        uint32 SentDuration() const { return m_sentDuration; }
        uint32 SentId() const { return m_sentId; }
        Move::Facing const& SentFacing() const { return m_sentFacing; }

    private:
        /// Ask the selected behaviour, then send or refuse what it asked for.
        void Serve(bool legEnded, bool cut);

        /// Halt whatever is running because it just became forbidden.
        void Forbidden();

        Unit* m_unit;
        Move::Movement m_movement;
        /// What is forbidden, counted by source. The only copy there is.
        Motion::Mobility m_blocks;
        Authority m_authority = Authority::Server;
        /// The counter a seized client must echo, 0 when nothing is outstanding.
        uint32 m_seizeAck = 0;
        uint32 m_seizeNext = 1;
        /// A pursuit reads its target through this, and holds a reference for its life, so
        /// it outlives the call that made it. Raw rather than a smart pointer because the
        /// header must not need the definition to declare the member.
        class MoveSighting* m_sighting = 0;
        /// A charge's speed, carried across the one Serve call the request makes.
        float m_chargeSpeed = 0.0f;
        /// What the last leg actually went out as. An observer who arrives mid-leg is told
        /// this, so its client builds the same spline as everyone else's rather than a
        /// second description of the same motion that will drift from the first.
        uint32 m_sentFlags = 0;
        uint32 m_sentDuration = 0;
        uint32 m_sentId = 0;
        Move::Facing m_sentFacing;
        uint32 m_legEndsAt = 0;
        bool m_legRunning = false;
};
