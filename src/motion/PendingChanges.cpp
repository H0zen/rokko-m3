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

#include "PendingChanges.h"

#include <cmath>

namespace Motion
{
    namespace
    {
        const float kPayloadTolerance = 0.01f;   // the legacy speed-ack handler's tolerance (MovementHandler.cpp:544)
    }

    PendingChanges::PendingChanges(TimeoutPolicy const& policy)
        : m_policy(policy), m_next(0), m_epoch(0), m_resyncs(0)
    {
    }

    PendingChange const* PendingChanges::Get(ChangeType type) const
    {
        for (size_t i = 0; i < m_pending.size(); ++i)
        {
            if (m_pending[i].type == type) { return &m_pending[i]; }
        }
        return NULL;
    }

    void PendingChanges::Retire(PendingChange const& entry, uint32 now)
    {
        Tombstone t;
        t.type = entry.type;
        t.counter = entry.counter;
        t.diesAt = now + m_policy.tombstoneTtlMs;
        m_tombstones.push_back(t);
    }

    bool PendingChanges::ConsumeTombstone(ChangeType type, uint32 counter)
    {
        for (size_t i = 0; i < m_tombstones.size(); ++i)
        {
            if (m_tombstones[i].type == type && m_tombstones[i].counter == counter)
            {
                m_tombstones.erase(m_tombstones.begin() + i);
                return true;
            }
        }
        return false;
    }

    uint32 PendingChanges::Reissue(PendingChange& entry, uint32 now)
    {
        const uint32 old = entry.counter;
        Retire(entry, now);
        entry.counter = m_next++;
        entry.sentAt = now;
        ++m_counters.resent;
        return old;
    }

    uint32 PendingChanges::Open(Change const& change, uint32 now)
    {
        for (size_t i = 0; i < m_pending.size(); ++i)
        {
            if (m_pending[i].type == change.type)
            {
                Retire(m_pending[i], now);
                m_pending.erase(m_pending.begin() + i);
                ++m_counters.superseded;
                break;
            }
        }
        PendingChange entry;
        entry.type = change.type;
        entry.counter = m_next++;
        entry.epoch = m_epoch;
        entry.change = change;
        entry.sentAt = now;
        entry.resends = 0;
        m_pending.push_back(entry);
        ++m_counters.opened;
        return entry.counter;
    }

    AckOutcome PendingChanges::Ack(ChangeType type, uint32 counter, AckPayload const& payload, uint32 /*now*/)
    {
        AckOutcome out;
        if (counter >= m_next)
        {
            out.result = AckResult::Future;
            ++m_counters.future;
            return out;
        }
        for (size_t i = 0; i < m_pending.size(); ++i)
        {
            if (m_pending[i].type != type || m_pending[i].counter != counter) { continue; }
            out.change = m_pending[i];
            m_pending.erase(m_pending.begin() + i);
            if (payload.hasValue && std::fabs(payload.value - out.change.change.value) > kPayloadTolerance)
            {
                out.result = AckResult::PayloadMismatch;
                ++m_counters.payloadMismatch;
            }
            else
            {
                out.result = AckResult::Matched;
                ++m_counters.matched;
            }
            return out;
        }
        if (ConsumeTombstone(type, counter))
        {
            out.result = AckResult::Tombstone;
            ++m_counters.tombstone;
            return out;
        }
        if (!Has(type))
        {
            out.result = AckResult::NoPending;
            ++m_counters.noPending;
            return out;
        }
        out.result = AckResult::Stale;
        ++m_counters.stale;
        return out;
    }

    void PendingChanges::NewEpoch(uint32 now)
    {
        for (size_t i = 0; i < m_pending.size(); ++i)
        {
            Retire(m_pending[i], now);
            ++m_counters.retired;
        }
        m_pending.clear();
        ++m_epoch;
        m_resyncs = 0;
    }

    void PendingChanges::ExpireTombstones(uint32 now)
    {
        for (size_t i = 0; i < m_tombstones.size();)
        {
            if (int32(now - m_tombstones[i].diesAt) >= 0) { m_tombstones.erase(m_tombstones.begin() + i); }
            else { ++i; }
        }
    }

    std::vector<TimeoutEvent> PendingChanges::Tick(uint32 now)
    {
        std::vector<TimeoutEvent> events;
        ExpireTombstones(now);
        if (m_policy.timeoutMs == 0) { return events; }

        // Two passes. If any late entry has spent its resends and a resync is
        // still allowed, this tick is a resync of everything and no entry takes
        // a plain resend; otherwise each late entry is resent or, with resyncs
        // spent too, kicked.
        bool spent = false;
        for (size_t i = 0; i < m_pending.size(); ++i)
        {
            if (now - m_pending[i].sentAt >= m_policy.timeoutMs && m_pending[i].resends >= m_policy.maxResends)
            {
                spent = true;
            }
        }

        if (spent && m_resyncs < m_policy.maxResyncs)
        {
            ++m_resyncs;
            ++m_counters.resynced;
            TimeoutEvent head;
            head.action = TimeoutAction::Resync;
            head.type = ChangeType::None;
            head.oldCounter = 0;
            head.newCounter = 0;
            events.push_back(head);
            for (size_t i = 0; i < m_pending.size(); ++i)
            {
                TimeoutEvent e;
                e.action = TimeoutAction::Resend;
                e.type = m_pending[i].type;
                e.oldCounter = Reissue(m_pending[i], now);
                e.newCounter = m_pending[i].counter;
                m_pending[i].resends = 0;
                events.push_back(e);
            }
            return events;
        }

        for (size_t i = 0; i < m_pending.size();)
        {
            PendingChange& entry = m_pending[i];
            if (now - entry.sentAt < m_policy.timeoutMs) { ++i; continue; }
            if (entry.resends < m_policy.maxResends)
            {
                TimeoutEvent e;
                e.action = TimeoutAction::Resend;
                e.type = entry.type;
                e.oldCounter = Reissue(entry, now);
                e.newCounter = entry.counter;
                ++entry.resends;
                events.push_back(e);
                ++i;
            }
            else
            {
                TimeoutEvent e;
                e.action = TimeoutAction::Kick;
                e.type = entry.type;
                e.oldCounter = entry.counter;
                e.newCounter = 0;
                events.push_back(e);
                Retire(entry, now);
                ++m_counters.kicked;
                m_pending.erase(m_pending.begin() + i);
            }
        }
        return events;
    }
}
