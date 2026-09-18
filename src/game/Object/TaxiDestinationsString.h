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
 */
namespace TaxiDestinationsString
{
    /// Empty for an empty route (the column's "not flying").
    inline std::string Format(uint32 faction, std::vector<uint32> const& nodes)
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
        return ss.str();
    }

    /// False on a token that is not a number; an empty string is an empty route.
    inline bool Parse(std::string const& values, uint32& faction, std::vector<uint32>& nodes)
    {
        faction = 0;
        nodes.clear();
        std::istringstream in(values);
        std::string token;
        bool first = true;
        while (in >> token)
        {
            char* end = NULL;
            const unsigned long value = std::strtoul(token.c_str(), &end, 10);
            if (end == token.c_str() || *end != '\0')
            {
                return false;
            }
            if (first)
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
