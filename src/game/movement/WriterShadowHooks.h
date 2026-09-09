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

#ifndef MANGOS_WRITER_SHADOW_HOOKS_H
#define MANGOS_WRITER_SHADOW_HOOKS_H

#include "Platform/Define.h"
#include "Change.h"

#include <functional>
#include <string>

class Unit;
class WorldPacket;

/**
 * The writer shadow's hooks (movement P2-A): each legacy movement writer
 * calls one of these with the change it is about to send and the packet it
 * built, before sending it. The kernel builds its own packet for the same
 * change and the bytes are compared and counted (Motion::JudgeWriter). The
 * legacy packet is never modified and always sent. Off unless
 * Movement.WriterShadow is set; then every hook is one relaxed atomic load.
 * Counts: `.server movement`, and the shutdown log.
 */
namespace WriterShadow
{
    void Enable(bool on);
    bool Enabled();
    bool Saw();
    void Report(std::function<void(std::string const&)> const& line);

    void Speed(Unit const& unit, uint8 moveType, float flat, WorldPacket const& legacy);
    void Flag(Unit const& unit, Motion::ChangeType type, bool apply, WorldPacket const& legacy);
    void Height(Unit const& unit, float height, uint8 reason, WorldPacket const& legacy);
    void KnockBack(uint64 guid, Motion::KnockBackParams const& params, WorldPacket const& legacy);
    void Teleport(uint64 guid, Motion::TeleportParams const& params, WorldPacket const& legacy);
}

#endif
