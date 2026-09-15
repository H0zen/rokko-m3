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

#include "WorldClock.h"

#include <atomic>

namespace
{
    std::chrono::steady_clock::time_point const& StartPointStorage()
    {
        static const std::chrono::steady_clock::time_point point = std::chrono::steady_clock::now();
        return point;
    }

    std::atomic<bool>   g_stepped{false};
    std::atomic<uint32> g_counterMs{0};
    std::atomic<uint32> g_offsetMs{0};
    std::atomic<uint32> g_anchorMs{0};     // NowMs() at the last EnterStepped()
    std::atomic<time_t> g_anchorUnix{0};   // NowUnix() at the last EnterStepped()
    static_assert(std::atomic<time_t>::is_always_lock_free, "the clock's anchor must be lock-free: readers are on every thread");
}

namespace WorldClock
{
    uint32 RealMs()
    {
        using namespace std::chrono;
        return uint32(duration_cast<milliseconds>(steady_clock::now() - StartPointStorage()).count());
    }

    uint32 NowMs()
    {
        if (g_stepped.load(std::memory_order_acquire))
        {
            return g_counterMs.load(std::memory_order_acquire);
        }
        return RealMs() + g_offsetMs.load(std::memory_order_acquire);
    }

    time_t NowUnix()
    {
        if (g_stepped.load(std::memory_order_acquire))
        {
            const uint32 counter = g_counterMs.load(std::memory_order_acquire);
            const uint32 anchorMs = g_anchorMs.load(std::memory_order_acquire);
            const time_t anchorUnix = g_anchorUnix.load(std::memory_order_acquire);
            return anchorUnix + time_t((counter - anchorMs) / 1000);
        }
        return std::time(nullptr);
    }

    bool IsStepped()
    {
        return g_stepped.load(std::memory_order_acquire);
    }

    void EnterStepped()
    {
        const uint32 nowMs = NowMs();          // still real here: the steady clock plus the ms offset
        const time_t nowUnix = NowUnix();      // still real here: the wall clock
        g_counterMs.store(nowMs, std::memory_order_release);
        g_anchorMs.store(nowMs, std::memory_order_release);
        g_anchorUnix.store(nowUnix, std::memory_order_release);
        g_stepped.store(true, std::memory_order_release);
    }

    void Step(uint32 ms)
    {
        g_counterMs.fetch_add(ms, std::memory_order_acq_rel);
    }

    void LeaveStepped()
    {
        if (!g_stepped.load(std::memory_order_acquire))
        {
            return;   // already real: the offsets stand, the counter and anchors are stale
        }

        const uint32 counter = g_counterMs.load(std::memory_order_acquire);
        const uint32 real = RealMs();
        // Both clocks wrap at 32 bits; the modular difference under half the range is a
        // lead (bounded by a run's virtual length), over it a lag (bounded by its real
        // length), so a run straddling the wrap keeps its lead like getMSTimeDiff would.
        const uint32 lead = counter - real;
        g_offsetMs.store(lead < 0x80000000u ? lead : 0, std::memory_order_release);

        g_stepped.store(false, std::memory_order_release);
    }

    uint32 OffsetMs()
    {
        return g_offsetMs.load(std::memory_order_acquire);
    }

    std::chrono::steady_clock::time_point StartPoint()
    {
        return StartPointStorage();
    }
}
