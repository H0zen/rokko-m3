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

#ifndef MANGOS_HARNESS_TIMELINE_H
#define MANGOS_HARNESS_TIMELINE_H

#include "Platform/Define.h"

#include <functional>
#include <string>
#include <vector>

/**
 * The GM harness's step timeline (movement P0-C, design v2 §12): a scenario
 * schedules steps at millisecond offsets and the runner advances the timeline at
 * its cadence, running every step whose moment has passed, in order, once. A step
 * may schedule further steps relative to its own moment, as the old harness's
 * timers did. Pure: no map, no clock; the runner hands the time in.
 */
namespace Harness
{
    class Timeline
    {
    public:
        Timeline();

        /// Outside a step: at `offsetMs` from the origin. Inside a running step: at Now() + offsetMs.
        void At(uint32 offsetMs, std::function<void()> step);
        /// Runs every step due by `nowMs`, in `at` order, ties in insertion order, each once.
        void Advance(uint32 nowMs);
        size_t Pending() const { return m_steps.size(); }
        bool Idle() const { return m_steps.empty(); }
        uint32 Now() const { return m_now; }

    private:
        struct Step
        {
            uint32                at;
            uint32                seq;
            std::function<void()> run;
        };
        std::vector<Step> m_steps;
        uint32            m_now;
        uint32            m_seq;
        bool              m_running;
    };

    /// "VERDICT <scenario> <body>": the old runner's line, minus the MVTEST prefix the log adds.
    std::string VerdictLine(char const* scenario, std::string const& body);
    /// The old harness's generator-type names, by MovementGeneratorType value.
    char const* TypeName(uint32 movementGeneratorType);
    /// Planar distance.
    float Dist2(float x1, float y1, float x2, float y2);
}

#endif
