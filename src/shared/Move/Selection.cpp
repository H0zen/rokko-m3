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

#include "Move/Selection.h"

namespace Move
{
    void Selection::Request(Layer layer, Policy policy, uint8_t kind, uint32_t id)
    {
        const uint8_t at = uint8_t(layer);
        if (at >= LAYERS)
        {
            return;
        }

        // Everything BELOW, decided by the policy. Above is untouched: a fear does not
        // dislodge a taxi, and asking it to would be asking a priority not to be one.
        if (policy == Policy::Override)
        {
            for (uint8_t i = 0; i < at; ++i)
            {
                m_slot[i] = Held();
            }
        }

        // The layer itself always takes the new kind, whatever the policy. Supersede says
        // so explicitly; the other two do it too, because two things cannot share a
        // priority and still have one of them be the answer.
        m_slot[at].held = true;
        m_slot[at].kind = kind;
        m_slot[at].id = id;
    }

    void Selection::Finish(Layer layer)
    {
        const uint8_t at = uint8_t(layer);
        if (at < LAYERS)
        {
            m_slot[at] = Held();
        }
    }

    void Selection::Clear()
    {
        for (uint8_t i = 0; i < LAYERS; ++i)
        {
            m_slot[i] = Held();
        }
    }

    bool Selection::Selected(Layer& out) const
    {
        for (uint8_t i = LAYERS; i-- > 0;)
        {
            if (m_slot[i].held)
            {
                out = Layer(i);
                return true;
            }
        }
        return false;
    }

    bool Selection::IsHeld(Layer layer) const
    {
        const uint8_t at = uint8_t(layer);
        return at < LAYERS && m_slot[at].held;
    }

    uint8_t Selection::Depth() const
    {
        uint8_t n = 0;
        for (uint8_t i = 0; i < LAYERS; ++i)
        {
            n += m_slot[i].held ? 1 : 0;
        }
        return n;
    }
}
