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

#include "Utilities/Errors.h"
#include <string>
#include <vector>
#include "PlayerTaxi.h"
#include "TaxiDestinationsString.h"
#include "TaxiRoute.h"
#include "Player.h"
#include "Language.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "UpdateMask.h"
#include "ObjectMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "AchievementMgr.h"
#include "DBCStores.h"
#include "MapManager.h"
#include "movement/MoveSplineInit.h"

#include <cmath>
#include <limits>
#include <sstream>

void PlayerTaxi::InitTaxiNodes(uint32 race, uint32 chrClass, uint8 level)
{
    InitTaxiNodesForClass(chrClass);
    InitTaxiNodesForRace(race);
    InitTaxiNodesForFaction(Player::TeamForRace(race));
    InitTaxiNodesForLvl(level);
}

void PlayerTaxi::InitTaxiNodesForClass(uint32 chrClass)
{
    // class specific initial known nodes
    switch (chrClass)
    {
        case CLASS_DEATH_KNIGHT:
        {
            for (int i = 0; i < TaxiMaskSize; ++i)
            {
                m_taximask[i] |= sOldContinentsNodesMask[i];
            }
            break;
        }
    }
}

void PlayerTaxi::InitTaxiNodesForRace(uint32 race)
{
    // race specific initial known nodes: capital and taxi hub masks
    switch (race)
    {
        case RACE_HUMAN:
        case RACE_DWARF:
        case RACE_NIGHTELF:
        case RACE_GNOME:
        case RACE_DRAENEI:
        case RACE_WORGEN:
            SetTaximaskNode(2);     // Stormwind, Elwynn
            SetTaximaskNode(6);     // Ironforge, Dun Morogh
            SetTaximaskNode(26);    // Lor'danel, Darkshore
            SetTaximaskNode(27);    // Rut'theran Village, Teldrassil
            SetTaximaskNode(49);    // Moonglade (Alliance)
            SetTaximaskNode(94);    // The Exodar
            SetTaximaskNode(456);   // Dolanaar, Teldrassil
            SetTaximaskNode(457);   // Darnassus, Teldrassil
            SetTaximaskNode(582);   // Goldshire, Elwynn
            SetTaximaskNode(589);   // Eastvale Logging Camp, Elwynn
            SetTaximaskNode(619);   // Kharanos, Dun Morogh
            SetTaximaskNode(620);   // Gol'Bolar Quarry, Dun Morogh
            SetTaximaskNode(624);   // Azure Watch, Azuremyst Isle
            break;

        case RACE_ORC:
        case RACE_UNDEAD:
        case RACE_TAUREN:
        case RACE_TROLL:
        case RACE_BLOODELF:
        case RACE_GOBLIN:
            SetTaximaskNode(11);    // Undercity, Tirisfal
            SetTaximaskNode(22);    // Thunder Bluff, Mulgore
            SetTaximaskNode(23);    // Orgrimmar, Durotar
            SetTaximaskNode(69);    // Moonglade (Horde)
            SetTaximaskNode(82);    // Silvermoon City
            SetTaximaskNode(384);   // The Bulwark, Tirisfal
            SetTaximaskNode(402);   // Bloodhoof Village, Mulgore
            SetTaximaskNode(460);   // Brill, Tirisfal Glades
            SetTaximaskNode(536);   // Sen'jin Village, Durotar
            SetTaximaskNode(537);   // Razor Hill, Durotar
            SetTaximaskNode(625);   // Fairbreeze Village, Eversong Woods
            SetTaximaskNode(631);   // Falconwing Square, Eversong Woods
            break;
    }
}

void PlayerTaxi::InitTaxiNodesForFaction(uint32 faction)
{
    // new continent starting masks (It will be accessible only at new map)
    switch (faction)
    {
        case ALLIANCE:
            SetTaximaskNode(100); // Honor Hold
            SetTaximaskNode(245); // Valiance Keep
            break;

        case HORDE:
            SetTaximaskNode(99);  // Thrallmar
            SetTaximaskNode(257); // Warsong Hold
            break;

        default:
            break;
    }
}

void PlayerTaxi::InitTaxiNodesForLvl(uint8 level)
{
    // level dependent taxi hubs
    if (level >= 68) // Shattered Sun Staging Area
    {
        SetTaximaskNode(213);
    }

    if (level >= 78) // Dalaran
    {
        SetTaximaskNode(310);
    }
}

void PlayerTaxi::LoadTaxiMask(const char* data)
{
    Tokens tokens = StrSplit(data, " ");

    int index;
    Tokens::iterator iter;
    for (iter = tokens.begin(), index = 0; (index < TaxiMaskSize) && (iter != tokens.end()); ++iter, ++index)
    {
        // load and set bits only for existing taxi nodes
        m_taximask[index] = sTaxiNodesMask[index] & uint8(std::stoul((*iter).c_str()));
    }
}

void PlayerTaxi::AppendTaximaskTo(ByteBuffer& data, bool all)
{
    data << uint32(TaxiMaskSize);
    if (all)
    {
        for (uint8 i = 0; i < TaxiMaskSize; ++i)
        {
            data << uint8(sTaxiNodesMask[i]);               // all existing nodes
        }
    }
    else
    {
        for (uint8 i = 0; i < TaxiMaskSize; ++i)
        {
            data << uint8(m_taximask[i]);                   // known nodes
        }
    }
}

bool PlayerTaxi::LoadTaxiDestinationsFromString(const std::string& values, Team team)
{
    ClearTaxiDestinations();

    // The faction first, then the nodes (the saver's order). The loader read every token as the
    // faction and then every token, the faction included, as a node, so the integrity check
    // below always failed and a login mid-flight never resumed (P5-B family 5, design §6.7).
    std::vector<uint32> nodes;
    uint32 faction = 0;
    uint32 landing = 0;
    if (!TaxiDestinationsString::Parse(values, faction, nodes, &landing))
    {
        return false;
    }
    m_flightMasterFactionId = faction;
    // Zero for the old form, which carries no stamp: the resume reads that as landed.
    m_landingTime = landing;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        AddTaxiDestination(nodes[i]);
    }

    if (m_TaxiDestinations.empty())
    {
        return true;
    }

    // Check integrity
    if (m_TaxiDestinations.size() < 2)
    {
        return false;
    }

    for (size_t i = 1; i < m_TaxiDestinations.size(); ++i)
    {
        uint32 cost;
        uint32 path;
        sObjectMgr.GetTaxiPath(m_TaxiDestinations[i - 1], m_TaxiDestinations[i], path, cost);
        if (!path)
        {
            return false;
        }
    }

    // can't load taxi path without mount set (quest taxi path?)
    if (!sObjectMgr.GetTaxiMountDisplayId(GetTaxiSource(), team, true))
    {
        return false;
    }

    return true;
}

std::string PlayerTaxi::SaveTaxiDestinationsToString()
{
    if (m_TaxiDestinations.empty())
    {
        return "";
    }

    MANGOS_ASSERT(m_TaxiDestinations.size() >= 2);

    return TaxiDestinationsString::Format(m_flightMasterFactionId, GetTaxiDestinations(), m_landingTime);
}

uint32 PlayerTaxi::GetCurrentTaxiPath() const
{
    if (m_TaxiDestinations.size() < 2)
    {
        return 0;
    }

    uint32 path;
    uint32 cost;

    sObjectMgr.GetTaxiPath(m_TaxiDestinations[0], m_TaxiDestinations[1], path, cost);

    return path;
}

std::ostringstream& operator<< (std::ostringstream& ss, PlayerTaxi const& taxi)
{
    for (int i = 0; i < TaxiMaskSize; ++i)
    {
        ss << uint32(taxi.m_taximask[i]) << " ";    // cast to prevent conversion to char
    }

    return ss;
}

FactionTemplateEntry const* PlayerTaxi::GetFlightMasterFactionTemplate() const
{
    return sFactionTemplateStore.LookupEntry(m_flightMasterFactionId);
}

/**
 * @brief Starts a taxi flight across a sequence of taxi nodes.
 *
 * @param nodes The ordered taxi node path to travel.
 * @param npc The taxi master providing the route, or NULL for spell/scripted travel.
 * @param spellid The spell initiating the taxi flight, if any.
 * @return True if the flight started successfully; otherwise, false.
 */
bool Player::ActivateTaxiPathTo(std::vector<uint32> const& nodes, Creature* npc /*= NULL*/, uint32 spellid /*= 0*/)
{
    if (nodes.size() < 2)
    {
        return false;
    }

    // not let cheating with start flight in time of logout process || if casting not finished || while in combat || if not use Spell's with EffectSendTaxi
    if (GetSession()->isLogingOut() || IsInCombat())
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERBUSY);
        return false;
    }

    // A flight in progress, or a passenger whose Control claim the flight would mask and whose
    // landing grant would be refused under it (design §6.9): feared, confused or possessed. Both
    // origins, since the mover argument holds for a scripted flight too; the reply is the flight
    // master's alone (a spell taxi has no taxi window open to show it).
    if (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE) ||
        Blocked(Motion::ReasonFeared | Motion::ReasonConfused | Motion::ReasonPossessed))
    {
        if (npc)
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERBUSY);
        }
        return false;
    }

    // taximaster case
    if (npc)
    {
        // Stunned or rooted: retail's flight master answers "busy" (the reference §15.6, the taxi
        // notes E.34). A scripted or spell flight on a stunned player is admitted: the kernel
        // flies it (Mobility::Decide returns before the stun and the root for a taxi).
        if (Blocked(Motion::ReasonStunned | Motion::ReasonRooted))
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERBUSY);
            return false;
        }

        // not let cheating with start flight mounted
        if (IsMounted())
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERALREADYMOUNTED);
            return false;
        }

        if (IsInDisallowedMountForm())
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERSHAPESHIFTED);
            return false;
        }

        // not let cheating with start flight in time of logout process || if casting not finished || while in combat || if not use Spell's with EffectSendTaxi
        if (IsNonMeleeSpellCasted(false))
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXIPLAYERBUSY);
            return false;
        }
    }
    // cast case or scripted call case
    else
    {
        RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

        if (IsInDisallowedMountForm())
        {
            RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        }

        if (Spell* spell = GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (spell->m_spellInfo->ID != spellid)
            {
                InterruptSpell(CURRENT_GENERIC_SPELL, false);
            }

        InterruptSpell(CURRENT_AUTOREPEAT_SPELL, false);

        if (Spell* spell = GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (spell->m_spellInfo->ID != spellid)
            {
                InterruptSpell(CURRENT_CHANNELED_SPELL, true);
            }
    }

    uint32 sourcenode = nodes[0];

    // starting node too far away (cheat?)
    TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(sourcenode);
    if (!node)
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXINOSUCHPATH);
        return false;
    }

    // check node starting pos data set case if provided
    if (node->Pos_0 != 0.0f || node->Pos_1 != 0.0f || node->Pos_2 != 0.0f)
    {
        if (node->ContinentID != GetMapId() ||
                (node->Pos_0 - Where().X()) * (node->Pos_0 - Where().X()) +
                (node->Pos_1 - Where().Y()) * (node->Pos_1 - Where().Y()) +
                (node->Pos_2 - Where().Z()) * (node->Pos_2 - Where().Z()) >
                (2 * INTERACTION_DISTANCE) * (2 * INTERACTION_DISTANCE) * (2 * INTERACTION_DISTANCE))
        {
            GetSession()->SendActivateTaxiReply(ERR_TAXITOOFARAWAY);
            return false;
        }
    }
    // node must have pos if taxi master case (npc != NULL)
    else if (npc)
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXIUNSPECIFIEDSERVERERROR);
        return false;
    }

    // Prepare to flight start now

    // stop combat at start taxi flight if any
    CombatStop();

    // stop trade (client cancel trade at taxi map open but cheating tools can be used for reopen it)
    TradeCancel(true);

    // clean not finished taxi path if any
    m_taxi.ClearTaxiDestinations();

    // 0 element current node
    m_taxi.AddTaxiDestination(sourcenode);

    // fill destinations path tail
    uint32 sourcepath = 0;
    uint32 totalcost = 0;
    uint32 firstcost = 0;

    uint32 prevnode = sourcenode;
    uint32 lastnode = 0;

    for (uint32 i = 1; i < nodes.size(); ++i)
    {
        uint32 path, cost;

        lastnode = nodes[i];
        sObjectMgr.GetTaxiPath(prevnode, lastnode, path, cost);

        // A path with no rows (6 of 1,601) or a later hop with a single row cannot be flown:
        // refused here, before the fare is charged (the weld would refuse it after).
        if (!path || path >= sTaxiPathNodesByPath.size() || sTaxiPathNodesByPath[path].size() < (i == 1 ? 1u : 2u))
        {
            m_taxi.ClearTaxiDestinations();
            return false;
        }

        totalcost += cost;

        if (i == 1)
        {
            firstcost = cost;
        }

        if (prevnode == sourcenode)
        {
            sourcepath = path;
        }

        m_taxi.AddTaxiDestination(lastnode);

        prevnode = lastnode;
    }

    // get mount model (in case non taximaster (npc==NULL) allow more wide lookup)
    uint32 mount_display_id = sObjectMgr.GetTaxiMountDisplayId(sourcenode, GetTeam(), npc == NULL);

    // in spell case allow 0 model
    if ((mount_display_id == 0 && spellid == 0) || sourcepath == 0)
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXIUNSPECIFIEDSERVERERROR);

        m_taxi.ClearTaxiDestinations();
        return false;
    }

    uint64 money = GetMoney();

    if (npc)
    {
        float discount = GetReputationPriceDiscount(npc);

        totalcost = uint32(ceil(totalcost * discount));
        firstcost = uint32(ceil(firstcost * discount));

        m_taxi.SetFlightMasterFactionTemplateId(npc->getFaction());
    }
    else
    {
        m_taxi.SetFlightMasterFactionTemplateId(0);
    }

    if (money < totalcost)
    {
        GetSession()->SendActivateTaxiReply(ERR_TAXINOTENOUGHMONEY);

        m_taxi.ClearTaxiDestinations();
        return false;
    }

    // Checks and preparations done, DO FLIGHT
    ModifyMoney(-(int64)totalcost);
    GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_GOLD_SPENT_FOR_TRAVELLING, totalcost);
    GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_FLIGHT_PATHS_TAKEN, 1);

    // prevent stealth flight
    RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

    GetSession()->SendActivateTaxiReply(ERR_TAXIOK);
    GetSession()->SendDoFlight(mount_display_id, m_taxi.GetTaxiDestinations(), 0);

    return true;
}

/**
 * @brief Starts a taxi flight using a direct taxi path identifier.
 *
 * @param taxi_path_id The taxi path identifier to use.
 * @param spellid The spell initiating the taxi flight, if any.
 * @return True if the flight started successfully; otherwise, false.
 */
bool Player::ActivateTaxiPathTo(uint32 taxi_path_id, uint32 spellid /*= 0*/)
{
    TaxiPathEntry const* entry = sTaxiPathStore.LookupEntry(taxi_path_id);
    if (!entry)
    {
        return false;
    }

    std::vector<uint32> nodes;

    nodes.resize(2);
    nodes[0] = entry->FromTaxiNode;
    nodes[1] = entry->ToTaxiNode;

    return ActivateTaxiPathTo(nodes, NULL, spellid);
}

/**
 * @brief The hops welded into one node array: the ONE weld, read both by the spline
 *        (MotionMaster::MoveTaxiFlight) and by the landing-time resume below.
 *
 * It lived inside MoveTaxiFlight until the resume needed to measure the same polyline the
 * spline is built from. Duplicating it there would have been exactly the "re-derive it
 * separately" the design forbids, so it moved out; MoveTaxiFlight now calls this and copies
 * the result into the kernel's own node type.
 */
bool TaxiRoute::Weld(std::vector<uint32> const& route, std::vector<TaxiRouteNode>& out)
{
    out.clear();
    if (route.size() < 2)
    {
        return false;
    }
    for (size_t hop = 1; hop < route.size(); ++hop)
    {
        uint32 path = 0;
        uint32 cost = 0;
        sObjectMgr.GetTaxiPath(route[hop - 1], route[hop], path, cost);
        // A path with no rows, or a later hop with a single row, cannot be flown: the outgoing
        // hop's node 0 is dropped (the hop chaining's pathNode = 1 skipped it), so a one-row
        // later hop would contribute nothing at all.
        if (!path || path >= sTaxiPathNodesByPath.size() || sTaxiPathNodesByPath[path].size() < (hop == 1 ? 1u : 2u))
        {
            out.clear();
            return false;
        }
        TaxiPathNodeList const& rows = sTaxiPathNodesByPath[path];
        for (size_t i = (hop == 1 ? 0 : 1); i < rows.size(); ++i)
        {
            TaxiPathNodeEntry const& row = rows[i];
            TaxiRouteNode node;
            node.mapId = row.ContinentID;
            node.x = row.Loc_0;
            node.y = row.Loc_1;
            node.z = row.Loc_2;
            node.arrivalEvent = row.ArrivalEventID;
            node.departureEvent = row.DepartureEventID;
            out.push_back(node);
        }
        if (hop + 1 < route.size())
        {
            out.back().seam = true;   // the incoming hop's last row is the hub, kept once
        }
    }
    return !out.empty();
}

/**
 * @brief Resumes an interrupted taxi flight -- or ends it -- by the landing time it was
 *        stamped with at the takeoff.
 *
 * ONE check, and all three callers come through here: the login (CharacterHandler), the
 * battleground return and the dungeon return (both Player::ProcessDelayedOperations, via
 * TeleportToBGEntryPoint). The user's model, design 2026-09-22 §2: a flight is a paid contract
 * to the destination. The gold went at the click, so
 *
 *     now <  landing  ->  still in the air: back on the mount at the point the route has
 *                         reached by now, and on from there;
 *     now >= landing  ->  the flight is over: he is put down at the destination he paid for,
 *                         on whatever map it is, and the taxi state is cleared.
 *
 * A twenty-second dungeon pop on a five-minute route resumes mid-air; a fifteen-minute
 * battleground on a one-minute flight lands. The cores' "dropped at the next waypoint" is
 * rejected, and so is the position-based search this used to do: it put a passenger back on
 * the leg he left however long ago he left it.
 */
void Player::ContinueTaxiFlight()
{
    const uint32 sourceNode = m_taxi.GetTaxiSource();
    if (!sourceNode)
    {
        return;
    }

    DEBUG_LOG("WORLD: Restart character %u taxi flight", GetGUIDLow());

    std::vector<TaxiRouteNode> nodes;
    if (!TaxiRoute::Weld(m_taxi.GetTaxiDestinations(), nodes))
    {
        sLog.outError("Character %u resumes a taxi flight over a missing path from node %u; the route is dropped", GetGUIDLow(), sourceNode);
        m_taxi.ClearTaxiDestinations();
        return;
    }

    const float speed = sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_TAXI_SPEED);
    const TaxiResume::Resume resume = TaxiResume::Decide(nodes, speed,
                                                         int64(sWorld.GetGameTime()),
                                                         int64(m_taxi.GetLandingTime()),
                                                         GetMapId());

    if (resume.verdict == TaxiResume::Verdict::NoRoute)
    {
        sLog.outError("Character %u resumes a taxi flight over path nodes none of which is on map %u; the route is dropped", GetGUIDLow(), GetMapId());
        m_taxi.ClearTaxiDestinations();
        return;
    }

    if (resume.verdict == TaxiResume::Verdict::Landed)
    {
        // The contract is honoured and the flight is not re-flown. The destination's TaxiNodes
        // position is the one the landing itself uses (the sniffed SMSG_MOVE_TELEPORT sits
        // 2.19 yd below the last path node); a node without one falls back to that row.
        float x = resume.x;
        float y = resume.y;
        float z = resume.z;
        uint32 mapId = resume.mapId;
        if (TaxiNodesEntry const* destination = sTaxiNodesStore.LookupEntry(m_taxi.GetTaxiDestinations().back()))
        {
            if (destination->Pos_0 != 0.0f || destination->Pos_1 != 0.0f || destination->Pos_2 != 0.0f)
            {
                mapId = destination->ContinentID;
                x = destination->Pos_0;
                y = destination->Pos_1;
                z = destination->Pos_2;
            }
        }
        DEBUG_LOG("WORLD: Character %u's flight ended while he was away (%.0f of %.0f yd flown); he is put down at node %u",
                  GetGUIDLow(), resume.flown, resume.total, m_taxi.GetTaxiDestinations().back());
        // Cleared BEFORE the teleport: a far teleport on a player who still reads as flying
        // would take the flight's own branches, and there is no flight left to take them.
        m_taxi.ClearTaxiDestinations();
        TeleportTo(mapId, x, y, z, Where().Facing());
        return;
    }

    const uint32 mountDisplayId = sObjectMgr.GetTaxiMountDisplayId(sourceNode, GetTeam(), true);

    // WHAT WAS BUILT, and it is the design's own named fallback (§2): he goes back on the mount
    // HERE, where he stands, and flies to the node the clock chose -- not to the one nearest
    // him, which is what this function used to pick and which put a passenger back on the leg he
    // left however long ago he left it.
    //
    // He is NOT snapped onto the elapsed point itself, and that is deliberate rather than
    // unfinished. A same-map TeleportTo does not move a player here: Player::SendTeleportPacket
    // names the destination to the client and then puts the server's own position back where it
    // was (Player.cpp:1679), because the move belongs to CMSG_MOVE_TELEPORT_ACK. The spline laid
    // two lines below would therefore start from the OLD position anyway, and the client would
    // be told to stand at one place and then flown from another. Choosing the node by time
    // already skips every leg the contract says he has flown; the remaining error is at most the
    // part of one leg he is short of, and the landing time he arrives on is unchanged either way.
    DEBUG_LOG("WORLD: Character %u resumes his flight at welded node %u of %u (%.0f of %.0f yd flown%s; the clock put him at %.0f, %.0f, %.0f)",
              GetGUIDLow(), uint32(resume.node), uint32(nodes.size()), resume.flown, resume.total,
              resume.clamped ? ", held at his map's last node" : "", resume.x, resume.y, resume.z);

    GetSession()->SendDoFlight(mountDisplayId, m_taxi.GetTaxiDestinations(), uint32(resume.node));
}

// ---- the taxi's six operations (P5-B family 5) ----------------------------------------------
// The kernel's TaxiBehaviour (src/motion/TaxiMove.cpp) says WHEN; these say HOW, each in one
// place and in retail's order (design/2026-09-18-movement-p5b5-taxi-design.md §5).

/**
 * @brief The takeoff: the stop, the control taken, the hostile references offline, the pet
 *        unsummoned, the mount display written without UNIT_FLAG_MOUNT, the flight flags set.
 * @param mountDisplayId The taxi mount's display id (0 for a spell taxi without one).
 */
void Player::TaxiTakeoff(uint32 mountDisplayId)
{
    // Retail's order (the family's notes A.6-A.7, A.11): a stop, the control update with
    // AllowMove = 0, then UNIT_FIELD_FLAGS and the mount display in one update. UNIT_FLAG_MOUNT
    // is not set on a taxi (0x10000C on the wire), so Unit::Mount is not used; what it did
    // beside the flag is done here.
    StopMoving();
    // The stop retail actually sends here, and StopMoving does not: the passenger has been
    // standing still under his own control, so no spline exists to cancel and Stop() returns
    // early. Retail sends a point-carrying stop anyway, immediately before the revoke -- the
    // `(stop, revoke)` pair of peer/retail-taxi-flights-2026-09-22.md §2 -- pinning where he is
    // at the instant the wheel is taken from him. Backlog §4.7, design 2026-09-22 §3.
    Movement::MoveSplineInit(*this).StopHere();
    SetClientControl(this, 0);
    GetHostileRefManager().setOnlineOfflineState(false);
    UnsummonPetTemporaryIfAny();
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MOUNTING);
    SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, mountDisplayId);
    SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT);
}

/**
 * @brief A route hop's seam was left: the route advances; a taxi cheater learns the hub.
 */
void Player::TaxiSeamPassed()
{
    if (m_taxi.empty())
    {
        return;   // a route cleared under a flight (a replaced flight's abort): nothing to advance
    }
    m_taxi.NextTaxiDestination();
    // The hub joins a taxi cheater's mask, as the hop chaining did: a cheater who lands with
    // the cheat off must still have a flight back.
    const uint32 hub = m_taxi.GetTaxiSource();
    if (hub && IsTaxiCheater() && m_taxi.SetTaximaskNode(hub))
    {
        WorldPacket data(SMSG_NEW_TAXI_PATH, 0);
        GetSession()->SendPacket(&data);
    }
}

/**
 * @brief The map crossing: the far teleport onto the next map's first path node.
 * @return False when the teleport was refused (the map cannot be entered).
 */
bool Player::TaxiCross(uint32 mapId, float x, float y, float z, float o)
{
    // Inside the motion update the teleport is deferred (SetDelayedTeleportFlagIfCan) and runs
    // at the end of Update(); the Taxi binding survives the map change and
    // WorldSession::HandleMoveWorldportAckOpcode resumes it through MotionMaster::TaxiContinue.
    return TeleportTo(mapId, x, y, z, o);
}

/**
 * @brief Stores the landing for Update() to perform once the teleport-deferral window has closed.
 */
void Player::ScheduleTaxiLanding(bool snap, float x, float y, float z, float o)
{
    m_taxiLandingPending = true;
    m_taxiLandingSnap = snap;
    m_taxiLanding = WorldLocation(GetMapId(), x, y, z, o);
}

/**
 * @brief The landing, in retail's order: the stop, control on, the teleport onto the TaxiNodes
 *        position, the flags and the mount display cleared, the pet back; then the server's own
 *        bookkeeping (the hostile references, the hostile-area spell, the route).
 */
void Player::PerformTaxiLanding()
{
    if (!m_taxiLandingPending)
    {
        return;
    }
    // Taken before the first step: a re-entry from a hook below finds nothing pending, and an
    // abort in between (TaxiAbort) clears the slot, so a landing runs once or never.
    m_taxiLandingPending = false;
    const bool snap = m_taxiLandingSnap;
    const WorldLocation where = m_taxiLanding;

    // The sniffed flight: a stop spline at the last path node, AllowMove = 1, SMSG_MOVE_TELEPORT
    // onto the node's TaxiNodes position (2.19 yd below the path, a 1 ms fall, no damage: the
    // near teleport resets the fall reference), the flags cleared and the mount display zeroed
    // in one update, the pet resummoned.
    //
    // STOP BEFORE GRANT, correcting the notes' A.11: the capture file has packets 205463 (stop),
    // 205464 (grant), 205465 (stop), 205466 (grant), so the landing is the exact mirror of the
    // takeoff's `(stop, revoke)` -- peer/retail-taxi-flights-2026-09-22.md §7. The flight's
    // spline expired ~180 ms ago, so StopMoving below sends nothing whatever it is passed;
    // StopHere is the form that goes out with nothing to cancel (§3.3).
    Movement::MoveSplineInit(*this).StopHere();
    SetClientControl(this, 1);
    StopMoving(true);
    // A teleport deferred earlier in this update (an aura's) wins over the snap: the near
    // teleport below would overwrite its destination and clear its flag.
    if (snap && !IsHasDelayedTeleport())
    {
        TeleportTo(where, TELE_TO_NOT_LEAVE_COMBAT | TELE_TO_NOT_UNSUMMON_PET);
    }
    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT);
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_MOUNTED);
    SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);
    ResummonPetTemporaryUnSummonedIfAny();
    // The generator's Finalize, which ran these only when the client's spline-done packet had
    // already emptied the route (the race, design fact 5): now every landing runs them.
    GetHostileRefManager().setOnlineOfflineState(true);
    if (pvpInfo.inHostileArea)
    {
        CastSpell(this, 2479, true);
    }
    m_taxi.ClearTaxiDestinations();
}

/**
 * @brief Every non-landing end of a flight: the flags and the mount display cleared, the pet
 *        back, the hostile references online, the route cleared, the control returned.
 */
void Player::TaxiAbort()
{
    // The flight's end published early (the commit that finishes it agrees at its end): the pet's
    // resummon and the hostile-state change below must not see a flight in progress.
    GetMotionMaster()->PublishTaxiEnded();
    m_taxiLandingPending = false;
    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT);
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_MOUNTED);
    SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);
    ResummonPetTemporaryUnSummonedIfAny();
    GetHostileRefManager().setOnlineOfflineState(true);
    m_taxi.ClearTaxiDestinations();
    // Every abort returns the client its mover, death included: no death or repop path grants
    // it, and a ghost whose mover the takeoff revoked could not move (design §6.6). The grant
    // is refused on its own while fleeing or confused, states the boarding refuses.
    SetClientControl(this, 1);
}
