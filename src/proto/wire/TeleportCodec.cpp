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

#include "wire/TeleportCodec.h"
#include "wire/GuidCodec.h"
#include "wire/ByteReader.h"

#include "Utilities/ByteBuffer.h"

namespace
{
    const uint8 kMaskA[4] = { 6, 0, 3, 2 };
    const uint8 kMaskB[1] = { 1 };
    const uint8 kMaskC[3] = { 4, 7, 5 };
    const uint8 kTransportMask[8]  = { 1, 3, 2, 5, 0, 7, 6, 4 };
    const uint8 kTransportBytes[8] = { 5, 6, 1, 7, 0, 2, 4, 3 };
    const uint8 kBytesA[4] = { 1, 2, 3, 5 };
    const uint8 kBytesB[1] = { 4 };
    const uint8 kBytesC[1] = { 7 };
    const uint8 kBytesD[2] = { 0, 6 };
    const uint8 kAckMask[8]  = { 5, 0, 1, 6, 3, 7, 2, 4 };
    const uint8 kAckBytes[8] = { 4, 2, 7, 6, 5, 1, 3, 0 };
}

namespace Wire
{
    void EncodeTeleport(ByteBuffer& out, Teleport const& v)
    {
        WriteGuidMask(out, v.guid, kMaskA);
        out.WriteBit(v.hasVehicle);
        if (v.hasVehicle)
        {
            out.WriteBit(v.vehicleExitVoluntary);
            out.WriteBit(v.vehicleExitTeleport);
        }
        out.WriteBit(v.hasTransport);
        WriteGuidMask(out, v.guid, kMaskB);
        if (v.hasTransport) { WriteGuidMask(out, v.transportGuid, kTransportMask); }
        WriteGuidMask(out, v.guid, kMaskC);
        out.FlushBits();
        if (v.hasTransport) { WriteGuidBytes(out, v.transportGuid, kTransportBytes); }
        out << uint32(v.counter);
        WriteGuidBytes(out, v.guid, kBytesA);
        out << float(v.pos.x);
        WriteGuidBytes(out, v.guid, kBytesB);
        out << float(v.pos.o);
        WriteGuidBytes(out, v.guid, kBytesC);
        out << float(v.pos.z);
        // One byte, not four: the client's reader (sub_140363890) fetches a
        // single byte here -- `if (a1[64]) { GetByte(store, &v); a1[65] = v; }`
        // -- so a four-byte seat would desync the three fields after it.
        if (v.hasVehicle) { out << uint8(v.vehicleSeat); }
        WriteGuidBytes(out, v.guid, kBytesD);
        out << float(v.pos.y);
    }

    DecodeResult DecodeTeleport(ByteBuffer& in, Teleport& out)
    {
        return Detail::Run(in, out, [](Detail::Reader& r, Teleport& v)
        {
            MaskedGuid g, t;
            ReadGuidMask(r, g, kMaskA);
            v.hasVehicle = r.Bit();
            if (v.hasVehicle)
            {
                v.vehicleExitVoluntary = r.Bit();
                v.vehicleExitTeleport = r.Bit();
            }
            v.hasTransport = r.Bit();
            ReadGuidMask(r, g, kMaskB);
            if (v.hasTransport) { ReadGuidMask(r, t, kTransportMask); }
            ReadGuidMask(r, g, kMaskC);
            if (v.hasTransport) { ReadGuidBytes(r, t, kTransportBytes); }
            v.counter = r.Get<uint32>();
            ReadGuidBytes(r, g, kBytesA);
            v.pos.x = r.Get<float>();
            ReadGuidBytes(r, g, kBytesB);
            v.pos.o = r.Get<float>();
            ReadGuidBytes(r, g, kBytesC);
            v.pos.z = r.Get<float>();
            if (v.hasVehicle) { v.vehicleSeat = r.Get<uint8>(); }
            ReadGuidBytes(r, g, kBytesD);
            v.pos.y = r.Get<float>();
            v.guid = g.Value();
            v.transportGuid = v.hasTransport ? t.Value() : 0;
        });
    }

    void EncodeTeleportAck(ByteBuffer& out, TeleportAck const& v)
    {
        out << uint32(v.counter);
        out << uint32(v.time);
        WriteGuidMask(out, v.guid, kAckMask);
        out.FlushBits();
        WriteGuidBytes(out, v.guid, kAckBytes);
    }

    DecodeResult DecodeTeleportAck(ByteBuffer& in, TeleportAck& out)
    {
        return Detail::Run(in, out, [](Detail::Reader& r, TeleportAck& v)
        {
            v.counter = r.Get<uint32>();
            v.time = r.Get<uint32>();
            MaskedGuid g;
            ReadGuidMask(r, g, kAckMask);
            ReadGuidBytes(r, g, kAckBytes);
            v.guid = g.Value();
        });
    }
}
