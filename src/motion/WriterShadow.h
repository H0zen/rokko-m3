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

#ifndef MANGOS_MOTION_WRITER_SHADOW_H
#define MANGOS_MOTION_WRITER_SHADOW_H

#include "Change.h"
#include "PacketMatrix.h"
#include "WorldPacket.h"

#include <atomic>
#include <functional>
#include <string>

/**
 * The writer shadow (P2-A): for a packet a legacy writer built, the packet
 * the kernel would build for the same change, and the verdict. Exact is the
 * gate. Two rows are known to differ by construction (the WotLK-shaped
 * collision-height writer; the player levitate builder that sends the
 * opposite gravity opcode) and are counted apart; a player's spline speed,
 * which the kernel does not send, is counted as legacy-only; a packet no
 * matrix row claims is counted as no-writer. The counters are per matrix
 * row and form, and print one line each.
 */
namespace Motion
{
    enum class ShadowVerdict : uint8 { Exact, Mismatch, KnownDifferent, LegacyOnly, NoWriter };

    struct ShadowResult
    {
        ShadowVerdict verdict;
        uint16        legacyOpcode;
        uint16        kernelOpcode;      ///< 0 when the kernel built nothing
        size_t        firstDifference;   ///< index of the first differing byte; size_t(-1) when none, or when the opcodes differ
        size_t        legacySize;
        size_t        kernelSize;
    };

    ShadowResult JudgeWriter(uint64 guid, Change const& change, Mode mode, uint32 legacyCounter, WorldPacket const& legacy);

    class ShadowCounters
    {
    public:
        ShadowCounters();
        void Count(ShadowResult const& result);
        bool Saw() const { return m_seen.load(std::memory_order_relaxed) != 0; }
        uint32 Seen() const { return m_seen.load(std::memory_order_relaxed); }
        uint32 Mismatched() const { return m_mismatched.load(std::memory_order_relaxed); }
        void Report(std::function<void(std::string const&)> const& line) const;
        static size_t Rows() { return kRows; }   ///< the counter table's row count, for a test to pin

    private:
        ShadowCounters(ShadowCounters const&);
        ShadowCounters& operator=(ShadowCounters const&);

        struct Row
        {
            std::atomic<uint32> seen, exact, mismatch, knownDifferent, legacyOnly, noWriter;
            std::atomic<uint32> firstDifference;   ///< +1; 0 = none recorded; 0xFFFFFFFF = the opcodes differed
            std::atomic<uint32> otherOpcode;       ///< the kernel's opcode on a mismatch, or the legacy opcode in the unmatched row
            Row();
        };
        enum { kForms = 2, kRows = 30 * kForms + 1 };   ///< MatrixSize() rows x {mover, spline} + one for packets no row claims;
                                                         ///< a row beyond MatrixSize() at the time this was sized lands in
                                                         ///< that last, unmatched-row slot instead of past the array (Count)

        Row                 m_rows[kRows];
        std::atomic<uint32> m_seen;
        std::atomic<uint32> m_mismatched;
    };
}

#endif
