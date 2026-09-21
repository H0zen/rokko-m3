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

// THE SHAPES MOVEMENT COMES IN.
//
// The old engine had fifteen classes. Sorted by what decides the destination and by what
// forces the server to act again, there are six, and the fifteen game names are labels on
// top of them. Each one answers the same two questions -- where next, and when should I be
// asked again -- so each is a small class and none of them is a system.
//
//   GoToPoint   one fixed place.                          Again: on arrival.
//   WalkNodes   a list known in advance.                  Again: at a node that stops.
//   Scatter     a draw inside a region.                   Again: on arrival, then a rest.
//   Pursue      another unit's live position.             Again: at the first moment the
//                                                         target CAN have drifted too far.
//   FleeFrom    away from a point.                        Again: on arrival.
//   Ballistic   physics, not a path.                      Again: on landing.
//   HoldStill   nowhere at all, for a set time.            Again: when the time is up.
//
// The one rule they all obey, and the one the old patrol broke: A FAILURE IS A RETRY. A
// route that cannot be found, a spot that cannot be reached, a destination the writer
// refuses -- none of them advances any state. The creature tries the same thing again in a
// moment. Treating a failure as an arrival is how a patrol ends up walking between a few
// scattered nodes with the rest abandoned.

#include "Move/Movement.h"

#include <cstdint>
#include <vector>

namespace Move
{
    /// One place, once. Covers a scripted point, a return home, a landing and a run to a
    /// friend -- everything whose destination is decided before the leg starts and does not
    /// change while it runs.
    class GoToPoint : public Behaviour
    {
        public:
            GoToPoint(Kind kind, const Vector3& to, uint32_t id = 0, bool walk = false)
                : m_kind(kind), m_to(to), m_id(id), m_walk(walk) {}

            Kind What() const override { return m_kind; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;

            /// Travel at an explicit pace rather than the mover's own. A charge is the case
            /// this exists for: the speed belongs to the request, and carrying it past the
            /// shape in a field on the caller was a crutch.
            void AtSpeed(float ydPerSec) { m_speed = ydPerSec; }

            bool Done() const { return m_done; }
            uint32_t Id() const { return m_id; }

        private:
            Kind m_kind;
            Vector3 m_to;
            uint32_t m_id;
            bool m_walk;
            float m_speed = 0.0f;   ///< 0 = the mover's own pace
            bool m_done = false;
            uint8_t m_failures = 0;
            std::vector<Vector3> m_points;
    };

    /// A list of places, in order, walked one run at a time. A run ends at the first node
    /// that has a reason to stop -- a wait, a script, an emote -- so a patrol with nothing
    /// to do at its nodes crosses many of them in one packet.
    class WalkNodes : public Behaviour
    {
        public:
            struct Node
            {
                uint32_t id = 0;
                Vector3 at;
                uint32_t waitMs = 0;
                bool stops = false;   ///< a script, an emote, a held facing: the run ends here
                float facing = 0.0f;
                bool hasFacing = false;
            };

            WalkNodes(std::vector<Node> nodes, bool loops = true, bool walk = true)
                : m_nodes(nodes), m_loops(loops), m_walk(walk) {}

            Kind What() const override { return Kind::Patrol; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;
            bool Anchor(Vector3& pos, float& facing) const override;

            /// The node being walked TO. Only ever advances when one is reached.
            uint32_t Heading() const { return m_target < m_nodes.size() ? m_nodes[m_target].id : 0; }
            uint32_t Reached() const { return m_reached; }
            /// How many nodes the run in flight covers. One is an ordinary leg; more means
            /// the client is walking several without the server hearing from it.
            size_t RunLength() const { return m_run.size(); }
            bool SetNext(uint32_t nodeId);
            /// Stand still for a while before carrying on -- a player struck up a
            /// conversation, a script asked. The node being walked to does not change: a
            /// pause is not a reason to give up on a destination.
            void WaitFor(uint32_t ms) { m_waitUntilMs = ms; }
            void Replace(std::vector<Node> nodes);

        private:
            size_t IndexOf(uint32_t nodeId) const;
            uint32_t Lay(uint32_t nowMs, World& world, Plan& out);
            /// Report every node the finished run covered, advance to the one after it, and
            /// answer how long the last of them asks the creature to wait. Shared by the
            /// arrival and by the case where the run covered nodes without walking any
            /// distance -- which is why it is a function and not part of either.
            uint32_t Reap(Plan& out);

            std::vector<Node> m_nodes;
            bool m_loops;
            bool m_walk;
            size_t m_target = 0;         ///< index of the node being walked to
            uint32_t m_reached = 0;      ///< id of the last node actually reached
            uint8_t m_failures = 0;      ///< consecutive failures on THIS node; never a skip
            uint32_t m_waitUntilMs = 0;  ///< an outside pause still owed, in milliseconds
            /// The run covered a node the creature was already standing on, so there is
            /// nothing to walk but the node counts as reached.
            bool m_reachedWithoutWalking = false;
            std::vector<Vector3> m_points;
            std::vector<size_t> m_run;   ///< node indices this leg covers, in order
    };

    /// A random point inside a leash, then a rest, then again. The confused stagger is the
    /// same shape with a tiny radius and no rest.
    class Scatter : public Behaviour
    {
        public:
            Scatter(Kind kind, const Vector3& centre, float radius,
                    uint32_t restMinMs, uint32_t restMaxMs, bool walk = true)
                : m_kind(kind), m_centre(centre), m_radius(radius),
                  m_restMin(restMinMs), m_restMax(restMaxMs), m_walk(walk) {}

            Kind What() const override { return m_kind; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;
            bool Anchor(Vector3& pos, float& facing) const override;

        private:
            Kind m_kind;
            Vector3 m_centre;
            float m_radius;
            uint32_t m_restMin;
            uint32_t m_restMax;
            bool m_walk;
            std::vector<Vector3> m_points;
    };

    /// A SEVENTH SHAPE, and the only one that is not a destination.
    ///
    /// A flier at rest does not pick a spot and walk to it -- it circles. The path is an
    /// inclined ellipse and it is deterministic: an angle that advances, not a draw. That is
    /// why it is its own shape rather than a flag on Scatter, which has no notion of a
    /// heading that continues.
    ///
    /// THE INCLINE IS NOT A PARAMETER, and the client is the reason. For a unit with the
    /// flying or swimming flag it sets the model's pitch to asin(tangent.z) on every
    /// evaluation (sub_A27380 in Wow.exe 15595), so the creature's nose follows the slope of
    /// the path whether that looks right or not. The steepest slope of this ellipse is
    /// asin(band / radius), and there is exactly one ratio that looks like a bird circling
    /// rather than one diving and pulling up -- so the band is a quarter of the radius and
    /// nobody is asked to choose it.
    ///
    /// That ratio also makes the shape scale-free: a circle of ten yards and one of a
    /// hundred are inclined by the same fourteen degrees, so a bat and a dragon look alike
    /// doing it.
    class Orbit : public Behaviour
    {
        public:
            Orbit(Kind kind, const Vector3& centre, float radius);

            Kind What() const override { return m_kind; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;
            bool Anchor(Vector3& pos, float& facing) const override;

            /// The steepest climb or dive this orbit can ask for, in radians. What the
            /// client will pitch the model to at the worst point of the circle.
            float SteepestPitch() const;

        private:
            Vector3 At(float angle) const;

            Kind m_kind;
            Vector3 m_centre;
            float m_radius;
            float m_verticalBand;
            float m_angle = 0.0f;
            std::vector<Vector3> m_points;
    };

    /// A FLIGHT PATH: a list of points known before it starts, flown at a fixed speed.
    ///
    /// It is WalkNodes with three differences, and the differences are the whole of it.
    /// The points are already in the air, so there is no routing and no floor to consult --
    /// asking the navmesh for a way between two zeppelin towers would fail, and succeeding
    /// would be worse. The speed is the path's, not the mover's. And it never loops: a
    /// flight ends, and what happens then is the game's business, not this shape's.
    ///
    /// Nothing is drawn or decided while it runs, so it is sent in as few packets as the
    /// packed reach allows and the passenger is left alone in between.
    class Flight : public Behaviour
    {
        public:
            struct Node
            {
                Vector3 at;
                uint32_t delayMs = 0;      ///< a pause at this node, from the path data
                uint32_t arriveEvent = 0;  ///< a DBC event to fire on reaching it
                uint32_t departEvent = 0;  ///< and one on leaving it
            };

            Flight(std::vector<Node> nodes, float speed)
                : m_nodes(nodes), m_speed(speed) {}

            Kind What() const override { return Kind::Taxi; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;

            /// How far along the route it is, in nodes. What the game needs to know when a
            /// flight crosses to another map and has to be picked up again.
            size_t NodesDone() const { return m_at; }
            bool Landed() const { return m_landed; }

        private:
            std::vector<Node> m_nodes;
            float m_speed;
            size_t m_at = 0;           ///< the next node to fly to
            bool m_landed = false;
            std::vector<Vector3> m_points;
            std::vector<size_t> m_run;  ///< the nodes this leg covers, in order
    };

    /// STANDS THERE, ON PURPOSE, FOR A WHILE.
    ///
    /// The only shape that never moves anything and never sends a packet. It exists because
    /// "stopped" and "stopped for six seconds" are different facts: a unit that is merely
    /// stopped has nothing to bring it back, so a distract built out of a plain stop froze
    /// the creature for good. This one holds its layer, which suspends what was running
    /// beneath it, and then ends by itself -- and the thing underneath becomes the answer
    /// again, which is the entire point of it.
    class HoldStill : public Behaviour
    {
        public:
            HoldStill(Kind kind, uint32_t ms) : m_kind(kind), m_ms(ms) {}

            Kind What() const override { return m_kind; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;

        private:
            Kind m_kind;
            uint32_t m_ms;
            uint32_t m_untilMs = 0;
            bool m_started = false;   ///< not "m_untilMs != 0": the clock wraps through zero
    };

    /// What the server knows about the thing being chased. A creature's position is exact,
    /// because the server holds its route; a player's is whatever it last reported, and the
    /// age of that report is the whole of the uncertainty.
    struct Quarry
    {
        bool known = false;
        Vector3 at;
        uint32_t reportedAtMs = 0;   ///< the server clock when `at` was true
        float facing = 0.0f;         ///< which way it was looking: a slot is defined against this
        float topSpeed = 7.0f;       ///< the fastest it could be travelling
        float reach = 0.0f;          ///< its bounding radius, added to the stop distance
    };

    /// Where a pursuer reads its target from. Separate from World because only pursuit
    /// needs it, and because a test wants to move a target without a map.
    class Sighting
    {
        public:
            virtual ~Sighting() {}
            virtual Quarry Look(uint64_t rawGuid) const = 0;
    };

    /// Close to a distance and stay there, without ever asking where the target is on a
    /// timer. The design's one non-obvious piece: instead of polling, work out the EARLIEST
    /// moment the target can have drifted further than we tolerate from the place we aimed
    /// at, and sleep until then. The bound cannot be wrong however the target moves, so a
    /// target that moves never has to notify anybody.
    ///
    /// TWO MEANINGS, and the difference is whether there is a slot.
    ///
    /// A CHASE closes on the target from wherever it happens to be. The place to stand is
    /// "near enough", anywhere on the circle, so it aims along the line between them and
    /// stops short. Walking round to some particular side of an enemy would be absurd.
    ///
    /// A FOLLOW holds a PLACE, and the place is defined against the target's own facing: a
    /// pet at a hundred and fifty-seven degrees is behind its owner's left shoulder, and
    /// stays there when the owner turns on the spot. That is what lets several followers
    /// hold a shape around one leader instead of all converging on the same spot -- which is
    /// exactly what they did while this was a line.
    class Pursue : public Behaviour
    {
        public:
            /// A chase: no slot, close from wherever you are.
            Pursue(Kind kind, uint64_t target, Sighting& sighting, float stopAt, float slack = 1.0f)
                : m_kind(kind), m_target(target), m_sighting(&sighting),
                  m_stopAt(stopAt), m_slack(slack) {}

            /// A follow: hold `angle` radians round from the target's own facing, at
            /// `stopAt` yards. The slot turns with the target.
            void HoldSlot(float angle) { m_angle = angle; m_hasSlot = true; }

            Kind What() const override { return m_kind; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;

            uint64_t Target() const { return m_target; }
            /// The place the last leg aimed at. What drift is measured against.
            const Vector3& AimedAt() const { return m_aim; }
            /// The bound itself, exposed so a test can check the arithmetic rather than the
            /// behaviour that uses it: the moment the target can first be `slack` yards from
            /// where we aimed, measured from the report, not from now.
            static uint32_t DriftDeadline(const Quarry& quarry, float slack);

        private:
            Kind m_kind;
            uint64_t m_target;
            Sighting* m_sighting;
            float m_stopAt;
            float m_slack;
            float m_angle = 0.0f;   ///< where round the target to stand, from its facing
            bool m_hasSlot = false; ///< a follow holds a place; a chase only holds a distance
            Vector3 m_aim;
            bool m_aimed = false;
            std::vector<Vector3> m_points;
    };

    /// Away from a point, as far as one leg goes. The direction is the only thing that
    /// makes this different from Scatter, so it is the only thing it computes.
    class FleeFrom : public Behaviour
    {
        public:
            FleeFrom(Kind kind, const Vector3& from, float distance)
                : m_kind(kind), m_from(from), m_distance(distance) {}

            Kind What() const override { return m_kind; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;

            void Source(const Vector3& from) { m_from = from; }

        private:
            Kind m_kind;
            Vector3 m_from;
            float m_distance;
            std::vector<Vector3> m_points;
    };

    /// A jump, a knockback or a fall. Not a path: the client computes the arc itself from
    /// an acceleration and a start time, so the server sends two points and the parameters.
    class Ballistic : public Behaviour
    {
        public:
            Ballistic(const Vector3& to, float speed, float height)
                : m_to(to), m_speed(speed), m_height(height) {}

            Kind What() const override { return Kind::Effect; }
            uint32_t Decide(uint32_t nowMs, bool restart, World& world, Plan& out) override;
            uint32_t Arrived(uint32_t nowMs, bool cut, World& world, Plan& out) override;

            bool Landed() const { return m_landed; }

        private:
            Vector3 m_to;
            float m_speed;
            float m_height;
            bool m_landed = false;
            std::vector<Vector3> m_points;
    };
}
