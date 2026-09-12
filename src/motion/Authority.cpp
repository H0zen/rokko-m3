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

#include "Authority.h"

#include <algorithm>

namespace Motion
{
    Authority::Authority() : m_selected(0), m_base(0), m_lastRemoved(0) {}

    void Authority::SetBase(uint64 guid)
    {
        m_base = guid;
    }

    void Authority::Add(uint64 guid)
    {
        if (!IsMember(guid))
        {
            m_members.push_back(guid);
            ++m_counters.added;
        }
        // The body yields the selection to a grant; a controlled unit does not --
        // a fear ending on a possessing player's own body must not steal the
        // selection from the creature still possessed.
        if (m_selected == 0 || m_selected == m_base)
        {
            m_selected = guid;
        }
    }

    bool Authority::Remove(uint64 guid)
    {
        std::vector<uint64>::iterator it = std::find(m_members.begin(), m_members.end(), guid);
        if (it == m_members.end())
        {
            return false;
        }
        m_members.erase(it);
        ++m_counters.removed;
        m_lastRemoved = guid;
        if (m_selected == guid)
        {
            m_selected = 0;
        }
        return true;
    }

    bool Authority::IsMember(uint64 guid) const
    {
        return std::find(m_members.begin(), m_members.end(), guid) != m_members.end();
    }

    bool Authority::Select(uint64 guid)
    {
        if (!IsMember(guid))
        {
            ++m_counters.badSelect;
            return false;
        }
        m_selected = guid;
        ++m_counters.selected;
        return true;
    }

    bool Authority::Deselect(uint64 guid)
    {
        // The selection changes only through Add, Remove and Select; a deselect
        // never touches it. Every legitimate CMSG_MOVE_NOT_ACTIVE_MOVER is either
        // followed by the client's own select (a grant of another unit) or
        // preceded by the server's own Remove (a revoke), so this only tells a
        // benign deselect -- the selected unit, the base player, a member, or the
        // unit Remove just dropped -- from a stranger's.
        if (guid != 0 && (guid == m_selected || guid == m_base || IsMember(guid) || guid == m_lastRemoved))
        {
            ++m_counters.deselected;
            return true;
        }
        ++m_counters.badDeselect;
        return false;
    }

    bool Authority::MovesAs(uint64 guid)
    {
        if (guid == 0 || guid != m_selected)
        {
            ++m_counters.notActive;
            return false;
        }
        return true;
    }

    bool Authority::MayAck(uint64 guid)
    {
        if (!IsMember(guid))
        {
            ++m_counters.notMember;
            return false;
        }
        return true;
    }

    void Authority::Clear()
    {
        m_counters.removed += uint32(m_members.size());
        m_members.clear();
        m_selected = 0;
        m_lastRemoved = 0;
    }
}
