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
#include "Restrictions.h"
#include "Timer.h"
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

        /// Adopt what this unit does when nothing else is asked of it, read from its spawn:
        /// a patrol, a wander, or standing still. StopAndDefault() is this with a Stop()
        /// in front of it.
        void UseDefault();
        /// Called from the unit's own update. Costs one comparison for a mover with nothing
        /// due, which is almost all of them almost all of the time: a creature walking a
        /// forty-yard leg has nothing to do for the eight seconds it takes.
        void Tick(uint32 diff);
        /// Stop, and stay stopped. Everything the unit was doing is dropped and nothing
        /// takes its place.
        void Stop();
        /// Stop, then go back to what this unit does when nothing else is asked of it --
        /// its patrol, its wander, or standing there. NOT the same as Finish(), which
        /// brings back whatever was suspended underneath: this one throws all of that away
        /// and starts again from the spawn's own default.
        ///
        /// It is what almost every caller wants, and the old Clear(reset = true) meant it
        /// and then did not do it -- the parameter was ignored, so a creature that finished
        /// a scripted move or left combat stood still for ever.
        void StopAndDefault();
        /// What was running has finished. ONLY the top goes: whatever was suspended
        /// beneath it becomes the answer again by itself, which is the whole reason a fear
        /// can interrupt a chase and the chase come back without anyone storing it.
        ///
        /// The old spelling took a `reset` flag asking whether the behaviour left exposed
        /// should start again from where the unit now stands. That is no longer a question:
        /// the component compares the layer that laid the route in flight against the one
        /// selected now, and asks a behaviour that did not lay it afresh, always.
        void Finish();

        /// Idle movement inside a leash. WHICH KIND is not the caller's to choose: a flier
        /// circles, anything else draws a reachable point and goes to it, and whether this
        /// creature flies is asked live -- a shapeshift or an aura changes the answer long
        /// after the spawn decided what it does by default. Water needs no case of its own:
        /// the draw already picks points in a volume for a swimmer.
        void Wander(float x, float y, float z, float radius);
        void GoHome();
        /// Hold a place beside another unit: `angle` radians round from ITS facing, at
        /// `distance` yards. The place turns with the target, which is what lets several
        /// followers keep a shape around one leader instead of stacking on the same spot.
        void Follow(Unit* target, float distance, float angle);
        /// Close on a target and stay within reach of it, from whatever side you are on.
        /// No angle: walking round an enemy to stand at some particular bearing would be a
        /// strange thing to do in a fight.
        void Chase(Unit* target, float distance = 0.0f);
        /// Stagger about on the spot, and keep staggering while any source says so. The
        /// source is the aura that imposed it -- which caster's which effect -- and it is
        /// not optional: two overlapping confusions have to be told apart, or the first to
        /// end frees a unit the second still holds.
        void Confused(uint64 source);
        /// Run away from something, and keep running while any source says so. Same rule
        /// as Confused: the source is the aura, and two fears need two releases.
        void FleeFrom(Unit* enemy, uint32 timeLimitMs, uint64 source);
        /// Walk to one place and report arriving there. `speed` of zero means this unit's
        /// own pace; anything else overrides it, which is what a script pushing something
        /// across a room at a fixed rate wants. A route that cannot be found is retried, not
        /// dropped in silence.
        void GoTo(uint32 id, float x, float y, float z, bool routed = true, float speed = 0.0f);
        void RunAskingHelp(float x, float y, float z);
        /// Reach a point through the air: no ground route, no floor. Whether that is a
        /// take-off or a landing is just whether the point is above or below, so nobody has
        /// to say which.
        void FlyTo(uint32 id, float x, float y, float z);
        void ChargeTarget(Unit* target, float speed);
        void ChargePoint(float x, float y, float z, float speed);
        bool JumpTo(float x, float y, float z, float speed, float apex, uint32 id = 0);
        void Fall();
        /// Walk the creature's waypoint path. No initial delay: every caller in the tree
        /// left it at zero, so it was a parameter describing a feature nobody used.
        void WalkPath(int32 pathId = 0, uint32 source = 0, uint32 overwriteEntry = 0);

        /// Fly a taxi route. The server takes the passenger over for the whole flight --
        /// TakeControl() -- and gives it back on landing, because for that span the client
        /// is not driving and must not be believed if it says otherwise.
        /// Answers false when the route has nothing to fly.
        bool FlyRoute(std::vector<uint32> const& route, uint32 startNode, float speed);
        bool PauseWaypoints(int32 ms);

        // ---- WHAT IS FORBIDDEN, counted by source. A reason holds while any source holds
        // it, so two roots from two casters need two releases. Read straight from here:
        // there is no published copy, which is the whole point of this class.
        void Forbid(Motion::Inhibition what, uint64 source);
        /// Release ONE source of a reason. If it was the last, whatever behaviour that
        /// reason was running ends here -- in this same call, so the reason and the thing it
        /// caused can never disagree. Two casters fearing the same target means two of these.
        void Allow(Motion::Inhibition what, uint64 source);
        /// Release EVERY source of a reason, whoever imposed it, and end what it was running.
        /// Possession is the case: it overrides whatever else held the unit.
        void AllowAll(Motion::Inhibition what);
        bool Forbids(Motion::Inhibition what) const { return m_blocks.Inhibited(what); }
        /// Release every source of a whole kind at once -- every seat, every aura -- which
        /// is what a death or a vehicle coming apart needs.
        void ReleaseAllFrom(Motion::SourceDomain domain);

        /// Every active reason as Reason bits: the counted inhibitions, plus the ones a held
        /// behaviour IS -- a fear is not a source anyone registered, it is a fear running.
        uint8 Reasons() const;
        bool MayMove() const { return (Reasons() & Motion::kCannotMoveReasons) == 0; }
        bool MayReact() const { return (Reasons() & Motion::kCannotReactReasons) == 0; }
        /// A Dead source that is not the death itself: a feign.
        bool Feigning() const;

        /// A speed changed under a route that was timed with the old one. The client was
        /// given a duration, not a speed, so the leg in flight still carries the old pace:
        /// it has to be laid again or the mover walks at a speed nobody asked for.
        void SpeedChanged();
        bool SetNextWaypoint(uint32 pointId);
        /// The node this patrol is walking towards. While it waits at a node, this is
        /// already the one after it.
        uint32 NextNode() const;
        /// The last node it actually arrived at. Only ever advances on an arrival, never on
        /// a leg that could not be built -- which is the whole reason the two are separate.
        uint32 ReachedNode() const;
        /// A line about the path for a GM to read: which one, where it is going, where it has been.
        void DescribeWalkPath(std::ostringstream& oss) const;
        /// Which path this creature is walking, and where that path was read from.
        bool CurrentWalkPath(int32& pathId, WaypointPathOrigin& origin) const;
        bool AddToSelectedPatrolPause(int32 ms);
        /// Where this unit is heading, when it is heading anywhere. False when it stands.
        bool Destination(Geometry::Vector3& out) const;

        // ---- WHO IS DRIVING
        Authority Driver() const { return m_authority; }

        /// THE SERVER TAKES A PLAYER OVER -- a taxi, a knockback, a fear, a root.
        ///
        /// Answers the counter the client must echo back. Until it does, the unit has two
        /// possible positions: the route the server is driving, and whatever the client is
        /// still describing from the world it has not yet been told it left. For that window
        /// the server's route is the truth and packets that contradict it are refused.
        uint32 TakeControl();

        /// The server hands a player back. Also answers a counter to be echoed, because the
        /// handback has to be acknowledged just as the seizure did -- until it is, a packet
        /// arriving might still belong to the seized window.
        uint32 GiveControl();

        /// An echo came back. Answers false for a counter that is not the one outstanding:
        /// a late reply to a handover that has already ended must not settle the current
        /// one. Only one is ever outstanding, so this covers both directions.
        bool Confirmed(uint32 counter);

        /// True while a handover is announced but unanswered -- the window in which a client
        /// packet may describe a world the server has already left.
        bool AwaitingHandover() const { return m_pending != 0; }
        /// Should a movement packet from this unit's client be believed right now?
        bool TrustsClient() const { return m_authority == Authority::Client && m_pending == 0; }

        /// Stop chasing. Combat ending is not something this component can see, so the
        /// side that does see it says so -- but what it asks for is a movement, not an
        /// announcement about combat.
        void StopChasing();

        /// A respawn or a revive: every source of every restriction is released and control
        /// goes back to whoever normally has it. Nothing that held this unit before it died
        /// still applies afterwards.
        void ReleaseEveryRestriction();

        /// What this unit is doing right now, in the game's own words. Idle when nothing.
        Motion::Kind Doing() const;
        bool IsChasing() const;
        Unit* ChaseTarget() const;
        bool IsFollowing() const;
        Unit* FollowTarget() const;
        bool IsPatrolling() const;
        bool IsOnTaxi() const { return (Reasons() & Motion::ReasonOnTaxi) != 0; }

        /// Where the mover is at this instant. While a route runs this is arithmetic over
        /// the polyline the client was sent -- the same arithmetic the client does -- so the
        /// two agree to the millisecond instead of to the last position write.
        bool PositionNow(Geometry::Vector3& out) const;
        /// The heading the travel direction gives at this instant, which is what the client
        /// shows: it takes the facing from the spline's tangent by itself.
        bool FacingNow(float& out) const;
        /// Is a route actually under way right now? Derived, not stored: the route knows
        /// its own points and its own end, and a second flag beside it would be one more
        /// pair to keep equal.
        bool IsMoving() const
        {
            Move::Route const& route = m_movement.InFlight();
            return route.Running() && !route.Arrived(getMSTime());
        }

        /// Turn on the spot, with no travel. One packet, no route.
        void FaceTo(float orientation);
        /// A raw leg at an explicit speed: what a script means by "move there this fast".
        /// Stop where the mover stands, and forget the route.
        /// Stop the route in flight and say so on the wire, but keep doing whatever this
        /// is -- a speed change that has to re-lay its leg, a relocation. Distinct from
        /// Stop(), which drops the doing as well.
        void StopRoute();

        // ---- what a late observer must be told, so it sees the same leg as everyone else.
        Move::Route const& InFlight() const { return m_movement.InFlight(); }
        uint32 SentFlags() const { return m_sentFlags; }
        uint32 SentDuration() const { return m_movement.InFlight().Duration(); }
        uint32 SentId() const { return m_sentId; }
        Move::Facing const& SentFacing() const { return m_sentFacing; }

    private:
        /// ONE STEP OF EVERYTHING. Ask whatever is running -- Ended() when the leg it laid
        /// has run out, Wake() when its own due time came -- perform the acts it asks the
        /// game for, write and send the leg it wants or refuse it, and relaunch the route
        /// from what actually went out. Every command ends here and so does every tick.
        void Advance(bool legEnded, bool cut);

        /// Stop whatever is running if it just became forbidden. Called from the one place
        /// that can make it so, which is why the reason and the stopping cannot drift apart.
        void StopIfForbidden();
        /// The reason has no sources left: stop whatever it was making the unit do.
        void EndWhatItWasRunning(Motion::Inhibition what);

        Unit* m_unit;
        Move::Movement m_movement;
        /// What is forbidden, counted by source. The only copy there is.
        Motion::Restrictions m_blocks;
        Authority m_authority = Authority::Server;
        /// The counter the client still owes an echo for, 0 when nothing is outstanding.
        uint32 m_pending = 0;
        uint32 m_nextCounter = 1;
        /// Which side the pending handover is moving control to.
        Authority m_pendingTo = Authority::Server;
        /// A pursuit reads its target through this, and holds a reference for its life, so
        /// it outlives the call that made it. Raw rather than a smart pointer because the
        /// header must not need the definition to declare the member.
        class MoveSighting* m_sighting = 0;
        /// What the last leg actually went out as. An observer who arrives mid-leg is told
        /// this, so its client builds the same spline as everyone else's rather than a
        /// second description of the same motion that will drift from the first.
        /// Which path the patrol walks, remembered because the GM commands ask and the shape
        /// itself is told only the nodes.
        int32 m_pathId = 0;
        WaypointPathOrigin m_pathOrigin = PATH_NO_PATH;
        uint32 m_sentFlags = 0;
        uint32 m_sentId = 0;
        Move::Facing m_sentFacing;
};
