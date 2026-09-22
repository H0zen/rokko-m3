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

#ifndef MANGOS_MOVEMENT_MONSTERMOVEPATH_H
#define MANGOS_MOVEMENT_MONSTERMOVEPATH_H

#include "Utilities/ByteBuffer.h"
#include "spline.h"

/**
 * THE PATH BODY of SMSG_MONSTER_MOVE, on its own so the suite can pin its bytes, exactly as
 * MonsterMoveStop.h carries the stop's. PacketBuilder::WriteMonsterMove picks one of the three
 * by the spline's flags and writes nothing else after it.
 *
 * The three forms and what the 15595 client does with them
 * (`peer/monster-move-wire-decompile-2026-09-21.md` §2, rows 20a/20b/20c):
 *
 *  - bit 22 `UncompressedPath` CLEAR -- the linear form: a point count, the destination as a
 *    full float3, and the points between it and the start as 11/11/10-bit offsets from the
 *    midpoint of the two ends. Four bytes an intermediate point, and the offsets wrap at
 *    +-255.75 yd.
 *  - bit 22 SET, not cyclic -- the Catmull-Rom form: a point count and that many full float3
 *    points. Twelve bytes a point, no range limit.
 *  - bit 22 SET and cyclic -- the same with the first point repeated at the head, which the
 *    client erases after the first cycle.
 *
 * The three agree on which points they carry. `SplineBase::InitCatmullRom` builds the point
 * array for BOTH interpolation modes (spline.cpp: "we should use catmullrom initializer even
 * for linear mode"), and it wraps the caller's `n` control points in two guards: one leading
 * virtual point a yard behind the first, built from the mover's facing, and one trailing
 * duplicate of the last. So `getPointCount() == n + 2`, and `count = getPointCount() - 3`
 * drops the leading guard, the START point -- which the packet's common part already carries
 * as the mover's position -- and the trailing guard. What goes on the wire is
 * `controls[1 .. n-1]` either way: n-1 points, the last of which is the destination.
 */
namespace Movement
{
    /**
     * @brief Writes a Vector3 as three floats, the order every point in this packet uses.
     */
    inline void operator << (ByteBuffer& b, const Vector3& v)
    {
        b << v.x << v.y << v.z;
    }

    /**
     * @brief Writes the compressed (linear) path body: count, destination, packed offsets.
     * @param spline The spline holding the path points, guards included.
     * @param data   The buffer to write to.
     */
    inline void WriteLinearPath(const Spline<int32>& spline, ByteBuffer& data)
    {
        uint32 last_idx = spline.getPointCount() - 3;
        const Vector3* real_path = &spline.getPoint(1);

        data << last_idx;
        data << real_path[last_idx];   // destination
        if (last_idx > 1)
        {
            Vector3 middle = (real_path[0] + real_path[last_idx]) / 2.f;
            Vector3 offset;
            // first and last points already appended
            for (uint32 i = 1; i < last_idx; ++i)
            {
                offset = middle - real_path[i];
                data.appendPackXYZ(offset.x, offset.y, offset.z);
            }
        }
    }

    /**
     * @brief Writes the uncompressed (Catmull-Rom) path body: count, then full float points.
     * @param spline The spline holding the path points, guards included.
     * @param data   The buffer to write to.
     */
    inline void WriteCatmullRomPath(const Spline<int32>& spline, ByteBuffer& data)
    {
        uint32 count = spline.getPointCount() - 3;
        data << count;
        data.append<Vector3>(&spline.getPoint(2), count);
    }

    /**
     * @brief Writes the cyclic uncompressed path body: the first point twice, then the points.
     * @param spline The spline holding the path points, guards included.
     * @param data   The buffer to write to.
     */
    inline void WriteCatmullRomCyclicPath(const Spline<int32>& spline, ByteBuffer& data)
    {
        uint32 count = spline.getPointCount() - 3;
        data << uint32(count + 1);
        data << spline.getPoint(1); // fake point, client will erase it from the spline after first cycle done
        data.append<Vector3>(&spline.getPoint(1), count);
    }
}

#endif
