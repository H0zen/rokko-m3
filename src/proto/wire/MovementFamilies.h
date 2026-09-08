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

#ifndef MANGOS_WIRE_MOVEMENTFAMILIES_H
#define MANGOS_WIRE_MOVEMENTFAMILIES_H

#include "Platform/Define.h"
#include "wire/MovementCodec.h"

#include <cstddef>

class ByteBuffer;

namespace Wire
{
    /// A packet the generated registry cannot describe: its shape is not a
    /// movement-status block, so it gets its own struct and codec instead of a
    /// layout table. None means "not one of these" -- FamilyFor's answer for
    /// every opcode outside the eight below, registry opcodes included.
    enum class Family : uint8
    {
        None,
        KnockBack,
        Teleport,
        TeleportAck,
        ActiveMover,
        ControlUpdate,
        MonsterMove
    };

    /// The family `opcode` belongs to, or Family::None when it has none (either
    /// it is a registry opcode, or it is not known to the wire layer at all).
    Family FamilyFor(uint16 opcode);
    /// True when `opcode` has a family (FamilyFor(opcode) != Family::None).
    bool IsFamily(uint16 opcode);
    /// True when the wire layer can decode `opcode` at all -- a registry packet
    /// layout or a family, whichever kind it is.
    bool IsKnown(uint16 opcode);
    /// The family's name for a report: "knock back", "teleport", "teleport ack",
    /// "active mover", "control update", "monster move", "none".
    char const* FamilyName(Family family);
    /// 0-based position of `opcode` among the family opcodes (FamilyOpcodeAt's
    /// domain), or -1 when it has no family.
    int FamilyIndex(uint16 opcode);
    /// How many opcodes have a family (8).
    size_t FamilyCount();
    /// The opcode at `index` in the family table (0 <= index < FamilyCount()),
    /// or 0 -- which is no opcode -- when `index` is past the end.
    uint16 FamilyOpcodeAt(size_t index);

    /// What Judge found: whether the packet decoded and, if it did, whether
    /// re-encoding the decoded value reproduces the packet byte for byte.
    struct Verdict
    {
        bool decoded = false;
        bool exact = false;
        DecodeResult result;
        size_t reencoded = 0;
        /// Where the re-encoding first parts company with the packet: the byte
        /// offset of the first difference, or the shorter of the two lengths when
        /// one is a prefix of the other. -1 when they are the same bytes (and so
        /// whenever `exact`, and whenever the packet did not decode at all).
        /// A size alone does not say what changed; this is what a report needs to
        /// point at the field a legacy writer got wrong.
        long firstDifference = -1;
    };

    /**
     * @brief Decodes a copy of `packet` -- whichever kind of packet `opcode`
     *        names, a registry layout or a family -- and re-encodes what it
     *        decoded, so the capture, the shadow, the replay and the tests need
     *        only one question: did this packet round-trip.
     * @param pendingWriteBits see DecodeWhole: true for a packet a writer built
     *        and has not flushed, false for one that has been read from.
     */
    Verdict Judge(uint16 opcode, ByteBuffer const& packet, bool pendingWriteBits);
}

#endif
