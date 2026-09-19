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

# Unit::Blocked takes Motion::Reason bits (P5-C2). Its UnitState overload is deleted, so one
# unit-state constant passed to it is a compile error; but an ORed mask of unit-state constants
# is an int, binds to Blocked(uint32) and would be read as reason bits (UNIT_STAT_MELEE_ATTACKING
# is ReasonRooted's bit, UNIT_STAT_ATTACK_PLAYER ReasonStunned's, UNIT_STAT_ISOLATED
# ReasonConfused's). This gate refuses any Blocked( call whose argument names a UNIT_STAT_
# constant, in the core and in the scripts (P5-C3), and checks the deleted overload still stands.
#
# Usage: cmake -DSOURCE_ROOT=<repo root> -P CheckBlockedMasks.cmake
# Limitation: the regex reads a call's argument up to its first ')'; a mask built in a variable
# first would escape it, and so would a constant after a nested call's ')' inside the argument
# (Blocked(f() | UNIT_STAT_X)). None is today.

set(ROOTS "${SOURCE_ROOT}/src/game" "${SOURCE_ROOT}/src/modules")
set(SOURCES "")
foreach(ROOT_DIR IN LISTS ROOTS)
    if(NOT IS_DIRECTORY "${ROOT_DIR}")
        message(FATAL_ERROR "Blocked-mask gate: missing ${ROOT_DIR}")
    endif()
    file(GLOB_RECURSE FOUND "${ROOT_DIR}/*.cpp" "${ROOT_DIR}/*.h")
    list(LENGTH FOUND FOUND_COUNT)
    if(FOUND_COUNT EQUAL 0)
        message(FATAL_ERROR "Blocked-mask gate found no sources under ${ROOT_DIR} -- gate is inert")
    endif()
    list(APPEND SOURCES ${FOUND})
endforeach()
list(LENGTH SOURCES SOURCE_COUNT)

set(UNIT_HEADER "${SOURCE_ROOT}/src/game/Object/Unit.h")
file(READ "${UNIT_HEADER}" UNIT_TEXT)
string(FIND "${UNIT_TEXT}" "bool Blocked(UnitState) const = delete;" HAS_GUARD)
if(HAS_GUARD EQUAL -1)
    message(FATAL_ERROR "Blocked-mask gate: Unit.h no longer deletes Blocked(UnitState); a single unit-state constant would bind to Blocked(uint32)")
endif()

set(MEMBER_CALLS 0)
set(STRAYS "")
foreach(FILE_PATH IN LISTS SOURCES)
    file(READ "${FILE_PATH}" CONTENTS)
    string(FIND "${CONTENTS}" "Blocked" HAS_CALL)
    if(HAS_CALL EQUAL -1)
        continue()
    endif()
    if(NOT FILE_PATH STREQUAL UNIT_HEADER)
        string(REGEX MATCHALL "(->|\\.)Blocked[ \t\r\n]*\\(" MEMBER_HITS "${CONTENTS}")
        list(LENGTH MEMBER_HITS MEMBER_HIT_COUNT)
        math(EXPR MEMBER_CALLS "${MEMBER_CALLS} + ${MEMBER_HIT_COUNT}")
    endif()
    string(REGEX MATCHALL "Blocked[ \t\r\n]*\\([^)]*UNIT_STAT_[A-Z_]*" HITS "${CONTENTS}")
    foreach(HIT IN LISTS HITS)
        file(RELATIVE_PATH REL "${SOURCE_ROOT}" "${FILE_PATH}")
        string(REGEX REPLACE "[\r\n\t ]+" " " FLAT "${HIT}")
        list(APPEND STRAYS "${REL}: ${FLAT}")
    endforeach()
endforeach()

if(MEMBER_CALLS EQUAL 0)
    message(FATAL_ERROR "Blocked-mask gate saw no Blocked( call outside Unit.h -- gate is inert")
endif()
if(STRAYS)
    string(REPLACE ";" "\n  " PRETTY "${STRAYS}")
    message(FATAL_ERROR
        "Unit::Blocked called with a unit-state mask:\n  ${PRETTY}\n\n"
        "Blocked takes Motion::Reason bits (Motion::ReasonStunned, Motion::kCannotMoveReasons, ...);\n"
        "a UNIT_STAT_ constant there is read as the reason with the same bit value.")
endif()
message(STATUS "blocked masks: ${MEMBER_CALLS} Blocked( member calls outside Unit.h in ${SOURCE_COUNT} files, none with a UNIT_STAT_ argument")
