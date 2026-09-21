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

// WHERE A PATH CHANGES CELL, computed once instead of watched for.
//
// A mover's position is cheap to ask for (Leg::At) but changing cell is not: it moves the
// object between grid containers and wakes every observer around it. Today that is found
// by comparing the cell every tick for every mover, which is why the position is only
// written a few times a second in the first place -- the comparison is the cost, not the
// arithmetic.
//
// It does not have to be watched for. The whole path is known the moment the leg is laid,
// so the distances at which it crosses a cell boundary are known then too: it is a segment
// against a set of evenly spaced planes, a division per crossing. Feed each distance to
// Leg::TimeAtDistance and the tick has a due time to wait for rather than a question to
// keep asking. A leg that stays inside one cell -- most of them -- schedules nothing at
// all and costs nothing per tick.
//
// The grid is passed in rather than included. This file must not know what a Map is.

#include "Geometry/Vector3.h"

#include <cstdint>
#include <vector>

namespace Move
{
    /// The server's cell grid, as GridDefines.h computes it, restated as numbers so this
    /// header depends on nothing. `ComputeCellPair` takes an index with
    ///     int((coord - size/2) / size + centre + 0.5)
    /// so in the continuous coordinate below a boundary is exactly an integer.
    struct Grid
    {
        float size;    ///< SIZE_OF_GRID_CELL
        int centre;    ///< CENTER_GRID_CELL_ID

        Grid(float cellSize, int centreIndex) : size(cellSize), centre(centreIndex) {}

        /// The continuous position along one axis, in cells. Its floor is the cell index,
        /// so the distance to the next boundary is the distance to the next integer.
        double Continuous(float coord) const
        {
            return (double(coord) - double(size) * 0.5) / double(size) + double(centre) + 0.5;
        }

        /// The cell index of one axis, identical to what ComputeCellPair would produce.
        int Index(float coord) const { return int(Continuous(coord)); }
    };

    /// Ceiling on how many crossings one leg may report. A cell is a third of a hundred
    /// yards and the longest route the server builds is a few hundred, so a real path
    /// yields a handful; anything near this ceiling is a path built from nonsense, and
    /// truncating is better than filling a scheduler from one bad leg.
    const size_t MAX_CROSSINGS = 256;

    /// Distances along the polyline, ascending and deduplicated, at which it enters a new
    /// grid cell. Empty when the whole path stays in one -- the common case, and the one
    /// worth being cheap.
    ///
    /// A path is a chain of straight segments whatever curve the client draws through it,
    /// because the cell a thing is in is decided by the point, and the point is what we
    /// interpolate. Both axes are tested; a corner cut diagonally reports one crossing,
    /// not two, because entering a new cell is one event however many axes changed.
    void CellCrossings(const Geometry::Vector3* points, uint16_t count, const Grid& grid,
                       std::vector<float>& out);
}
