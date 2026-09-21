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

#pragma once

// WHICH ONE IS RUNNING, and what happens to the others.
//
// A creature can want several things at once: it is chasing, and then something frightens
// it, and when the fear ends the chase has to come back. That is a real problem and it
// needs somewhere to live. It does not need seven hundred lines: it is seven slots, one
// per priority, and one rule per kind saying what its arrival does to the slots beneath.
//
//   SUPERSEDE  replace whatever shares this slot; leave everything else alone.
//   SUSPEND    take over, but the slots below keep their contents and run again after.
//   OVERRIDE   take over and empty the slots below; nothing comes back.
//
// The selected kind is simply the highest occupied slot. There is no event stream here and
// no transaction: a caller that needs to know whether the selection moved compares it
// before and after, which is one byte.
//
// KINDS ARE OPAQUE. This piece never learns what a Chase is -- it takes the layer and the
// policy as arguments, because the tables that map a kind to them are game vocabulary and
// belong on the game's side of the wall. What is left is the arbitration itself, and that
// is the same whatever the words.

#include <cstdint>

namespace Move
{
    /// Ascending priority. Names kept from the tree so the two can be compared while both
    /// are running; the ORDER is the whole content of the type.
    enum class Layer : uint8_t
    {
        Default,    ///< idle, wander, patrol, follow
        Combat,     ///< chase
        Scripted,   ///< point, fly-land, home, assist-run
        Distract,
        Control,    ///< fear, confused
        Forced,     ///< effect
        Taxi,
        Count
    };

    enum class Policy : uint8_t { Supersede, Suspend, Override };

    /// What sits in one slot. `kind` and `id` are the caller's, carried and never read.
    struct Held
    {
        bool held = false;
        uint8_t kind = 0;
        uint32_t id = 0;
    };

    class Selection
    {
        public:
            static const uint8_t LAYERS = uint8_t(Layer::Count);

            /// Put a kind on its layer and apply its policy to everything below it.
            /// Whatever shared the layer is gone either way -- that is what a layer is.
            void Request(Layer layer, Policy policy, uint8_t kind, uint32_t id = 0);

            /// The thing on this layer ended by itself. Anything suspended beneath it
            /// becomes selected again, which is the whole point of Suspend.
            void Finish(Layer layer);

            /// Kinds that expire the moment anything else is requested -- home, distract
            /// and effect in the tree. Applied by the caller before its own Request, since
            /// which kinds those are is game vocabulary.
            void Expire(Layer layer) { Finish(layer); }

            void Clear();

            /// The highest occupied layer, or false when nothing is held. This is the
            /// selection, in full: there is nothing else to compute.
            bool Selected(Layer& out) const;

            bool IsHeld(Layer layer) const;
            const Held& At(Layer layer) const { return m_slot[uint8_t(layer)]; }

            /// How many layers hold something. For tests and for a caller that wants to
            /// notice a stack growing without bound.
            uint8_t Depth() const;

        private:
            Held m_slot[LAYERS];
    };
}
