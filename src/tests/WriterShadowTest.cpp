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
#include "WriterShadow.h"

#include <string>
#include <vector>

using namespace Motion;

namespace
{
    const uint64 kGuid = 0x46;
    WorldPacket Mover(Change const& c, uint32 counter) { WorldPacket p; BuildMover(p, kGuid, counter, c); return p; }
    WorldPacket Spline(Change const& c) { WorldPacket p; BuildSpline(p, kGuid, c); return p; }
}

TEST(WriterShadow_exact_when_the_legacy_bytes_are_the_kernels)
{
    WorldPacket legacy = Mover(SpeedChange(1, 7.0f), 0);
    ShadowResult r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, legacy);
    CHECK(r.verdict == ShadowVerdict::Exact);
    CHECK_EQ(r.legacyOpcode, uint16(SMSG_MOVE_SET_RUN_SPEED));
    CHECK_EQ(r.kernelOpcode, uint16(SMSG_MOVE_SET_RUN_SPEED));
    CHECK_EQ(r.kernelSize, legacy.size());
    WorldPacket spline = Spline(FlagChange(ChangeType::Root, true));
    r = JudgeWriter(kGuid, FlagChange(ChangeType::Root, true), Mode::ServerDriven, 0, spline);
    CHECK(r.verdict == ShadowVerdict::Exact);
    CHECK_EQ(r.kernelOpcode, uint16(SMSG_SPLINE_MOVE_ROOT));
}

TEST(WriterShadow_reports_the_first_differing_byte_and_a_size_difference)
{
    WorldPacket legacy = Mover(SpeedChange(1, 7.0f), 0);
    REQUIRE(legacy.size() > 6);
    WorldPacket bent(legacy);
    bent.put<uint8>(5, uint8(bent.contents()[5] ^ 0xFF));
    ShadowResult r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, bent);
    CHECK(r.verdict == ShadowVerdict::Mismatch);
    CHECK_EQ(r.firstDifference, size_t(5));
    WorldPacket longer(legacy);
    longer << uint8(0);
    r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, longer);
    CHECK(r.verdict == ShadowVerdict::Mismatch);
    CHECK_EQ(r.firstDifference, legacy.size());
    CHECK_EQ(r.legacySize, legacy.size() + 1);
    // The counter the hook decoded is what the kernel writes; a different one is a real difference.
    r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 9, legacy);
    CHECK(r.verdict == ShadowVerdict::Mismatch);
}

TEST(WriterShadow_known_different_rows_count_apart)
{
    // The legacy collision-height writer is WotLK-shaped: whatever it sent, the kernel's differs.
    WorldPacket legacy(SMSG_MOVE_SET_COLLISION_HGT, 17);
    for (int i = 0; i < 17; ++i) { legacy << uint8(i); }
    ShadowResult r = JudgeWriter(kGuid, HeightChange(2.0f, 1), Mode::ClientDriven, 0, legacy);
    CHECK(r.verdict == ShadowVerdict::KnownDifferent);
    // The legacy player levitate builder sends ENABLE when asked to apply; the kernel sends DISABLE.
    WorldPacket enable = Mover(FlagChange(ChangeType::GravityDisabled, false), 0);
    r = JudgeWriter(kGuid, FlagChange(ChangeType::GravityDisabled, true), Mode::ClientDriven, 0, enable);
    CHECK(r.verdict == ShadowVerdict::KnownDifferent);
    CHECK_EQ(r.legacyOpcode, uint16(SMSG_MOVE_GRAVITY_ENABLE));
    CHECK_EQ(r.kernelOpcode, uint16(SMSG_MOVE_GRAVITY_DISABLE));
    CHECK_EQ(r.firstDifference, size_t(-1));
    // A creature's levitate is not inverted.
    WorldPacket spline = Spline(FlagChange(ChangeType::GravityDisabled, true));
    r = JudgeWriter(kGuid, FlagChange(ChangeType::GravityDisabled, true), Mode::ServerDriven, 0, spline);
    CHECK(r.verdict == ShadowVerdict::Exact);
}

TEST(WriterShadow_a_players_spline_speed_is_legacy_only_and_a_creatures_is_judged)
{
    WorldPacket spline = Spline(SpeedChange(1, 7.0f));
    ShadowResult r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, spline);
    CHECK(r.verdict == ShadowVerdict::LegacyOnly);
    CHECK_EQ(r.kernelOpcode, uint16(0));
    r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ServerDriven, 0, spline);
    CHECK(r.verdict == ShadowVerdict::Exact);
}

TEST(WriterShadow_a_packet_no_row_claims_or_a_wrong_change_has_no_writer)
{
    WorldPacket update(SMSG_MOVE_UPDATE_RUN_SPEED, 4);
    update << uint32(0);
    ShadowResult r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, update);
    CHECK(r.verdict == ShadowVerdict::NoWriter);
    WorldPacket legacy = Mover(SpeedChange(1, 7.0f), 0);
    r = JudgeWriter(kGuid, FlagChange(ChangeType::Root, true), Mode::ClientDriven, 0, legacy);   // a hook passing the wrong change
    CHECK(r.verdict == ShadowVerdict::NoWriter);
    r = JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ServerDriven, 0, legacy);                 // a mover form for a server-driven unit
    CHECK(r.verdict == ShadowVerdict::NoWriter);
}

TEST(WriterShadow_counts_per_row_and_form_and_reports)
{
    REQUIRE(MatrixSize() == size_t(30));   // guards ShadowCounters::kRows below: a mismatch must stop before Count() can run OOB
    CHECK_EQ(ShadowCounters::Rows(), MatrixSize() * 2 + 1);
    ShadowCounters counters;
    CHECK(!counters.Saw());
    WorldPacket legacy = Mover(SpeedChange(1, 7.0f), 0);
    counters.Count(JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, legacy));
    counters.Count(JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 9, legacy));
    WorldPacket spline = Spline(SpeedChange(1, 7.0f));
    counters.Count(JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, spline));
    WorldPacket update(SMSG_MOVE_UPDATE_RUN_SPEED, 4);
    update << uint32(0);
    counters.Count(JudgeWriter(kGuid, SpeedChange(1, 7.0f), Mode::ClientDriven, 0, update));
    CHECK(counters.Saw());
    CHECK_EQ(counters.Seen(), 4u);
    CHECK_EQ(counters.Mismatched(), 1u);
    std::vector<std::string> lines;
    counters.Report([&lines](std::string const& l) { lines.push_back(l); });
    REQUIRE(lines.size() == 4);   // the summary, the run-speed mover form, the run-speed spline form, the unmatched row
    CHECK(lines[0].find("4 seen") != std::string::npos);
    CHECK(lines[0].find("1 mismatched") != std::string::npos);
    CHECK(lines[0].find("1 legacy-only") != std::string::npos);
    CHECK(lines[0].find("1 no-writer") != std::string::npos);
    bool moverRow = false, splineRow = false;
    for (size_t i = 1; i < lines.size(); ++i)
    {
        if (lines[i].find("RunSpeed") == std::string::npos) { continue; }
        if (lines[i].find("mover") != std::string::npos)  { moverRow = true;  CHECK(lines[i].find("first at byte") != std::string::npos); }
        if (lines[i].find("spline") != std::string::npos) { splineRow = true; CHECK(lines[i].find("legacy-only 1") != std::string::npos); }
    }
    CHECK(moverRow);
    CHECK(splineRow);
}
