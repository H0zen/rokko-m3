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

#include "WriterShadowHooks.h"

#include "Unit.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "wire/KnockBackCodec.h"
#include "wire/MovementCodec.h"
#include "wire/MovementSequences.h"
#include "wire/TeleportCodec.h"
#include "PacketMatrix.h"
#include "WriterShadow.h"

#include <atomic>

namespace
{
    std::atomic<bool> s_enabled(false);

    // Immortal, like WireParity's rows: the shutdown report runs after static
    // destruction could have started.
    Motion::ShadowCounters& Counters()
    {
        static Motion::ShadowCounters* counters = new Motion::ShadowCounters();
        return *counters;
    }

    bool On() { return s_enabled.load(std::memory_order_relaxed); }

    Motion::Mode ModeOf(Unit const& unit)
    {
        return unit.GetTypeId() == TYPEID_PLAYER ? Motion::Mode::ClientDriven : Motion::Mode::ServerDriven;
    }

    // The counter the legacy writer put in the packet, so the comparison is
    // about shape. 0 when the packet cannot be decoded (the collision-height
    // writer), which the known-different bin absorbs. Every writer this task
    // hooks ends with a byte-aligned write, so the packet is already flushed
    // (pendingWriteBits = false) when the hook runs.
    uint32 LegacyCounter(WorldPacket const& legacy)
    {
        const uint16 opcode = legacy.GetOpcode();
        if (Wire::IsPacketLayout(opcode))
        {
            Wire::MovementStatus status;
            Wire::DecodeResult result;
            if (Wire::DecodeWhole(legacy, Wire::SequenceFor(opcode), status, result, false))
            {
                return status.counter;
            }
            return 0;
        }
        if (opcode == SMSG_MOVE_KNOCK_BACK)
        {
            WorldPacket copy(legacy);
            Wire::KnockBack k;
            if (Wire::DecodeKnockBack(copy, k).ok()) { return k.counter; }
        }
        else if (opcode == SMSG_MOVE_TELEPORT)
        {
            WorldPacket copy(legacy);
            Wire::Teleport t;
            if (Wire::DecodeTeleport(copy, t).ok()) { return t.counter; }
        }
        return 0;
    }

    void Judge(uint64 guid, Motion::Change const& change, Motion::Mode mode, WorldPacket const& legacy)
    {
        Counters().Count(Motion::JudgeWriter(guid, change, mode, LegacyCounter(legacy), legacy));
    }
}

namespace WriterShadow
{
    void Enable(bool on) { s_enabled.store(on, std::memory_order_relaxed); }
    bool Enabled() { return On(); }
    bool Saw() { return Counters().Saw(); }
    void Report(std::function<void(std::string const&)> const& line) { Counters().Report(line); }

    void Speed(Unit const& unit, uint8 moveType, float flat, WorldPacket const& legacy)
    {
        if (!On()) { return; }
        Judge(unit.GetObjectGuid().GetRawValue(), Motion::SpeedChange(moveType, flat), ModeOf(unit), legacy);
    }

    void Flag(Unit const& unit, Motion::ChangeType type, bool apply, WorldPacket const& legacy)
    {
        if (!On()) { return; }
        Judge(unit.GetObjectGuid().GetRawValue(), Motion::FlagChange(type, apply), ModeOf(unit), legacy);
    }

    void Height(Unit const& unit, float height, uint8 reason, WorldPacket const& legacy)
    {
        if (!On()) { return; }
        Judge(unit.GetObjectGuid().GetRawValue(), Motion::HeightChange(height, reason), ModeOf(unit), legacy);
    }

    void KnockBack(uint64 guid, Motion::KnockBackParams const& params, WorldPacket const& legacy)
    {
        if (!On()) { return; }
        Judge(guid, Motion::KnockBackChange(params), Motion::Mode::ClientDriven, legacy);
    }

    void Teleport(uint64 guid, Motion::TeleportParams const& params, WorldPacket const& legacy)
    {
        if (!On()) { return; }
        Judge(guid, Motion::TeleportChange(params), Motion::Mode::ClientDriven, legacy);
    }
}
