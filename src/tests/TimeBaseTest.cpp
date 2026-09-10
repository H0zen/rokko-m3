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

#include "TimeBase.h"

using namespace Motion;

TEST(TimeBase_is_not_acquired_until_the_first_pair_and_rebases_to_server_now_meanwhile)
{
    TimeBase tb;
    CHECK(!tb.Acquired(5000));
    CHECK_EQ(tb.Rebase(123u, 5000u), 5000u);
    CHECK_EQ(tb.Counters().fallbacks, 1u);
    tb.Requested(0, 1000);
    CHECK(tb.Responded(0, 500000, 1100) == SampleResult::Accepted);
    CHECK_EQ(tb.LastRtt(), 100u);
    CHECK_EQ(tb.Delta(), 500000u - 1050u);
    CHECK(tb.Acquired(1100));
    CHECK_EQ(tb.Rebase(500100u, 1200u), 1150u);
    CHECK_EQ(tb.LastSampleAt(), 1100u);
    CHECK_EQ(tb.Counters().samples, 1u);
    CHECK_EQ(tb.Counters().requested, 1u);
}

TEST(TimeBase_wraps_at_the_top_of_the_client_clock)
{
    // The golden the design asks for (6.3): the client's tick counter wraps at
    // 0xFFFFFFFF while the session runs; the server's clock wraps at a
    // different moment. Both sides of both wraps must rebase correctly.
    TimeBase tb;
    tb.Requested(0, 0xFFFFFF00u);
    CHECK(tb.Responded(0, 0x00000010u, 0xFFFFFF40u) == SampleResult::Accepted);   // rtt 0x40; the client's clock has wrapped
    CHECK_EQ(tb.LastRtt(), 0x40u);
    CHECK_EQ(tb.Delta(), 0xF0u);                                                     // 0x10 - 0xFFFFFF20
    CHECK_EQ(tb.Rebase(0x30u, 0xFFFFFF50u), 0xFFFFFF40u);                            // before the server's wrap
    CHECK_EQ(tb.Rebase(0x100u, 0x20u), 0x10u);                                       // after it
    tb.Requested(1, 0x10u);
    CHECK(tb.Responded(1, 0x110u, 0x30u) == SampleResult::Accepted);                 // consistent with the first: delta unchanged
    CHECK_EQ(tb.Delta(), 0xF0u);
}

TEST(TimeBase_slews_small_drift_and_jumps_on_large)
{
    TimeBase tb;
    tb.Requested(0, 1000);
    tb.Responded(0, 11000, 1000);               // rtt 0, delta 10000
    const uint32 d = tb.Delta();
    CHECK_EQ(d, 10000u);
    tb.Requested(1, 2000);
    CHECK(tb.Responded(1, 12040, 2000) == SampleResult::Slewed);   // the client reads 40 ms ahead of the model
    CHECK_EQ(tb.Delta(), d + 5);
    tb.Requested(2, 3000);
    CHECK(tb.Responded(2, 13040, 3000) == SampleResult::Slewed);
    CHECK_EQ(tb.Delta(), d + 10);
    tb.Requested(3, 4000);
    CHECK(tb.Responded(3, 13980, 4000) == SampleResult::Slewed);   // 30 ms behind the model (4000 + 10010 - 30): slews back by 5
    CHECK_EQ(tb.Delta(), d + 5);
    tb.Requested(4, 5000);
    CHECK(tb.Responded(4, 16000, 5000) == SampleResult::Jumped);   // 1000 ms off: a real clock change
    CHECK_EQ(tb.Delta(), 11000u);
    CHECK_EQ(tb.Counters().slewed, 3u);
    CHECK_EQ(tb.Counters().jumped, 1u);
}

TEST(TimeBase_goes_stale_without_samples_and_resets_on_epoch_events)
{
    TimeBase tb;
    tb.Requested(0, 1000);
    tb.Responded(0, 11000, 1000);
    CHECK(tb.Acquired(60999));
    CHECK(!tb.Acquired(61000));
    CHECK_EQ(tb.Rebase(70000u, 61000u), 61000u);     // the fallback while stale
    CHECK_EQ(tb.Counters().fallbacks, 1u);
    tb.Requested(1, 61000);
    CHECK(tb.Responded(1, 71000, 61000) == SampleResult::Accepted);   // a stale base takes the sample outright, no slew
    CHECK(tb.Acquired(61000));
    tb.Reset();
    CHECK(!tb.Acquired(61001));
    CHECK_EQ(tb.Delta(), 0u);
    tb.Requested(2, 62000);
    CHECK(tb.Responded(2, 5000, 62000) == SampleResult::Accepted);     // a wholly different delta, accepted after the reset
    CHECK_EQ(tb.Delta(), uint32(5000u - 62000u));
}

TEST(TimeBase_drops_a_response_older_than_the_sample_age)
{
    // A reply to a request three sync periods old would put the sample point
    // half a minute in the past and jump the delta; it is dropped and counted.
    TimeBase tb;
    tb.Requested(0, 1000);
    CHECK(tb.Responded(0, 500000, 1000 + 30001) == SampleResult::TooOld);
    CHECK(!tb.Acquired(31001));
    CHECK_EQ(tb.Counters().tooOld, 1u);
    tb.Requested(1, 40000);
    CHECK(tb.Responded(1, 600000, 40000 + 30000) == SampleResult::Accepted);   // exactly the bound is still fresh
    CHECK(tb.Acquired(70000));
}

TEST(TimeBase_ignores_a_response_to_a_counter_it_did_not_send)
{
    TimeBase tb;
    CHECK(tb.Responded(7, 1000, 1000) == SampleResult::UnknownCounter);
    CHECK(!tb.Acquired(1000));
    for (uint32 c = 0; c < 10; ++c) { tb.Requested(c, 1000 + c); }
    CHECK(tb.Responded(0, 5000, 2000) == SampleResult::UnknownCounter);   // pushed out of the window of eight
    CHECK(tb.Responded(9, 5000, 2000) == SampleResult::Accepted);
    CHECK_EQ(tb.Counters().unknownCounter, 2u);
    CHECK(tb.Responded(9, 5000, 2000) == SampleResult::UnknownCounter);   // a counter answers once
}
