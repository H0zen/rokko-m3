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

#ifndef MANGOS_MOVEMENTGENERATOR_H
#define MANGOS_MOVEMENTGENERATOR_H

#include "Platform/Define.h"
#include "Policies/Singleton.h"
#include "Dynamic/ObjectRegistry.h"
#include "Dynamic/FactoryHolder.h"
#include "MotionMaster.h"
#include "Timer.h"

class Unit;
class Creature;
class Player;

/**
 * @brief Base class for all movement generators
 *
 * Provides the interface for all movement generator implementations.
 * Movement generators control how units (players, creatures, NPCs) move
 * through the game world.
 */
class MovementGenerator
{
    public:
        /**
         * @brief Virtual destructor
         */
        virtual ~MovementGenerator();

        /**
         * @brief Initialize the movement generator
         *
         * Called before adding movement generator to motion stack.
         *
         * @param owner Reference to the unit
         */
        virtual void Initialize(Unit& owner) = 0;

        /**
         * @brief Finalize the movement generator
         *
         * Called after removing movement generator from motion stack.
         *
         * @param owner Reference to the unit
         */
        virtual void Finalize(Unit& owner) = 0;

        /**
         * @brief Interrupt the movement generator
         *
         * Called before losing top position (before pushing new
         * movement generator above).
         *
         * @param owner Reference to the unit
         */
        virtual void Interrupt(Unit& owner) = 0;

        /**
         * @brief Reset the movement generator
         *
         * Called after returning movement generator to top position
         * (after removing above movement generator).
         *
         * @param owner Reference to the unit
         */
        virtual void Reset(Unit& owner) = 0;

        /**
         * @brief Update the movement generator
         * @param owner Reference to the unit
         * @param time_diff Time difference in milliseconds
         * @return True if update successful, false otherwise
         */
        virtual bool Update(Unit& owner, const uint32& time_diff) = 0;

        /**
         * @brief Get the type of the movement generator
         * @return Movement generator type
         */
        virtual MovementGeneratorType GetMovementGeneratorType() const = 0;

        /**
         * @brief Called when the unit's speed changes
         */
        virtual void unitSpeedChanged() { }


        /**
         * @brief Get reset position for evade behavior
         *
         * Used by evade code to select point to evade with expected
         * restart of default movement.
         *
         * @param owner Reference to the unit
         * @param x X-coordinate output
         * @param y Y-coordinate output
         * @param z Z-coordinate output
         * @param o Orientation output
         * @return True if reset position obtained, false otherwise
         */
        virtual bool GetResetPosition(Unit& /*owner*/, float& /*x*/, float& /*y*/, float& /*z*/, float& /*o*/) const { return false; }

        /**
         * @brief Check if destination is reachable
         * @return True if reachable, false otherwise
         */
        virtual bool IsReachable() const { return true; }
};

/**
 * @brief SelectableMovement is a factory holder for movement generators.
 */
struct SelectableMovement : public FactoryHolder<MovementGenerator, MovementGeneratorType>
{
    /**
     * @brief Constructor for SelectableMovement.
     * @param mgt Type of the movement generator.
     */
    SelectableMovement(MovementGeneratorType mgt) : FactoryHolder<MovementGenerator, MovementGeneratorType>(mgt) {}
};

/**
 * @brief Template class for movement generator factories.
 * @tparam REAL_MOVEMENT Type of the real movement generator.
 */
template<class REAL_MOVEMENT>
struct MovementGeneratorFactory : public SelectableMovement
{
    /**
     * @brief Constructor for MovementGeneratorFactory.
     * @param mgt Type of the movement generator.
     */
    MovementGeneratorFactory(MovementGeneratorType mgt) : SelectableMovement(mgt) {}

    /**
     * @brief Creates a new movement generator.
     * @param data Pointer to the data.
     * @return Pointer to the created movement generator.
     */
    MovementGenerator* Create(void*) const override;
};

typedef FactoryHolder<MovementGenerator, MovementGeneratorType> MovementGeneratorCreator;
typedef FactoryHolder<MovementGenerator, MovementGeneratorType>::FactoryHolderRegistry MovementGeneratorRegistry;
typedef FactoryHolder<MovementGenerator, MovementGeneratorType>::FactoryHolderRepository MovementGeneratorRepository;

#endif // MANGOS_MOVEMENTGENERATOR_H
