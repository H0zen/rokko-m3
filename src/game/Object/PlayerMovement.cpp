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
#include "SkillDiscovery.h"
#include "QuestDef.h"
#include "GossipDef.h"
#include "UpdateData.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "MapManager.h"
#include "MapPersistentStateMgr.h"
#include "InstanceData.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "ObjectMgr.h"
#include "CreatureAI.h"
#include "Formulas.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Pet.h"
#include "Util.h"
#include "Transports.h"
#include "Weather.h"
#include "BattleGround/BattleGround.h"
#include "BattleGround/BattleGroundMgr.h"
#include "BattleGround/BattleGroundAV.h"
#include "OutdoorPvP/OutdoorPvP.h"
#include "ArenaTeam.h"
#include "Chat.h"
#include "Spell.h"
#include "ScriptMgr.h"
#include "SocialMgr.h"
#include "AchievementMgr.h"
#include "Mail.h"
#include "SpellAuras.h"
#include "DBCStores.h"
#include "DB2Stores.h"
#include "SQLStorages.h"
#include "Vehicle.h"
#include "Calendar.h"
#include "DisableMgr.h"
#include "GameTime.h"

#include <cmath>

/**
 * @brief Forces or clears rooted movement for the player.
 *
 * @param enable True to root the player; false to unroot them.
 */
void Player::SetRoot(bool enable)
{
    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::Root, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables water walking for the player.
 *
 * @param enable True to enable water walking; false to restore normal movement.
 */
void Player::SetWaterWalk(bool enable)
{
    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::WaterWalk, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Levitates (gravity off) or lands the player through the kernel: the
 * gravity-disable/enable mover form and, on the ack, the observers' update.
 *
 * @param enable true to levitate.
 */
void Player::SetLevitate(bool enable)
{
    // Gravity off is what "levitate" means on the wire (design v2 §7's matrix); the
    // legacy builder sent the opposite gravity opcode.
    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::GravityDisabled, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables flying movement flags for the player.
 *
 * @param enable True to enable flight-related movement flags; false to clear them.
 */
void Player::SetCanFly(bool enable)
{
    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::CanFly, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables feather fall movement for the player.
 *
 * @param enable True to enable feather fall; false to restore normal falling.
 */
void Player::SetFeatherFall(bool enable)
{
    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::FeatherFall, enable), GameTime::GetGameTimeMS()));
    if (!enable)
    {
        SetFallInformation(0, Where().Z());
    }
}

/**
 * @brief Enables or disables hover movement for the player.
 *
 * @param enable True to enable hovering; false to disable it.
 */
void Player::SetHover(bool enable)
{
    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::Hover, enable), GameTime::GetGameTimeMS()));
}
