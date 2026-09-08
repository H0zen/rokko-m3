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

#include "wire/MonsterMoveCodec.h"
#include "wire/GuidCodec.h"
#include "wire/ByteReader.h"

#include "Opcodes.h"
#include "Utilities/ByteBuffer.h"

#include <vector>

namespace Wire
{
    namespace
    {
        void WriteVec3(ByteBuffer& out, Vec3 const& v) { out << float(v.x) << float(v.y) << float(v.z); }
        Vec3 ReadVec3(Detail::Reader& r) { Vec3 v; v.x = r.Get<float>(); v.y = r.Get<float>(); v.z = r.Get<float>(); return v; }
    }

    void EncodeMonsterMove(ByteBuffer& out, uint16 opcode, MonsterMove const& v)
    {
        WritePackedGuid(out, v.mover);
        if (opcode == SMSG_MONSTER_MOVE_TRANSPORT)
        {
            WritePackedGuid(out, v.transport);
            out << int8(v.seat);
        }
        out << uint8(v.exitVoluntary);
        WriteVec3(out, v.start);
        out << uint32(v.id);
        out << uint8(v.type);
        switch (v.type)
        {
            case MonsterMoveType::Stop:         return;                 // nothing follows a stop
            case MonsterMoveType::FacingTarget: out << uint64(v.facingTarget); break;
            case MonsterMoveType::FacingAngle:  out << float(v.facingAngle);   break;
            case MonsterMoveType::FacingSpot:   WriteVec3(out, v.facingSpot);  break;
            case MonsterMoveType::Normal:       break;
        }
        out << uint32(v.flags);
        if (v.flags & kSplineFlagAnimation)
        {
            out << uint8(v.animationId);
            out << int32(v.animationStart);
        }
        out << uint32(v.duration);
        if (v.flags & kSplineFlagTrajectory)
        {
            out << float(v.verticalAcceleration);
            out << int32(v.parabolicStart);
        }
        // The flag, not `path`: the flag is what is on the wire, and it is what
        // the decoder branches on, so the encoder follows the same thing. `path`
        // is the decode output that reports which branch was taken.
        if (v.flags & kSplineFlagUncompressedPath)
        {
            out << uint32(v.points.size());
            for (size_t i = 0; i < v.points.size(); ++i) { WriteVec3(out, v.points[i]); }
        }
        else
        {
            out << uint32(v.packedOffsets.size() + 1);      // WriteLinearPath's last index
            WriteVec3(out, v.destination);
            for (size_t i = 0; i < v.packedOffsets.size(); ++i) { out << uint32(v.packedOffsets[i]); }
        }
    }

    DecodeResult DecodeMonsterMove(ByteBuffer& in, uint16 opcode, MonsterMove& out)
    {
        const bool transport = (opcode == SMSG_MONSTER_MOVE_TRANSPORT);
        return Detail::Run(in, out, [transport](Detail::Reader& r, MonsterMove& v)
        {
            v.mover = ReadPackedGuid(r);
            v.onTransport = transport;
            if (transport)
            {
                v.transport = ReadPackedGuid(r);
                v.seat = r.Get<int8>();
            }
            v.exitVoluntary = r.Get<uint8>();
            v.start = ReadVec3(r);
            v.id = r.Get<uint32>();
            const uint8 type = r.Get<uint8>();
            if (type > uint8(MonsterMoveType::FacingAngle)) { throw Detail::Overread(); }   // not a shape we know: refuse, whole-or-nothing
            v.type = MonsterMoveType(type);
            switch (v.type)
            {
                case MonsterMoveType::Stop:         return;
                case MonsterMoveType::FacingTarget: v.facingTarget = r.Get<uint64>(); break;
                case MonsterMoveType::FacingAngle:  v.facingAngle = r.Get<float>();   break;
                case MonsterMoveType::FacingSpot:   v.facingSpot = ReadVec3(r);       break;
                case MonsterMoveType::Normal:       break;
            }
            v.flags = r.Get<uint32>();
            if (v.flags & kSplineFlagAnimation)
            {
                v.animationId = r.Get<uint8>();
                v.animationStart = r.Get<int32>();
            }
            v.duration = r.Get<uint32>();
            if (v.flags & kSplineFlagTrajectory)
            {
                v.verticalAcceleration = r.Get<float>();
                v.parabolicStart = r.Get<int32>();
            }
            const uint32 count = r.Get<uint32>();
            // Bound the count by the bytes that remain before reserving anything: a
            // count of 4 billion must not become a 4-billion-element reserve just
            // because the packet claimed one. Whole-or-nothing: refuse as an
            // overread rather than reserve first and fail the loop partway through.
            const size_t left = r.in.size() - r.in.rpos();
            if (v.flags & kSplineFlagUncompressedPath)
            {
                v.path = SplinePath::Uncompressed;
                if (count > left / 12) { throw Detail::Overread(); }
                v.points.reserve(count);
                for (uint32 i = 0; i < count; ++i) { v.points.push_back(ReadVec3(r)); }
            }
            else
            {
                v.path = SplinePath::Linear;
                // The linear form spends twelve of those bytes on the destination
                // before the first packed offset, so the offsets are bounded by
                // what is left after it -- and a count of 0 is not a path at all
                // (WriteLinearPath writes the last index, never -1).
                if (count == 0 || left < 12 || count - 1 > (left - 12) / 4) { throw Detail::Overread(); }
                v.destination = ReadVec3(r);
                v.packedOffsets.reserve(count - 1);
                for (uint32 i = 1; i < count; ++i) { v.packedOffsets.push_back(r.Get<uint32>()); }
            }
        });
    }
}
