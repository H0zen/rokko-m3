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

#include "MoveStats.h"
#include "Move/MoveWriter.h"
#include "Timer.h"
#include "Log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace MoveStats
{
    namespace
    {
        /// Counted from every map thread, so the arithmetic has to be atomic -- but nothing
        /// reads them except a GM dump, so the cheapest ordering will do.
        std::atomic<uint64> s_visits(0);
        std::atomic<uint64> s_decisions(0);
        std::atomic<uint64> s_packets(0);
        std::atomic<uint64> s_points(0);
        std::atomic<uint64> s_turns(0);
        std::atomic<uint64> s_refusals[6];
        std::atomic<uint32> s_since(0);

        /// How often the block goes to the log. Long enough that it is not noise, short
        /// enough that a session of play produces several of them to compare.
        const uint32 REPORT_EVERY_MS = 60000;
        uint32 s_untilReport = REPORT_EVERY_MS;

        /// A refusal is logged with the creature that caused it, but only the first few of
        /// each kind: one broken shape must not be able to fill the log by itself.
        const uint32 NAMED_PER_KIND = 5;
        std::atomic<uint32> s_named[6];

        void Bump(std::atomic<uint64>& counter)
        {
            counter.fetch_add(1, std::memory_order_relaxed);
        }

        std::string Line(char const* format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return std::string(buffer);
        }
    }

    void Visited()
    {
        Bump(s_visits);
    }

    void Decided()
    {
        Bump(s_decisions);
    }

    void Sent(uint32 points)
    {
        Bump(s_packets);
        s_points.fetch_add(points, std::memory_order_relaxed);
    }

    void Turned()
    {
        Bump(s_turns);
    }

    void Refused(uint8 refusal, uint64 who, uint32 entry)
    {
        if (refusal >= 6)
        {
            return;
        }
        Bump(s_refusals[refusal]);

        if (s_named[refusal].fetch_add(1, std::memory_order_relaxed) < NAMED_PER_KIND)
        {
            sLog.outError("Move: leg refused for guid " UI64FMTD " (entry %u): %s",
                          who, entry, Move::MoveWriter::Why(Move::Refusal(refusal)));
        }
    }

    void Tick(uint32 diff)
    {
        if (diff >= s_untilReport)
        {
            s_untilReport = REPORT_EVERY_MS;
            for (int i = 0; i < 6; ++i)
            {
                s_named[i].store(0, std::memory_order_relaxed);
            }
            ReportAndReset([](std::string const& line) { sLog.outString("%s", line.c_str()); });
        }
        else
        {
            s_untilReport -= diff;
        }
    }

    void ReportAndReset(std::function<void(std::string const&)> const& line)
    {
        const uint32 now = getMSTime();
        const uint32 started = s_since.exchange(now, std::memory_order_relaxed);
        const uint32 elapsed = started ? getMSTimeDiff(started, now) : 0;
        const float seconds = elapsed ? float(elapsed) * 0.001f : 0.0f;

        const uint64 visits = s_visits.exchange(0, std::memory_order_relaxed);
        const uint64 decisions = s_decisions.exchange(0, std::memory_order_relaxed);
        const uint64 packets = s_packets.exchange(0, std::memory_order_relaxed);
        const uint64 points = s_points.exchange(0, std::memory_order_relaxed);
        const uint64 turns = s_turns.exchange(0, std::memory_order_relaxed);

        line(Line("movement over %.1f s:", seconds));

        // The ratio is the measurement. A visit that decides nothing is one integer
        // comparison; the old engine made every visit a decision.
        const double ratio = decisions ? double(visits) / double(decisions) : 0.0;
        line(Line("  visits %llu, decisions %llu  (%.0f visits per decision)",
                  (unsigned long long)visits, (unsigned long long)decisions, ratio));
        if (seconds > 0.0f)
        {
            line(Line("  %.0f decisions per second", double(decisions) / seconds));
        }

        // Points per packet above one means welding is working: a patrol crossing several
        // nodes without a wait sends them together instead of one packet each.
        const double perPacket = packets ? double(points) / double(packets) : 0.0;
        line(Line("  legs %llu carrying %llu points  (%.2f points per leg)",
                  (unsigned long long)packets, (unsigned long long)points, perPacket));

        // Near zero is the claim: the client takes the facing from the spline's tangent, so
        // nothing should be sent merely to turn a creature that is moving.
        line(Line("  turns %llu", (unsigned long long)turns));

        static char const* const NAMES[6] =
        {
            "none", "too few points", "no length", "exceeds ceiling",
            "out of pack reach", "not finite"
        };
        uint64 total = 0;
        std::string detail;
        for (int i = 1; i < 6; ++i)
        {
            const uint64 count = s_refusals[i].exchange(0, std::memory_order_relaxed);
            total += count;
            if (count)
            {
                detail += Line(" %s=%llu", NAMES[i], (unsigned long long)count);
            }
        }
        if (total == 0)
        {
            line("  refusals 0");
        }
        else
        {
            // Not a warning about load. A refusal is a shape asking for something the client
            // would have mishandled, and the shape is what needs fixing.
            line(Line("  refusals %llu --%s", (unsigned long long)total, detail.c_str()));
        }
    }
}
