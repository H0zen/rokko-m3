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

#ifndef MANGOS_MOTION_TIME_BASE_H
#define MANGOS_MOTION_TIME_BASE_H

#include "Platform/Define.h"

/**
 * A session's clock model (design v2 §6.3): the delta between the client's
 * millisecond tick and the server's, learned from time-sync pairs, so every
 * movement status the client sends can be rebased to server time before it
 * is stored or relayed. All arithmetic is uint32 modulo 2^32: the client's
 * counter wraps at 0xFFFFFFFF about every 49 days and so does the server's,
 * at a different moment, and the difference of two wrapped counters is the
 * right difference either way. Time is an argument; nothing here reads a
 * clock. Replaces the legacy `clientTime + GetLatency()` guess.
 *
 * Thread-safety is the world tick's, not this class's: a session's clock is
 * touched from the session phase (World::UpdateSessions, the world thread)
 * and the map phase (Map::Update on a worker, behind MapUpdater::wait) and
 * never from both at once, and no handler runs on the network thread. A
 * change to that ordering is what would break this, not a change here.
 */
namespace Motion
{
    struct TimeBaseConfig
    {
        uint32 staleAfterMs;   ///< no sample for this long: not acquired, rebase falls back to server-now
        uint32 slewBandMs;     ///< a sample within this of the current delta slews; beyond it, jumps
        uint32 maxSlewMs;      ///< how far one sample may move the delta inside the band
        uint32 maxSampleAgeMs; ///< a response to a request older than this is dropped rather than trusted
        TimeBaseConfig() : staleAfterMs(60000), slewBandMs(100), maxSlewMs(5), maxSampleAgeMs(30000) {}
    };

    enum class SampleResult : uint8 { Accepted, Slewed, Jumped, UnknownCounter, TooOld };

    char const* SampleResultName(SampleResult result);

    struct TimeBaseCounters
    {
        uint32 requested, samples, slewed, jumped, unknownCounter, fallbacks, tooOld;
        TimeBaseCounters() : requested(0), samples(0), slewed(0), jumped(0), unknownCounter(0), fallbacks(0), tooOld(0) {}
    };

    class TimeBase
    {
    public:
        explicit TimeBase(TimeBaseConfig const& config = TimeBaseConfig());

        void Requested(uint32 counter, uint32 serverNow);                          ///< SMSG_TIME_SYNC_REQ went out
        SampleResult Responded(uint32 counter, uint32 clientTicks, uint32 serverNow); ///< CMSG_TIME_SYNC_RESP came back
        bool Acquired(uint32 serverNow) const;
        uint32 Rebase(uint32 clientTime, uint32 serverNow);                        ///< server time for a client timestamp; counts a fallback when not acquired
        uint32 Delta() const { return m_delta; }                                   ///< clientTicks - serverTime, modulo 2^32
        uint32 LastRtt() const { return m_lastRtt; }
        uint32 LastSampleAt() const { return m_lastSampleAt; }
        void Reset();                                                              ///< an epoch event: login, worldport, control handback
        TimeBaseCounters const& Counters() const { return m_counters; }

    private:
        enum { kWindow = 8 };
        struct Outstanding
        {
            uint32 counter;
            uint32 sentAt;
        };

        TimeBaseConfig   m_config;
        bool             m_acquired;
        uint32           m_delta;
        uint32           m_lastRtt;
        uint32           m_lastSampleAt;
        Outstanding      m_outstanding[kWindow];
        uint8            m_outstandingCount;
        TimeBaseCounters m_counters;
    };
}

#endif
