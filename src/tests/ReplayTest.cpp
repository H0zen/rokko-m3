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

// The replay unit: a capture file's lines through the registry's layouts and
// back, judged by the bytes. The two lines below are real 15595-shaped
// packets the P0-B peer sent (a start-forward and a heartbeat), recorded by the
// server's capture; the third is a server speed set built by the P1-A engine.
#include "TestHarness.h"

#include "Replay.hpp"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "wire/MovementCodec.h"
#include "wire/MovementSequences.h"

#include <sstream>

namespace
{
    const char* kStartForward = "C 0x7814 CDAC59C6E13AC9423D9A77C52011800000012F5C5A054067920100";
    const char* kHeartbeat    = "C 0x3914 E13AC94232B677C564A059C69104000000012F5C5A054064940100";
}

TEST(Replay_parses_a_capture_line_and_skips_comments)
{
    loadtest::CaptureLine line;
    std::string error;
    REQUIRE(loadtest::ParseCaptureLine(kStartForward, line, error));
    CHECK_EQ(int(line.direction), int('C'));
    CHECK_EQ(int(line.opcode), int(CMSG_MOVE_START_FORWARD));
    CHECK_EQ(line.bytes.size(), size_t(27));
    CHECK_EQ(int(line.bytes[0]), 0xCD);
    CHECK_EQ(int(line.bytes[26]), 0x00);

    CHECK(!loadtest::ParseCaptureLine("", line, error));
    CHECK(error.empty());
    CHECK(!loadtest::ParseCaptureLine("# a comment", line, error));
    CHECK(error.empty());
    CHECK(!loadtest::ParseCaptureLine("X 0x7814 00", line, error));
    CHECK(!error.empty());
    CHECK(!loadtest::ParseCaptureLine("C 0x7814 0", line, error));   // odd hex length
    CHECK(!error.empty());
}

TEST(Replay_judges_real_client_shaped_lines_exact)
{
    std::stringstream capture;
    capture << "# two peer lines\n" << kStartForward << "\n" << kHeartbeat << "\n";
    const loadtest::ReplayReport report = loadtest::Replay(capture);
    CHECK_EQ(report.lines, uint32(2));
    CHECK_EQ(report.malformed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK_EQ(report.decoded, uint32(2));
    CHECK_EQ(report.exact, uint32(2));
    CHECK(report.Clean());
    CHECK_EQ(report.byOpcode.at(CMSG_MOVE_START_FORWARD).exact, uint32(1));
    CHECK_EQ(report.byOpcode.at(MSG_MOVE_HEARTBEAT).exact, uint32(1));
}

TEST(Replay_counts_an_unregistered_opcode_without_judging_it)
{
    // CMSG_PING: not a movement packet at all, so neither a registry layout nor
    // a family. (SMSG_MOVE_KNOCK_BACK stood here until P1-C gave it a family.)
    std::stringstream capture;
    capture << "C 0x444D 0102030405\n";
    const loadtest::ReplayReport report = loadtest::Replay(capture);
    CHECK_EQ(report.lines, uint32(1));
    CHECK_EQ(report.unregistered, uint32(1));
    CHECK_EQ(report.decoded, uint32(0));
    CHECK(report.Clean());
}

TEST(Replay_reports_a_truncated_line_as_failed_and_a_malformed_one_as_malformed)
{
    // A valid packet followed by text is malformed too: the format is three
    // tokens, and a replay must not certify a line it only partly read.
    std::stringstream capture;
    capture << "C 0x7814 CDAC59C6\n" << "C 0x7814 zz\n"
            << "C 0x7814 CDAC59C6E13AC9423D9A77C52011800000012F5C5A054067920100 extra\n";
    const loadtest::ReplayReport report = loadtest::Replay(capture);
    CHECK_EQ(report.lines, uint32(1));
    CHECK_EQ(report.failed, uint32(1));
    CHECK_EQ(report.malformed, uint32(2));
    CHECK(!report.Clean());
    CHECK(!report.byOpcode.at(CMSG_MOVE_START_FORWARD).firstProblem.empty());
}

TEST(Replay_judges_a_server_speed_set_the_codec_wrote)
{
    Wire::MovementStatus set;
    set.guid = 0x0000000000000030ull;
    set.counter = 0;
    set.value = 7.0f;
    WorldPacket p(SMSG_MOVE_SET_RUN_SPEED, 32);
    Wire::Encode(p, Wire::SequenceFor(SMSG_MOVE_SET_RUN_SPEED), set);
    std::string hex;
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < p.size(); ++i)
    {
        hex += digits[p.contents()[i] >> 4];
        hex += digits[p.contents()[i] & 0x0F];
    }
    std::stringstream capture;
    capture << "S 0x3DB5 " << hex << "\n";
    const loadtest::ReplayReport report = loadtest::Replay(capture);
    CHECK_EQ(report.exact, uint32(1));
    CHECK(report.Clean());
}

TEST(Replay_counts_an_embedded_layout_apart_and_does_not_judge_it)
{
    // A real CMSG_USE_ITEM from a 15595 client: item fields first, the movement
    // block after them. Its opcode has a layout, but not for the whole packet.
    std::stringstream capture;
    capture << "C 0x2C06 FF0F01098D0100CC00000000000047000000000000000000\n";
    const loadtest::ReplayReport report = loadtest::Replay(capture);
    CHECK_EQ(report.lines, uint32(1));
    CHECK_EQ(report.embedded, uint32(1));
    CHECK_EQ(report.decoded, uint32(0));
    CHECK_EQ(report.failed, uint32(0));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK(report.Clean());
    CHECK_EQ(report.byOpcode.at(CMSG_USE_ITEM).embedded, uint32(1));
}

TEST(Replay_judges_a_family_packet_like_a_registry_one)
{
    // A knockback the tree's writer produced for guid 0x46 (Task 2's bytes)
    // and a control update: neither has a layout, both have a family, both
    // are judged, decoded, exact.
    std::stringstream capture;
    capture << "S 0x5CB4 " "80" "00000000" "07000000" "00002041" "000040C1" "0000803F" "47" "\n"
            << "S 0x2837 014601\n"
            << "C 0x3314 1047\n";
    const loadtest::ReplayReport report = loadtest::Replay(capture);
    CHECK_EQ(report.lines, uint32(3));
    CHECK_EQ(report.decoded, uint32(3));
    CHECK_EQ(report.exact, uint32(3));
    CHECK_EQ(report.unregistered, uint32(0));
    CHECK(report.Clean());
    CHECK_EQ(report.byOpcode.at(SMSG_MOVE_KNOCK_BACK).exact, uint32(1));
    CHECK_EQ(report.byOpcode.at(CMSG_SET_ACTIVE_MOVER).exact, uint32(1));
}
