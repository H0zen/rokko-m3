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

#include "wire/MovementCodec.h"
#include "wire/ByteReader.h"
#include "wire/GuidCodec.h"

#include "Utilities/ByteBuffer.h"

namespace Wire
{
    namespace
    {
        inline bool InRange(Element e, Element first, Element last)
        {
            return int(e) >= int(first) && int(e) <= int(last);
        }

        inline int Index(Element e, Element first)
        {
            return int(e) - int(first);
        }
    }

    void Encode(ByteBuffer& out, Sequence sequence, MovementStatus const& status)
    {
        if (!sequence)
        {
            return;
        }

        const MaskedGuid guid = MaskedGuid::Of(status.guid);
        const MaskedGuid guid2 = MaskedGuid::Of(status.guid2);
        const MaskedGuid tguid = MaskedGuid::Of(status.transport.guid);
        const bool hasTransport = status.transport.present;
        // Presence is derived from the value, except that a decoded packet may have
        // announced a block that carried zero; the status remembers that case so it
        // re-encodes byte-identical instead of dropping the block and shifting the rest.
        const bool hasFlags = status.flags != 0 || status.has.emptyFlagsBlock;
        const bool hasFlags2 = status.flags2 != 0 || status.has.emptyFlags2Block;

        for (Sequence p = sequence; *p != Element::End; ++p)
        {
            const Element e = *p;

            if (InRange(e, Element::GuidBit0, Element::GuidBit7))
            {
                out.WriteBit(guid.present[Index(e, Element::GuidBit0)]);
                continue;
            }
            if (InRange(e, Element::Guid2Bit0, Element::Guid2Bit7))
            {
                out.WriteBit(guid2.present[Index(e, Element::Guid2Bit0)]);
                continue;
            }
            if (InRange(e, Element::TransportGuidBit0, Element::TransportGuidBit7))
            {
                if (hasTransport)
                {
                    out.WriteBit(tguid.present[Index(e, Element::TransportGuidBit0)]);
                }
                continue;
            }
            if (InRange(e, Element::GuidByte0, Element::GuidByte7))
            {
                const int i = Index(e, Element::GuidByte0);
                if (guid.present[i])
                {
                    out << uint8(guid.byte[i] ^ 1);
                }
                continue;
            }
            if (InRange(e, Element::Guid2Byte0, Element::Guid2Byte7))
            {
                const int i = Index(e, Element::Guid2Byte0);
                if (guid2.present[i])
                {
                    out << uint8(guid2.byte[i] ^ 1);
                }
                continue;
            }
            if (InRange(e, Element::TransportGuidByte0, Element::TransportGuidByte7))
            {
                const int i = Index(e, Element::TransportGuidByte0);
                if (hasTransport && tguid.present[i])
                {
                    out << uint8(tguid.byte[i] ^ 1);
                }
                continue;
            }

            switch (e)
            {
                case Element::HasMovementFlags:   out.WriteBit(!hasFlags);                     break;
                case Element::HasMovementFlags2:  out.WriteBit(!hasFlags2);                    break;
                case Element::Flags:              if (hasFlags)  { out.WriteBits(status.flags, 30); }  break;
                case Element::Flags2:             if (hasFlags2) { out.WriteBits(status.flags2, 12); } break;
                case Element::HasTimestamp:       out.WriteBit(!status.has.timestamp);         break;
                case Element::Timestamp:          if (status.has.timestamp) { out << uint32(status.time); } break;
                case Element::HasPitch:           out.WriteBit(!status.has.pitch);             break;
                case Element::Pitch:              if (status.has.pitch) { out << float(status.pitch); } break;
                case Element::HasOrientation:     out.WriteBit(!status.has.orientation);       break;
                case Element::HasSpline:          out.WriteBit(status.has.spline);             break;
                case Element::HasSplineElevation: out.WriteBit(!status.has.splineElevation);   break;
                case Element::SplineElevation:    if (status.has.splineElevation) { out << float(status.splineElevation); } break;
                case Element::HasUnknownBit:      out.WriteBit(status.has.unknownBit);         break;
                case Element::PositionX:          out << float(status.pos.x);                  break;
                case Element::PositionY:          out << float(status.pos.y);                  break;
                case Element::PositionZ:          out << float(status.pos.z);                  break;
                case Element::PositionO:          if (status.has.orientation) { out << float(status.pos.o); } break;

                case Element::HasFallData:        out.WriteBit(status.fall.present);           break;
                case Element::HasFallDirection:   if (status.fall.present) { out.WriteBit(status.fall.hasDirection); } break;
                case Element::FallTime:           if (status.fall.present) { out << uint32(status.fall.time); } break;
                case Element::FallVerticalSpeed:  if (status.fall.present) { out << float(status.fall.vertical); } break;
                case Element::FallHorizontalSpeed:
                    if (status.fall.present && status.fall.hasDirection) { out << float(status.fall.horizontal); }
                    break;
                case Element::FallCosAngle:
                    if (status.fall.present && status.fall.hasDirection) { out << float(status.fall.cosAngle); }
                    break;
                case Element::FallSinAngle:
                    if (status.fall.present && status.fall.hasDirection) { out << float(status.fall.sinAngle); }
                    break;

                case Element::HasTransportData:   out.WriteBit(hasTransport);                  break;
                case Element::HasTransportTime2:  if (hasTransport) { out.WriteBit(status.transport.hasTime2); } break;
                case Element::HasVehicleId:       if (hasTransport) { out.WriteBit(status.transport.hasVehicleId); } break;
                case Element::TransportSeat:      if (hasTransport) { out << int8(status.transport.seat); } break;
                case Element::TransportPositionX: if (hasTransport) { out << float(status.transport.pos.x); } break;
                case Element::TransportPositionY: if (hasTransport) { out << float(status.transport.pos.y); } break;
                case Element::TransportPositionZ: if (hasTransport) { out << float(status.transport.pos.z); } break;
                case Element::TransportPositionO: if (hasTransport) { out << float(status.transport.pos.o); } break;
                case Element::TransportTime:      if (hasTransport) { out << uint32(status.transport.time); } break;
                case Element::TransportTime2:
                    if (hasTransport && status.transport.hasTime2) { out << uint32(status.transport.time2); }
                    break;
                case Element::TransportVehicleId:
                    if (hasTransport && status.transport.hasVehicleId) { out << uint32(status.transport.vehicleId); }
                    break;

                case Element::MovementCounter:    out << uint32(status.counter);               break;
                case Element::ByteParam:          out << int8(status.byteParam);               break;

                case Element::ExtraFloat:            out << float(status.value);                         break;
                case Element::ExtraTwoBits:          out.WriteBits(status.twoBits, 2);                   break;
                case Element::HasHeightChangeFailed: out.WriteBit(status.has.heightChangeFailed);        break;
                case Element::OneBit:                out.WriteBit(true);                                 break;
                case Element::FlushBits:             out.FlushBits();                                    break;

                default:
                    // Every Element has a case above; only a value cast from outside the enum
                    // lands here. Nothing is written -- which would corrupt the packet for the
                    // client -- so the registry test walks every table to keep this unreachable.
                    break;
            }
        }

        out.FlushBits();
    }

    namespace
    {
        DecodeResult DecodeUnchecked(ByteBuffer& buffer, Sequence sequence, MovementStatus& out)
        {
            Detail::Reader in(buffer);
            MaskedGuid guid, guid2, tguid;
            bool hasFlags = false;
            bool hasFlags2 = false;

            for (Sequence p = sequence; *p != Element::End; ++p)
            {
                const Element e = *p;

                if (InRange(e, Element::GuidBit0, Element::GuidBit7))
                {
                    guid.present[Index(e, Element::GuidBit0)] = in.Bit();
                    continue;
                }
                if (InRange(e, Element::Guid2Bit0, Element::Guid2Bit7))
                {
                    guid2.present[Index(e, Element::Guid2Bit0)] = in.Bit();
                    continue;
                }
                if (InRange(e, Element::TransportGuidBit0, Element::TransportGuidBit7))
                {
                    if (out.transport.present)
                    {
                        tguid.present[Index(e, Element::TransportGuidBit0)] = in.Bit();
                    }
                    continue;
                }
                if (InRange(e, Element::GuidByte0, Element::GuidByte7))
                {
                    const int i = Index(e, Element::GuidByte0);
                    if (guid.present[i])
                    {
                        guid.byte[i] = in.Get<uint8>() ^ 1;
                    }
                    continue;
                }
                if (InRange(e, Element::Guid2Byte0, Element::Guid2Byte7))
                {
                    const int i = Index(e, Element::Guid2Byte0);
                    if (guid2.present[i])
                    {
                        guid2.byte[i] = in.Get<uint8>() ^ 1;
                    }
                    continue;
                }
                if (InRange(e, Element::TransportGuidByte0, Element::TransportGuidByte7))
                {
                    const int i = Index(e, Element::TransportGuidByte0);
                    if (out.transport.present && tguid.present[i])
                    {
                        tguid.byte[i] = in.Get<uint8>() ^ 1;
                    }
                    continue;
                }

                switch (e)
                {
                    case Element::HasMovementFlags:   hasFlags = !in.Bit();                        break;
                    case Element::HasMovementFlags2:  hasFlags2 = !in.Bit();                       break;
                    case Element::Flags:
                        if (hasFlags)
                        {
                            out.flags = in.Bits(30);
                            out.has.emptyFlagsBlock = (out.flags == 0);
                        }
                        break;
                    case Element::Flags2:
                        if (hasFlags2)
                        {
                            out.flags2 = in.Bits(12);
                            out.has.emptyFlags2Block = (out.flags2 == 0);
                        }
                        break;
                    case Element::HasTimestamp:       out.has.timestamp = !in.Bit();               break;
                    case Element::Timestamp:          if (out.has.timestamp) { out.time = in.Get<uint32>(); } break;
                    case Element::HasPitch:           out.has.pitch = !in.Bit();                   break;
                    case Element::Pitch:              if (out.has.pitch) { out.pitch = in.Get<float>(); } break;
                    case Element::HasOrientation:     out.has.orientation = !in.Bit();             break;
                    case Element::HasSpline:          out.has.spline = in.Bit();                   break;
                    case Element::HasSplineElevation: out.has.splineElevation = !in.Bit();         break;
                    case Element::SplineElevation:    if (out.has.splineElevation) { out.splineElevation = in.Get<float>(); } break;
                    case Element::HasUnknownBit:      out.has.unknownBit = in.Bit();               break;
                    case Element::PositionX:          out.pos.x = in.Get<float>();                    break;
                    case Element::PositionY:          out.pos.y = in.Get<float>();                    break;
                    case Element::PositionZ:          out.pos.z = in.Get<float>();                    break;
                    case Element::PositionO:          if (out.has.orientation) { out.pos.o = in.Get<float>(); } break;

                    case Element::HasFallData:        out.fall.present = in.Bit();                 break;
                    case Element::HasFallDirection:   if (out.fall.present) { out.fall.hasDirection = in.Bit(); } break;
                    case Element::FallTime:           if (out.fall.present) { out.fall.time = in.Get<uint32>(); } break;
                    case Element::FallVerticalSpeed:  if (out.fall.present) { out.fall.vertical = in.Get<float>(); } break;
                    case Element::FallHorizontalSpeed:
                        if (out.fall.present && out.fall.hasDirection) { out.fall.horizontal = in.Get<float>(); }
                        break;
                    case Element::FallCosAngle:
                        if (out.fall.present && out.fall.hasDirection) { out.fall.cosAngle = in.Get<float>(); }
                        break;
                    case Element::FallSinAngle:
                        if (out.fall.present && out.fall.hasDirection) { out.fall.sinAngle = in.Get<float>(); }
                        break;

                    case Element::HasTransportData:   out.transport.present = in.Bit();            break;
                    case Element::HasTransportTime2:  if (out.transport.present) { out.transport.hasTime2 = in.Bit(); } break;
                    case Element::HasVehicleId:       if (out.transport.present) { out.transport.hasVehicleId = in.Bit(); } break;
                    case Element::TransportSeat:      if (out.transport.present) { out.transport.seat = in.Get<int8>(); } break;
                    case Element::TransportPositionX: if (out.transport.present) { out.transport.pos.x = in.Get<float>(); } break;
                    case Element::TransportPositionY: if (out.transport.present) { out.transport.pos.y = in.Get<float>(); } break;
                    case Element::TransportPositionZ: if (out.transport.present) { out.transport.pos.z = in.Get<float>(); } break;
                    case Element::TransportPositionO: if (out.transport.present) { out.transport.pos.o = in.Get<float>(); } break;
                    case Element::TransportTime:      if (out.transport.present) { out.transport.time = in.Get<uint32>(); } break;
                    case Element::TransportTime2:
                        if (out.transport.present && out.transport.hasTime2) { out.transport.time2 = in.Get<uint32>(); }
                        break;
                    case Element::TransportVehicleId:
                        if (out.transport.present && out.transport.hasVehicleId) { out.transport.vehicleId = in.Get<uint32>(); }
                        break;

                    case Element::MovementCounter:    out.counter = in.Get<uint32>();                 break;
                    case Element::ByteParam:          out.byteParam = in.Get<int8>();                 break;

                    case Element::ExtraFloat:            out.value = in.Get<float>();                      break;
                    case Element::ExtraTwoBits:          out.twoBits = uint8(in.Bits(2));               break;
                    case Element::HasHeightChangeFailed: out.has.heightChangeFailed = in.Bit();         break;
                    case Element::OneBit:                in.Bit();                                      break;
                    case Element::FlushBits:             in.Reset();                               break;

                    default:
                    {
                        DecodeResult bad;
                        bad.error = DecodeError::BadElement;
                        return bad;
                    }
                }
            }

            out.guid = guid.Value();
            out.guid2 = guid2.Value();
            out.transport.guid = out.transport.present ? tguid.Value() : 0;
            return DecodeResult();
        }
    }

    DecodeResult Decode(ByteBuffer& in, Sequence sequence, MovementStatus& out)
    {
        DecodeResult result;
        if (!sequence)
        {
            result.error = DecodeError::NoSequence;
            return result;
        }

        const size_t start = in.rpos();
        MovementStatus candidate;
        try
        {
            result = DecodeUnchecked(in, sequence, candidate);
        }
        catch (Detail::Overread const&)
        {
            result.error = DecodeError::Overread;
        }
        catch (ByteBufferException const&)
        {
            // Backstop only: the Reader checks every bound first, so this is
            // reached only if a caller broke the byte-boundary precondition.
            result.error = DecodeError::Overread;
        }
        result.consumed = in.rpos() - start;
        // Nothing half-read reaches the caller: the whole status, or the defaults.
        out = result.ok() ? candidate : MovementStatus();
        return result;
    }

    bool DecodeWhole(ByteBuffer const& packet, Sequence sequence, MovementStatus& out,
                     DecodeResult& result, bool pendingWriteBits)
    {
        ByteBuffer copy(packet);
        if (pendingWriteBits)
        {
            copy.FlushBits();
        }
        copy.rpos(0);
        copy.ResetBitReader();
        result = Decode(copy, sequence, out);
        if (result.ok() && result.consumed != copy.size())
        {
            result.error = DecodeError::LeftBytes;
            out = MovementStatus();                       // whole, or nothing
        }
        return result.ok();
    }

    char const* ErrorName(DecodeError error)
    {
        switch (error)
        {
            case DecodeError::None:       return "ok";
            case DecodeError::NoSequence: return "no layout";
            case DecodeError::Overread:   return "overread";
            case DecodeError::BadElement: return "bad element";
            case DecodeError::LeftBytes:  return "left bytes";
        }
        return "?";
    }
}
