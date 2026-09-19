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
 * @file PlayerAndCreatureCommands.cpp
 * @brief Implementation of player and creature interaction chat commands.
 *
 * This file contains chat command handlers for interactions including:
 * - Player and creature following
 * - Movement commands
 * - Unit state management
 */

#include "Chat.h"
#include "Language.h"
#include "ObjectLookup.h"
#include "Geometry/Vector3.h"
#include "WorldSession.h"
#include "Unit.h"
#include "Player.h"
#include "DBCStores.h"
#include <cstdio>
#include <string>

/**
 * @brief Handler for HandleDeMorphCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleDeMorphCommand(char* /*args*/)
{
    Unit* target = getSelectedUnit();
    if (!target)
    {
        target = m_session->GetPlayer();
    }

    // check online security
    else if (target->GetTypeId() == TYPEID_PLAYER && HasLowerSecurity((Player*)target))
    {
        return false;
    }

    target->DeMorph();

    return true;
}

/**
 * @brief Handler for HandleModifyMorphCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleModifyMorphCommand(char* args)
{
    if (!*args)
    {
        return false;
    }

    uint32 display_id = (uint32)atoi(args);

    CreatureDisplayInfoEntry const* displayEntry = sCreatureDisplayInfoStore.LookupEntry(display_id);
    if (!displayEntry)
    {
        SendSysMessage(LANG_BAD_VALUE);
        SetSentErrorMessage(true);
        return false;
    }

    Unit* target = getSelectedUnit();
    if (!target)
    {
        target = m_session->GetPlayer();
    }

    // check online security
    else if (target->GetTypeId() == TYPEID_PLAYER && HasLowerSecurity((Player*)target))
    {
        return false;
    }

    target->SetDisplayId(display_id);

    return true;
}

/**
 * @brief Handler for HandleDamageCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleDamageCommand(char* args)
{
    if (!*args)
    {
        return false;
    }

    Unit* target = getSelectedUnit();
    Player* player = m_session->GetPlayer();

    if (!target || !player->GetSelectionGuid())
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    if (!target->IsAlive())
    {
        return true;
    }

    int32 damage_int;
    if (!ExtractInt32(&args, damage_int))
    {
        return false;
    }

    if (damage_int <= 0)
    {
        return true;
    }

    uint32 damage = damage_int;

    // flat melee damage without resistance/etc reduction
    if (!*args)
    {
        player->DealDamage(target, damage, NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
        if (target != player)
        {
            player->SendAttackStateUpdate(HITINFO_NORMALSWING2, target, SPELL_SCHOOL_MASK_NORMAL, damage, 0, 0, VICTIMSTATE_NORMAL, 0);
        }
        return true;
    }

    uint32 school;
    if (!ExtractUInt32(&args, school))
    {
        return false;
    }

    if (school >= MAX_SPELL_SCHOOL)
    {
        return false;
    }

    SpellSchoolMask schoolmask = SpellSchoolMask(1 << school);

    if (schoolmask & SPELL_SCHOOL_MASK_NORMAL)
    {
        damage = player->CalcArmorReducedDamage(target, damage);
    }

    // melee damage by specific school
    if (!*args)
    {
        uint32 absorb = 0;
        uint32 resist = 0;

        target->CalculateDamageAbsorbAndResist(player, schoolmask, SPELL_DIRECT_DAMAGE, damage, &absorb, &resist);

        if (damage <= absorb + resist)
        {
            return true;
        }

        damage -= absorb + resist;

        player->DealDamageMods(target, damage, &absorb);
        player->DealDamage(target, damage, NULL, DIRECT_DAMAGE, schoolmask, NULL, false);
        player->SendAttackStateUpdate(HITINFO_NORMALSWING2, target, schoolmask, damage, absorb, resist, VICTIMSTATE_NORMAL, 0);
        return true;
    }

    // non-melee damage
    uint32 spellid = ExtractSpellIdFromLink(&args);
    if (!spellid || !sSpellStore.LookupEntry(spellid))
    {
        return false;
    }

    player->SpellNonMeleeDamageLog(target, spellid, damage);
    return true;
}

/**
 * @brief Handler for HandleDieCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleDieCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();
    Unit* target = getSelectedUnit();

    if (!target || !player->GetSelectionGuid())
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    if (target->GetTypeId() == TYPEID_PLAYER)
    {
        if (HasLowerSecurity((Player*)target, ObjectGuid(), false))
        {
            return false;
        }
    }

    if (target->IsAlive())
    {
        player->DealDamage(target, target->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
    }

    return true;
}

/**
 * @brief Handler for HandleMovegensCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleMovegensCommand(char* /*args*/)
{
    Unit* unit = getSelectedUnit();
    if (!unit)
    {
        SendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage(LANG_MOVEGENS_LIST, (unit->GetTypeId() == TYPEID_PLAYER ? "Player" : "Creature"), unit->GetGUIDLow());

    MotionMaster* mm = unit->GetMotionMaster();
    float x = 0.0f, y = 0.0f, z = 0.0f;
    const bool hasDestination = mm->GetDestination(x, y, z);
    std::vector<MotionMaster::HeldView> held = mm->Held();
    for (size_t i = 0; i < held.size(); ++i)
    {
        // One line per held entry in the debug dump's style: the layer, the kind, the marks,
        // and what it tracks (the entry's own guid, resolved here) or where the selected one goes.
        std::string line = std::string("  [") + Motion::LayerName(Motion::LayerOf(held[i].kind)) + "] " + Motion::KindName(held[i].kind);
        if (held[i].selected)
        {
            line += " (selected)";
        }
        if (!held[i].reachable)
        {
            line += " (unreachable)";
        }
        char tail[256];
        tail[0] = '\0';
        if (held[i].target)
        {
            Unit* target = ObjectLookup::GetUnit(*unit, ObjectGuid(held[i].target));
            if (target)
            {
                snprintf(tail, sizeof(tail), " -> %s %s (lowguid %u)", target->GetTypeId() == TYPEID_PLAYER ? "player" : "creature", target->GetName(), target->GetGUIDLow());
            }
            else
            {
                snprintf(tail, sizeof(tail), " -> <gone>");
            }
        }
        else if (hasDestination && held[i].selected && (held[i].kind == Motion::Kind::Point || held[i].kind == Motion::Kind::FlyLand || held[i].kind == Motion::Kind::Home))
        {
            snprintf(tail, sizeof(tail), " -> (%.2f %.2f %.2f)", x, y, z);
        }
        line += tail;
        SendSysMessage(line.c_str());
    }
    return true;
}

/**
 * @brief Handler for HandleSetViewCommand command.
 *
 * @param args Command arguments.
 * @returns True if the command executed successfully, false otherwise.
 */
bool ChatHandler::HandleSetViewCommand(char* /*args*/)
{
    if (Unit* unit = getSelectedUnit())
    {
        m_session->GetPlayer()->GetCamera().SetView(unit);
    }
    else
    {
        PSendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}
