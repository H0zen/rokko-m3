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

# Rule zero, enforced: src/motion is the movement kernel's pure-unit seam. It
# may not reach the world, the database, the scripting engines or the game
# tree -- game adapts motion, never the reverse. Comments are stripped before
# matching, because the seam is documented in prose that necessarily names the
# very types the code may not touch.

set(MOTION_DIR "${SOURCE_ROOT}/src/motion")

if(NOT IS_DIRECTORY "${MOTION_DIR}")
    message(FATAL_ERROR "Motion boundary missing: ${MOTION_DIR}")
endif()

file(GLOB_RECURSE MOTION_SOURCES
    "${MOTION_DIR}/*.h" "${MOTION_DIR}/*.hpp"
    "${MOTION_DIR}/*.cpp" "${MOTION_DIR}/*.cc")

set(FORBIDDEN_PATTERNS
    "#[ \t]*include[ \t]*[\"<](Database/|World\\.h|WorldSession\\.h|AddonHandler\\.h|Warden)"
    "#[ \t]*include[ \t]*[\"<](Unit\\.h|Player\\.h|Creature\\.h|ObjectGuid\\.h|SharedDefines\\.h|Object/|WorldHandlers/|Server/|ChatCommands/|Maps/|MotionGenerators/|movement/)"
    "(^|[^A-Za-z0-9_])(WorldSession|sWorld|sLog|LoginDatabase|CharacterDatabase|WorldDatabase|sAddOnHandler|Warden)([^A-Za-z0-9_]|$)")

set(VIOLATIONS "")

foreach(FILE_PATH IN LISTS MOTION_SOURCES)
    file(STRINGS "${FILE_PATH}" RAW_LINES)

    set(IN_BLOCK OFF)
    set(CODE_ONLY "")

    foreach(LINE IN LISTS RAW_LINES)
        if(IN_BLOCK)
            string(FIND "${LINE}" "*/" CLOSE_AT)
            if(CLOSE_AT EQUAL -1)
                continue()
            endif()
            math(EXPR CLOSE_AT "${CLOSE_AT} + 2")
            string(SUBSTRING "${LINE}" ${CLOSE_AT} -1 LINE)
            set(IN_BLOCK OFF)
        endif()

        string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" " " LINE "${LINE}")

        string(FIND "${LINE}" "/*" OPEN_AT)
        if(NOT OPEN_AT EQUAL -1)
            string(SUBSTRING "${LINE}" 0 ${OPEN_AT} LINE)
            set(IN_BLOCK ON)
        endif()

        string(REGEX REPLACE "//.*$" "" LINE "${LINE}")

        string(APPEND CODE_ONLY "${LINE}\n")
    endforeach()

    foreach(PATTERN IN LISTS FORBIDDEN_PATTERNS)
        if(CODE_ONLY MATCHES "${PATTERN}")
            list(APPEND VIOLATIONS "${FILE_PATH}: ${CMAKE_MATCH_0}")
        endif()
    endforeach()
endforeach()

if(VIOLATIONS)
    string(REPLACE ";" "\n  " VIOLATIONS "${VIOLATIONS}")
    message(FATAL_ERROR "Forbidden game dependency in the motion library:\n  ${VIOLATIONS}")
endif()
