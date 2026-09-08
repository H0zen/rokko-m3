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

// The protocol peer's pure parts: the units the loadtest client feeds with a
// clock value and sends the output of. Nothing here opens a socket.

#include "TestHarness.h"

#include "Peer.hpp"
#include "TimeSync.hpp"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "wire/MovementSequences.h"

#include <cmath>

TEST(TimeSync_answers_the_request_with_the_counter_and_the_client_clock)
{
    WorldPacket request(SMSG_TIME_SYNC_REQ, 4);
    request << uint32(7);

    uint32 counter = 0;
    REQUIRE(loadtest::ReadTimeSyncRequest(request, counter));
    CHECK_EQ(counter, uint32(7));

    const WorldPacket reply = loadtest::MakeTimeSyncResponse(counter, 123456);
    CHECK_EQ(int(reply.GetOpcode()), int(CMSG_TIME_SYNC_RESP));
    CHECK_BYTES(reply.contents(), reply.size(), { 0x07, 0x00, 0x00, 0x00, 0x40, 0xE2, 0x01, 0x00 });
}

TEST(TimeSync_refuses_a_short_request)
{
    WorldPacket request(SMSG_TIME_SYNC_REQ, 2);
    request << uint16(7);
    uint32 counter = 99;
    CHECK(!loadtest::ReadTimeSyncRequest(request, counter));
    CHECK_EQ(counter, uint32(99));
}

TEST(ClientClock_starts_at_its_base_and_never_runs_backwards)
{
    loadtest::ClientClock clock(100000);
    const uint32 a = clock.Ticks();
    const uint32 b = clock.Ticks();
    CHECK(a >= 100000);
    CHECK(b >= a);
    CHECK(b - a < 1000);
}

TEST(PeerReport_defaults_are_empty)
{
    loadtest::PeerReport r;
    CHECK_EQ(r.timeSyncsAnswered, uint32(0));
    CHECK_EQ(r.walkStarts, uint32(0));
    CHECK_EQ(r.observedTarget, uint32(0));
    CHECK(r.unregisteredChanges.empty());
    CHECK_EQ(r.acksSent, uint32(0));
}

#include "Walker.hpp"
#include "wire/MovementCodec.h"
#include "wire/MovementSequences.h"

namespace
{
    Wire::MovementStatus DecodeWalk(WorldPacket& packet)
    {
        Wire::MovementStatus s;
        const Wire::DecodeResult r = Wire::Decode(packet, Wire::SequenceFor(packet.GetOpcode()), s);
        CHECK(r.ok());
        return s;
    }
}

TEST(Walker_reports_a_walk_the_way_the_client_does)
{
    // Two seconds due east at 7 yd/s with a half-second heartbeat: one start,
    // three heartbeats, one stop; the reported position advances 3.5 yd per
    // heartbeat and the packets decode with the same layouts the server reads.
    loadtest::WalkScript script;
    script.seconds = 2;
    script.leadMs = 0;
    script.headingSet = true;
    script.heading = 0.0f;
    Wire::Vec4 start;
    start.x = 100.0f;
    start.y = 200.0f;
    start.z = 30.0f;
    start.o = 1.0f;
    loadtest::Walker walker(script, 0x0000000000000123ull, start);

    CHECK(!walker.Started());
    std::vector<WorldPacket> out = walker.Advance(1000);
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_START_FORWARD));
    Wire::MovementStatus s = DecodeWalk(out[0]);
    CHECK_EQ(s.guid, uint64(0x123));
    CHECK_EQ(s.flags, uint32(0x00000001));   // MOVEFLAG_FORWARD
    CHECK_EQ(s.time, uint32(1000));
    CHECK_EQ(s.pos.x, 100.0f);
    CHECK_EQ(s.pos.o, 0.0f);                 // the scripted heading, not the facing
    CHECK(walker.Started());

    CHECK(walker.Advance(1200).empty());     // nothing due yet
    out = walker.Advance(1500);
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(MSG_MOVE_HEARTBEAT));
    s = DecodeWalk(out[0]);
    CHECK_EQ(s.pos.x, 103.5f);
    CHECK_EQ(s.pos.y, 200.0f);
    CHECK_EQ(s.flags, uint32(0x00000001));

    out = walker.Advance(2000);
    REQUIRE(out.size() == 1);
    CHECK_EQ(DecodeWalk(out[0]).pos.x, 107.0f);
    out = walker.Advance(2500);
    REQUIRE(out.size() == 1);
    CHECK_EQ(DecodeWalk(out[0]).pos.x, 110.5f);

    out = walker.Advance(3000);              // two seconds are up
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_STOP));
    s = DecodeWalk(out[0]);
    CHECK_EQ(s.flags, uint32(0));
    CHECK_EQ(s.pos.x, 114.0f);
    CHECK(walker.Done());
    CHECK_EQ(walker.Position().x, 114.0f);

    CHECK(walker.Advance(3500).empty());     // done means silent
    CHECK_EQ(walker.Starts(), uint32(1));
    CHECK_EQ(walker.Heartbeats(), uint32(3));
    CHECK_EQ(walker.Stops(), uint32(1));
    CHECK_EQ(walker.Relocations(), uint32(0));
}

TEST(Walker_counts_the_legs_a_teleport_cut_short)
{
    // The arithmetic the walk verdict rests on: a teleport landing mid-leg ends
    // that leg where it stands, sends NO stop for it, and opens a fresh leg with
    // a fresh start. So one interrupted leg means starts == legs + 1 and stops
    // == legs -- which reads as a lost walk unless the relocations are counted.
    loadtest::WalkScript script;
    script.seconds = 2;
    script.leadMs = 0;
    script.headingSet = true;
    script.heading = 0.0f;
    Wire::Vec4 start;
    start.x = 100.0f;
    loadtest::Walker walker(script, 0x42, start);

    std::vector<WorldPacket> out = walker.Advance(1000);
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_START_FORWARD));

    Wire::Vec4 landed;
    landed.x = 500.0f;
    walker.Relocate(landed);                 // the server carried the mover away
    CHECK_EQ(walker.Relocations(), uint32(1));
    CHECK(!walker.Started());
    CHECK_EQ(walker.Stops(), uint32(0));     // no stop is owed from the old place

    out = walker.Advance(2000);              // a fresh leg from where it landed
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_START_FORWARD));
    CHECK_EQ(DecodeWalk(out[0]).pos.x, 500.0f);

    out = walker.Advance(4000);              // the restarted leg's two seconds are up
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_STOP));
    CHECK(walker.Done());
    // legs == 1: starts == legs + relocations, stops == legs.
    CHECK_EQ(walker.Starts(), uint32(2));
    CHECK_EQ(walker.Stops(), uint32(1));
    CHECK_EQ(walker.Relocations(), uint32(1));
}

TEST(Walker_does_not_count_a_teleport_that_lands_between_legs)
{
    // Relocating a walker that has no leg running costs no extra start and skips
    // no stop, so it must not be counted: it would break the very arithmetic the
    // count exists to keep straight.
    loadtest::WalkScript script;
    script.seconds = 1;
    script.leadMs = 3000;
    Wire::Vec4 start;
    loadtest::Walker walker(script, 0x42, start);

    Wire::Vec4 landed;
    landed.x = 7.0f;
    walker.Relocate(landed);                 // before the first leg opens
    CHECK_EQ(walker.Relocations(), uint32(0));
    CHECK_EQ(walker.Position().x, 7.0f);     // it still moved the mover

    std::vector<WorldPacket> out = walker.Advance(0);   // Relocate cleared the lead
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_START_FORWARD));
    out = walker.Advance(1000);
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_STOP));
    CHECK(walker.Done());
    walker.Relocate(landed);                 // and after the last leg closes
    CHECK_EQ(walker.Relocations(), uint32(0));
    CHECK_EQ(walker.Starts(), uint32(1));
    CHECK_EQ(walker.Stops(), uint32(1));
}

TEST(Walker_waits_out_its_lead_and_uses_the_facing_by_default)
{
    loadtest::WalkScript script;
    script.seconds = 1;
    script.leadMs = 3000;
    Wire::Vec4 start;
    start.o = 1.5f;
    loadtest::Walker walker(script, 0x42, start);

    CHECK(walker.Advance(0).empty());
    CHECK(walker.Advance(2999).empty());
    CHECK(!walker.Started());
    std::vector<WorldPacket> out = walker.Advance(3000);
    REQUIRE(out.size() == 1);
    CHECK_EQ(DecodeWalk(out[0]).pos.o, 1.5f);
}

TEST(Walker_with_no_seconds_never_moves)
{
    loadtest::WalkScript script;   // seconds = 0
    loadtest::Walker walker(script, 0x42, Wire::Vec4());
    CHECK(walker.Advance(10000).empty());
    CHECK(!walker.Started());
    CHECK(!walker.Done());
}

#include "AckEngine.hpp"
#include "wire/MovementFamilies.h"

namespace
{
    // A change and its ack sharing one tiny layout that carries the counter --
    // the real ack layouts arrive in P1 and plug into the same engine through
    // the registry; here the lookup is injected so the engine can be pinned now.
    const uint16 kFakeChange = 0x1001;
    const uint16 kFakeAck    = 0x1002;
    const Wire::Element kCounterLayout[] =
    {
        Wire::Element::MovementCounter, Wire::Element::PositionX, Wire::Element::End
    };

    Wire::Sequence FakeLookup(uint16 opcode)
    {
        return (opcode == kFakeChange || opcode == kFakeAck) ? kCounterLayout : nullptr;
    }

    WorldPacket FakeChange(uint32 counter)
    {
        Wire::MovementStatus s;
        s.counter = counter;
        s.pos.x = 1.0f;
        WorldPacket p(kFakeChange, 8);
        Wire::Encode(p, kCounterLayout, s);
        return p;
    }

    std::vector<loadtest::ChangePair> FakePairs()
    {
        std::vector<loadtest::ChangePair> pairs;
        loadtest::ChangePair pair;
        pair.change = kFakeChange;
        pair.ack = kFakeAck;
        pairs.push_back(pair);
        return pairs;
    }

    uint32 CounterOf(WorldPacket& ack)
    {
        Wire::MovementStatus s;
        CHECK(Wire::Decode(ack, kCounterLayout, s).ok());
        return s.counter;
    }
}

TEST(AckEngine_answers_at_once_with_the_counter_echoed)
{
    loadtest::AckPolicy policy;
    loadtest::AckEngine engine(policy, &FakeLookup, FakePairs());
    CHECK(engine.IsChange(kFakeChange));
    CHECK(!engine.IsChange(kFakeAck));

    REQUIRE(engine.Plan(FakeChange(7), 1000));
    std::vector<WorldPacket> due = engine.Due(1000);
    REQUIRE(due.size() == 1);
    CHECK_EQ(int(due[0].GetOpcode()), int(kFakeAck));
    CHECK_EQ(CounterOf(due[0]), uint32(7));
    CHECK_EQ(engine.Sent(), uint32(1));
    CHECK(engine.Due(1000).empty());
}

TEST(AckEngine_delays_when_told_to)
{
    loadtest::AckPolicy policy;
    policy.mode = loadtest::AckMode::Delay;
    policy.delayMs = 250;
    loadtest::AckEngine engine(policy, &FakeLookup, FakePairs());

    REQUIRE(engine.Plan(FakeChange(3), 1000));
    CHECK(engine.Due(1000).empty());
    CHECK(engine.Due(1249).empty());
    std::vector<WorldPacket> due = engine.Due(1250);
    REQUIRE(due.size() == 1);
    CHECK_EQ(CounterOf(due[0]), uint32(3));
}

TEST(AckEngine_reports_what_is_still_pending)
{
    // A delayed ack that has not gone out yet is a fact the run must report: a
    // hold that ends before it is due would otherwise look like a clean run.
    loadtest::AckPolicy policy;
    policy.mode = loadtest::AckMode::Delay;
    policy.delayMs = 250;
    loadtest::AckEngine engine(policy, &FakeLookup, FakePairs());
    CHECK_EQ(engine.PendingCount(), uint32(0));
    REQUIRE(engine.Plan(FakeChange(3), 1000));
    CHECK_EQ(engine.PendingCount(), uint32(1));
    CHECK(engine.Due(1100).empty());
    CHECK_EQ(engine.PendingCount(), uint32(1));
    REQUIRE(engine.Due(1250).size() == 1);
    CHECK_EQ(engine.PendingCount(), uint32(0));
}

TEST(AckEngine_mismatches_the_counter_when_told_to)
{
    loadtest::AckPolicy policy;
    policy.mode = loadtest::AckMode::Mismatch;
    loadtest::AckEngine engine(policy, &FakeLookup, FakePairs());

    REQUIRE(engine.Plan(FakeChange(9), 1000));
    std::vector<WorldPacket> due = engine.Due(1000);
    REQUIRE(due.size() == 1);
    CHECK_EQ(CounterOf(due[0]), uint32(10));
}

TEST(AckEngine_drops_when_told_to_and_counts_it)
{
    loadtest::AckPolicy policy;
    policy.mode = loadtest::AckMode::Drop;
    loadtest::AckEngine engine(policy, &FakeLookup, FakePairs());

    REQUIRE(engine.Plan(FakeChange(1), 1000));
    CHECK(engine.Due(5000).empty());
    CHECK_EQ(engine.Dropped(), uint32(1));
    CHECK_EQ(engine.Sent(), uint32(0));
}

TEST(AckEngine_counts_a_change_whose_layout_is_not_registered)
{
    loadtest::AckPolicy policy;
    loadtest::AckEngine engine(policy, [](uint16) -> Wire::Sequence { return nullptr; }, FakePairs());

    CHECK(!engine.Plan(FakeChange(1), 1000));
    CHECK(engine.Due(1000).empty());
    REQUIRE(engine.Unregistered().count(kFakeChange) == 1);
    CHECK_EQ(engine.Unregistered().at(kFakeChange), uint32(1));
}

TEST(AckEngine_knows_the_real_pairs)
{
    // The table itself is data; this pins that it names real opcodes on both
    // sides and that speed changes map to the force-change acks.
    const std::vector<loadtest::ChangePair>& pairs = loadtest::KnownChangePairs();
    CHECK(pairs.size() >= 20);
    bool foundRun = false;
    for (const loadtest::ChangePair& p : pairs)
    {
        CHECK(p.change != 0);
        CHECK(p.ack != 0);
        if (p.change == SMSG_MOVE_SET_RUN_SPEED)
        {
            foundRun = (p.ack == CMSG_FORCE_RUN_SPEED_CHANGE_ACK);
        }
    }
    CHECK(foundRun);
}

TEST(Walker_returns_home_when_told_to)
{
    loadtest::WalkScript script;
    script.seconds = 1;
    script.leadMs = 0;
    script.headingSet = true;
    script.heading = 0.0f;
    script.returnHome = true;
    Wire::Vec4 start;
    start.x = 50.0f;
    start.y = 60.0f;
    loadtest::Walker walker(script, 0x42, start);

    std::vector<WorldPacket> out = walker.Advance(0);      // start, outbound
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_START_FORWARD));
    out = walker.Advance(500);                              // heartbeat at x = 53.5
    REQUIRE(out.size() == 1);
    out = walker.Advance(1000);                             // stop outbound at x = 57
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_STOP));
    CHECK_EQ(DecodeWalk(out[0]).pos.x, 57.0f);
    CHECK(!walker.Done());

    out = walker.Advance(1000);                             // start the return leg at once
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_START_FORWARD));
    Wire::MovementStatus s = DecodeWalk(out[0]);
    CHECK(std::fabs(s.pos.o - 3.14159265f) < 0.001f);
    out = walker.Advance(1500);                             // heartbeat, heading back
    REQUIRE(out.size() == 1);
    out = walker.Advance(2000);                             // stop: home again
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_STOP));
    s = DecodeWalk(out[0]);
    CHECK(std::fabs(s.pos.x - 50.0f) < 0.01f);
    CHECK(std::fabs(s.pos.y - 60.0f) < 0.01f);
    CHECK(walker.Done());
    CHECK_EQ(walker.Starts(), uint32(2));
    CHECK_EQ(walker.Stops(), uint32(2));
    CHECK_EQ(walker.Heartbeats(), uint32(2));
    CHECK_EQ(walker.LastStampedTime(), uint32(2000));
}

TEST(Walker_clamps_each_leg_so_a_late_poll_still_ends_at_home)
{
    // The Serve loop polls, so a leg's end is seen late; the walk must stop
    // where the leg's time ran out, not where the late poll would put it, or
    // the two legs differ in length and "home" drifts by a run's overshoot.
    loadtest::WalkScript script;
    script.seconds = 1;
    script.leadMs = 0;
    script.headingSet = true;
    script.heading = 0.0f;
    script.returnHome = true;
    Wire::Vec4 start;
    start.x = 50.0f;
    start.y = 60.0f;
    loadtest::Walker walker(script, 0x42, start);

    std::vector<WorldPacket> out = walker.Advance(0);      // start, outbound
    REQUIRE(out.size() == 1);
    out = walker.Advance(700);                              // heartbeat at x = 54.9
    REQUIRE(out.size() == 1);
    CHECK(std::fabs(DecodeWalk(out[0]).pos.x - 54.9f) < 0.01f);
    out = walker.Advance(1300);                             // 300 ms late: stops at x = 57 all the same
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_STOP));
    CHECK(std::fabs(DecodeWalk(out[0]).pos.x - 57.0f) < 0.01f);

    out = walker.Advance(1300);                             // the return leg starts at once
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_START_FORWARD));
    out = walker.Advance(1900);                             // heartbeat, heading back
    REQUIRE(out.size() == 1);
    out = walker.Advance(2450);                             // 150 ms late: home exactly
    REQUIRE(out.size() == 1);
    CHECK_EQ(int(out[0].GetOpcode()), int(CMSG_MOVE_STOP));
    const Wire::MovementStatus s = DecodeWalk(out[0]);
    CHECK_EQ(s.pos.x, 50.0f);
    CHECK_EQ(s.pos.y, 60.0f);
    CHECK(walker.Done());
}

TEST(AckEngine_stale_acks_the_previous_counter)
{
    loadtest::AckPolicy policy;
    policy.mode = loadtest::AckMode::Stale;
    loadtest::AckEngine engine(policy, &FakeLookup, FakePairs());

    REQUIRE(engine.Plan(FakeChange(9), 1000));
    std::vector<WorldPacket> due = engine.Due(1000);
    REQUIRE(due.size() == 1);
    CHECK_EQ(CounterOf(due[0]), uint32(8));
}

TEST(AckEngine_counts_a_registered_change_that_fails_to_decode_apart)
{
    loadtest::AckPolicy policy;
    loadtest::AckEngine engine(policy, &FakeLookup, FakePairs());

    // The layout is registered, but the packet is one byte short.
    WorldPacket whole = FakeChange(1);
    WorldPacket cut(kFakeChange, 8);
    cut.append(whole.contents(), whole.size() - 1);
    CHECK(!engine.Plan(cut, 1000));
    CHECK(engine.Unregistered().empty());
    REQUIRE(engine.DecodeFailures().count(kFakeChange) == 1);
    CHECK_EQ(engine.DecodeFailures().at(kFakeChange), uint32(1));
}

namespace
{
    const Wire::Element kTimedLayout[] =
    {
        Wire::Element::HasTimestamp, Wire::Element::Timestamp,
        Wire::Element::MovementCounter, Wire::Element::PositionX, Wire::Element::End
    };

    Wire::Sequence TimedLookup(uint16 opcode)
    {
        return (opcode == kFakeChange || opcode == kFakeAck) ? kTimedLayout : nullptr;
    }
}

TEST(AckEngine_stamps_the_ack_with_the_client_clock_not_the_servers_time)
{
    loadtest::AckPolicy policy;
    loadtest::AckEngine engine(policy, &TimedLookup, FakePairs());

    Wire::MovementStatus s;
    s.has.timestamp = true;
    s.time = 5;              // what the server stamped on the change
    s.counter = 7;
    s.pos.x = 1.0f;
    WorldPacket change(kFakeChange, 16);
    Wire::Encode(change, kTimedLayout, s);

    REQUIRE(engine.Plan(change, 1000));
    std::vector<WorldPacket> due = engine.Due(1234);
    REQUIRE(due.size() == 1);
    Wire::MovementStatus got;
    REQUIRE(Wire::Decode(due[0], kTimedLayout, got).ok());
    CHECK_EQ(got.time, uint32(1234));
    CHECK_EQ(got.counter, uint32(7));
}

TEST(AckEngine_answers_a_real_speed_change_from_the_registry_layouts)
{
    // A SET carries the mover's guid, a counter and the new speed; the ack the
    // client returns carries the mover's whole status plus that counter and
    // speed. The engine is told the mover's status and echoes the rest.
    loadtest::AckPolicy policy;
    loadtest::AckEngine engine(policy, [](uint16 op) { return Wire::SequenceFor(op); });
    Wire::MovementStatus mover;
    mover.guid = 0x42;
    mover.pos.x = 1.0f; mover.pos.y = 2.0f; mover.pos.z = 3.0f; mover.pos.o = 0.5f;
    engine.SetMover(mover);

    Wire::MovementStatus change;
    change.guid = 0x42;
    change.counter = 5;
    change.value = 14.0f;
    WorldPacket set(SMSG_MOVE_SET_RUN_SPEED, 32);
    Wire::Encode(set, Wire::SequenceFor(SMSG_MOVE_SET_RUN_SPEED), change);

    REQUIRE(engine.IsChange(SMSG_MOVE_SET_RUN_SPEED));
    REQUIRE(engine.Plan(set, 1000));
    std::vector<WorldPacket> due = engine.Due(1000);
    REQUIRE(due.size() == 1);
    CHECK_EQ(int(due[0].GetOpcode()), int(CMSG_FORCE_RUN_SPEED_CHANGE_ACK));
    Wire::MovementStatus ack;
    REQUIRE(Wire::Decode(due[0], Wire::SequenceFor(CMSG_FORCE_RUN_SPEED_CHANGE_ACK), ack).ok());
    CHECK_EQ(ack.guid, uint64(0x42));
    CHECK_EQ(ack.counter, uint32(5));
    CHECK_EQ(ack.value, 14.0f);
    CHECK_EQ(ack.pos.x, 1.0f);
    CHECK_EQ(ack.pos.o, 0.5f);
    CHECK_EQ(ack.time, uint32(1000));
    CHECK(engine.Unregistered().empty());
    CHECK(engine.DecodeFailures().empty());
    CHECK_EQ(engine.Sent(), uint32(1));
}

TEST(AckEngine_knows_which_known_pairs_still_lack_a_layout)
{
    // The table is the engine's business only: a change whose status the engine
    // can decode and echo back as an ack. Knock-back and teleport carry no
    // movement status at all, so they are not in it -- they have families, and
    // the peer answers them from those codecs (P1-C). Everything the table does
    // hold has a registry layout for its change; the turn-rate and pitch-rate
    // acks are the only halves no source has a layout for yet.
    for (const loadtest::ChangePair& pair : loadtest::KnownChangePairs())
    {
        CHECK(pair.change != SMSG_MOVE_KNOCK_BACK);
        CHECK(pair.change != SMSG_MOVE_TELEPORT);
        CHECK(Wire::SequenceFor(pair.change) != nullptr);
        if (pair.change == SMSG_MOVE_SET_TURN_RATE || pair.change == SMSG_MOVE_SET_PITCH_RATE)
        {
            CHECK(Wire::SequenceFor(pair.ack) == nullptr);
            continue;
        }
        CHECK(Wire::SequenceFor(pair.ack) != nullptr);
    }
    // And the two that left are answered elsewhere, not forgotten.
    CHECK(Wire::IsFamily(SMSG_MOVE_KNOCK_BACK));
    CHECK(Wire::IsFamily(SMSG_MOVE_TELEPORT));
}

TEST(AckEngine_stamps_the_mover_as_it_stands_when_the_ack_is_sent)
{
    // The change may arrive a tick before the ack goes out; the ack carries the
    // mover's position at the send, not at the decode, so position and time agree.
    loadtest::AckPolicy policy;
    loadtest::AckEngine engine(policy, [](uint16 op) { return Wire::SequenceFor(op); });
    Wire::MovementStatus before;
    before.guid = 0x42;
    before.pos.x = 1.0f;
    engine.SetMover(before);

    Wire::MovementStatus change;
    change.guid = 0x42;
    change.counter = 9;
    change.value = 8.0f;
    WorldPacket set(SMSG_MOVE_SET_RUN_SPEED, 32);
    Wire::Encode(set, Wire::SequenceFor(SMSG_MOVE_SET_RUN_SPEED), change);
    REQUIRE(engine.Plan(set, 1000));

    Wire::MovementStatus after = before;
    after.pos.x = 4.5f;
    engine.SetMover(after);
    std::vector<WorldPacket> due = engine.Due(1050);
    REQUIRE(due.size() == 1);
    Wire::MovementStatus ack;
    REQUIRE(Wire::Decode(due[0], Wire::SequenceFor(CMSG_FORCE_RUN_SPEED_CHANGE_ACK), ack).ok());
    CHECK_EQ(ack.pos.x, 4.5f);
    CHECK_EQ(ack.time, uint32(1050));
    CHECK_EQ(ack.counter, uint32(9));
    CHECK_EQ(ack.value, 8.0f);
}

TEST(Walker_reports_its_status_without_a_timestamp)
{
    loadtest::WalkScript script;
    script.seconds = 1;
    script.leadMs = 0;
    script.headingSet = true;
    script.heading = 0.0f;
    Wire::Vec4 start;
    start.x = 10.0f;
    loadtest::Walker walker(script, 0x42, start);
    Wire::MovementStatus s = walker.Status();
    CHECK_EQ(s.guid, uint64(0x42));
    CHECK_EQ(s.flags, uint32(0));            // not walking yet
    CHECK_EQ(s.pos.x, 10.0f);
    CHECK_EQ(s.pos.o, 0.0f);
    walker.Advance(0);                       // start
    walker.Advance(500);                     // heartbeat at x = 13.5
    s = walker.Status();
    CHECK_EQ(s.flags, uint32(0x00000001));   // MOVEFLAG_FORWARD
    CHECK_EQ(s.pos.x, 13.5f);
    walker.Advance(1000);                    // stop
    CHECK_EQ(walker.Status().flags, uint32(0));
}
