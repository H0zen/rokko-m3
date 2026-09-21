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

#include "TestHarness.h"

#include "Move/Crossings.h"
#include "Move/Route.h"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using Geometry::Vector3;
using Move::CellCrossings;
using Move::Grid;
using Move::Route;

namespace
{
    /// The server's own grid, from GridDefines.h: SIZE_OF_GRIDS 533.33333 over
    /// MAX_NUMBER_OF_CELLS 16, centred on CENTER_GRID_CELL_ID.
    const float CELL = 533.33333f / 16.0f;
    const int CENTRE = 16 * 64 / 2;

    Grid TheGrid() { return Grid(CELL, CENTRE); }

    /// The x of the boundary `m` cells above the centre, so a test can sit either side of
    /// a real boundary without hard-coding a magic coordinate.
    float BoundaryAt(int cellsFromCentre)
    {
        return float((double(cellsFromCentre) - 0.5) * double(CELL) + double(CELL) * 0.5);
    }

    std::vector<float> Crossings(const std::vector<Vector3>& pts)
    {
        std::vector<float> out;
        CellCrossings(pts.data(), uint16_t(pts.size()), TheGrid(), out);
        return out;
    }

    bool Near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) <= tol; }
}

TEST(GridIndexMatchesTheServersOwnCellFormula)
{
    const Grid g = TheGrid();

    // int((coord - size/2)/size + centre + 0.5), spelled out, is what ComputeCellPair does.
    const float probes[5] = {0.0f, 100.0f, -100.0f, 5000.0f, -5000.0f};
    for (int i = 0; i < 5; ++i)
    {
        const double off = (double(probes[i]) - double(CELL) * 0.5) / double(CELL);
        CHECK_EQ(g.Index(probes[i]), int(off + CENTRE + 0.5));
    }
}

TEST(APathInsideOneCellCrossesNothing)
{
    const float inside = BoundaryAt(3) + CELL * 0.5f;
    const std::vector<Vector3> pts = {Vector3(inside, inside, 0.f),
                                      Vector3(inside + 2.f, inside + 2.f, 0.f)};
    CHECK(Crossings(pts).empty());
}

// The scheduling claim in one test: a leg that never leaves its cell has nothing to
// schedule, so it costs a tick exactly nothing.
TEST(MostLegsScheduleNothingAtAll)
{
    const float at = BoundaryAt(10) + 1.0f;
    const std::vector<Vector3> shortHop = {Vector3(at, at, 0.f), Vector3(at + 5.f, at, 0.f)};
    CHECK_EQ(Crossings(shortHop).size(), size_t(0));
}

TEST(ASegmentCrossingOneBoundaryReportsItWhereItHappens)
{
    const float edge = BoundaryAt(4);
    const std::vector<Vector3> pts = {Vector3(edge - 3.f, 0.f, 0.f), Vector3(edge + 3.f, 0.f, 0.f)};

    const std::vector<float> at = Crossings(pts);
    REQUIRE(at.size() == 1u);
    CHECK(Near(at[0], 3.0f));
}

TEST(WalkingWestCrossesTheSameBoundaryAsWalkingEast)
{
    const float edge = BoundaryAt(4);
    const std::vector<Vector3> east = {Vector3(edge - 3.f, 0.f, 0.f), Vector3(edge + 3.f, 0.f, 0.f)};
    const std::vector<Vector3> west = {Vector3(edge + 3.f, 0.f, 0.f), Vector3(edge - 3.f, 0.f, 0.f)};

    const std::vector<float> there = Crossings(east);
    const std::vector<float> back = Crossings(west);
    REQUIRE(there.size() == 1u);
    REQUIRE(back.size() == 1u);
    CHECK(Near(there[0], back[0]));
}

TEST(ALongRunReportsEveryBoundaryInOrder)
{
    const float start = BoundaryAt(2) + 1.0f;
    const std::vector<Vector3> pts = {Vector3(start, 0.f, 0.f), Vector3(start + CELL * 4.f, 0.f, 0.f)};

    const std::vector<float> at = Crossings(pts);
    CHECK_EQ(at.size(), size_t(4));
    for (size_t i = 1; i < at.size(); ++i)
    {
        CHECK(at[i] > at[i - 1]);
        CHECK(Near(at[i] - at[i - 1], CELL, 0.05f));
    }
}

// Cutting a corner passes an x boundary and a y boundary at the same instant. That is one
// new cell, so it is one event -- a scheduler handed two would notify twice.
TEST(ACornerCutDiagonallyIsOneEventNotTwo)
{
    const float edge = BoundaryAt(5);
    const std::vector<Vector3> pts = {Vector3(edge - 2.f, edge - 2.f, 0.f),
                                      Vector3(edge + 2.f, edge + 2.f, 0.f)};

    CHECK_EQ(Crossings(pts).size(), size_t(1));
}

// But a path that passes the two boundaries at different moments has genuinely entered two
// cells, and both have to be reported.
TEST(TwoBoundariesReachedSeparatelyAreTwoEvents)
{
    const float ex = BoundaryAt(5);
    const float ey = BoundaryAt(7);
    // Deliberately at different fractions of the segment -- x at a half, y at three
    // quarters. Placed at the same fraction they would be one event, which is the case
    // the test above covers.
    const std::vector<Vector3> pts = {Vector3(ex - 2.f, ey - 15.f, 0.f),
                                      Vector3(ex + 2.f, ey + 5.f, 0.f)};

    const std::vector<float> at = Crossings(pts);
    REQUIRE(at.size() == 2u);
    CHECK(at[1] > at[0]);
}

TEST(CrossingsAccumulateAcrossSegmentsOfOnePath)
{
    const float edge = BoundaryAt(6);
    const std::vector<Vector3> pts = {Vector3(edge - 5.f, 0.f, 0.f),
                                      Vector3(edge - 1.f, 0.f, 0.f),
                                      Vector3(edge + 4.f, 0.f, 0.f)};

    const std::vector<float> at = Crossings(pts);
    REQUIRE(at.size() == 1u);
    CHECK(Near(at[0], 5.0f));           // four yards of the first segment, one of the second
}

TEST(ADuplicatedPointDoesNotInventACrossing)
{
    const float edge = BoundaryAt(6);
    const std::vector<Vector3> pts = {Vector3(edge - 5.f, 0.f, 0.f),
                                      Vector3(edge - 1.f, 0.f, 0.f),
                                      Vector3(edge - 1.f, 0.f, 0.f),
                                      Vector3(edge + 4.f, 0.f, 0.f)};

    const std::vector<float> at = Crossings(pts);
    REQUIRE(at.size() == 1u);
    CHECK(Near(at[0], 5.0f));
}

TEST(CrossingsRefuseNonsenseInsteadOfFillingAScheduler)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<Vector3> bad = {Vector3(0.f, 0.f, 0.f), Vector3(nan, 0.f, 0.f)};
    CHECK(Crossings(bad).empty());

    const std::vector<Vector3> one = {Vector3(0.f, 0.f, 0.f)};
    CHECK(Crossings(one).empty());

    std::vector<float> out;
    CellCrossings(nullptr, 2, TheGrid(), out);
    CHECK(out.empty());
}

TEST(CrossingsAreCappedSoOneBadLegCannotFloodTheQueue)
{
    const std::vector<Vector3> huge = {Vector3(-16000.f, 0.f, 0.f), Vector3(16000.f, 0.f, 0.f)};
    const std::vector<float> at = Crossings(huge);
    CHECK(at.size() <= Move::MAX_CROSSINGS);
    CHECK(!at.empty());
}

// ---------------------------------------------------------------------------------
//  The property the whole piece rests on.
// ---------------------------------------------------------------------------------

// At every reported distance the cell index really does change, and between two reported
// distances it really does not. If that holds, a tick can wait for the due times instead
// of comparing the cell every pass -- which is the entire point.
TEST(TheCellChangesAtEveryReportedDistanceAndNowhereElse)
{
    const float sx = BoundaryAt(3) + 4.0f;
    const float sy = BoundaryAt(8) - 6.0f;
    const std::vector<Vector3> pts = {Vector3(sx, sy, 0.f),
                                      Vector3(sx + 70.f, sy + 20.f, 0.f),
                                      Vector3(sx + 70.f, sy + 90.f, 0.f)};

    Route leg;
    REQUIRE(leg.Launch(pts.data(), uint16_t(pts.size()), 7.0f, 1000));

    const std::vector<float> at = Crossings(pts);
    REQUIRE(!at.empty());

    const Grid g = TheGrid();
    auto cellOf = [&](float distance)
    {
        const Vector3 p = leg.At(leg.TimeAtDistance(distance));
        return std::make_pair(g.Index(p.x), g.Index(p.y));
    };

    // Just before each reported distance the cell is the previous one, just after it is a
    // new one. A tenth of a yard either side is far wider than the interpolation error and
    // far narrower than any cell.
    for (size_t i = 0; i < at.size(); ++i)
    {
        CHECK(cellOf(at[i] - 0.1f) != cellOf(at[i] + 0.1f));
    }

    // And nothing changes in between: walk the whole path and count the changes.
    size_t changes = 0;
    std::pair<int, int> previous = cellOf(0.0f);
    for (float s = 0.0f; s <= leg.Length(); s += 0.25f)
    {
        const std::pair<int, int> here = cellOf(s);
        if (here != previous)
        {
            ++changes;
            previous = here;
        }
    }
    CHECK_EQ(changes, at.size());
}

// The crossings are distances; a scheduler wants times. That conversion is the leg's, and
// it has to land inside the leg rather than before or after it.
TEST(EveryCrossingBecomesADueTimeInsideTheLeg)
{
    const float sx = BoundaryAt(3) + 4.0f;
    const std::vector<Vector3> pts = {Vector3(sx, 0.f, 0.f), Vector3(sx + 100.f, 0.f, 0.f)};

    Route leg;
    REQUIRE(leg.Launch(pts.data(), 2, 5.0f, 40000));

    const std::vector<float> at = Crossings(pts);
    REQUIRE(!at.empty());

    uint32_t previous = leg.StartTime();
    for (size_t i = 0; i < at.size(); ++i)
    {
        const uint32_t due = leg.TimeAtDistance(at[i]);
        CHECK(due >= leg.StartTime());
        CHECK(due <= leg.EndTime());
        CHECK(due >= previous);
        previous = due;
    }
}
