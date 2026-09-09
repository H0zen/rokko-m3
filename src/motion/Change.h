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

#ifndef MANGOS_MOTION_CHANGE_H
#define MANGOS_MOTION_CHANGE_H

#include "Platform/Define.h"
#include "wire/MovementStatus.h"

/**
 * What the kernel can change on a unit's kinematic state (design v2 §3.2
 * Apply(Change)). Each ChangeType names one row family of the packet matrix
 * (PacketMatrix.h); a Change carries the row's payload. Speeds are flat
 * yards per second as the wire carries them, never rates. Gait and Swim are
 * server-driven only (§6.4): the protocol has no client negotiation for them.
 */
namespace Motion
{
    enum class ChangeType : uint8
    {
        None = 0,
        // The nine speeds, in UnitMoveType order (src/game/Object/Unit.h:580).
        WalkSpeed, RunSpeed, RunBackSpeed, SwimSpeed, SwimBackSpeed, TurnRate, FlightSpeed, FlightBackSpeed, PitchRate,
        // Flags: apply = set, !apply = unset.
        Root, CanFly, WaterWalk, FeatherFall, Hover, GravityDisabled, CanTransitionSwimFly,
        // Values and events.
        CollisionHeight, KnockBack, Teleport,
        // ServerDriven only.
        Gait,   // apply = walk
        Swim,   // apply = swimming
        Count
    };

    struct KnockBackParams
    {
        float directionX;   ///< cos of the knock-back angle, as SMSG_MOVE_KNOCK_BACK carries it
        float directionY;   ///< sin
        float horizontal;
        float vertical;     ///< as written on the wire (the legacy caller negates it)
        KnockBackParams() : directionX(0.0f), directionY(0.0f), horizontal(0.0f), vertical(0.0f) {}
    };

    struct TeleportParams
    {
        Wire::Vec4 pos;
        bool   hasTransport;
        uint64 transportGuid;
        TeleportParams() : hasTransport(false), transportGuid(0) {}
    };

    struct Change
    {
        ChangeType      type;
        bool            apply;      ///< flags, Gait, Swim
        float           value;      ///< speeds: flat yd/s; CollisionHeight: the height
        uint8           reason;     ///< CollisionHeight only: the layout's two bits (0 scale, 1 mount, 2 force, CPP's UpdateCollisionHeightReason)
        KnockBackParams knockBack;
        TeleportParams  teleport;
        Change() : type(ChangeType::None), apply(true), value(0.0f), reason(0) {}
    };

    Change SpeedChange(uint8 moveType, float flat);
    Change FlagChange(ChangeType type, bool apply);
    Change HeightChange(float height, uint8 reason);
    Change KnockBackChange(KnockBackParams const& params);
    Change TeleportChange(TeleportParams const& params);

    bool        IsSpeed(ChangeType type);
    int         SpeedIndex(ChangeType type);          ///< 0..8 for a speed, -1 otherwise
    ChangeType  SpeedChangeType(uint8 moveType);      ///< None when moveType > 8
    char const* ChangeName(ChangeType type);
}

#endif
