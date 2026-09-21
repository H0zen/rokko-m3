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

// ONE MOVER'S MOVEMENT, START TO FINISH.
//
// The fact this is built on is the client's: once a polyline is on the wire, the client
// walks it alone. It does not report progress, it does not ask for more, and it will not
// be contacted again until the route it was given runs out. A server that visits every creature every world
// tick is therefore simulating something that is already happening somewhere else -- and
// simulating it wrongly, because the client re-times every spline it receives against a
// ceiling of its own (ClientRules::Retime).
//
// So a Movement is not ticked. It says WHEN it next has something to do, and the map comes
// back then. A creature walking a forty-yard route has nothing to do for eight seconds and
// costs nothing during them. A creature standing at a waypoint costs nothing until its
// delay runs out. What wakes a mover early is an event -- it was attacked, it was feared,
// its target moved -- and an event is a call, not a poll.
//
// WHAT IS HERE AND WHAT IS NOT. A Movement owns the arbitration (Selection), the route in
// flight (Route), its own next-wake time and the small amount of state a behaviour
// needs between wake-ups. It owns no unit, no map, no packet: everything it needs from the
// world arrives through World, and everything it wants done leaves as a Plan. That is
// what makes it testable without a server, and it is the whole of the rule -- there is no
// second kind of state kept somewhere else, no pending-change queue, no transaction.

#include "Move/Route.h"
#include "Move/Selection.h"
#include "Geometry/Vector3.h"

#include <cstdint>
#include <vector>

namespace Move
{
    using Geometry::Vector3;

    /// The kinds of movement, in the order of the priority they hold. The Layer a kind
    /// occupies is a property of the kind, so it is written down once, here, rather than
    /// carried in every request.
    enum class Kind : uint8_t
    {
        Idle,       ///< stands where it is
        Wander,     ///< hops inside a leash
        Patrol,     ///< walks a list of nodes
        Follow,     ///< keeps a bearing and a distance from another unit
        Chase,      ///< closes to melee and stays there
        Point,      ///< goes to one place and reports
        FlyLand,    ///< descends to the floor
        Home,       ///< returns to where it started
        AssistRun,  ///< runs to a friend
        Distract,   ///< holds a facing for a while
        Fear,       ///< runs away from a point
        Confused,   ///< staggers at random
        Effect,     ///< a jump, a knockback, a fall
        Taxi,       ///< a flight path
        Count
    };

    Layer LayerOf(Kind kind);
    Policy PolicyOf(Kind kind);

    /// How a route ends its orientation. While a spline runs the client faces the direction
    /// of travel by itself, so this only decides what happens at the end of one.
    struct Facing
    {
        enum class Mode : uint8_t { Travel, Angle, Spot, Unit };
        Mode mode = Mode::Travel;
        float angle = 0.0f;
        Vector3 spot;
        uint64_t unit = 0;

        static Facing Along() { return Facing(); }
        static Facing At(float radians) { Facing f; f.mode = Mode::Angle; f.angle = radians; return f; }
        static Facing Toward(const Vector3& p) { Facing f; f.mode = Mode::Spot; f.spot = p; return f; }
        static Facing Upon(uint64_t rawGuid) { Facing f; f.mode = Mode::Unit; f.unit = rawGuid; return f; }
    };

    enum Gait : uint32_t
    {
        GAIT_RUN      = 0x00,
        GAIT_WALK     = 0x01,
        GAIT_FLY      = 0x02,
        GAIT_STRAIGHT = 0x04,  ///< do not route, go directly
        GAIT_EXACT    = 0x08,  ///< arrive at the goal even if the router will not reach it
        GAIT_ROUTED   = 0x10,  ///< refuse the route unless the router found real world
        GAIT_CURVED   = 0x20   ///< a Catmull-Rom path: not packed, so not bound by PACK_REACH
    };

    /// The things a behaviour can ask the game to do or to hear about. Deliberately few:
    /// anything richer belongs on the game's side of the wall, where the vocabulary lives.
    enum ActCode : uint8_t
    {
        ACT_ARRIVED = 1,      ///< the destination was reached; `id` is the caller's own id
        ACT_NODE_REACHED,     ///< a waypoint was passed; `id` is the node
        ACT_TARGET_LOST,      ///< the unit being pursued can no longer be found
        ACT_LANDED,           ///< a jump, knockback or fall ended
        ACT_SCRIPT,           ///< run the script named by `id`
        ACT_EMOTE,
        ACT_SPELL,
        ACT_DISPLAY,
        ACT_SAY
    };

    /// What a Movement wants done. The map performs it and nothing else; a Plan that asks
    /// for nothing is the normal answer, because most wake-ups only move state along.
    struct Plan
    {
        bool send = false;        ///< put `points` on the wire as a spline
        bool halt = false;        ///< stop the route in flight where it stands
        const Vector3* points = nullptr;
        uint16_t count = 0;
        float speed = 0.0f;       ///< 0 = the mover's own pace for the gait
        uint32_t gait = GAIT_RUN;
        Facing facing;
        /// Things for the game to do at this instant, in order: a script, an emote, a
        /// spell, a line of text, an AI hook. Opaque here -- a Movement never learns what
        /// any of them mean, which is what keeps the game's vocabulary out of it.
        struct Act { uint8_t what; uint32_t id; uint32_t extra; };
        std::vector<Act> acts;
    };

    /// What a Movement may ask of the world, when it wakes and only then. Every call is a
    /// query the map can answer without knowing which behaviour asked.
    class World
    {
        public:
            virtual ~World() {}
            /// A walkable polyline from one point to another. False when there is no way.
            /// `points` is filled with at least two points when it answers true.
            virtual bool Route(const Vector3& from, const Vector3& to,
                               std::vector<Vector3>& points) = 0;
            /// The floor under a point.
            virtual bool Floor(const Vector3& at, float& z) = 0;
            /// A reachable random point within `radius` of a centre.
            virtual bool Scatter(const Vector3& centre, float radius, Vector3& out) = 0;
            virtual float Frand(float lo, float hi) = 0;
            virtual uint32_t Urand(uint32_t lo, uint32_t hi) = 0;
            /// The mover's own pace for a gait, in yards per second.
            virtual float Pace(uint32_t gait) const = 0;
            /// Where the mover is right now, as the server believes it.
            virtual Vector3 Here() const = 0;
            virtual float Heading() const = 0;
    };

    /// A behaviour: one wake-up, one answer. There is no activate, no suspend, no resume
    /// and no finish recipe -- a behaviour that loses its slot is simply not woken again,
    /// and one that regains it is woken with `restart` set. Everything the old model spread
    /// across six virtuals is one call and one bool.
    class Behaviour
    {
        public:
            virtual ~Behaviour() {}
            virtual Kind What() const = 0;
            /// Decide. `nowMs` is the map's clock, `restart` means this behaviour was not
            /// the one that laid the route in flight. Answers the instant it wants to be
            /// woken next, or 0 for "only when something happens".
            virtual uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) = 0;
            /// The route laid for this behaviour has ended. Answers as Decide does.
            virtual uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) = 0;
            /// The place this behaviour would send a creature home to, when it has one.
            virtual bool Anchor(Vector3& /*pos*/, float& /*facing*/) const { return false; }
    };

    /// ONE MOVER. Holds which behaviours are held, which of them is running, and the route
    /// that behaviour laid. The map calls Wake when the clock reaches DueAt, and Ended
    /// when the route it sent finishes; nothing else drives it.
    class Movement
    {
        public:
            Movement();
            ~Movement();

            /// Put a behaviour on the layer its kind names, applying that kind's policy to
            /// everything beneath. Whatever shared the layer is replaced.
            void Take(Behaviour* behaviour);
            /// The behaviour of this kind ended by itself; anything suspended beneath it
            /// becomes the answer again and is woken with `restart`.
            void Drop(Kind kind);
            void Clear();

            /// The instant the map should come back, or 0 for "only when something
            /// happens to this mover".
            uint32_t DueAt() const { return m_due; }

            /// The clock reached DueAt, or an event made this mover interesting again.
            uint32_t Wake(uint32_t nowMs, World& world, Plan& out);
            /// The route this Movement sent has finished; `cut` when something ended it early.
            uint32_t Ended(uint32_t nowMs, bool cut, World& world, Plan& out);

            Route& InFlight() { return m_route; }
            const Route& InFlight() const { return m_route; }

            bool Running(Kind& out) const;
            Behaviour* Held(Kind kind) const;
            /// The behaviour that is selected right now, or null when none is held.
            Behaviour* Selected() const;

        private:
            uint32_t Ask(uint32_t nowMs, bool restart, bool ended, bool cut,
                         World& world, Plan& out);

            Selection m_who;
            Behaviour* m_behaviour[uint8_t(Layer::Count)];
            Route m_route;
            uint32_t m_due = 0;
            uint8_t m_routeLayer = uint8_t(Layer::Count);  ///< which layer laid the route in flight
    };
}
