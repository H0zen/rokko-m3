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
#include "wire/MovementStatus.h"

#include <functional>
#include <string>

class MovementInfo;
class WorldPacket;

/**
 * @brief The wire codec run in shadow beside the legacy movement reader and
 *        writers, counting where they disagree. Design v2 §13 P1: passive.
 *
 * Nothing here changes what the server does: the legacy MovementInfo still
 * feeds every handler and the legacy writers still build every packet. The
 * shadow decodes a copy, compares, counts, and drops the result. Off unless
 * Movement.WireParity is set; then every hook is one atomic load.
 *
 * Two legacy quirks are counted apart, not as mismatches, because they are
 * known and each has its own fate:
 *
 *  - labelSwapped: the legacy header and the registry disagree about which of
 *    the two fall-direction floats is the cosine. P1-B's real-client golden
 *    settled the 28 layouts where the legacy header's labels were suspect --
 *    the client's own bytes agree with them, the registry now emits those
 *    labels, and the bin can no longer fire for any of the 28. It can still
 *    fire for the two the same flip moved the other way,
 *    CMSG_MOVE_SET_RUN_MODE and CMSG_MOVE_SET_WALK_MODE, which the legacy
 *    header labels the CPP way (MovementCodecTest's kLegacyFallAngleSwapped).
 *    Only an airborne run/walk toggle produces such a packet, and the golden
 *    has none, so those two are unproven either way: a parity run that catches
 *    one lands it in this bin, and that is the evidence that would settle
 *    them. The bin therefore outlives P1.
 *
 *  - vehicleIdInFallTime: the legacy reader stores a transport's vehicle id
 *    into fallTime (P2 retires that reader). The bin only fires on a packet
 *    that carries both a fall block and a transport vehicle id: ToWire reads
 *    fallTime only when the legacy status says hasFallData, so the commoner
 *    corruption -- the vehicle id landing in fallTime on a packet with no fall
 *    block at all -- is masked there and counted nowhere. A 0 in that column
 *    is not evidence that the defect is absent.
 *
 * Boundary: Inbound runs only from HandleMovementOpcodes, so the registered
 * inbound acks (CMSG_FORCE_*_CHANGE_ACK, CMSG_MOVE_SET_CAN_FLY_ACK, ...) --
 * which have their own handlers -- are captured and replayed, but never
 * compared against the legacy reader here. That is the plan's scope, not a
 * defect; P1-C must not read these counters as covering them.
 *
 * The families (P1-C). Eight opcodes carry no movement status and so have no
 * generated layout; each has a hand-written codec instead, and the shadow now
 * counts them too -- one row per family opcode, after the registry's, so an
 * existing row index still means what it did. Outbound judges every packet the
 * wire layer knows, of either kind, by decoding it whole and re-encoding what it
 * decoded: a packet that decodes but does not reproduce its own bytes is counted
 * as `inexact` rather than as a failure, because the decode was sound and it is
 * the writer or the encoder that disagrees. Two of the families are built by the
 * client and read by hand in their own handlers rather than through
 * MovementInfo, so they get a hook each -- InboundMover and InboundTeleportAck --
 * which compare the codec's fields with what the handler just read, exactly as
 * Inbound compares a movement status.
 *
 * One of those two handlers reads nothing, which is why its hook runs first.
 * HandleSetActiveMoverOpcode calls recv_data's WriteGuidMask/WriteGuidBytes
 * templates rather than the ReadGuidMask/ReadGuidBytes ones its neighbours use
 * -- the only such call site in the tree. Those write: handed the empty guid the
 * handler declares, they append a zero mask byte to the received packet and fill
 * nothing, so the guid is never read, the handler always reports an incorrect
 * mover and always returns having set none. The instrument must judge the wire
 * rather than the packet that read mutated, so InboundMover is called before it,
 * on the bytes the client sent, and compares the client's guid with the mover
 * the session holds -- the comparison the handler was meant to make. The handler
 * is P2's, with the rest of mover authority.
 */
namespace WireParity
{
    void Enable(bool on);
    bool Enabled();

    /// The legacy status in wire terms. Fields the legacy reader never carries are
    /// taken from `wireOnly` (the codec's decode of the same bytes) so they cannot
    /// count as a difference: counter, value, twoBits, the unnamed bit, the
    /// empty-flags-block markers, the height-change-failed bit, the vehicle id.
    Wire::MovementStatus ToWire(MovementInfo const& legacy, Wire::MovementStatus const& wireOnly);

    /// An inbound registered packet, after the legacy reader consumed it.
    void Inbound(uint16 opcode, WorldPacket const& packet, MovementInfo const& legacy);
    /// A relay the legacy writer built from `legacy`, before it is sent -- and
    /// before SendPacket flushes its trailing bits.
    void Relay(uint16 opcode, WorldPacket const& packet, MovementInfo const& legacy);
    /// Any other packet the wire layer knows that this server sends -- a registry
    /// layout or a family: must decode whole, and should re-encode to its own bytes.
    void Outbound(uint16 opcode, WorldPacket const& packet);

    /// CMSG_SET_ACTIVE_MOVER as the client sent it -- called before the legacy
    /// handler's read, which writes into the packet rather than reading it.
    /// `sessionMover` is the mover the session holds, which is what that
    /// handler's check was meant to compare the client's guid against.
    void InboundMover(WorldPacket const& packet, uint64 sessionMover);
    /// CMSG_MOVE_TELEPORT_ACK after the legacy handler read its fields.
    void InboundTeleportAck(WorldPacket const& packet, uint32 legacyCounter, uint32 legacyTime, uint64 legacyGuid);

    /// True once any hook has counted a packet, whatever the switch says now.
    /// The shutdown report asks this instead of Enabled(), so a `.reload config`
    /// that turns the shadow off before shutdown does not discard the run.
    bool Saw();

    /// One summary line, then one line per opcode that saw traffic.
    void Report(std::function<void(std::string const&)> const& line);
}

#endif
