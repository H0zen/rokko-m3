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

// THE THREE NUMBERS THAT SAY WHETHER THIS WORKS.
//
// Not impressions. Each one answers a question the design made a claim about, and each one
// has a value that means the claim is false.
//
//   VISITS vs DECISIONS. The claim is that a mover with nothing due costs a comparison, so
//   the two should differ by orders of magnitude. If they are close, movers are waking for
//   no reason and the whole scheduling idea has bought nothing.
//
//   PACKETS, and POINTS PER PACKET. The claim is that traffic falls: nothing is sent to turn
//   a creature (the client takes the facing from the tangent), and a welded patrol sends
//   many nodes in one packet instead of one each. Turns near zero and points-per-packet
//   above one are what that looks like. Compare on the SAME scene, not against an absolute.
//
//   REFUSALS, by category. This one must be ZERO in ordinary play. A refusal means a shape
//   asked for something the client would have mishandled -- and since a refused spline
//   teleports the mover, the refusal is the only thing standing between a bug and a visible
//   one. Any recurring refusal is a defect in the shape that produced it, never a reason to
//   loosen the writer.

#include "Platform/Define.h"

#include <functional>
#include <string>

namespace MoveStats
{
    /// A mover was visited by its unit's update and had nothing due.
    void Visited();
    /// A mover was actually asked what to do.
    void Decided();
    /// A leg went out, carrying `points` points.
    void Sent(uint32 points);
    /// A turn-on-the-spot packet went out. Should stay near zero.
    void Turned();
    /// A leg was refused; `refusal` is the Move::Refusal value. `who` and `entry` name the
    /// mover, because a refusal is a defect in whatever asked for that leg and the first
    /// question is always which creature it was.
    void Refused(uint8 refusal, uint64 who, uint32 entry);

    /// Called from the world update. Writes the block below to the log on its own schedule,
    /// so the numbers are there to read afterwards without anyone having been in-game with
    /// a GM account at the right moment.
    void Tick(uint32 diff);

    /// Print every counter through `line`, then start counting again.
    void ReportAndReset(std::function<void(std::string const&)> const& line);
}
