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

# 37 opcode names whose value disagrees with every independent 4.3.4 source and
# which nothing currently builds or dispatches. They are inert TODAY. Adopting
# one without correcting its value ships a silent no-op -- an SMSG the client
# discards, or a CMSG handler that never runs.
#
# The rule is deliberately blunt: any non-comment appearance of a denylisted
# name in src/game is a violation, full stop. An earlier version of this gate
# only matched a value bound to a named local on a single line -- "WorldPacket
# foo(NAME, ...)" -- which a wrapped constructor argument or an unnamed
# temporary (SendPacket(WorldPacket(NAME, 0))) walks straight past. There is no
# safe narrower shape to look for: the client does not care how the value
# reached it, only that it did.
#
# The one exemption is OPCODE(SMSG_..., ...) in OpcodeTable.cpp: a
# STATUS_NEVER + Handle_ServerSide table-completeness placeholder, never a
# dispatch target, because the server never receives an SMSG. A CMSG/MSG
# registration gets no such pass -- a wrong value there leaves a handler that
# is wired up and simply never runs, which is the exact failure this gate
# exists to catch.
#
# One alternation regex is built from all 37 names and matched once per line,
# rather than 37 names times two hand-written patterns times every line. The
# per-name loop cost 235 seconds over src/game -- as much as a warm build --
# for a gate meant to run on every configure.

set(DENYLIST_FILE "${CMAKE_CURRENT_LIST_DIR}/opcode_denylist.txt")
if(NOT EXISTS "${DENYLIST_FILE}")
    message(FATAL_ERROR "Opcode denylist missing: ${DENYLIST_FILE}")
endif()

file(STRINGS "${DENYLIST_FILE}" DENY_LINES REGEX "^(SMSG|CMSG|MSG)_[A-Z0-9_]+")
set(DENY_NAMES "")
foreach(LINE IN LISTS DENY_LINES)
    if(LINE MATCHES "^((SMSG|CMSG|MSG)_[A-Z0-9_]+)")
        list(APPEND DENY_NAMES "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(LENGTH DENY_NAMES DENY_COUNT)
if(DENY_COUNT EQUAL 0)
    message(FATAL_ERROR "Opcode denylist parsed to zero names -- gate is inert")
endif()

# Every name is [A-Z0-9_]+, so none need escaping to sit inside a regex
# alternation. The (^|[^...]) / ([^...]|$) wrapping is a word boundary: without
# it SMSG_CALENDAR_EVENT_INVITE_NOTES would also match inside the unrelated,
# and separately denylisted, SMSG_CALENDAR_EVENT_INVITE_NOTES_ALERT.
list(JOIN DENY_NAMES "|" DENY_ALT)
set(DENY_PATTERN "(^|[^A-Za-z0-9_])(${DENY_ALT})([^A-Za-z0-9_]|$)")

file(GLOB_RECURSE GAME_SOURCES "${SOURCE_ROOT}/src/game/*.cpp" "${SOURCE_ROOT}/src/game/*.h")
set(VIOLATIONS "")

foreach(FILE_PATH IN LISTS GAME_SOURCES)
    # Most of src/game never mentions an opcode name at all -- 428 of the
    # tree's 586 files, by count, when this was written. A whole-file check
    # against the same "MSG_" substring skips file(STRINGS) and the per-line
    # loop entirely for those, which is cheaper than entering the loop only
    # to skip every line of it one at a time.
    file(READ "${FILE_PATH}" FILE_CONTENTS)
    string(FIND "${FILE_CONTENTS}" "MSG_" FILE_HAS_MSG)
    if(FILE_HAS_MSG EQUAL -1)
        continue()
    endif()

    file(STRINGS "${FILE_PATH}" RAW_LINES)
    set(IN_BLOCK OFF)
    set(LINE_NO 0)
    foreach(LINE IN LISTS RAW_LINES)
        math(EXPR LINE_NO "${LINE_NO} + 1")

        # Block-comment state carries across lines, the same way
        # CheckProtoBoundary.cmake tracks it: a name mentioned only inside a
        # /* ... */ span opened on an earlier line is not an adoption.
        if(IN_BLOCK)
            string(FIND "${LINE}" "*/" CLOSE_AT)
            if(CLOSE_AT EQUAL -1)
                continue()
            endif()
            math(EXPR CLOSE_AT "${CLOSE_AT} + 2")
            string(SUBSTRING "${LINE}" ${CLOSE_AT} -1 LINE)
            set(IN_BLOCK OFF)
        endif()

        # A /* ... */ span that opens and closes on this same line is not
        # code either. Most lines carry no "/*" at all, and the collapsing
        # regex is by far the costliest single pattern here, so a plain
        # string(FIND) -- no regex engine, no backtracking -- gates whether
        # it runs at all. That guard is most of the difference between this
        # gate costing 235 seconds and costing single digits.
        string(FIND "${LINE}" "/*" OPEN_AT)
        if(NOT OPEN_AT EQUAL -1)
            string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" " " LINE "${LINE}")

            # An unclosed /* left after that opens a block that continues past
            # this line -- keep only the code before it.
            string(FIND "${LINE}" "/*" OPEN_AT)
            if(NOT OPEN_AT EQUAL -1)
                string(SUBSTRING "${LINE}" 0 ${OPEN_AT} LINE)
                set(IN_BLOCK ON)
            endif()
        endif()

        # A trailing // comment is not code, even on a line that carries real
        # code before it -- the one denylisted name that appears anywhere in
        # src/game outside OpcodeTable.cpp is exactly this shape: a remark
        # about a future change, not a use. Same reasoning as above: skip the
        # regex on the lines that plainly have no "//" to strip.
        string(FIND "${LINE}" "//" SLASHES_AT)
        if(NOT SLASHES_AT EQUAL -1)
            string(REGEX REPLACE "//.*$" "" LINE "${LINE}")
        endif()

        # Every denylisted name contains "MSG_" -- SMSG_, CMSG_ and MSG_ all
        # do -- so a line that lacks the substring cannot match DENY_PATTERN.
        # Checking that with string(FIND) first skips the alternation regex,
        # by construction the most expensive single check in this script, on
        # the overwhelming majority of lines that have no opcode name on them
        # at all.
        string(FIND "${LINE}" "MSG_" HAS_MSG)
        if(HAS_MSG EQUAL -1)
            continue()
        endif()

        if(LINE MATCHES "${DENY_PATTERN}")
            set(HIT_NAME "${CMAKE_MATCH_2}")

            string(STRIP "${LINE}" TRIMMED)
            if(FILE_PATH MATCHES "OpcodeTable\\.cpp$"
               AND TRIMMED MATCHES "^OPCODE[ \t]*\\([ \t]*SMSG_[A-Z0-9_]+[ \t]*,")
                # SMSG registration: a STATUS_NEVER + Handle_ServerSide table
                # placeholder, never a dispatch target, because the server
                # never receives an SMSG. Eleven denylisted SMSG names already
                # have such entries; flagging them would fail the gate on an
                # unmodified tree. A CMSG/MSG registration does not match this
                # pattern and is never exempt.
            else()
                list(APPEND VIOLATIONS "${FILE_PATH}:${LINE_NO}: ${HIT_NAME}")
            endif()
        endif()
    endforeach()
endforeach()

if(VIOLATIONS)
    string(REPLACE ";" "\n  " PRETTY "${VIOLATIONS}")
    message(FATAL_ERROR
        "Known-wrong opcode value adopted:\n  ${PRETTY}\n\n"
        "These names carry values the 4.3.4 client does not agree with. Using one\n"
        "ships a silent no-op. Correct the value in src/proto/Opcodes.h from the\n"
        "'correct' column of src/tests/opcode_denylist.txt, then remove the name\n"
        "from that file in the same commit.")
endif()

message(STATUS "opcode denylist: ${DENY_COUNT} names, no adoptions")
