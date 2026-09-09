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

#include "AckEngine.hpp"

#include "Opcodes.h"
#include "wire/MovementCodec.h"

namespace loadtest
{
    const std::vector<ChangePair>& KnownChangePairs()
    {
        static const std::vector<ChangePair> pairs =
        {
            { SMSG_MOVE_SET_WALK_SPEED,        CMSG_FORCE_WALK_SPEED_CHANGE_ACK        },
            { SMSG_MOVE_SET_RUN_SPEED,         CMSG_FORCE_RUN_SPEED_CHANGE_ACK         },
            { SMSG_MOVE_SET_RUN_BACK_SPEED,    CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK    },
            { SMSG_MOVE_SET_SWIM_SPEED,        CMSG_FORCE_SWIM_SPEED_CHANGE_ACK        },
            { SMSG_MOVE_SET_SWIM_BACK_SPEED,   CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK   },
            { SMSG_MOVE_SET_TURN_RATE,         CMSG_FORCE_TURN_RATE_CHANGE_ACK         },
            { SMSG_MOVE_SET_FLIGHT_SPEED,      CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK      },
            { SMSG_MOVE_SET_FLIGHT_BACK_SPEED, CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK },
            { SMSG_MOVE_SET_PITCH_RATE,        CMSG_FORCE_PITCH_RATE_CHANGE_ACK        },
            { SMSG_FORCE_MOVE_ROOT,            CMSG_FORCE_MOVE_ROOT_ACK                },
            { SMSG_FORCE_MOVE_UNROOT,          CMSG_FORCE_MOVE_UNROOT_ACK              },
            { SMSG_MOVE_WATER_WALK,            CMSG_MOVE_WATER_WALK_ACK                },
            { SMSG_MOVE_LAND_WALK,             CMSG_MOVE_WATER_WALK_ACK                },
            { SMSG_MOVE_FEATHER_FALL,          CMSG_MOVE_FEATHER_FALL_ACK              },
            { SMSG_MOVE_NORMAL_FALL,           CMSG_MOVE_FEATHER_FALL_ACK              },
            { SMSG_MOVE_SET_HOVER,             CMSG_MOVE_HOVER_ACK                     },
            { SMSG_MOVE_UNSET_HOVER,           CMSG_MOVE_HOVER_ACK                     },
            { SMSG_MOVE_SET_CAN_FLY,           CMSG_MOVE_SET_CAN_FLY_ACK               },
            { SMSG_MOVE_UNSET_CAN_FLY,         CMSG_MOVE_SET_CAN_FLY_ACK               },
            { SMSG_MOVE_GRAVITY_DISABLE,       CMSG_MOVE_GRAVITY_DISABLE_ACK           },
            { SMSG_MOVE_GRAVITY_ENABLE,        CMSG_MOVE_GRAVITY_ENABLE_ACK            },
            { SMSG_MOVE_SET_COLLISION_HGT,     CMSG_MOVE_SET_COLLISION_HGT_ACK         },
            { SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY,
              CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK                     },
            // SMSG_MOVE_KNOCK_BACK and SMSG_MOVE_TELEPORT are not here: neither
            // carries a movement status, so this engine could never decode one.
            // The peer answers both from their own codecs (P1-C's families), which
            // is why a change with no layout is a defect again rather than a gap.
        };
        return pairs;
    }

    AckEngine::AckEngine(const AckPolicy& policy, Lookup lookup, std::vector<ChangePair> pairs)
        : m_policy(policy), m_lookup(std::move(lookup)), m_pairs(std::move(pairs))
    {
    }

    const ChangePair* AckEngine::Find(uint16 change) const
    {
        for (const ChangePair& pair : m_pairs)
        {
            if (pair.change == change)
            {
                return &pair;
            }
        }
        return nullptr;
    }

    bool AckEngine::IsChange(uint16 opcode) const
    {
        return Find(opcode) != nullptr;
    }

    bool AckEngine::Plan(const WorldPacket& change, uint32 nowTicks)
    {
        const ChangePair* pair = Find(change.GetOpcode());
        if (!pair)
        {
            return false;
        }

        const Wire::Sequence in = m_lookup(pair->change);
        const Wire::Sequence out = m_lookup(pair->ack);
        if (!in || !out)
        {
            ++m_unregistered[pair->change];
            return false;
        }

        // Decode from a copy: the caller's packet keeps its read cursor. The
        // whole payload must be the change: trailing bytes are what the
        // shadow and the replay call LeftBytes, and the peer agrees with them.
        WorldPacket copy(change);
        copy.rpos(0);
        copy.ResetBitReader();
        Wire::MovementStatus status;
        Wire::DecodeResult const decoded = Wire::Decode(copy, in, status);
        if (!decoded.ok() || decoded.consumed != copy.size())
        {
            ++m_decodeFailures[pair->change];
            return false;
        }

        uint32 counter = status.counter;

        switch (m_policy.mode)
        {
            case AckMode::Drop:
                ++m_dropped;
                return true;
            case AckMode::Mismatch:
                counter += 1;
                break;
            case AckMode::Stale:
                counter -= 1;
                break;
            case AckMode::Delay:
            case AckMode::Immediate:
                break;
        }

        Pending pending;
        pending.opcode  = pair->ack;
        pending.guid    = status.guid;
        pending.counter = counter;
        pending.value   = status.value;
        pending.twoBits = status.twoBits;
        pending.dueTicks = nowTicks + (m_policy.mode == AckMode::Delay ? m_policy.delayMs : 0);
        m_pending.push_back(pending);
        return true;
    }

    std::vector<WorldPacket> AckEngine::Due(uint32 nowTicks)
    {
        std::vector<WorldPacket> out;
        std::vector<Pending> keep;
        for (const Pending& p : m_pending)
        {
            if (nowTicks < p.dueTicks)
            {
                keep.push_back(p);
                continue;
            }
            // The ack is the mover's own status, as last given by SetMover, with
            // the change's counter and value echoed; the SET names the unit, and
            // the ack names the same one. A real client stamps its ack with its
            // own clock, not the time the server put in the change it is
            // answering, so the time comes from here too, not from Plan.
            Wire::MovementStatus reply = m_mover;
            reply.guid    = p.guid;
            reply.counter = p.counter;
            reply.value   = p.value;
            reply.twoBits = p.twoBits;
            if (reply.has.timestamp)
            {
                reply.time = nowTicks;
            }
            WorldPacket packet(p.opcode, 64);
            Wire::Encode(packet, m_lookup(p.opcode), reply);
            out.push_back(packet);
            ++m_sent;
        }
        m_pending.swap(keep);
        return out;
    }
}
