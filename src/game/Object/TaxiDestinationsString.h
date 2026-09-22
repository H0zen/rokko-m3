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

#ifndef MANGOS_TAXIDESTINATIONSSTRING_H
#define MANGOS_TAXIDESTINATIONSSTRING_H

#include "Platform/Define.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

/**
 * The character table's taxi_path column (P5-B family 5, design §6.7): the flight master's
 * faction template id first, then the route's remaining node ids, space-separated with a
 * trailing space. Header-only so the suite pins the round trip without the game library.
 *
 * Since the landing-time resume (design 2026-09-22 §2) the string may carry ONE more token,
 * last: `L<seconds>`, the server time the whole route's flight ends. It is a lettered token
 * rather than a further number precisely so that the two forms cannot be confused -- the old
 * form has no `L`, parses exactly as it always did, and yields a landing time of zero, which
 * the resume reads as "landed". A column is extended instead of added: a `characters` column
 * would need a database migration AND a matching revision_data.h.in bump, or the core refuses
 * to start.
 */
namespace TaxiDestinationsString
{
    /// Empty for an empty route (the column's "not flying"). A zero landing time is not written,
    /// so a build that stamps none produces byte-for-byte the string the old one did.
    inline std::string Format(uint32 faction, std::vector<uint32> const& nodes, uint32 landing = 0)
    {
        if (nodes.empty())
        {
            return std::string();
        }
        std::ostringstream ss;
        ss << faction << ' ';
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            ss << nodes[i] << ' ';
        }
        if (landing)
        {
            ss << 'L' << landing << ' ';
        }
        return ss.str();
    }

    /// False on a token that is neither a number nor an `L`-stamp; an empty string is an empty
    /// route. `landing` may be NULL for a caller that does not want the stamp; it is set to zero
    /// when the string carries none, which is the old form and reads as "landed".
    inline bool Parse(std::string const& values, uint32& faction, std::vector<uint32>& nodes,
                      uint32* landing = NULL)
    {
        faction = 0;
        nodes.clear();
        if (landing)
        {
            *landing = 0;
        }
        std::istringstream in(values);
        std::string token;
        bool first = true;
        while (in >> token)
        {
            // The landing stamp. It never takes the faction's slot and never becomes a node,
            // whatever position it is found in, so a reader of this string cannot mistake the
            // route for one hop longer than it is.
            const bool stamp = (token[0] == 'L');
            char const* digits = token.c_str() + (stamp ? 1 : 0);
            char* end = NULL;
            const unsigned long value = std::strtoul(digits, &end, 10);
            if (end == digits || *end != '\0')
            {
                return false;
            }
            if (stamp)
            {
                if (landing)
                {
                    *landing = uint32(value);
                }
            }
            else if (first)
            {
                faction = uint32(value);
                first = false;
            }
            else
            {
                nodes.push_back(uint32(value));
            }
        }
        return true;
    }
}

#endif
