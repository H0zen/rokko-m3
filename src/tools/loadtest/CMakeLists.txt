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
# mangos-loadtest -- a synthetic 4.3.4 client, for putting known load on a
# running mangosd and measuring how a session's traffic divides between the two
# world streams.
#
# It links `proto` for the opcode table and the packet buffer, and `shared` for
# the cipher and the hashes -- the same primitives the server verifies with, so a
# proof this builds is a proof the server will accept. It does NOT link `game`
# and does NOT link the database: a client has no world and no tables, and the
# one row it needs is planted by SQL the tool prints.
#
# Gated on BUILD_TOOLS: a server-only build has no use for a client.
# =============================================================================

if(NOT BUILD_TOOLS)
    return()
endif()

add_executable(mangos-loadtest
    src/LoadTest.cpp
    src/Framing.cpp
    src/Framing.hpp
    src/Socket.cpp
    src/Socket.hpp
    src/SyntheticClient.cpp
    src/SyntheticClient.hpp
    src/Peer.hpp
    src/TimeSync.cpp
    src/TimeSync.hpp
    src/Walker.cpp
    src/Walker.hpp
    src/Control.cpp
    src/Control.hpp
    src/AckEngine.cpp
    src/AckEngine.hpp
    src/Replay.hpp
    src/Replay.cpp
)

target_include_directories(mangos-loadtest PRIVATE src)

target_link_libraries(mangos-loadtest
    PRIVATE
        proto
        shared
)

if(MSVC)
    target_compile_options(mangos-loadtest PRIVATE /W4)
else()
    target_compile_options(mangos-loadtest PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_link_libraries(mangos-loadtest PRIVATE ws2_32)
endif()

set_target_properties(mangos-loadtest PROPERTIES FOLDER "tools")

install(TARGETS mangos-loadtest DESTINATION ${BIN_DIR}/tools)
