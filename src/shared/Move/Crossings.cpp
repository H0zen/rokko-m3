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

#include "Move/Crossings.h"

#include <algorithm>
#include <cmath>

namespace Move
{
    namespace
    {
        /// Two crossings this close in arc length are the same event: a path cutting a
        /// corner passes both an x and a y boundary at once, and the mover enters one new
        /// cell, not two. Well under a millimetre, so nothing real is merged away.
        const float SAME_CROSSING = 0.0005f;

        /// Boundaries of one axis cut by a segment, as fractions of the segment.
        ///
        /// Travelling up, a boundary at integer m is reached when the continuous
        /// coordinate reaches m; travelling down, the cell is left the instant it drops
        /// BELOW m, so the boundaries that matter are the ones at or under where we
        /// started. Getting that asymmetry wrong is an off-by-one that only shows up when
        /// a mover walks west.
        void AxisCuts(double from, double to, std::vector<float>& into)
        {
            const double lo = std::floor(from);
            const double hi = std::floor(to);
            if (lo == hi)
            {
                return;
            }

            const double span = to - from;
            if (span == 0.0)
            {
                return;
            }

            if (span > 0.0)
            {
                for (double m = lo + 1.0; m <= hi; m += 1.0)
                {
                    into.push_back(float((m - from) / span));
                }
            }
            else
            {
                for (double m = lo; m > hi; m -= 1.0)
                {
                    into.push_back(float((m - from) / span));
                }
            }
        }
    }

    void CellCrossings(const Geometry::Vector3* points, uint16_t count, const Grid& grid,
                       std::vector<float>& out)
    {
        out.clear();
        if (!points || count < 2 || !(grid.size > 0.0f))
        {
            return;
        }

        std::vector<float> cuts;
        float travelled = 0.0f;

        for (uint16_t i = 0; i + 1 < count; ++i)
        {
            const Geometry::Vector3& a = points[i];
            const Geometry::Vector3& b = points[i + 1];
            if (!a.isFinite() || !b.isFinite())
            {
                out.clear();
                return;
            }

            const float segment = (b - a).magnitude();
            if (!(segment > 0.0f))
            {
                continue;
            }

            cuts.clear();
            AxisCuts(grid.Continuous(a.x), grid.Continuous(b.x), cuts);
            AxisCuts(grid.Continuous(a.y), grid.Continuous(b.y), cuts);

            // Within a segment the two axes' cuts interleave in no particular order, and
            // the caller is owed one ascending list over the whole path.
            std::sort(cuts.begin(), cuts.end());
            for (size_t k = 0; k < cuts.size(); ++k)
            {
                const float at = travelled + cuts[k] * segment;
                if (!out.empty() && at - out.back() < SAME_CROSSING)
                {
                    continue;
                }
                out.push_back(at);
                if (out.size() >= MAX_CROSSINGS)
                {
                    return;
                }
            }

            travelled += segment;
        }
    }
}
