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

// The protocol peer's vocabulary: what a scripted session is told to do, and
// what it reports having seen. Pure values; the units that act on them are
// TimeSync, Walker and AckEngine, and the SyntheticClient's Serve loop is
// the only place that touches a socket.

#include "Platform/Define.h"
#include "wire/MovementStatus.h"

#include <map>
#include <string>
#include <vector>

namespace loadtest
{
    /// How the peer answers a server-initiated movement change.
    enum class AckMode
    {
        Immediate,  ///< ack at once, counter echoed
        Delay,      ///< ack after `delayMs`
        Mismatch,   ///< ack at once with counter + 1: a counter the server never issued (the violation case)
        Stale,      ///< ack at once with counter - 1: an ack for a change already retired (consumed by a tombstone)
        WrongValue, ///< ack at once, counter echoed, the value + 1.0: a payload the server did not send (the resend-once case)
        Drop        ///< never ack
    };

    struct AckPolicy
    {
        AckMode mode = AckMode::Immediate;
        uint32  delayMs = 0;
    };

    /// A straight-line walk reported the way the real client reports one.
    struct WalkScript
    {
        uint32 seconds = 0;          ///< 0 = do not walk
        uint32 leadMs = 3000;        ///< wait this long in the world before starting
        bool   headingSet = false;
        float  heading = 0.0f;       ///< radians; the character's facing when not set
        float  speed = 7.0f;         ///< yards per second (4.3.4 base run speed)
        uint32 heartbeatMs = 500;    ///< the real client's heartbeat cadence
        /// Turn round at the end and walk the same time back along the reversed
        /// heading, so a run leaves the character where it found it and the next
        /// run's observer is still in range.
        bool   returnHome = false;
    };

    struct PeerScript
    {
        WalkScript walk;
        AckPolicy  ack;
        uint64     observeGuid = 0;  ///< count SMSG_MOVE_UPDATE relays for this mover
        /// What the run must have seen for it to have proved anything: each name
        /// gets its own verdict line. "teleport", "knockback", "splines".
        std::vector<std::string> expect;
    };

    /// One relayed movement status (SMSG_PLAYER_MOVE, the client's SMSG_MOVE_UPDATE) the peer saw.
    struct Observation
    {
        uint64     guid = 0;
        Wire::Vec4 pos;
        uint32     time = 0;         ///< the mover's movement time as relayed
        uint32     atTicks = 0;      ///< our clock when it arrived
    };

    /// How far two relayed timestamps of one mover disagree with the wall clock
    /// between their arrivals: |(later.time - earlier.time) - (later.atTicks -
    /// earlier.atTicks)|, both differences modulo 2^32.
    inline uint32 RelaySkew(Observation const& earlier, Observation const& later)
    {
        const uint32 relayed = later.time - earlier.time;
        const uint32 arrived = later.atTicks - earlier.atTicks;
        return relayed > arrived ? relayed - arrived : arrived - relayed;
    }

    struct PeerReport
    {
        uint32 timeSyncsAnswered = 0;
        uint32 controlUpdates = 0;

        uint32     walkStarts = 0;
        uint32     walkHeartbeats = 0;
        uint32     walkStops = 0;
        uint32     relocations = 0;    ///< legs cut short by a teleport: one extra start each, no stop
        Wire::Vec4 walkFinal;
        uint32     walkLastTime = 0;   ///< the movement time stamped on the walker's last packet

        uint32      observedTarget = 0;
        uint32      observedOthers = 0;
        Observation lastTargetObservation;
        uint32      relayMaxSkew = 0;      ///< worst RelaySkew seen between consecutive target observations
        uint32      relaySkewSamples = 0;  ///< how many consecutive pairs fed relayMaxSkew

        // The hand-written families (P1-C): what the peer decoded and how it answered.
        uint32     teleports = 0;      ///< SMSG_MOVE_TELEPORT decoded
        uint32     teleportAcks = 0;   ///< CMSG_MOVE_TELEPORT_ACK sent back
        Wire::Vec4 teleportFinal;      ///< where the last teleport put the mover
        uint32     knockBacks = 0;     ///< SMSG_MOVE_KNOCK_BACK decoded
        uint32     knockBackAcks = 0;  ///< CMSG_MOVE_KNOCK_BACK_ACK sent back
        uint32     activeMoverSets = 0; ///< SMSG_MOVE_SET_ACTIVE_MOVER decoded
        uint32     monsterMoves = 0;   ///< SMSG_MONSTER_MOVE(_TRANSPORT) decoded and re-encoded exact

        std::map<uint16, uint32> decodeFailures;      ///< by opcode
        std::map<uint16, uint32> unregisteredChanges; ///< change opcodes with no layout yet
        uint32 acksSent = 0;
        uint32 acksDropped = 0;
        uint32 acksPending = 0;      ///< planned but not sent when the hold ended
        uint32 otherPackets = 0;

        /// Observer forms seen for `observeGuid`, by opcode: the speed updates, the root
        /// and unroot rebroadcasts, the collision-height, knock-back and teleport updates
        /// (SMSG_PLAYER_MOVE is `observedTarget`).
        std::map<uint16, uint32> observerForms;
        /// Server-initiated changes decoded and answered (or dropped), by opcode: a
        /// resend shows as a second change of the same opcode.
        std::map<uint16, uint32> changesSeen;
        bool kicked = false;         ///< the server closed the socket before the hold ended
    };
}
