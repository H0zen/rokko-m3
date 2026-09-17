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

#include "FlightPathMovementGenerator.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "movement/MoveSplineInit.h"
#include "movement/MoveSpline.h"

//----------------------------------------------------//
/**
 * @brief Gets the path at the end of the map.
 * @return The path at the end of the map.
 */
uint32 FlightPathMovementGenerator::GetPathAtMapEnd() const
{
    if (i_currentNode >= i_path->size())
    {
        return i_path->size();
    }

    uint32 curMapId = (*i_path)[i_currentNode].ContinentID;

    for (uint32 i = i_currentNode; i < i_path->size(); ++i)
    {
        if ((*i_path)[i].ContinentID != curMapId)
        {
            return i;
        }
    }

    return i_path->size();
}

/**
 * @brief Initializes the FlightPathMovementGenerator.
 * @param player Reference to the player.
 */
void FlightPathMovementGenerator::Initialize(Unit& u)
{
    Player& player = static_cast<Player&>(u);
    Reset(player);
}

/**
 * @brief Finalizes the FlightPathMovementGenerator.
 * @param player Reference to the player.
 */
void FlightPathMovementGenerator::Finalize(Unit& u)
{
    Player& player = static_cast<Player&>(u);
    // The mirror clears this bit at the commit's end, but Unmount and the online-state change
    // below must not see a flight still in progress (the old comment warned of a crash sending
    // an object-build movement packet for a flight state with the generator already off the
    // stack); the one deliberate second writer of a mirrored bit -- MirrorUnitState agrees once
    // the commit runs.
    player.clearUnitState(UNIT_STAT_TAXI_FLIGHT);
    player.Unmount();
    player.RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT);

    if (player.m_taxi.empty())
    {
        player.GetHostileRefManager().setOnlineOfflineState(true);
        if (player.pvpInfo.inHostileArea)
        {
            player.CastSpell(&player, 2479, true);
        }

        // Update z position to ground and orientation for landing point
        // This prevent cheating with landing  point at lags
        // When client side flight end early in comparison server side
        player.StopMoving(true);
    }
}

/**
 * @brief Interrupts the FlightPathMovementGenerator.
 * @param player Reference to the player.
 */
void FlightPathMovementGenerator::Interrupt(Unit& /*u*/)
{
}

#define PLAYER_FLIGHT_SPEED        32.0f

/**
 * @brief Resets the FlightPathMovementGenerator.
 * @param player Reference to the player.
 */
void FlightPathMovementGenerator::Reset(Unit& u)
{
    Player& player = static_cast<Player&>(u);
    // Set the player to offline state for hostile references
    player.GetHostileRefManager().setOnlineOfflineState(false);

    // Set the client control lost and taxi flight flags
    player.SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT);

    // Initialize the movement spline for the player
    Movement::MoveSplineInit init(player);
    uint32 end = GetPathAtMapEnd();
    for (uint32 i = GetCurrentNode(); i != end; ++i)
    {
        Geometry::Vector3 vertice((*i_path)[i].Loc_0, (*i_path)[i].Loc_1, (*i_path)[i].Loc_2);
        init.Path().push_back(vertice);
    }
    init.SetFirstPointId(GetCurrentNode());
    init.SetFly();
    // 4.3.4 reads float path points only with UncompressedPath (Catmull-Rom sets it);
    // the packed linear path wraps at +-255 yd, which every taxi route exceeds.
    init.SetSmooth();
    init.SetVelocity(PLAYER_FLIGHT_SPEED);
    init.Launch();
}

/**
 * @brief Updates the FlightPathMovementGenerator.
 * @param player Reference to the player.
 * @param diff Time difference.
 * @return True if the update was successful, false otherwise.
 */
bool FlightPathMovementGenerator::Update(Unit& u, const uint32& /*diff*/)
{
    Player& player = static_cast<Player&>(u);
    uint32 pointId = (uint32)player.movespline->currentPathIdx();
    if (pointId > i_currentNode)
    {
        bool departureEvent = true;
        do
        {
            DoEventIfAny(player, (*i_path)[i_currentNode], departureEvent);
            if (pointId == i_currentNode)
            {
                break;
            }
            i_currentNode += (uint32)departureEvent;
            departureEvent = !departureEvent;
        }
        while (true);
    }

    return i_currentNode < (i_path->size() - 1);
}

/**
 * @brief Sets the current node after teleporting the player.
 */
void FlightPathMovementGenerator::SetCurrentNodeAfterTeleport()
{
    if (i_path->empty())
    {
        return;
    }

    uint32 map0 = (*i_path)[0].ContinentID;

    for (size_t i = 1; i < i_path->size(); ++i)
    {
        if ((*i_path)[i].ContinentID != map0)
        {
            i_currentNode = i;
            return;
        }
    }
}

void FlightPathMovementGenerator::DoEventIfAny(Player& player, TaxiPathNodeEntry const& node, bool departure)
{
    if (uint32 eventid = departure ? node.DepartureEventID : node.ArrivalEventID)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "Taxi %s event %u of node %u of path %u for player %s", departure ? "departure" : "arrival", eventid, node.NodeIndex, node.PathID, player.GetName());
        StartEvents_Event(player.GetMap(), eventid, &player, &player, departure);
    }
}

/**
 * @brief Gets the reset position for the player.
 * @param player Reference to the player.
 * @param x Reference to the X-coordinate.
 * @param y Reference to the Y-coordinate.
 * @param z Reference to the Z-coordinate.
 * @param o Reference to the orientation.
 * @return True if the reset position was successfully obtained, false otherwise.
 */
bool FlightPathMovementGenerator::GetResetPosition(Unit& /*u*/, float& x, float& y, float& z, float& o) const
{
    const TaxiPathNodeEntry& node = (*i_path)[i_currentNode];
    x = node.Loc_0;
    y = node.Loc_1;
    z = node.Loc_2;

    return true;
}
