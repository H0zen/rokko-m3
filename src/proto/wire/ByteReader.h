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

#ifndef MANGOS_H_WIRE_BYTEREADER
#define MANGOS_H_WIRE_BYTEREADER

#include "Platform/Define.h"
#include "wire/MovementCodec.h"

#include "Utilities/ByteBuffer.h"

namespace Wire { namespace Detail
{
    /// Thrown when the layout asks for a byte the buffer does not hold. A
    /// codec-local type on purpose: ByteBufferException logs from its
    /// constructor, and a shadow or a replay that judges bad packets must not
    /// write a line per packet.
    struct Overread {};

    /// Every read of a decode goes through here, so the bounds are checked
    /// before ByteBuffer's own check could throw. The bit position mirrors
    /// ByteBuffer's: both start at a byte boundary (Run clears the buffer's bit
    /// cursor before it builds one of these) and every read in between is one of
    /// these, so a bit read is bounds-checked exactly when ByteBuffer would
    /// fetch a new cursor byte.
    struct Reader
    {
        ByteBuffer& in;
        size_t bitpos = 8;

        explicit Reader(ByteBuffer& buffer) : in(buffer) {}

        template <typename T>
        T Get()
        {
            if (in.rpos() + sizeof(T) > in.size()) { throw Overread(); }
            bitpos = 8;                              // read<T>() resets the bit reader
            return in.read<T>();
        }

        bool Bit()
        {
            ++bitpos;
            if (bitpos > 7)
            {
                if (in.rpos() >= in.size()) { throw Overread(); }
                bitpos = 0;
            }
            return in.ReadBit();
        }

        uint32 Bits(size_t bits)
        {
            uint32 value = 0;
            for (int32 i = int32(bits) - 1; i >= 0; --i)
            {
                if (Bit()) { value |= (1u << i); }
            }
            return value;
        }

        void Reset()
        {
            in.ResetBitReader();
            bitpos = 8;
        }
    };

    /// The whole-or-nothing decode every codec shares: `body(reader, candidate)`
    /// reads from `in`'s cursor; an overread (the codec's own check first,
    /// ByteBuffer's as a backstop) becomes DecodeError::Overread; `consumed` is
    /// how far the cursor moved; `out` gets the candidate only on success.
    template <typename T, typename Body>
    DecodeResult Run(ByteBuffer& in, T& out, Body body)
    {
        DecodeResult result;
        const size_t start = in.rpos();
        T candidate;
        try
        {
            // Every family decode starts at a byte boundary: Reader's bit
            // position starts at 8 and assumes ByteBuffer's does too, and the
            // bounds check that stands in for ByteBuffer's own is written from
            // that assumption. Clearing the buffer's bit cursor here makes the
            // precondition true by construction rather than by the caller
            // remembering it -- and with it goes the last path by which a decode
            // could reach ByteBufferException instead of Overread.
            in.ResetBitReader();
            Reader reader(in);
            body(reader, candidate);
        }
        catch (Overread const&)
        {
            result.error = DecodeError::Overread;
        }
        catch (ByteBufferException const&)
        {
            result.error = DecodeError::Overread;
        }
        result.consumed = in.rpos() - start;
        out = result.ok() ? candidate : T();
        return result;
    }
} }

#endif
