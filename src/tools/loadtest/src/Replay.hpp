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

#pragma once

#include "Platform/Define.h"

#include <cstdio>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace loadtest
{
    /// One line of a movement capture: a direction, an opcode and its raw payload.
    struct CaptureLine
    {
        char               direction = 'C';   ///< 'C' client to server, 'S' server to client
        uint16             opcode = 0;
        std::vector<uint8> bytes;
    };

    /// Parses one capture line. Returns false for a blank or '#' comment line
    /// (skip it) and for a malformed one (`error` says why); true with `out` filled.
    bool ParseCaptureLine(std::string const& text, CaptureLine& out, std::string& error);

    /// The verdict for one opcode across a capture.
    struct ReplayRow
    {
        uint32 lines = 0;        ///< capture lines with this opcode
        uint32 unregistered = 0; ///< no layout and no family: counted, not judged
        uint32 embedded = 0;     ///< the layout is a block inside the packet (the cast opcodes): counted, not judged
        uint32 decoded = 0;      ///< decoded whole (ok, consumed == size)
        uint32 failed = 0;       ///< decode error or a short read
        uint32 exact = 0;        ///< decoded and re-encoded to the same bytes
        std::string firstProblem;///< first failure or inexact line, for the report
    };

    /// The verdict for a whole capture.
    struct ReplayReport
    {
        std::map<uint16, ReplayRow> byOpcode;
        /// `lines` counts the lines that PARSED; a malformed line is counted in
        /// `malformed` and nowhere else, so a file's total is lines + malformed.
        uint32 lines = 0, malformed = 0, unregistered = 0, embedded = 0, decoded = 0, failed = 0, exact = 0;

        /// True when every line the wire knows -- a registry layout or a family --
        /// decoded whole and re-encoded byte for byte, and nothing was malformed.
        /// Unregistered lines never make it false.
        bool Clean() const { return malformed == 0 && failed == 0 && exact == decoded; }
    };

    ReplayReport Replay(std::istream& capture);

    /// The report as the CLI prints it: a summary, then one row per opcode.
    void PrintReplay(ReplayReport const& report, std::FILE* to);
}
