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

#include "wire/GuidCodec.h"

#include "Utilities/ByteBuffer.h"

namespace Wire
{
    uint64 MaskedGuid::Value() const
    {
        uint64 v = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (present[i]) { v |= uint64(byte[i]) << (8 * i); }
        }
        return v;
    }

    MaskedGuid MaskedGuid::Of(uint64 guid)
    {
        MaskedGuid g;
        for (int i = 0; i < 8; ++i)
        {
            const uint8 b = uint8((guid >> (8 * i)) & 0xFF);
            g.present[i] = b != 0;
            g.byte[i] = b;
        }
        return g;
    }

    void WritePackedGuid(ByteBuffer& out, uint64 guid)
    {
        uint8 mask = 0;
        uint8 bytes[8];
        for (int i = 0; i < 8; ++i)
        {
            bytes[i] = uint8((guid >> (8 * i)) & 0xFF);
            if (bytes[i] != 0) { mask |= uint8(1 << i); }
        }
        out << mask;
        for (int i = 0; i < 8; ++i)
        {
            if (bytes[i] != 0) { out << bytes[i]; }
        }
    }

    uint64 ReadPackedGuid(Detail::Reader& in)
    {
        const uint8 mask = in.Get<uint8>();
        uint64 value = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (mask & (1 << i)) { value |= uint64(in.Get<uint8>()) << (8 * i); }
        }
        return value;
    }
}
