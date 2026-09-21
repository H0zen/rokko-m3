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

// THE MAP, AS A BEHAVIOUR SEES IT.
//
// Move::World is the whole of what the movement library may ask of the game: a route, a
// floor, a scattered point, two random numbers, a pace and where the mover is. This is the
// one implementation of it, and it is the only file in the library's way that knows what a
// Unit is.
//
// It exists so the six behaviours can be tested against numbers instead of a server, and
// so that the answer to "what does movement depend on" is this list and nothing else.

#include "Move/Movement.h"
#include "Move/Behaviours.h"

class Unit;

/// Answers a behaviour's questions about one unit's surroundings.
class MoveWorld : public Move::World
{
    public:
        explicit MoveWorld(Unit& unit) : m_unit(unit) {}

        bool Route(const Geometry::Vector3& from, const Geometry::Vector3& to,
                   std::vector<Geometry::Vector3>& points) override;
        bool Floor(const Geometry::Vector3& at, float& z) override;
        bool Scatter(const Geometry::Vector3& centre, float radius,
                     Geometry::Vector3& out) override;
        float Frand(float lo, float hi) override;
        uint32_t Urand(uint32_t lo, uint32_t hi) override;
        float Pace(uint32_t gait) const override;
        Geometry::Vector3 Here() const override;
        float Heading() const override;

    private:
        Unit& m_unit;
};

/// Where a pursuit reads its target. A creature the server drives is known exactly, because
/// the server holds its route; a player is known only as of its last report, and the age of
/// that report is the entire uncertainty in the chase.
class MoveSighting : public Move::Sighting
{
    public:
        explicit MoveSighting(Unit& unit) : m_unit(unit) {}
        Move::Quarry Look(uint64_t rawGuid) const override;

    private:
        Unit& m_unit;
};
