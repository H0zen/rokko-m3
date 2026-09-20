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

#include "Version.h"
#include "BuildInfo.h"

#include <sstream>

// --- Which build this is ---------------------------------------------------

char const* Version::GetRelease()
{
    return MANGOS_RELEASE_STR;
}

char const* Version::GetRevision()
{
    return REVISION_STR;
}

char const* Version::GetFullVersion()
{
    return MANGOS_RELEASE_STR " (" REVISION_STR ")";
}

char const* Version::GetPackageName()
{
    return MANGOS_PACKAGENAME;
}

char const* Version::GetRunningSystem()
{
    return "Running on: " BUILD_HOST_SYSTEM;
}

// --- Where the installed configs are ---------------------------------------

char const* Version::GetMangosdConfigFile()
{
    return MANGOSD_CONFIG_LOCATION;
}

char const* Version::GetRealmdConfigFile()
{
    return REALMD_CONFIG_LOCATION;
}

char const* Version::GetAhbotConfigFile()
{
    return AUCTIONHOUSEBOT_CONFIG_LOCATION;
}

char const* Version::GetHash()
{
    return REVISION_HASH;
}

char const* Version::GetDate()
{
    return REVISION_DATE;
}

// --- Database schemas ------------------------------------------------------

char const* Version::GetRealmDBVersion()
{
    return REALMD_DB_VERSION_NR;
}

char const* Version::GetRealmDBStructure()
{
    return REALMD_DB_STRUCTURE_NR;
}

char const* Version::GetRealmDBContent()
{
    return REALMD_DB_CONTENT_NR;
}

char const* Version::GetRealmDBUpdateDescription()
{
    return REALMD_DB_UPDATE_DESCRIPT;
}

char const* Version::GetCharDBVersion()
{
    return CHAR_DB_VERSION_NR;
}

char const* Version::GetCharDBStructure()
{
    return CHAR_DB_STRUCTURE_NR;
}

char const* Version::GetCharDBContent()
{
    return CHAR_DB_CONTENT_NR;
}

char const* Version::GetCharDBUpdateDescription()
{
    return CHAR_DB_UPDATE_DESCRIPT;
}

char const* Version::GetWorldDBVersion()
{
    return WORLD_DB_VERSION_NR;
}

char const* Version::GetWorldDBStructure()
{
    return WORLD_DB_STRUCTURE_NR;
}

char const* Version::GetWorldDBContent()
{
    return WORLD_DB_CONTENT_NR;
}

char const* Version::GetWorldDBUpdateDescription()
{
    return WORLD_DB_UPDATE_DESCRIPT;
}

// --- Configuration files ---------------------------------------------------

uint32 Version::GetWorldConfigVersion()
{
    return MANGOSD_CONFIG_VERSION;
}

uint32 Version::GetRealmConfigVersion()
{
    return REALMD_CONFIG_VERSION;
}

uint32 Version::GetAhbotConfigVersion()
{
    return AHBOT_CONFIG_VERSION;
}

// --- Client ----------------------------------------------------------------

const std::vector<uint32>& Version::GetAcceptedClientBuilds()
{
    // The generated macro is a brace-init list terminated by a zero, which is
    // how the two call sites used to walk it. Unpack it once, here, so that
    // neither of them has to know the terminator exists.
    static const std::vector<uint32> builds = []
    {
        const int declared[] = EXPECTED_MANGOSD_CLIENT_BUILD;
        std::vector<uint32> out;
        for (size_t i = 0; declared[i]; ++i)
        {
            out.push_back(uint32(declared[i]));
        }
        return out;
    }();

    return builds;
}

std::string Version::GetAcceptedClientBuildsStr()
{
    std::ostringstream text;
    for (uint32 build : GetAcceptedClientBuilds())
    {
        text << build << " ";
    }
    return text.str();
}

bool Version::IsAcceptedClientBuild(uint32 build)
{
    for (uint32 accepted : GetAcceptedClientBuilds())
    {
        if (accepted == build)
        {
            return true;
        }
    }
    return false;
}

char const* Version::GetClientVersion()
{
    return EXPECTED_MANGOSD_CLIENT_VERSION;
}
