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

#ifndef MANGOS_MOTION_TAXIMOVE_H
#define MANGOS_MOTION_TAXIMOVE_H

#include "BehaviourModel.h"

#include <vector>

// The taxi flight (P5-B family 5): TaxiBehaviour replaces FlightPathMovementGenerator, the last
// legacy generator, deleted on this branch with LegacyBehaviour and MovementGenerator. It owns
// the whole journey the shell hands it as one welded node array: one spline per map through the
// driver, the nodes' events, the route's seams, the map crossings and the landing, all at the
// server's clock. It never waits on a client packet. The shell's six taxi operations
// (Player::Taxi*) are asked for by name through the effects; nothing else of the shell is known.

namespace Motion
{
    /// One welded route flown as one native: the hops' rows in order, the seam node once.
    class TaxiBehaviour : public Behaviour
    {
        public:
            struct Node
            {
                uint32  mapId = 0;           ///< TaxiPathNode.ContinentID: a change of map cuts the leg
                Vector3 pos;                 ///< world
                uint32  arrivalEvent = 0;    ///< fired when the node is reached
                uint32  departureEvent = 0;  ///< fired when the node is left (never at a seam, never at the leg's last node)
                bool    seam = false;        ///< the incoming hop's last row: the route advances when it is left
            };
            struct Params
            {
                std::vector<Node> nodes;
                uint32  startNode = 0;         ///< the first node flown to (the closest segment on a resume)
                float   speed = 30.0f;         ///< Movement.TaxiSpeed, yd/s
                uint32  mountDisplayId = 0;    ///< written at the takeoff, without UNIT_FLAG_MOUNT
                bool    hasLanding = false;    ///< the destination TaxiNodes row carries a position
                Vector3 landing;               ///< that position (world): the landing teleports onto it
            };
            explicit TaxiBehaviour(Params const& p);
            Motion::Kind Kind() const override { return Motion::Kind::Taxi; }
            /// The takeoff (the shell's stop, revoke, mount and flags) and the first map's leg.
            Step Activate(Sight const& sight, Services& svc) override;
            Step Suspend() override;   ///< inert: the arbiter never masks a taxi; the generator's Interrupt was empty
            /// reset: after a confirmed crossing (CrossingLandedOn), the next map's leg from the node
            /// after the one the teleport landed on (the generator's SkipCurrentNode), the seam fired
            /// first when that node is a hub; an unconfirmed reset while crossing holds where the mover
            /// is; otherwise the current leg re-laid (the generator's Reset). No takeoff either way.
            Step Resume(Sight const& sight, Services& svc, bool reset) override;
            Step Tick(Sight const& sight, Services& svc, uint32 diff) override;
            FinishReason EndReason(Sight const& sight) const override;   ///< Arrived, or the cut/refused leg's own reason
            /// Arrived: the landing (snapped onto the TaxiNodes position when there is one); any
            /// other reason: the abort, and the interrupt for a displacing one.
            Outcome Finish(FinishReason why, Sight const& sight, Services& svc) override;
            bool ResetPosition(Sight const& sight, Services& svc, Vector3& pos, float& o) const override;

            /// The worldport ack (MotionMaster::TaxiContinue): true when a crossing is pending and
            /// this is the map it aimed at; the confirmation is latched so the resume that follows
            /// lays the next leg. An unconfirmed reset while crossing holds where the mover is.
            bool CrossingLandedOn(uint32 mapId);
            size_t CurrentNode() const { return m_node; }   ///< the node the mover has reached (the tests, the GM dump)
            bool Crossing() const { return m_crossing; }    ///< waiting for the worldport ack

        private:
            /// The leg from the current node to the map's end: the leading slot (a copy of the
            /// first node, which the launch overwrites with the mover's real position) and the
            /// nodes; resetLeg, and the Move unless there is nothing to fly.
            Step LayLeg();
            /// The first node on another map at or after `from`, else the end (the generator's GetPathAtMapEnd).
            size_t LegEnd(size_t from) const;
            /// The generator's Update loop: from the node reached so far up to `target`, the
            /// departure of each node left (never a seam's), the seam, the arrival of each node reached.
            void EventsUpTo(size_t target, std::vector<Effect>& effects);

            Params      m_p;
            size_t      m_node;        ///< the node the mover has reached (the generator's i_currentNode)
            size_t      m_legStart;    ///< the running leg's first node
            size_t      m_legEnd;      ///< one past its last node
            PointsArray m_legPoints;   ///< the leg's geometry: stable for the leg's life (the intent points at it)
            bool        m_crossing;    ///< the map's leg arrived short of the route's end; the teleport is the shell's
            bool        m_crossingConfirmed;   ///< CrossingLandedOn said yes since the crossing was emitted
    };
}

#endif
