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

#include "TimeBase.h"

namespace Motion
{
    TimeBase::TimeBase(TimeBaseConfig const& config)
        : m_config(config), m_acquired(false), m_delta(0), m_lastRtt(0), m_lastSampleAt(0), m_outstandingCount(0)
    {
    }

    void TimeBase::Requested(uint32 counter, uint32 serverNow)
    {
        if (m_outstandingCount == kWindow)
        {
            for (int i = 1; i < kWindow; ++i) { m_outstanding[i - 1] = m_outstanding[i]; }
            --m_outstandingCount;
        }
        m_outstanding[m_outstandingCount].counter = counter;
        m_outstanding[m_outstandingCount].sentAt = serverNow;
        ++m_outstandingCount;
        ++m_counters.requested;
    }

    SampleResult TimeBase::Responded(uint32 counter, uint32 clientTicks, uint32 serverNow)
    {
        int found = -1;
        for (int i = 0; i < m_outstandingCount; ++i)
        {
            if (m_outstanding[i].counter == counter) { found = i; break; }
        }
        if (found < 0)
        {
            ++m_counters.unknownCounter;
            return SampleResult::UnknownCounter;
        }
        const uint32 sentAt = m_outstanding[found].sentAt;
        // Drop the answered request and everything older than it.
        const int keep = m_outstandingCount - (found + 1);
        for (int i = 0; i < keep; ++i) { m_outstanding[i] = m_outstanding[found + 1 + i]; }
        m_outstandingCount = uint8(keep);

        const uint32 rtt = serverNow - sentAt;
        const uint32 serverAtTick = sentAt + rtt / 2;
        const uint32 sample = clientTicks - serverAtTick;
        const bool fresh = m_acquired && (serverNow - m_lastSampleAt) < m_config.staleAfterMs;

        m_lastRtt = rtt;
        m_lastSampleAt = serverNow;
        ++m_counters.samples;

        if (!fresh)
        {
            m_delta = sample;
            m_acquired = true;
            return SampleResult::Accepted;
        }

        const int32 diff = int32(sample - m_delta);
        if (diff == 0) { return SampleResult::Accepted; }
        const int32 band = int32(m_config.slewBandMs);
        if (diff > band || diff < -band)
        {
            m_delta = sample;
            ++m_counters.jumped;
            return SampleResult::Jumped;
        }
        const int32 step = int32(m_config.maxSlewMs);
        const int32 move = diff > step ? step : (diff < -step ? -step : diff);
        m_delta += uint32(move);
        ++m_counters.slewed;
        return SampleResult::Slewed;
    }

    bool TimeBase::Acquired(uint32 serverNow) const
    {
        return m_acquired && (serverNow - m_lastSampleAt) < m_config.staleAfterMs;
    }

    uint32 TimeBase::Rebase(uint32 clientTime, uint32 serverNow)
    {
        if (!Acquired(serverNow))
        {
            ++m_counters.fallbacks;
            return serverNow;
        }
        return clientTime - m_delta;
    }

    void TimeBase::Reset()
    {
        m_acquired = false;
        m_delta = 0;
        m_lastRtt = 0;
        m_lastSampleAt = 0;
        m_outstandingCount = 0;
    }
}
