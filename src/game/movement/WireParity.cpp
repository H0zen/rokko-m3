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

#include "WireParity.h"

#include "MovementBridge.h"
#include "Unit.h"
#include "OpcodeTable.h"
#include "WorldPacket.h"
#include "wire/MovementCodec.h"
#include "wire/MovementFamilies.h"
#include "wire/MovementParity.h"
#include "wire/MovementSequences.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace WireParity
{
    namespace
    {
        std::atomic<bool> g_enabled{ false };

        // One row per registry entry followed by one per family opcode: the
        // registry rows are indexed by Wire::RegistryIndex and the family rows by
        // RegistrySize() + Wire::FamilyIndex, which is what RowIndex below says.
        // Counters are atomics because map threads and the network thread both
        // arrive here; the first-mismatch text is written once, under the lock.
        struct Row
        {
            std::atomic<uint32> inSeen{ 0 };
            std::atomic<uint32> inRejected{ 0 };
            std::atomic<uint32> inBridgeMismatch{ 0 };
            std::atomic<uint32> outSeen{ 0 };
            std::atomic<uint32> outFailed{ 0 };
            std::atomic<uint32> outInexact{ 0 };
            std::atomic<bool>   hasFirst{ false };
            std::string         first;
        };

        // Rejected's fallback when RowIndex finds neither a registry layout nor a
        // family for the opcode: nothing in the tree exercises this today (every
        // opcode Rejected is called for -- MovementInfo::Read's, and
        // HandleMoveTeleportAckOpcode's -- has a row), but a rejection must count
        // somewhere rather than be dropped silently.
        struct UnknownOpcode
        {
            std::atomic<uint32> rejected{ 0 };
            std::atomic<bool>   hasFirst{ false };
            std::string         first;
        };
        UnknownOpcode g_unknownOpcode;

        std::mutex g_firstLock;

        std::vector<Row>& Rows()
        {
            // Leaked on purpose: the process is exiting when the last reader runs
            // (Master::ShutdownWorld's report, then whatever static destruction
            // follows), and static destruction order across singletons is not
            // ours to control.
            static std::vector<Row>* rows = new std::vector<Row>(Wire::RegistrySize() + Wire::FamilyCount());
            return *rows;
        }

        /// The row `opcode` counts in, or -1 when neither the registry nor the
        /// family table names it. Registry rows come first so an existing index
        /// keeps its meaning. An embedded layout answers with its registry index;
        /// that row is written by BridgeCheck and Rejected (the record reads the
        /// embedded block through them) and read by nothing in Outbound, whose
        /// callers gate on IsKnown/IsPacketLayout.
        int RowIndex(uint16 opcode)
        {
            const int registry = Wire::RegistryIndex(opcode);
            if (registry >= 0) { return registry; }
            const int family = Wire::FamilyIndex(opcode);
            if (family >= 0) { return int(Wire::RegistrySize()) + family; }
            return -1;
        }

        /// The opcode a row counts, for the report: the registry's own for a
        /// registry row, the family table's for a family row.
        uint16 RowOpcode(size_t i)
        {
            if (i < Wire::RegistrySize()) { return Wire::AllSequences().begin[i].opcode; }
            return Wire::FamilyOpcodeAt(i - Wire::RegistrySize());
        }

        // Shared by a Row and by g_unknownOpcode, which is not one.
        void NoteFirst(std::atomic<bool>& hasFirst, std::string& dest, std::string const& text)
        {
            if (hasFirst.load(std::memory_order_acquire))
            {
                return;
            }
            std::lock_guard<std::mutex> guard(g_firstLock);
            if (!hasFirst.load(std::memory_order_relaxed))
            {
                dest = text;
                hasFirst.store(true, std::memory_order_release);
            }
        }

    }

    void Enable(bool on) { g_enabled.store(on, std::memory_order_release); }
    bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

    void Rejected(uint16 opcode, Wire::DecodeError error)
    {
        char text[128];
        std::snprintf(text, sizeof(text), "0x%.4X %s: rejected, decode %s", uint32(opcode),
                      LookupOpcodeName(opcode), Wire::ErrorName(error));
        const int i = RowIndex(opcode);
        if (i < 0)
        {
            ++g_unknownOpcode.rejected;
            NoteFirst(g_unknownOpcode.hasFirst, g_unknownOpcode.first, text);
            return;
        }
        Row& row = Rows()[size_t(i)];
        ++row.inRejected;
        NoteFirst(row.hasFirst, row.first, text);
    }

    void BridgeCheck(uint16 opcode, Wire::MovementStatus const& decoded, MovementInfo const& record)
    {
        if (!Enabled()) { return; }
        const int i = RowIndex(opcode);
        if (i < 0) { return; }
        Row& row = Rows()[size_t(i)];
        ++row.inSeen;
        Wire::MovementStatus const back = Movement::ToWire(record);
        if (back == decoded) { return; }
        ++row.inBridgeMismatch;
        char const* field = Wire::FirstDifference(decoded, back);
        char text[128];
        std::snprintf(text, sizeof(text), "0x%.4X %s: bridge round trip differs at %s", uint32(opcode),
                      LookupOpcodeName(opcode), field ? field : "?");
        NoteFirst(row.hasFirst, row.first, text);
    }

    void Outbound(uint16 opcode, WorldPacket const& packet)
    {
        if (!Enabled()) { return; }
        if (!Wire::IsKnown(opcode)) { return; }
        Row& row = Rows()[size_t(RowIndex(opcode))];
        ++row.outSeen;
        // SendPacket flushed this packet's trailing bits before the hook ran.
        const Wire::Verdict v = Wire::Judge(opcode, packet, false);
        if (!v.decoded)
        {
            ++row.outFailed;
            char text[128];
            std::snprintf(text, sizeof(text), "0x%.4X %s: outbound decode %s, consumed %u, payload %u", uint32(opcode),
                          LookupOpcodeName(opcode), Wire::ErrorName(v.result.error), uint32(v.result.consumed), uint32(packet.size()));
            NoteFirst(row.hasFirst, row.first, text);
            return;
        }
        if (!v.exact)
        {
            // The offset, not the two lengths: an inexact re-encoding is often
            // exactly as long as the packet, and then the lengths say nothing
            // about which field the writer got wrong.
            ++row.outInexact;
            char text[160];
            std::snprintf(text, sizeof(text), "0x%.4X %s: outbound re-encodes to %u byte(s), %u on the wire, first difference at byte %ld",
                          uint32(opcode), LookupOpcodeName(opcode), uint32(v.reencoded), uint32(packet.size()), v.firstDifference);
            NoteFirst(row.hasFirst, row.first, text);
        }
    }

    bool Saw()
    {
        for (Row const& r : Rows())
        {
            if (r.inSeen.load(std::memory_order_relaxed) != 0 || r.inRejected.load(std::memory_order_relaxed) != 0 ||
                r.inBridgeMismatch.load(std::memory_order_relaxed) != 0 || r.outSeen.load(std::memory_order_relaxed) != 0 ||
                r.outFailed.load(std::memory_order_relaxed) != 0 || r.outInexact.load(std::memory_order_relaxed) != 0)
            {
                return true;
            }
        }
        return g_unknownOpcode.rejected.load(std::memory_order_relaxed) != 0;
    }

    namespace
    {
        // The one line Report() opens with: the totals across every row. Nothing
        // outside this file asks for it -- the GM command and the shutdown log
        // both go through Report -- so it stays file-local.
        std::string Summary()
        {
            uint32 inSeen = 0, inRejected = 0, inBridgeMismatch = 0, outSeen = 0, outFailed = 0, outInexact = 0;
            for (Row const& r : Rows())
            {
                inSeen += r.inSeen; inRejected += r.inRejected; inBridgeMismatch += r.inBridgeMismatch;
                outSeen += r.outSeen; outFailed += r.outFailed; outInexact += r.outInexact;
            }
            inRejected += g_unknownOpcode.rejected.load(std::memory_order_relaxed);
            char text[256];
            std::snprintf(text, sizeof(text),
                          "wire parity: in %u seen, %u rejected, %u bridge-mismatched; out %u seen, %u failed, %u inexact",
                          inSeen, inRejected, inBridgeMismatch, outSeen, outFailed, outInexact);
            return text;
        }
    }

    void Report(std::function<void(std::string const&)> const& line)
    {
        line(Summary());
        std::vector<Row>& rows = Rows();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            Row const& row = rows[i];
            if (row.inSeen == 0 && row.inRejected == 0 && row.inBridgeMismatch == 0 &&
                row.outSeen == 0 && row.outFailed == 0 && row.outInexact == 0)
            {
                continue;
            }
            const uint16 opcode = RowOpcode(i);
            char text[256];
            std::snprintf(text, sizeof(text), "  0x%.4X %-44s in %u/%u/%u  out %u/%u inexact %u",
                          uint32(opcode), LookupOpcodeName(opcode),
                          uint32(row.inSeen), uint32(row.inRejected), uint32(row.inBridgeMismatch),
                          uint32(row.outSeen), uint32(row.outFailed), uint32(row.outInexact));
            line(text);
            if (row.hasFirst.load(std::memory_order_acquire))
            {
                line("    " + row.first);
            }
        }
        if (g_unknownOpcode.rejected.load(std::memory_order_relaxed) != 0)
        {
            char text[64];
            std::snprintf(text, sizeof(text), "  (unknown opcode) rejected %u", uint32(g_unknownOpcode.rejected));
            line(text);
            if (g_unknownOpcode.hasFirst.load(std::memory_order_acquire))
            {
                line("    " + g_unknownOpcode.first);
            }
        }
        line("  columns: in seen/rejected/bridge-mismatched, out seen/failed, inexact");
        line("  inexact: decoded whole but re-encodes to different bytes");
    }
}
