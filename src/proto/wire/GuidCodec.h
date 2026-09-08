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

#ifndef MANGOS_WIRE_GUIDCODEC_H
#define MANGOS_WIRE_GUIDCODEC_H

#include "Platform/Define.h"
#include "wire/ByteReader.h"

#include "Utilities/ByteBuffer.h"

#include <cstddef>

namespace Wire
{
    /// A 4.3.4 masked GUID in flight: which bytes are present (the bits) and
    /// their values (the bytes, already un-XORed).
    struct MaskedGuid
    {
        bool  present[8] = { false, false, false, false, false, false, false, false };
        uint8 byte[8]    = { 0, 0, 0, 0, 0, 0, 0, 0 };
        uint64 Value() const;
        static MaskedGuid Of(uint64 guid);
    };

    template <size_t N>
    void WriteGuidMask(ByteBuffer& out, uint64 guid, const uint8 (&order)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            out.WriteBit(((guid >> (8 * order[i])) & 0xFF) != 0);
        }
    }

    template <size_t N>
    void WriteGuidBytes(ByteBuffer& out, uint64 guid, const uint8 (&order)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            const uint8 b = uint8((guid >> (8 * order[i])) & 0xFF);
            if (b != 0) { out << uint8(b ^ 1); }
        }
    }

    template <size_t N>
    void ReadGuidMask(Detail::Reader& in, MaskedGuid& g, const uint8 (&order)[N])
    {
        for (size_t i = 0; i < N; ++i) { g.present[order[i]] = in.Bit(); }
    }

    template <size_t N>
    void ReadGuidBytes(Detail::Reader& in, MaskedGuid& g, const uint8 (&order)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (g.present[order[i]]) { g.byte[order[i]] = in.Get<uint8>() ^ 1; }
        }
    }

    /// The classic packed GUID: one mask byte (bit i = byte i present), then the
    /// present bytes low to high. Nothing masks the values.
    void WritePackedGuid(ByteBuffer& out, uint64 guid);
    uint64 ReadPackedGuid(Detail::Reader& in);
}

#endif
