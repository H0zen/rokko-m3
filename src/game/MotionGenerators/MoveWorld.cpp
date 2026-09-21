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

#include "MoveWorld.h"
#include "PathFinder.h"
#include "Unit.h"
#include "Map.h"
#include "ObjectLookup.h"
#include "Timer.h"
#include "Util.h"

namespace
{
    /// The oldest a player's reported position can be: the client's own heartbeat period.
    const uint32 PLAYER_REPORT_AGE_MS = 500;
}

bool MoveWorld::Route(const Geometry::Vector3& from, const Geometry::Vector3& to,
                      std::vector<Geometry::Vector3>& points)
{
    PathFinder finder(&m_unit);
    finder.calculate(from.x, from.y, from.z, to.x, to.y, to.z, false);

    const PathType type = finder.getPathType();
    if ((type & PATHFIND_NOPATH) != 0)
    {
        return false;
    }

    PointsArray const& found = finder.getPath();
    if (found.size() < 2)
    {
        return false;
    }

    // A partial route that gets no closer is not a route: walking it would burn a packet
    // and leave the mover where it stood, and the behaviour would read the arrival as
    // progress it did not make.
    if ((type & PATHFIND_INCOMPLETE) != 0)
    {
        const float before = (to - from).squaredMagnitude();
        const float after = (to - found.back()).squaredMagnitude();
        if (after >= before)
        {
            return false;
        }
    }

    points.assign(found.begin(), found.end());
    return true;
}

bool MoveWorld::Floor(const Geometry::Vector3& at, float& z)
{
    Map* map = m_unit.FindMap();
    if (!map)
    {
        return false;
    }
    const float found = map->GetHeight(m_unit.GetPhaseMask(), at.x, at.y, at.z);
    if (found <= INVALID_HEIGHT)
    {
        return false;
    }
    z = found;
    return true;
}

bool MoveWorld::Scatter(const Geometry::Vector3& centre, float radius, Geometry::Vector3& out)
{
    Map* map = m_unit.FindMap();
    if (!map)
    {
        return false;
    }
    float x = centre.x;
    float y = centre.y;
    float z = centre.z;
    if (!map->GetReachableRandomPosition(&m_unit, x, y, z, radius))
    {
        return false;
    }
    out = Geometry::Vector3(x, y, z);
    return true;
}

float MoveWorld::Frand(float lo, float hi)
{
    return frand(lo, hi);
}

uint32_t MoveWorld::Urand(uint32_t lo, uint32_t hi)
{
    return urand(lo, hi);
}

float MoveWorld::Pace(uint32_t gait) const
{
    if ((gait & Move::GAIT_FLY) != 0)
    {
        return m_unit.GetSpeed(MOVE_FLIGHT);
    }
    if ((gait & Move::GAIT_WALK) != 0)
    {
        return m_unit.GetSpeed(MOVE_WALK);
    }
    return m_unit.GetSpeed(MOVE_RUN);
}

Geometry::Vector3 MoveWorld::Here() const
{
    return m_unit.Where().Pos();
}

float MoveWorld::Heading() const
{
    return m_unit.Where().Facing();
}

Move::Quarry MoveSighting::Look(uint64_t rawGuid) const
{
    Move::Quarry quarry;

    Unit* target = ObjectLookup::GetUnit(m_unit, ObjectGuid(rawGuid));
    if (!target || !target->IsInWorld() || !target->IsAlive())
    {
        return quarry;
    }
    if (target->FindMap() != m_unit.FindMap())
    {
        return quarry;
    }

    quarry.known = true;
    quarry.at = target->Where().Pos();
    quarry.reach = target->Where().Extent();

    // The fastest the target could be travelling, which is what the drift bound needs --
    // not the speed it happens to be using. A player that is walking may start running
    // between two of our wake-ups, and a bound built on the slower number would be wrong.
    const float run = target->GetSpeed(MOVE_RUN);
    const float fly = target->GetSpeed(MOVE_FLIGHT);
    quarry.topSpeed = run > fly ? run : fly;
    if (quarry.topSpeed <= 0.0f)
    {
        quarry.topSpeed = 7.0f;
    }

    // WHEN that position was true, which is the whole of the uncertainty in a chase.
    //
    // For a unit the server drives, the answer is NOW: its route is ours and its position
    // is arithmetic, exact to the millisecond.
    //
    // For a player it is the moment of its last movement packet, which is not recorded
    // anywhere yet. What IS known is the bound: the client schedules a heartbeat at
    // now + 500 ms after every movement packet it sends (sub_573470 in Wow.exe 15595) and
    // sub-divides its own simulation step to land on it, so a translating player's position
    // is never staler than that plus the trip. Treating every player's report as half a
    // second old is therefore always safe and never wrong by more than the same amount --
    // and it costs a subtraction instead of a field on every Unit.
    const uint32 now = getMSTime();
    quarry.reportedAtMs = target->GetTypeId() == TYPEID_PLAYER ? now - PLAYER_REPORT_AGE_MS : now;
    return quarry;
}
