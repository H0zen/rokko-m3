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

#ifndef MANGOS_VERSION_H
#define MANGOS_VERSION_H

#include "Define.h"

#include <string>
#include <vector>

// Everything the generated BuildInfo.h holds, behind functions.
//
// Version.cpp is the ONLY translation unit that includes BuildInfo.h, and
// src/tests/CheckVersionSources.cmake keeps it that way. BuildInfo.h carries the
// commit hash, so it is rewritten by every commit: each file that included it
// recompiled on every commit, and eight of them did. Now one does, and the rest
// relink.
//
// Values that are NOT generated -- config file names, default ports, the player
// limit -- are in Common/ServerDefines.h, where a .cpp can have them for free.

namespace Version
{
    // Three phrasings of one answer, so that no caller assembles its own:
    //
    //   GetRelease()     "Mangos Three 0.22.0"
    //   GetRevision()    "a1b2c3 2026-09-20 12:00:00 +0000 (master branch)"
    //   GetFullVersion() both; what --version prints
    char const* GetRelease();
    char const* GetRevision();
    char const* GetFullVersion();

    char const* GetPackageName();
    char const* GetRunningSystem();

    // The checkout in parts, for the columns that record which build wrote a row.
    char const* GetHash();
    char const* GetDate();

    // --- Database schemas ----------------------------------------------------

    char const* GetRealmDBVersion();
    char const* GetRealmDBStructure();
    char const* GetRealmDBContent();
    char const* GetRealmDBUpdateDescription();

    char const* GetCharDBVersion();
    char const* GetCharDBStructure();
    char const* GetCharDBContent();
    char const* GetCharDBUpdateDescription();

    char const* GetWorldDBVersion();
    char const* GetWorldDBStructure();
    char const* GetWorldDBContent();
    char const* GetWorldDBUpdateDescription();

    // --- Configuration files -------------------------------------------------
    //
    // Compared against the ConfVersion in the loaded .conf by Config::CheckVersion.
    uint32 GetWorldConfigVersion();
    uint32 GetRealmConfigVersion();
    uint32 GetAhbotConfigVersion();

    // --- Where the installed configs are --------------------------------------
    //
    // SYSCONFDIR is generated, so these are functions like everything else here.
    char const* GetMangosdConfigFile();
    char const* GetRealmdConfigFile();
    char const* GetAhbotConfigFile();

    // --- Client --------------------------------------------------------------
    //
    // One build today; a list because the call sites name every accepted build
    // when they refuse one.
    const std::vector<uint32>& GetAcceptedClientBuilds();
    std::string GetAcceptedClientBuildsStr();
    bool IsAcceptedClientBuild(uint32 build);
    char const* GetClientVersion();
}

#endif
