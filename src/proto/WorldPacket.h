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

#ifndef MANGOSSERVER_WORLDPACKET_H
#define MANGOSSERVER_WORLDPACKET_H

#include "Platform/Define.h"
#include "ByteBuffer.h"
#include "LinkSlot.h"
#include "Opcodes.h"

// Note: m_opcode and size stored in platfom dependent format
// ignore endianess until send, and converted at receive
/**
 * @brief
 *
 */
class WorldPacket : public ByteBuffer
{
    public:
        /**
         * @brief just container for later use
         *
         */
        WorldPacket() : ByteBuffer(0), m_opcode(MSG_NULL_ACTION)
        {
        }
        /**
         * @brief
         *
         * @param opcode
         * @param res
         */
        explicit WorldPacket(uint16 opcode, size_t res = 200) : ByteBuffer(res), m_opcode(opcode) { }
        /**
         * @brief copy constructor
         *
         * @param packet
         */
        WorldPacket(const WorldPacket& packet)
            : ByteBuffer(packet), m_opcode(packet.m_opcode),
              m_stream(packet.m_stream), m_hasStream(packet.m_hasStream)
        {
            // The stream has to survive a copy. Every packet the client sends
            // is copied once on its way to the world thread -- WorldGateway
            // enqueues `new WorldPacket(std::move(packet))`, and since this
            // class declares a copy constructor there is no move to take it
            // instead. Leaving these two out of the list would drop the field
            // exactly where it is needed and nowhere the compiler would say so.
        }

        /// See the note above: the user-provided copy constructor makes the implicit copy
        /// assignment deprecated, so it is spelled out. Member-wise, identical to implicit.
        WorldPacket& operator=(const WorldPacket&) = default;

        /**
         * @brief
         *
         * @param opcode
         * @param newres
         */
        void Initialize(uint16 opcode, size_t newres = 200)
        {
            clear();
            _storage.reserve(newres);
            m_opcode = opcode;
            m_hasStream = false;        // a repurposed buffer keeps no provenance
        }

        /**
         * @brief
         *
         * @return uint16
         */
        uint16 GetOpcode() const { return m_opcode; }
        /**
         * @brief
         *
         * @param opcode
         */
        void SetOpcode(uint16 opcode) { m_opcode = opcode; }

        /**
         * @brief The world stream this packet belongs to, when it is not the
         *        one its opcode implies.
         *
         * Unset on nearly everything, and unset is what the routing policy
         * wants: which stream a packet leaves on is a property of its opcode
         * (proto::ServerSlotOf). The exception is a reply the client is timing
         * on one particular wire. It pings both streams, keeps a separate
         * sequence counter for each, and matches a pong only against the
         * counter of the connection it arrived on -- so a pong for stream 1
         * answered on stream 0 is never matched, and that stream's latency
         * readout stays at zero for ever.
         *
         * Inbound this records where a packet came from; outbound it says
         * where it must go. That is one fact, so it is one field.
         */
        bool HasStream() const { return m_hasStream; }
        proto::LinkSlot GetStream() const { return m_stream; }
        void SetStream(proto::LinkSlot stream) { m_stream = stream; m_hasStream = true; }
        void ClearStream() { m_hasStream = false; }
        // Deliberately no GetOpcodeName() here. The opcode-name table belongs to
        // the protocol/game layer, and having this convenience accessor in a
        // shared header made shared depend on game just to format a log line.
        // Callers use LookupOpcodeName(pkt.GetOpcode()) instead.

    protected:
        uint16 m_opcode; /**< TODO */
        proto::LinkSlot m_stream = proto::LinkSlot::Zero;
        bool m_hasStream = false;
};
#endif
