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

#ifndef MANGOS_TAXIROUTE_H
#define MANGOS_TAXIROUTE_H

#include "Platform/Define.h"

#include <cmath>
#include <cstddef>
#include <vector>

/**
 * ONE welded taxi route, and the ONE place the resume reads it.
 *
 * A route is a list of taxi NODES (hubs); the geometry is the TaxiPathNode rows of the hops
 * between them, welded end to end with the seam row kept once. That weld used to live inside
 * MotionMaster::MoveTaxiFlight alone; the landing-time resume (design 2026-09-22 §2) has to
 * measure the same polyline the spline is built from -- "do not re-derive it separately" -- so
 * the weld moved here and both read it.
 *
 * TaxiRoute::Weld is DBC-fed and lives in the game library. Everything in TaxiResume is pure
 * arithmetic over the welded array and is inline here so the test suite can pin the decision
 * without linking the game (the same arrangement TaxiDestinationsString.h already uses).
 */
struct TaxiRouteNode
{
    uint32 mapId = 0;             ///< TaxiPathNode.ContinentID: a change of map cuts the leg
    float  x = 0.0f;              ///< world
    float  y = 0.0f;
    float  z = 0.0f;
    uint32 arrivalEvent = 0;      ///< fired when the node is reached
    uint32 departureEvent = 0;    ///< fired when the node is left
    bool   seam = false;          ///< the incoming hop's last row: the route advances when it is left
};

namespace TaxiRoute
{
    /**
     * @brief The hops of `route` welded into one node array: the seam node once -- the incoming
     *        hop's last row, kept and marked -- and the outgoing hop's node 0 dropped, as the
     *        hop chaining's pathNode = 1 skipped it.
     * @param route The node ids, source first.
     * @param out   Filled on success; untouched meaning is not promised on failure.
     * @return False for a route of fewer than two nodes, or when a hop's path is missing or
     *         too short to fly. The caller drops the route and says so.
     */
    bool Weld(std::vector<uint32> const& route, std::vector<TaxiRouteNode>& out);
}

/**
 * The flight as a paid contract to the destination (design 2026-09-22 §2, the user's model):
 * the fare is taken at the click, and wherever the passenger goes mid-route -- a logout, a
 * dungeon, a battleground -- he ends up at the end of the route he paid for. The landing time
 * is stamped at the takeoff; every resume asks this one question of it.
 */
namespace TaxiResume
{
    /**
     * @brief The polyline's length from `from` to the route's end, in yards.
     *
     * A segment whose two ends are on different maps is the route's SEAM: a teleport, not a
     * flight, and it measures zero here so that elapsed time never buys any of it. That keeps
     * the length this returns the same quantity the spline is built from, which lays one leg
     * per map and crosses the seam by teleport.
     */
    inline float Length(std::vector<TaxiRouteNode> const& nodes, size_t from)
    {
        float total = 0.0f;
        for (size_t i = from + 1; i < nodes.size(); ++i)
        {
            if (nodes[i].mapId != nodes[i - 1].mapId)
            {
                continue;
            }
            const float dx = nodes[i].x - nodes[i - 1].x;
            const float dy = nodes[i].y - nodes[i - 1].y;
            const float dz = nodes[i].z - nodes[i - 1].z;
            total += std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        return total;
    }

    enum class Verdict : uint8
    {
        NoRoute,    ///< nothing to resume: the caller drops the route
        Landed,     ///< the contract is over: teleport to the destination and clear the taxi
        Airborne    ///< still in the air: back on the mount at the point below
    };

    /// What one resume decided. `node` indexes the WELDED array and is the next node to fly to,
    /// which is what MotionMaster::MoveTaxiFlight takes as its startNode.
    struct Resume
    {
        Verdict verdict = Verdict::NoRoute;
        size_t  node = 0;
        uint32  mapId = 0;
        float   x = 0.0f;
        float   y = 0.0f;
        float   z = 0.0f;
        bool    clamped = false;   ///< the elapsed point lay on a later map; held at this map's last node
        float   flown = 0.0f;      ///< yards the route has covered by `now`
        float   total = 0.0f;      ///< the whole route's flyable length
    };

    /**
     * @brief The one check behind all three resumes (login, dungeon return, battleground return).
     *
     * @param nodes      The welded route.
     * @param speed      Movement.TaxiSpeed, yd/s: the speed the spline was built at.
     * @param now        Server time, seconds.
     * @param landing    The stamp saved with the route. ZERO means the route was saved by a build
     *                   that did not stamp one (the old taxi_path string, or a logout taken inside
     *                   a battleground, whose stored path has no room for it): that reads as
     *                   LANDED, which is the safe direction -- the passenger gets the destination
     *                   he paid for rather than a flight whose age nobody knows.
     * @param currentMap The map the passenger is standing on.
     *
     * Airborne holds the elapsed point back to `currentMap` when the elapsed distance has carried
     * it onto a later map: the resume is pinned at the last node of the route that is on his own
     * map -- the seam -- and the kernel's own crossing carries him over from there. `clamped`
     * says when that happened. A route with no node on his map at all cannot be resumed and
     * reads NoRoute.
     */
    inline Resume Decide(std::vector<TaxiRouteNode> const& nodes, float speed,
                         int64 now, int64 landing, uint32 currentMap)
    {
        Resume r;
        if (nodes.size() < 2 || speed <= 0.0f)
        {
            return r;   // NoRoute
        }
        r.total = Length(nodes, 0);

        // No stamp, or the contract's time is up: the destination, and no flight.
        if (landing <= 0 || now >= landing)
        {
            r.verdict = Verdict::Landed;
            r.node = nodes.size() - 1;
            r.mapId = nodes.back().mapId;
            r.x = nodes.back().x;
            r.y = nodes.back().y;
            r.z = nodes.back().z;
            r.flown = r.total;
            return r;
        }

        // Still in the air. The distance the route has covered is what the whole route is worth
        // less what the remaining time can still buy; a landing stamped further out than the
        // route is long (a speed changed under a saved flight) simply reads as the start.
        const float remaining = float(landing - now) * speed;
        r.flown = r.total - remaining;
        if (r.flown < 0.0f)
        {
            r.flown = 0.0f;
        }

        r.verdict = Verdict::Airborne;
        r.node = nodes.size() - 1;
        r.mapId = nodes.back().mapId;
        r.x = nodes.back().x;
        r.y = nodes.back().y;
        r.z = nodes.back().z;

        float covered = 0.0f;
        for (size_t i = 1; i < nodes.size(); ++i)
        {
            if (nodes[i].mapId != nodes[i - 1].mapId)
            {
                continue;   // the seam: no distance, and no point lies "on" it
            }
            const float dx = nodes[i].x - nodes[i - 1].x;
            const float dy = nodes[i].y - nodes[i - 1].y;
            const float dz = nodes[i].z - nodes[i - 1].z;
            const float seg = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (covered + seg >= r.flown)
            {
                const float t = seg > 0.0f ? (r.flown - covered) / seg : 0.0f;
                r.node = i;
                r.mapId = nodes[i].mapId;
                r.x = nodes[i - 1].x + dx * t;
                r.y = nodes[i - 1].y + dy * t;
                r.z = nodes[i - 1].z + dz * t;
                break;
            }
            covered += seg;
        }

        if (r.mapId == currentMap)
        {
            return r;
        }

        // The elapsed point is on a map he is not on. Hold him at the last node of the route
        // that is on his own map and at or before the point: the flight then runs that map's
        // remaining leg and the kernel's crossing takes the seam, which is the tested path.
        const size_t reached = r.node;
        r.clamped = true;
        for (size_t i = reached + 1; i-- > 0;)
        {
            if (nodes[i].mapId == currentMap)
            {
                r.node = i;
                r.mapId = nodes[i].mapId;
                r.x = nodes[i].x;
                r.y = nodes[i].y;
                r.z = nodes[i].z;
                return r;
            }
        }
        // Nothing of his map lies behind the elapsed point either: he is AHEAD of it, which is
        // what a logout taken right after a crossing looks like (the deque's front is still the
        // hop's old-map source). The first node of the route on his map is where he rejoins it.
        for (size_t i = reached + 1; i < nodes.size(); ++i)
        {
            if (nodes[i].mapId == currentMap)
            {
                r.node = i;
                r.mapId = nodes[i].mapId;
                r.x = nodes[i].x;
                r.y = nodes[i].y;
                r.z = nodes[i].z;
                return r;
            }
        }
        r.verdict = Verdict::NoRoute;   // nothing of this route is on his map
        return r;
    }
}

#endif
