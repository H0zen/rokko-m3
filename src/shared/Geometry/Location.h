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

// A POSE AND WHAT IT IS MEASURED AGAINST -- Placement without the extent.
//
// Enough to say where something is; not enough to say whether it touches anything, which
// is the whole of the difference. A Location is what you STORE: a bind point, a teleport
// destination, a battleground entry, a taxi landing. It outlives the object it came from,
// so it has to carry its own frame rather than borrow one from whoever reads it back.
//
// The frame is the full key, map AND instance, where the struct this replaces carried a
// bare map id. A stored location is written with instance 0, which names the continent and
// therefore matches a live frame on it -- right for a homebind, and right for an instance
// bind too, which must NOT match the live copy it was taken in.

#include "Geometry/Frame.h"
#include "Geometry/Position.h"

namespace Geometry
{
    struct Location
    {
        Frame    frame;
        Position at;

        Location() {}
        Location(const Frame& f, const Position& p) : frame(f), at(p) {}
        Location(const Frame& f, float x, float y, float z, float o) : frame(f), at(x, y, z, o) {}

        /// The stored form: a map id, no instance. What the database and the DBCs speak.
        explicit Location(uint32_t mapId, float x = 0.0f, float y = 0.0f, float z = 0.0f, float o = 0.0f)
            : frame(Frame::World(mapId, 0)), at(x, y, z, o) {}

        bool IsPlaced() const { return frame.IsPlaced(); }
        bool ShareFrame(const Location& other) const { return frame.IsPlaced() && frame == other.frame; }

        uint32_t MapId() const { return frame.MapId(); }
        uint32_t InstanceId() const { return frame.InstanceId(); }

        const Vector3& Pos() const { return at.pos; }
        float X() const { return at.X(); }
        float Y() const { return at.Y(); }
        float Z() const { return at.Z(); }
        float Facing() const { return at.Facing(); }

        void MoveTo(float x, float y, float z, float o) { at.MoveTo(x, y, z, o); }
        void MoveTo(const Vector3& p) { at.MoveTo(p); }
        void Rebase(const Frame& f) { frame = f; }

        bool IsFinite() const { return at.IsFinite(); }
    };
}
