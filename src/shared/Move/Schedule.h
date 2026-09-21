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

#pragma once

// WHAT IS DUE, AND WHEN. The other half of scheduling a leg instead of watching it.
//
// Crossings.h says at which distances a path changes cell and Leg::TimeAtDistance turns
// those into moments. This holds the moments. A tick asks what has come due since the last
// one instead of asking every mover where it is, so a map full of units walking quietly
// across the middle of a cell costs a comparison against the earliest due time and nothing
// else.
//
// EVENTS COME IN BATCHES, because that is how they are produced: a leg is laid and its
// whole future is known at once -- some crossings and an arrival. Arming a mover replaces
// everything previously armed for it, which is also what re-aiming means. A chase that
// re-aims twice a second therefore never accumulates: the old batch is void the moment the
// new one is armed.
//
// Nothing here knows what a map, a unit or a cell is. `who` is an opaque handle and `what`
// an opaque tag; giving them meaning is the caller's business.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Move
{
    /// One moment, as the caller supplies it.
    struct Due
    {
        uint32_t when = 0;   ///< server clock, milliseconds, and it wraps
        uint32_t what = 0;   ///< the caller's tag
    };

    /// One moment, as it comes back out.
    struct Fired
    {
        uint64_t who = 0;
        uint32_t when = 0;
        uint32_t what = 0;
    };

    class Schedule
    {
        public:
            Schedule() : m_stamp(0), m_stale(0) {}

            /// Arm a mover's whole batch, replacing anything armed for it before. An empty
            /// batch is the same as disarming.
            void Arm(uint64_t who, const Due* events, size_t count);

            /// Everything armed for this mover is void. Cheap: the entries are left in the
            /// heap and skipped when they surface.
            void Disarm(uint64_t who);

            /// The earliest event due at or before `now`, or false when nothing is. Call
            /// it in a loop: a tick may have several.
            bool Next(uint32_t now, Fired& out);

            /// When the earliest live event is due. False when nothing is armed -- which is
            /// what lets a caller skip the whole mechanism on a quiet map.
            bool Earliest(uint32_t& when);

            void Clear();

            /// Entries held, live and void together. Not the number of armed events; see
            /// Armed() for that.
            size_t Held() const { return m_heap.size(); }

            /// Movers with a live batch.
            size_t Armed() const { return m_current.size(); }

        private:
            struct Entry
            {
                uint32_t when;
                uint32_t what;
                uint64_t who;
                /// Globally unique per batch, so an entry is live exactly when its mover's
                /// current batch is still this one. Never reused, so a disarmed mover's
                /// old entries can never be mistaken for a later batch's.
                uint64_t stamp;
            };

            /// The clock wraps every 49 days, so "earlier" is a signed difference, not a
            /// comparison. Everything armed at one time is within seconds of everything
            /// else, so the difference is never near the half-range where this would fail.
            static bool Sooner(const Entry& a, const Entry& b)
            {
                const int32_t d = int32_t(a.when - b.when);
                if (d != 0)
                {
                    return d < 0;
                }
                // A tie broken by stamp keeps one batch's events in the order they were
                // armed, so a crossing and the arrival it precedes cannot come out swapped.
                return a.stamp < b.stamp;
            }

            /// A mover's live batch: which one, and how many of its events are still to
            /// come. The count is what lets a mover be forgotten the moment its last event
            /// fires, without scanning the heap for siblings.
            struct Batch
            {
                uint64_t stamp;
                uint32_t remaining;
            };

            /// std::*_heap builds a MAX-heap, so the comparator is inverted once, in the
            /// source, rather than at each call site.
            struct Later;

            bool Live(const Entry& e) const;
            void Discard();
            void Compact();

            std::vector<Entry> m_heap;
            /// The batch each mover is currently on. A mover with nothing armed is absent.
            std::unordered_map<uint64_t, Batch> m_current;
            uint64_t m_stamp;
            /// Entries known to be void: the events of batches that were disarmed and are
            /// still sitting in the heap. Exact, because a batch knows how many it had left.
            size_t m_stale;
    };
}
