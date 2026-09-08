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

#include "TestHarness.h"

#include "WorldPacket.h"
#include "Opcodes.h"

#include "wire/GuidCodec.h"
#include "wire/KnockBackCodec.h"
#include "wire/MonsterMoveCodec.h"
#include "wire/MovementFamilies.h"
#include "wire/MovementSequences.h"
#include "wire/MoverCodec.h"
#include "wire/TeleportCodec.h"

#include <cstring>
#include <string>

namespace
{
    // A player guid whose low byte is the only non-zero one: 0x46. Its masked
    // form sets exactly one presence bit and writes exactly one byte, 0x46 ^ 1 =
    // 0x47, so every order below produces a distinct, hand-checkable packet.
    const uint64 kGuid = 0x46;

    std::string Hex(ByteBuffer const& b)
    {
        static const char* digits = "0123456789ABCDEF";
        std::string s;
        for (size_t i = 0; i < b.size(); ++i)
        {
            s += digits[b.contents()[i] >> 4];
            s += digits[b.contents()[i] & 0xF];
        }
        return s;
    }

    WorldPacket FromHex(uint16 opcode, char const* hex)
    {
        WorldPacket p(opcode, std::strlen(hex) / 2);
        for (char const* c = hex; *c; c += 2)
        {
            const int hi = (c[0] <= '9') ? c[0] - '0' : c[0] - 'A' + 10;
            const int lo = (c[1] <= '9') ? c[1] - '0' : c[1] - 'A' + 10;
            p << uint8((hi << 4) | lo);
        }
        return p;
    }
}

TEST(GuidCodec_masked_guid_writes_presence_bits_in_one_order_and_bytes_in_another)
{
    // WriteBit fills a byte from its top bit down, so an order that lists byte
    // 0 fifth puts the one set bit at 0x08.
    const uint8 mask[8]  = { 5, 7, 3, 6, 0, 4, 1, 2 };
    const uint8 bytes[8] = { 6, 2, 3, 0, 5, 7, 1, 4 };
    WorldPacket p;
    Wire::WriteGuidMask(p, kGuid, mask);
    Wire::WriteGuidBytes(p, kGuid, bytes);
    CHECK_STR(Hex(p), "0847");

    p.rpos(0);
    p.ResetBitReader();
    Wire::MaskedGuid g;
    Wire::DecodeResult r = Wire::Detail::Run(p, g, [&](Wire::Detail::Reader& in, Wire::MaskedGuid& out)
    {
        Wire::ReadGuidMask(in, out, mask);
        Wire::ReadGuidBytes(in, out, bytes);
    });
    CHECK(r.ok());
    CHECK_EQ(r.consumed, size_t(2));
    CHECK_EQ(g.Value(), kGuid);
}

TEST(GuidCodec_all_zero_guid_writes_a_clear_mask_and_no_bytes)
{
    // Every byte absent: WriteGuidMask writes eight clear bits (one byte, all
    // zero) and WriteGuidBytes writes nothing at all, for any order.
    const uint8 order[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    WorldPacket p;
    Wire::WriteGuidMask(p, 0, order);
    Wire::WriteGuidBytes(p, 0, order);
    CHECK_STR(Hex(p), "00");

    p.rpos(0);
    p.ResetBitReader();
    Wire::MaskedGuid g;
    Wire::DecodeResult r = Wire::Detail::Run(p, g, [&](Wire::Detail::Reader& in, Wire::MaskedGuid& out)
    {
        Wire::ReadGuidMask(in, out, order);
        Wire::ReadGuidBytes(in, out, order);
    });
    CHECK(r.ok());
    CHECK_EQ(r.consumed, size_t(1));
    CHECK_EQ(g.Value(), uint64(0));

    WorldPacket packed;
    Wire::WritePackedGuid(packed, 0);
    CHECK_STR(Hex(packed), "00");
    packed.rpos(0);
    uint64 v = 0;
    Wire::DecodeResult r2 = Wire::Detail::Run(packed, v, [](Wire::Detail::Reader& in, uint64& out) { out = Wire::ReadPackedGuid(in); });
    CHECK(r2.ok());
    CHECK_EQ(v, uint64(0));
}

TEST(GuidCodec_packed_guid_is_a_mask_byte_then_the_bytes_low_to_high)
{
    WorldPacket p;
    Wire::WritePackedGuid(p, 0x0000000000010046ULL);
    // Byte 0 is 0x46, byte 1 is zero (skipped), byte 2 is 0x01: the mask (bit 0
    // and bit 2) is 0x05, then the present bytes low to high.
    CHECK_STR(Hex(p), "054601");
    p.rpos(0);
    uint64 v = 0;
    Wire::DecodeResult r = Wire::Detail::Run(p, v, [](Wire::Detail::Reader& in, uint64& out) { out = Wire::ReadPackedGuid(in); });
    CHECK(r.ok());
    CHECK_EQ(v, 0x0000000000010046ULL);
    // A mask that promises a byte the buffer lacks is an overread, not a log line.
    WorldPacket shortOne = FromHex(SMSG_CLIENT_CONTROL_UPDATE, "03");
    shortOne.rpos(0);
    r = Wire::Detail::Run(shortOne, v, [](Wire::Detail::Reader& in, uint64& out) { out = Wire::ReadPackedGuid(in); });
    CHECK(r.error == Wire::DecodeError::Overread);
}

TEST(MoverCodec_active_mover_has_one_order_per_opcode)
{
    // SMSG (CPP's MoveSetActiveMover::Write): mask 5,7,3,6,0,4,1,2 / bytes 6,2,3,0,5,7,1,4.
    // CMSG (the tree's HandleSetActiveMoverOpcode, CPP's SetActiveMover::Read):
    // mask 7,2,1,0,4,5,6,3 / bytes 3,2,4,0,5,1,6,7.
    Wire::ActiveMover v;
    v.guid = kGuid;
    WorldPacket s(SMSG_MOVE_SET_ACTIVE_MOVER, 16);
    Wire::EncodeActiveMover(s, SMSG_MOVE_SET_ACTIVE_MOVER, v);
    CHECK_STR(Hex(s), "0847");
    WorldPacket c(CMSG_SET_ACTIVE_MOVER, 16);
    Wire::EncodeActiveMover(c, CMSG_SET_ACTIVE_MOVER, v);
    CHECK_STR(Hex(c), "1047");

    Wire::ActiveMover back;
    s.rpos(0); s.ResetBitReader();
    CHECK(Wire::DecodeActiveMover(s, SMSG_MOVE_SET_ACTIVE_MOVER, back).ok());
    CHECK_EQ(back.guid, kGuid);
    c.rpos(0); c.ResetBitReader();
    CHECK(Wire::DecodeActiveMover(c, CMSG_SET_ACTIVE_MOVER, back).ok());
    CHECK_EQ(back.guid, kGuid);
    // The wrong order does not read the guid back.
    c.rpos(0); c.ResetBitReader();
    CHECK(Wire::DecodeActiveMover(c, SMSG_MOVE_SET_ACTIVE_MOVER, back).ok());
    CHECK(back.guid != kGuid);
}

TEST(MoverCodec_control_update_is_a_packed_guid_and_a_byte)
{
    Wire::ControlUpdate v;
    v.guid = kGuid;
    v.allowMove = 1;
    WorldPacket p(SMSG_CLIENT_CONTROL_UPDATE, 16);
    Wire::EncodeControlUpdate(p, v);
    CHECK_STR(Hex(p), "014601");
    Wire::ControlUpdate back;
    p.rpos(0);
    Wire::DecodeResult r = Wire::DecodeControlUpdate(p, back);
    CHECK(r.ok());
    CHECK_EQ(r.consumed, size_t(3));
    CHECK_EQ(back.guid, kGuid);
    CHECK_EQ(int(back.allowMove), 1);
}

TEST(MovementFamilies_every_family_opcode_is_known_and_no_registry_opcode_is_a_family)
{
    CHECK(Wire::FamilyFor(SMSG_MOVE_KNOCK_BACK) == Wire::Family::KnockBack);
    CHECK(Wire::FamilyFor(SMSG_MOVE_TELEPORT) == Wire::Family::Teleport);
    CHECK(Wire::FamilyFor(CMSG_MOVE_TELEPORT_ACK) == Wire::Family::TeleportAck);
    CHECK(Wire::FamilyFor(SMSG_MOVE_SET_ACTIVE_MOVER) == Wire::Family::ActiveMover);
    CHECK(Wire::FamilyFor(CMSG_SET_ACTIVE_MOVER) == Wire::Family::ActiveMover);
    CHECK(Wire::FamilyFor(SMSG_CLIENT_CONTROL_UPDATE) == Wire::Family::ControlUpdate);
    CHECK(Wire::FamilyFor(SMSG_MONSTER_MOVE) == Wire::Family::MonsterMove);
    CHECK(Wire::FamilyFor(SMSG_MONSTER_MOVE_TRANSPORT) == Wire::Family::MonsterMove);
    CHECK(Wire::FamilyFor(CMSG_MOVE_START_FORWARD) == Wire::Family::None);
    CHECK_EQ(Wire::FamilyCount(), size_t(8));
    CHECK(Wire::IsKnown(SMSG_MOVE_KNOCK_BACK));
    CHECK(Wire::IsKnown(CMSG_MOVE_START_FORWARD));
    CHECK(!Wire::IsKnown(CMSG_PING));
    CHECK(!Wire::IsKnown(CMSG_USE_ITEM));               // embedded: not a packet layout, not a family
    for (size_t i = 0; i < Wire::FamilyCount(); ++i)
    {
        const uint16 op = Wire::FamilyOpcodeAt(i);
        CHECK_EQ(Wire::FamilyIndex(op), int(i));
        CHECK(!Wire::IsPacketLayout(op));               // the two kinds never overlap
        CHECK(Wire::SequenceFor(op) == nullptr);
    }
    // Every enumerator has a name of its own: FamilyName's switch is what a
    // report prints, so a value added without a case must not fall through to
    // "?" unnoticed, and no two may share a name.
    const Wire::Family all[7] =
    {
        Wire::Family::None, Wire::Family::KnockBack, Wire::Family::Teleport,
        Wire::Family::TeleportAck, Wire::Family::ActiveMover,
        Wire::Family::ControlUpdate, Wire::Family::MonsterMove
    };
    const char* names[7] =
    {
        "none", "knock back", "teleport", "teleport ack",
        "active mover", "control update", "monster move"
    };
    for (int i = 0; i < 7; ++i)
    {
        CHECK_STR(Wire::FamilyName(all[i]), names[i]);
        for (int j = 0; j < i; ++j) { CHECK(std::string(names[i]) != names[j]); }
    }
    // An index past the end is no opcode, not whatever follows the table.
    CHECK_EQ(Wire::FamilyOpcodeAt(Wire::FamilyCount()), uint16(0));
    CHECK_EQ(Wire::FamilyIndex(CMSG_PING), -1);
}

TEST(MovementFamilies_judge_covers_both_kinds_the_same_way)
{
    // A registry packet: judged by DecodeWhole + Encode; a family packet: by its
    // own codec. Both answer decoded/exact/consumed the same way.
    WorldPacket status = FromHex(CMSG_MOVE_START_FORWARD, "CDAC59C6E13AC9423D9A77C52011800000012F5C5A054067920100");
    Wire::Verdict v = Wire::Judge(CMSG_MOVE_START_FORWARD, status, false);
    CHECK(v.decoded);
    CHECK(v.exact);
    CHECK_EQ(v.result.consumed, size_t(27));
    CHECK_EQ(v.firstDifference, -1L);                   // exact: nothing to point at

    WorldPacket mover = FromHex(SMSG_MOVE_SET_ACTIVE_MOVER, "0847");
    v = Wire::Judge(SMSG_MOVE_SET_ACTIVE_MOVER, mover, false);
    CHECK(v.decoded);
    CHECK(v.exact);
    CHECK_EQ(v.result.consumed, size_t(2));

    WorldPacket left = FromHex(SMSG_MOVE_SET_ACTIVE_MOVER, "0847EE");
    v = Wire::Judge(SMSG_MOVE_SET_ACTIVE_MOVER, left, false);
    CHECK(!v.decoded);
    CHECK(v.result.error == Wire::DecodeError::LeftBytes);

    WorldPacket unknown = FromHex(CMSG_PING, "0000");
    v = Wire::Judge(CMSG_PING, unknown, false);
    CHECK(!v.decoded);
    CHECK(v.result.error == Wire::DecodeError::NoSequence);
}

TEST(MovementFamilies_judge_says_where_an_inexact_re_encoding_first_differs)
{
    // The heartbeat golden with one byte of its flag block altered -- 0x91 to
    // 0xC7 at offset 12. The codec still decodes it whole, and re-encodes it to
    // 27 bytes: the SAME length as the packet, so the length alone says nothing
    // at all about what moved. firstDifference is the only thing that points at
    // it, and it points at byte 19 -- not at 12, because what the altered flags
    // changed is which fields the block below them carries.
    //
    // This is the one such case in the two registry goldens: a sweep of all 256
    // values at all 27 offsets of both the start-forward and the heartbeat
    // fixture found 33 decoded-but-inexact packets, and every other one differs
    // in length as well. No single-BIT flip of the start-forward fixture is
    // decoded-but-inexact at all -- the codec round-trips them or rejects them.
    WorldPacket beat = FromHex(MSG_MOVE_HEARTBEAT, "E13AC94232B677C564A059C69104000000012F5C5A054064940100");
    Wire::Verdict v = Wire::Judge(MSG_MOVE_HEARTBEAT, beat, false);
    CHECK(v.decoded);
    CHECK(v.exact);
    CHECK_EQ(v.firstDifference, -1L);

    const_cast<uint8*>(beat.contents())[12] = 0xC7;
    v = Wire::Judge(MSG_MOVE_HEARTBEAT, beat, false);
    CHECK(v.decoded);
    CHECK(!v.exact);
    CHECK_EQ(v.reencoded, size_t(27));                  // the same length as the packet
    CHECK_EQ(beat.size(), size_t(27));
    CHECK_EQ(v.firstDifference, 19L);
}

TEST(KnockBackCodec_matches_the_tree_writer_and_cpp)
{
    // WorldSession::SendKnockBack: mask 0,3,6,7,2,5,1,4; byte 1; float sin;
    // uint32 counter; bytes 6,7; float horizontal; bytes 4,5,3; float vertical;
    // float cos; bytes 2,0. For kGuid the mask is 0x80 (byte 0 listed first)
    // and the one guid byte lands last.
    Wire::KnockBack v;
    v.guid = kGuid; v.counter = 7; v.directionY = 0.0f; v.horizontal = 10.0f; v.vertical = -12.0f; v.directionX = 1.0f;
    WorldPacket p(SMSG_MOVE_KNOCK_BACK, 32);
    Wire::EncodeKnockBack(p, v);
    CHECK_STR(Hex(p), "80" "00000000" "07000000" "00002041" "000040C1" "0000803F" "47");
    Wire::KnockBack back;
    p.rpos(0); p.ResetBitReader();
    Wire::DecodeResult r = Wire::DecodeKnockBack(p, back);
    CHECK(r.ok());
    CHECK_EQ(r.consumed, size_t(22));
    CHECK_EQ(back.guid, kGuid);
    CHECK_EQ(back.counter, uint32(7));
    CHECK_EQ(back.horizontal, 10.0f);
    CHECK_EQ(back.vertical, -12.0f);
    CHECK_EQ(back.directionX, 1.0f);
    Wire::Verdict judged = Wire::Judge(SMSG_MOVE_KNOCK_BACK, p, false);
    CHECK(judged.decoded);
    CHECK(judged.exact);
}

TEST(TeleportCodec_without_transport_or_vehicle_matches_the_tree_writer)
{
    // Player::SendTeleportPacket: mask 6,0,3,2; bit hasVehicle (the tree writes
    // 0); bit hasTransport; mask 1; [transport mask]; mask 4,7,5; flush;
    // [transport bytes]; uint32 counter; bytes 1,2,3,5; x; byte 4; o; byte 7;
    // z; [vehicle seat]; bytes 0,6; y. Ten bits: 0x40 0x00.
    Wire::Teleport v;
    v.guid = kGuid; v.counter = 1;
    v.pos.x = 1.5f; v.pos.y = -1.5f; v.pos.z = 2.5f; v.pos.o = 0.5f;
    WorldPacket p(SMSG_MOVE_TELEPORT, 64);
    Wire::EncodeTeleport(p, v);
    CHECK_STR(Hex(p), "40" "00" "01000000" "0000C03F" "0000003F" "00002040" "47" "0000C0BF");
    Wire::Teleport back;
    p.rpos(0); p.ResetBitReader();
    Wire::DecodeResult r = Wire::DecodeTeleport(p, back);
    CHECK(r.ok());
    CHECK_EQ(r.consumed, size_t(23));
    CHECK_EQ(back.guid, kGuid);
    CHECK_EQ(back.pos.x, 1.5f);
    CHECK_EQ(back.pos.y, -1.5f);
    CHECK_EQ(back.pos.z, 2.5f);
    CHECK_EQ(back.pos.o, 0.5f);
    CHECK(!back.hasTransport);
    CHECK(!back.hasVehicle);
    CHECK(Wire::Judge(SMSG_MOVE_TELEPORT, p, false).exact);
}

TEST(TeleportCodec_transport_and_vehicle_branches_round_trip)
{
    // The branches CPP's MoveTeleport::Write carries and the tree's writer does
    // not (yet): the transport guid's own mask and bytes, the two vehicle bits
    // after hasVehicle, the one-byte seat after z (P1-C task 5's cross-check
    // against the client's reader: one byte, not four).
    Wire::Teleport v;
    v.guid = 0x0000000000000102ULL; v.counter = 9;
    v.pos.x = 1.0f; v.pos.y = 2.0f; v.pos.z = 3.0f; v.pos.o = 4.0f;
    v.hasTransport = true; v.transportGuid = 0x1F00000000000A01ULL;
    v.hasVehicle = true; v.vehicleExitVoluntary = true; v.vehicleExitTeleport = false; v.vehicleSeat = 3;
    WorldPacket p(SMSG_MOVE_TELEPORT, 64);
    Wire::EncodeTeleport(p, v);
    Wire::Teleport back;
    p.rpos(0); p.ResetBitReader();
    Wire::DecodeResult r = Wire::DecodeTeleport(p, back);
    CHECK(r.ok());
    CHECK_EQ(r.consumed, p.size());
    CHECK_EQ(back.guid, v.guid);
    CHECK_EQ(back.transportGuid, v.transportGuid);
    CHECK(back.hasVehicle);
    CHECK(back.vehicleExitVoluntary);
    CHECK(!back.vehicleExitTeleport);
    CHECK_EQ(int(back.vehicleSeat), 3);
    // The seat is one byte on the wire. Dropping the vehicle branch drops the
    // seat and its two bits -- and 18 bits pad to the same three bytes as 20 --
    // so the whole difference must be that single byte.
    Wire::Teleport noVehicle = v;
    noVehicle.hasVehicle = false;
    noVehicle.vehicleExitVoluntary = false;
    WorldPacket q(SMSG_MOVE_TELEPORT, 64);
    Wire::EncodeTeleport(q, noVehicle);
    CHECK_EQ(p.size(), q.size() + 1);
    CHECK_EQ(back.pos.y, 2.0f);
    CHECK(Wire::Judge(SMSG_MOVE_TELEPORT, p, false).exact);
}

TEST(TeleportCodec_ack_is_counter_time_then_the_masked_guid)
{
    // HandleMoveTeleportAckOpcode and CPP's MoveTeleportAck::Read: uint32
    // counter, uint32 time, mask 5,0,1,6,3,7,2,4, bytes 4,2,7,6,5,1,3,0.
    Wire::TeleportAck v;
    v.counter = 1; v.time = 1000; v.guid = kGuid;
    WorldPacket p(CMSG_MOVE_TELEPORT_ACK, 16);
    Wire::EncodeTeleportAck(p, v);
    CHECK_STR(Hex(p), "01000000" "E8030000" "40" "47");
    Wire::TeleportAck back;
    p.rpos(0); p.ResetBitReader();
    Wire::DecodeResult r = Wire::DecodeTeleportAck(p, back);
    CHECK(r.ok());
    CHECK_EQ(r.consumed, size_t(10));
    CHECK_EQ(back.counter, uint32(1));
    CHECK_EQ(back.time, uint32(1000));
    CHECK_EQ(back.guid, kGuid);
    CHECK(Wire::Judge(CMSG_MOVE_TELEPORT_ACK, p, false).exact);
    // Truncated after the time: an overread, whole-or-nothing.
    WorldPacket cut = FromHex(CMSG_MOVE_TELEPORT_ACK, "01000000E8030000");
    cut.rpos(0);
    r = Wire::DecodeTeleportAck(cut, back);
    CHECK(r.error == Wire::DecodeError::Overread);
    // Whole-or-nothing means the whole struct, not just the first field: `back`
    // held a good ack a moment ago, and a failed decode must leave none of it
    // behind for a caller to mistake for this packet's.
    CHECK_EQ(back.counter, uint32(0));
    CHECK_EQ(back.time, uint32(0));
    CHECK_EQ(back.guid, uint64(0));
}

TEST(MonsterMoveCodec_linear_path_matches_the_packet_builder)
{
    // PacketBuilder::WriteMonsterMove for the common case: packed mover guid,
    // the zero byte, start, id, type Normal, flags, duration, then a linear
    // path: last index, destination, and one packed offset per middle point.
    Wire::MonsterMove v;
    v.mover = kGuid;
    v.start.x = 1.0f; v.start.y = 2.0f; v.start.z = 3.0f;
    v.id = 5;
    v.flags = 0x00000000;
    v.duration = 1500;
    v.path = Wire::SplinePath::Linear;
    v.destination.x = 4.0f; v.destination.y = 5.0f; v.destination.z = 6.0f;
    v.packedOffsets.push_back(0x00400801u);   // one middle point, kept packed
    WorldPacket p(SMSG_MONSTER_MOVE, 64);
    Wire::EncodeMonsterMove(p, SMSG_MONSTER_MOVE, v);
    CHECK_STR(Hex(p), "0146" "00" "0000803F" "00000040" "00004040" "05000000" "00" "00000000" "DC050000"
                      "02000000" "00008040" "0000A040" "0000C040" "01084000");
    Wire::MonsterMove back;
    p.rpos(0);
    Wire::DecodeResult r = Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE, back);
    CHECK(r.ok());
    CHECK_EQ(r.consumed, p.size());
    CHECK_EQ(back.mover, kGuid);
    CHECK_EQ(back.id, uint32(5));
    CHECK_EQ(back.duration, uint32(1500));
    CHECK(back.path == Wire::SplinePath::Linear);
    CHECK_EQ(back.destination.z, 6.0f);
    CHECK_EQ(back.packedOffsets.size(), size_t(1));
    CHECK_EQ(back.packedOffsets[0], 0x00400801u);
    CHECK(Wire::Judge(SMSG_MONSTER_MOVE, p, false).exact);
}

TEST(MonsterMoveCodec_stop_form_ends_at_the_type)
{
    // MoveSplineInit::Stop: guid, zero byte, position, id, MonsterMoveStop, nothing more.
    Wire::MonsterMove v;
    v.mover = kGuid;
    v.start.x = 1.0f; v.start.y = 2.0f; v.start.z = 3.0f;
    v.id = 6;
    v.type = Wire::MonsterMoveType::Stop;
    WorldPacket p(SMSG_MONSTER_MOVE, 32);
    Wire::EncodeMonsterMove(p, SMSG_MONSTER_MOVE, v);
    CHECK_STR(Hex(p), "0146" "00" "0000803F" "00000040" "00004040" "06000000" "01");
    Wire::MonsterMove back;
    p.rpos(0);
    CHECK(Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE, back).ok());
    CHECK(back.type == Wire::MonsterMoveType::Stop);
    CHECK(Wire::Judge(SMSG_MONSTER_MOVE, p, false).exact);
}

TEST(MonsterMoveCodec_transport_form_facing_animation_parabolic_and_uncompressed_path_round_trip)
{
    Wire::MonsterMove v;
    v.mover = 0x0000000000000102ULL;
    v.onTransport = true; v.transport = 0x1F00000000000A01ULL; v.seat = 2;
    v.exitVoluntary = 0;
    v.start.x = -1.0f; v.start.y = -2.0f; v.start.z = -3.0f;
    v.id = 77;
    v.type = Wire::MonsterMoveType::FacingTarget; v.facingTarget = 0xF130000000001234ULL;
    v.flags = Wire::kSplineFlagAnimation | Wire::kSplineFlagTrajectory | Wire::kSplineFlagUncompressedPath | 0x00000200;
    v.animationId = 2; v.animationStart = 300;
    v.duration = 4000;
    v.verticalAcceleration = 9.5f; v.parabolicStart = 100;
    v.path = Wire::SplinePath::Uncompressed;
    Wire::Vec3 a; a.x = 1; a.y = 1; a.z = 1;
    Wire::Vec3 b; b.x = 2; b.y = 2; b.z = 2;
    v.points.push_back(a); v.points.push_back(b);
    WorldPacket p(SMSG_MONSTER_MOVE_TRANSPORT, 128);
    Wire::EncodeMonsterMove(p, SMSG_MONSTER_MOVE_TRANSPORT, v);
    Wire::MonsterMove back;
    p.rpos(0);
    Wire::DecodeResult r = Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE_TRANSPORT, back);
    CHECK(r.ok());
    CHECK_EQ(r.consumed, p.size());
    CHECK(back.onTransport);
    CHECK_EQ(back.transport, v.transport);
    CHECK_EQ(int(back.seat), 2);
    CHECK(back.type == Wire::MonsterMoveType::FacingTarget);
    CHECK_EQ(back.facingTarget, v.facingTarget);
    CHECK_EQ(int(back.animationId), 2);
    CHECK_EQ(back.animationStart, int32(300));
    CHECK_EQ(back.verticalAcceleration, 9.5f);
    CHECK_EQ(back.parabolicStart, int32(100));
    CHECK(back.path == Wire::SplinePath::Uncompressed);
    CHECK_EQ(back.points.size(), size_t(2));
    CHECK_EQ(back.points[1].z, 2.0f);
    CHECK(Wire::Judge(SMSG_MONSTER_MOVE_TRANSPORT, p, false).exact);
    // The other two facings round-trip too.
    v.onTransport = false; v.type = Wire::MonsterMoveType::FacingAngle; v.facingAngle = 1.25f;
    WorldPacket q(SMSG_MONSTER_MOVE, 128);
    Wire::EncodeMonsterMove(q, SMSG_MONSTER_MOVE, v);
    q.rpos(0);
    CHECK(Wire::DecodeMonsterMove(q, SMSG_MONSTER_MOVE, back).ok());
    CHECK_EQ(back.facingAngle, 1.25f);
    v.type = Wire::MonsterMoveType::FacingSpot; v.facingSpot = a;
    WorldPacket s(SMSG_MONSTER_MOVE, 128);
    Wire::EncodeMonsterMove(s, SMSG_MONSTER_MOVE, v);
    s.rpos(0);
    CHECK(Wire::DecodeMonsterMove(s, SMSG_MONSTER_MOVE, back).ok());
    CHECK_EQ(back.facingSpot.y, 1.0f);
}

TEST(MonsterMoveCodec_refuses_a_path_count_the_buffer_cannot_hold)
{
    // A count of 4 billion points must not reserve 4 billion points: the
    // decoder bounds the count by the bytes that remain before it reserves.
    WorldPacket p = FromHex(SMSG_MONSTER_MOVE, "0146" "00" "0000803F" "00000040" "00004040" "05000000" "00" "00004000" "DC050000" "FFFFFFFF");
    Wire::MonsterMove back;
    p.rpos(0);
    Wire::DecodeResult r = Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE, back);
    CHECK(r.error == Wire::DecodeError::Overread);
    CHECK(back.points.empty());
}

TEST(MonsterMoveCodec_refuses_a_move_type_it_does_not_know)
{
    // The type byte decides what follows it, so a value outside the five the
    // writer emits leaves the reader with no idea what the rest of the packet
    // is. Refuse the whole packet rather than guess, and leave `out` as it was
    // built -- nothing of a packet that did not decode may reach a caller.
    WorldPacket p = FromHex(SMSG_MONSTER_MOVE, "0146" "00" "0000803F" "00000040" "00004040" "05000000" "05");
    Wire::MonsterMove back;
    back.mover = 0x1234u;                                // a value the failure must clear
    p.rpos(0);
    Wire::DecodeResult r = Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE, back);
    CHECK(r.error == Wire::DecodeError::Overread);
    CHECK_EQ(back.mover, uint64(0));
    CHECK_EQ(back.id, uint32(0));
    CHECK(back.type == Wire::MonsterMoveType::Normal);
    CHECK(back.packedOffsets.empty());
}

TEST(MonsterMoveCodec_refuses_a_linear_path_of_no_points)
{
    // WriteLinearPath writes the LAST INDEX, so the count is one more than the
    // middle points and is never zero: a zero would mean "read the destination
    // and then step back one", which is not a path at all.
    WorldPacket p = FromHex(SMSG_MONSTER_MOVE, "0146" "00" "0000803F" "00000040" "00004040" "05000000" "00" "00000000" "DC050000"
                            "00000000" "00008040" "0000A040" "0000C040");
    Wire::MonsterMove back;
    p.rpos(0);
    Wire::DecodeResult r = Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE, back);
    CHECK(r.error == Wire::DecodeError::Overread);
    CHECK(back.packedOffsets.empty());
}

TEST(MonsterMoveCodec_refuses_a_linear_count_the_bytes_after_the_destination_cannot_hold)
{
    // Count 4 promises the destination and three packed offsets, and the packet
    // holds the destination alone. The bound has to subtract the twelve bytes of
    // that destination before it divides, or a count of four looks affordable in
    // exactly the twelve bytes the destination is about to spend.
    WorldPacket p = FromHex(SMSG_MONSTER_MOVE, "0146" "00" "0000803F" "00000040" "00004040" "05000000" "00" "00000000" "DC050000"
                            "04000000" "00008040" "0000A040" "0000C040");
    Wire::MonsterMove back;
    p.rpos(0);
    Wire::DecodeResult r = Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE, back);
    CHECK(r.error == Wire::DecodeError::Overread);
    CHECK(back.packedOffsets.empty());
    CHECK_EQ(back.destination.x, 0.0f);                  // whole-or-nothing: not even the part it read
}

