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

#include "Move/Schedule.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

using Move::Due;
using Move::Fired;
using Move::Schedule;

namespace
{
    std::vector<Due> Batch(std::initializer_list<uint32_t> whens)
    {
        std::vector<Due> out;
        uint32_t tag = 1;
        for (const uint32_t when : whens)
        {
            Due d;
            d.when = when;
            d.what = tag++;
            out.push_back(d);
        }
        return out;
    }

    void Arm(Schedule& s, uint64_t who, const std::vector<Due>& batch)
    {
        s.Arm(who, batch.data(), batch.size());
    }

    /// Everything due at or before `now`, in the order it comes out.
    std::vector<Fired> Drain(Schedule& s, uint32_t now)
    {
        std::vector<Fired> out;
        Fired f;
        while (s.Next(now, f))
        {
            out.push_back(f);
        }
        return out;
    }
}

TEST(AnEmptyScheduleHasNothingDueAndNothingEarliest)
{
    Schedule s;
    Fired f;
    CHECK(!s.Next(1000, f));

    uint32_t when = 0;
    CHECK(!s.Earliest(when));
    CHECK_EQ(s.Held(), size_t(0));
    CHECK_EQ(s.Armed(), size_t(0));
}

TEST(NothingFiresBeforeItIsDue)
{
    Schedule s;
    Arm(s, 7, Batch({5000}));

    Fired f;
    CHECK(!s.Next(4999, f));
    CHECK(s.Next(5000, f));
    CHECK_EQ(f.who, uint64_t(7));
    CHECK_EQ(f.when, uint32_t(5000));
}

TEST(ABatchComesOutInTimeOrderHoweverItWasArmed)
{
    Schedule s;
    Arm(s, 1, Batch({3000, 1000, 2000}));

    const std::vector<Fired> fired = Drain(s, 9000);
    REQUIRE(fired.size() == 3u);
    CHECK_EQ(fired[0].when, uint32_t(1000));
    CHECK_EQ(fired[1].when, uint32_t(2000));
    CHECK_EQ(fired[2].when, uint32_t(3000));
}

// Two events at the same instant -- a crossing and the arrival it coincides with -- keep
// the order they were armed in, so a caller never sees an arrival before the crossing
// that led to it.
TEST(EventsAtTheSameInstantKeepTheOrderTheyWereArmedIn)
{
    Schedule s;
    Arm(s, 1, Batch({4000, 4000, 4000}));

    const std::vector<Fired> fired = Drain(s, 4000);
    REQUIRE(fired.size() == 3u);
    CHECK_EQ(fired[0].what, uint32_t(1));
    CHECK_EQ(fired[1].what, uint32_t(2));
    CHECK_EQ(fired[2].what, uint32_t(3));
}

TEST(SeveralMoversInterleaveByTime)
{
    Schedule s;
    Arm(s, 10, Batch({1000, 5000}));
    Arm(s, 20, Batch({2000, 3000}));
    CHECK_EQ(s.Armed(), size_t(2));

    const std::vector<Fired> fired = Drain(s, 9000);
    REQUIRE(fired.size() == 4u);
    CHECK_EQ(fired[0].who, uint64_t(10));
    CHECK_EQ(fired[1].who, uint64_t(20));
    CHECK_EQ(fired[2].who, uint64_t(20));
    CHECK_EQ(fired[3].who, uint64_t(10));
}

// Re-aiming is what a chase does constantly: the new batch replaces the old one whole, and
// nothing from the old one may survive to fire.
TEST(ArmingAgainVoidsEverythingArmedBefore)
{
    Schedule s;
    Arm(s, 5, Batch({1000, 2000, 3000}));
    Arm(s, 5, Batch({8000}));

    const std::vector<Fired> fired = Drain(s, 9000);
    REQUIRE(fired.size() == 1u);
    CHECK_EQ(fired[0].when, uint32_t(8000));
}

TEST(DisarmingLeavesNothingToFire)
{
    Schedule s;
    Arm(s, 5, Batch({1000, 2000}));
    Arm(s, 6, Batch({1500}));
    s.Disarm(5);
    CHECK_EQ(s.Armed(), size_t(1));

    const std::vector<Fired> fired = Drain(s, 9000);
    REQUIRE(fired.size() == 1u);
    CHECK_EQ(fired[0].who, uint64_t(6));
}

TEST(AnEmptyBatchIsTheSameAsDisarming)
{
    Schedule s;
    Arm(s, 5, Batch({1000}));
    s.Arm(5, nullptr, 0);

    Fired f;
    CHECK(!s.Next(9000, f));
    CHECK_EQ(s.Armed(), size_t(0));
}

// A mover is forgotten the moment its last event fires. Without that, a map would
// accumulate one entry per unit that ever moved.
TEST(AMoverIsForgottenWhenItsLastEventFires)
{
    Schedule s;
    Arm(s, 5, Batch({1000, 2000}));
    CHECK_EQ(s.Armed(), size_t(1));

    Fired f;
    REQUIRE(s.Next(1000, f));
    CHECK_EQ(s.Armed(), size_t(1));      // one still to come
    REQUIRE(s.Next(2000, f));
    CHECK_EQ(s.Armed(), size_t(0));
    CHECK_EQ(s.Held(), size_t(0));
}

// The cheap question a quiet map asks every tick: is anything due at all? It must never
// report a time belonging to a batch that has been replaced or disarmed.
TEST(EarliestSkipsWhatIsNoLongerArmed)
{
    Schedule s;
    Arm(s, 5, Batch({1000}));
    Arm(s, 6, Batch({4000}));

    uint32_t when = 0;
    REQUIRE(s.Earliest(when));
    CHECK_EQ(when, uint32_t(1000));

    s.Disarm(5);
    REQUIRE(s.Earliest(when));
    CHECK_EQ(when, uint32_t(4000));

    s.Disarm(6);
    CHECK(!s.Earliest(when));
}

// The clock wraps every 49 days. A batch armed just before the wrap has to keep its order
// across it, or a mover freezes until the clock comes round again.
TEST(TheQueueSurvivesTheServerClockWrapping)
{
    const uint32_t beforeWrap = 0xFFFFFC18u;      // 1000 ms short of zero

    Schedule s;
    Arm(s, 1, Batch({beforeWrap + 1500u, beforeWrap + 500u}));

    Fired f;
    CHECK(!s.Next(beforeWrap, f));

    REQUIRE(s.Next(beforeWrap + 500u, f));
    CHECK_EQ(f.when, uint32_t(beforeWrap + 500u));

    // Now past zero.
    REQUIRE(s.Next(600u, f));
    CHECK_EQ(f.when, uint32_t(beforeWrap + 1500u));
}

// Disarming does not remove entries from the middle of the heap -- that costs as much as
// rebuilding it -- so the dead ones are skipped when they surface. Left alone that grows
// without bound under constant re-aiming, which is exactly what a chase does.
TEST(ConstantReAimingDoesNotGrowTheQueueWithoutBound)
{
    Schedule s;
    for (uint32_t round = 0; round < 500; ++round)
    {
        for (uint64_t who = 1; who <= 20; ++who)
        {
            Arm(s, who, Batch({round * 100u + 5000u, round * 100u + 6000u}));
        }
    }

    CHECK_EQ(s.Armed(), size_t(20));
    // Twenty movers with two events each are live; the rest is slack the compaction keeps
    // bounded. A hard number here would be asserting the threshold rather than the property.
    CHECK(s.Held() < size_t(200));
}

TEST(EverythingArmedStillFiresAfterCompactionHasRun)
{
    Schedule s;
    for (uint64_t who = 1; who <= 40; ++who)
    {
        Arm(s, who, Batch({1000, 2000}));
    }
    for (uint64_t who = 1; who <= 39; ++who)
    {
        s.Disarm(who);
    }

    CHECK_EQ(s.Armed(), size_t(1));
    const std::vector<Fired> fired = Drain(s, 9000);
    REQUIRE(fired.size() == 2u);
    CHECK_EQ(fired[0].who, uint64_t(40));
    CHECK_EQ(fired[1].who, uint64_t(40));
}

TEST(ClearEmptiesEverything)
{
    Schedule s;
    Arm(s, 1, Batch({1000}));
    Arm(s, 2, Batch({2000}));
    s.Clear();

    Fired f;
    CHECK(!s.Next(9000, f));
    CHECK_EQ(s.Held(), size_t(0));
    CHECK_EQ(s.Armed(), size_t(0));
}

// The shape a tick actually has: drain what is due, then look at what is next. A mover
// whose events are all in the future contributes one comparison and nothing more.
TEST(ATickDrainsWhatIsDueAndLearnsWhenToLookAgain)
{
    Schedule s;
    Arm(s, 1, Batch({1000, 4000}));
    Arm(s, 2, Batch({2500}));

    CHECK_EQ(Drain(s, 1200).size(), size_t(1));

    uint32_t when = 0;
    REQUIRE(s.Earliest(when));
    CHECK_EQ(when, uint32_t(2500));

    CHECK_EQ(Drain(s, 2500).size(), size_t(1));
    REQUIRE(s.Earliest(when));
    CHECK_EQ(when, uint32_t(4000));

    CHECK_EQ(Drain(s, 4000).size(), size_t(1));
    CHECK(!s.Earliest(when));
}
