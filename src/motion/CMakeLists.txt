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

# The movement kernel's pure units (design v2 §3): what can change, which
# packet carries it, the pending-change machine, the session time base, and
# the writers. Nothing here knows a Unit, a Player or a World; the game side
# adapts (src/game/movement/). CheckMotionBoundary.cmake enforces that.
set(SRC_GRP_MOTION
    Change.h
    Change.cpp
    Authority.h
    Authority.cpp
    PacketMatrix.h
    PacketMatrix.cpp
    PendingChanges.h
    PendingChanges.cpp
    TimeBase.h
    TimeBase.cpp
    Writers.h
    Writers.cpp
    State.h
    State.cpp
    Mobility.h
    Mobility.cpp
    Arbiter.h
    Arbiter.cpp
    MoveIntent.h
    BehaviourModel.h
    SimpleMoves.h
    SimpleMoves.cpp
    DefaultMoves.h
    DefaultMoves.cpp
    PatrolWelding.h
    PatrolWelding.cpp
)
source_group("motion" FILES ${SRC_GRP_MOTION})

add_library(motion STATIC ${SRC_GRP_MOTION})

target_include_directories(motion
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(motion
    PUBLIC
        proto
)
