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

#include "Move/Leg.h"

#include "Geometry/GeometryMath.h"

#include <cmath>

namespace Move
{
    bool Leg::Launch(const Geometry::Vector3* points, uint16_t count, float speed,
                     uint32_t startMs)
    {
        // clear() rather than a fresh vector: a chase re-aims several times a second on
        // the same mover, and keeping the capacity means it allocates once in its life.
        m_points.clear();
        m_cumulative.clear();
        m_speed = 0.0f;
        m_startMs = startMs;
        m_cursor = 0;

        if (!points || count < 2 || !(speed > 0.0f) || !Geometry::isFinite(speed))
        {
            return false;
        }

        m_points.reserve(count);
        m_cumulative.reserve(count);

        float travelled = 0.0f;
        for (uint16_t i = 0; i < count; ++i)
        {
            if (!points[i].isFinite())
            {
                m_points.clear();
                m_cumulative.clear();
                return false;
            }
            if (i > 0)
            {
                travelled += (points[i] - points[i - 1]).magnitude();
            }
            m_points.push_back(points[i]);
            m_cumulative.push_back(travelled);
        }

        // A path with no length has no direction and no duration, and dividing by it is
        // how a mover ends up at a NaN. Duplicate points WITHIN a path are fine -- the
        // segment walk steps over them -- but a path that goes nowhere at all is not a leg.
        if (!(travelled > 0.0f))
        {
            m_points.clear();
            m_cumulative.clear();
            return false;
        }

        m_speed = speed;
        return true;
    }

    void Leg::Clear()
    {
        m_points.clear();
        m_cumulative.clear();
        m_speed = 0.0f;
        m_startMs = 0;
        m_cursor = 0;
    }

    uint32_t Leg::Duration() const
    {
        if (!Running())
        {
            return 0;
        }
        const uint32_t ms = uint32_t((Length() / m_speed) * 1000.0f);
        return ms == 0 ? 1u : ms;
    }

    uint16_t Leg::SegmentAt(float distance) const
    {
        const uint16_t last = uint16_t(m_points.size() - 1);

        // The cursor is a bookmark, not a cache of the answer: a query that runs backwards
        // in time (a test, a replay, a scenario rewind) simply pays for a rewind once.
        if (m_cursor >= last || m_cumulative[m_cursor] > distance)
        {
            m_cursor = 0;
        }
        while (m_cursor + 1 < last && m_cumulative[m_cursor + 1] <= distance)
        {
            ++m_cursor;
        }
        return m_cursor;
    }

    float Leg::DistanceAt(uint32_t nowMs) const
    {
        if (!Running())
        {
            return 0.0f;
        }
        const int32_t elapsed = Elapsed(nowMs);
        if (elapsed <= 0)
        {
            return 0.0f;
        }
        const float travelled = float(elapsed) * m_speed * 0.001f;
        const float total = Length();
        return travelled >= total ? total : travelled;
    }

    Geometry::Vector3 Leg::At(uint32_t nowMs) const
    {
        if (m_points.empty())
        {
            return Geometry::Vector3();
        }
        if (!Running())
        {
            return m_points.front();
        }

        const int32_t elapsed = Elapsed(nowMs);
        if (elapsed <= 0)
        {
            return m_points.front();
        }

        const float travelled = float(elapsed) * m_speed * 0.001f;
        if (travelled >= Length())
        {
            return m_points.back();
        }

        const uint16_t seg = SegmentAt(travelled);
        const float span = m_cumulative[seg + 1] - m_cumulative[seg];
        if (!(span > 0.0f))
        {
            return m_points[seg];
        }
        const float u = (travelled - m_cumulative[seg]) / span;
        return m_points[seg].lerp(m_points[seg + 1], u);
    }

    float Leg::FacingAt(uint32_t nowMs) const
    {
        if (!Running())
        {
            return 0.0f;
        }

        const float travelled = DistanceAt(nowMs);
        // At or past the end the heading is the last segment's: a mover that has stopped
        // keeps looking the way it was going, it does not snap north.
        const uint16_t seg = travelled >= Length() ? uint16_t(m_points.size() - 2)
                                                   : SegmentAt(travelled);

        const Geometry::Vector3 step = m_points[seg + 1] - m_points[seg];
        if (step.x == 0.0f && step.y == 0.0f)
        {
            return 0.0f;
        }
        const float angle = std::atan2(step.y, step.x);
        return angle >= 0.0f ? angle : angle + 2.0f * Geometry::pif();
    }

    bool Leg::Arrived(uint32_t nowMs) const
    {
        if (!Running())
        {
            return true;
        }
        const int32_t elapsed = Elapsed(nowMs);
        return elapsed > 0 && float(elapsed) * m_speed * 0.001f >= Length();
    }

    uint32_t Leg::TimeAtDistance(float distance) const
    {
        if (!Running())
        {
            return m_startMs;
        }
        const float total = Length();
        const float along = distance <= 0.0f ? 0.0f : (distance >= total ? total : distance);
        return m_startMs + uint32_t((along / m_speed) * 1000.0f);
    }
}
