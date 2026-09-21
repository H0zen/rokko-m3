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

#ifndef MANGOS_H_SESSIONMAILBOX
#define MANGOS_H_SESSIONMAILBOX

#include "LockedQueue/LockedQueue.h"
#include "WorldPacket.h"

#include <memory>
#include <mutex>

/**
 * One session's inbound queue, filled by a network thread and drained by whichever
 * game thread owns the session this phase.
 *
 * It is bounded. It used to refuse a packet only once the mailbox was closed, which
 * meant a client that sent as fast as it could grew the heap without limit, held a map
 * worker inside a single session's drain loop for as long as it liked, and through
 * MapUpdater::wait() held the world thread -- and therefore every other player on every
 * other map -- behind it. The outbound direction had thought this through (FlowControl's
 * FlowGate, backpressure in bytes, producer parked); this direction had nothing.
 *
 * The cap is on packets rather than bytes because proto already bounds a single packet
 * at MAX_CLIENT_PACKET_SIZE (10240), so a count also bounds the memory: at most ~10 MB
 * per session before the cap trips, and it trips long before a real client would reach
 * it. A legitimate 4.3.4 client queues a handful between drains at 20 Hz.
 *
 * Overflow is not a dropped packet. Dropping one silently desynchronises a protocol
 * that assumes what it sent arrived, so the mailbox latches the condition instead and
 * lets the session end itself on the world thread, where ending sessions is legal.
 */
class SessionMailbox
{
    public:
        /// Most packets one session may have waiting. See the class comment for why
        /// a count is a sufficient bound on memory.
        static const size_t kMaxQueuedPackets = 1024;

        SessionMailbox() = default;
        ~SessionMailbox();

        bool Enqueue(std::unique_ptr<WorldPacket> packet);
        bool Next(WorldPacket*& packet);

        template<class Checker>
        bool Next(WorldPacket*& packet, Checker& checker)
        {
            std::lock_guard<std::mutex> guard(m_stateLock);
            if (m_closed)
                return false;
            if (!m_packets.next(packet, checker))
            {
                return false;
            }
            --m_queued;
            return true;
        }

        void Close();
        bool IsClosed() const;

        /// True once a client has exceeded kMaxQueuedPackets. Latched: the session
        /// reads it on its own thread and disconnects.
        bool Overflowed() const;

    private:
        mutable std::mutex m_stateLock;
        bool m_closed = false;
        bool m_overflowed = false;
        size_t m_queued = 0;
        MaNGOS::LockedQueue<WorldPacket*> m_packets;
};

#endif
