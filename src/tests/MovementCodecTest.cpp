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

// The 4.3.4 movement-status codec in proto/wire. Everything here is pure: bytes
// in, a value out, and back. The legacy reader lives in src/game (Unit.cpp) and
// stays live until P2; these tests pin the replacement before it goes anywhere
// near the server.

#include "TestHarness.h"

#include <cstring>

#include "wire/MovementStatus.h"
#include "wire/MovementElements.h"

TEST(MovementStatus_defaults_are_a_plain_standing_mover)
{
    Wire::MovementStatus s;
    CHECK_EQ(s.guid, uint64(0));
    CHECK_EQ(s.flags, uint32(0));
    CHECK_EQ(s.flags2, uint32(0));
    CHECK_EQ(s.time, uint32(0));
    CHECK_EQ(s.counter, uint32(0));
    CHECK(s.has.orientation);
    CHECK(s.has.timestamp);
    CHECK(!s.has.pitch);
    CHECK(!s.has.spline);
    CHECK(!s.has.splineElevation);
    CHECK(!s.fall.present);
    CHECK(!s.transport.present);
    CHECK_EQ(int(s.transport.seat), -1);

    Wire::MovementStatus t;
    CHECK(s == t);
    t.pos.x = 1.0f;
    CHECK(!(s == t));

    // The vocabulary is closed by End; the count is what sequence tables size by.
    CHECK(int(Wire::Element::End) > int(Wire::Element::MovementCounter));
}

#include "wire/MovementCodec.h"
#include "Utilities/ByteBuffer.h"

namespace
{
    // A deliberately tiny layout that exercises every mechanism the real ones
    // use -- bit block, a byte-aligned float after bits (forces a flush), a
    // uint32, and the mask-then-xor GUID bytes -- with bytes small enough to
    // derive by hand. Real layouts are in Task 4; they are round-tripped, not
    // hand-derived: their bytes come from the client binary in P1.
    const Wire::Element kTinySequence[] =
    {
        Wire::Element::HasTimestamp,
        Wire::Element::GuidBit3,
        Wire::Element::GuidBit0,
        Wire::Element::HasUnknownBit,
        Wire::Element::PositionX,
        Wire::Element::Timestamp,
        Wire::Element::GuidByte0,
        Wire::Element::GuidByte3,
        Wire::Element::End
    };

    Wire::MovementStatus TinyFixture()
    {
        Wire::MovementStatus s;
        s.guid = 0x000000000A000008ull; // byte0 = 0x08, byte3 = 0x0A, the rest zero
        s.has.timestamp = true;
        s.time = 0x11223344;
        s.pos.x = 1.0f;                 // 0x3F800000
        return s;
    }
}

TEST(MovementCodec_encode_matches_the_hand_derived_bytes)
{
    // Bit block, MSB first: HasTimestamp is written inverted (present -> 0),
    // GuidBit3 = 1 (0x0A != 0), GuidBit0 = 1 (0x08 != 0), unknown = 0.
    //   0b0110_0000 = 0x60, flushed by the float that follows.
    // Then 1.0f little-endian, then the timestamp little-endian, then the two
    // GUID bytes as (byte ^ 1) because their mask bits were set.
    ByteBuffer out;
    Wire::Encode(out, kTinySequence, TinyFixture());
    CHECK_BYTES(out.contents(), out.size(),
                { 0x60,
                  0x00, 0x00, 0x80, 0x3F,
                  0x44, 0x33, 0x22, 0x11,
                  0x09,
                  0x0B });
}

TEST(MovementCodec_carries_the_unnamed_bit)
{
    Wire::MovementStatus s = TinyFixture();
    s.has.unknownBit = true;

    ByteBuffer out;
    Wire::Encode(out, kTinySequence, s);
    CHECK_BYTES(out.contents(), out.size(),
                { 0x70,
                  0x00, 0x00, 0x80, 0x3F,
                  0x44, 0x33, 0x22, 0x11,
                  0x09,
                  0x0B });

    Wire::MovementStatus got;
    REQUIRE(Wire::Decode(out, kTinySequence, got).ok());
    CHECK(got.has.unknownBit);
    CHECK(got == s);
}

TEST(MovementCodec_decode_reads_the_golden_back)
{
    ByteBuffer buf;
    buf.append<uint8>(0x60);
    buf << float(1.0f);
    buf << uint32(0x11223344);
    buf << uint8(0x09) << uint8(0x0B);

    Wire::MovementStatus got;
    const Wire::DecodeResult r = Wire::Decode(buf, kTinySequence, got);
    REQUIRE(r.ok());
    CHECK_EQ(r.consumed, size_t(11));
    CHECK(got == TinyFixture());
}

TEST(MovementCodec_round_trips_the_tiny_layout)
{
    ByteBuffer buf;
    Wire::Encode(buf, kTinySequence, TinyFixture());

    Wire::MovementStatus got;
    REQUIRE(Wire::Decode(buf, kTinySequence, got).ok());
    CHECK(got == TinyFixture());
}

TEST(MovementCodec_decode_reports_a_truncated_packet_instead_of_throwing)
{
    ByteBuffer full;
    Wire::Encode(full, kTinySequence, TinyFixture());

    // Every strict prefix of a valid packet must come back as Overread, never
    // as an exception or a half-filled status the caller might trust.
    for (size_t keep = 0; keep < full.size(); ++keep)
    {
        ByteBuffer cut;
        cut.append(full.contents(), keep);
        Wire::MovementStatus got;
        const Wire::DecodeResult r = Wire::Decode(cut, kTinySequence, got);
        CHECK(r.error == Wire::DecodeError::Overread);
    }
}

TEST(MovementCodec_decode_refuses_a_null_sequence)
{
    ByteBuffer buf;
    buf << uint32(0);
    Wire::MovementStatus got;
    const Wire::DecodeResult r = Wire::Decode(buf, nullptr, got);
    CHECK(r.error == Wire::DecodeError::NoSequence);
    CHECK_EQ(r.consumed, size_t(0));
}

#include "wire/MovementSequences.h"
#include "Opcodes.h"

namespace
{
    // A mover with every optional block populated, so a round trip proves each
    // element writes what it reads: on a transport, mid-fall with a direction,
    // pitched, spline-elevated, with a counter.
    Wire::MovementStatus FullFixture()
    {
        Wire::MovementStatus s;
        s.guid = 0x0F0E0D0C0B0A0123ull;   // every byte non-zero; byte 1 is 0x01 so the (byte ^ 1) == 0x00 edge stays covered
        s.guid2 = 0x0000000000000000ull;
        s.flags = 0x00000001 | 0x00000200 | 0x00100000;   // forward | falling | swimming-ish bits, any 30-bit value
        s.flags2 = 0x00000040;
        s.has.timestamp = true;
        s.time = 0x000ABCDEu;
        s.pos = { -8949.95f, -132.493f, 83.5312f, 0.5f };
        s.has.orientation = true;
        s.has.pitch = true;
        s.pitch = -0.25f;
        s.has.spline = false;
        s.has.splineElevation = true;
        s.splineElevation = 2.5f;
        s.counter = 7;
        s.byteParam = 3;

        s.fall.present = true;
        s.fall.hasDirection = true;
        s.fall.time = 1200;
        s.fall.vertical = -7.9f;
        s.fall.horizontal = 7.0f;
        s.fall.cosAngle = 0.6f;
        s.fall.sinAngle = 0.8f;

        s.transport.present = true;
        s.transport.guid = 0x1F40A1B2C3C0FFEEull; // every byte non-zero
        s.transport.pos = { 1.5f, -2.5f, 3.5f, 1.0f };
        s.transport.time = 555;
        s.transport.hasTime2 = true;
        s.transport.time2 = 556;
        s.transport.hasVehicleId = true;
        s.transport.vehicleId = 557;
        s.transport.seat = 2;
        return s;
    }

    // Round-trip a fixture through one opcode's layout and hand back what came out.
    Wire::MovementStatus RoundTrip(uint16 opcode, Wire::MovementStatus const& in)
    {
        ByteBuffer buf;
        Wire::Encode(buf, Wire::SequenceFor(opcode), in);
        Wire::MovementStatus out;
        const Wire::DecodeResult r = Wire::Decode(buf, Wire::SequenceFor(opcode), out);
        // CHECK, not REQUIRE: REQUIRE's bare `return;` cannot compile in a
        // function that returns a value. A failed decode leaves `out` at its
        // defaults, which the caller's equality check then reports.
        CHECK(r.ok());
        CHECK_EQ(r.consumed, buf.size());
        return out;
    }
}

TEST(MovementSequences_unknown_opcode_has_no_layout)
{
    CHECK(Wire::SequenceFor(0xFFFF) == nullptr);
    CHECK(Wire::SequenceFor(0x0000) == nullptr);
}

TEST(MovementSequences_heartbeat_round_trips_a_full_mover)
{
    // Heartbeat carries no counter and no byteParam; everything else must survive.
    Wire::MovementStatus expect = FullFixture();
    expect.counter = 0;
    expect.byteParam = 0;
    CHECK(RoundTrip(MSG_MOVE_HEARTBEAT, FullFixture()) == expect);
}

TEST(MovementSequences_player_move_round_trips_a_full_mover)
{
    Wire::MovementStatus expect = FullFixture();
    expect.counter = 0;
    expect.byteParam = 0;
    CHECK(RoundTrip(SMSG_PLAYER_MOVE, FullFixture()) == expect);
}

TEST(MovementSequences_start_forward_and_stop_round_trip_a_full_mover)
{
    Wire::MovementStatus expect = FullFixture();
    expect.counter = 0;
    expect.byteParam = 0;
    CHECK(RoundTrip(CMSG_MOVE_START_FORWARD, FullFixture()) == expect);
    CHECK(RoundTrip(CMSG_MOVE_STOP, FullFixture()) == expect);
}

TEST(MovementSequences_a_bare_mover_round_trips_without_optional_blocks)
{
    // No transport, no fall, no pitch, no spline elevation, zero flags: every
    // "has" bit takes its absent value and no optional byte is written.
    Wire::MovementStatus bare;
    bare.guid = 0x0000000000000042ull;
    bare.pos = { 10.0f, 20.0f, 30.0f, 1.25f };
    bare.time = 99;
    CHECK(RoundTrip(MSG_MOVE_HEARTBEAT, bare) == bare);
    CHECK(RoundTrip(SMSG_PLAYER_MOVE, bare) == bare);
}

TEST(MovementCodec_carries_the_movement_counter)
{
    // The legacy reader skips MSEMovementCounter; the acks P2 builds need it.
    const Wire::Element seq[] =
    {
        Wire::Element::MovementCounter,
        Wire::Element::PositionX,
        Wire::Element::End
    };
    Wire::MovementStatus in;
    in.counter = 0xDEADBEEF;
    in.pos.x = 4.0f;

    ByteBuffer buf;
    Wire::Encode(buf, seq, in);
    CHECK_BYTES(buf.contents(), buf.size(), { 0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x80, 0x40 });

    Wire::MovementStatus out;
    REQUIRE(Wire::Decode(buf, seq, out).ok());
    CHECK_EQ(out.counter, uint32(0xDEADBEEF));
}

namespace
{
    // Position of `e` in `seq`, or -1 when the layout does not carry it.
    int IndexOf(Wire::Sequence seq, Wire::Element e)
    {
        for (int i = 0; seq[i] != Wire::Element::End; ++i)
        {
            if (seq[i] == e)
            {
                return i;
            }
        }
        return -1;
    }

    // A gated element must be preceded by the bit announcing it -- the decoder
    // can only know what to read after it has read that bit. A layout that
    // broke this would round-trip (both sides share the table) and still be
    // undecodable on the real wire.
    void CheckAnnouncedBefore(Wire::Sequence s, Wire::Element flag, Wire::Element gated)
    {
        const int gatedAt = IndexOf(s, gated);
        if (gatedAt < 0)
        {
            return; // the layout does not carry that element
        }
        const int flagAt = IndexOf(s, flag);
        CHECK(flagAt >= 0 && flagAt < gatedAt);
    }

    void CheckLayoutAnnouncesBeforeItReads(uint16 opcode)
    {
        using E = Wire::Element;
        const Wire::Sequence s = Wire::SequenceFor(opcode);
        REQUIRE(s != nullptr);

        CheckAnnouncedBefore(s, E::HasTimestamp, E::Timestamp);
        CheckAnnouncedBefore(s, E::HasPitch, E::Pitch);
        CheckAnnouncedBefore(s, E::HasOrientation, E::PositionO);
        CheckAnnouncedBefore(s, E::HasSplineElevation, E::SplineElevation);
        CheckAnnouncedBefore(s, E::HasMovementFlags, E::Flags);
        CheckAnnouncedBefore(s, E::HasMovementFlags2, E::Flags2);

        CheckAnnouncedBefore(s, E::HasFallData, E::HasFallDirection);
        CheckAnnouncedBefore(s, E::HasFallData, E::FallTime);
        CheckAnnouncedBefore(s, E::HasFallData, E::FallVerticalSpeed);
        CheckAnnouncedBefore(s, E::HasFallDirection, E::FallHorizontalSpeed);
        CheckAnnouncedBefore(s, E::HasFallDirection, E::FallCosAngle);
        CheckAnnouncedBefore(s, E::HasFallDirection, E::FallSinAngle);

        for (int i = 0; i < 8; ++i)
        {
            CheckAnnouncedBefore(s, E::HasTransportData, E(int(E::TransportGuidBit0) + i));
            CheckAnnouncedBefore(s, E::HasTransportData, E(int(E::TransportGuidByte0) + i));
        }

        // A GUID byte is read only if its mask bit said so: mask bit before byte, all three GUIDs.
        for (int i = 0; i < 8; ++i)
        {
            CheckAnnouncedBefore(s, E(int(E::GuidBit0) + i),          E(int(E::GuidByte0) + i));
            CheckAnnouncedBefore(s, E(int(E::Guid2Bit0) + i),         E(int(E::Guid2Byte0) + i));
            CheckAnnouncedBefore(s, E(int(E::TransportGuidBit0) + i), E(int(E::TransportGuidByte0) + i));
        }
        CheckAnnouncedBefore(s, E::HasTransportData, E::HasTransportTime2);
        CheckAnnouncedBefore(s, E::HasTransportData, E::HasVehicleId);
        CheckAnnouncedBefore(s, E::HasTransportData, E::TransportSeat);
        CheckAnnouncedBefore(s, E::HasTransportData, E::TransportPositionX);
        CheckAnnouncedBefore(s, E::HasTransportData, E::TransportPositionY);
        CheckAnnouncedBefore(s, E::HasTransportData, E::TransportPositionZ);
        CheckAnnouncedBefore(s, E::HasTransportData, E::TransportPositionO);
        CheckAnnouncedBefore(s, E::HasTransportData, E::TransportTime);
        CheckAnnouncedBefore(s, E::HasTransportTime2, E::TransportTime2);
        CheckAnnouncedBefore(s, E::HasVehicleId, E::TransportVehicleId);
    }
}

TEST(MovementSequences_every_layout_announces_before_it_reads)
{
    const Wire::Registry r = Wire::AllSequences();
    CHECK(r.begin != r.end);
    for (Wire::Entry const* e = r.begin; e != r.end; ++e)
    {
        CheckLayoutAnnouncesBeforeItReads(e->opcode);
    }
}

TEST(MovementSequences_every_layout_reports_overread_on_every_strict_prefix)
{
    const Wire::Registry r = Wire::AllSequences();
    for (Wire::Entry const* e = r.begin; e != r.end; ++e)
    {
        ByteBuffer full;
        Wire::Encode(full, e->sequence, FullFixture());
        REQUIRE(full.size() > 0);
        for (size_t keep = 0; keep < full.size(); ++keep)
        {
            ByteBuffer cut;
            cut.append(full.contents(), keep);
            Wire::MovementStatus got;
            const Wire::DecodeResult d = Wire::Decode(cut, e->sequence, got);
            CHECK(d.error == Wire::DecodeError::Overread);
        }
    }
}

#include "wire/MovementCapture.h"

#include <cstdio>
#include <fstream>
#include <string>

TEST(MovementCapture_writes_one_parseable_line_per_packet)
{
    const std::string path = "mvcapture_test.log";
    std::remove(path.c_str());

    REQUIRE(Wire::MovementCapture::Open(path));
    CHECK(Wire::MovementCapture::IsOpen());

    const uint8 a[] = { 0x60, 0x00, 0x00, 0x80, 0x3F };
    const uint8 b[] = { 0xEF };
    Wire::MovementCapture::Record('C', 0x791A, a, sizeof(a));
    Wire::MovementCapture::Record('S', 0x79A2, b, sizeof(b));
    Wire::MovementCapture::Close();
    CHECK(!Wire::MovementCapture::IsOpen());

    std::ifstream in(path.c_str());
    std::string line1, line2, line3;
    std::getline(in, line1);
    std::getline(in, line2);
    const bool third = bool(std::getline(in, line3));
    CHECK_STR(line1.c_str(), "C 0x791A 600000803F");
    CHECK_STR(line2.c_str(), "S 0x79A2 EF");
    CHECK(!third);
    in.close();

    // Closed means silent: recording must be a no-op, not a crash.
    Wire::MovementCapture::Record('C', 0x0001, a, sizeof(a));
    // The stream is closed, so the removal must succeed; a leftover file is a failure.
    CHECK(std::remove(path.c_str()) == 0);
}

TEST(MovementCodec_remembers_a_present_but_zero_flags_block)
{
    // A block announced present and carrying zero has no value to derive presence
    // from, so the status remembers it; otherwise re-encoding would drop thirty bits
    // and shift every later field. Layout: the presence bit, then the 30-bit block.
    const Wire::Element seq[] = { Wire::Element::HasMovementFlags, Wire::Element::Flags, Wire::Element::End };

    // Present (the inverted bit is 0) followed by thirty zero bits: 31 bits, four zero bytes.
    ByteBuffer wire;
    wire << uint8(0x00) << uint8(0x00) << uint8(0x00) << uint8(0x00);
    Wire::MovementStatus got;
    REQUIRE(Wire::Decode(wire, seq, got).ok());
    CHECK_EQ(got.flags, uint32(0));
    CHECK(got.has.emptyFlagsBlock);

    ByteBuffer again;
    Wire::Encode(again, seq, got);
    CHECK_BYTES(again.contents(), again.size(), { 0x00, 0x00, 0x00, 0x00 });

    // The same value without that memory is "absent": one set bit, one byte.
    Wire::MovementStatus plain;
    ByteBuffer absent;
    Wire::Encode(absent, seq, plain);
    CHECK_BYTES(absent.contents(), absent.size(), { 0x80 });
}

TEST(MovementCodec_failed_decode_leaves_out_at_defaults)
{
    ByteBuffer full;
    Wire::Encode(full, kTinySequence, TinyFixture());

    // Cut after the position float: the timestamp read must fail, and nothing
    // decoded before the failure may leak into `out` -- nor may what the caller
    // left there survive.
    ByteBuffer cut;
    cut.append(full.contents(), 5);
    Wire::MovementStatus got;
    got.pos.x = 99.0f;
    const Wire::DecodeResult r = Wire::Decode(cut, kTinySequence, got);
    CHECK(r.error == Wire::DecodeError::Overread);
    CHECK(got == Wire::MovementStatus());
}

#include "WorldPacket.h"

TEST(MovementCodec_writes_the_p1_elements_in_their_slots)
{
    // Two gate bits, the constant one, a two-bit field, an explicit flush, then a
    // float and the counter in the byte section. Hand-derived: bits 1,1,1,0 make
    // 0xE0; 1.5f is 00 00 C0 3F; the counter 7 is 07 00 00 00.
    static const Wire::Element kLayout[] =
    {
        Wire::Element::HasHeightChangeFailed, Wire::Element::OneBit, Wire::Element::ExtraTwoBits,
        Wire::Element::FlushBits, Wire::Element::ExtraFloat, Wire::Element::MovementCounter,
        Wire::Element::End
    };
    Wire::MovementStatus s;
    s.has.heightChangeFailed = true;
    s.twoBits = 2;
    s.value = 1.5f;
    s.counter = 7;
    WorldPacket p(0, 16);
    Wire::Encode(p, kLayout, s);
    CHECK_BYTES(p.contents(), p.size(), { 0xE0, 0x00, 0x00, 0xC0, 0x3F, 0x07, 0x00, 0x00, 0x00 });

    Wire::MovementStatus back;
    REQUIRE(Wire::Decode(p, kLayout, back).ok());
    CHECK(back.has.heightChangeFailed);
    CHECK_EQ(int(back.twoBits), 2);
    CHECK_EQ(back.value, 1.5f);
    CHECK_EQ(back.counter, uint32(7));
    CHECK(back == s);
}

TEST(MovementCodec_carries_the_transport_vehicle_id_only_under_its_gates)
{
    // The slot the legacy header called "transport time 3" is the vehicle id, and
    // it sits behind two gates: the transport block and its own presence bit.
    static const Wire::Element kLayout[] =
    {
        Wire::Element::HasTransportData, Wire::Element::HasVehicleId,
        Wire::Element::TransportVehicleId, Wire::Element::End
    };
    Wire::MovementStatus s;
    s.transport.present = true;
    s.transport.hasVehicleId = true;
    s.transport.vehicleId = 0x1234;
    WorldPacket p(0, 8);
    Wire::Encode(p, kLayout, s);
    CHECK_BYTES(p.contents(), p.size(), { 0xC0, 0x34, 0x12, 0x00, 0x00 });

    Wire::MovementStatus back;
    REQUIRE(Wire::Decode(p, kLayout, back).ok());
    CHECK(back.transport.present);
    CHECK(back.transport.hasVehicleId);
    CHECK_EQ(back.transport.vehicleId, uint32(0x1234));
    CHECK(back == s);

    Wire::MovementStatus bare;      // no transport block: both gates closed, one zero byte
    WorldPacket q(0, 8);
    Wire::Encode(q, kLayout, bare);
    CHECK_BYTES(q.contents(), q.size(), { 0x00 });
}

TEST(MovementSequences_the_embedded_cast_block_gates_nothing_on_its_two_swapped_bits)
{
    // The legacy header and the registry named the unnamed bit and HasSpline the
    // other way round in the cast packet's movement block (P1-B's fence). Neither
    // gates a field in that table, so the bytes consumed are the same either way
    // and only two flag labels differ; the handler reads the position only.
    Wire::Sequence const seq = Wire::SequenceFor(CMSG_CAST_SPELL);
    REQUIRE(seq != nullptr);
    CHECK(Wire::IsEmbeddedLayout(CMSG_CAST_SPELL));
    bool sawSpline = false, sawUnknown = false;
    for (Wire::Sequence e = seq; *e != Wire::Element::End; ++e)
    {
        if (*e == Wire::Element::HasSpline) { sawSpline = true; }
        if (*e == Wire::Element::HasUnknownBit) { sawUnknown = true; }
        // No element of this table is read under either gate: the codec's Decode
        // consults has.spline and has.unknownBit for nothing but the flags it stores.
    }
    CHECK(sawSpline);
    CHECK(sawUnknown);
    Wire::MovementStatus a, b;
    a.guid = b.guid = 0x46; a.pos.x = b.pos.x = 1.0f;
    a.has.spline = true;  b.has.unknownBit = true;
    WorldPacket pa(CMSG_CAST_SPELL), pb(CMSG_CAST_SPELL);
    Wire::Encode(pa, seq, a);
    Wire::Encode(pb, seq, b);
    CHECK_EQ(pa.size(), pb.size());   // the two labels move a bit, never a byte

    Wire::MovementStatus a2, b2;
    Wire::DecodeResult const ra = Wire::Decode(pa, seq, a2);
    Wire::DecodeResult const rb = Wire::Decode(pb, seq, b2);
    REQUIRE(ra.ok());
    REQUIRE(rb.ok());
    CHECK_EQ(ra.consumed, rb.consumed);
    CHECK_EQ(ra.consumed, pa.size());
    CHECK(a2.has.spline && !a2.has.unknownBit);
    CHECK(!b2.has.spline && b2.has.unknownBit);
}

TEST(MovementSequences_registers_every_layout_of_the_source)
{
    const Wire::Registry r = Wire::AllSequences();
    CHECK_EQ(int(r.end - r.begin), 113);
    for (Wire::Entry const* e = r.begin; e != r.end; ++e)
    {
        REQUIRE(e->sequence != nullptr);
        REQUIRE(e->table != nullptr);
        int n = 0;
        while (e->sequence[n] != Wire::Element::End) { ++n; REQUIRE(n < 128); }
        for (Wire::Entry const* o = e + 1; o != r.end; ++o) { CHECK(o->opcode != e->opcode); }
    }
    // The families §7 names, one probe each; the client set's own layouts are
    // pinned by the goldens and the round-trip tests elsewhere in this file.
    CHECK(Wire::SequenceFor(SMSG_MOVE_SET_RUN_SPEED) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_UPDATE_RUN_SPEED) != nullptr);
    CHECK(Wire::SequenceFor(CMSG_FORCE_RUN_SPEED_CHANGE_ACK) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_SPLINE_MOVE_SET_RUN_SPEED) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_FORCE_MOVE_ROOT) != nullptr);
    CHECK(Wire::SequenceFor(CMSG_FORCE_MOVE_ROOT_ACK) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_GRAVITY_DISABLE) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_WATER_WALK) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_FEATHER_FALL) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_SET_HOVER) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_SET_COLLISION_HGT) != nullptr);
    CHECK(Wire::SequenceFor(CMSG_MOVE_SET_COLLISION_HGT_ACK) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY) != nullptr);
    CHECK(Wire::SequenceFor(CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_SPLINE_MOVE_ROOT) != nullptr);
    // Hand-written in every source; P1-C's.
    CHECK(Wire::SequenceFor(SMSG_MOVE_KNOCK_BACK) == nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_TELEPORT) == nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_SET_ACTIVE_MOVER) == nullptr);
    CHECK(Wire::SequenceFor(SMSG_CLIENT_CONTROL_UPDATE) == nullptr);
    // lifted from the client's readers (Task 5 of P1-C)
    CHECK(Wire::SequenceFor(SMSG_MOVE_UPDATE_KNOCK_BACK) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_UPDATE_RUN_BACK_SPEED) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_UPDATE_WALK_SPEED) != nullptr);
    // lifted from the client's readers with no reference counterpart at all (ADDED, Task 6
    // of P2-A). PitchRate's own reader is BLOCKED (see gen_movement_layouts.py's ADDED
    // comment) and so has no row here either.
    CHECK(Wire::SequenceFor(SMSG_MOVE_UPDATE_TELEPORT) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_UPDATE_TURN_RATE) != nullptr);
    CHECK(Wire::SequenceFor(SMSG_MOVE_UPDATE_PITCH_RATE) == nullptr);
}

TEST(MovementSequences_the_lifted_tables_carry_presence_gates)
{
    // The reason the first three were excluded: CPP's tables read gated fields with no
    // gate. The client's own readers have the gates; a lifted table must too. The other
    // two have no CPP table to have been excluded from (ADDED, Task 6 of P2-A) but read
    // the same gated fields from the same kind of reader, so the same check applies.
    const uint16 ops[5] = { SMSG_MOVE_UPDATE_KNOCK_BACK, SMSG_MOVE_UPDATE_RUN_BACK_SPEED, SMSG_MOVE_UPDATE_WALK_SPEED,
                             SMSG_MOVE_UPDATE_TELEPORT, SMSG_MOVE_UPDATE_TURN_RATE };
    for (int i = 0; i < 5; ++i)
    {
        Wire::Sequence s = Wire::SequenceFor(ops[i]);
        REQUIRE(s != nullptr);
        bool hasFallGate = false, hasTransportGate = false, readsFall = false, readsTransport = false;
        for (Wire::Sequence p = s; *p != Wire::Element::End; ++p)
        {
            if (*p == Wire::Element::HasFallData) { hasFallGate = true; }
            if (*p == Wire::Element::HasTransportData) { hasTransportGate = true; }
            if (*p == Wire::Element::FallTime) { readsFall = true; }
            if (*p == Wire::Element::TransportPositionX) { readsTransport = true; }
        }
        // The two implications below are vacuously true of a table that reads
        // neither block -- which is exactly the shape the exclusion was about --
        // so pin that these three do read both before asking whether they gate.
        CHECK(readsFall);
        CHECK(readsTransport);
        CHECK(!readsFall || hasFallGate);
        CHECK(!readsTransport || hasTransportGate);
    }
}

TEST(MovementSequences_the_lifted_tables_round_trip_a_full_status)
{
    // Every field the fixture fills survives a pass through each lifted table: all five
    // carry the whole status, gates included, so nothing may drop. Six when PitchRate's
    // lift is unblocked; see the ADDED comment in gen_movement_layouts.py.
    const uint16 ops[5] = { SMSG_MOVE_UPDATE_KNOCK_BACK, SMSG_MOVE_UPDATE_RUN_BACK_SPEED, SMSG_MOVE_UPDATE_WALK_SPEED,
                             SMSG_MOVE_UPDATE_TELEPORT, SMSG_MOVE_UPDATE_TURN_RATE };
    for (int i = 0; i < 5; ++i)
    {
        Wire::Sequence s = Wire::SequenceFor(ops[i]);
        REQUIRE(s != nullptr);
        const Wire::MovementStatus in = FullFixture();
        WorldPacket p(ops[i], 256);
        Wire::Encode(p, s, in);
        Wire::MovementStatus out;
        REQUIRE(Wire::Decode(p, s, out).ok());
        // The mover guid first: it is sixteen elements (eight presence bits and
        // eight bytes), and because the encoder and the decoder walk the same
        // table, a table missing one of them round-trips happily -- the guid is
        // the only thing that notices. The fixture's guid has every byte
        // non-zero, so every one of the eight is on the wire to be lost.
        CHECK_EQ(out.guid, in.guid);
        CHECK_EQ(out.guid2, in.guid2);
        CHECK(out.pos == in.pos);
        CHECK_EQ(out.flags, in.flags);
        CHECK_EQ(out.flags2, in.flags2);
        CHECK_EQ(out.time, in.time);
        CHECK_EQ(out.pitch, in.pitch);
        CHECK_EQ(out.splineElevation, in.splineElevation);
        CHECK_EQ(int(out.fall.present), int(in.fall.present));
        CHECK_EQ(int(out.fall.hasDirection), int(in.fall.hasDirection));
        CHECK_EQ(out.fall.time, in.fall.time);
        CHECK_EQ(out.fall.vertical, in.fall.vertical);
        CHECK_EQ(out.fall.horizontal, in.fall.horizontal);
        CHECK_EQ(out.fall.cosAngle, in.fall.cosAngle);
        CHECK_EQ(out.fall.sinAngle, in.fall.sinAngle);
        CHECK_EQ(int(out.transport.present), int(in.transport.present));
        CHECK_EQ(out.transport.guid, in.transport.guid);
        CHECK(out.transport.pos == in.transport.pos);
        CHECK_EQ(out.transport.time, in.transport.time);
        CHECK_EQ(out.transport.time2, in.transport.time2);
        CHECK_EQ(out.transport.vehicleId, in.transport.vehicleId);
        CHECK_EQ(int(out.transport.seat), int(in.transport.seat));
    }
}

TEST(MovementSequences_every_layout_re_encodes_its_own_bytes)
{
    // Layout-agnostic round trip: whatever a layout carries, decoding what it
    // wrote and writing that again must give the same bytes. This is the check
    // that catches a reader and a writer disagreeing about one element.
    const Wire::Registry r = Wire::AllSequences();
    for (Wire::Entry const* e = r.begin; e != r.end; ++e)
    {
        Wire::MovementStatus full = FullFixture();
        full.value = 7.5f;
        full.twoBits = 3;
        full.has.heightChangeFailed = true;
        full.transport.hasVehicleId = true;
        full.transport.vehicleId = 0xABCD;
        WorldPacket once(e->opcode, 256);
        Wire::Encode(once, e->sequence, full);
        Wire::MovementStatus back;
        REQUIRE(Wire::Decode(once, e->sequence, back).ok());
        WorldPacket twice(e->opcode, 256);
        Wire::Encode(twice, e->sequence, back);
        CHECK_EQ(twice.size(), once.size());
        CHECK(twice.size() == once.size() && std::memcmp(twice.contents(), once.contents(), once.size()) == 0);
    }
}

TEST(MovementSequences_random_bytes_never_escape_the_decoder)
{
    // The seeded fuzz P0-A's review asked for: any bytes, any layout, the decoder
    // answers ok or Overread and never throws or reads past the buffer.
    uint32 x = 0x2026u;
    const Wire::Registry r = Wire::AllSequences();
    for (Wire::Entry const* e = r.begin; e != r.end; ++e)
    {
        for (int round = 0; round < 64; ++round)
        {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;      // xorshift32
            const size_t size = x % 97;
            WorldPacket p(e->opcode, size);
            for (size_t i = 0; i < size; ++i)
            {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                p << uint8(x & 0xFF);
            }
            Wire::MovementStatus out;
            const Wire::DecodeResult result = Wire::Decode(p, e->sequence, out);
            CHECK(result.error == Wire::DecodeError::None || result.error == Wire::DecodeError::Overread);
            CHECK(result.consumed <= size);
        }
    }
}

TEST(MovementSequences_index_answers_for_every_entry_and_nothing_else)
{
    const Wire::Registry r = Wire::AllSequences();
    CHECK_EQ(int(Wire::RegistrySize()), int(r.end - r.begin));
    for (Wire::Entry const* e = r.begin; e != r.end; ++e)
    {
        CHECK_EQ(Wire::RegistryIndex(e->opcode), int(e - r.begin));
        CHECK(Wire::IsRegistered(e->opcode));
    }
    CHECK_EQ(Wire::RegistryIndex(0x0000), -1);
    CHECK_EQ(Wire::RegistryIndex(SMSG_MOVE_KNOCK_BACK), -1);
    CHECK(!Wire::IsRegistered(CMSG_PING));
}

TEST(MovementSequences_the_three_cast_opcodes_are_embedded_layouts)
{
    // Their table is the movement block inside a cast packet, not the packet;
    // everything that judges a whole packet must leave them alone.
    CHECK(Wire::IsEmbeddedLayout(CMSG_CAST_SPELL));
    CHECK(Wire::IsEmbeddedLayout(CMSG_PET_CAST_SPELL));
    CHECK(Wire::IsEmbeddedLayout(CMSG_USE_ITEM));
    CHECK(Wire::IsRegistered(CMSG_USE_ITEM));
    CHECK(!Wire::IsPacketLayout(CMSG_USE_ITEM));
    CHECK(!Wire::IsEmbeddedLayout(CMSG_MOVE_START_FORWARD));
    CHECK(Wire::IsPacketLayout(CMSG_MOVE_START_FORWARD));
    CHECK(!Wire::IsPacketLayout(CMSG_PING));
    // Every row: embedded exactly when its table is the embedded one, so a
    // fourth opcode the generator maps to that table cannot be judged from
    // byte 0 just because a hand-kept list did not know about it.
    int embedded = 0;
    const Wire::Registry r = Wire::AllSequences();
    for (Wire::Entry const* e = r.begin; e != r.end; ++e)
    {
        const bool byTable = std::strcmp(e->table, "CastSpellEmbeddedMovement") == 0;
        CHECK_EQ(Wire::IsEmbeddedLayout(e->opcode), byTable);
        embedded += byTable ? 1 : 0;
    }
    CHECK_EQ(embedded, 3);
}

// DecodeWhole: copy a packet, decode it with one layout, and say whether the
// layout consumed every byte. The one thing it has to get right is ByteBuffer's
// single bit cursor, which serves both the read and the write side.
namespace
{
    // A layout ending on a bit, so a packet a writer has built and not flushed
    // still holds its last bit in the cursor byte -- the state
    // WorldSession::SendPacket receives, and the one the relay hook is handed.
    const Wire::Element kTrailingBitSequence[] =
    {
        Wire::Element::PositionX,
        Wire::Element::OneBit,
        Wire::Element::End
    };
}

TEST(MovementCodec_DecodeWhole_leaves_a_read_packet_whole)
{
    // The legacy reader leaves ByteBuffer's one bit cursor in read state; a
    // whole-packet decode of a copy must not flush that state into a byte.
    WorldPacket packet(CMSG_MOVE_START_FORWARD, 64);
    Wire::Encode(packet, Wire::SequenceFor(CMSG_MOVE_START_FORWARD), FullFixture());
    const size_t bytes = packet.size();
    packet.rpos(0);
    packet.ResetBitReader();
    (void)packet.ReadBit();                        // as the legacy reader leaves it
    Wire::MovementStatus out;
    Wire::DecodeResult result;
    CHECK(Wire::DecodeWhole(packet, Wire::SequenceFor(CMSG_MOVE_START_FORWARD), out, result, false));
    CHECK_EQ(result.consumed, bytes);
    CHECK_EQ(packet.size(), bytes);                // the original is untouched
    CHECK_EQ(out.pos.x, FullFixture().pos.x);
}

TEST(MovementCodec_DecodeWhole_flushes_a_writers_pending_bits)
{
    // A packet a writer built and has not flushed: its last bits are still in
    // the cursor byte, and the copy must carry them before it is judged.
    // Wire::Encode flushes at the end of every layout, so the state the relay
    // hook actually sees is built here by hand instead.
    WorldPacket packet;
    packet << float(1.5f);
    packet.WriteBit(true);                         // pending: the cursor byte is not appended yet
    CHECK_EQ(packet.size(), size_t(4));
    Wire::MovementStatus out;
    Wire::DecodeResult result;
    CHECK(Wire::DecodeWhole(packet, kTrailingBitSequence, out, result, true));
    CHECK_EQ(result.consumed, packet.size() + 1);  // the flushed byte
    CHECK_EQ(packet.size(), size_t(4));            // the original is untouched
    CHECK_EQ(out.pos.x, 1.5f);
    // And without being told: the copy is short by that byte and cannot pass.
    CHECK(!Wire::DecodeWhole(packet, kTrailingBitSequence, out, result, false));
    CHECK(result.error == Wire::DecodeError::Overread);
}

TEST(MovementCodec_DecodeWhole_names_the_bytes_a_layout_left_behind)
{
    // A decode that succeeds but stops short of the payload's end is not a
    // whole decode; the result says so by name, not by a bare false.
    WorldPacket packet(CMSG_MOVE_START_FORWARD, 64);
    Wire::Encode(packet, Wire::SequenceFor(CMSG_MOVE_START_FORWARD), FullFixture());
    const size_t bytes = packet.size();
    packet << uint8(0xEE);                         // one byte the layout never reads
    Wire::MovementStatus out;
    Wire::DecodeResult result;
    CHECK(!Wire::DecodeWhole(packet, Wire::SequenceFor(CMSG_MOVE_START_FORWARD), out, result, false));
    CHECK(result.error == Wire::DecodeError::LeftBytes);
    CHECK_EQ(result.consumed, bytes);
    CHECK_EQ(out.pos.x, Wire::MovementStatus().pos.x);   // whole, or nothing
    CHECK_STR(Wire::ErrorName(result.error), "left bytes");
    CHECK_STR(Wire::ErrorName(Wire::DecodeError::Overread), "overread");
}
