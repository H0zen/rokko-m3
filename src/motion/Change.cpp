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

#include "Change.h"

namespace Motion
{
    Change SpeedChange(uint8 moveType, float flat)
    {
        Change c;
        c.type = SpeedChangeType(moveType);
        c.value = flat;
        return c;
    }

    Change FlagChange(ChangeType type, bool apply)
    {
        Change c;
        c.type = type;
        c.apply = apply;
        return c;
    }

    Change HeightChange(float height, uint8 reason)
    {
        Change c;
        c.type = ChangeType::CollisionHeight;
        c.value = height;
        c.reason = reason;
        return c;
    }

    Change KnockBackChange(KnockBackParams const& params)
    {
        Change c;
        c.type = ChangeType::KnockBack;
        c.knockBack = params;
        return c;
    }

    Change TeleportChange(TeleportParams const& params)
    {
        Change c;
        c.type = ChangeType::Teleport;
        c.teleport = params;
        return c;
    }

    bool IsSpeed(ChangeType type)
    {
        return type >= ChangeType::WalkSpeed && type <= ChangeType::PitchRate;
    }

    int SpeedIndex(ChangeType type)
    {
        return IsSpeed(type) ? int(uint8(type) - uint8(ChangeType::WalkSpeed)) : -1;
    }

    ChangeType SpeedChangeType(uint8 moveType)
    {
        return moveType <= 8 ? ChangeType(uint8(ChangeType::WalkSpeed) + moveType) : ChangeType::None;
    }

    char const* ChangeName(ChangeType type)
    {
        switch (type)
        {
            case ChangeType::None:                 return "None";
            case ChangeType::WalkSpeed:            return "WalkSpeed";
            case ChangeType::RunSpeed:             return "RunSpeed";
            case ChangeType::RunBackSpeed:         return "RunBackSpeed";
            case ChangeType::SwimSpeed:            return "SwimSpeed";
            case ChangeType::SwimBackSpeed:        return "SwimBackSpeed";
            case ChangeType::TurnRate:             return "TurnRate";
            case ChangeType::FlightSpeed:          return "FlightSpeed";
            case ChangeType::FlightBackSpeed:      return "FlightBackSpeed";
            case ChangeType::PitchRate:            return "PitchRate";
            case ChangeType::Root:                 return "Root";
            case ChangeType::CanFly:               return "CanFly";
            case ChangeType::WaterWalk:            return "WaterWalk";
            case ChangeType::FeatherFall:          return "FeatherFall";
            case ChangeType::Hover:                return "Hover";
            case ChangeType::GravityDisabled:      return "GravityDisabled";
            case ChangeType::CanTransitionSwimFly: return "CanTransitionSwimFly";
            case ChangeType::CollisionHeight:      return "CollisionHeight";
            case ChangeType::KnockBack:            return "KnockBack";
            case ChangeType::Teleport:             return "Teleport";
            case ChangeType::Gait:                 return "Gait";
            case ChangeType::Swim:                 return "Swim";
            default:                               return "?";
        }
    }
}
