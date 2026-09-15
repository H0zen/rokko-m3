# SPDX-License-Identifier: GPL-3.0-or-later
#
# MaNGOS is a full featured server for World of Warcraft, supporting
# the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
#
# Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# =============================================================================
# Every number in this tree that changes because something OUTSIDE the code
# changed: a config file gained a key, a database schema moved, a client build
# was retargeted. One file, so that bumping one is a single edit and so that the
# things which have to agree cannot be edited apart.
#
# They were previously spread across four places, and two of them had already
# drifted: the auction-house bot's config version existed three times -- here,
# hardcoded in AuctionHouseBot.cpp, and hardcoded again in ahbot.conf.dist.in --
# with the copy in this file holding a value neither of the other two had ever
# used. Bumping it changed nothing, which is the worst way for a version to
# fail. Everything below is now substituted into whatever consumes it, so a
# value that is wrong is wrong everywhere at once and visibly.
#
# What does NOT belong here: anything the build discovers rather than declares
# (the git hash and date, the CMake and host versions), and the realm server's
# table of every client build it has ever known, which is a policy table and not
# a version.
# =============================================================================

# --- Server identity ---------------------------------------------------------
#
# The release version itself is `project(MaNGOS VERSION ...)` in the top-level
# CMakeLists, because CMake has to have it before anything else runs.

set(MANGOS_PKG "Mangos Three")

# Bumped per release. Cosmetic: reported by `.server info` and in the console
# banner, never compared against anything.
set(MANGOS_REVISION_NR "2201001")

# --- Configuration files -----------------------------------------------------
#
# Format YYYYMMDDRR, where RR distinguishes two bumps on the same day.
#
# Each server compares the number below against the `ConfVersion` in the .conf
# it actually loaded, and warns when they differ. So bump the matching one
# whenever a key is ADDED, REMOVED or RENAMED in that file's .dist.in -- that is
# what makes an operator running last month's config hear about it, instead of
# silently getting a default for a setting they believe they set.
#
# Changing only a default value is not a reason to bump: an operator's existing
# file stays valid, and a spurious warning is how these come to be ignored.

set(MANGOS_WORLD_VER 2026082900)
set(MANGOS_REALM_VER 2021010100)

# This one reads oddly next to the other two because it is the value that has
# actually been in force all along, carried over from the two hardcoded copies
# rather than from the unused one this file used to declare. Left as-is on
# purpose: adopting the never-used number would warn every existing deployment
# about an ahbot.conf that has not changed.
set(MANGOS_AHBOT_VER 2010102201)

# --- Databases ---------------------------------------------------------------
#
# Reported by the server and used to tell an operator which update they are
# missing. Bump alongside the SQL that changes the schema.
#
#   VERSION    the release line the schema belongs to
#   STRUCTURE  incremented by a change to tables, columns or indexes
#   CONTENT    incremented by a change to the data those tables hold
#   DESCRIPT   short name of the last update applied, for the log line

set(MANGOS_REALMD_DB_VERSION   "22")
set(MANGOS_REALMD_DB_STRUCTURE "5")
set(MANGOS_REALMD_DB_CONTENT   "1")
set(MANGOS_REALMD_DB_DESCRIPT  "Remove_Playerbots")

set(MANGOS_CHAR_DB_VERSION   "22")
set(MANGOS_CHAR_DB_STRUCTURE "10")
set(MANGOS_CHAR_DB_CONTENT   "1")
set(MANGOS_CHAR_DB_DESCRIPT  "Remove_Playerbots")

set(MANGOS_WORLD_DB_VERSION   "22")
set(MANGOS_WORLD_DB_STRUCTURE "10")
set(MANGOS_WORLD_DB_CONTENT   "3")
set(MANGOS_WORLD_DB_DESCRIPT  "Pinfo_Both_Latencies")

# --- Client ------------------------------------------------------------------
#
# One build, deliberately. Supporting two means half the server works for one
# set of players and the other half for the other, and neither half is testable.
# This gates the DBC/DB2 files the extractor produced and the build a client
# claims in CMSG_AUTH_SESSION.

set(MANGOS_CLIENT_BUILD   15595)
set(MANGOS_CLIENT_VERSION "4.3.4")
