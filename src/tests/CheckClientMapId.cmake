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

# An update packet's header carries a map id, and the 4.3.4 client acts on it: a header
# naming a map other than the one the client is on is a WORLD CHANGE, which rebuilds its
# object manager, destroys its active player and faults on the next frame. A vessel's hull
# is a server-side map of its own (TransportMap), so a passenger's GetMapId() is the hull's
# -- a value the client has never heard of. Every sender must therefore build its UpdateData
# from GetClientMapId() (the vessel's world map for anyone aboard), which is what
# TransportMap's own senders already do through WorldMapIdOf.
#
# This gate refuses a raw GetMapId() in an UpdateData construction. It cost two client
# crashes to learn (the zeppelin boarding of 2026-09-14 and the login of 2026-09-20).
#
# Usage: cmake -DSOURCE_ROOT=<repo root> -P CheckClientMapId.cmake
# Limitation: the regex reads one construction's argument list up to its first ')'; a map id
# computed into a variable first would escape it. None is today.

set(ROOTS "${SOURCE_ROOT}/src/game" "${SOURCE_ROOT}/src/modules")
set(SOURCES "")
foreach(ROOT_DIR IN LISTS ROOTS)
    if(NOT IS_DIRECTORY "${ROOT_DIR}")
        message(FATAL_ERROR "Client-map-id gate: missing ${ROOT_DIR}")
    endif()
    file(GLOB_RECURSE FOUND "${ROOT_DIR}/*.cpp" "${ROOT_DIR}/*.h")
    if(NOT FOUND)
        message(FATAL_ERROR "Client-map-id gate: no sources under ${ROOT_DIR} -- gate is inert")
    endif()
    list(APPEND SOURCES ${FOUND})
endforeach()

set(CONSTRUCTIONS 0)
set(STRAYS "")
foreach(FILE_PATH IN LISTS SOURCES)
    file(READ "${FILE_PATH}" CONTENTS)
    string(FIND "${CONTENTS}" "UpdateData" HAS_USE)
    if(HAS_USE EQUAL -1)
        continue()
    endif()
    string(REGEX MATCHALL "UpdateData[ \t\r\n]*[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*\\(|UpdateData[ \t\r\n]*\\(" OPENS "${CONTENTS}")
    list(LENGTH OPENS OPEN_COUNT)
    math(EXPR CONSTRUCTIONS "${CONSTRUCTIONS} + ${OPEN_COUNT}")
    string(REGEX MATCHALL "UpdateData[ \t\r\n]*[A-Za-z_0-9]*[ \t\r\n]*\\([^)]*GetMapId[ \t\r\n]*\\(" HITS "${CONTENTS}")
    foreach(HIT IN LISTS HITS)
        file(RELATIVE_PATH REL "${SOURCE_ROOT}" "${FILE_PATH}")
        string(REGEX REPLACE "[\r\n\t ]+" " " FLAT "${HIT}")
        list(APPEND STRAYS "${REL}: ${FLAT})")
    endforeach()
endforeach()

if(CONSTRUCTIONS EQUAL 0)
    message(FATAL_ERROR "Client-map-id gate saw no UpdateData construction at all -- gate is inert")
endif()
if(STRAYS)
    string(REPLACE ";" "\n  " PRETTY "${STRAYS}")
    message(FATAL_ERROR
        "An update packet is built from a raw map id:\n  ${PRETTY}\n\n"
        "Build it from WorldObject::GetClientMapId() instead: a passenger's GetMapId() is the\n"
        "vessel's hull map, which the client reads as a world change and crashes on.")
endif()
message(STATUS "client map id: ${CONSTRUCTIONS} UpdateData construction(s), none from a raw GetMapId()")
