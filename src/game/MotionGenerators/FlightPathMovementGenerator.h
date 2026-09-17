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

#ifndef MANGOS_FLIGHTPATHMOVEMENTGENERATOR_H
#define MANGOS_FLIGHTPATHMOVEMENTGENERATOR_H

/** @page PathMovementGenerator is used to generate movement
 * along flight paths, serving the purpose of generating
 * activities so that it generates updated packets for the
 * players. Waypoint movement is a native kernel behaviour now
 * (Motion::PatrolBehaviour, src/motion/DefaultMoves.h).
 */

#include "MovementGenerator.h"
#include "DBCStructure.h"

/**
 * @brief Base class for path movement generators
 *
 * Provides common functionality for path-based movement.
 *
 * @tparam T Type of the unit (Player or Creature)
 * @tparam P Type of the path
 */
template<class T, class P>
class PathMovementBase
{
    public:
        /**
         * @brief Constructor
         */
        PathMovementBase() : i_path(nullptr), i_currentNode(0) {}

        /**
         * @brief Virtual destructor
         */
        virtual ~PathMovementBase() {};

        /**
         * @brief Load path for the unit
         * @param unit Reference to the unit
         */
        void LoadPath(T&);

        /**
         * @brief Get current node in the path
         * @return Current node index
         */
        uint32 GetCurrentNode() const { return i_currentNode; }

    protected:
        P i_path; ///< Path for the movement
        uint32 i_currentNode; ///< Current node in the path
};

/**
 * @brief Flight path movement generator for players
 *
 * Generates movement of the player along taxi flight paths.
 * Handles ground and activities for the player during flight.
 */
// Derives from MovementGenerator directly, like every other generator in this
// tree. It used to go through MovementGeneratorMedium<Player, ...>, a CRTP
// forwarder that overrode Initialize(Unit&) and re-dispatched to a typed
// Initialize(Player&) here -- which meant this class hid the base's virtual at
// six methods, and did the Unit-to-Player conversion with an unchecked C-style
// cast whose accompanying AssertIsType<T>() was commented out.
//
// Every sibling had already been migrated off that template; this was the last
// user of it, and the only source of 84 of the tree's 98 -Woverloaded-virtual
// warnings. The downcast is now explicit and in one place per method.
class FlightPathMovementGenerator
    : public MovementGenerator,
  public PathMovementBase<Player, TaxiPathNodeList const*>
{
    public:
        /**
         * @brief Constructor
         * @param pathnodes Reference to path nodes
         * @param startNode Starting node index
         */
        explicit FlightPathMovementGenerator(TaxiPathNodeList const& pathnodes, uint32 startNode = 0)
        {
            i_path = &pathnodes;
            i_currentNode = startNode;
        }

        /**
         * @brief Initialize the movement generator
         * @param player Reference to the player
         */
        void Initialize(Unit& u) override;

        /**
         * @brief Finalize the movement generator
         * @param player Reference to the player
         */
        void Finalize(Unit& u) override;

        /**
         * @brief Interrupt the movement generator
         * @param player Reference to the player
         */
        void Interrupt(Unit& u) override;

        /**
         * @brief Reset the movement generator
         * @param player Reference to the player
         */
        void Reset(Unit& u) override;

        /**
         * @brief Update the movement generator
         * @param player Reference to the player
         * @param diff Time difference in milliseconds
         * @return True if update successful
         */
        bool Update(Unit& u, const uint32& diff) override;

        /**
         * @brief Get movement generator type
         * @return FLIGHT_MOTION_TYPE
         */
        MovementGeneratorType GetMovementGeneratorType() const override { return FLIGHT_MOTION_TYPE; }

        /**
         * @brief Get the flight path
         * @return Reference to path nodes
         */
        TaxiPathNodeList const& GetPath() { return *i_path; }

        /**
         * @brief Get node index at map end
         * @return Node index at map end
         */
        uint32 GetPathAtMapEnd() const;

        /**
         * @brief Check if player has arrived at destination
         * @return True if arrived
         */
        bool HasArrived() const { return (i_currentNode >= i_path->size()); }

        /**
         * @brief Set current node after teleport
         */
        void SetCurrentNodeAfterTeleport();

        /**
         * @brief Skip current node
         */
        void SkipCurrentNode() { ++i_currentNode; }
        void DoEventIfAny(Player& player, TaxiPathNodeEntry const& node, bool departure);

        /**
         * @brief Get reset position for evade
         * @param player Reference to the player
         * @param x X-coordinate output
         * @param y Y-coordinate output
         * @param z Z-coordinate output
         * @param o Orientation output
         * @return True if reset position obtained
         */
        bool GetResetPosition(Unit& u, float& x, float& y, float& z, float& o) const override;
};

#endif // MANGOS_FLIGHTPATHMOVEMENTGENERATOR_H
