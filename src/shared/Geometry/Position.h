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

// A POSE WITH NO FRAME: where, and which way, and nothing that says what those numbers are
// measured against. Two things this shape is used for, and they are the same thing:
//
//   - what the wire carries, decoded (MovementInfo);
//   - what a spline answers for one moment in time (MoveSpline::ComputePosition).
//
// Both are SAMPLES. A sample has no identity and no size, so it can be read and copied but
// never compared: no distance, no arc, no reach. Those live on Placement, which knows the
// frame and can refuse across one.
//
// The facing is NOT normalised here. A sample is what was measured or received, and the
// wire is entitled to send what it likes; Placement::Face normalises when a sample becomes
// a state.

#include "Geometry/Vector3.h"

namespace Geometry
{
    struct Position
    {
        Vector3 pos;
        float   facing;

        Position() : facing(0.0f) {}
        Position(float x, float y, float z, float o) : pos(x, y, z), facing(o) {}
        Position(const Vector3& p, float o) : pos(p), facing(o) {}
        explicit Position(const Vector3& p) : pos(p), facing(0.0f) {}

        const Vector3& Pos() const { return pos; }
        /// The mutable point, for the few callers that must hand an axis out as a float& --
        /// a spawn builder taking a free spot back from the collision selector, say.
        Vector3& Pos() { return pos; }
        float X() const { return pos.x; }
        float Y() const { return pos.y; }
        float Z() const { return pos.z; }
        float Facing() const { return facing; }

        void MoveTo(const Vector3& p) { pos = p; }
        void MoveTo(float x, float y, float z) { pos = Vector3(x, y, z); }
        void MoveTo(const Vector3& p, float o) { pos = p; facing = o; }
        void MoveTo(float x, float y, float z, float o) { pos = Vector3(x, y, z); facing = o; }
        void Face(float o) { facing = o; }

        bool IsFinite() const { return pos.isFinite() && Geometry::isFinite(facing); }
    };
}
