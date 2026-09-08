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

#include "wire/KnockBackCodec.h"
#include "wire/GuidCodec.h"
#include "wire/ByteReader.h"

#include "Utilities/ByteBuffer.h"

namespace
{
    const uint8 kMask[8]   = { 0, 3, 6, 7, 2, 5, 1, 4 };   // WorldSession::SendKnockBack
    const uint8 kBytes1[1] = { 1 };
    const uint8 kBytes67[2] = { 6, 7 };
    const uint8 kBytes453[3] = { 4, 5, 3 };
    const uint8 kBytes20[2] = { 2, 0 };
}

namespace Wire
{
    void EncodeKnockBack(ByteBuffer& out, KnockBack const& v)
    {
        WriteGuidMask(out, v.guid, kMask);
        out.FlushBits();
        WriteGuidBytes(out, v.guid, kBytes1);
        out << float(v.directionY);
        out << uint32(v.counter);
        WriteGuidBytes(out, v.guid, kBytes67);
        out << float(v.horizontal);
        WriteGuidBytes(out, v.guid, kBytes453);
        out << float(v.vertical);
        out << float(v.directionX);
        WriteGuidBytes(out, v.guid, kBytes20);
    }

    DecodeResult DecodeKnockBack(ByteBuffer& in, KnockBack& out)
    {
        return Detail::Run(in, out, [](Detail::Reader& r, KnockBack& v)
        {
            MaskedGuid g;
            ReadGuidMask(r, g, kMask);
            ReadGuidBytes(r, g, kBytes1);
            v.directionY = r.Get<float>();
            v.counter = r.Get<uint32>();
            ReadGuidBytes(r, g, kBytes67);
            v.horizontal = r.Get<float>();
            ReadGuidBytes(r, g, kBytes453);
            v.vertical = r.Get<float>();
            v.directionX = r.Get<float>();
            ReadGuidBytes(r, g, kBytes20);
            v.guid = g.Value();
        });
    }
}
