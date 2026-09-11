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

// Every golden capture under src/tests/goldens/movement replays clean through
// the registry: decoded whole, re-encoded byte for byte. The files are listed
// here by name on purpose -- a golden that goes missing must fail, not vanish.
#include "TestHarness.h"

#include "Replay.hpp"
#include "Opcodes.h"
#include "PacketMatrix.h"
#include "WorldPacket.h"
#include "wire/MovementCodec.h"
#include "wire/MovementSequences.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#ifndef MANGOS_TEST_DATA_DIR
#error "MANGOS_TEST_DATA_DIR must name src/tests/goldens (see src/tests/CMakeLists.txt)"
#endif

namespace
{
    // void, not value-returning: the harness's REQUIRE leaves the enclosing
    // function with a bare `return;` on failure (the P0-A plan hit exactly this).
    void ReplayGolden(const char* name, loadtest::ReplayReport& out)
    {
        const std::string path = std::string(MANGOS_TEST_DATA_DIR) + "/movement/" + name;
        std::ifstream capture(path.c_str());
        REQUIRE(bool(capture));
        out = loadtest::Replay(capture);
    }

    // Splits a golden by direction so the client's lines (ground truth for the
    // layouts) and this server's lines (its legacy writers under test) are
    // judged apart.
    void ReplayGoldenByDirection(const char* name, char direction, loadtest::ReplayReport& out)
    {
        const std::string path = std::string(MANGOS_TEST_DATA_DIR) + "/movement/" + name;
        std::ifstream capture(path.c_str());
        REQUIRE(bool(capture));
        std::stringstream picked;
        std::string text;
        while (std::getline(capture, text))
        {
            if (!text.empty() && text[0] == direction) { picked << text << "\n"; }
        }
        out = loadtest::Replay(picked);
    }

    // The four movement flags a direction key sets, by their names in
    // src/game/Object/Unit.h:739-742 -- MOVEFLAG_FORWARD 0x1,
    // MOVEFLAG_BACKWARD 0x2, MOVEFLAG_STRAFE_LEFT 0x4, MOVEFLAG_STRAFE_RIGHT
    // 0x8 -- spelled out here because this test links proto and shared only and
    // cannot include Unit.h.
    const uint32 kDirectionKeys = 0x1 | 0x2 | 0x4 | 0x8;
    const uint32 kForwardOnly   = 0x1;
}

TEST(GoldenCapture_peer_walk_replays_clean)
{
    loadtest::ReplayReport report;
    ReplayGolden("peer-walk-15595.log", report);
    CHECK(report.lines >= 60);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.failed, uint32(0));
    // Clean() ignores unregistered lines on purpose, so without this a golden
    // that had lost every layout would still pass the presence checks below.
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    CHECK(report.Clean());
    // What a walk-and-relay must contain, both directions.
    CHECK(report.byOpcode.count(CMSG_MOVE_START_FORWARD) == 1);
    CHECK(report.byOpcode.count(MSG_MOVE_HEARTBEAT) == 1);
    CHECK(report.byOpcode.count(CMSG_MOVE_STOP) == 1);
    CHECK(report.byOpcode.count(SMSG_PLAYER_MOVE) == 1);
    CHECK(report.byOpcode.count(SMSG_MOVE_SET_RUN_SPEED) == 1);
    CHECK(report.byOpcode.count(CMSG_FORCE_RUN_SPEED_CHANGE_ACK) == 1);
}

TEST(GoldenCapture_client_built_lines_replay_clean)
{
    // Every packet the 15595 client built decodes whole with the registry's
    // layout and re-encodes byte for byte; the three cast packets are embedded
    // layouts and are counted apart, not judged.
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595.log", 'C', report);
    CHECK(report.lines >= 100);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.failed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    CHECK(report.Clean());
    CHECK_EQ(report.embedded, uint32(3));
    CHECK_EQ(report.byOpcode.at(CMSG_USE_ITEM).embedded, uint32(2));
    CHECK_EQ(report.byOpcode.at(CMSG_CAST_SPELL).embedded, uint32(1));
}

TEST(GoldenCapture_server_built_lines_fail_only_where_the_legacy_writer_is_known_wrong)
{
    // This server's own packets from the same session. The failures are the
    // known list of legacy writers that do not produce the 4.3.4 layout: P2
    // rewrites them, and each removal from this list is a fix landing.
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595.log", 'S', report);
    CHECK(report.lines >= 20);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    // SMSG_MOVE_SET_COLLISION_HGT: Unit.cpp:7430 writes mask, bytes, counter,
    // bytes, float -- the WotLK shape; 4.3.4 carries a two-bit reason before the
    // float. Sent at both mount/dismount cycles the session had: four packets.
    CHECK_EQ(report.failed, uint32(4));
    CHECK_EQ(report.byOpcode.at(SMSG_MOVE_SET_COLLISION_HGT).failed, uint32(4));
    CHECK_EQ(report.byOpcode.at(SMSG_MOVE_SET_COLLISION_HGT).decoded, uint32(0));
    for (std::map<uint16, loadtest::ReplayRow>::const_iterator it = report.byOpcode.begin(); it != report.byOpcode.end(); ++it)
    {
        if (it->first != SMSG_MOVE_SET_COLLISION_HGT) { CHECK_EQ(it->second.failed, uint32(0)); }
    }
}

TEST(GoldenCapture_client_session_covers_the_checklist)
{
    loadtest::ReplayReport report;
    ReplayGolden("client-15595.log", report);
    // Required by the checklist's steps 1-9.
    static const uint16 kRequired[] =
    {
        CMSG_MOVE_START_FORWARD, MSG_MOVE_HEARTBEAT, CMSG_MOVE_STOP,
        CMSG_MOVE_START_BACKWARD, CMSG_MOVE_START_STRAFE_LEFT, CMSG_MOVE_START_STRAFE_RIGHT, CMSG_MOVE_STOP_STRAFE,
        CMSG_MOVE_START_TURN_LEFT, CMSG_MOVE_START_TURN_RIGHT, CMSG_MOVE_STOP_TURN, CMSG_MOVE_SET_FACING,
        CMSG_MOVE_SET_WALK_MODE, CMSG_MOVE_SET_RUN_MODE,
        CMSG_MOVE_JUMP, CMSG_MOVE_FALL_LAND,
        CMSG_MOVE_START_SWIM, CMSG_MOVE_STOP_SWIM, CMSG_MOVE_SET_PITCH,
        SMSG_MOVE_SET_RUN_SPEED, CMSG_FORCE_RUN_SPEED_CHANGE_ACK,
        SMSG_MOVE_SET_CAN_FLY, CMSG_MOVE_SET_CAN_FLY_ACK, SMSG_MOVE_UNSET_CAN_FLY,
        CMSG_MOVE_START_ASCEND, CMSG_MOVE_STOP_ASCEND,
        SMSG_MOVE_SET_FLIGHT_SPEED, CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK,
        CMSG_FORCE_MOVE_ROOT_ACK, CMSG_FORCE_MOVE_UNROOT_ACK,
    };
    for (uint16 op : kRequired)
    {
        CHECK(report.byOpcode.count(op) == 1);
        if (report.byOpcode.count(op) == 1)
        {
            CHECK(report.byOpcode.at(op).exact >= 1);
        }
    }
    // Optional (steps 10-11): counted when present, so the report says what the run had.
    static const uint16 kOptional[] =
    {
        SMSG_FORCE_MOVE_ROOT, SMSG_FORCE_MOVE_UNROOT,
        CMSG_MOVE_CHNG_TRANSPORT,
    };
    int optional = 0;
    for (uint16 op : kOptional) { optional += report.byOpcode.count(op) == 1 ? 1 : 0; }
    std::printf("    client golden: %d of 3 optional opcodes present\n", optional);
}

// Four protocol facts a real client's own bytes establish here, and P2's fall
// model will rely on all of them:
//  1. Landing (CMSG_MOVE_FALL_LAND) reports fall time and vertical speed but
//     never a direction -- HasFallDirection is never set on any of the
//     golden's 82 lines, so a landing packet is never judged for direction.
//  2. A fall that ends in water sends CMSG_MOVE_START_SWIM, never
//     CMSG_MOVE_FALL_LAND -- a packet-layout line with no fall block at all
//     (fall.present false) closes an open window just as landing does.
//  3. A fall can begin without a jump (stepping off an edge, leaving the
//     water onto a drop): no CMSG_MOVE_JUMP announces its direction, so no
//     window opens for it and its packets are never echo-checked.
//  4. A jump with no direction key held is a standing jump: its own bytes
//     carry no usable takeoff (fall.hasDirection can still be set, but there
//     is nothing frozen to echo), and the client's one piece of air control
//     is that the first direction key pressed while still airborne sets the
//     fall's horizontal direction from the facing at that moment -- a second
//     takeoff this model does not track. A standing jump therefore opens no
//     window, and closes any window already open.
//
// A jump's own direction, when a direction key is held at takeoff, is frozen
// there and echoed unchanged in every other packet the client sends while
// still airborne (heartbeats, turns, strafes, facing changes...) even as its
// own facing keeps changing -- the pair belongs to the fall, not read fresh
// each packet. That echo is this test's second proof, alongside the jump
// itself: MSG_MOVE_HEARTBEAT, CMSG_MOVE_START_STRAFE_LEFT,
// CMSG_MOVE_SET_FACING and CMSG_MOVE_START_BACKWARD carry the two fall floats
// Cos-then-Sin in CPP's tables, the opposite wire order from CMSG_MOVE_JUMP's
// Sin-then-Cos -- if the flipped labels decode the identical takeoff pair from
// both wire orders, the error in CPP was in the names, not the positions.
TEST(GoldenCapture_client_fall_blocks_settle_the_fall_angle_labels)
{
    const std::string path = std::string(MANGOS_TEST_DATA_DIR) + "/movement/client-15595.log";
    std::ifstream capture(path.c_str());
    REQUIRE(bool(capture));
    std::string text;
    std::map<uint16, int> judged;
    std::map<uint16, int> echoed;
    bool fallOpen = false;
    float takeoffCos = 0.0f;
    float takeoffSin = 0.0f;
    int spread = 0;
    while (std::getline(capture, text))
    {
        loadtest::CaptureLine line;
        std::string error;
        if (!loadtest::ParseCaptureLine(text, line, error) || line.direction != 'C')
        {
            continue;
        }
        if (Wire::IsEmbeddedLayout(line.opcode))
        {
            continue;                                   // the cast opcodes: a block inside the packet, not judged
        }
        const Wire::Sequence layout = Wire::SequenceFor(line.opcode);
        if (!layout)
        {
            continue;
        }
        WorldPacket packet(line.opcode, line.bytes.size());
        packet.append(line.bytes.data(), line.bytes.size());
        Wire::MovementStatus s;
        REQUIRE(Wire::Decode(packet, layout, s).ok());

        if (line.opcode == CMSG_MOVE_JUMP)
        {
            if (s.fall.present && s.fall.hasDirection && s.has.orientation && (s.flags & kDirectionKeys) == kForwardOnly)
            {
                // Pure forward: direction is the facing.
                const float c = std::cos(s.pos.o);
                const float n = std::sin(s.pos.o);
                CHECK(std::fabs(s.fall.cosAngle - c) < 0.15f);
                CHECK(std::fabs(s.fall.sinAngle - n) < 0.15f);
                // Cos and sin far apart: a heading where the two labels cannot be
                // swapped and still pass the 0.15 tolerance.
                if (std::fabs(c - n) > 0.5f) { ++spread; }
                ++judged[CMSG_MOVE_JUMP];
            }
            if (s.fall.present && s.fall.hasDirection && (s.flags & kDirectionKeys) != 0)
            {
                // A jump with a direction key held is frozen at takeoff: opens
                // the echo window with that pair.
                takeoffCos = s.fall.cosAngle;
                takeoffSin = s.fall.sinAngle;
                fallOpen = true;
            }
            else
            {
                // A standing jump: no takeoff this model tracks (fact 4). Closing
                // an open window here is an addition beyond the two ruled
                // conditions (CMSG_MOVE_FALL_LAND, the first packet with no fall
                // block): a standing jump's direction is set by the first key
                // pressed in the air, so a window still open across it would judge
                // echoes of a pair no jump ever announced.
                fallOpen = false;
            }
            continue;
        }
        if (line.opcode == CMSG_MOVE_FALL_LAND)
        {
            CHECK(!s.fall.hasDirection);            // fact 1, on every landing the golden has
            fallOpen = false;
            continue;
        }
        if (!s.fall.present)
        {
            fallOpen = false;                           // the client says it is no longer falling (facts 2, 3)
        }
        else if (fallOpen && s.fall.hasDirection)
        {
            CHECK(std::fabs(s.fall.cosAngle - takeoffCos) < 1e-5f);
            CHECK(std::fabs(s.fall.sinAngle - takeoffSin) < 1e-5f);
            ++echoed[line.opcode];
        }
    }
    int totalEchoes = 0;
    for (std::map<uint16, int>::const_iterator it = judged.begin(); it != judged.end(); ++it)
    {
        std::printf("    fall block 0x%04X: %d judged pure-forward\n", uint32(it->first), it->second);
    }
    for (std::map<uint16, int>::const_iterator it = echoed.begin(); it != echoed.end(); ++it)
    {
        std::printf("    fall block 0x%04X: %d echoed the takeoff pair\n", uint32(it->first), it->second);
        totalEchoes += it->second;
    }
    std::printf("    fall block 0x%04X: %d judged from a heading with cos and sin far apart\n",
                uint32(CMSG_MOVE_JUMP), spread);
    REQUIRE(judged[CMSG_MOVE_JUMP] >= 20);
    REQUIRE(totalEchoes >= 20);
    // A capture whose jumps all faced a diagonal (cos == sin) could not tell the
    // two labels apart at all -- swapping them would pass the tolerance above --
    // so the proof rests on the judged jumps spanning headings that do not.
    CHECK(spread >= 10);
    // At least one Cos-then-Sin table (the opposite wire order from the jump
    // itself) echoed the identical pair: the flip is proven at both wire orders.
    REQUIRE(echoed[MSG_MOVE_HEARTBEAT] + echoed[CMSG_MOVE_START_STRAFE_LEFT] +
            echoed[CMSG_MOVE_SET_FACING] + echoed[CMSG_MOVE_START_BACKWARD] >= 1);
}

// A second real 15595 client's session (branch feat/movement-wire-families), around the
// families the generated registry cannot describe on its own: the client's own teleport
// ack and active mover, and this server's teleport, knockback and several hundred
// monster moves. See src/tests/goldens/movement/README.md for the session's contents.
TEST(GoldenCapture_families_client_built_lines_replay_clean)
{
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595-families.log", 'C', report);
    CHECK(report.lines >= 200);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.failed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    CHECK(report.byOpcode.at(CMSG_MOVE_TELEPORT_ACK).exact >= 3);
    CHECK(report.byOpcode.at(CMSG_SET_ACTIVE_MOVER).exact >= 2);
    CHECK(report.byOpcode.at(CMSG_MOVE_KNOCK_BACK_ACK).exact >= 3);
}

TEST(GoldenCapture_families_server_built_lines_fail_only_where_the_legacy_writer_is_known_wrong)
{
    // This server's own packets from the same session. SMSG_CLIENT_CONTROL_UPDATE has no
    // floor here: this tree sends none at login (the client assumes control without one);
    // P2's packet matrix decides whether that is right. SMSG_MOVE_UPDATE_KNOCK_BACK is
    // ABSENT, not wrong: no observer stood in the world for this session, and that relay
    // goes only to the observers around the mover, so there was nothing to capture. The
    // live gate's two-bot pair (an observer holding nearby while a walker is knocked back)
    // did capture one, and the shadow judged it under the lifted layout: decoded whole and
    // re-encoded exact. So the relay (MovementHandler.cpp, `data << movementInfo`) is not
    // WotLK-shaped after all, and the collision-height writer below is the one known
    // server-side failure that remains.
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595-families.log", 'S', report);
    CHECK(report.lines >= 200);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK(report.byOpcode.at(SMSG_MONSTER_MOVE).exact >= 200);
    CHECK_EQ(report.byOpcode.at(SMSG_MONSTER_MOVE).failed, uint32(0));
    CHECK(report.byOpcode.at(SMSG_MOVE_TELEPORT).exact >= 3);
    CHECK(report.byOpcode.at(SMSG_MOVE_KNOCK_BACK).exact >= 3);
    // SMSG_MOVE_SET_COLLISION_HGT: the one known legacy writer in this session (P1-B) --
    // Unit.cpp still writes the WotLK shape, not the 4.3.4 one.
    CHECK_EQ(report.byOpcode.at(SMSG_MOVE_SET_COLLISION_HGT).failed, uint32(1));
    for (std::map<uint16, loadtest::ReplayRow>::const_iterator it = report.byOpcode.begin(); it != report.byOpcode.end(); ++it)
    {
        if (it->first != SMSG_MOVE_SET_COLLISION_HGT)
        {
            CHECK_EQ(it->second.failed, uint32(0));
            CHECK_EQ(it->second.exact, it->second.decoded);
        }
    }
}

// The relay flip's client session (P2-B, branch feat/movement-relay-flip): the
// session whose taxi flights showed that the reference table for
// CMSG_MOVE_SPLINE_DONE lacks the movement counter a 15595 client writes first.
// One of its four spline-done packets overran that table and was rejected; the
// other three decoded four bytes off into a status that happened to fit. The
// generator's CORRECTED entry leads the table with the counter, and these four
// lines are the witness: whole and exact, or the suite fails.
TEST(GoldenCapture_flip_client_built_lines_replay_clean)
{
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595-flip.log", 'C', report);
    CHECK(report.lines >= 200);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.failed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    CHECK(report.Clean());
    // No spell was cast in the session: nothing embedded to count apart.
    CHECK_EQ(report.embedded, uint32(0));
    CHECK(report.byOpcode.count(CMSG_MOVE_SPLINE_DONE) == 1);
    if (report.byOpcode.count(CMSG_MOVE_SPLINE_DONE) == 1)
    {
        CHECK_EQ(report.byOpcode.at(CMSG_MOVE_SPLINE_DONE).lines, uint32(4));
        CHECK_EQ(report.byOpcode.at(CMSG_MOVE_SPLINE_DONE).exact, uint32(4));
    }
    // The acks the flip made live decode-or-throw (hover excepted: nothing in
    // the session hovers), the former 45-element stub and its ack, the speed
    // acks whose correct-or-kick the flip switched on, and the acks of the two
    // lifted families.
    static const uint16 kRequired[] =
    {
        CMSG_MOVE_WATER_WALK_ACK, CMSG_MOVE_FEATHER_FALL_ACK,
        CMSG_FORCE_MOVE_ROOT_ACK, CMSG_FORCE_MOVE_UNROOT_ACK,
        CMSG_MOVE_SET_CAN_FLY, CMSG_MOVE_SET_CAN_FLY_ACK,
        CMSG_FORCE_RUN_SPEED_CHANGE_ACK, CMSG_FORCE_SWIM_SPEED_CHANGE_ACK, CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK,
        CMSG_MOVE_KNOCK_BACK_ACK, CMSG_MOVE_TELEPORT_ACK,
    };
    for (uint16 op : kRequired)
    {
        CHECK(report.byOpcode.count(op) == 1);
        if (report.byOpcode.count(op) == 1)
        {
            CHECK(report.byOpcode.at(op).exact >= 1);
        }
    }
}

TEST(GoldenCapture_flip_server_built_lines_fail_only_where_the_legacy_writer_is_known_wrong)
{
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595-flip.log", 'S', report);
    CHECK(report.lines >= 50);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    // SMSG_MOVE_SET_COLLISION_HGT: the recorded golden carries the WotLK-shaped
    // collision packets the legacy writer sent when it was captured; P2-C's
    // writer sends the 4.3.4 shape, which the flip's golden (Task 8) will carry.
    // One at each of the session's four taxi landings.
    CHECK_EQ(report.failed, uint32(4));
    CHECK_EQ(report.byOpcode.at(SMSG_MOVE_SET_COLLISION_HGT).failed, uint32(4));
    CHECK_EQ(report.byOpcode.at(SMSG_MOVE_SET_COLLISION_HGT).decoded, uint32(0));
    for (std::map<uint16, loadtest::ReplayRow>::const_iterator it = report.byOpcode.begin(); it != report.byOpcode.end(); ++it)
    {
        if (it->first != SMSG_MOVE_SET_COLLISION_HGT) { CHECK_EQ(it->second.failed, uint32(0)); }
    }
}

// What the one ack handler reads from a real client's ack, through the same registry
// decode: the counter it matches on and, for a speed or a height, the value it checks.
// The lines are the goldens' own; a table that stops yielding these fails here before
// it fails live.
static void DecodeGoldenAck(char const* hex, uint16 opcode, Wire::MovementStatus& status)
{
    WorldPacket packet(opcode, 64);
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
    {
        packet << uint8(std::strtoul(std::string(hex + i, 2).c_str(), NULL, 16));
    }
    Wire::DecodeResult const r = Wire::Decode(packet, Wire::SequenceFor(opcode), status);
    REQUIRE(r.ok());
    CHECK_EQ(r.consumed, packet.size());
}

TEST(GoldenCapture_the_acks_yield_the_counter_and_payload_the_handler_matches_on)
{
    Wire::MovementStatus s;
    // client-15595-flip.log: the first run speed ack of the session (counter 0, the
    // base 7.0 yd/s echoed: bytes 8-11 are 0000E040); client-15595.log: the root ack.
    DecodeGoldenAck("00000000009C77C50000E040E13AC94200AC59C60226802FB18601001635A740", CMSG_FORCE_RUN_SPEED_CHANGE_ACK, s);
    CHECK_EQ(s.counter, 0u);
    CHECK_EQ(s.value, 7.0f);
    REQUIRE(Motion::RowForAck(CMSG_FORCE_RUN_SPEED_CHANGE_ACK) != NULL);
    CHECK(Motion::IsSpeed(Motion::RowForAck(CMSG_FORCE_RUN_SPEED_CHANGE_ACK)->type));
    DecodeGoldenAck("B833244418AAC542000000005A7706C6122020000000400008BDCAC63D1936B040", CMSG_FORCE_MOVE_ROOT_ACK, s);
    CHECK_EQ(s.counter, 0u);
    CHECK(Motion::RowForAck(CMSG_FORCE_MOVE_ROOT_ACK)->type == Motion::ChangeType::Root);
    // client-15595-flip.log: water walk, feather fall, can-fly, knock-back.
    DecodeGoldenAck("625A76C55F0D924100000000FE48944450008400000080000FBC53A44030380600", CMSG_MOVE_WATER_WALK_ACK, s);
    CHECK_EQ(s.counter, 0u);
    CHECK(Motion::RowForAck(CMSG_MOVE_WATER_WALK_ACK)->type == Motion::ChangeType::WaterWalk);
    DecodeGoldenAck("5F0D924100000000625A76C5FE4894442800A000800000000FDAF50500BC53A440", CMSG_MOVE_FEATHER_FALL_ACK, s);
    CHECK(Motion::RowForAck(CMSG_MOVE_FEATHER_FALL_ACK)->type == Motion::ChangeType::FeatherFall);
    DecodeGoldenAck("EA9289C50000000089A3C344BCD4834110806000080000000F94BD304083470500", CMSG_MOVE_SET_CAN_FLY_ACK, s);
    CHECK(Motion::RowForAck(CMSG_MOVE_SET_CAN_FLY_ACK)->type == Motion::ChangeType::CanFly);
    DecodeGoldenAck("00AC59C6E13AC94200000000009C77C53016602F2A37FBBE000020410B115F3F00000000000020C12F9501001635A740", CMSG_MOVE_KNOCK_BACK_ACK, s);
    CHECK(Motion::RowForAck(CMSG_MOVE_KNOCK_BACK_ACK)->type == Motion::ChangeType::KnockBack);
    CHECK(s.fall.present);   // a knock-back ack carries the fall the client is in
}

// The change pipeline's client session (P2-C, branch feat/movement-change-pipeline):
// the login snapshot's seven speed sets and their acks -- the walk, run-back,
// swim-back and flight-back acks among them, which no server in this tree had asked
// a client for before -- the forced speed change and its acks, and the mount and
// dismount with the collision-height sets the kernel writes in the 4.3.4 shape and
// their acks. Every line, client- and server-built, replays whole and exact: the
// collision-height writer is no longer a known failure.
TEST(GoldenCapture_pipeline_client_built_lines_replay_clean)
{
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595-pipeline.log", 'C', report);
    CHECK(report.lines >= 30);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.failed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    CHECK(report.Clean());
    CHECK_EQ(report.embedded, uint32(0));
    // The seven speed acks of the login snapshot, each at least once; the four the
    // legacy never elicited exactly once (nothing else in the session changed them).
    static const uint16 kOnce[] =
    {
        CMSG_FORCE_WALK_SPEED_CHANGE_ACK, CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK,
        CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK, CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK,
    };
    for (uint16 op : kOnce)
    {
        CHECK(report.byOpcode.count(op) == 1);
        if (report.byOpcode.count(op) == 1)
        {
            CHECK_EQ(report.byOpcode.at(op).lines, uint32(1));
            CHECK_EQ(report.byOpcode.at(op).exact, uint32(1));
        }
    }
    static const uint16 kRequired[] =
    {
        CMSG_FORCE_RUN_SPEED_CHANGE_ACK, CMSG_FORCE_SWIM_SPEED_CHANGE_ACK, CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK,
        CMSG_MOVE_SET_COLLISION_HGT_ACK,
    };
    for (uint16 op : kRequired)
    {
        CHECK(report.byOpcode.count(op) == 1);
        if (report.byOpcode.count(op) == 1)
        {
            CHECK(report.byOpcode.at(op).exact >= 1);
        }
    }
    // The mount and the dismount: two collision-height acks.
    CHECK_EQ(report.byOpcode.at(CMSG_MOVE_SET_COLLISION_HGT_ACK).exact, uint32(2));
}

TEST(GoldenCapture_pipeline_server_built_lines_replay_clean)
{
    loadtest::ReplayReport report;
    ReplayGoldenByDirection("client-15595-pipeline.log", 'S', report);
    CHECK(report.lines >= 50);
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.failed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.exact, report.decoded);
    CHECK(report.Clean());
    // The two collision-height sets in the 4.3.4 shape, the seven login speed sets
    // (run at least three times: the snapshot and the forced change and back).
    CHECK(report.byOpcode.count(SMSG_MOVE_SET_COLLISION_HGT) == 1);
    if (report.byOpcode.count(SMSG_MOVE_SET_COLLISION_HGT) == 1)
    {
        CHECK_EQ(report.byOpcode.at(SMSG_MOVE_SET_COLLISION_HGT).exact, uint32(2));
    }
    CHECK(report.byOpcode.count(SMSG_MOVE_SET_RUN_SPEED) == 1);
    if (report.byOpcode.count(SMSG_MOVE_SET_RUN_SPEED) == 1)
    {
        CHECK(report.byOpcode.at(SMSG_MOVE_SET_RUN_SPEED).exact >= 3);
    }
    static const uint16 kSets[] =
    {
        SMSG_MOVE_SET_WALK_SPEED, SMSG_MOVE_SET_RUN_BACK_SPEED, SMSG_MOVE_SET_SWIM_SPEED,
        SMSG_MOVE_SET_SWIM_BACK_SPEED, SMSG_MOVE_SET_FLIGHT_SPEED, SMSG_MOVE_SET_FLIGHT_BACK_SPEED,
    };
    for (uint16 op : kSets)
    {
        CHECK(report.byOpcode.count(op) == 1);
        if (report.byOpcode.count(op) == 1)
        {
            CHECK(report.byOpcode.at(op).exact >= 1);
        }
    }
}
