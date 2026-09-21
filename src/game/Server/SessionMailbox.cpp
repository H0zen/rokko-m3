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

#include <memory>
#include <mutex>
#include "SessionMailbox.h"

SessionMailbox::~SessionMailbox()
{
    Close();
}

bool SessionMailbox::Enqueue(std::unique_ptr<WorldPacket> packet)
{
    if (!packet)
        return false;

    std::lock_guard<std::mutex> guard(m_stateLock);
    if (m_closed)
        return false;

    if (m_queued >= kMaxQueuedPackets)
    {
        // Latch and refuse. The packet dies with the unique_ptr on return; the session
        // notices the latch on its own thread and disconnects the client, because a
        // client this far ahead of the server is either broken or hostile and in both
        // cases the conversation is already lost.
        m_overflowed = true;
        return false;
    }

    WorldPacket* accepted = packet.get();
    m_packets.add(accepted);
    (void)packet.release();
    ++m_queued;
    return true;
}

bool SessionMailbox::Next(WorldPacket*& packet)
{
    std::lock_guard<std::mutex> guard(m_stateLock);
    if (m_closed)
        return false;
    if (!m_packets.next(packet))
    {
        return false;
    }
    --m_queued;
    return true;
}

void SessionMailbox::Close()
{
    {
        std::lock_guard<std::mutex> guard(m_stateLock);
        if (m_closed)
            return;
        m_closed = true;
    }

    WorldPacket* packet = nullptr;
    while (m_packets.next(packet))
        delete packet;

    std::lock_guard<std::mutex> guard(m_stateLock);
    m_queued = 0;
}

bool SessionMailbox::IsClosed() const
{
    std::lock_guard<std::mutex> guard(m_stateLock);
    return m_closed;
}

bool SessionMailbox::Overflowed() const
{
    std::lock_guard<std::mutex> guard(m_stateLock);
    return m_overflowed;
}
