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

#include "Move/Schedule.h"

#include <algorithm>

namespace Move
{
    namespace
    {
        /// Rebuild once void entries are this share of the heap. Removing an entry from
        /// the middle of a heap costs as much as rebuilding it, so nothing is removed when
        /// a mover is disarmed -- the entries are skipped when they surface. Left alone
        /// that would grow without bound under a chase that re-aims every few ticks, so
        /// the heap is rebuilt when enough of it is dead to be worth the pass.
        const size_t COMPACT_NUMERATOR = 1;
        const size_t COMPACT_DENOMINATOR = 2;

        /// Below this a rebuild is not worth its own cost, whatever the ratio says.
        const size_t COMPACT_FLOOR = 32;
    }

    /// A min-heap out of std::*_heap, which builds a max-heap: the comparator is reversed
    /// once, here, rather than at each of the four call sites.
    struct Schedule::Later
    {
        bool operator()(const Entry& a, const Entry& b) const { return Sooner(b, a); }
    };

    bool Schedule::Live(const Entry& e) const
    {
        const std::unordered_map<uint64_t, Batch>::const_iterator it = m_current.find(e.who);
        return it != m_current.end() && it->second.stamp == e.stamp;
    }

    void Schedule::Arm(uint64_t who, const Due* events, size_t count)
    {
        // The previous batch dies here, whatever else happens: a new aim replaces an old
        // one rather than adding to it.
        Disarm(who);

        if (!events || count == 0)
        {
            return;
        }

        const uint64_t stamp = ++m_stamp;
        Batch batch;
        batch.stamp = stamp;
        batch.remaining = uint32_t(count);
        m_current[who] = batch;

        for (size_t i = 0; i < count; ++i)
        {
            Entry e;
            e.when = events[i].when;
            e.what = events[i].what;
            e.who = who;
            e.stamp = stamp;
            m_heap.push_back(e);
            std::push_heap(m_heap.begin(), m_heap.end(), Later());
        }
    }

    void Schedule::Disarm(uint64_t who)
    {
        const std::unordered_map<uint64_t, Batch>::iterator it = m_current.find(who);
        if (it == m_current.end())
        {
            return;
        }
        // Exactly how many of this mover's events are still in the heap, so the rebuild
        // threshold below compares entries with entries.
        m_stale += it->second.remaining;
        m_current.erase(it);
        Compact();
    }

    void Schedule::Discard()
    {
        std::pop_heap(m_heap.begin(), m_heap.end(), Later());
        m_heap.pop_back();
    }

    bool Schedule::Next(uint32_t now, Fired& out)
    {
        while (!m_heap.empty())
        {
            const Entry top = m_heap.front();
            if (!Live(top))
            {
                Discard();
                continue;
            }
            if (int32_t(top.when - now) > 0)
            {
                return false;
            }

            Discard();
            out.who = top.who;
            out.when = top.when;
            out.what = top.what;

            // The batch carries its own count, so the mover is forgotten the moment its
            // last event fires without anyone searching the heap for siblings.
            const std::unordered_map<uint64_t, Batch>::iterator it = m_current.find(top.who);
            if (it != m_current.end() && --it->second.remaining == 0)
            {
                m_current.erase(it);
            }
            return true;
        }
        return false;
    }

    bool Schedule::Earliest(uint32_t& when)
    {
        while (!m_heap.empty())
        {
            if (Live(m_heap.front()))
            {
                when = m_heap.front().when;
                return true;
            }
            Discard();
        }
        return false;
    }

    void Schedule::Clear()
    {
        m_heap.clear();
        m_current.clear();
        m_stale = 0;
    }

    void Schedule::Compact()
    {
        if (m_heap.size() < COMPACT_FLOOR ||
            m_stale * COMPACT_DENOMINATOR < m_heap.size() * COMPACT_NUMERATOR)
        {
            return;
        }

        std::vector<Entry> kept;
        kept.reserve(m_heap.size());
        for (size_t i = 0; i < m_heap.size(); ++i)
        {
            if (Live(m_heap[i]))
            {
                kept.push_back(m_heap[i]);
            }
        }
        m_heap.swap(kept);
        std::make_heap(m_heap.begin(), m_heap.end(), Later());
        m_stale = 0;
    }
}
