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
#include "State.h"

using namespace Motion;

namespace
{
    TimeoutPolicy Off() { return TimeoutPolicy(); }
    TimeoutPolicy Enforcing() { TimeoutPolicy p; p.timeoutMs = 1000; p.maxResends = 1; p.maxResyncs = 1; return p; }
    AckPayload Speed(float v) { AckPayload a; a.hasValue = true; a.value = v; return a; }
    AckPayload Flag() { return AckPayload(); }
}

TEST(MotionState_server_driven_commits_at_once_and_emits_the_spline_form)
{
    State s(Mode::ServerDriven, Off(), Kinematics());
    std::vector<Emission> e = s.Apply(SpeedChange(1, 7.0f), 0);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == EmissionKind::Spline);
    CHECK_EQ(e[0].opcode, uint16(SMSG_SPLINE_MOVE_SET_RUN_SPEED));
    CHECK_EQ(e[0].counter, 0u);
    CHECK_EQ(e[0].change.value, 7.0f);
    CHECK_EQ(s.Desired().speed[1], 7.0f);
    CHECK(s.Confirmed() == s.Desired());

    e = s.Apply(FlagChange(ChangeType::Gait, true), 1);
    REQUIRE(e.size() == 1);
    CHECK_EQ(e[0].opcode, uint16(SMSG_SPLINE_MOVE_SET_WALK_MODE));
    CHECK(s.Desired().walk);

    e = s.Apply(HeightChange(2.5f, 1), 2);          // no spline cell: committed, nothing to send
    CHECK(e.empty());
    CHECK_EQ(s.Confirmed().collisionHeight, 2.5f);
    CHECK_EQ(s.Pending().Size(), size_t(0));
    CHECK_EQ(s.Counters().applied, 3u);
    CHECK_EQ(s.Counters().emitted, 2u);
}

TEST(MotionState_client_driven_commits_desired_opens_a_pending_and_emits_the_mover_form)
{
    State s(Mode::ClientDriven, Off(), Kinematics());
    std::vector<Emission> e = s.Apply(SpeedChange(1, 7.0f), 0);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == EmissionKind::Mover);
    CHECK_EQ(e[0].opcode, uint16(SMSG_MOVE_SET_RUN_SPEED));
    CHECK_EQ(e[0].counter, 0u);
    CHECK_EQ(s.Desired().speed[1], 7.0f);
    CHECK_EQ(s.Confirmed().speed[1], 0.0f);
    CHECK(s.Pending().Has(ChangeType::RunSpeed));

    KnockBackParams k; k.directionX = 1.0f; k.horizontal = 10.0f; k.vertical = -5.0f;
    e = s.Apply(KnockBackChange(k), 1);
    REQUIRE(e.size() == 1);
    CHECK_EQ(e[0].opcode, uint16(SMSG_MOVE_KNOCK_BACK));
    CHECK_EQ(e[0].counter, 1u);
    CHECK(s.Pending().Has(ChangeType::KnockBack));
    Kinematics expected;
    expected.speed[1] = 7.0f;
    CHECK(s.Desired() == expected);   // a knock-back changes no kinematic field
}

TEST(MotionState_ack_confirms_and_emits_the_observer_form)
{
    State s(Mode::ClientDriven, Off(), Kinematics());
    s.Apply(SpeedChange(1, 7.0f), 0);
    std::vector<Emission> e = s.Ack(ChangeType::RunSpeed, 0, Speed(7.0f), 10);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == EmissionKind::Observer);
    CHECK_EQ(e[0].opcode, uint16(SMSG_MOVE_UPDATE_RUN_SPEED));
    CHECK_EQ(e[0].counter, 0u);
    CHECK_EQ(e[0].change.value, 7.0f);
    CHECK(s.LastAck() == AckResult::Matched);
    CHECK_EQ(s.Confirmed().speed[1], 7.0f);
    CHECK_EQ(s.Pending().Size(), size_t(0));

    e = s.Apply(FlagChange(ChangeType::Root, true), 20);
    REQUIRE(e.size() == 1);
    CHECK_EQ(e[0].opcode, uint16(SMSG_FORCE_MOVE_ROOT));
    const uint32 c = e[0].counter;
    e = s.Ack(ChangeType::Root, c, Flag(), 30);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == EmissionKind::Observer);
    CHECK_EQ(e[0].opcode, uint16(SMSG_FORCE_MOVE_ROOT));   // root is rebroadcast as itself
    CHECK(s.Confirmed().root);
    CHECK_EQ(s.Counters().confirmed, 2u);
}

TEST(MotionState_a_mismatched_or_stray_ack_confirms_nothing)
{
    State s(Mode::ClientDriven, Off(), Kinematics());
    s.Apply(SpeedChange(1, 7.0f), 0);
    std::vector<Emission> e = s.Ack(ChangeType::RunSpeed, 0, Speed(9.0f), 1);
    CHECK(e.empty());
    CHECK(s.LastAck() == AckResult::PayloadMismatch);
    CHECK_EQ(s.Confirmed().speed[1], 0.0f);
    CHECK_EQ(s.Pending().Size(), size_t(0));
    e = s.Ack(ChangeType::RunSpeed, 0, Speed(7.0f), 2);
    CHECK(e.empty());
    CHECK(s.LastAck() == AckResult::NoPending);

    State server(Mode::ServerDriven, Off(), Kinematics());
    e = server.Ack(ChangeType::RunSpeed, 0, Speed(7.0f), 3);
    CHECK(e.empty());
    CHECK(server.LastAck() == AckResult::NoPending);
}

TEST(MotionState_refuses_gait_and_swim_on_a_client_driven_unit)
{
    State s(Mode::ClientDriven, Off(), Kinematics());
    CHECK(s.Apply(FlagChange(ChangeType::Gait, true), 0).empty());
    CHECK(s.Apply(FlagChange(ChangeType::Swim, true), 0).empty());
    CHECK(!s.Desired().walk);
    CHECK(!s.Desired().swim);
    CHECK_EQ(s.Counters().refused, 2u);
    CHECK_EQ(s.Counters().applied, 0u);
    Change none;
    CHECK(s.Apply(none, 0).empty());
    CHECK_EQ(s.Counters().refused, 3u);
}

TEST(MotionState_new_epoch_retires_pending_and_keeps_desired)
{
    State s(Mode::ClientDriven, Off(), Kinematics());
    std::vector<Emission> e = s.Apply(SpeedChange(1, 7.0f), 0);
    const uint32 c = e[0].counter;
    s.NewEpoch(5);
    CHECK_EQ(s.Pending().Size(), size_t(0));
    CHECK_EQ(s.Pending().Epoch(), 1u);
    CHECK_EQ(s.Desired().speed[1], 7.0f);
    CHECK_EQ(s.Confirmed().speed[1], 0.0f);
    CHECK(s.Ack(ChangeType::RunSpeed, c, Speed(7.0f), 6).empty());
    CHECK(s.LastAck() == AckResult::Tombstone);
    CHECK_EQ(s.Counters().epochs, 1u);
}

TEST(MotionState_mode_switch_retires_pending_and_a_server_driven_unit_confirms_its_desired_state)
{
    State s(Mode::ClientDriven, Off(), Kinematics());
    s.Apply(SpeedChange(1, 7.0f), 0);
    s.SetMode(Mode::ServerDriven, 1);
    CHECK(s.GetMode() == Mode::ServerDriven);
    CHECK_EQ(s.Pending().Size(), size_t(0));
    CHECK(s.Confirmed() == s.Desired());
    CHECK_EQ(s.Confirmed().speed[1], 7.0f);
    CHECK_EQ(s.Counters().modeChanges, 1u);
    s.SetMode(Mode::ServerDriven, 2);                  // same mode: nothing
    CHECK_EQ(s.Counters().modeChanges, 1u);
    CHECK_EQ(s.Pending().Epoch(), 1u);
    s.SetMode(Mode::ClientDriven, 3);
    CHECK_EQ(s.Pending().Epoch(), 2u);
    std::vector<Emission> e = s.Apply(SpeedChange(1, 8.0f), 4);
    REQUIRE(e.size() == 1);
    CHECK_EQ(e[0].counter, 1u);                        // counters keep counting across modes
}

TEST(MotionState_timeouts_reissue_the_mover_packet_and_a_kick_is_reported)
{
    State s(Mode::ClientDriven, Enforcing(), Kinematics());
    std::vector<Emission> e = s.Apply(SpeedChange(1, 7.0f), 0);
    const uint32 c0 = e[0].counter;
    CHECK(s.Tick(999).empty());
    e = s.Tick(1000);
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == EmissionKind::Mover);
    CHECK_EQ(e[0].opcode, uint16(SMSG_MOVE_SET_RUN_SPEED));
    CHECK_EQ(e[0].counter, c0 + 1);
    CHECK_EQ(e[0].change.value, 7.0f);
    e = s.Tick(2000);                                  // the resync: one resend per pending entry
    REQUIRE(e.size() == 1);
    CHECK_EQ(e[0].counter, c0 + 2);
    e = s.Tick(3000);
    REQUIRE(e.size() == 1);
    CHECK_EQ(e[0].counter, c0 + 3);
    CHECK(!s.KickRequested());
    e = s.Tick(4000);
    CHECK(e.empty());
    CHECK(s.KickRequested());
    CHECK_EQ(s.Pending().Size(), size_t(0));
    CHECK_EQ(s.Counters().kicks, 1u);
    s.NewEpoch(5000);
    CHECK(!s.KickRequested());
}
