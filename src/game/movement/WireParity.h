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

#ifndef MANGOS_WIREPARITY_H
#define MANGOS_WIREPARITY_H

#include "Platform/Define.h"
#include "wire/MovementCodec.h"
#include "wire/MovementStatus.h"

#include <functional>
#include <string>

class MovementInfo;
class WorldPacket;

/**
 * @brief What the shadow now is: the outbound judge of the legacy writers
 *        that remain until P2-C, the bridge witness, and the codec-rejection
 *        counters. Design v2 §13.
 *
 * MovementInfo::Read decodes every inbound status through the registry with
 * nothing beside it to compare against any more, so P1's Inbound, Relay,
 * InboundMover and InboundTeleportAck -- each a comparison against a legacy
 * reader that no longer exists -- are gone along with their call sites. Two
 * things take their place:
 *
 *  - Rejected: a packet the registry's codec could not decode -- the
 *    validation ladder's first rung (design v2 §10.1), not a diagnostic.
 *    MovementInfo::Read counts it and throws; HandleMoveTeleportAckOpcode's
 *    own decode does the same for the one family opcode still read by hand.
 *    The tree's own bad-packet handling (WorldSession::Update's
 *    log-and-maybe-kick) takes it from there. Counted whether or not the
 *    shadow is on -- this is production behaviour now.
 *  - BridgeCheck: under the shadow only. Whether MovementBridge's ToWire,
 *    given the record FromWire just filled in, reproduces the status the
 *    codec decoded off the wire -- the bridge's own round-trip witness, not
 *    the reader's. A mismatch means the bridge lost something on the way in;
 *    fix MovementBridge.cpp, not the codec.
 *
 * Outbound is unchanged: it still judges every packet the wire layer knows,
 * of either kind (a registry layout or a family), by decoding it whole and
 * re-encoding what it decoded -- a packet that decodes but does not reproduce
 * its own bytes is counted as `inexact` rather than as a failure, because the
 * decode was sound and it is the writer or the encoder that disagrees.
 * SMSG_PLAYER_MOVE is judged here too now: the relay used to be excluded
 * because the P1-B shadow compared it at its writer, but that comparison is
 * gone with Relay, and the packet is the registry's own encoding, so nothing
 * exempts it any more.
 *
 * HandleSetActiveMoverOpcode calls recv_data's WriteGuidMask/WriteGuidBytes
 * templates rather than the ReadGuidMask/ReadGuidBytes ones its neighbours
 * use -- the only such call site in the tree. Those write: handed the empty
 * guid the handler declares, they append a zero mask byte to the received
 * packet and fill nothing, so the guid is never read, the handler always
 * reports an incorrect mover and always returns having set none. P2-D's to
 * fix; nothing here instruments it, since the comparison it would take (the
 * client's guid against the session's mover) belonged to the inbound entry
 * points this task removed.
 */
namespace WireParity
{
    void Enable(bool on);
    bool Enabled();

    /// Any other packet the wire layer knows that this server sends -- a registry
    /// layout or a family: must decode whole, and should re-encode to its own bytes.
    void Outbound(uint16 opcode, WorldPacket const& packet);

    /// A packet the registry's codec could not decode -- MovementInfo::Read's own
    /// bad-packet path. Counted on the opcode's row (or a single unknown-opcode
    /// bucket, when the opcode names neither a registry layout nor a family)
    /// whether or not the shadow is on.
    void Rejected(uint16 opcode, Wire::DecodeError error);

    /// Under the shadow only. `decoded` is what the codec just read off the wire;
    /// `record` is the MovementInfo the bridge mapped it into. Counts the opcode's
    /// row seen either way; a mismatch means the bridge lost something on the way
    /// in -- fix MovementBridge.cpp, not the codec.
    void BridgeCheck(uint16 opcode, Wire::MovementStatus const& decoded, MovementInfo const& record);

    /// True once any hook has counted a packet, whatever the switch says now.
    /// The shutdown report asks this instead of Enabled(), so a `.reload config`
    /// that turns the shadow off before shutdown does not discard the run.
    bool Saw();

    /// One summary line, then one line per opcode that saw traffic.
    void Report(std::function<void(std::string const&)> const& line);
}

#endif
