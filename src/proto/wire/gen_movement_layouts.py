#!/usr/bin/env python3
"""Generate src/proto/wire/MovementLayouts.inc from the Cataclysm Preservation
Project's MovementStructures.cpp (kept out of tree; pass its path).

Every table in that file is transcribed into Wire's vocabulary, with the per-packet
"extra" elements CPP passes at call time spliced in place of MSEExtraElement, and
its opcode switch becomes the MAP rows. Run by hand; the output is committed.
"""
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# MaNGOS is a full featured server for World of Warcraft, supporting
# the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
#
# Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# World of Warcraft, and all World of Warcraft or Warcraft art, images,
# and lore are copyrighted by Blizzard Entertainment, Inc.
#
import io, re, sys

SRC = sys.argv[1] if len(sys.argv) > 1 else "MovementStructures.cpp"
OUT = "MovementLayouts.inc"
SOURCE_REV = "716f480 (2026-05-05)"

# CPP's opcode names -> this tree's (same values; see OpcodeValuesTest).
OPCODE_NAMES = {}
for k in ("FALL_LAND", "JUMP", "SET_FACING", "SET_PITCH", "SET_RUN_MODE", "SET_WALK_MODE",
          "START_ASCEND", "START_BACKWARD", "START_DESCEND", "START_FORWARD",
          "START_PITCH_DOWN", "START_PITCH_UP", "START_STRAFE_LEFT", "START_STRAFE_RIGHT",
          "START_SWIM", "START_TURN_LEFT", "START_TURN_RIGHT", "STOP", "STOP_ASCEND",
          "STOP_PITCH", "STOP_STRAFE", "STOP_SWIM", "STOP_TURN"):
    OPCODE_NAMES["MSG_MOVE_" + k] = "CMSG_MOVE_" + k
for k in ("WALK", "RUN", "RUN_BACK", "SWIM", "SWIM_BACK", "FLIGHT", "FLIGHT_BACK"):
    OPCODE_NAMES["CMSG_MOVE_FORCE_%s_SPEED_CHANGE_ACK" % k] = "CMSG_FORCE_%s_SPEED_CHANGE_ACK" % k
OPCODE_NAMES.update({
    "SMSG_MOVE_SET_COLLISION_HEIGHT":     "SMSG_MOVE_SET_COLLISION_HGT",
    "CMSG_MOVE_SET_COLLISION_HEIGHT_ACK": "CMSG_MOVE_SET_COLLISION_HGT_ACK",
    "SMSG_MOVE_ROOT":                     "SMSG_FORCE_MOVE_ROOT",
    "SMSG_MOVE_UNROOT":                   "SMSG_FORCE_MOVE_UNROOT",
    "SMSG_MOVE_UPDATE":                   "SMSG_PLAYER_MOVE",
    "SMSG_SPLINE_MOVE_SET_FEATHER_FALL":  "SMSG_SPLINE_MOVE_FEATHER_FALL",
    "SMSG_SPLINE_MOVE_SET_LAND_WALK":     "SMSG_SPLINE_MOVE_LAND_WALK",
    "SMSG_SPLINE_MOVE_SET_NORMAL_FALL":   "SMSG_SPLINE_MOVE_NORMAL_FALL",
    "SMSG_SPLINE_MOVE_SET_WATER_WALK":    "SMSG_SPLINE_MOVE_WATER_WALK",
})

# CPP's element names -> Wire::Element. Anything not listed maps by dropping "MSE".
ELEMENT_NAMES = {
    "MSEMovementFlags": "Flags", "MSEMovementFlags2": "Flags2",
    "MSEOrientation": "PositionO", "MSETransportOrientation": "TransportPositionO",
    "MSECounter": "MovementCounter",
    "MSEZeroBit": "HasUnknownBit",          # the same positional bit P0-A carries
    "MSEOneBit": "OneBit", "MSEFlushBits": "FlushBits",
    "MSEExtraInt8": "ByteParam", "MSEExtraFloat": "ExtraFloat", "MSEExtraTwoBits": "ExtraTwoBits",
    # The P1-B client golden proves CPP has these two the wrong way round: a
    # real CMSG_MOVE_JUMP's own bytes show the float CPP names sin is actually
    # the cosine of the facing (and vice versa), and every other packet the
    # client sends while that jump's fall stays open (MSG_MOVE_HEARTBEAT,
    # CMSG_MOVE_START_STRAFE_LEFT -- Cos-then-Sin tables, the opposite wire
    # order from the jump's Sin-then-Cos) echoes the identical, now-correct
    # pair. CPP names both elements from one offset-to-name mapping applied to
    # every table, so the flip belongs in that one mapping, not per table.
    "MSEFallCosAngle": "FallSinAngle", "MSEFallSinAngle": "FallCosAngle",
}
for i in range(8):
    ELEMENT_NAMES["MSEHasGuidByte%d" % i] = "GuidBit%d" % i
    ELEMENT_NAMES["MSEHasTransportGuidByte%d" % i] = "TransportGuidBit%d" % i

# What CPP passes as ExtraMovementStatusElement per table, in order. A table not
# listed and holding one MSEExtraElement gets one ExtraFloat (every speed packet).
EXTRAS = {
    "MovementSetCollisionHeight":    ["ExtraTwoBits", "ExtraFloat"],   # MovementPacketSender::SendHeightChangeToMover
    "MovementSetCollisionHeightAck": ["ExtraFloat", "ExtraTwoBits"],   # WorldSession::HandleSetCollisionHeightAck
    "ChangeSeatsOnControlledVehicle":                                 # WorldSession::HandleChangeSeatsOnControlledVehicle
        ["ByteParam"] + ["Guid2Bit%d" % i for i in (2, 4, 7, 6, 5, 0, 1, 3)]
                      + ["Guid2Byte%d" % i for i in (6, 1, 2, 5, 3, 0, 4, 7)],
}

# CPP tables that read gated fields (Timestamp, Pitch, Fall*, Transport*) with none of the
# presence gates every other update carries; a reader cannot know from the table what to
# read, so no layout is better than this one. P1-C's reader lift supplies all three from
# the client instead (LIFTED below), so the opcodes still get a row.
EXCLUDED = {
    "MovementUpdateKnockBack":     "no HasTimestamp/HasPitch/HasFallData/HasTransportData gates",
    "MovementUpdateRunBackSpeed":  "same",
    "MovementUpdateWalkSpeed":     "same",
}

# Tables lifted from the client's own readers by lift_client_reader.py (the
# reader named per table, from Wow-64.c of build 15595), because CPP's tables
# for these three read gated fields with no gates. The lifter reproduces
# MovementUpdateRunSpeed from its reader exactly (its --control mode), which is
# what makes these three trustworthy.
LIFTED = {
    "MovementUpdateKnockBack": ("sub_140382F50", [
        "HasUnknownBit", "GuidBit4", "HasMovementFlags", "HasPitch", "HasTimestamp", "GuidBit1",
        "GuidBit0", "GuidBit3", "GuidBit2", "GuidBit7", "HasSpline", "HasTransportData",
        "TransportGuidBit7", "TransportGuidBit5", "TransportGuidBit1", "TransportGuidBit6",
        "HasTransportTime2", "TransportGuidBit2", "TransportGuidBit4", "TransportGuidBit0",
        "HasVehicleId", "TransportGuidBit3", "GuidBit5", "HasSplineElevation",
        "HasMovementFlags2", "GuidBit6", "Flags", "HasFallData", "HasFallDirection",
        "HasOrientation", "Flags2", "FlushBits", "PositionO", "FallCosAngle",
        "FallHorizontalSpeed", "FallSinAngle", "FallTime", "FallVerticalSpeed",
        "SplineElevation", "GuidByte3", "TransportGuidByte5", "TransportVehicleId",
        "TransportGuidByte7", "TransportSeat", "TransportGuidByte3", "TransportGuidByte6",
        "TransportPositionZ", "TransportGuidByte1", "TransportPositionY", "TransportPositionX",
        "TransportGuidByte2", "TransportGuidByte0", "TransportPositionO", "TransportTime",
        "TransportGuidByte4", "TransportTime2", "Pitch", "PositionZ", "Timestamp", "PositionX",
        "GuidByte4", "GuidByte6", "GuidByte7", "GuidByte2", "GuidByte1", "PositionY",
        "GuidByte0", "GuidByte5"]),
    "MovementUpdateRunBackSpeed": ("sub_14038E360", [
        "GuidBit1", "GuidBit2", "HasSplineElevation", "GuidBit4", "GuidBit3", "HasFallData",
        "GuidBit6", "HasTimestamp", "GuidBit0", "HasUnknownBit", "HasMovementFlags", "HasPitch",
        "HasSpline", "GuidBit5", "HasMovementFlags2", "Flags2", "HasOrientation", "Flags",
        "HasFallDirection", "HasTransportData", "TransportGuidBit5", "HasTransportTime2",
        "TransportGuidBit3", "TransportGuidBit1", "TransportGuidBit6", "TransportGuidBit7",
        "TransportGuidBit2", "TransportGuidBit4", "TransportGuidBit0", "HasVehicleId",
        "GuidBit7", "FlushBits", "TransportPositionX", "TransportGuidByte2",
        "TransportGuidByte5", "TransportGuidByte4", "TransportGuidByte6", "TransportTime2",
        "TransportGuidByte0", "TransportGuidByte3", "TransportPositionY", "TransportGuidByte7",
        "TransportVehicleId", "TransportPositionZ", "TransportTime", "TransportSeat",
        "TransportGuidByte1", "TransportPositionO", "GuidByte4", "FallTime",
        "FallHorizontalSpeed", "FallCosAngle", "FallSinAngle", "FallVerticalSpeed",
        "Timestamp", "SplineElevation", "GuidByte1", "PositionO", "GuidByte0", "GuidByte5",
        "GuidByte3", "PositionX", "PositionY", "Pitch", "GuidByte7", "ExtraFloat",
        "GuidByte2", "GuidByte6", "PositionZ"]),
    "MovementUpdateWalkSpeed": ("sub_14038EFE0", [
        "HasPitch", "HasOrientation", "HasUnknownBit", "GuidBit3", "HasSplineElevation",
        "GuidBit2", "HasTransportData", "TransportGuidBit6", "TransportGuidBit3",
        "TransportGuidBit2", "TransportGuidBit0", "TransportGuidBit4", "HasTransportTime2",
        "TransportGuidBit7", "TransportGuidBit1", "TransportGuidBit5", "HasVehicleId",
        "GuidBit7", "GuidBit5", "GuidBit1", "HasFallData", "GuidBit0", "HasMovementFlags2",
        "HasTimestamp", "HasMovementFlags", "GuidBit6", "HasFallDirection", "Flags2", "Flags",
        "HasSpline", "GuidBit4", "FlushBits", "Pitch", "TransportGuidByte6",
        "TransportGuidByte0", "TransportGuidByte4", "TransportGuidByte2", "TransportPositionX",
        "TransportGuidByte7", "TransportTime", "TransportTime2", "TransportPositionZ",
        "TransportSeat", "TransportGuidByte5", "TransportVehicleId", "TransportPositionO",
        "TransportGuidByte1", "TransportPositionY", "TransportGuidByte3", "SplineElevation",
        "FallVerticalSpeed", "FallHorizontalSpeed", "FallCosAngle", "FallSinAngle", "FallTime",
        "GuidByte1", "GuidByte4", "GuidByte2", "GuidByte6", "GuidByte7", "Timestamp",
        "PositionO", "PositionY", "GuidByte0", "PositionZ", "PositionX", "GuidByte3",
        "GuidByte5", "ExtraFloat"]),
}
assert set(LIFTED) == set(EXCLUDED)

src = io.open(SRC, encoding="utf-8").read()

tables = {}
order = []
for m in re.finditer(r"MovementStatusElements\s+(?:const\s+)?([A-Za-z0-9_]+)\[\]\s*=\s*\{(.*?)\};", src, re.S):
    body = re.sub(r"//.*", "", m.group(2))
    tables[m.group(1)] = re.findall(r"MSE[A-Za-z0-9_]+", body)
    order.append(m.group(1))

opmap = {}
for block, table in re.findall(r"((?:\s*case\s+(?:C|S|)MSG_[A-Z_0-9]+:\s*)+)return\s+([A-Za-z0-9_]+);", src):
    for op in re.findall(r"case\s+((?:C|S|)MSG_[A-Z_0-9]+)", block):
        opmap[OPCODE_NAMES.get(op, op)] = table

assert len(tables) == 109, len(tables)
assert len(opmap) == 111, len(opmap)
assert all(t in tables for t in opmap.values())

our_names = set(re.findall(r"^\s+((?:C|S|)MSG_[A-Z_0-9]+)\s*=", io.open("../Opcodes.h", encoding="utf-8").read(), re.M))
missing = sorted(op for op in opmap if op not in our_names)
assert not missing, "not in Opcodes.h: %s" % " ".join(missing)

emitted_order = [name for name in order if name not in EXCLUDED]
emitted_opmap = {op: t for op, t in opmap.items() if t not in EXCLUDED}
assert len(emitted_order) == 106, len(emitted_order)
assert len(emitted_opmap) == 108, len(emitted_opmap)

lifted_order = [name for name in order if name in LIFTED]
lifted_opmap = {op: t for op, t in opmap.items() if t in LIFTED}
assert len(lifted_order) == 3, len(lifted_order)
assert len(lifted_opmap) == 3, len(lifted_opmap)

def wire(name, elements):
    extras = list(EXTRAS.get(name, ["ExtraFloat"] * elements.count("MSEExtraElement")))
    out = []
    for e in elements:
        if e == "MSEExtraElement":
            out.append(extras.pop(0))
        elif e in ELEMENT_NAMES:
            out.append(ELEMENT_NAMES[e])
        else:
            assert e.startswith("MSE") and e != "MSEExtraElement", e
            out.append(e[3:])
    assert not extras, name
    assert out[-1] == "End", name
    return out

buf = io.StringIO()
buf.write("// SPDX-License-Identifier: GPL-3.0-or-later\n//\n")
buf.write("// GENERATED -- do not edit. Regenerate with src/proto/wire/gen_movement_layouts.py.\n//\n")
buf.write("// %d movement-status layouts for %d opcodes of build 15595, %d of them transcribed from the\n"
          % (len(emitted_order) + len(lifted_order), len(emitted_opmap) + len(lifted_opmap), len(emitted_order)))
buf.write("// Cataclysm Preservation Project's MovementStructures.cpp (%s, GPL-3.0-or-later)\n" % SOURCE_REV)
buf.write("// into Wire's vocabulary, with that source's per-packet extra elements spliced in\n")
buf.write("// place. Every CPP table is CPP-SOURCED and BINARY-UNVERIFIED: P1-B's real-client goldens\n")
buf.write("// and P1-C's reader lift are what turn a table into a verified one, and the legacy\n")
buf.write("// fence in MovementCodecTest records where the tree's older transcription disagrees.\n")
buf.write("// One rename: MSEFallCosAngle -> FallSinAngle and MSEFallSinAngle -> FallCosAngle, in every\n")
buf.write("// table -- P1-B's client golden proved the source's mapping backwards (see ELEMENT_NAMES).\n//\n")
buf.write("// %d of the source's tables were excluded -- they read gated fields with none of the\n" % len(EXCLUDED))
buf.write("// presence gates, so no table of theirs would be better than a wrong one -- and are\n")
buf.write("// supplied by the client's own readers instead (LIFTED in the generator, lifted by\n")
buf.write("// src/proto/wire/lift_client_reader.py, which reproduces MovementUpdateRunSpeed from\n")
buf.write("// its reader exactly; those three are CLIENT-SOURCED, the only ones here that are):\n")
for name in sorted(EXCLUDED):
    op = next(o for o, t in opmap.items() if t == name)
    buf.write("//   %s (%s): %s\n" % (name, op, EXCLUDED[name]))
buf.write("//\n")
buf.write("// LAYOUT(name, elements...)   one table\n// MAP(opcode, name)           one registry row\n\n")


def layout(name, elems, note=None):
    if note:
        buf.write("// %s\n" % note)
    buf.write("LAYOUT(%s,\n" % name)
    line = "   "
    for e in elems:
        item = " E::%s," % e
        if len(line) + len(item) > 100:
            buf.write(line + "\n")
            line = "   "
        line += item
    buf.write(line.rstrip(",") + ")\n\n")


for name in order:
    if name not in EXCLUDED:
        layout(name, wire(name, tables[name]))
for name in lifted_order:
    reader, elems = LIFTED[name]
    layout(name, elems + ["End"],
           "lifted from the client reader %s; see lift_client_reader.py" % reader)
rows = dict(emitted_opmap, **lifted_opmap)
for op in sorted(rows):
    buf.write("MAP(%s, %s)\n" % (op, rows[op]))
io.open(OUT, "w", encoding="utf-8", newline="\n").write(buf.getvalue())
print("wrote %s: %d tables, %d rows (%d of them lifted from the client's readers)"
      % (OUT, len(emitted_order) + len(lifted_order), len(emitted_opmap) + len(lifted_opmap), len(LIFTED)))
