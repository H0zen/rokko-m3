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

#pragma once

#include "Peer.hpp"
#include "Platform/Define.h"
#include "WorldPacket.h"
#include "wire/MovementElements.h"
#include "wire/MovementStatus.h"

#include <functional>
#include <map>
#include <vector>

namespace loadtest
{
    /// A server-initiated movement change and the ack the client owes for it.
    struct ChangePair
    {
        uint16 change;
        uint16 ack;
    };

    /// The 4.3.4 pairs, by name from Opcodes.h. Data, not policy.
    const std::vector<ChangePair>& KnownChangePairs();

    /// True for the nine acknowledgeable rate changes (the seven SMSG_MOVE_SET_*_SPEED
    /// opcodes, SMSG_MOVE_SET_TURN_RATE and SMSG_MOVE_SET_PITCH_RATE): login and
    /// worldport re-send exactly these seven speeds, so the resend/resync verdicts
    /// count them.
    bool IsSpeedChange(uint16 opcode);

    /**
     * @brief Answers server-initiated movement changes under a scripted policy.
     *
     * The change is decoded for its guid, counter and value; the ack is the
     * mover's status as last given by SetMover, with those three (and the
     * two-bit field) written into it and the time stamped when it is sent --
     * the real client echoes its own status the same way. A change or ack with
     * no registered layout is counted as unregistered; a change whose layout is
     * registered but does not decode is counted apart, as a decode failure --
     * the first list is what P1 has to register, the second what it has to fix.
     * An ack is stamped with the peer's own clock, as a real client's would be.
     */
    class AckEngine
    {
        public:
            typedef std::function<Wire::Sequence(uint16)> Lookup;

            AckEngine(const AckPolicy& policy, Lookup lookup,
                      std::vector<ChangePair> pairs = KnownChangePairs());

            bool IsChange(uint16 opcode) const;

            /// The mover's current status: what an ack carries besides the change's
            /// counter and value. Set it before every Due; the default is an empty
            /// status, which a server accepts but a reader of the capture would not.
            void SetMover(const Wire::MovementStatus& mover) { m_mover = mover; }

            /// Decode `change` and queue its ack per the policy. Returns false, and
            /// counts the opcode, when no layout is registered for it or its ack.
            bool Plan(const WorldPacket& change, uint32 nowTicks);

            /// Acks whose time has come, encoded, in the order they were planned.
            std::vector<WorldPacket> Due(uint32 nowTicks);

            uint32 Sent() const { return m_sent; }
            uint32 Dropped() const { return m_dropped; }
            /// Planned and not yet sent. Non-zero when a hold ends means a change
            /// went unanswered, which the run must say rather than look clean.
            uint32 PendingCount() const { return uint32(m_pending.size()); }
            const std::map<uint16, uint32>& Unregistered() const { return m_unregistered; }
            const std::map<uint16, uint32>& DecodeFailures() const { return m_decodeFailures; }
            /// Decoded changes, by opcode, whether answered, altered or dropped: a
            /// resend shows as a second entry for the same opcode.
            std::map<uint16, uint32> const& ChangesSeen() const { return m_changesSeen; }

        private:
            struct Pending
            {
                uint16 opcode;
                uint64 guid;
                uint32 counter;
                float  value;
                uint8  twoBits;
                uint32 dueTicks;
            };

            const ChangePair* Find(uint16 change) const;

            AckPolicy               m_policy;
            Lookup                  m_lookup;
            std::vector<ChangePair> m_pairs;
            Wire::MovementStatus    m_mover;
            std::vector<Pending>    m_pending;
            std::map<uint16, uint32> m_unregistered;
            std::map<uint16, uint32> m_decodeFailures;
            std::map<uint16, uint32> m_changesSeen;
            uint32                  m_sent = 0;
            uint32                  m_dropped = 0;
    };
}
