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

#include "Timeline.h"

#include <algorithm>
#include <cmath>

namespace Harness
{
    Timeline::Timeline() : m_now(0), m_seq(0), m_running(false) {}

    void Timeline::At(uint32 offsetMs, std::function<void()> step)
    {
        Step s;
        s.at = (m_running ? m_now : 0) + offsetMs;
        s.seq = m_seq++;
        s.run = step;
        m_steps.push_back(s);
    }

    void Timeline::Advance(uint32 nowMs)
    {
        m_now = nowMs;
        for (;;)
        {
            // The earliest due step, by moment then by insertion; taken out before it
            // runs, so a step that schedules more never sees itself again.
            size_t best = m_steps.size();
            for (size_t i = 0; i < m_steps.size(); ++i)
            {
                if (m_steps[i].at > nowMs)
                {
                    continue;
                }
                if (best == m_steps.size() || m_steps[i].at < m_steps[best].at ||
                    (m_steps[i].at == m_steps[best].at && m_steps[i].seq < m_steps[best].seq))
                {
                    best = i;
                }
            }
            if (best == m_steps.size())
            {
                return;
            }
            Step due = m_steps[best];
            m_steps.erase(m_steps.begin() + best);
            m_now = due.at;   // a step scheduling from itself is relative to its own moment
            m_running = true;
            due.run();
            m_running = false;
            m_now = nowMs;
        }
    }

    std::string VerdictLine(char const* scenario, std::string const& body)
    {
        return std::string("VERDICT ") + scenario + " " + body;
    }

    float Dist2(float x1, float y1, float x2, float y2)
    {
        const float dx = x1 - x2;
        const float dy = y1 - y2;
        return std::sqrt(dx * dx + dy * dy);
    }
}
