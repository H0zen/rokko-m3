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
 * @file GMCommands.cpp
 * @brief Implementation of general GM (Game Master) utility chat commands.
 *
 * This file contains chat command handlers for basic GM operations including:
 * - Help and command information display
 * - GM level and security queries
 * - General purpose administrative utilities
 */

#include <cstdio>
#include <string>
#include <list>
#include "Chat.h"
#include "DBCStores.h"
#include "Language.h"
#include "PlayerRegistry.h"
#include "Player.h"
#include "SpellMgr.h"
#include "Util.h"
#include "Weather.h"
#include "World.h"
#include "WorldSession.h"

/**
 * @brief Handler for HandlePInfoCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandlePInfoCommand(char* args)
{
    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&args, &target, &target_guid, &target_name))
    {
        return false;
    }

    uint32 accId = 0;
    uint32 money = 0;
    uint32 total_player_time = 0;
    uint32 level = 0;
    uint32 latencyHome = 0;
    uint32 latencyWorld = 0;

    // get additional information from Player object
    if (target)
    {
        // check online security
        if (HasLowerSecurity(target))
        {
            return false;
        }

        accId = target->GetSession()->GetAccountId();
        money = target->GetMoney();
        total_player_time = target->GetTotalPlayedTime();
        level = target->getLevel();
        // Both, because the client measures and displays both.
        latencyHome  = target->GetSession()->GetLatency(proto::LinkSlot::Zero);
        latencyWorld = target->GetSession()->GetLatency(proto::LinkSlot::One);
    }
    // get additional information from DB
    else
    {
        // check offline security
        if (HasLowerSecurity(NULL, target_guid))
        {
            return false;
        }

        //                                                     0          1      2      3
        QueryResult* result = CharacterDatabase.PQuery("SELECT `totaltime`, `level`, `money`, `account` FROM `characters` WHERE `guid` = '%u'", target_guid.GetCounter());
        if (!result)
        {
            return false;
        }

        Field* fields = result->Fetch();
        total_player_time = fields[0].GetUInt32();
        level = fields[1].GetUInt32();
        money = fields[2].GetUInt32();
        accId = fields[3].GetUInt32();
        delete result;
    }

    std::string username = GetMangosString(LANG_ERROR);
    std::string last_ip = GetMangosString(LANG_ERROR);
    AccountTypes security = SEC_PLAYER;
    std::string last_login = GetMangosString(LANG_ERROR);

    QueryResult* result = LoginDatabase.PQuery("SELECT `username`,`gmlevel`,`last_ip`,`last_login` FROM `account` WHERE `id` = '%u'", accId);
    if (result)
    {
        Field* fields = result->Fetch();
        username = fields[0].GetCppString();
        security = (AccountTypes)fields[1].GetUInt32();

        if (GetAccessLevel() >= security)
        {
            last_ip = fields[2].GetCppString();
            last_login = fields[3].GetCppString();
        }
        else
        {
            last_ip = "-";
            last_login = "-";
        }

        delete result;
    }

    std::string nameLink = playerLink(target_name);

    // Nine arguments for nine conversions. LANG_PINFO_ACCOUNT carries no
    // e-mail field -- the shipped string drops it, and its translations drop
    // it too -- but this call passed one anyway. Every argument after it
    // landed one place early: "Last login" printed the address, and "Latency"
    // read a char* through %u and printed a piece of a pointer, which is why
    // it came out as a different absurd number every time it was run.
    PSendSysMessage(LANG_PINFO_ACCOUNT, (target ? "" : GetMangosString(LANG_OFFLINE)),
                    nameLink.c_str(), target_guid.GetCounter(), username.c_str(),
                    accId, security, last_ip.c_str(), last_login.c_str(),
                    latencyHome, latencyWorld);

    if (target)
    {
        Motion::TimeBase const& timeBase = target->GetSession()->TimeBase();
        uint32 const now = GameTime::GetGameTimeMS();
        Motion::TimeBaseCounters const& counters = timeBase.Counters();
        PSendSysMessage("Clock: delta %u ms, rtt %u ms, samples %u, slewed %u, jumped %u, too old %u, unknown %u, fallbacks %u, %s, bad packets %u",
                        timeBase.Delta(), timeBase.LastRtt(), counters.samples, counters.slewed, counters.jumped,
                        counters.tooOld, counters.unknownCounter, counters.fallbacks,
                        timeBase.Acquired(now) ? "acquired" : "not acquired", target->GetSession()->GetBadPacketCount());

        WorldSession::AckCounters const& acks = target->GetSession()->GetAckCounters();
        std::string pending;
        std::vector<Motion::PendingChange> const entries = target->MotionState().Pending().All();
        for (size_t i = 0; i < entries.size(); ++i)
        {
            char one[64];
            snprintf(one, sizeof(one), "%s%s#%u %u ms", i ? ", " : "", Motion::ChangeName(entries[i].type), entries[i].counter, now - entries[i].sentAt);
            pending += one;
        }
        PSendSysMessage("Acks: seen %u, matched %u, mismatched %u, resent %u, tombstone %u, stale %u, future %u, wrong guid %u, unverified %u, teleporting %u; pending: %s; last %s; dropped emissions %u",
                        acks.seen, acks.matched, acks.mismatched, acks.resent, acks.tombstone, acks.stale, acks.future, acks.wrongGuid, acks.unverified, acks.teleporting,
                        pending.empty() ? "none" : pending.c_str(), Motion::AckResultName(target->MotionState().LastAck()), target->GetMotionDropped());

        Motion::Authority const& movers = target->GetSession()->Movers();
        Motion::AuthorityCounters const& m = movers.Counters();
        std::string selected = "none";
        if (movers.Selected() == target->GetObjectGuid().GetRawValue())
        {
            selected = "self";
        }
        else if (movers.Selected() != 0)
        {
            selected = ObjectGuid(movers.Selected()).GetString();
        }
        PSendSysMessage("Mover: selected %s, members %u; added %u, removed %u, selected %u, deselected %u, bad select %u, bad deselect %u, not active %u, not member %u, unresolved %u",
                        selected.c_str(), uint32(movers.Members().size()), m.added, m.removed, m.selected, m.deselected,
                        m.badSelect, m.badDeselect, m.notActive, m.notMember, m.unresolved);
    }

    std::string timeStr = secsToTimeString(total_player_time, TimeFormat::ShortText, true);
    uint32 gold = money / GOLD;
    uint32 silv = (money % GOLD) / SILVER;
    uint32 copp = (money % GOLD) % SILVER;

    // LANG_PINFO_LEVEL asks for the purse as one string, not three numbers.
    // Passing the gold count to that %s dereferenced it as a pointer: harmless
    // only while the character held under a gold, where the count is zero and
    // glibc prints "(null)". At one gold or more it is a wild read, and
    // .pinfo on an ordinary player took the world server down with it.
    char moneyStr[64];
    snprintf(moneyStr, sizeof(moneyStr), "%ug%us%uc", gold, silv, copp);
    PSendSysMessage(LANG_PINFO_LEVEL, timeStr.c_str(), level, moneyStr);
//    if (target)
//    {
//        uint32 mapId = target->GetMapId();
//        uint32 zoneId = target->GetTerrain()->GetZoneId(target->Where().X(), target->Where().Y(), target->Where().Z());
//        float posX = target->Where().X();
//        float posY = target->Where().Y();
//        float posZ = target->Where().Z();
//        float orientation = target->Where().Facing();
//
//        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
//        AreaTableEntry const* zoneEntry = GetAreaEntryByAreaID(zoneId);
//
//        PSendSysMessage("Location: Map %u (%s), Zone %u (%s)",
//                        mapId,
//                        (mapEntry ? mapEntry->name[GetSessionDbcLocale()] : "<unknown>"),
//                        zoneId,
//                        (zoneEntry ? zoneEntry->area_name[GetSessionDbcLocale()] : "<unknown>"));
//
//        PSendSysMessage("Coordinates: X=%.2f Y=%.2f Z=%.2f O=%.2f",
//                        posX, posY, posZ, orientation);
//    }
    return true;
}

/**
 * @brief Handler for HandleWaterwalkCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleWaterwalkCommand(char* args)
{
    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    Player* player = getSelectedPlayer();

    if (!player)
    {
        PSendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    // check online security
    if (HasLowerSecurity(player))
    {
        return false;
    }

    if (value)
    {
        player->SetWaterWalk(true); // ON
    }
    else
    {
        player->SetWaterWalk(false); // OFF
    }

    PSendSysMessage(LANG_YOU_SET_WATERWALK, args, GetNameLink(player).c_str());
    if (needReportToTarget(player))
    {
        ChatHandler(player).PSendSysMessage(LANG_YOUR_WATERWALK_SET, args, GetNameLink().c_str());
    }
    return true;
}

/**
 * @brief Handler for HandleGMCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleGMCommand(char* args)
{
    if (!*args)
    {
        if (m_session->GetPlayer()->isGameMaster())
        {
            m_session->SendNotification(LANG_GM_ON);
        }
        else
        {
            m_session->SendNotification(LANG_GM_OFF);
        }
        return true;
    }

    std::string argstr = (char*)args;

    if (argstr == "on")
    {
        m_session->GetPlayer()->SetGameMaster(true);
        m_session->SendNotification(LANG_GM_ON);
        return true;
    }

    if (argstr == "off")
    {
        m_session->GetPlayer()->SetGameMaster(false);
        m_session->SendNotification(LANG_GM_OFF);
        return true;
    }

    SendSysMessage(LANG_USE_BOL);
    SetSentErrorMessage(true);
    return false;
}

/**
 * @brief Handler for HandleGMVisibleCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleGMVisibleCommand(char* args)
{
    if (!*args)
    {
        PSendSysMessage(LANG_YOU_ARE, m_session->GetPlayer()->isGMVisible() ?  GetMangosString(LANG_VISIBLE) : GetMangosString(LANG_INVISIBLE));
        return true;
    }

    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    Player* player = m_session->GetPlayer();
    SpellEntry const* invisibleAuraInfo = sSpellStore.LookupEntry(sWorld.getConfig(CONFIG_UINT32_GM_INVISIBLE_AURA));
    if (!invisibleAuraInfo || !IsSpellAppliesAura(invisibleAuraInfo))
    {
        invisibleAuraInfo = NULL;
    }

    if (value)
    {
        player->SetGMVisible(true);
        m_session->SendNotification(LANG_INVISIBLE_VISIBLE);
        if (invisibleAuraInfo)
        {
            player->RemoveAurasDueToSpell(invisibleAuraInfo->ID);
        }
    }
    else
    {
        m_session->SendNotification(LANG_INVISIBLE_INVISIBLE);
        player->SetGMVisible(false);
        if (invisibleAuraInfo)
        {
            player->CastSpell(player, invisibleAuraInfo, true);
        }
    }

    return true;
}

/**
 * @brief Handler for HandleGMFlyCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleGMFlyCommand(char* args)
{
    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    Player* target = getSelectedPlayer();
    if (!target)
    {
        target = m_session->GetPlayer();
    }

    // [-ZERO] Need reimplement in another way
    // GM fly wil be achieved with the swimming moveflag
    // Warning : Still buggy when Jump
    target->SetCanFly(value);
    PSendSysMessage(LANG_COMMAND_FLYMODE_STATUS, GetNameLink(target).c_str(), args);
    return true;
}

/**
 * @brief Handler for HandleGMListIngameCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleGMListIngameCommand(char* /*args*/)
{
    std::list< std::pair<std::string, bool> > names;
    sPlayerRegistry.ForEach([&names, this](Player *player)
    {
        AccountTypes security = player->GetSession()->GetSecurity();
        if ((player->isGameMaster() || (security > SEC_PLAYER && security <= (AccountTypes)sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_GM_LIST))) &&
            (!m_session || player->IsVisibleGloballyFor(m_session->GetPlayer())))
        {
            names.push_back(std::make_pair<std::string, bool>(GetNameLink(player), player->isAcceptWhispers()));
        }
    });

    if (!names.empty())
    {
        SendSysMessage(LANG_GMS_ON_SRV);

        char const* accepts = GetMangosString(LANG_GM_ACCEPTS_WHISPER);
        char const* not_accept = GetMangosString(LANG_GM_NO_WHISPER);
        for (std::list<std::pair< std::string, bool> >::const_iterator iter = names.begin(); iter != names.end(); ++iter)
        {
            PSendSysMessage("%s - %s", iter->first.c_str(), iter->second ? accepts : not_accept);
        }
    }
    else
    {
        SendSysMessage(LANG_GMS_NOT_LOGGED);
    }

    return true;
}

/**
 * @brief Handler for HandleGMListFullCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleGMListFullCommand(char* /*args*/)
{
    ///- Get the accounts with GM Level >0
    QueryResult* result = LoginDatabase.Query("SELECT `username`,`gmlevel` FROM `account` WHERE `gmlevel` > 0");
    if (result)
    {
        SendSysMessage(LANG_GMLIST);
        SendSysMessage("========================");
        SendSysMessage(LANG_GMLIST_HEADER);
        SendSysMessage("========================");

        ///- Circle through them. Display username and GM level
        do
        {
            Field* fields = result->Fetch();
            PSendSysMessage("|%15s|%6s|", fields[0].GetString(), fields[1].GetString());
        }
        while (result->NextRow());

        PSendSysMessage("========================");
        delete result;
    }
    else
    {
        PSendSysMessage(LANG_GMLIST_EMPTY);
    }
    return true;
}

/**
 * @brief Handler for HandleModifyStandStateCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleModifyStandStateCommand(char* args)
{
    uint32 anim_id;
    if (!ExtractUInt32(&args, anim_id))
    {
        return false;
    }

    if (!sEmotesStore.LookupEntry(anim_id))
    {
        return false;
    }

    m_session->GetPlayer()->HandleEmoteState(anim_id);

    return true;
}

/**
 * @brief Handler for HandleChangeWeatherCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleChangeWeatherCommand(char* args)
{
    // Weather is OFF
    if (!sWorld.getConfig(CONFIG_BOOL_WEATHER))
    {
        SendSysMessage(LANG_WEATHER_DISABLED);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 type;
    if (!ExtractUInt32(&args, type))
    {
        return false;
    }

    // see enum WeatherType
    if (!Weather::IsValidWeatherType(type))
    {
        return false;
    }

    float grade;
    if (!ExtractFloat(&args, grade))
    {
        return false;
    }

    // 0 to 1, sending -1 is instant good weather
    if (grade < 0.0f || grade > 1.0f)
    {
        return false;
    }

    Player* player = m_session->GetPlayer();
    uint32 zoneId = player->GetTerrain()->GetZoneId(player->Where().X(), player->Where().Y(), player->Where().Z());
    if (!sWeatherMgr.GetWeatherChances(zoneId))
    {
        SendSysMessage(LANG_NO_WEATHER);
        SetSentErrorMessage(true);
    }
    player->GetMap()->SetWeather(zoneId, (WeatherType)type, grade, false);

    return true;
}

// Internal shortcut function to freeze a player
bool freezePlayer(Player* player, WorldObject* caster)
{
    SpellEntry const* spellInfo = sSpellStore.LookupEntry(SPELL_GM_FREEZE);
    return AddAuraToPlayer(spellInfo, player, caster);
}

// Internal shortcut function to freeze a player
void unFreezePlayer(Player* player)
{
    player->RemoveAurasDueToSpell(SPELL_GM_FREEZE);
}

/**
 * @brief Handler for HandleFreezePlayerCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleFreezePlayerCommand(char* args)
{
    Player* targetPlayer = nullptr;

    // 1. Try to extract player name from args if not empty
    if (*args)
    {
        char* playerName = ExtractLiteralArg(&args);

        if (!ExtractPlayerTarget(&playerName, &targetPlayer))
        {
            SendSysMessage(LANG_COMMAND_FREEZE_PLAYER_PLAYER_NOT_FOUND);
            SetSentErrorMessage(true);
            return false;
        }
    }

    // 2. If arg is empty, gets the current selected target (returns current player if no unit selected)
    if (!targetPlayer)
    {
        Unit* selectedTtarget = getSelectedPlayer();

        if (!selectedTtarget)
        {
            SendSysMessage(LANG_NO_CHAR_SELECTED);
            SetSentErrorMessage(true);
            return false;
        }
        targetPlayer = (Player*)selectedTtarget;
    }


    const char* targetName = targetPlayer->GetName();
    Player * currentGM = m_session->GetPlayer();

    // Prevent freezing yourself !
    if (targetPlayer == currentGM)
    {
        SendSysMessage(LANG_NO_CHAR_SELECTED);
        SendSysMessage(LANG_COMMAND_FREEZE_PLAYER_CANNOT_FREEZE_YOURSELF);
        SetSentErrorMessage(true);
        return false;
    }

    // Check if target can be freezed
    if (targetPlayer->GetSession()->GetSecurity() > m_session->GetSecurity())
    {
        PSendSysMessage(LANG_COMMAND_FREEZE_PLAYER_CANNOT_FREEZE_HIGHER_SECLEVEL, targetName);
        SetSentErrorMessage(true);
        return false;
    }

    freezePlayer(targetPlayer, currentGM);

    // Notif GM
    PSendSysMessage(LANG_COMMAND_FREEZE_PLAYER, targetName);

    // Send message to player to prevent he has been frozen
    ChatHandler(targetPlayer).SendSysMessage(LANG_COMMAND_FREEZE_PLAYER_YOU_HAVE_BEEN_FROZEN);

    return true;
}

/**
 * @brief Handler for HandleUnfreezePlayerCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleUnfreezePlayerCommand(char* args)
{

    Player* targetPlayer = nullptr;

    // 1. Try to extract player name from args if not empty
    if (*args)
    {
        char* playerName = ExtractLiteralArg(&args);

        if (!ExtractPlayerTarget(&playerName, &targetPlayer))
        {
            SendSysMessage(LANG_COMMAND_UNFREEZE_PLAYER_PLAYER_NOT_FOUND);
            SetSentErrorMessage(true);
            return false;
        }
    }

    // 2. If arg is empty, gets the current selected target (returns current player if no unit selected)
    if (!targetPlayer)
    {
        Unit* selectedTtarget = getSelectedPlayer();

        if (!selectedTtarget)
        {
            SendSysMessage(LANG_NO_CHAR_SELECTED);
            SetSentErrorMessage(true);
            return false;
        }
        targetPlayer = (Player*)selectedTtarget;
    }

    unFreezePlayer(targetPlayer);

    // Notif GM
    PSendSysMessage(LANG_COMMAND_UNFREEZE_PLAYER, targetPlayer->GetName());

    // Send message to player to prevent he has been unfrozen
    ChatHandler(targetPlayer).SendSysMessage(LANG_COMMAND_FREEZE_PLAYER_YOU_HAVE_BEEN_UNFROZEN);

    return true;
}
