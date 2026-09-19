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

#include "Platform/Define.h"
#include <vector>
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "UpdateMask.h"
#include "Path.h"

/**
 * @brief Handles a client request for the known status of a taxi node.
 *
 * @param recv_data The incoming taxi node status packet.
 */
void WorldSession::HandleTaxiNodeStatusQueryOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_TAXINODE_STATUS_QUERY");

    ObjectGuid guid;

    recv_data >> guid;
    SendTaxiStatus(guid);
}

/**
 * @brief Sends whether the nearest taxi node for a flight master is known to the player.
 *
 * @param guid The flight master guid.
 */
void WorldSession::SendTaxiStatus(ObjectGuid guid)
{
    // cheating checks
    Creature* unit = GetPlayer()->GetMap()->GetCreature(guid);
    if (!unit)
    {
        DEBUG_LOG("WorldSession::SendTaxiStatus - %s not found or you can't interact with it.", guid.GetString().c_str());
        return;
    }

    uint32 curloc = sObjectMgr.GetNearestTaxiNode(unit->Where().X(), unit->Where().Y(), unit->Where().Z(), unit->GetMapId(), GetPlayer()->GetTeam());

    // not found nearest
    if (curloc == 0)
    {
        return;
    }

    DEBUG_LOG("WORLD: current location %u ", curloc);

    WorldPacket data(SMSG_TAXINODE_STATUS, 9);
    data << ObjectGuid(guid);
    data << uint8(GetPlayer()->m_taxi.IsTaximaskNodeKnown(curloc) ? 1 : 0);
    SendPacket(&data);

    DEBUG_LOG("WORLD: Sent SMSG_TAXINODE_STATUS");
}

/**
 * @brief Handles a request to open a flight master's available taxi menu.
 *
 * @param recv_data The incoming taxi query packet.
 */
void WorldSession::HandleTaxiQueryAvailableNodes(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_TAXIQUERYAVAILABLENODES");

    ObjectGuid guid;
    recv_data >> guid;

    // cheating checks
    Creature* unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!unit)
    {
        DEBUG_LOG("WORLD: HandleTaxiQueryAvailableNodes - %s not found or you can't interact with him.", guid.GetString().c_str());
        return;
    }

    // remove fake death
    if (GetPlayer()->IsFeigningDeath())
    {
        GetPlayer()->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);
    }

    // unknown taxi node case
    if (SendLearnNewTaxiNode(unit))
    {
        return;
    }

    // known taxi node case
    SendTaxiMenu(unit);
}

/**
 * @brief Sends the taxi route selection menu for a flight master.
 *
 * @param unit The flight master creature.
 */
void WorldSession::SendTaxiMenu(Creature* unit)
{
    // find current node
    uint32 curloc = sObjectMgr.GetNearestTaxiNode(unit->Where().X(), unit->Where().Y(), unit->Where().Z(), unit->GetMapId(), GetPlayer()->GetTeam());

    if (curloc == 0)
    {
        return;
    }

    DEBUG_LOG("WORLD: CMSG_TAXINODE_STATUS_QUERY %u ", curloc);

    WorldPacket data(SMSG_SHOWTAXINODES, (4 + 8 + 4 + 8 * 4));
    data << uint32(1);
    data << unit->GetObjectGuid();
    data << uint32(curloc);
    GetPlayer()->m_taxi.AppendTaximaskTo(data, GetPlayer()->IsTaxiCheater());
    SendPacket(&data);

    DEBUG_LOG("WORLD: Sent SMSG_SHOWTAXINODES");
}

/**
 * @brief Starts taxi flight movement for the player over the whole route.
 *
 * @param mountDisplayId The taxi mount display id, written at the takeoff without UNIT_FLAG_MOUNT.
 * @param route The node ids, the source first.
 * @param startNode The first hop's path node the flight starts toward.
 */
void WorldSession::SendDoFlight(uint32 mountDisplayId, std::vector<uint32> const& route, uint32 startNode)
{
    // remove fake death
    if (GetPlayer()->IsFeigningDeath())
    {
        GetPlayer()->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);
    }

    while (GetPlayer()->GetMotionMaster()->IsOnTaxi())
    {
        GetPlayer()->GetMotionMaster()->MovementExpired(false);
    }

    GetPlayer()->GetMotionMaster()->MoveTaxiFlight(route, startNode, mountDisplayId);
}

/**
 * @brief Learns a newly discovered taxi node and notifies the client.
 *
 * @param unit The flight master creature.
 * @return true if the node was newly learned or no valid node existed; otherwise false.
 */
bool WorldSession::SendLearnNewTaxiNode(Creature* unit)
{
    // find current node
    uint32 curloc = sObjectMgr.GetNearestTaxiNode(unit->Where().X(), unit->Where().Y(), unit->Where().Z(), unit->GetMapId(), GetPlayer()->GetTeam());

    if (curloc == 0)
    {
        return true;                                        // `true` send to avoid WorldSession::SendTaxiMenu call with one more curlock seartch with same false result.
    }

    if (GetPlayer()->m_taxi.SetTaximaskNode(curloc))
    {
        WorldPacket msg(SMSG_NEW_TAXI_PATH, 0);
        SendPacket(&msg);

        WorldPacket update(SMSG_TAXINODE_STATUS, 9);
        update << ObjectGuid(unit->GetObjectGuid());
        update << uint8(1);
        SendPacket(&update);

        return true;
    }
    else
    {
        return false;
    }
}

/**
 * @brief Sends the result of a taxi activation attempt.
 *
 * @param reply The taxi activation status.
 */
void WorldSession::SendActivateTaxiReply(ActivateTaxiReply reply)
{
    WorldPacket data(SMSG_ACTIVATETAXIREPLY, 4);
    data << uint32(reply);
    SendPacket(&data);

    DEBUG_LOG("WORLD: Sent SMSG_ACTIVATETAXIREPLY");
}

/**
 * @brief Handles a multi-node taxi activation request.
 *
 * @param recv_data The incoming taxi express packet.
 */
void WorldSession::HandleActivateTaxiExpressOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_ACTIVATETAXIEXPRESS");

    ObjectGuid guid;
    uint32 node_count;

    recv_data >> guid >> node_count;

    Creature* npc = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        DEBUG_LOG("WORLD: HandleActivateTaxiExpressOpcode - %s not found or you can't interact with it.", guid.GetString().c_str());
        return;
    }
    std::vector<uint32> nodes;

    for (uint32 i = 0; i < node_count; ++i)
    {
        uint32 node;
        recv_data >> node;

        if (!_player->m_taxi.IsTaximaskNodeKnown(node) && !_player->IsTaxiCheater())
        {
            SendActivateTaxiReply(ERR_TAXINOTVISITED);
            recv_data.rpos(recv_data.wpos()); // prevent additional spam at rejected packet
            return;
        }

        nodes.push_back(node);
    }

    if (nodes.empty())
    {
        return;
    }

    DEBUG_LOG("WORLD: Received opcode CMSG_ACTIVATETAXIEXPRESS from %d to %d" , nodes.front(), nodes.back());

    GetPlayer()->ActivateTaxiPathTo(nodes, npc);
}

/**
 * @brief The client's spline-done ack: read for its shape, nothing acts on it.
 *
 * @param recv_data The incoming move-spline-done packet.
 */
void WorldSession::HandleMoveSplineDoneOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_SPLINE_DONE");

    // The client sends it only as the active mover, so a taxi passenger -- its mover revoked at
    // the takeoff -- acks the landing's stop with it and never the flight spline's end. The
    // flight, its seams, its map crossings and its landing are the server's clock (P5-B family 5,
    // design §6.2); the hop chaining and the crossing that ran here are the kernel's TaxiBehaviour.
    MovementInfo movementInfo;
    recv_data >> movementInfo;
}

/**
 * @brief Handles a standard two-node taxi activation request.
 *
 * @param recv_data The incoming taxi activation packet.
 */
void WorldSession::HandleActivateTaxiOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_ACTIVATETAXI");

    ObjectGuid guid;
    std::vector<uint32> nodes;
    nodes.resize(2);

    recv_data >> guid >> nodes[0] >> nodes[1];
    DEBUG_LOG("WORLD: Received opcode CMSG_ACTIVATETAXI from %d to %d" , nodes[0], nodes[1]);
    Creature* npc = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        DEBUG_LOG("WORLD: HandleActivateTaxiOpcode - %s not found or you can't interact with it.", guid.GetString().c_str());
        return;
    }

    if (!_player->IsTaxiCheater())
    {
        if (!_player->m_taxi.IsTaximaskNodeKnown(nodes[0]) || !_player->m_taxi.IsTaximaskNodeKnown(nodes[1]))
        {
            SendActivateTaxiReply(ERR_TAXINOTVISITED);
            return;
        }
    }

    GetPlayer()->ActivateTaxiPathTo(nodes, npc);
}
