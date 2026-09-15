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

/**
 * @file
 * @brief A map built from one global WMO must not be resident for ever.
 *
 * The tile cache sweeps a (tx, ty) grid. A map with no ADT grid has one tile that
 * belongs to no cell, so it is not reachable from that loop and was never dropped
 * once loaded. The map-level release (TerrainManager::UnloadTerrain) covered the
 * common case, but it returns early when GridUnload is 0 while the sweep below
 * keeps running -- so under that setting the tile was resident for the life of
 * the process, across every such map ever visited. 68 of them ship with the
 * client, 80 MB in total.
 */

#include "TestHarness.h"

#include "terrain/FusedTerrain.hpp"
#include "terrain/Terrain.hpp"

#include <memory>

using world::terrain::FusedTerrain;
using world::terrain::ITileSource;
using world::terrain::TerrainTile;

namespace
{
    constexpr uint32_t SWEEP_INTERVAL_MS = 60u * 1000u;
    constexpr uint32_t TILE_IDLE_MS = 5u * 60u * 1000u;

    /// A map with no grid and one global tile, counting how often it is read.
    class GlobalOnlySource : public ITileSource
    {
        public:
            std::shared_ptr<TerrainTile> Load(uint32_t, int, int) override
            {
                return nullptr;                    // no ADT grid: this is a WMO-only map
            }

            std::shared_ptr<TerrainTile> LoadGlobal(uint32_t) override
            {
                ++loads;
                auto tile = std::make_shared<TerrainTile>();
                tile->isGlobalWmo = true;
                return tile;
            }

            int loads = 0;
    };

    /// Runs the cache clock far enough forward that a sweep must happen and the
    /// idle threshold must have passed, without any query in between.
    void IdleFor(FusedTerrain& terrain, uint32_t ms)
    {
        while (ms > 0)
        {
            const uint32_t step = ms > SWEEP_INTERVAL_MS ? SWEEP_INTERVAL_MS : ms;
            terrain.Update(step);
            ms -= step;
        }
    }

    /// Any query that reaches the global tile.
    void Touch(const FusedTerrain& terrain)
    {
        uint32_t mogp = 0;
        int32_t adt = 0, root = 0, group = 0;
        float ground = 0.0f;
        terrain.GetAreaInfo(0.0f, 0.0f, 0.0f, mogp, adt, root, group, ground);
    }
}

TEST(GlobalWmo_is_read_once_while_the_map_is_busy)
{
    auto source = std::make_shared<GlobalOnlySource>();
    FusedTerrain terrain(1234, source);

    for (int i = 0; i < 20; ++i)
    {
        Touch(terrain);
    }

    CHECK_EQ(source->loads, 1);      // probed once, then cached
}

TEST(GlobalWmo_is_dropped_once_the_map_goes_idle)
{
    // The regression. Before this, the tile was assigned once behind a probe latch
    // and never released by anything in this class.
    auto source = std::make_shared<GlobalOnlySource>();
    FusedTerrain terrain(1234, source);

    Touch(terrain);
    CHECK_EQ(source->loads, 1);

    IdleFor(terrain, TILE_IDLE_MS + SWEEP_INTERVAL_MS);

    // Dropped, so the next query has to read it again.
    Touch(terrain);
    CHECK_EQ(source->loads, 2);
}

TEST(GlobalWmo_survives_while_a_cell_is_pinned)
{
    // It stands in for the whole map rather than for a cell, so it may only go when
    // nothing anywhere on the map is pinned. A grid that is merely idle is not
    // enough -- an active grid holds a pin even when no query has run for minutes.
    auto source = std::make_shared<GlobalOnlySource>();
    FusedTerrain terrain(1234, source);

    Touch(terrain);
    CHECK_EQ(source->loads, 1);

    terrain.PinCell(30, 30);
    IdleFor(terrain, TILE_IDLE_MS + SWEEP_INTERVAL_MS);

    Touch(terrain);
    CHECK_EQ(source->loads, 1);      // still resident

    terrain.UnpinCell(30, 30);
    IdleFor(terrain, TILE_IDLE_MS + SWEEP_INTERVAL_MS);

    Touch(terrain);
    CHECK_EQ(source->loads, 2);      // and now it goes
}

TEST(GlobalWmo_absence_is_still_remembered_between_sweeps)
{
    // A map with no global tile at all must not re-probe on every query; the miss is
    // memoed like any other. The sweep skips a null, so the memo survives it.
    class NoGlobalSource : public ITileSource
    {
        public:
            std::shared_ptr<TerrainTile> Load(uint32_t, int, int) override
            {
                return nullptr;
            }

            std::shared_ptr<TerrainTile> LoadGlobal(uint32_t) override
            {
                ++loads;
                return nullptr;
            }

            int loads = 0;
    };

    auto source = std::make_shared<NoGlobalSource>();
    FusedTerrain terrain(1234, source);

    for (int i = 0; i < 10; ++i)
    {
        Touch(terrain);
    }
    CHECK_EQ(source->loads, 1);

    IdleFor(terrain, TILE_IDLE_MS + SWEEP_INTERVAL_MS);
    Touch(terrain);

    CHECK_EQ(source->loads, 1);      // nothing to evict, so nothing to re-probe
}

TEST(RestartSweep_drops_an_unpinned_tile_but_not_a_pinned_one)
{
    // The harness's pin (P0-D): a run needs the sweep to land at the SAME virtual
    // moment on every launch, not at a phase the boot's clock happened to set, so
    // RestartSweep does not wait for TILE_IDLE_MS -- only whether a cell still pins.
    class GridTileSource : public ITileSource
    {
        public:
            std::shared_ptr<TerrainTile> Load(uint32_t, int, int) override
            {
                ++loads;
                return std::make_shared<TerrainTile>();
            }

            int loads = 0;
    };

    auto source = std::make_shared<GridTileSource>();
    FusedTerrain terrain(1234, source);
    CHECK_EQ(source->loads, 0);

    // (0, 0) resolves to tile (32, 32); nothing else is ever queried, so
    // ResidentTiles() below counts exactly this one tile.
    terrain.ColumnAt(0.0f, 0.0f, 100.0f, -100.0f);
    CHECK_EQ(terrain.ResidentTiles(), size_t(1));
    CHECK_EQ(source->loads, 1);    // one real read

    terrain.RestartSweep();
    CHECK_EQ(terrain.ResidentTiles(), size_t(0));    // unpinned: gone right away
    CHECK_EQ(source->loads, 1);    // a drop, not a reload

    terrain.PinCell(32, 32);
    terrain.ColumnAt(0.0f, 0.0f, 100.0f, -100.0f);
    CHECK_EQ(terrain.ResidentTiles(), size_t(1));
    CHECK_EQ(source->loads, 2);    // dropped above, so this is a real re-read

    terrain.RestartSweep();
    CHECK_EQ(terrain.ResidentTiles(), size_t(1));    // pinned: survives
    CHECK_EQ(source->loads, 2);    // pinned tile was never dropped, so nothing to reload

    terrain.UnpinCell(32, 32);
}
