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
# The third-party libraries this tree builds. See README.md for versions and
# origins.
#
# The CMakeLists inside each subdirectory are OURS, not upstream's: they exist
# to expose one target each, under the name the rest of the build links against,
# out of a trimmed copy of the upstream sources. Nothing here mirrors an
# upstream build system.
#
# zlib and bzip2 are built only when the platform did not already provide them,
# which is why their targets are named for the find_package results they stand
# in for: a consumer links ZLIB::ZLIB and never learns which it got.
#
# NOTHING HERE IS WARNED ABOUT. These are upstream releases, imported whole and
# not patched (see README.md), so a warning raised in them is not a defect
# anyone here is going to act on -- and forty of them buried in the build output
# is forty lines an engineer learns to scroll past on the way to the ones that
# ARE ours. StormLib alone accounts for every -Wself-assign in the tree, all of
# them the `x = x;` idiom for silencing unused parameters.
#
# Fix the warnings by updating the library, not by editing it.
# =============================================================================

function(mangos_silence_vendored_warnings)
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            continue()
        endif()

        get_target_property(type ${target} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()

        if(MSVC)
            target_compile_options(${target} PRIVATE /w)
        else()
            target_compile_options(${target} PRIVATE -w)
        endif()
    endforeach()
endfunction()

if(NOT TARGET "ZLIB::ZLIB")
    add_subdirectory(zlib)
endif()

if(NOT TARGET "BZip2::BZip2")
    add_subdirectory(bzip2)
endif()

add_subdirectory(utf8cpp)

# Recast and Detour are needed by the server's pathfinder AND by the baker's
# navmesh stage, and the baker always builds, so this is not gated.
add_subdirectory(recastnavigation)

# The MPQ reader is not gated either: mep is what produces the
# tiles the server reads, so it is always built rather than being an optional
# extra that a fresh clone can leave out and then have nothing to run on.
add_subdirectory(StormLib)

# After every add_subdirectory above: the targets have to exist to be silenced.
# utf8cpp is header-only (an INTERFACE target with nothing to compile) and is
# skipped by the function.
mangos_silence_vendored_warnings(zlib bzip2 utf8 Recast Detour stormlib)
