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
 * @file ColumnAudit.cpp
 * @brief WHAT DOES THE COLUMN HIDE, AND FROM WHOM?
 *
 * Every spatial answer in the server is a selection over one gather: ColumnAt sweeps a
 * point and the caller picks a floor, a ceiling, a water level out of what came back.
 * Two things can go wrong and neither shows up in a unit test, because a unit test only
 * contains the cases someone already thought of:
 *
 *   1. The gather was INCOMPLETE. ColumnAt used to raycast only inside the height window
 *      its caller passed, so a surface outside the window was not "absent", it was never
 *      looked for -- and every selection then answered confidently from a column that was
 *      missing the thing that mattered.
 *
 *   2. The selection is WRONG for the question. Picking the highest liquid anywhere in
 *      the column answers "what liquid exists here"; it does not answer "what liquid is
 *      this point in", and for a point under a roof the two are different.
 *
 * This tool measures both against real spawns rather than invented ones, and reports
 * DISTRIBUTIONS, per map and overall. A single count hides the difference between a
 * handful of odd spawns and one map where every probe is wrong.
 *
 * ON MEASURING A DEFECT THAT IS FIXED. The first one is no longer reachable through
 * ColumnAt: each instance is swept over its own extent now, so gathering twice with two
 * windows returns the same column twice. It is still measurable exactly, from a single
 * gather, because the old window's effect was per surface and not per instance: the ray
 * started at zTop and ran (zTop - zBottom), so precisely the surfaces outside that
 * interval were the ones never gathered. Counting them is counting what the window used
 * to hide, and it costs one sweep instead of two.
 *
 *   SELECT CONCAT('c', guid, '_', id), map, position_x, position_y, position_z
 *   FROM   creature;
 *
 * feeds it directly: `name,map,x,y,z` per line.
 */

#include "terrain/Column.hpp"
#include "terrain/FusedTerrain.hpp"
#include "terrain/GoModelStore.hpp"
#include "terrain/Terrain.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using world::terrain::Column;
using world::terrain::FusedTerrain;
using world::terrain::LiquidKind;
using world::terrain::Surface;

namespace
{
    /// The window the engine itself passes, from GridMap.h. Reproduced rather than
    /// included because this tool links `terrain` alone and must not drag in the game.
    constexpr float ENGINE_LIFT = 50.0f;
    constexpr float ENGINE_DOWN = 10000.0f;
    constexpr float ENGINE_UP = 2.0f;

    /// Two heights are the same surface when they are this close. Well under the
    /// smallest real floor-to-floor gap, and deliberately well OVER float noise: a hit
    /// is recovered as `sweepTop - t`, and a sweep that starts at the top of a tall WMO
    /// has an ulp far coarser than the exact arithmetic would suggest.
    constexpr float SAME = 0.05f;

    const char* LiquidName(LiquidKind k)
    {
        switch (k)
        {
            case LiquidKind::None:  return "none";
            case LiquidKind::Water: return "water";
            case LiquidKind::Ocean: return "ocean";
            case LiquidKind::Magma: return "magma";
            case LiquidKind::Slime: return "slime";
        }
        return "?";
    }

    struct Probe
    {
        std::string name;
        uint32_t map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    /// One counter per defect, so the report says which one a map is suffering from
    /// rather than that "something is wrong here".
    struct Score
    {
        uint32_t probes = 0;
        uint32_t emptyColumn = 0;

        uint32_t windowHidSolid = 0;     ///< the old window would never have looked here
        uint32_t windowHidLiquid = 0;
        uint64_t solidsHidden = 0;
        uint64_t liquidsHidden = 0;

        uint32_t stackedLiquid = 0;      ///< more than one liquid surface, distinct z
        uint32_t duplicateSurface = 0;   ///< two surfaces at one height, same kind

        uint64_t liquidSurfaces = 0;
        uint64_t liquidDeep = 0;

        uint32_t verdictDiffers = 0;     ///< the headline: old selection against new
        uint32_t wasLiquidNowNone = 0;   ///< the Undercity shape exactly
        uint32_t wasLiquidNowLower = 0;  ///< a nearer surface won instead

        uint32_t liquidWithNoFloor = 0;  ///< a water surface with nothing under it
        uint32_t floorMissing = 0;
    };

    bool ReadProbes(const std::string& path, std::vector<Probe>& out)
    {
        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "cannot open probe file: %s\n", path.c_str());
            return false;
        }

        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            std::istringstream fields(line);
            std::string cell;
            Probe p;

            if (!std::getline(fields, p.name, ',')) { continue; }
            if (!std::getline(fields, cell, ',')) { continue; }
            p.map = uint32_t(std::strtoul(cell.c_str(), nullptr, 10));
            if (!std::getline(fields, cell, ',')) { continue; }
            p.x = std::strtof(cell.c_str(), nullptr);
            if (!std::getline(fields, cell, ',')) { continue; }
            p.y = std::strtof(cell.c_str(), nullptr);
            if (!std::getline(fields, cell, ',')) { continue; }
            p.z = std::strtof(cell.c_str(), nullptr);

            out.push_back(p);
        }
        return true;
    }

    void Tally(Score& s, const Column& column, float z)
    {
        ++s.probes;

        const std::vector<Surface>& all = column.Surfaces();
        if (all.empty())
        {
            ++s.emptyColumn;
            return;
        }

        // What the caller's window used to cut away. The old ray started at zTop and ran
        // for (zTop - zBottom), so a surface outside that interval was never gathered --
        // whether or not its instance was also culled whole by the bounds test on top.
        const float windowTop = z + ENGINE_LIFT;
        const float windowBottom = z - ENGINE_DOWN;

        uint32_t hidSolid = 0, hidLiquid = 0;
        for (const Surface& f : all)
        {
            if (f.z <= windowTop && f.z >= windowBottom)
            {
                continue;
            }
            if (f.Solid()) { ++hidSolid; } else { ++hidLiquid; }
        }
        if (hidSolid) { ++s.windowHidSolid; s.solidsHidden += hidSolid; }
        if (hidLiquid) { ++s.windowHidLiquid; s.liquidsHidden += hidLiquid; }

        // Stacked liquid over one XY, which is what made WMO group order matter.
        std::vector<float> liquidZ;
        for (const Surface& f : all)
        {
            if (f.Solid())
            {
                continue;
            }
            ++s.liquidSurfaces;
            if (f.deep) { ++s.liquidDeep; }
            bool seen = false;
            for (const float lz : liquidZ)
            {
                if (std::fabs(lz - f.z) < SAME) { seen = true; break; }
            }
            if (!seen) { liquidZ.push_back(f.z); }
        }
        if (liquidZ.size() > 1) { ++s.stackedLiquid; }

        // The same surface gathered twice: a tile instance and the global WMO both
        // carrying it, or one model listed under two tiles.
        for (size_t i = 0; i < all.size(); ++i)
        {
            bool dup = false;
            for (size_t j = i + 1; j < all.size(); ++j)
            {
                if (all[i].kind == all[j].kind && std::fabs(all[i].z - all[j].z) < SAME)
                {
                    dup = true;
                    break;
                }
            }
            if (dup) { ++s.duplicateSurface; break; }
        }

        // The headline: the selection that was, against the selection that is.
        const auto highest = column.HighestLiquid();
        const auto over = column.LiquidOver(z);

        if (highest.has_value() != over.has_value())
        {
            ++s.verdictDiffers;
            if (highest && !over) { ++s.wasLiquidNowNone; }
        }
        else if (highest && over && std::fabs(highest->z - over->z) >= SAME)
        {
            ++s.verdictDiffers;
            ++s.wasLiquidNowLower;
        }

        // A water surface with nothing under it anywhere in the column: the column is
        // describing a lake with no bed, which is how the Undercity drowning looked.
        if (highest && !column.HighestSolidAtOrBelow(highest->z))
        {
            ++s.liquidWithNoFloor;
        }

        if (!column.Floor(z, ENGINE_UP))
        {
            ++s.floorMissing;
        }
    }

    void Report(const char* label, const Score& s)
    {
        if (!s.probes)
        {
            return;
        }
        auto pct = [&](uint64_t n) { return 100.0 * double(n) / double(s.probes); };

        std::printf("%-22s probes %-8u empty %-6u floor-missing %u\n", label, s.probes,
                    s.emptyColumn, s.floorMissing);
        std::printf("  old window hid a SOLID      : %7u (%5.2f%%)  %llu surface(s)\n",
                    s.windowHidSolid, pct(s.windowHidSolid),
                    (unsigned long long)s.solidsHidden);
        std::printf("  old window hid a LIQUID     : %7u (%5.2f%%)  %llu surface(s)\n",
                    s.windowHidLiquid, pct(s.windowHidLiquid),
                    (unsigned long long)s.liquidsHidden);
        std::printf("  stacked liquid over one XY  : %7u (%5.2f%%)\n", s.stackedLiquid,
                    pct(s.stackedLiquid));
        std::printf("  duplicate surface in column : %7u (%5.2f%%)\n", s.duplicateSurface,
                    pct(s.duplicateSurface));
        std::printf("  liquid with no bed under it : %7u (%5.2f%%)\n", s.liquidWithNoFloor,
                    pct(s.liquidWithNoFloor));
        std::printf("  liquid surfaces seen        : %llu, of which dark water %llu\n",
                    (unsigned long long)s.liquidSurfaces, (unsigned long long)s.liquidDeep);
        std::printf("  VERDICT CHANGED             : %7u (%5.2f%%)   "
                    "was-liquid-now-none %u, now-lower-surface %u\n",
                    s.verdictDiffers, pct(s.verdictDiffers), s.wasLiquidNowNone,
                    s.wasLiquidNowLower);
        std::printf("\n");
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: mangos-column-audit <tileDir> <probes.csv> [gomodelDir] "
                    "[--verbose] [--limit N]\n"
                    "\n"
                    "  Gathers each probe's column once and reports what the caller's\n"
                    "  height window used to hide from it, where the old and new liquid\n"
                    "  selections disagree, and what one gather costs.\n");
        return 2;
    }

    const std::string tileDir = argv[1];
    const std::string probePath = argv[2];
    std::string goDir;
    bool verbose = false;
    size_t limit = 0;

    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--verbose")
        {
            verbose = true;
        }
        else if (a == "--limit" && i + 1 < argc)
        {
            limit = size_t(std::strtoull(argv[++i], nullptr, 10));
        }
        else if (a.rfind("--", 0) != 0)
        {
            goDir = a;
        }
    }

    FusedTerrain::SetTileDir(tileDir);
    if (!goDir.empty())
    {
        world::terrain::GoModelStore::Instance().SetDirectory(goDir);
    }

    std::vector<Probe> probes;
    if (!ReadProbes(probePath, probes))
    {
        return 1;
    }
    if (limit && probes.size() > limit)
    {
        probes.resize(limit);
    }
    std::printf("%zu probe(s) from %s\ntiles: %s\n\n", probes.size(), probePath.c_str(),
                tileDir.c_str());

    std::map<uint32_t, std::unique_ptr<FusedTerrain>> engines;
    std::map<uint32_t, Score> perMap;
    Score total;

    uint64_t sweepNs = 0;
    uint64_t sweepSurfaces = 0;

    size_t done = 0;
    for (const Probe& p : probes)
    {
        auto& engine = engines[p.map];
        if (!engine)
        {
            engine.reset(new FusedTerrain(p.map));
        }

        // Warm first, UNTIMED. The first column at a point pays for reading the tile off
        // disk, which is not what is being measured; leaving it in the timing makes the
        // first call at every new tile stand in for the cost of a sweep.
        (void)engine->ColumnAt(p.x, p.y, p.z + ENGINE_LIFT, p.z - ENGINE_DOWN);

        const auto t0 = std::chrono::steady_clock::now();
        const Column column =
            engine->ColumnAt(p.x, p.y, p.z + ENGINE_LIFT, p.z - ENGINE_DOWN);
        const auto t1 = std::chrono::steady_clock::now();

        sweepNs +=
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        sweepSurfaces += column.Surfaces().size();

        Tally(perMap[p.map], column, p.z);
        Tally(total, column, p.z);

        if (verbose)
        {
            const auto highest = column.HighestLiquid();
            const auto over = column.LiquidOver(p.z);
            const bool changed =
                highest.has_value() != over.has_value() ||
                (highest && over && std::fabs(highest->z - over->z) >= SAME);
            if (changed)
            {
                std::printf("  [CHANGED] %-26s map %-4u (%.2f, %.2f, %.2f)  "
                            "was %s@%.2f  now %s\n",
                            p.name.c_str(), p.map, p.x, p.y, p.z,
                            highest ? LiquidName(highest->liquid) : "none",
                            highest ? highest->z : 0.f,
                            over ? LiquidName(over->liquid) : "none");
            }
        }

        if ((++done % 10000) == 0)
        {
            std::fprintf(stderr, "  ... %zu / %zu\n", done, probes.size());
        }
    }

    std::printf("\n================ per map ================\n\n");
    for (const auto& kv : perMap)
    {
        char label[32];
        std::snprintf(label, sizeof label, "map %u", kv.first);
        Report(label, kv.second);
    }

    std::printf("================ total ================\n\n");
    Report("ALL", total);

    const double n = double(probes.size() ? probes.size() : 1);
    std::printf("================ cost of one ColumnAt ================\n\n");
    std::printf("  sweep (window z+%.0f .. z-%.0f) : %8.3f us/call, %6.2f surfaces/call\n",
                ENGINE_LIFT, ENGINE_DOWN, double(sweepNs) / n / 1000.0,
                double(sweepSurfaces) / n);
    std::printf("\n  total wall time in ColumnAt: %.2f s over %zu probes\n",
                double(sweepNs) / 1e9, probes.size());
    return 0;
}
