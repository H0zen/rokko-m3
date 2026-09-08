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

#include <vector>

namespace loadtest
{
    /**
     * @brief Moves a character in a straight line the way the real client
     *        reports it: CMSG_MOVE_START_FORWARD once, MSG_MOVE_HEARTBEAT every
     *        half second, CMSG_MOVE_STOP when the time is up.
     *
     * Pure: fed the clock, it returns the packets due; the caller sends them.
     * The first Advance() call fixes the moment the lead starts counting.
     * With `returnHome` set, the stop of the outbound leg is followed at once
     * by a second leg along the reversed heading for the same time, ending
     * exactly where the walk began: each leg is clamped at its end, so a late
     * poll never lengthens it, and the return leg snaps to the origin.
     */
    class Walker
    {
        public:
            Walker(const WalkScript& script, uint64 guid, const Wire::Vec4& start);

            /// Packets due by `nowTicks`, in order; empty when nothing is due.
            std::vector<WorldPacket> Advance(uint32 nowTicks);

            bool Started() const { return m_started; }
            bool Done() const { return m_done; }
            const Wire::Vec4& Position() const { return m_pos; }

            /**
             * @brief The mover is now here -- a teleport the server ordered and the
             *        peer acked; the next packet starts from this position.
             *
             * Position and home both move: `--return` then walks back to where the
             * teleport left the character rather than to a point it can no longer
             * reach. Any leg in progress ENDS here -- the state is reset as if none
             * were running, keeping the heading and whatever legs are still owed --
             * because a leg that outlived a teleport would owe its stop, and a
             * return leg its snap to the origin, from a place the mover has left;
             * that snap is exactly how a small rounding correction turns into a
             * jump across the map. No CMSG_MOVE_STOP is sent for the leg that ends
             * this way: the server has just carried the mover elsewhere, and a stop
             * reported from the old place would be a lie about where it is. The
             * next Advance therefore opens a fresh leg from `pos` with no lead,
             * which costs one more CMSG_MOVE_START_FORWARD (and one more `Starts()`)
             * than an undisturbed walk -- the same thing a real client does after
             * acking a teleport mid-run. `Relocations()` counts those, so a caller
             * judging the walk can tell that extra start from a lost one.
             */
            void Relocate(const Wire::Vec4& pos);

            /// The mover as it stands now: guid, forward flag while a leg is under
            /// way, position with the heading as orientation; no timestamp -- the
            /// caller stamps.
            Wire::MovementStatus Status() const;

            uint32 Starts() const { return m_starts; }
            uint32 Heartbeats() const { return m_heartbeats; }
            uint32 Stops() const { return m_stops; }
            /// How many times a leg that was actually running was cut short by a
            /// Relocate. Each one costs an extra Starts() and no Stops(), so a
            /// verdict that expects one start per leg needs this to balance its
            /// arithmetic. A Relocate arriving between legs changes no counts and
            /// is not one of these.
            uint32 Relocations() const { return m_relocations; }
            uint32 LastStampedTime() const { return m_lastStampedTime; }

        private:
            WorldPacket Packet(uint16 opcode, uint32 flags, uint32 nowTicks) const;

            WalkScript m_script;
            uint64     m_guid;
            Wire::Vec4 m_pos;
            Wire::Vec4 m_origin;         ///< where the walk began; the return leg ends here
            float      m_heading;

            bool   m_armed = false;      ///< first Advance seen; the lead is counting
            bool   m_started = false;
            bool   m_done = false;
            uint32 m_armedTicks = 0;
            uint32 m_startTicks = 0;
            uint32 m_lastTicks = 0;
            uint32 m_nextHeartbeat = 0;
            uint32 m_starts = 0;
            uint32 m_heartbeats = 0;
            uint32 m_stops = 0;
            uint32 m_relocations = 0;    ///< Relocates that ended a leg in progress
            bool   m_returning = false;      ///< on the way back (returnHome)
            uint32 m_lastStampedTime = 0;
    };
}
