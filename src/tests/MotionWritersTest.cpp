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

#include "Opcodes.h"
#include "WorldPacket.h"
#include "Change.h"
#include "PacketMatrix.h"
#include "Writers.h"
#include "MonsterMoveStop.h"
#include "wire/MonsterMoveCodec.h"
#include "wire/KnockBackCodec.h"
#include "wire/MovementCodec.h"
#include "wire/MovementFamilies.h"
#include "wire/MovementSequences.h"
#include "wire/TeleportCodec.h"
#include "Replay.hpp"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace Motion;

namespace
{
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

    struct GoldenPacket
    {
        uint16      opcode;
        std::string hex;      ///< the payload as the golden spells it (upper-case hex)
    };

    // Every S line of `name` whose opcode is `opcode`.
    void ServerLines(const char* name, uint16 opcode, std::vector<GoldenPacket>& out)
    {
        const std::string path = std::string(MANGOS_TEST_DATA_DIR) + "/movement/" + name;
        std::ifstream in(path.c_str());
        REQUIRE(bool(in));
        std::string text;
        while (std::getline(in, text))
        {
            loadtest::CaptureLine line;
            std::string error;
            if (!loadtest::ParseCaptureLine(text, line, error) || line.direction != 'S' || line.opcode != opcode) { continue; }
            GoldenPacket g;
            g.opcode = opcode;
            static const char* digits = "0123456789ABCDEF";
            for (size_t i = 0; i < line.bytes.size(); ++i)
            {
                g.hex += digits[line.bytes[i] >> 4];
                g.hex += digits[line.bytes[i] & 0xF];
            }
            out.push_back(g);
        }
    }

    // Decodes a registry packet whole; REQUIREs success.
    void DecodeRegistry(GoldenPacket const& g, Wire::MovementStatus& st)
    {
        WorldPacket p = FromHex(g.opcode, g.hex.c_str());
        Wire::DecodeResult res;
        REQUIRE(Wire::DecodeWhole(p, Wire::SequenceFor(g.opcode), st, res, false));
    }

    uint8 MoveTypeOf(uint16 opcode)
    {
        bool isSpline = false;
        MatrixRow const* row = RowForOpcode(opcode, &isSpline);
        return row ? uint8(SpeedIndex(row->type)) : 0xFF;
    }
}

TEST(MotionWriters_rebuild_every_speed_set_in_the_client_golden)
{
    const uint16 opcodes[] = { SMSG_MOVE_SET_RUN_SPEED, SMSG_MOVE_SET_SWIM_SPEED, SMSG_MOVE_SET_FLIGHT_SPEED };
    size_t rebuilt = 0;
    for (size_t i = 0; i < 3; ++i)
    {
        std::vector<GoldenPacket> lines;
        ServerLines("client-15595.log", opcodes[i], lines);
        for (size_t j = 0; j < lines.size(); ++j)
        {
            Wire::MovementStatus st;
            DecodeRegistry(lines[j], st);
            WorldPacket out;
            REQUIRE(BuildMover(out, st.guid, st.counter, SpeedChange(MoveTypeOf(opcodes[i]), st.value)));
            CHECK_EQ(out.GetOpcode(), opcodes[i]);
            CHECK_STR(Hex(out), lines[j].hex);
            ++rebuilt;
        }
    }
    CHECK(rebuilt >= 19);   // 9 run, 3 swim, 7 flight in the golden
}

TEST(MotionWriters_rebuild_the_flag_sets_in_the_client_golden)
{
    const uint16 opcodes[] = { SMSG_MOVE_SET_CAN_FLY, SMSG_MOVE_UNSET_CAN_FLY, SMSG_FORCE_MOVE_ROOT, SMSG_FORCE_MOVE_UNROOT };
    size_t rebuilt = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        std::vector<GoldenPacket> lines;
        ServerLines("client-15595.log", opcodes[i], lines);
        bool isSpline = true;
        MatrixRow const* row = RowForOpcode(opcodes[i], &isSpline);
        REQUIRE(row != NULL && !isSpline);
        for (size_t j = 0; j < lines.size(); ++j)
        {
            Wire::MovementStatus st;
            DecodeRegistry(lines[j], st);
            WorldPacket out;
            REQUIRE(BuildMover(out, st.guid, st.counter, FlagChange(row->type, row->apply)));
            CHECK_STR(Hex(out), lines[j].hex);
            ++rebuilt;
        }
    }
    CHECK(rebuilt >= 6);   // 2 + 2 + 1 + 1
}

TEST(MotionWriters_rebuild_the_spline_packets_in_the_goldens)
{
    struct Source { const char* golden; uint16 opcode; };
    const Source sources[] =
    {
        { "peer-walk-15595.log",   SMSG_SPLINE_MOVE_SET_RUN_SPEED },
        { "peer-walk-15595.log",   SMSG_SPLINE_MOVE_SET_SWIM_SPEED },
        { "peer-walk-15595.log",   SMSG_SPLINE_MOVE_SET_FLIGHT_SPEED },
        { "client-15595.log",      SMSG_SPLINE_MOVE_SET_RUN_MODE },
        { "client-15595.log",      SMSG_SPLINE_MOVE_SET_WALK_MODE },
        { "client-15595.log",      SMSG_SPLINE_MOVE_START_SWIM },
        { "client-15595.log",      SMSG_SPLINE_MOVE_STOP_SWIM },
    };
    size_t rebuilt = 0;
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i)
    {
        std::vector<GoldenPacket> lines;
        ServerLines(sources[i].golden, sources[i].opcode, lines);
        bool isSpline = false;
        MatrixRow const* row = RowForOpcode(sources[i].opcode, &isSpline);
        REQUIRE(row != NULL && isSpline);
        for (size_t j = 0; j < lines.size(); ++j)
        {
            Wire::MovementStatus st;
            DecodeRegistry(lines[j], st);
            Change change = IsSpeed(row->type) ? SpeedChange(uint8(SpeedIndex(row->type)), st.value) : FlagChange(row->type, row->apply);
            WorldPacket out;
            REQUIRE(BuildSpline(out, st.guid, change));
            CHECK_STR(Hex(out), lines[j].hex);
            ++rebuilt;
        }
    }
    CHECK(rebuilt >= 240);   // 3 speeds + 50 + 50 + 69 + 68 modes and swims
}

TEST(MotionWriters_rebuild_the_teleports_and_knockbacks_in_the_families_golden)
{
    std::vector<GoldenPacket> teleports;
    ServerLines("client-15595-families.log", SMSG_MOVE_TELEPORT, teleports);
    REQUIRE(teleports.size() == 3);
    for (size_t i = 0; i < teleports.size(); ++i)
    {
        WorldPacket p = FromHex(SMSG_MOVE_TELEPORT, teleports[i].hex.c_str());
        Wire::Teleport t;
        REQUIRE(Wire::DecodeTeleport(p, t).ok());
        TeleportParams params;
        params.pos = t.pos;
        params.hasTransport = t.hasTransport;
        params.transportGuid = t.transportGuid;
        WorldPacket out;
        REQUIRE(BuildMover(out, t.guid, t.counter, TeleportChange(params)));
        CHECK_STR(Hex(out), teleports[i].hex);
    }
    std::vector<GoldenPacket> knocks;
    ServerLines("client-15595-families.log", SMSG_MOVE_KNOCK_BACK, knocks);
    REQUIRE(knocks.size() == 3);
    for (size_t i = 0; i < knocks.size(); ++i)
    {
        WorldPacket p = FromHex(SMSG_MOVE_KNOCK_BACK, knocks[i].hex.c_str());
        Wire::KnockBack k;
        REQUIRE(Wire::DecodeKnockBack(p, k).ok());
        KnockBackParams params;
        params.directionX = k.directionX;
        params.directionY = k.directionY;
        params.horizontal = k.horizontal;
        params.vertical = k.vertical;
        WorldPacket out;
        REQUIRE(BuildMover(out, k.guid, k.counter, KnockBackChange(params)));
        CHECK_STR(Hex(out), knocks[i].hex);
    }
}

TEST(MotionWriters_every_cell_round_trips_through_the_judge_and_carries_its_counter)
{
    const uint64 guid = 0x46;
    const uint32 counter = 0x1234;
    Wire::MovementStatus status;
    status.guid = 0;   // overwritten by the writer
    status.flags = 0x1;
    status.time = 0x55667788;
    status.pos.x = 1.0f; status.pos.y = 2.0f; status.pos.z = 3.0f; status.pos.o = 0.5f;

    size_t movers = 0, splines = 0, observers = 0;
    for (size_t i = 0; i < MatrixSize(); ++i)
    {
        MatrixRow const& row = MatrixRowAt(i);
        Change change;
        if (IsSpeed(row.type))                       { change = SpeedChange(uint8(SpeedIndex(row.type)), 7.5f); }
        else if (row.type == ChangeType::CollisionHeight) { change = HeightChange(2.25f, 1); }
        else if (row.type == ChangeType::KnockBack)  { KnockBackParams k; k.directionX = 0.6f; k.directionY = 0.8f; k.horizontal = 10.0f; k.vertical = -20.0f; change = KnockBackChange(k); }
        else if (row.type == ChangeType::Teleport)   { TeleportParams t; t.pos.x = 10.0f; t.pos.y = 20.0f; t.pos.z = 30.0f; t.pos.o = 1.5f; change = TeleportChange(t); }
        else                                         { change = FlagChange(row.type, row.apply); }

        WorldPacket out;
        if (row.mover)
        {
            REQUIRE(BuildMover(out, guid, counter, change));
            CHECK_EQ(out.GetOpcode(), row.mover);
            Wire::Verdict v = Wire::Judge(row.mover, out, false);
            CHECK(v.decoded);
            CHECK(v.exact);
            if (row.type == ChangeType::KnockBack)     { WorldPacket p(out); Wire::KnockBack k; REQUIRE(Wire::DecodeKnockBack(p, k).ok()); CHECK_EQ(k.counter, counter); CHECK_EQ(k.guid, guid); }
            else if (row.type == ChangeType::Teleport) { WorldPacket p(out); Wire::Teleport t; REQUIRE(Wire::DecodeTeleport(p, t).ok()); CHECK_EQ(t.counter, counter); CHECK_EQ(t.guid, guid); }
            else { Wire::MovementStatus st; Wire::DecodeResult res; REQUIRE(Wire::DecodeWhole(out, Wire::SequenceFor(row.mover), st, res, false)); CHECK_EQ(st.counter, counter); CHECK_EQ(st.guid, guid); if (IsSpeed(row.type) || row.type == ChangeType::CollisionHeight) { CHECK_EQ(st.value, change.value); } }
            ++movers;
        }
        else
        {
            CHECK(!BuildMover(out, guid, counter, change));
        }
        if (row.spline)
        {
            REQUIRE(BuildSpline(out, guid, change));
            CHECK_EQ(out.GetOpcode(), row.spline);
            Wire::Verdict v = Wire::Judge(row.spline, out, false);
            CHECK(v.decoded);
            CHECK(v.exact);
            ++splines;
        }
        else
        {
            CHECK(!BuildSpline(out, guid, change));
        }
        if (row.observer)
        {
            REQUIRE(BuildObserver(out, guid, counter, change, status));
            CHECK_EQ(out.GetOpcode(), row.observer);
            Wire::Verdict v = Wire::Judge(row.observer, out, false);
            CHECK(v.decoded);
            CHECK(v.exact);
            ++observers;
        }
        else
        {
            CHECK(!BuildObserver(out, guid, counter, change, status));
        }
    }
    CHECK_EQ(movers, size_t(26));      // 30 rows less the four server-only ones
    CHECK_EQ(splines, size_t(25));     // 30 less CanTransition x2, CollisionHeight, KnockBack, Teleport
    CHECK_EQ(observers, size_t(25));   // 26 movers less PitchRate (Task 6 fills TurnRate and Teleport; PitchRate's own reader is BLOCKED)
}

TEST(MotionWriters_the_collision_height_writer_is_the_client_layout_not_the_legacy_one)
{
    std::vector<GoldenPacket> legacy;
    ServerLines("client-15595.log", SMSG_MOVE_SET_COLLISION_HGT, legacy);
    REQUIRE(legacy.size() == 4);
    for (size_t i = 0; i < legacy.size(); ++i)
    {
        WorldPacket p = FromHex(SMSG_MOVE_SET_COLLISION_HGT, legacy[i].hex.c_str());
        CHECK(!Wire::Judge(SMSG_MOVE_SET_COLLISION_HGT, p, false).decoded);   // the pinned golden failure
    }
    WorldPacket out;
    REQUIRE(BuildMover(out, 0x46, 7, HeightChange(2.0f, 1)));
    Wire::Verdict v = Wire::Judge(SMSG_MOVE_SET_COLLISION_HGT, out, false);
    CHECK(v.decoded);
    CHECK(v.exact);
    Wire::MovementStatus st;
    Wire::DecodeResult res;
    REQUIRE(Wire::DecodeWhole(out, Wire::SequenceFor(SMSG_MOVE_SET_COLLISION_HGT), st, res, false));
    CHECK_EQ(st.value, 2.0f);
    CHECK_EQ(st.twoBits, uint8(1));
    CHECK_EQ(st.counter, 7u);
}

TEST(MotionWriters_refuse_cells_the_matrix_leaves_empty)
{
    WorldPacket out;
    CHECK(!BuildMover(out, 0x46, 0, FlagChange(ChangeType::Gait, true)));
    CHECK(!BuildSpline(out, 0x46, HeightChange(1.0f, 0)));
    CHECK(!BuildObserver(out, 0x46, 0, FlagChange(ChangeType::Swim, true), Wire::MovementStatus()));
    Change none;
    CHECK(!BuildMover(out, 0x46, 0, none));
}

// ---- the point-carrying stop (design 2026-09-22 §3) -----------------------------------------

TEST(MotionWriters_the_point_carrying_stop_ends_at_the_type)
{
    // MoveSplineInit::StopHere's body, byte for byte. The layout the client read pinned:
    // packed guid | u8 0 | float xyz | u32 splineId | u8 type = Stop, and NOTHING after --
    // a classic type-1 spline reads no flags word, no duration and no facing.
    // peer/retail-taxi-flights-2026-09-22.md §3.2: the one waypoint equals the position in
    // 1,017 of 1,017 of the corpus's one-point packets, so there is only one point to write.
    WorldPacket p(SMSG_MONSTER_MOVE, 32);
    Movement::WriteMonsterMoveStop(p, 0x0000000000010046ULL, false, 0, 0, 1.0f, 2.0f, 3.0f, 6);
    CHECK_STR(Hex(p), "05" "4601" "00" "0000803F" "00000040" "00004040" "06000000" "01");
    CHECK_EQ(p.size(), size_t(21));

    // And it is the shape the decoder -- built from the client's own read -- expects: whole,
    // exact, a Stop, at the point the writer was given.
    Wire::MonsterMove back;
    p.rpos(0);
    REQUIRE(Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE, back).ok());
    CHECK(back.type == Wire::MonsterMoveType::Stop);
    CHECK_EQ(back.mover, 0x0000000000010046ULL);
    CHECK_EQ(back.id, uint32(6));
    CHECK_EQ(back.exitVoluntary, uint8(0));
    CHECK_EQ(back.start.x, 1.0f);
    CHECK_EQ(back.start.y, 2.0f);
    CHECK_EQ(back.start.z, 3.0f);
    CHECK(Wire::Judge(SMSG_MONSTER_MOVE, p, false).exact);
}

TEST(MotionWriters_the_point_carrying_stop_on_a_deck_carries_the_vessel_and_no_seat)
{
    // The transport form inserts the vessel and the seat between the mover and the exit byte.
    // A DECK has no seat: -1, which is what both reference cores send for a MO_TRANSPORT and
    // what MoveSplineInit passes for a unit standing on a ship.
    WorldPacket p(SMSG_MONSTER_MOVE_TRANSPORT, 32);
    Movement::WriteMonsterMoveStop(p, 0x0000000000010046ULL, true, 0x0000000000000102ULL, -1,
                                   1.0f, 2.0f, 3.0f, 6);
    CHECK_STR(Hex(p), "05" "4601" "03" "0201" "FF" "00" "0000803F" "00000040" "00004040" "06000000" "01");
    Wire::MonsterMove back;
    p.rpos(0);
    REQUIRE(Wire::DecodeMonsterMove(p, SMSG_MONSTER_MOVE_TRANSPORT, back).ok());
    CHECK(back.type == Wire::MonsterMoveType::Stop);
    CHECK(back.onTransport);
    CHECK_EQ(back.transport, 0x0000000000000102ULL);
    CHECK_EQ(back.seat, int8(-1));
    CHECK(Wire::Judge(SMSG_MONSTER_MOVE_TRANSPORT, p, false).exact);
}
