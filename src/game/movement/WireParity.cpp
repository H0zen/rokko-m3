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

#include "Unit.h"
#include "Geometry/Placement.h"
#include "OpcodeTable.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "wire/MovementCodec.h"
#include "wire/MovementFamilies.h"
#include "wire/MovementParity.h"
#include "wire/MovementSequences.h"
#include "wire/MoverCodec.h"
#include "wire/TeleportCodec.h"

#include <atomic>
#include <cstdio>
#include <cstring>
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
            std::atomic<uint32> inFailed{ 0 };
            std::atomic<uint32> inMismatch{ 0 };
            std::atomic<uint32> labelSwapped{ 0 };
            std::atomic<uint32> vehicleIdInFallTime{ 0 };
            std::atomic<uint32> outSeen{ 0 };
            std::atomic<uint32> outFailed{ 0 };
            std::atomic<uint32> outInexact{ 0 };
            std::atomic<bool>   hasFirst{ false };
            std::string         first;
        };

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
        /// keeps its meaning. An embedded layout answers with its registry index,
        /// which is a row nothing writes to: every caller gates on IsKnown or
        /// IsPacketLayout first, and both reject an embedded layout.
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

        void NoteFirst(Row& row, std::string const& text)
        {
            if (row.hasFirst.load(std::memory_order_acquire))
            {
                return;
            }
            std::lock_guard<std::mutex> guard(g_firstLock);
            if (!row.hasFirst.load(std::memory_order_relaxed))
            {
                row.first = text;
                row.hasFirst.store(true, std::memory_order_release);
            }
        }

        // Where the codec's decode and the legacy status disagree, sorted into the
        // three bins the header describes.
        void Compare(Row& row, uint16 opcode, Wire::MovementStatus const& wire, MovementInfo const& legacy, bool relayed)
        {
            Wire::MovementStatus expected = ToWire(legacy, wire);
            if (relayed)
            {
                // The relay writer wraps the orientation into [0, 2pi) (Unit.cpp:480,
                // :537); the client's own packets are compared raw, where a wrapped
                // expectation would hide a reader defect.
                if (expected.has.orientation)
                {
                    expected.pos.o = Geometry::Placement::NormalizeOrientation(expected.pos.o);
                }
                if (expected.transport.present)
                {
                    expected.transport.pos.o = Geometry::Placement::NormalizeOrientation(expected.transport.pos.o);
                }
            }
            char const* field = Wire::FirstDifference(wire, expected);
            if (!field)
            {
                return;
            }
            if ((std::strcmp(field, "fall.cosAngle") == 0 || std::strcmp(field, "fall.sinAngle") == 0) &&
                wire.fall.cosAngle == expected.fall.sinAngle && wire.fall.sinAngle == expected.fall.cosAngle)
            {
                Wire::MovementStatus crossed = expected;
                crossed.fall.cosAngle = expected.fall.sinAngle;
                crossed.fall.sinAngle = expected.fall.cosAngle;
                if (!Wire::FirstDifference(wire, crossed))
                {
                    ++row.labelSwapped;
                    return;
                }
            }
            if (std::strcmp(field, "fall.time") == 0 && wire.transport.present && wire.transport.hasVehicleId &&
                expected.fall.time == wire.transport.vehicleId)
            {
                // fall.time comes before fall.vertical/horizontal/cosAngle/sinAngle and
                // the whole transport block in struct order, so FirstDifference stopping
                // here does not clear those fields -- patch fall.time to what the wire
                // actually carried and re-compare the rest before crediting the quirk.
                Wire::MovementStatus patched = expected;
                patched.fall.time = wire.fall.time;
                char const* patchedField = Wire::FirstDifference(wire, patched);
                if (!patchedField)
                {
                    ++row.vehicleIdInFallTime;
                    return;
                }
                field = patchedField;
            }
            ++row.inMismatch;
            char text[128];
            std::snprintf(text, sizeof(text), "0x%.4X %s: first mismatch in %s", uint32(opcode),
                          LookupOpcodeName(opcode), field);
            NoteFirst(row, text);
        }

        // The one body both compared directions run: decode a copy of the packet
        // whole with its layout, then compare against the legacy status. `relayed`
        // says the bytes came from this server's legacy writer, which has not
        // flushed its trailing bits yet (WorldSession::SendPacket does that later),
        // rather than from the client, whose packet the legacy reader has just read
        // and whose bit cursor therefore holds read state.
        //
        // Not "Judge": Wire::Judge is the wire's own round-trip verdict, used a
        // few lines below in Outbound, and two functions of that name in one unit
        // is one too many.
        void CompareToLegacy(Row& row, uint16 opcode, WorldPacket const& packet, MovementInfo const& legacy, bool relayed)
        {
            ++row.inSeen;
            Wire::MovementStatus wire;
            Wire::DecodeResult result;
            if (!Wire::DecodeWhole(packet, Wire::SequenceFor(opcode), wire, result, relayed))
            {
                ++row.inFailed;
                char text[128];
                std::snprintf(text, sizeof(text), "0x%.4X %s: decode %s, consumed %u, payload %u", uint32(opcode),
                              LookupOpcodeName(opcode), Wire::ErrorName(result.error), uint32(result.consumed), uint32(packet.size()));
                NoteFirst(row, text);
                return;
            }
            Compare(row, opcode, wire, legacy, relayed);
        }
    }

    void Enable(bool on) { g_enabled.store(on, std::memory_order_release); }
    bool Enabled() { return g_enabled.load(std::memory_order_acquire); }

    Wire::MovementStatus ToWire(MovementInfo const& legacy, Wire::MovementStatus const& wireOnly)
    {
        MovementInfo::StatusInfo const& si = legacy.GetStatusInfo();
        Wire::MovementStatus w;
        w.guid   = legacy.GetGuid().GetRawValue();
        w.guid2  = legacy.GetGuid2().GetRawValue();
        w.flags  = uint32(legacy.GetMovementFlags());
        w.flags2 = uint32(legacy.GetMovementFlags2());
        w.has.timestamp = si.hasTimeStamp;
        w.time = si.hasTimeStamp ? legacy.GetTime() : 0;
        w.pos.x = legacy.GetPos()->x;
        w.pos.y = legacy.GetPos()->y;
        w.pos.z = legacy.GetPos()->z;
        w.has.orientation = si.hasOrientation;
        w.pos.o = si.hasOrientation ? legacy.GetPos()->o : 0.0f;
        w.has.pitch = si.hasPitch;
        w.pitch = si.hasPitch ? legacy.GetPitch() : 0.0f;
        w.has.spline = si.hasSpline;
        w.has.splineElevation = si.hasSplineElevation;
        w.splineElevation = si.hasSplineElevation ? legacy.GetSplineElevation() : 0.0f;
        w.fall.present = si.hasFallData;
        w.fall.hasDirection = si.hasFallDirection;
        if (si.hasFallData)
        {
            w.fall.time = legacy.GetFallTime();
            w.fall.vertical = legacy.GetJumpInfo().velocity;
            if (si.hasFallDirection)
            {
                w.fall.horizontal = legacy.GetJumpInfo().xyspeed;
                w.fall.cosAngle = legacy.GetJumpInfo().cosAngle;
                w.fall.sinAngle = legacy.GetJumpInfo().sinAngle;
            }
        }
        // MovementInfo does not expose the HasTransportData gate (it is a local in
        // MovementInfo::Read), so a non-empty transport guid is the only signal: a
        // client that sends the gate with a zero guid shows as a transport.present
        // disagreement, which is right.
        w.transport.present = !legacy.GetTransportGuid().IsEmpty();
        if (w.transport.present)
        {
            w.transport.guid = legacy.GetTransportGuid().GetRawValue();
            w.transport.pos.x = legacy.GetTransportPos()->x;
            w.transport.pos.y = legacy.GetTransportPos()->y;
            w.transport.pos.z = legacy.GetTransportPos()->z;
            w.transport.pos.o = legacy.GetTransportPos()->o;
            w.transport.time = legacy.GetTransportTime();
            w.transport.seat = legacy.GetTransportSeat();
            w.transport.hasTime2 = si.hasTransportTime2;
            w.transport.time2 = si.hasTransportTime2 ? legacy.GetTransportTime2() : 0;
            w.transport.hasVehicleId = si.hasTransportTime3;
        }
        w.byteParam = legacy.GetByteParam();
        // What the legacy reader never carries: take the codec's own reading.
        w.counter = wireOnly.counter;
        w.value = wireOnly.value;
        w.twoBits = wireOnly.twoBits;
        w.has.unknownBit = wireOnly.has.unknownBit;
        w.has.emptyFlagsBlock = wireOnly.has.emptyFlagsBlock;
        w.has.emptyFlags2Block = wireOnly.has.emptyFlags2Block;
        w.has.heightChangeFailed = wireOnly.has.heightChangeFailed;
        w.transport.vehicleId = wireOnly.transport.vehicleId;
        return w;
    }

    void Inbound(uint16 opcode, WorldPacket const& packet, MovementInfo const& legacy)
    {
        if (!Enabled()) { return; }
        if (!Wire::IsPacketLayout(opcode)) { return; }
        CompareToLegacy(Rows()[size_t(RowIndex(opcode))], opcode, packet, legacy, false);
    }

    void Relay(uint16 opcode, WorldPacket const& packet, MovementInfo const& legacy)
    {
        // The same comparison as Inbound; the bytes came from the legacy writer
        // instead of the client, which is what makes it a test of that writer --
        // and that writer has not flushed its trailing bits yet.
        if (!Enabled()) { return; }
        if (!Wire::IsPacketLayout(opcode)) { return; }
        CompareToLegacy(Rows()[size_t(RowIndex(opcode))], opcode, packet, legacy, true);
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
            NoteFirst(row, text);
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
            NoteFirst(row, text);
        }
    }

    void InboundMover(WorldPacket const& packet, uint64 sessionMover)
    {
        if (!Enabled()) { return; }
        const int i = RowIndex(CMSG_SET_ACTIVE_MOVER);
        if (i < 0) { return; }
        Row& row = Rows()[size_t(i)];
        ++row.inSeen;
        WorldPacket copy(packet);
        copy.rpos(0);
        copy.ResetBitReader();
        Wire::ActiveMover value;
        Wire::DecodeResult r = Wire::DecodeActiveMover(copy, CMSG_SET_ACTIVE_MOVER, value);
        // A decode that stopped short of the payload is a short read, not a
        // success -- Wire::Judge calls that LeftBytes, and so does this.
        if (r.ok() && r.consumed != copy.size()) { r.error = Wire::DecodeError::LeftBytes; }
        if (!r.ok())
        {
            ++row.inFailed;
            char text[128];
            std::snprintf(text, sizeof(text), "0x3314 CMSG_SET_ACTIVE_MOVER: decode %s, consumed %u of %u",
                          Wire::ErrorName(r.error), uint32(r.consumed), uint32(copy.size()));
            NoteFirst(row, text);
            return;
        }
        if (value.guid != sessionMover)
        {
            // The client naming a mover this session does not hold -- the
            // disagreement the legacy handler's own check was written to catch.
            ++row.inMismatch;
            char text[128];
            std::snprintf(text, sizeof(text), "0x3314 CMSG_SET_ACTIVE_MOVER: guid %llu, session mover %llu",
                          (unsigned long long)value.guid, (unsigned long long)sessionMover);
            NoteFirst(row, text);
        }
    }

    void InboundTeleportAck(WorldPacket const& packet, uint32 legacyCounter, uint32 legacyTime, uint64 legacyGuid)
    {
        if (!Enabled()) { return; }
        const int i = RowIndex(CMSG_MOVE_TELEPORT_ACK);
        if (i < 0) { return; }
        Row& row = Rows()[size_t(i)];
        ++row.inSeen;
        WorldPacket copy(packet);
        copy.rpos(0);
        copy.ResetBitReader();
        Wire::TeleportAck value;
        Wire::DecodeResult r = Wire::DecodeTeleportAck(copy, value);
        if (r.ok() && r.consumed != copy.size()) { r.error = Wire::DecodeError::LeftBytes; }
        if (!r.ok())
        {
            ++row.inFailed;
            char text[128];
            std::snprintf(text, sizeof(text), "0x390C CMSG_MOVE_TELEPORT_ACK: decode %s, consumed %u of %u",
                          Wire::ErrorName(r.error), uint32(r.consumed), uint32(copy.size()));
            NoteFirst(row, text);
            return;
        }
        // The first field that differs, in the order the packet carries them.
        char const* field = NULL;
        unsigned long long mine = 0, theirs = 0;
        if (value.counter != legacyCounter)   { field = "counter"; mine = value.counter; theirs = legacyCounter; }
        else if (value.time != legacyTime)    { field = "time";    mine = value.time;    theirs = legacyTime; }
        else if (value.guid != legacyGuid)    { field = "guid";    mine = value.guid;    theirs = legacyGuid; }
        if (field)
        {
            ++row.inMismatch;
            char text[128];
            std::snprintf(text, sizeof(text), "0x390C CMSG_MOVE_TELEPORT_ACK: %s %llu, legacy read %llu", field, mine, theirs);
            NoteFirst(row, text);
        }
    }

    bool Saw()
    {
        for (Row const& r : Rows())
        {
            if (r.inSeen.load(std::memory_order_relaxed) != 0 || r.outSeen.load(std::memory_order_relaxed) != 0)
            {
                return true;
            }
        }
        return false;
    }

    namespace
    {
        // The one line Report() opens with: the totals across every row. Nothing
        // outside this file asks for it -- the GM command and the shutdown log
        // both go through Report -- so it stays file-local.
        std::string Summary()
        {
            uint32 inSeen = 0, inFailed = 0, inMismatch = 0, swapped = 0, vehicle = 0, outSeen = 0, outFailed = 0, outInexact = 0;
            for (Row const& r : Rows())
            {
                inSeen += r.inSeen; inFailed += r.inFailed; inMismatch += r.inMismatch;
                swapped += r.labelSwapped; vehicle += r.vehicleIdInFallTime;
                outSeen += r.outSeen; outFailed += r.outFailed; outInexact += r.outInexact;
            }
            char text[256];
            std::snprintf(text, sizeof(text),
                          "wire parity %s: in %u seen, %u failed, %u mismatched (%u fall-label swapped, %u vehicle id in fall time); out %u seen, %u failed, %u inexact",
                          Enabled() ? "on" : "off", inSeen, inFailed, inMismatch, swapped, vehicle, outSeen, outFailed, outInexact);
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
            if (row.inSeen == 0 && row.outSeen == 0)
            {
                continue;
            }
            const uint16 opcode = RowOpcode(i);
            char text[256];
            std::snprintf(text, sizeof(text), "  0x%.4X %-44s in %u/%u/%u (swapped %u, vehicle %u)  out %u/%u inexact %u",
                          uint32(opcode), LookupOpcodeName(opcode),
                          uint32(row.inSeen), uint32(row.inFailed), uint32(row.inMismatch),
                          uint32(row.labelSwapped), uint32(row.vehicleIdInFallTime),
                          uint32(row.outSeen), uint32(row.outFailed), uint32(row.outInexact));
            line(text);
            if (row.hasFirst.load(std::memory_order_acquire))
            {
                line("    " + row.first);
            }
        }
        line("  columns: in seen/failed/mismatched (the SMSG_PLAYER_MOVE row counts the relays this server built), out seen/failed, inexact");
        line("  inexact: decoded whole but re-encodes to different bytes");
        line("  vehicle counts only packets carrying both a fall block and a transport vehicle id; a 0 is not evidence the defect is absent");
    }
}
