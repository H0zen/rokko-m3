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

#include "Walker.hpp"

#include "Opcodes.h"
#include "wire/MovementCodec.h"
#include "wire/MovementSequences.h"

#include <cmath>

namespace loadtest
{
    namespace
    {
        /// MovementFlags::MOVEFLAG_FORWARD (src/game/Object/Unit.h). The tool
        /// cannot include game headers, so the one bit it needs lives here.
        const uint32 MOVEFLAG_FORWARD = 0x00000001;
        const float kPi = 3.14159265358979f;
    }

    Walker::Walker(const WalkScript& script, uint64 guid, const Wire::Vec4& start)
        : m_script(script), m_guid(guid), m_pos(start), m_origin(start),
          m_heading(script.headingSet ? script.heading : start.o)
    {
    }

    Wire::MovementStatus Walker::Status() const
    {
        Wire::MovementStatus status;
        status.guid = m_guid;
        status.flags = (m_started && !m_done) ? MOVEFLAG_FORWARD : 0;
        status.pos = m_pos;
        status.pos.o = m_heading;
        return status;
    }

    void Walker::Relocate(const Wire::Vec4& pos)
    {
        // The mover was carried somewhere else, so whatever leg was running is
        // over: leaving it running would leave a stop, and a return leg's
        // snap-to-origin, owed from a place the character is no longer at. The
        // state is reset as if no leg had started -- heading kept, `--return`'s
        // second leg still owed if it was -- and no CMSG_MOVE_STOP is sent for
        // the leg that ends here: the server has just teleported this mover, and
        // a stop reported from the old place would be a lie about where it is.
        // The next Advance starts a fresh leg from `pos`, with no lead.
        //
        // Count it only when a leg was actually running: that is the case that
        // costs an extra start and skips a stop, and so the case a walk verdict
        // has to know about. A teleport that lands between legs -- during the
        // lead, before the return leg opens, or after the walk is done -- opens
        // no fresh leg, so counting it would be the arithmetic error the count
        // exists to prevent. `m_started` alone does not say a leg is running: it
        // is still set when the last leg stopped and `m_done` went up, which is
        // exactly when a teleport is most likely to arrive (the peer holds after
        // the walk), so both are asked. Status() reads the pair the same way.
        if (m_started && !m_done) { ++m_relocations; }
        m_pos = pos;
        m_origin = pos;
        m_started = false;
        m_armed = false;
        m_script.leadMs = 0;
    }

    WorldPacket Walker::Packet(uint16 opcode, uint32 flags, uint32 nowTicks) const
    {
        Wire::MovementStatus status = Status();
        status.flags = flags;
        status.time = nowTicks;

        WorldPacket packet(opcode, 64);
        Wire::Encode(packet, Wire::SequenceFor(opcode), status);
        return packet;
    }

    std::vector<WorldPacket> Walker::Advance(uint32 nowTicks)
    {
        std::vector<WorldPacket> out;
        if (m_done || m_script.seconds == 0)
        {
            return out;
        }

        if (!m_armed)
        {
            m_armed = true;
            m_armedTicks = nowTicks;
        }

        if (!m_started)
        {
            if (nowTicks - m_armedTicks < m_script.leadMs)
            {
                return out;
            }
            m_started = true;
            m_startTicks = nowTicks;
            m_lastTicks = nowTicks;
            m_nextHeartbeat = nowTicks + m_script.heartbeatMs;
            ++m_starts;
            out.push_back(Packet(CMSG_MOVE_START_FORWARD, MOVEFLAG_FORWARD, nowTicks));
            m_lastStampedTime = nowTicks;
            return out;
        }

        // Advance the reported position by what the clock says has elapsed, but
        // never past the leg's end: the caller polls, so the end is seen late,
        // and the walk stops where the leg's time ran out rather than where the
        // late poll would put it. Both legs then cover speed x seconds exactly.
        const uint32 legMs = m_script.seconds * 1000;
        const bool legOver = nowTicks - m_startTicks >= legMs;
        const uint32 upto = legOver ? m_startTicks + legMs : nowTicks;
        const uint32 dt = upto - m_lastTicks;
        m_lastTicks = upto;
        const float yards = m_script.speed * float(dt) / 1000.0f;
        m_pos.x += std::cos(m_heading) * yards;
        m_pos.y += std::sin(m_heading) * yards;

        if (legOver)
        {
            if (m_returning)
            {
                // Rounding along two legs of trigonometry is not "home".
                m_pos.x = m_origin.x;
                m_pos.y = m_origin.y;
            }
            ++m_stops;
            out.push_back(Packet(CMSG_MOVE_STOP, 0, nowTicks));
            m_lastStampedTime = nowTicks;
            if (m_script.returnHome && !m_returning)
            {
                // Turn round and walk the same time back: the next Advance starts
                // the return leg at once, with no lead.
                m_returning = true;
                m_heading += kPi;
                m_started = false;
                m_armed = false;
                m_script.leadMs = 0;
                return out;
            }
            m_done = true;
            return out;
        }

        if (nowTicks >= m_nextHeartbeat)
        {
            ++m_heartbeats;
            m_nextHeartbeat += m_script.heartbeatMs;
            out.push_back(Packet(MSG_MOVE_HEARTBEAT, MOVEFLAG_FORWARD, nowTicks));
            m_lastStampedTime = nowTicks;
        }
        return out;
    }
}
