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

#include "PendingChanges.h"

#include <limits>

using namespace Motion;

namespace
{
    TimeoutPolicy Enforcing()
    {
        TimeoutPolicy p;
        p.timeoutMs = 1000;
        p.maxResends = 1;
        p.maxResyncs = 1;
        p.tombstoneTtlMs = 10000;
        return p;
    }
    AckPayload Speed(float v) { AckPayload a; a.hasValue = true; a.value = v; return a; }
    AckPayload Flag() { AckPayload a; a.hasValue = false; a.value = 0.0f; return a; }
}

TEST(PendingChanges_opens_with_increasing_counters_and_one_pending_per_type)
{
    PendingChanges pending((TimeoutPolicy()));
    CHECK_EQ(pending.Open(SpeedChange(1, 7.0f), 1000), 0u);
    CHECK_EQ(pending.Open(SpeedChange(3, 4.7f), 1000), 1u);
    CHECK_EQ(pending.Open(SpeedChange(1, 8.0f), 1001), 2u);   // supersedes the run speed
    CHECK_EQ(pending.Size(), size_t(2));
    REQUIRE(pending.Get(ChangeType::RunSpeed) != NULL);
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->counter, 2u);
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->change.value, 8.0f);
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->sentAt, 1001u);
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->epoch, 0u);
    CHECK_EQ(pending.Tombstones(), size_t(1));
    CHECK_EQ(pending.Counters().opened, 3u);
    CHECK_EQ(pending.Counters().superseded, 1u);
    CHECK_EQ(pending.NextCounter(), 3u);
    CHECK(!pending.Has(ChangeType::Root));
}

TEST(PendingChanges_ack_matches_by_type_and_counter_and_checks_the_payload)
{
    PendingChanges pending((TimeoutPolicy()));
    const uint32 c = pending.Open(SpeedChange(1, 7.0f), 0);
    AckOutcome out = pending.Ack(ChangeType::RunSpeed, c, Speed(7.0f), 10);
    CHECK(out.result == AckResult::Matched);
    CHECK_EQ(out.change.counter, c);
    CHECK_EQ(out.change.change.value, 7.0f);
    CHECK_EQ(pending.Size(), size_t(0));

    const uint32 c2 = pending.Open(SpeedChange(1, 8.0f), 20);
    out = pending.Ack(ChangeType::RunSpeed, c2, Speed(7.5f), 30);
    CHECK(out.result == AckResult::PayloadMismatch);
    CHECK_EQ(pending.Size(), size_t(0));
    CHECK_EQ(pending.Counters().payloadMismatch, 1u);

    const uint32 c3 = pending.Open(FlagChange(ChangeType::Root, true), 40);
    CHECK(pending.Ack(ChangeType::Root, c3, Flag(), 50).result == AckResult::Matched);
    CHECK_EQ(pending.Counters().matched, 2u);
}

TEST(PendingChanges_a_superseded_counter_hits_a_tombstone_then_nothing)
{
    PendingChanges pending((TimeoutPolicy()));
    const uint32 c0 = pending.Open(SpeedChange(1, 7.0f), 0);
    const uint32 c1 = pending.Open(SpeedChange(1, 8.0f), 1);
    CHECK(pending.Ack(ChangeType::RunSpeed, c0, Speed(7.0f), 2).result == AckResult::Tombstone);
    CHECK_EQ(pending.Tombstones(), size_t(0));
    CHECK(pending.Ack(ChangeType::RunSpeed, c0, Speed(7.0f), 3).result == AckResult::Stale);
    CHECK(pending.Ack(ChangeType::RunSpeed, c1, Speed(8.0f), 4).result == AckResult::Matched);
    CHECK_EQ(pending.Counters().tombstone, 1u);
    CHECK_EQ(pending.Counters().stale, 1u);
}

TEST(PendingChanges_new_epoch_retires_everything_to_tombstones_and_keeps_counting)
{
    PendingChanges pending((TimeoutPolicy()));
    const uint32 c0 = pending.Open(SpeedChange(1, 7.0f), 0);
    pending.Open(FlagChange(ChangeType::Root, true), 0);
    pending.NewEpoch(5);
    CHECK_EQ(pending.Epoch(), 1u);
    CHECK_EQ(pending.Size(), size_t(0));
    CHECK_EQ(pending.Tombstones(), size_t(2));
    CHECK_EQ(pending.Counters().retired, 2u);
    CHECK(pending.Ack(ChangeType::RunSpeed, c0, Speed(7.0f), 6).result == AckResult::Tombstone);
    const uint32 c2 = pending.Open(SpeedChange(1, 9.0f), 7);
    CHECK_EQ(c2, 2u);
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->epoch, 1u);
}

TEST(PendingChanges_future_and_unknown_counters_are_told_apart)
{
    PendingChanges pending((TimeoutPolicy()));
    CHECK(pending.Ack(ChangeType::RunSpeed, 5, Speed(1.0f), 0).result == AckResult::Future);
    const uint32 c0 = pending.Open(SpeedChange(1, 7.0f), 0);
    CHECK(pending.Ack(ChangeType::Root, c0, Flag(), 1).result == AckResult::NoPending);
    CHECK(pending.Ack(ChangeType::RunSpeed, c0 + 1, Speed(7.0f), 1).result == AckResult::Future);
    CHECK_EQ(pending.Counters().future, 2u);
    CHECK_EQ(pending.Counters().noPending, 1u);
    CHECK_EQ(pending.Size(), size_t(1));
}

TEST(PendingChanges_timeouts_are_off_by_default)
{
    PendingChanges pending((TimeoutPolicy()));
    CHECK_EQ(pending.Policy().timeoutMs, 0u);
    pending.Open(SpeedChange(1, 7.0f), 0);
    CHECK(pending.Tick(100000).empty());
    CHECK_EQ(pending.Size(), size_t(1));
    CHECK_EQ(pending.Counters().resent, 0u);
}

TEST(PendingChanges_timeout_resends_once_then_resyncs_then_kicks)
{
    PendingChanges pending(Enforcing());
    const uint32 c0 = pending.Open(SpeedChange(1, 7.0f), 0);
    CHECK(pending.Tick(999).empty());

    std::vector<TimeoutEvent> events = pending.Tick(1000);
    REQUIRE(events.size() == 1);
    CHECK(events[0].action == TimeoutAction::Resend);
    CHECK(events[0].type == ChangeType::RunSpeed);
    CHECK_EQ(events[0].oldCounter, c0);
    const uint32 c1 = events[0].newCounter;
    CHECK_EQ(c1, c0 + 1);
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->counter, c1);
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->resends, uint8(1));
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->sentAt, 1000u);
    CHECK(pending.Ack(ChangeType::RunSpeed, c0, Speed(7.0f), 1001).result == AckResult::Tombstone);

    events = pending.Tick(2000);
    REQUIRE(events.size() == 2);
    CHECK(events[0].action == TimeoutAction::Resync);
    CHECK(events[1].action == TimeoutAction::Resend);
    CHECK_EQ(events[1].oldCounter, c1);
    const uint32 c2 = events[1].newCounter;
    CHECK_EQ(pending.Get(ChangeType::RunSpeed)->resends, uint8(0));
    CHECK_EQ(pending.Counters().resynced, 1u);

    events = pending.Tick(3000);
    REQUIRE(events.size() == 1);
    CHECK(events[0].action == TimeoutAction::Resend);
    const uint32 c3 = events[0].newCounter;
    CHECK_EQ(c3, c2 + 1);

    events = pending.Tick(4000);
    REQUIRE(events.size() == 1);
    CHECK(events[0].action == TimeoutAction::Kick);
    CHECK_EQ(events[0].oldCounter, c3);
    CHECK_EQ(pending.Size(), size_t(0));
    CHECK_EQ(pending.Counters().kicked, 1u);
    CHECK_EQ(pending.Counters().resent, 3u);
}

TEST(PendingChanges_a_resync_reissues_every_pending_entry_and_tombstones_expire)
{
    PendingChanges pending(Enforcing());
    pending.Open(SpeedChange(1, 7.0f), 0);
    pending.Open(FlagChange(ChangeType::Root, true), 500);
    std::vector<TimeoutEvent> events = pending.Tick(1000);   // the run speed is late; the root is not
    REQUIRE(events.size() == 1);
    events = pending.Tick(2000);                             // both late; the run speed has spent its resend
    REQUIRE(events.size() == 3);
    CHECK(events[0].action == TimeoutAction::Resync);
    CHECK(events[1].action == TimeoutAction::Resend);
    CHECK(events[2].action == TimeoutAction::Resend);
    CHECK_EQ(pending.All().size(), size_t(2));
    CHECK_EQ(pending.Tombstones(), size_t(3));               // c0 at 1000, then c1 and the root's at 2000
    pending.ExpireTombstones(10999);
    CHECK_EQ(pending.Tombstones(), size_t(3));               // the first dies at 11000
    pending.ExpireTombstones(11000);
    CHECK_EQ(pending.Tombstones(), size_t(2));
    pending.ExpireTombstones(12000);
    CHECK_EQ(pending.Tombstones(), size_t(0));
}

TEST(PendingChanges_a_new_epoch_restores_the_resync_budget)
{
    PendingChanges pending(Enforcing());
    pending.Open(SpeedChange(1, 7.0f), 0);
    REQUIRE(pending.Tick(1000).size() == 1);                     // the resend
    std::vector<TimeoutEvent> events = pending.Tick(2000);       // the resync: budget spent
    REQUIRE(events.size() == 2);
    CHECK(events[0].action == TimeoutAction::Resync);
    pending.NewEpoch(2500);
    pending.Open(SpeedChange(1, 8.0f), 3000);
    REQUIRE(pending.Tick(4000).size() == 1);                     // the resend
    events = pending.Tick(5000);                                 // a resync again, not a kick
    REQUIRE(events.size() == 2);
    CHECK(events[0].action == TimeoutAction::Resync);
    CHECK_EQ(pending.Counters().resynced, 2u);
    CHECK_EQ(pending.Counters().kicked, 0u);
}

TEST(PendingChanges_a_non_finite_payload_is_a_mismatch)
{
    // fabs(NaN - v) > tolerance is false, so a NaN would confirm a change it
    // does not echo. The legacy speed-ack handler has that hole; this does not.
    PendingChanges pending((TimeoutPolicy()));
    const uint32 c0 = pending.Open(SpeedChange(1, 7.0f), 0);
    CHECK(pending.Ack(ChangeType::RunSpeed, c0, Speed(std::numeric_limits<float>::quiet_NaN()), 1).result == AckResult::PayloadMismatch);
    const uint32 c1 = pending.Open(SpeedChange(1, 7.0f), 2);
    CHECK(pending.Ack(ChangeType::RunSpeed, c1, Speed(std::numeric_limits<float>::infinity()), 3).result == AckResult::PayloadMismatch);
    const uint32 c2 = pending.Open(SpeedChange(1, 7.0f), 4);
    CHECK(pending.Ack(ChangeType::RunSpeed, c2, Speed(7.0f), 5).result == AckResult::Matched);
    CHECK_EQ(pending.Counters().payloadMismatch, 2u);
    CHECK_EQ(pending.Counters().matched, 1u);
}

TEST(PendingChanges_an_ack_sweeps_expired_tombstones_first)
{
    // A consumer that never ticks (enforcement off) must not grow tombstones
    // without bound: an ack sweeps the expired ones before it looks for one.
    PendingChanges pending((TimeoutPolicy()));
    const uint32 c0 = pending.Open(SpeedChange(1, 7.0f), 0);
    pending.Open(SpeedChange(1, 8.0f), 1);                                              // c0 becomes a tombstone that dies at 10001
    CHECK_EQ(pending.Tombstones(), size_t(1));
    CHECK(pending.Ack(ChangeType::Root, c0, Flag(), 5000).result == AckResult::NoPending);   // alive, and not this one
    CHECK_EQ(pending.Tombstones(), size_t(1));
    CHECK(pending.Ack(ChangeType::RunSpeed, c0, Speed(7.0f), 10001).result == AckResult::Stale);   // swept before the lookup
    CHECK_EQ(pending.Tombstones(), size_t(0));
}
