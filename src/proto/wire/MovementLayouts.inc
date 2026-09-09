// SPDX-License-Identifier: GPL-3.0-or-later
//
// GENERATED -- do not edit. Regenerate with src/proto/wire/gen_movement_layouts.py.
//
// 111 movement-status layouts for 113 opcodes of build 15595, 106 of them transcribed from the
// Cataclysm Preservation Project's MovementStructures.cpp (716f480 (2026-05-05), GPL-3.0-or-later)
// into Wire's vocabulary, with that source's per-packet extra elements spliced in
// place. Every CPP table is CPP-SOURCED and BINARY-UNVERIFIED: P1-B's real-client goldens
// and P1-C's reader lift are what turn a table into a verified one, and the legacy
// fence in MovementCodecTest records where the tree's older transcription disagrees.
// One rename: MSEFallCosAngle -> FallSinAngle and MSEFallSinAngle -> FallCosAngle, in every
// table -- P1-B's client golden proved the source's mapping backwards (see ELEMENT_NAMES).
//
// 3 of the source's tables were excluded -- they read gated fields with none of the
// presence gates, so no table of theirs would be better than a wrong one -- and are
// supplied by the client's own readers instead (LIFTED in the generator, lifted by
// src/proto/wire/lift_client_reader.py, which reproduces MovementUpdateRunSpeed from
// its reader exactly). 2 more tables have no reference counterpart at all -- CPP's
// switch has no case and no table for them, only the client does -- and are lifted the
// same way (ADDED in the generator, P2-A task 6). These 5 are CLIENT-SOURCED, the only
// ones here that are:
//   MovementUpdateKnockBack (SMSG_MOVE_UPDATE_KNOCK_BACK): no HasTimestamp/HasPitch/HasFallData/HasTransportData gates
//   MovementUpdateRunBackSpeed (SMSG_MOVE_UPDATE_RUN_BACK_SPEED): same
//   MovementUpdateWalkSpeed (SMSG_MOVE_UPDATE_WALK_SPEED): same
//   MovementUpdateTeleport (SMSG_MOVE_UPDATE_TELEPORT): no reference table; lifted from the client reader sub_140384210
//   MovementUpdateTurnRate (SMSG_MOVE_UPDATE_TURN_RATE): no reference table; lifted from the client reader sub_14038C780
//
// A fourth client-only update, SMSG_MOVE_UPDATE_PITCH_RATE (0x1DB5), has no table here
// either: its reader (sub_14037B330) takes a gate bit as a bitwise-complement (`~expr >> 7`)
// that lift_client_reader.py cannot follow (see ADDED's comment above); it is BLOCKED, not
// excluded -- there was never a source table to exclude.
//
// LAYOUT(name, elements...)   one table
// MAP(opcode, name)           one registry row

LAYOUT(MovementUpdate,
    E::HasFallData, E::GuidBit3, E::GuidBit6, E::HasMovementFlags2, E::HasSpline, E::HasTimestamp,
    E::GuidBit0, E::GuidBit1, E::Flags2, E::GuidBit7, E::HasMovementFlags, E::HasOrientation,
    E::GuidBit2, E::HasSplineElevation, E::HasHeightChangeFailed, E::GuidBit4, E::HasFallDirection,
    E::GuidBit5, E::HasTransportData, E::Flags, E::TransportGuidBit3, E::HasVehicleId,
    E::TransportGuidBit6, E::TransportGuidBit1, E::TransportGuidBit7, E::TransportGuidBit0,
    E::TransportGuidBit4, E::HasTransportTime2, E::TransportGuidBit5, E::TransportGuidBit2,
    E::HasPitch, E::FlushBits, E::GuidByte5, E::FallHorizontalSpeed, E::FallCosAngle,
    E::FallSinAngle, E::FallVerticalSpeed, E::FallTime, E::SplineElevation, E::GuidByte7,
    E::PositionY, E::GuidByte3, E::TransportVehicleId, E::TransportGuidByte6, E::TransportSeat,
    E::TransportGuidByte5, E::TransportPositionX, E::TransportGuidByte1, E::TransportPositionO,
    E::TransportGuidByte2, E::TransportTime2, E::TransportGuidByte0, E::TransportPositionZ,
    E::TransportGuidByte7, E::TransportGuidByte4, E::TransportGuidByte3, E::TransportPositionY,
    E::TransportTime, E::GuidByte4, E::PositionX, E::GuidByte6, E::PositionZ, E::Timestamp,
    E::GuidByte2, E::Pitch, E::GuidByte0, E::PositionO, E::GuidByte1, E::End)

LAYOUT(MovementFallLand,
    E::PositionX, E::PositionY, E::PositionZ, E::HasTransportData, E::GuidBit7, E::GuidBit1,
    E::HasMovementFlags2, E::GuidBit3, E::HasSplineElevation, E::HasOrientation, E::GuidBit6,
    E::HasTimestamp, E::HasUnknownBit, E::HasPitch, E::GuidBit4, E::HasSpline, E::GuidBit5,
    E::HasMovementFlags, E::HasFallData, E::GuidBit0, E::GuidBit2, E::TransportGuidBit0,
    E::TransportGuidBit5, E::TransportGuidBit3, E::TransportGuidBit2, E::HasVehicleId,
    E::HasTransportTime2, E::TransportGuidBit6, E::TransportGuidBit4, E::TransportGuidBit1,
    E::TransportGuidBit7, E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte1, E::GuidByte7,
    E::GuidByte4, E::GuidByte3, E::GuidByte6, E::GuidByte0, E::GuidByte2, E::GuidByte5,
    E::TransportTime, E::TransportPositionZ, E::TransportPositionY, E::TransportPositionX,
    E::TransportGuidByte5, E::TransportVehicleId, E::TransportGuidByte1, E::TransportSeat,
    E::TransportTime2, E::TransportGuidByte4, E::TransportPositionO, E::TransportGuidByte0,
    E::TransportGuidByte7, E::TransportGuidByte3, E::TransportGuidByte6, E::TransportGuidByte2,
    E::FallVerticalSpeed, E::FallTime, E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle,
    E::SplineElevation, E::Timestamp, E::Pitch, E::PositionO, E::End)

LAYOUT(MovementHeartBeat,
    E::PositionZ, E::PositionX, E::PositionY, E::HasPitch, E::HasTimestamp, E::HasFallData,
    E::HasMovementFlags2, E::HasTransportData, E::GuidBit7, E::GuidBit1, E::GuidBit0, E::GuidBit4,
    E::GuidBit2, E::HasOrientation, E::GuidBit5, E::GuidBit3, E::HasSplineElevation, E::HasSpline,
    E::HasUnknownBit, E::GuidBit6, E::HasMovementFlags, E::HasVehicleId, E::TransportGuidBit4,
    E::TransportGuidBit2, E::HasTransportTime2, E::TransportGuidBit5, E::TransportGuidBit7,
    E::TransportGuidBit6, E::TransportGuidBit0, E::TransportGuidBit3, E::TransportGuidBit1,
    E::HasFallDirection, E::Flags, E::Flags2, E::GuidByte3, E::GuidByte6, E::GuidByte1,
    E::GuidByte7, E::GuidByte2, E::GuidByte5, E::GuidByte0, E::GuidByte4, E::TransportPositionZ,
    E::TransportSeat, E::TransportPositionO, E::TransportGuidByte4, E::TransportPositionY,
    E::TransportTime, E::TransportPositionX, E::TransportGuidByte5, E::TransportGuidByte1,
    E::TransportGuidByte3, E::TransportGuidByte7, E::TransportVehicleId, E::TransportTime2,
    E::TransportGuidByte2, E::TransportGuidByte0, E::TransportGuidByte6, E::PositionO,
    E::FallVerticalSpeed, E::FallTime, E::FallHorizontalSpeed, E::FallSinAngle, E::FallCosAngle,
    E::Pitch, E::SplineElevation, E::Timestamp, E::End)

LAYOUT(MovementJump,
    E::PositionY, E::PositionX, E::PositionZ, E::HasTimestamp, E::GuidBit5, E::HasMovementFlags,
    E::GuidBit4, E::GuidBit6, E::HasMovementFlags2, E::GuidBit0, E::HasTransportData, E::GuidBit3,
    E::HasPitch, E::GuidBit7, E::HasFallData, E::HasSpline, E::HasOrientation, E::GuidBit1,
    E::HasSplineElevation, E::HasUnknownBit, E::GuidBit2, E::TransportGuidBit3,
    E::TransportGuidBit7, E::TransportGuidBit6, E::TransportGuidBit0, E::TransportGuidBit5,
    E::TransportGuidBit1, E::TransportGuidBit4, E::HasVehicleId, E::HasTransportTime2,
    E::TransportGuidBit2, E::HasFallDirection, E::Flags, E::Flags2, E::GuidByte6, E::GuidByte5,
    E::GuidByte4, E::GuidByte0, E::GuidByte2, E::GuidByte3, E::GuidByte7, E::GuidByte1,
    E::TransportGuidByte1, E::TransportGuidByte7, E::TransportPositionX, E::TransportGuidByte4,
    E::TransportVehicleId, E::TransportGuidByte0, E::TransportPositionO, E::TransportTime2,
    E::TransportTime, E::TransportGuidByte6, E::TransportPositionZ, E::TransportSeat,
    E::TransportPositionY, E::TransportGuidByte3, E::TransportGuidByte2, E::TransportGuidByte5,
    E::Pitch, E::Timestamp, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed,
    E::FallVerticalSpeed, E::FallTime, E::SplineElevation, E::PositionO, E::End)

LAYOUT(MovementSetFacing,
    E::PositionX, E::PositionY, E::PositionZ, E::GuidBit6, E::HasTransportData, E::GuidBit4,
    E::HasSpline, E::GuidBit0, E::HasOrientation, E::HasTimestamp, E::HasPitch,
    E::HasMovementFlags2, E::GuidBit5, E::GuidBit7, E::GuidBit2, E::HasFallData,
    E::HasSplineElevation, E::HasMovementFlags, E::GuidBit3, E::HasUnknownBit, E::GuidBit1,
    E::HasVehicleId, E::TransportGuidBit3, E::TransportGuidBit7, E::TransportGuidBit5,
    E::TransportGuidBit2, E::TransportGuidBit6, E::TransportGuidBit0, E::HasTransportTime2,
    E::TransportGuidBit4, E::TransportGuidBit1, E::HasFallDirection, E::Flags2, E::Flags,
    E::GuidByte6, E::GuidByte7, E::GuidByte2, E::GuidByte0, E::GuidByte4, E::GuidByte1,
    E::GuidByte5, E::GuidByte3, E::TransportGuidByte0, E::TransportPositionX, E::TransportGuidByte1,
    E::TransportTime2, E::TransportGuidByte4, E::TransportPositionZ, E::TransportPositionO,
    E::TransportGuidByte2, E::TransportVehicleId, E::TransportGuidByte3, E::TransportSeat,
    E::TransportPositionY, E::TransportGuidByte5, E::TransportGuidByte6, E::TransportTime,
    E::TransportGuidByte7, E::FallHorizontalSpeed, E::FallSinAngle, E::FallCosAngle, E::FallTime,
    E::FallVerticalSpeed, E::SplineElevation, E::PositionO, E::Timestamp, E::Pitch, E::End)

LAYOUT(MovementSetPitch,
    E::PositionX, E::PositionZ, E::PositionY, E::HasFallData, E::HasMovementFlags, E::GuidBit1,
    E::HasOrientation, E::GuidBit7, E::GuidBit3, E::HasMovementFlags2, E::HasTransportData,
    E::HasSplineElevation, E::GuidBit6, E::HasPitch, E::GuidBit4, E::HasSpline, E::GuidBit2,
    E::HasUnknownBit, E::HasTimestamp, E::GuidBit0, E::GuidBit5, E::TransportGuidBit3,
    E::TransportGuidBit6, E::TransportGuidBit2, E::TransportGuidBit4, E::TransportGuidBit0,
    E::TransportGuidBit1, E::TransportGuidBit5, E::HasVehicleId, E::TransportGuidBit7,
    E::HasTransportTime2, E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte3, E::GuidByte7,
    E::GuidByte1, E::GuidByte6, E::GuidByte0, E::GuidByte5, E::GuidByte2, E::GuidByte4,
    E::TransportGuidByte4, E::TransportGuidByte3, E::TransportGuidByte6, E::TransportPositionO,
    E::TransportGuidByte1, E::TransportVehicleId, E::TransportTime, E::TransportGuidByte2,
    E::TransportPositionZ, E::TransportGuidByte0, E::TransportTime2, E::TransportPositionY,
    E::TransportGuidByte5, E::TransportSeat, E::TransportGuidByte7, E::TransportPositionX,
    E::FallVerticalSpeed, E::FallTime, E::FallHorizontalSpeed, E::FallCosAngle, E::FallSinAngle,
    E::Pitch, E::SplineElevation, E::Timestamp, E::PositionO, E::End)

LAYOUT(MovementStartBackward,
    E::PositionX, E::PositionZ, E::PositionY, E::HasTransportData, E::GuidBit3, E::GuidBit0,
    E::GuidBit2, E::HasTimestamp, E::GuidBit7, E::HasPitch, E::HasUnknownBit, E::HasMovementFlags,
    E::HasOrientation, E::HasSpline, E::HasMovementFlags2, E::HasFallData, E::GuidBit5, E::GuidBit1,
    E::GuidBit4, E::GuidBit6, E::HasSplineElevation, E::TransportGuidBit3, E::TransportGuidBit1,
    E::HasTransportTime2, E::TransportGuidBit2, E::TransportGuidBit7, E::TransportGuidBit0,
    E::TransportGuidBit6, E::TransportGuidBit5, E::TransportGuidBit4, E::HasVehicleId, E::Flags,
    E::Flags2, E::HasFallDirection, E::GuidByte6, E::GuidByte7, E::GuidByte4, E::GuidByte1,
    E::GuidByte5, E::GuidByte0, E::GuidByte2, E::GuidByte3, E::TransportPositionZ,
    E::TransportGuidByte2, E::TransportVehicleId, E::TransportGuidByte0, E::TransportGuidByte5,
    E::TransportPositionY, E::TransportGuidByte1, E::TransportPositionX, E::TransportTime2,
    E::TransportGuidByte4, E::TransportPositionO, E::TransportSeat, E::TransportGuidByte7,
    E::TransportTime, E::TransportGuidByte6, E::TransportGuidByte3, E::Pitch, E::Timestamp,
    E::FallHorizontalSpeed, E::FallSinAngle, E::FallCosAngle, E::FallVerticalSpeed, E::FallTime,
    E::PositionO, E::SplineElevation, E::End)

LAYOUT(MovementStartForward,
    E::PositionY, E::PositionZ, E::PositionX, E::GuidBit5, E::GuidBit2, E::GuidBit0,
    E::HasUnknownBit, E::HasMovementFlags, E::GuidBit7, E::GuidBit3, E::GuidBit1, E::HasOrientation,
    E::GuidBit6, E::HasSpline, E::HasSplineElevation, E::GuidBit4, E::HasTransportData,
    E::HasTimestamp, E::HasPitch, E::HasMovementFlags2, E::HasFallData, E::Flags,
    E::TransportGuidBit3, E::TransportGuidBit4, E::TransportGuidBit6, E::TransportGuidBit2,
    E::TransportGuidBit5, E::TransportGuidBit0, E::TransportGuidBit7, E::TransportGuidBit1,
    E::HasVehicleId, E::HasTransportTime2, E::HasFallDirection, E::Flags2, E::GuidByte2,
    E::GuidByte4, E::GuidByte6, E::GuidByte1, E::GuidByte7, E::GuidByte3, E::GuidByte5,
    E::GuidByte0, E::FallVerticalSpeed, E::FallHorizontalSpeed, E::FallCosAngle, E::FallSinAngle,
    E::FallTime, E::TransportGuidByte3, E::TransportPositionY, E::TransportPositionZ,
    E::TransportGuidByte1, E::TransportGuidByte4, E::TransportGuidByte7, E::TransportPositionO,
    E::TransportGuidByte2, E::TransportPositionX, E::TransportGuidByte5, E::TransportVehicleId,
    E::TransportTime, E::TransportGuidByte6, E::TransportGuidByte0, E::TransportSeat,
    E::TransportTime2, E::SplineElevation, E::Pitch, E::PositionO, E::Timestamp, E::End)

LAYOUT(MovementStartStrafeLeft,
    E::PositionZ, E::PositionX, E::PositionY, E::HasSplineElevation, E::GuidBit5, E::HasPitch,
    E::GuidBit6, E::HasTimestamp, E::GuidBit1, E::HasUnknownBit, E::GuidBit4, E::GuidBit0,
    E::GuidBit2, E::HasFallData, E::HasOrientation, E::GuidBit3, E::HasMovementFlags2, E::GuidBit7,
    E::HasSpline, E::HasMovementFlags, E::HasTransportData, E::HasFallDirection,
    E::HasTransportTime2, E::TransportGuidBit7, E::HasVehicleId, E::TransportGuidBit3,
    E::TransportGuidBit1, E::TransportGuidBit6, E::TransportGuidBit5, E::TransportGuidBit2,
    E::TransportGuidBit4, E::TransportGuidBit0, E::Flags, E::Flags2, E::GuidByte2, E::GuidByte6,
    E::GuidByte3, E::GuidByte1, E::GuidByte0, E::GuidByte7, E::GuidByte4, E::GuidByte5,
    E::FallSinAngle, E::FallHorizontalSpeed, E::FallCosAngle, E::FallTime, E::FallVerticalSpeed,
    E::TransportSeat, E::TransportGuidByte2, E::TransportTime2, E::TransportGuidByte3,
    E::TransportPositionZ, E::TransportVehicleId, E::TransportGuidByte0, E::TransportGuidByte7,
    E::TransportPositionY, E::TransportGuidByte5, E::TransportGuidByte1, E::TransportPositionO,
    E::TransportTime, E::TransportGuidByte6, E::TransportGuidByte4, E::TransportPositionX,
    E::Timestamp, E::PositionO, E::Pitch, E::SplineElevation, E::End)

LAYOUT(MovementStartStrafeRight,
    E::PositionY, E::PositionX, E::PositionZ, E::HasPitch, E::GuidBit1, E::HasOrientation,
    E::GuidBit4, E::HasSpline, E::HasUnknownBit, E::GuidBit5, E::HasFallData, E::HasSplineElevation,
    E::HasTimestamp, E::HasMovementFlags, E::GuidBit2, E::GuidBit7, E::GuidBit6, E::GuidBit3,
    E::HasMovementFlags2, E::HasTransportData, E::GuidBit0, E::TransportGuidBit7,
    E::TransportGuidBit2, E::TransportGuidBit3, E::TransportGuidBit5, E::TransportGuidBit0,
    E::HasTransportTime2, E::TransportGuidBit6, E::TransportGuidBit1, E::TransportGuidBit4,
    E::HasVehicleId, E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte7, E::GuidByte5,
    E::GuidByte3, E::GuidByte1, E::GuidByte2, E::GuidByte4, E::GuidByte6, E::GuidByte0,
    E::TransportGuidByte5, E::TransportGuidByte1, E::TransportGuidByte6, E::TransportPositionY,
    E::TransportPositionO, E::TransportGuidByte0, E::TransportGuidByte2, E::TransportSeat,
    E::TransportPositionX, E::TransportVehicleId, E::TransportTime, E::TransportGuidByte4,
    E::TransportGuidByte7, E::TransportTime2, E::TransportPositionZ, E::TransportGuidByte3,
    E::Pitch, E::PositionO, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed, E::FallTime,
    E::FallVerticalSpeed, E::Timestamp, E::SplineElevation, E::End)

LAYOUT(MovementStartTurnLeft,
    E::PositionY, E::PositionX, E::PositionZ, E::HasUnknownBit, E::GuidBit1, E::HasOrientation,
    E::HasSpline, E::HasMovementFlags, E::GuidBit4, E::GuidBit2, E::HasMovementFlags2, E::GuidBit5,
    E::GuidBit7, E::HasTransportData, E::GuidBit6, E::HasSplineElevation, E::GuidBit0, E::GuidBit3,
    E::HasPitch, E::HasTimestamp, E::HasFallData, E::Flags2, E::TransportGuidBit3,
    E::TransportGuidBit1, E::TransportGuidBit0, E::HasTransportTime2, E::TransportGuidBit2,
    E::HasVehicleId, E::TransportGuidBit4, E::TransportGuidBit7, E::TransportGuidBit5,
    E::TransportGuidBit6, E::HasFallDirection, E::Flags, E::GuidByte0, E::GuidByte4, E::GuidByte7,
    E::GuidByte5, E::GuidByte6, E::GuidByte3, E::GuidByte2, E::GuidByte1, E::FallSinAngle,
    E::FallCosAngle, E::FallHorizontalSpeed, E::FallVerticalSpeed, E::FallTime,
    E::TransportGuidByte0, E::TransportPositionX, E::TransportTime, E::TransportSeat,
    E::TransportPositionZ, E::TransportGuidByte4, E::TransportPositionO, E::TransportGuidByte2,
    E::TransportGuidByte6, E::TransportGuidByte1, E::TransportGuidByte3, E::TransportPositionY,
    E::TransportVehicleId, E::TransportTime2, E::TransportGuidByte5, E::TransportGuidByte7,
    E::Timestamp, E::Pitch, E::PositionO, E::SplineElevation, E::End)

LAYOUT(MovementStartTurnRight,
    E::PositionX, E::PositionZ, E::PositionY, E::GuidBit3, E::GuidBit5, E::HasMovementFlags,
    E::HasSpline, E::GuidBit0, E::HasOrientation, E::HasTransportData, E::GuidBit7,
    E::HasUnknownBit, E::HasMovementFlags2, E::GuidBit1, E::HasTimestamp, E::GuidBit6, E::GuidBit2,
    E::GuidBit4, E::HasSplineElevation, E::HasPitch, E::HasFallData, E::TransportGuidBit1,
    E::HasTransportTime2, E::TransportGuidBit6, E::TransportGuidBit0, E::TransportGuidBit3,
    E::TransportGuidBit5, E::TransportGuidBit2, E::TransportGuidBit4, E::TransportGuidBit7,
    E::HasVehicleId, E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte5, E::GuidByte0,
    E::GuidByte7, E::GuidByte3, E::GuidByte2, E::GuidByte1, E::GuidByte4, E::GuidByte6,
    E::TransportPositionY, E::TransportGuidByte0, E::TransportGuidByte4, E::TransportGuidByte1,
    E::TransportGuidByte6, E::TransportGuidByte2, E::TransportSeat, E::TransportPositionO,
    E::TransportGuidByte5, E::TransportVehicleId, E::TransportPositionZ, E::TransportPositionX,
    E::TransportTime, E::TransportGuidByte7, E::TransportGuidByte3, E::TransportTime2,
    E::FallHorizontalSpeed, E::FallSinAngle, E::FallCosAngle, E::FallTime, E::FallVerticalSpeed,
    E::Pitch, E::PositionO, E::SplineElevation, E::Timestamp, E::End)

LAYOUT(MovementStop,
    E::PositionX, E::PositionY, E::PositionZ, E::GuidBit3, E::GuidBit6, E::HasSplineElevation,
    E::HasSpline, E::HasOrientation, E::GuidBit7, E::HasMovementFlags, E::GuidBit5, E::HasFallData,
    E::HasMovementFlags2, E::HasTransportData, E::HasTimestamp, E::GuidBit4, E::GuidBit1,
    E::HasUnknownBit, E::GuidBit2, E::GuidBit0, E::HasPitch, E::TransportGuidBit7,
    E::TransportGuidBit4, E::TransportGuidBit1, E::TransportGuidBit5, E::HasTransportTime2,
    E::HasVehicleId, E::TransportGuidBit3, E::TransportGuidBit6, E::TransportGuidBit0,
    E::TransportGuidBit2, E::Flags, E::Flags2, E::HasFallDirection, E::GuidByte6, E::GuidByte3,
    E::GuidByte0, E::GuidByte4, E::GuidByte2, E::GuidByte1, E::GuidByte5, E::GuidByte7,
    E::TransportGuidByte4, E::TransportGuidByte7, E::TransportTime, E::TransportSeat,
    E::TransportPositionZ, E::TransportVehicleId, E::TransportGuidByte2, E::TransportGuidByte0,
    E::TransportPositionY, E::TransportGuidByte1, E::TransportGuidByte3, E::TransportTime2,
    E::TransportPositionX, E::TransportPositionO, E::TransportGuidByte5, E::TransportGuidByte6,
    E::Timestamp, E::PositionO, E::Pitch, E::SplineElevation, E::FallCosAngle, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallVerticalSpeed, E::FallTime, E::End)

LAYOUT(MovementStopStrafe,
    E::PositionY, E::PositionZ, E::PositionX, E::HasPitch, E::HasTimestamp, E::GuidBit2,
    E::HasFallData, E::GuidBit7, E::HasSplineElevation, E::GuidBit3, E::HasOrientation,
    E::HasMovementFlags2, E::HasTransportData, E::HasMovementFlags, E::HasSpline, E::GuidBit0,
    E::HasUnknownBit, E::GuidBit6, E::GuidBit5, E::GuidBit1, E::GuidBit4, E::TransportGuidBit7,
    E::TransportGuidBit0, E::TransportGuidBit4, E::TransportGuidBit2, E::TransportGuidBit6,
    E::TransportGuidBit5, E::HasTransportTime2, E::TransportGuidBit1, E::TransportGuidBit3,
    E::HasVehicleId, E::Flags, E::HasFallDirection, E::Flags2, E::GuidByte2, E::GuidByte7,
    E::GuidByte3, E::GuidByte4, E::GuidByte5, E::GuidByte6, E::GuidByte1, E::GuidByte0,
    E::TransportSeat, E::TransportGuidByte6, E::TransportPositionZ, E::TransportVehicleId,
    E::TransportGuidByte1, E::TransportGuidByte3, E::TransportGuidByte2, E::TransportGuidByte4,
    E::TransportGuidByte5, E::TransportTime, E::TransportPositionO, E::TransportPositionX,
    E::TransportGuidByte0, E::TransportPositionY, E::TransportTime2, E::TransportGuidByte7,
    E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle, E::FallTime, E::FallVerticalSpeed,
    E::SplineElevation, E::PositionO, E::Pitch, E::Timestamp, E::End)

LAYOUT(MovementStopTurn,
    E::PositionX, E::PositionZ, E::PositionY, E::GuidBit5, E::GuidBit4, E::HasFallData,
    E::HasUnknownBit, E::GuidBit1, E::GuidBit0, E::HasSpline, E::HasMovementFlags, E::GuidBit2,
    E::GuidBit6, E::HasPitch, E::HasTransportData, E::GuidBit3, E::HasSplineElevation,
    E::HasTimestamp, E::HasMovementFlags2, E::HasOrientation, E::GuidBit7, E::TransportGuidBit2,
    E::TransportGuidBit7, E::HasVehicleId, E::TransportGuidBit4, E::HasTransportTime2,
    E::TransportGuidBit5, E::TransportGuidBit0, E::TransportGuidBit3, E::TransportGuidBit6,
    E::TransportGuidBit1, E::HasFallDirection, E::Flags, E::Flags2, E::GuidByte3, E::GuidByte2,
    E::GuidByte6, E::GuidByte4, E::GuidByte0, E::GuidByte7, E::GuidByte1, E::GuidByte5,
    E::SplineElevation, E::TransportPositionX, E::TransportGuidByte5, E::TransportSeat,
    E::TransportGuidByte2, E::TransportGuidByte3, E::TransportPositionO, E::TransportTime2,
    E::TransportVehicleId, E::TransportGuidByte7, E::TransportGuidByte1, E::TransportGuidByte0,
    E::TransportGuidByte4, E::TransportPositionY, E::TransportPositionZ, E::TransportTime,
    E::TransportGuidByte6, E::FallTime, E::FallHorizontalSpeed, E::FallCosAngle, E::FallSinAngle,
    E::FallVerticalSpeed, E::Timestamp, E::Pitch, E::PositionO, E::End)

LAYOUT(MovementStartAscend,
    E::PositionX, E::PositionY, E::PositionZ, E::GuidBit0, E::GuidBit1, E::GuidBit7,
    E::HasUnknownBit, E::GuidBit5, E::HasTransportData, E::GuidBit2, E::HasSplineElevation,
    E::HasFallData, E::HasSpline, E::GuidBit3, E::HasMovementFlags2, E::GuidBit6,
    E::HasMovementFlags, E::HasPitch, E::HasTimestamp, E::HasOrientation, E::GuidBit4, E::Flags,
    E::TransportGuidBit0, E::TransportGuidBit2, E::HasVehicleId, E::TransportGuidBit6,
    E::TransportGuidBit1, E::TransportGuidBit4, E::HasTransportTime2, E::TransportGuidBit5,
    E::TransportGuidBit3, E::TransportGuidBit7, E::Flags2, E::HasFallDirection, E::GuidByte6,
    E::GuidByte3, E::GuidByte1, E::GuidByte4, E::GuidByte2, E::GuidByte0, E::GuidByte5,
    E::GuidByte7, E::TransportGuidByte3, E::TransportGuidByte5, E::TransportGuidByte4,
    E::TransportSeat, E::TransportGuidByte2, E::TransportPositionO, E::TransportPositionY,
    E::TransportGuidByte7, E::TransportTime2, E::TransportPositionZ, E::TransportVehicleId,
    E::TransportGuidByte6, E::TransportTime, E::TransportGuidByte0, E::TransportGuidByte1,
    E::TransportPositionX, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed, E::FallTime,
    E::FallVerticalSpeed, E::PositionO, E::Pitch, E::Timestamp, E::SplineElevation, E::End)

LAYOUT(MovementStartDescend,
    E::PositionY, E::PositionZ, E::PositionX, E::GuidBit0, E::HasPitch, E::HasFallData, E::GuidBit4,
    E::HasOrientation, E::HasTimestamp, E::HasMovementFlags2, E::HasMovementFlags, E::GuidBit6,
    E::HasUnknownBit, E::GuidBit1, E::HasTransportData, E::HasSpline, E::HasSplineElevation,
    E::GuidBit5, E::GuidBit3, E::GuidBit7, E::GuidBit2, E::TransportGuidBit0, E::TransportGuidBit1,
    E::TransportGuidBit2, E::HasVehicleId, E::TransportGuidBit7, E::TransportGuidBit4,
    E::TransportGuidBit6, E::HasTransportTime2, E::TransportGuidBit3, E::TransportGuidBit5,
    E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte2, E::GuidByte7, E::GuidByte6,
    E::GuidByte0, E::GuidByte1, E::GuidByte5, E::GuidByte4, E::GuidByte3, E::Pitch,
    E::TransportPositionZ, E::TransportSeat, E::TransportVehicleId, E::TransportGuidByte6,
    E::TransportTime2, E::TransportGuidByte1, E::TransportTime, E::TransportGuidByte4,
    E::TransportPositionY, E::TransportPositionX, E::TransportGuidByte7, E::TransportGuidByte3,
    E::TransportGuidByte5, E::TransportGuidByte2, E::TransportPositionO, E::TransportGuidByte0,
    E::FallTime, E::FallSinAngle, E::FallCosAngle, E::FallHorizontalSpeed, E::FallVerticalSpeed,
    E::Timestamp, E::PositionO, E::SplineElevation, E::End)

LAYOUT(MovementStartSwim,
    E::PositionZ, E::PositionX, E::PositionY, E::GuidBit3, E::HasSplineElevation, E::GuidBit4,
    E::GuidBit7, E::HasUnknownBit, E::HasPitch, E::GuidBit0, E::HasOrientation, E::HasMovementFlags,
    E::HasFallData, E::GuidBit5, E::HasTransportData, E::HasMovementFlags2, E::HasTimestamp,
    E::GuidBit6, E::GuidBit1, E::HasSpline, E::GuidBit2, E::TransportGuidBit2, E::TransportGuidBit3,
    E::TransportGuidBit7, E::TransportGuidBit5, E::TransportGuidBit4, E::TransportGuidBit0,
    E::HasTransportTime2, E::TransportGuidBit6, E::TransportGuidBit1, E::HasVehicleId, E::Flags,
    E::HasFallDirection, E::Flags2, E::GuidByte0, E::GuidByte2, E::GuidByte1, E::GuidByte5,
    E::GuidByte4, E::GuidByte6, E::GuidByte3, E::GuidByte7, E::Pitch, E::TransportTime2,
    E::TransportGuidByte2, E::TransportPositionY, E::TransportGuidByte3, E::TransportTime,
    E::TransportPositionX, E::TransportPositionO, E::TransportVehicleId, E::TransportGuidByte6,
    E::TransportGuidByte5, E::TransportGuidByte4, E::TransportPositionZ, E::TransportGuidByte1,
    E::TransportGuidByte7, E::TransportGuidByte0, E::TransportSeat, E::FallCosAngle,
    E::FallHorizontalSpeed, E::FallSinAngle, E::FallVerticalSpeed, E::FallTime, E::PositionO,
    E::Timestamp, E::SplineElevation, E::End)

LAYOUT(MovementStopSwim,
    E::PositionX, E::PositionY, E::PositionZ, E::HasSpline, E::HasPitch, E::GuidBit5, E::GuidBit3,
    E::GuidBit7, E::HasSplineElevation, E::HasMovementFlags, E::GuidBit4, E::HasMovementFlags2,
    E::GuidBit2, E::GuidBit6, E::HasTransportData, E::HasOrientation, E::HasUnknownBit,
    E::HasTimestamp, E::GuidBit1, E::GuidBit0, E::HasFallData, E::HasVehicleId,
    E::TransportGuidBit4, E::TransportGuidBit6, E::TransportGuidBit3, E::TransportGuidBit7,
    E::TransportGuidBit1, E::TransportGuidBit0, E::HasTransportTime2, E::TransportGuidBit2,
    E::TransportGuidBit5, E::Flags, E::Flags2, E::HasFallDirection, E::GuidByte0, E::GuidByte4,
    E::GuidByte3, E::GuidByte6, E::GuidByte7, E::GuidByte1, E::GuidByte5, E::GuidByte2,
    E::TransportVehicleId, E::TransportGuidByte4, E::TransportTime, E::TransportPositionZ,
    E::TransportSeat, E::TransportGuidByte3, E::TransportGuidByte2, E::TransportPositionY,
    E::TransportGuidByte7, E::TransportGuidByte1, E::TransportGuidByte6, E::TransportGuidByte5,
    E::TransportTime2, E::TransportPositionX, E::TransportGuidByte0, E::TransportPositionO,
    E::FallVerticalSpeed, E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle, E::FallTime,
    E::Timestamp, E::Pitch, E::PositionO, E::SplineElevation, E::End)

LAYOUT(MovementStopAscend,
    E::PositionZ, E::PositionY, E::PositionX, E::GuidBit3, E::GuidBit2, E::HasFallData, E::GuidBit7,
    E::HasOrientation, E::HasPitch, E::HasSpline, E::HasUnknownBit, E::GuidBit1, E::GuidBit4,
    E::HasMovementFlags, E::GuidBit0, E::GuidBit6, E::HasSplineElevation, E::HasTransportData,
    E::HasMovementFlags2, E::HasTimestamp, E::GuidBit5, E::HasTransportTime2, E::HasVehicleId,
    E::TransportGuidBit1, E::TransportGuidBit3, E::TransportGuidBit2, E::TransportGuidBit5,
    E::TransportGuidBit7, E::TransportGuidBit4, E::TransportGuidBit6, E::TransportGuidBit0,
    E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte7, E::GuidByte5, E::GuidByte4,
    E::GuidByte3, E::GuidByte2, E::GuidByte1, E::GuidByte0, E::GuidByte6, E::TransportGuidByte5,
    E::TransportPositionY, E::TransportGuidByte7, E::TransportGuidByte2, E::TransportGuidByte6,
    E::TransportTime2, E::TransportVehicleId, E::TransportPositionO, E::TransportGuidByte3,
    E::TransportGuidByte4, E::TransportSeat, E::TransportGuidByte1, E::TransportTime,
    E::TransportGuidByte0, E::TransportPositionX, E::TransportPositionZ, E::FallTime,
    E::FallVerticalSpeed, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed, E::Pitch,
    E::SplineElevation, E::Timestamp, E::PositionO, E::End)

LAYOUT(MovementStopPitch,
    E::PositionX, E::PositionZ, E::PositionY, E::GuidBit0, E::GuidBit5, E::GuidBit3, E::HasFallData,
    E::GuidBit2, E::GuidBit4, E::GuidBit7, E::HasPitch, E::HasSpline, E::HasOrientation,
    E::HasUnknownBit, E::HasMovementFlags2, E::HasSplineElevation, E::GuidBit6, E::GuidBit1,
    E::HasTimestamp, E::HasTransportData, E::HasMovementFlags, E::TransportGuidBit7,
    E::TransportGuidBit0, E::TransportGuidBit5, E::TransportGuidBit2, E::HasVehicleId,
    E::TransportGuidBit6, E::TransportGuidBit3, E::HasTransportTime2, E::TransportGuidBit4,
    E::TransportGuidBit1, E::HasFallDirection, E::Flags2, E::Flags, E::GuidByte1, E::GuidByte7,
    E::GuidByte0, E::GuidByte6, E::GuidByte4, E::GuidByte3, E::GuidByte5, E::GuidByte2,
    E::Timestamp, E::FallHorizontalSpeed, E::FallSinAngle, E::FallCosAngle, E::FallVerticalSpeed,
    E::FallTime, E::TransportGuidByte5, E::TransportGuidByte2, E::TransportTime2,
    E::TransportPositionX, E::TransportPositionO, E::TransportGuidByte6, E::TransportGuidByte3,
    E::TransportGuidByte1, E::TransportPositionZ, E::TransportVehicleId, E::TransportGuidByte0,
    E::TransportGuidByte4, E::TransportTime, E::TransportPositionY, E::TransportSeat,
    E::TransportGuidByte7, E::SplineElevation, E::PositionO, E::Pitch, E::End)

LAYOUT(MovementStartPitchDown,
    E::PositionX, E::PositionZ, E::PositionY, E::HasMovementFlags, E::GuidBit7, E::GuidBit6,
    E::HasPitch, E::HasUnknownBit, E::GuidBit1, E::GuidBit4, E::HasTimestamp, E::HasSplineElevation,
    E::HasMovementFlags2, E::HasTransportData, E::GuidBit0, E::GuidBit5, E::GuidBit3, E::HasSpline,
    E::HasOrientation, E::HasFallData, E::GuidBit2, E::TransportGuidBit1, E::TransportGuidBit5,
    E::HasVehicleId, E::TransportGuidBit7, E::TransportGuidBit4, E::TransportGuidBit6,
    E::HasTransportTime2, E::TransportGuidBit0, E::TransportGuidBit3, E::TransportGuidBit2,
    E::Flags2, E::HasFallDirection, E::Flags, E::GuidByte3, E::GuidByte7, E::GuidByte0,
    E::GuidByte5, E::GuidByte2, E::GuidByte6, E::GuidByte4, E::GuidByte1, E::TransportVehicleId,
    E::TransportGuidByte3, E::TransportTime2, E::TransportGuidByte5, E::TransportGuidByte1,
    E::TransportPositionY, E::TransportGuidByte6, E::TransportGuidByte4, E::TransportTime,
    E::TransportPositionZ, E::TransportPositionX, E::TransportGuidByte0, E::TransportSeat,
    E::TransportGuidByte2, E::TransportGuidByte7, E::TransportPositionO, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallCosAngle, E::FallVerticalSpeed, E::FallTime, E::Pitch,
    E::PositionO, E::SplineElevation, E::Timestamp, E::End)

LAYOUT(MovementStartPitchUp,
    E::PositionZ, E::PositionY, E::PositionX, E::GuidBit4, E::HasMovementFlags,
    E::HasMovementFlags2, E::HasSpline, E::GuidBit2, E::GuidBit6, E::GuidBit3, E::HasUnknownBit,
    E::HasSplineElevation, E::HasFallData, E::GuidBit0, E::HasTransportData, E::GuidBit1,
    E::GuidBit5, E::HasPitch, E::HasTimestamp, E::HasOrientation, E::GuidBit7, E::Flags2,
    E::TransportGuidBit7, E::TransportGuidBit3, E::TransportGuidBit1, E::HasTransportTime2,
    E::TransportGuidBit4, E::HasVehicleId, E::TransportGuidBit0, E::TransportGuidBit6,
    E::TransportGuidBit5, E::TransportGuidBit2, E::Flags, E::HasFallDirection, E::GuidByte0,
    E::GuidByte3, E::GuidByte4, E::GuidByte6, E::GuidByte7, E::GuidByte1, E::GuidByte5,
    E::GuidByte2, E::TransportPositionX, E::TransportGuidByte1, E::TransportGuidByte3,
    E::TransportGuidByte2, E::TransportGuidByte0, E::TransportTime, E::TransportPositionZ,
    E::TransportPositionO, E::TransportGuidByte7, E::TransportTime2, E::TransportSeat,
    E::TransportGuidByte6, E::TransportGuidByte5, E::TransportVehicleId, E::TransportPositionY,
    E::TransportGuidByte4, E::FallTime, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed,
    E::FallVerticalSpeed, E::PositionO, E::SplineElevation, E::Pitch, E::Timestamp, E::End)

LAYOUT(MoveChngTransport,
    E::PositionY, E::PositionX, E::PositionZ, E::GuidBit4, E::GuidBit0, E::GuidBit2,
    E::HasTransportData, E::HasSpline, E::HasOrientation, E::GuidBit6, E::HasTimestamp, E::GuidBit5,
    E::GuidBit7, E::HasPitch, E::HasUnknownBit, E::GuidBit3, E::HasSplineElevation, E::GuidBit1,
    E::HasFallData, E::HasMovementFlags, E::HasMovementFlags2, E::TransportGuidBit3,
    E::HasVehicleId, E::TransportGuidBit4, E::TransportGuidBit1, E::TransportGuidBit6,
    E::TransportGuidBit2, E::HasTransportTime2, E::TransportGuidBit0, E::TransportGuidBit7,
    E::TransportGuidBit5, E::Flags, E::Flags2, E::HasFallDirection, E::GuidByte7, E::GuidByte5,
    E::GuidByte1, E::GuidByte2, E::GuidByte6, E::GuidByte4, E::GuidByte0, E::GuidByte3,
    E::TransportPositionY, E::TransportSeat, E::TransportGuidByte1, E::TransportGuidByte0,
    E::TransportGuidByte2, E::TransportGuidByte3, E::TransportGuidByte6, E::TransportPositionZ,
    E::TransportPositionX, E::TransportVehicleId, E::TransportTime, E::TransportGuidByte7,
    E::TransportGuidByte5, E::TransportGuidByte4, E::TransportPositionO, E::TransportTime2,
    E::FallTime, E::FallHorizontalSpeed, E::FallCosAngle, E::FallSinAngle, E::FallVerticalSpeed,
    E::PositionO, E::Pitch, E::SplineElevation, E::Timestamp, E::End)

LAYOUT(MoveSplineDone,
    E::PositionY, E::PositionX, E::PositionZ, E::GuidBit6, E::HasOrientation, E::HasFallData,
    E::HasTimestamp, E::GuidBit2, E::HasSplineElevation, E::GuidBit4, E::HasTransportData,
    E::GuidBit3, E::HasMovementFlags, E::GuidBit0, E::HasUnknownBit, E::GuidBit1, E::GuidBit5,
    E::HasPitch, E::HasSpline, E::HasMovementFlags2, E::GuidBit7, E::TransportGuidBit1,
    E::TransportGuidBit7, E::TransportGuidBit5, E::TransportGuidBit3, E::TransportGuidBit4,
    E::TransportGuidBit6, E::HasTransportTime2, E::TransportGuidBit2, E::HasVehicleId,
    E::TransportGuidBit0, E::HasFallDirection, E::Flags2, E::Flags, E::GuidByte7, E::GuidByte4,
    E::GuidByte5, E::GuidByte6, E::GuidByte0, E::GuidByte1, E::GuidByte2, E::GuidByte3,
    E::FallVerticalSpeed, E::FallSinAngle, E::FallCosAngle, E::FallHorizontalSpeed, E::FallTime,
    E::Pitch, E::PositionO, E::TransportGuidByte1, E::TransportVehicleId, E::TransportGuidByte7,
    E::TransportTime, E::TransportPositionY, E::TransportPositionX, E::TransportPositionZ,
    E::TransportSeat, E::TransportPositionO, E::TransportGuidByte0, E::TransportTime2,
    E::TransportGuidByte2, E::TransportGuidByte3, E::TransportGuidByte5, E::TransportGuidByte6,
    E::TransportGuidByte4, E::Timestamp, E::SplineElevation, E::End)

LAYOUT(MoveNotActiveMover,
    E::PositionZ, E::PositionX, E::PositionY, E::HasMovementFlags2, E::HasTransportData,
    E::GuidBit6, E::HasSplineElevation, E::GuidBit3, E::HasUnknownBit, E::HasTimestamp, E::GuidBit0,
    E::HasOrientation, E::GuidBit5, E::HasPitch, E::GuidBit1, E::GuidBit4, E::GuidBit7,
    E::HasSpline, E::GuidBit2, E::HasFallData, E::HasMovementFlags, E::TransportGuidBit4,
    E::TransportGuidBit0, E::TransportGuidBit1, E::TransportGuidBit6, E::TransportGuidBit2,
    E::HasVehicleId, E::TransportGuidBit5, E::TransportGuidBit7, E::HasTransportTime2,
    E::TransportGuidBit3, E::HasFallDirection, E::Flags, E::Flags2, E::GuidByte1, E::GuidByte0,
    E::GuidByte4, E::GuidByte2, E::GuidByte7, E::GuidByte5, E::GuidByte6, E::GuidByte3,
    E::FallVerticalSpeed, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed, E::FallTime,
    E::TransportVehicleId, E::TransportGuidByte1, E::TransportTime2, E::TransportPositionO,
    E::TransportGuidByte0, E::TransportSeat, E::TransportGuidByte4, E::TransportGuidByte6,
    E::TransportGuidByte3, E::TransportGuidByte5, E::TransportPositionY, E::TransportPositionX,
    E::TransportGuidByte2, E::TransportPositionZ, E::TransportGuidByte7, E::TransportTime,
    E::Timestamp, E::SplineElevation, E::Pitch, E::PositionO, E::End)

LAYOUT(DismissControlledVehicle,
    E::PositionY, E::PositionZ, E::PositionX, E::HasSplineElevation, E::HasFallData,
    E::HasTransportData, E::GuidBit4, E::GuidBit7, E::HasMovementFlags2, E::GuidBit2,
    E::HasOrientation, E::GuidBit0, E::GuidBit6, E::HasPitch, E::HasTimestamp, E::GuidBit1,
    E::HasUnknownBit, E::HasSpline, E::GuidBit3, E::HasMovementFlags, E::GuidBit5,
    E::TransportGuidBit5, E::HasVehicleId, E::TransportGuidBit2, E::TransportGuidBit0,
    E::TransportGuidBit7, E::TransportGuidBit3, E::TransportGuidBit4, E::TransportGuidBit1,
    E::TransportGuidBit6, E::HasTransportTime2, E::Flags2, E::HasFallDirection, E::Flags,
    E::GuidByte6, E::GuidByte3, E::GuidByte1, E::GuidByte5, E::GuidByte2, E::GuidByte4,
    E::GuidByte7, E::GuidByte0, E::Timestamp, E::TransportVehicleId, E::TransportGuidByte4,
    E::TransportGuidByte7, E::TransportTime2, E::TransportGuidByte6, E::TransportTime,
    E::TransportPositionZ, E::TransportGuidByte0, E::TransportPositionX, E::TransportGuidByte3,
    E::TransportSeat, E::TransportPositionY, E::TransportPositionO, E::TransportGuidByte1,
    E::TransportGuidByte5, E::TransportGuidByte2, E::FallTime, E::FallHorizontalSpeed,
    E::FallSinAngle, E::FallCosAngle, E::FallVerticalSpeed, E::PositionO, E::SplineElevation,
    E::Pitch, E::End)

LAYOUT(MovementSetRunMode,
    E::PositionY, E::PositionX, E::PositionZ, E::HasTimestamp, E::HasMovementFlags2, E::GuidBit1,
    E::HasSpline, E::HasMovementFlags, E::GuidBit7, E::HasTransportData, E::HasUnknownBit,
    E::GuidBit0, E::GuidBit3, E::HasSplineElevation, E::GuidBit5, E::HasPitch, E::GuidBit6,
    E::GuidBit4, E::HasFallData, E::HasOrientation, E::GuidBit2, E::TransportGuidBit6,
    E::TransportGuidBit4, E::HasVehicleId, E::TransportGuidBit5, E::TransportGuidBit3,
    E::TransportGuidBit1, E::HasTransportTime2, E::TransportGuidBit7, E::TransportGuidBit0,
    E::TransportGuidBit2, E::HasFallDirection, E::Flags2, E::Flags, E::GuidByte3, E::GuidByte6,
    E::GuidByte0, E::GuidByte7, E::GuidByte4, E::GuidByte1, E::GuidByte5, E::GuidByte2, E::Pitch,
    E::TransportTime2, E::TransportGuidByte3, E::TransportPositionX, E::TransportSeat,
    E::TransportGuidByte5, E::TransportGuidByte1, E::TransportPositionZ, E::TransportGuidByte2,
    E::TransportGuidByte7, E::TransportPositionO, E::TransportGuidByte4, E::TransportTime,
    E::TransportVehicleId, E::TransportGuidByte0, E::TransportPositionY, E::TransportGuidByte6,
    E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle, E::FallTime, E::FallVerticalSpeed,
    E::SplineElevation, E::Timestamp, E::PositionO, E::End)

LAYOUT(MovementSetWalkMode,
    E::PositionY, E::PositionX, E::PositionZ, E::GuidBit6, E::HasSpline, E::HasTimestamp,
    E::GuidBit0, E::GuidBit1, E::HasMovementFlags, E::HasPitch, E::GuidBit7, E::HasSplineElevation,
    E::GuidBit4, E::HasOrientation, E::HasTransportData, E::GuidBit2, E::GuidBit5, E::GuidBit3,
    E::HasUnknownBit, E::HasMovementFlags2, E::HasFallData, E::TransportGuidBit2,
    E::TransportGuidBit0, E::TransportGuidBit6, E::TransportGuidBit1, E::TransportGuidBit3,
    E::HasTransportTime2, E::TransportGuidBit5, E::HasVehicleId, E::TransportGuidBit4,
    E::TransportGuidBit7, E::HasFallDirection, E::Flags, E::Flags2, E::GuidByte5, E::GuidByte6,
    E::GuidByte4, E::GuidByte7, E::GuidByte3, E::GuidByte0, E::GuidByte2, E::GuidByte1,
    E::TransportGuidByte2, E::TransportGuidByte5, E::TransportSeat, E::TransportPositionZ,
    E::TransportGuidByte3, E::TransportGuidByte6, E::TransportGuidByte0, E::TransportTime,
    E::TransportGuidByte4, E::TransportTime2, E::TransportPositionO, E::TransportPositionX,
    E::TransportVehicleId, E::TransportGuidByte7, E::TransportPositionY, E::TransportGuidByte1,
    E::FallSinAngle, E::FallHorizontalSpeed, E::FallCosAngle, E::FallVerticalSpeed, E::FallTime,
    E::SplineElevation, E::Pitch, E::Timestamp, E::PositionO, E::End)

LAYOUT(MovementSetCanFly,
    E::PositionZ, E::PositionX, E::PositionY, E::HasTransportData, E::GuidBit1, E::GuidBit6,
    E::GuidBit4, E::GuidBit2, E::HasFallData, E::GuidBit0, E::HasMovementFlags, E::GuidBit3,
    E::HasMovementFlags2, E::GuidBit5, E::HasTimestamp, E::HasSplineElevation, E::HasSpline,
    E::HasOrientation, E::GuidBit7, E::HasUnknownBit, E::HasPitch, E::TransportGuidBit3,
    E::HasTransportTime2, E::TransportGuidBit6, E::TransportGuidBit0, E::TransportGuidBit7,
    E::TransportGuidBit4, E::HasVehicleId, E::TransportGuidBit1, E::TransportGuidBit2,
    E::TransportGuidBit5, E::Flags2, E::HasFallDirection, E::Flags, E::GuidByte2, E::GuidByte0,
    E::GuidByte4, E::GuidByte7, E::GuidByte5, E::GuidByte1, E::GuidByte3, E::GuidByte6,
    E::TransportPositionZ, E::TransportGuidByte3, E::TransportGuidByte5, E::TransportGuidByte4,
    E::TransportSeat, E::TransportPositionX, E::TransportGuidByte2, E::TransportTime2,
    E::TransportPositionY, E::TransportGuidByte1, E::TransportTime, E::TransportGuidByte7,
    E::TransportGuidByte0, E::TransportPositionO, E::TransportGuidByte6, E::TransportVehicleId,
    E::SplineElevation, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed,
    E::FallVerticalSpeed, E::FallTime, E::Pitch, E::Timestamp, E::PositionO, E::End)

LAYOUT(MovementSetCanTransitionBetweenSwimAndFlyAck,
    E::PositionZ, E::PositionY, E::MovementCounter, E::PositionX, E::GuidBit3, E::HasOrientation,
    E::HasTransportData, E::HasMovementFlags, E::HasMovementFlags2, E::HasSplineElevation,
    E::HasTimestamp, E::HasSpline, E::HasUnknownBit, E::HasFallData, E::GuidBit5, E::GuidBit0,
    E::GuidBit1, E::GuidBit4, E::GuidBit6, E::GuidBit2, E::HasPitch, E::GuidBit7,
    E::TransportGuidBit4, E::TransportGuidBit2, E::HasTransportTime2, E::TransportGuidBit0,
    E::TransportGuidBit3, E::TransportGuidBit6, E::TransportGuidBit5, E::HasVehicleId,
    E::TransportGuidBit7, E::TransportGuidBit1, E::Flags2, E::HasFallDirection, E::Flags,
    E::GuidByte3, E::GuidByte2, E::GuidByte0, E::GuidByte4, E::GuidByte1, E::GuidByte5,
    E::GuidByte7, E::GuidByte6, E::FallTime, E::FallHorizontalSpeed, E::FallSinAngle,
    E::FallCosAngle, E::FallVerticalSpeed, E::TransportPositionY, E::TransportPositionZ,
    E::TransportGuidByte5, E::TransportGuidByte1, E::TransportGuidByte4, E::TransportTime,
    E::TransportSeat, E::TransportGuidByte3, E::TransportGuidByte0, E::TransportGuidByte2,
    E::TransportGuidByte7, E::TransportPositionX, E::TransportTime2, E::TransportGuidByte6,
    E::TransportPositionO, E::TransportVehicleId, E::Pitch, E::PositionO, E::Timestamp,
    E::SplineElevation, E::End)

LAYOUT(MovementUpdateSwimSpeed,
    E::HasMovementFlags, E::GuidBit2, E::HasSpline, E::HasTimestamp, E::Flags, E::HasFallData,
    E::GuidBit3, E::HasTransportData, E::HasUnknownBit, E::GuidBit7, E::HasFallDirection,
    E::TransportGuidBit4, E::TransportGuidBit5, E::TransportGuidBit3, E::HasTransportTime2,
    E::TransportGuidBit2, E::HasVehicleId, E::TransportGuidBit0, E::TransportGuidBit1,
    E::TransportGuidBit6, E::TransportGuidBit7, E::HasMovementFlags2, E::GuidBit6,
    E::HasSplineElevation, E::GuidBit0, E::GuidBit4, E::GuidBit5, E::HasOrientation, E::HasPitch,
    E::Flags2, E::GuidBit1, E::FlushBits, E::TransportGuidByte2, E::TransportPositionX,
    E::TransportVehicleId, E::TransportTime2, E::TransportGuidByte6, E::TransportPositionO,
    E::TransportGuidByte0, E::TransportGuidByte4, E::TransportPositionY, E::TransportTime,
    E::TransportGuidByte1, E::TransportGuidByte7, E::TransportPositionZ, E::TransportSeat,
    E::TransportGuidByte3, E::TransportGuidByte5, E::PositionX, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallCosAngle, E::FallTime, E::FallVerticalSpeed, E::GuidByte7,
    E::Timestamp, E::PositionY, E::SplineElevation, E::PositionZ, E::ExtraFloat, E::GuidByte4,
    E::PositionO, E::GuidByte2, E::GuidByte3, E::GuidByte0, E::GuidByte6, E::GuidByte1,
    E::GuidByte5, E::Pitch, E::End)

LAYOUT(MovementUpdateRunSpeed,
    E::PositionZ, E::PositionX, E::PositionY, E::ExtraFloat, E::GuidBit6, E::HasMovementFlags2,
    E::HasPitch, E::GuidBit2, E::GuidBit5, E::HasSplineElevation, E::HasSpline, E::HasMovementFlags,
    E::HasTimestamp, E::GuidBit1, E::Flags2, E::GuidBit3, E::Flags, E::GuidBit7, E::GuidBit0,
    E::HasOrientation, E::HasTransportData, E::TransportGuidBit5, E::HasTransportTime2,
    E::HasVehicleId, E::TransportGuidBit7, E::TransportGuidBit4, E::TransportGuidBit2,
    E::TransportGuidBit3, E::TransportGuidBit6, E::TransportGuidBit1, E::TransportGuidBit0,
    E::HasFallData, E::HasFallDirection, E::GuidBit4, E::HasUnknownBit, E::FlushBits,
    E::TransportGuidByte4, E::TransportGuidByte5, E::TransportPositionX, E::TransportPositionO,
    E::TransportGuidByte1, E::TransportGuidByte0, E::TransportGuidByte6, E::TransportTime,
    E::TransportGuidByte7, E::TransportSeat, E::TransportTime2, E::TransportPositionY,
    E::TransportGuidByte3, E::TransportGuidByte2, E::TransportVehicleId, E::TransportPositionZ,
    E::Timestamp, E::FallSinAngle, E::FallHorizontalSpeed, E::FallCosAngle, E::FallVerticalSpeed,
    E::FallTime, E::Pitch, E::GuidByte6, E::SplineElevation, E::GuidByte5, E::GuidByte7,
    E::GuidByte4, E::PositionO, E::GuidByte0, E::GuidByte3, E::GuidByte2, E::GuidByte1, E::End)

LAYOUT(MovementUpdateFlightSpeed,
    E::PositionY, E::ExtraFloat, E::PositionX, E::PositionZ, E::HasMovementFlags, E::GuidBit4,
    E::HasUnknownBit, E::GuidBit5, E::GuidBit1, E::Flags, E::HasFallData, E::GuidBit2,
    E::HasOrientation, E::HasPitch, E::HasSplineElevation, E::GuidBit7, E::HasTimestamp,
    E::HasTransportData, E::TransportGuidBit3, E::TransportGuidBit5, E::TransportGuidBit1,
    E::TransportGuidBit0, E::TransportGuidBit6, E::TransportGuidBit4, E::TransportGuidBit2,
    E::HasVehicleId, E::TransportGuidBit7, E::HasTransportTime2, E::GuidBit6, E::GuidBit3,
    E::HasMovementFlags2, E::HasSpline, E::Flags2, E::HasFallDirection, E::GuidBit0, E::FlushBits,
    E::SplineElevation, E::Pitch, E::GuidByte0, E::GuidByte5, E::TransportPositionO,
    E::TransportTime, E::TransportGuidByte5, E::TransportGuidByte4, E::TransportGuidByte6,
    E::TransportPositionZ, E::TransportGuidByte7, E::TransportGuidByte0, E::TransportGuidByte2,
    E::TransportGuidByte3, E::TransportPositionY, E::TransportVehicleId, E::TransportTime2,
    E::TransportGuidByte1, E::TransportSeat, E::TransportPositionX, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallCosAngle, E::FallVerticalSpeed, E::FallTime, E::GuidByte1,
    E::GuidByte6, E::Timestamp, E::GuidByte7, E::GuidByte2, E::GuidByte4, E::PositionO,
    E::GuidByte3, E::End)

LAYOUT(MovementSetCollisionHeight,
    E::ExtraTwoBits, E::GuidBit6, E::GuidBit1, E::GuidBit4, E::GuidBit7, E::GuidBit5, E::GuidBit2,
    E::GuidBit0, E::GuidBit3, E::FlushBits, E::GuidByte6, E::GuidByte0, E::GuidByte4, E::GuidByte3,
    E::GuidByte5, E::MovementCounter, E::GuidByte1, E::GuidByte2, E::GuidByte7, E::ExtraFloat,
    E::End)

LAYOUT(MovementUpdateCollisionHeight,
    E::PositionZ, E::ExtraFloat, E::PositionX, E::PositionY, E::GuidBit6, E::GuidBit7,
    E::HasSplineElevation, E::HasTransportData, E::TransportGuidBit6, E::TransportGuidBit4,
    E::TransportGuidBit3, E::TransportGuidBit7, E::TransportGuidBit5, E::HasVehicleId,
    E::TransportGuidBit1, E::TransportGuidBit0, E::TransportGuidBit2, E::HasTransportTime2,
    E::GuidBit3, E::HasOrientation, E::GuidBit4, E::GuidBit5, E::HasTimestamp, E::HasFallData,
    E::GuidBit0, E::HasPitch, E::HasHeightChangeFailed, E::GuidBit2, E::HasMovementFlags,
    E::HasFallDirection, E::GuidBit1, E::HasMovementFlags2, E::HasSpline, E::Flags2, E::Flags,
    E::FlushBits, E::GuidByte3, E::TransportGuidByte7, E::TransportTime, E::TransportGuidByte4,
    E::TransportVehicleId, E::TransportTime2, E::TransportGuidByte5, E::TransportPositionO,
    E::TransportPositionX, E::TransportGuidByte6, E::TransportGuidByte0, E::TransportPositionY,
    E::TransportGuidByte2, E::TransportPositionZ, E::TransportGuidByte3, E::TransportGuidByte1,
    E::TransportSeat, E::Pitch, E::GuidByte6, E::FallSinAngle, E::FallCosAngle,
    E::FallHorizontalSpeed, E::FallVerticalSpeed, E::FallTime, E::GuidByte7, E::PositionO,
    E::GuidByte0, E::GuidByte5, E::GuidByte4, E::SplineElevation, E::Timestamp, E::GuidByte2,
    E::GuidByte1, E::End)

LAYOUT(MovementForceRunSpeedChangeAck,
    E::MovementCounter, E::PositionX, E::ExtraFloat, E::PositionZ, E::PositionY, E::GuidBit2,
    E::GuidBit4, E::GuidBit1, E::GuidBit7, E::HasOrientation, E::HasFallData, E::GuidBit0,
    E::HasSpline, E::HasTransportData, E::HasTimestamp, E::HasMovementFlags2, E::GuidBit6,
    E::HasUnknownBit, E::HasSplineElevation, E::HasPitch, E::GuidBit5, E::HasMovementFlags,
    E::GuidBit3, E::HasVehicleId, E::TransportGuidBit5, E::HasTransportTime2, E::TransportGuidBit3,
    E::TransportGuidBit2, E::TransportGuidBit0, E::TransportGuidBit7, E::TransportGuidBit6,
    E::TransportGuidBit1, E::TransportGuidBit4, E::Flags, E::HasFallDirection, E::Flags2,
    E::GuidByte6, E::GuidByte4, E::GuidByte1, E::GuidByte3, E::GuidByte5, E::GuidByte2,
    E::GuidByte7, E::GuidByte0, E::TransportPositionZ, E::TransportGuidByte6, E::TransportGuidByte1,
    E::TransportPositionY, E::TransportGuidByte0, E::TransportGuidByte5, E::TransportTime2,
    E::TransportPositionX, E::TransportTime, E::TransportGuidByte7, E::TransportPositionO,
    E::TransportGuidByte3, E::TransportVehicleId, E::TransportGuidByte2, E::TransportSeat,
    E::TransportGuidByte4, E::FallVerticalSpeed, E::FallHorizontalSpeed, E::FallCosAngle,
    E::FallSinAngle, E::FallTime, E::SplineElevation, E::Pitch, E::Timestamp, E::PositionO, E::End)

LAYOUT(MovementSetCollisionHeightAck,
    E::ExtraFloat, E::PositionX, E::PositionZ, E::MovementCounter, E::PositionY, E::GuidBit6,
    E::GuidBit4, E::ExtraTwoBits, E::HasPitch, E::GuidBit5, E::HasHeightChangeFailed, E::GuidBit2,
    E::GuidBit1, E::HasFallData, E::GuidBit3, E::HasSpline, E::GuidBit7, E::HasMovementFlags,
    E::HasTransportData, E::HasTimestamp, E::HasSplineElevation, E::HasMovementFlags2,
    E::HasOrientation, E::GuidBit0, E::TransportGuidBit4, E::TransportGuidBit3,
    E::HasTransportTime2, E::HasVehicleId, E::TransportGuidBit5, E::TransportGuidBit1,
    E::TransportGuidBit7, E::TransportGuidBit2, E::TransportGuidBit6, E::TransportGuidBit0,
    E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte0, E::GuidByte3, E::GuidByte1,
    E::GuidByte5, E::GuidByte7, E::GuidByte6, E::GuidByte2, E::GuidByte4, E::TransportPositionX,
    E::TransportGuidByte4, E::TransportTime2, E::TransportGuidByte0, E::TransportPositionO,
    E::TransportPositionY, E::TransportGuidByte7, E::TransportSeat, E::TransportGuidByte5,
    E::TransportGuidByte2, E::TransportTime, E::TransportGuidByte6, E::TransportGuidByte3,
    E::TransportGuidByte1, E::TransportVehicleId, E::TransportPositionZ, E::FallVerticalSpeed,
    E::FallTime, E::FallSinAngle, E::FallCosAngle, E::FallHorizontalSpeed, E::Timestamp,
    E::SplineElevation, E::PositionO, E::Pitch, E::End)

LAYOUT(MovementForceFlightSpeedChangeAck,
    E::MovementCounter, E::PositionZ, E::PositionX, E::PositionY, E::ExtraFloat, E::GuidBit4,
    E::GuidBit5, E::HasUnknownBit, E::HasMovementFlags, E::HasSplineElevation, E::GuidBit3,
    E::HasMovementFlags2, E::GuidBit7, E::HasTransportData, E::GuidBit0, E::HasFallData,
    E::HasTimestamp, E::HasPitch, E::HasSpline, E::HasOrientation, E::GuidBit6, E::GuidBit2,
    E::GuidBit1, E::TransportGuidBit6, E::TransportGuidBit1, E::TransportGuidBit7,
    E::HasTransportTime2, E::TransportGuidBit0, E::HasVehicleId, E::TransportGuidBit3,
    E::TransportGuidBit2, E::TransportGuidBit5, E::TransportGuidBit4, E::HasFallDirection,
    E::Flags2, E::Flags, E::GuidByte5, E::GuidByte6, E::GuidByte1, E::GuidByte7, E::GuidByte3,
    E::GuidByte0, E::GuidByte2, E::GuidByte4, E::TransportGuidByte6, E::TransportGuidByte0,
    E::TransportSeat, E::TransportTime2, E::TransportPositionO, E::TransportGuidByte1,
    E::TransportGuidByte5, E::TransportVehicleId, E::TransportGuidByte2, E::TransportPositionZ,
    E::TransportGuidByte7, E::TransportGuidByte3, E::TransportGuidByte4, E::TransportPositionX,
    E::TransportTime, E::TransportPositionY, E::Timestamp, E::SplineElevation, E::FallCosAngle,
    E::FallSinAngle, E::FallHorizontalSpeed, E::FallTime, E::FallVerticalSpeed, E::Pitch,
    E::PositionO, E::End)

LAYOUT(MovementSetCanFlyAck,
    E::PositionY, E::MovementCounter, E::PositionX, E::PositionZ, E::GuidBit3, E::HasTimestamp,
    E::GuidBit4, E::GuidBit0, E::HasOrientation, E::HasFallData, E::GuidBit2, E::GuidBit5,
    E::HasSplineElevation, E::HasMovementFlags2, E::HasUnknownBit, E::GuidBit7, E::HasSpline,
    E::GuidBit6, E::GuidBit1, E::HasMovementFlags, E::HasTransportData, E::HasPitch,
    E::TransportGuidBit3, E::TransportGuidBit1, E::TransportGuidBit2, E::HasVehicleId,
    E::HasTransportTime2, E::TransportGuidBit0, E::TransportGuidBit5, E::TransportGuidBit7,
    E::TransportGuidBit4, E::TransportGuidBit6, E::Flags2, E::Flags, E::HasFallDirection,
    E::GuidByte1, E::GuidByte0, E::GuidByte2, E::GuidByte3, E::GuidByte7, E::GuidByte6,
    E::GuidByte4, E::GuidByte5, E::TransportTime2, E::TransportGuidByte6, E::TransportTime,
    E::TransportVehicleId, E::TransportGuidByte7, E::TransportPositionZ, E::TransportGuidByte3,
    E::TransportPositionY, E::TransportGuidByte5, E::TransportPositionX, E::TransportGuidByte2,
    E::TransportPositionO, E::TransportSeat, E::TransportGuidByte1, E::TransportGuidByte0,
    E::TransportGuidByte4, E::FallTime, E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle,
    E::FallVerticalSpeed, E::Pitch, E::PositionO, E::Timestamp, E::SplineElevation, E::End)

LAYOUT(MoveForceSwimBackSpeedChangeAck,
    E::ExtraFloat, E::PositionX, E::PositionZ, E::PositionY, E::MovementCounter, E::GuidBit7,
    E::HasTransportData, E::GuidBit6, E::HasPitch, E::HasOrientation, E::GuidBit2,
    E::HasMovementFlags, E::GuidBit0, E::GuidBit5, E::GuidBit3, E::HasSpline, E::HasTimestamp,
    E::HasUnknownBit, E::HasMovementFlags2, E::GuidBit4, E::GuidBit1, E::HasSplineElevation,
    E::HasFallData, E::TransportGuidBit6, E::TransportGuidBit3, E::TransportGuidBit4,
    E::TransportGuidBit7, E::HasVehicleId, E::TransportGuidBit1, E::TransportGuidBit0,
    E::HasTransportTime2, E::TransportGuidBit5, E::TransportGuidBit2, E::Flags, E::Flags2,
    E::HasFallDirection, E::GuidByte0, E::GuidByte7, E::GuidByte4, E::GuidByte6, E::GuidByte1,
    E::GuidByte2, E::GuidByte5, E::GuidByte3, E::FallCosAngle, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallVerticalSpeed, E::FallTime, E::TransportGuidByte4,
    E::TransportGuidByte6, E::TransportGuidByte1, E::TransportPositionY, E::TransportPositionO,
    E::TransportPositionZ, E::TransportGuidByte7, E::TransportVehicleId, E::TransportTime,
    E::TransportGuidByte3, E::TransportGuidByte2, E::TransportSeat, E::TransportGuidByte0,
    E::TransportTime2, E::TransportPositionX, E::TransportGuidByte5, E::Timestamp,
    E::SplineElevation, E::Pitch, E::PositionO, E::End)

LAYOUT(MoveForceFlightBackSpeedChangeAck,
    E::PositionY, E::PositionZ, E::MovementCounter, E::PositionX, E::ExtraFloat,
    E::HasSplineElevation, E::HasPitch, E::HasFallData, E::HasSpline, E::GuidBit7, E::GuidBit4,
    E::HasUnknownBit, E::GuidBit1, E::HasOrientation, E::HasMovementFlags, E::GuidBit5, E::GuidBit3,
    E::HasTransportData, E::GuidBit6, E::HasMovementFlags2, E::GuidBit2, E::GuidBit0,
    E::HasTimestamp, E::TransportGuidBit2, E::TransportGuidBit5, E::TransportGuidBit1,
    E::HasVehicleId, E::TransportGuidBit0, E::TransportGuidBit7, E::TransportGuidBit6,
    E::TransportGuidBit3, E::HasTransportTime2, E::TransportGuidBit4, E::Flags2, E::Flags,
    E::HasFallDirection, E::GuidByte7, E::GuidByte0, E::GuidByte5, E::GuidByte6, E::GuidByte3,
    E::GuidByte2, E::GuidByte4, E::GuidByte1, E::TransportGuidByte2, E::TransportGuidByte0,
    E::TransportTime, E::TransportGuidByte3, E::TransportPositionO, E::TransportGuidByte1,
    E::TransportPositionX, E::TransportPositionZ, E::TransportGuidByte4, E::TransportGuidByte7,
    E::TransportVehicleId, E::TransportGuidByte5, E::TransportTime2, E::TransportPositionY,
    E::TransportSeat, E::TransportGuidByte6, E::FallSinAngle, E::FallCosAngle,
    E::FallHorizontalSpeed, E::FallVerticalSpeed, E::FallTime, E::Timestamp, E::Pitch, E::PositionO,
    E::SplineElevation, E::End)

LAYOUT(MovementForceSwimSpeedChangeAck,
    E::PositionX, E::MovementCounter, E::ExtraFloat, E::PositionY, E::PositionZ, E::GuidBit4,
    E::HasOrientation, E::HasPitch, E::HasTransportData, E::HasMovementFlags, E::HasMovementFlags2,
    E::GuidBit1, E::GuidBit3, E::HasFallData, E::GuidBit2, E::GuidBit7, E::HasTimestamp,
    E::HasUnknownBit, E::GuidBit5, E::GuidBit6, E::HasSplineElevation, E::HasSpline, E::GuidBit0,
    E::TransportGuidBit1, E::TransportGuidBit4, E::TransportGuidBit0, E::TransportGuidBit5,
    E::TransportGuidBit7, E::TransportGuidBit3, E::HasTransportTime2, E::HasVehicleId,
    E::TransportGuidBit6, E::TransportGuidBit2, E::Flags2, E::HasFallDirection, E::Flags,
    E::GuidByte2, E::GuidByte0, E::GuidByte6, E::GuidByte5, E::GuidByte1, E::GuidByte3,
    E::GuidByte4, E::GuidByte7, E::TransportGuidByte0, E::TransportVehicleId, E::TransportTime2,
    E::TransportPositionX, E::TransportGuidByte7, E::TransportSeat, E::TransportPositionY,
    E::TransportGuidByte5, E::TransportPositionZ, E::TransportPositionO, E::TransportTime,
    E::TransportGuidByte2, E::TransportGuidByte6, E::TransportGuidByte3, E::TransportGuidByte1,
    E::TransportGuidByte4, E::FallTime, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed,
    E::FallVerticalSpeed, E::PositionO, E::Pitch, E::SplineElevation, E::Timestamp, E::End)

LAYOUT(MovementForceWalkSpeedChangeAck,
    E::PositionZ, E::PositionY, E::ExtraFloat, E::PositionX, E::MovementCounter, E::GuidBit0,
    E::HasMovementFlags2, E::HasTimestamp, E::GuidBit4, E::GuidBit5, E::GuidBit2,
    E::HasMovementFlags, E::HasFallData, E::HasOrientation, E::HasSpline, E::HasUnknownBit,
    E::GuidBit3, E::GuidBit1, E::HasSplineElevation, E::HasTransportData, E::GuidBit7, E::GuidBit6,
    E::HasPitch, E::HasTransportTime2, E::TransportGuidBit5, E::TransportGuidBit1,
    E::TransportGuidBit6, E::HasVehicleId, E::TransportGuidBit2, E::TransportGuidBit0,
    E::TransportGuidBit4, E::TransportGuidBit7, E::TransportGuidBit3, E::Flags, E::Flags2,
    E::HasFallDirection, E::GuidByte5, E::GuidByte6, E::GuidByte7, E::GuidByte2, E::GuidByte1,
    E::GuidByte3, E::GuidByte4, E::GuidByte0, E::FallVerticalSpeed, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallCosAngle, E::FallTime, E::TransportPositionZ,
    E::TransportGuidByte7, E::TransportGuidByte5, E::TransportPositionX, E::TransportGuidByte0,
    E::TransportTime, E::TransportVehicleId, E::TransportPositionY, E::TransportGuidByte2,
    E::TransportGuidByte4, E::TransportGuidByte1, E::TransportGuidByte3, E::TransportPositionO,
    E::TransportSeat, E::TransportGuidByte6, E::TransportTime2, E::SplineElevation, E::Timestamp,
    E::PositionO, E::Pitch, E::End)

LAYOUT(MovementForceRunBackSpeedChangeAck,
    E::ExtraFloat, E::MovementCounter, E::PositionX, E::PositionZ, E::PositionY, E::GuidBit2,
    E::GuidBit4, E::HasTimestamp, E::HasTransportData, E::GuidBit1, E::GuidBit0, E::GuidBit5,
    E::GuidBit3, E::HasOrientation, E::HasFallData, E::HasMovementFlags2, E::HasMovementFlags,
    E::GuidBit7, E::HasSpline, E::HasPitch, E::GuidBit6, E::HasSplineElevation, E::HasUnknownBit,
    E::TransportGuidBit3, E::HasTransportTime2, E::TransportGuidBit7, E::TransportGuidBit0,
    E::TransportGuidBit2, E::TransportGuidBit1, E::HasVehicleId, E::TransportGuidBit4,
    E::TransportGuidBit6, E::TransportGuidBit5, E::Flags, E::Flags2, E::HasFallDirection,
    E::GuidByte0, E::GuidByte7, E::GuidByte2, E::GuidByte4, E::GuidByte3, E::GuidByte6,
    E::GuidByte5, E::GuidByte1, E::FallTime, E::FallHorizontalSpeed, E::FallCosAngle,
    E::FallSinAngle, E::FallVerticalSpeed, E::TransportGuidByte5, E::TransportSeat,
    E::TransportGuidByte6, E::TransportGuidByte1, E::TransportGuidByte0, E::TransportGuidByte2,
    E::TransportPositionY, E::TransportVehicleId, E::TransportPositionX, E::TransportPositionO,
    E::TransportGuidByte3, E::TransportTime, E::TransportTime2, E::TransportGuidByte4,
    E::TransportPositionZ, E::TransportGuidByte7, E::Timestamp, E::SplineElevation, E::PositionO,
    E::Pitch, E::End)

LAYOUT(ForceMoveRootAck,
    E::PositionY, E::PositionZ, E::MovementCounter, E::PositionX, E::GuidBit3, E::HasTimestamp,
    E::HasTransportData, E::HasPitch, E::GuidBit2, E::HasOrientation, E::HasSplineElevation,
    E::HasFallData, E::HasSpline, E::GuidBit4, E::GuidBit0, E::GuidBit6, E::GuidBit1, E::GuidBit7,
    E::HasMovementFlags2, E::HasMovementFlags, E::HasUnknownBit, E::GuidBit5, E::TransportGuidBit6,
    E::TransportGuidBit0, E::TransportGuidBit7, E::TransportGuidBit3, E::TransportGuidBit1,
    E::TransportGuidBit2, E::HasTransportTime2, E::TransportGuidBit4, E::HasVehicleId,
    E::TransportGuidBit5, E::Flags2, E::Flags, E::HasFallDirection, E::GuidByte5, E::GuidByte3,
    E::GuidByte1, E::GuidByte7, E::GuidByte4, E::GuidByte0, E::GuidByte6, E::GuidByte2,
    E::TransportGuidByte5, E::TransportGuidByte4, E::TransportPositionO, E::TransportPositionZ,
    E::TransportVehicleId, E::TransportTime2, E::TransportGuidByte7, E::TransportGuidByte2,
    E::TransportGuidByte0, E::TransportTime, E::TransportGuidByte6, E::TransportGuidByte3,
    E::TransportPositionX, E::TransportGuidByte1, E::TransportPositionY, E::TransportSeat,
    E::Timestamp, E::FallVerticalSpeed, E::FallHorizontalSpeed, E::FallSinAngle, E::FallCosAngle,
    E::FallTime, E::PositionO, E::SplineElevation, E::Pitch, E::End)

LAYOUT(ForceMoveUnrootAck,
    E::MovementCounter, E::PositionZ, E::PositionY, E::PositionX, E::GuidBit7,
    E::HasSplineElevation, E::HasSpline, E::GuidBit5, E::GuidBit4, E::HasUnknownBit,
    E::HasTimestamp, E::HasMovementFlags, E::HasOrientation, E::HasPitch, E::GuidBit3, E::GuidBit1,
    E::HasTransportData, E::HasMovementFlags2, E::GuidBit2, E::GuidBit0, E::HasFallData,
    E::GuidBit6, E::HasFallDirection, E::TransportGuidBit0, E::HasVehicleId, E::TransportGuidBit6,
    E::HasTransportTime2, E::TransportGuidBit3, E::TransportGuidBit5, E::TransportGuidBit7,
    E::TransportGuidBit2, E::TransportGuidBit4, E::TransportGuidBit1, E::Flags2, E::Flags,
    E::GuidByte7, E::GuidByte1, E::GuidByte0, E::GuidByte6, E::GuidByte2, E::GuidByte4,
    E::GuidByte5, E::GuidByte3, E::TransportGuidByte6, E::TransportPositionZ, E::TransportGuidByte2,
    E::TransportGuidByte3, E::TransportPositionY, E::TransportVehicleId, E::TransportGuidByte5,
    E::TransportSeat, E::TransportGuidByte0, E::TransportTime, E::TransportTime2,
    E::TransportGuidByte4, E::TransportPositionO, E::TransportPositionX, E::TransportGuidByte1,
    E::TransportGuidByte7, E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle,
    E::FallVerticalSpeed, E::FallTime, E::Timestamp, E::SplineElevation, E::PositionO, E::Pitch,
    E::End)

LAYOUT(MoveUpdateSwimBackSpeed,
    E::GuidBit7, E::GuidBit2, E::HasTransportData, E::GuidBit1, E::TransportGuidBit2,
    E::TransportGuidBit1, E::TransportGuidBit0, E::TransportGuidBit7, E::TransportGuidBit3,
    E::TransportGuidBit4, E::TransportGuidBit6, E::HasTransportTime2, E::TransportGuidBit5,
    E::HasVehicleId, E::HasPitch, E::HasOrientation, E::HasUnknownBit, E::GuidBit5, E::GuidBit3,
    E::GuidBit6, E::GuidBit4, E::HasSpline, E::HasMovementFlags, E::GuidBit0, E::HasFallData,
    E::HasFallDirection, E::Flags, E::HasTimestamp, E::HasSplineElevation, E::HasMovementFlags2,
    E::Flags2, E::FlushBits, E::GuidByte0, E::GuidByte4, E::PositionY, E::TransportGuidByte7,
    E::TransportVehicleId, E::TransportGuidByte6, E::TransportGuidByte4, E::TransportGuidByte2,
    E::TransportGuidByte3, E::TransportPositionO, E::TransportGuidByte5, E::TransportTime,
    E::TransportGuidByte0, E::TransportGuidByte1, E::TransportPositionX, E::TransportPositionZ,
    E::TransportTime2, E::TransportPositionY, E::TransportSeat, E::FallCosAngle, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallVerticalSpeed, E::FallTime, E::Pitch, E::GuidByte2, E::GuidByte7,
    E::ExtraFloat, E::SplineElevation, E::PositionZ, E::GuidByte3, E::PositionX, E::Timestamp,
    E::GuidByte5, E::PositionO, E::GuidByte6, E::GuidByte1, E::End)

LAYOUT(MovementFallReset,
    E::PositionZ, E::PositionX, E::PositionY, E::GuidBit1, E::HasTimestamp, E::HasMovementFlags,
    E::HasUnknownBit, E::GuidBit6, E::HasSplineElevation, E::HasPitch, E::GuidBit3,
    E::HasTransportData, E::GuidBit2, E::HasMovementFlags2, E::HasOrientation, E::GuidBit4,
    E::GuidBit5, E::HasSpline, E::GuidBit7, E::GuidBit0, E::HasFallData, E::TransportGuidBit0,
    E::TransportGuidBit1, E::TransportGuidBit5, E::TransportGuidBit4, E::HasTransportTime2,
    E::TransportGuidBit2, E::HasVehicleId, E::TransportGuidBit6, E::TransportGuidBit7,
    E::TransportGuidBit3, E::HasFallDirection, E::Flags2, E::Flags, E::GuidByte4, E::GuidByte0,
    E::GuidByte1, E::GuidByte7, E::GuidByte5, E::GuidByte2, E::GuidByte3, E::GuidByte6,
    E::TransportPositionX, E::TransportGuidByte7, E::TransportGuidByte2, E::TransportGuidByte3,
    E::TransportGuidByte5, E::TransportPositionY, E::TransportPositionZ, E::TransportSeat,
    E::TransportTime2, E::TransportGuidByte6, E::TransportVehicleId, E::TransportTime,
    E::TransportGuidByte0, E::TransportPositionO, E::TransportGuidByte4, E::TransportGuidByte1,
    E::FallVerticalSpeed, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed, E::FallTime,
    E::PositionO, E::Timestamp, E::SplineElevation, E::Pitch, E::End)

LAYOUT(MovementFeatherFallAck,
    E::PositionZ, E::MovementCounter, E::PositionY, E::PositionX, E::HasUnknownBit, E::GuidBit3,
    E::HasSplineElevation, E::GuidBit1, E::HasPitch, E::GuidBit2, E::HasSpline, E::HasTimestamp,
    E::GuidBit5, E::HasFallData, E::HasOrientation, E::HasTransportData, E::GuidBit7,
    E::HasMovementFlags2, E::GuidBit6, E::HasMovementFlags, E::GuidBit0, E::GuidBit4,
    E::TransportGuidBit6, E::HasVehicleId, E::TransportGuidBit2, E::TransportGuidBit4,
    E::TransportGuidBit7, E::TransportGuidBit3, E::TransportGuidBit5, E::HasTransportTime2,
    E::TransportGuidBit1, E::TransportGuidBit0, E::HasFallDirection, E::Flags2, E::Flags,
    E::GuidByte6, E::GuidByte1, E::GuidByte7, E::GuidByte0, E::GuidByte5, E::GuidByte4,
    E::GuidByte3, E::GuidByte2, E::TransportVehicleId, E::TransportPositionO, E::TransportPositionZ,
    E::TransportGuidByte7, E::TransportGuidByte5, E::TransportGuidByte2, E::TransportPositionX,
    E::TransportGuidByte1, E::TransportGuidByte6, E::TransportTime2, E::TransportTime,
    E::TransportPositionY, E::TransportGuidByte4, E::TransportGuidByte0, E::TransportSeat,
    E::TransportGuidByte3, E::Timestamp, E::SplineElevation, E::PositionO, E::FallVerticalSpeed,
    E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle, E::FallTime, E::Pitch, E::End)

LAYOUT(MovementGravityDisableAck,
    E::PositionZ, E::PositionY, E::MovementCounter, E::PositionX, E::GuidBit3, E::HasTransportData,
    E::GuidBit4, E::GuidBit5, E::HasMovementFlags, E::HasOrientation, E::HasSpline, E::GuidBit6,
    E::GuidBit0, E::GuidBit7, E::HasSplineElevation, E::HasTimestamp, E::HasPitch,
    E::HasMovementFlags2, E::HasUnknownBit, E::GuidBit2, E::HasFallData, E::GuidBit1,
    E::TransportGuidBit2, E::HasVehicleId, E::TransportGuidBit0, E::TransportGuidBit1,
    E::TransportGuidBit5, E::TransportGuidBit3, E::TransportGuidBit7, E::TransportGuidBit6,
    E::TransportGuidBit4, E::HasTransportTime2, E::Flags, E::Flags2, E::HasFallDirection,
    E::GuidByte0, E::GuidByte2, E::GuidByte1, E::GuidByte3, E::GuidByte5, E::GuidByte7,
    E::GuidByte4, E::GuidByte6, E::TransportPositionZ, E::TransportGuidByte2, E::TransportGuidByte5,
    E::TransportGuidByte3, E::TransportGuidByte4, E::TransportPositionY, E::TransportTime,
    E::TransportSeat, E::TransportGuidByte6, E::TransportGuidByte1, E::TransportGuidByte0,
    E::TransportPositionO, E::TransportTime2, E::TransportPositionX, E::TransportGuidByte7,
    E::TransportVehicleId, E::FallCosAngle, E::FallSinAngle, E::FallHorizontalSpeed, E::FallTime,
    E::FallVerticalSpeed, E::PositionO, E::SplineElevation, E::Timestamp, E::Pitch, E::End)

LAYOUT(MovementGravityEnableAck,
    E::PositionZ, E::PositionX, E::MovementCounter, E::PositionY, E::HasSplineElevation,
    E::HasMovementFlags2, E::GuidBit6, E::HasOrientation, E::HasUnknownBit, E::GuidBit5,
    E::HasSpline, E::GuidBit3, E::HasTransportData, E::HasPitch, E::GuidBit1, E::HasTimestamp,
    E::GuidBit2, E::GuidBit4, E::GuidBit7, E::HasMovementFlags, E::HasFallData, E::GuidBit0,
    E::HasVehicleId, E::TransportGuidBit0, E::TransportGuidBit1, E::TransportGuidBit3,
    E::TransportGuidBit7, E::TransportGuidBit2, E::TransportGuidBit5, E::HasTransportTime2,
    E::TransportGuidBit4, E::TransportGuidBit6, E::HasFallDirection, E::Flags, E::Flags2,
    E::GuidByte5, E::GuidByte4, E::GuidByte1, E::GuidByte7, E::GuidByte0, E::GuidByte2,
    E::GuidByte3, E::GuidByte6, E::FallTime, E::FallHorizontalSpeed, E::FallSinAngle,
    E::FallCosAngle, E::FallVerticalSpeed, E::TransportGuidByte1, E::TransportPositionX,
    E::TransportVehicleId, E::TransportPositionO, E::TransportGuidByte2, E::TransportGuidByte0,
    E::TransportGuidByte7, E::TransportGuidByte6, E::TransportSeat, E::TransportGuidByte3,
    E::TransportGuidByte4, E::TransportPositionZ, E::TransportTime2, E::TransportGuidByte5,
    E::TransportTime, E::TransportPositionY, E::SplineElevation, E::PositionO, E::Pitch,
    E::Timestamp, E::End)

LAYOUT(MovementHoverAck,
    E::MovementCounter, E::PositionZ, E::PositionY, E::PositionX, E::GuidBit4, E::HasTransportData,
    E::GuidBit2, E::HasTimestamp, E::HasSpline, E::HasMovementFlags, E::GuidBit1, E::HasPitch,
    E::GuidBit0, E::GuidBit6, E::GuidBit7, E::GuidBit5, E::HasUnknownBit, E::HasFallData,
    E::HasMovementFlags2, E::HasSplineElevation, E::HasOrientation, E::GuidBit3,
    E::TransportGuidBit4, E::TransportGuidBit6, E::HasVehicleId, E::TransportGuidBit1,
    E::TransportGuidBit0, E::TransportGuidBit3, E::TransportGuidBit2, E::TransportGuidBit5,
    E::TransportGuidBit7, E::HasTransportTime2, E::Flags, E::Flags2, E::HasFallDirection,
    E::GuidByte1, E::GuidByte4, E::GuidByte7, E::GuidByte2, E::GuidByte5, E::GuidByte6,
    E::GuidByte3, E::GuidByte0, E::Timestamp, E::FallCosAngle, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallTime, E::FallVerticalSpeed, E::PositionO, E::TransportGuidByte1,
    E::TransportVehicleId, E::TransportGuidByte2, E::TransportGuidByte7, E::TransportGuidByte3,
    E::TransportGuidByte4, E::TransportSeat, E::TransportPositionX, E::TransportPositionZ,
    E::TransportPositionO, E::TransportTime, E::TransportGuidByte0, E::TransportPositionY,
    E::TransportGuidByte5, E::TransportTime2, E::TransportGuidByte6, E::SplineElevation, E::Pitch,
    E::End)

LAYOUT(MovementKnockBackAck,
    E::PositionY, E::PositionZ, E::MovementCounter, E::PositionX, E::GuidBit6, E::HasOrientation,
    E::HasPitch, E::HasSplineElevation, E::GuidBit3, E::GuidBit4, E::GuidBit1, E::GuidBit2,
    E::HasSpline, E::GuidBit7, E::HasUnknownBit, E::HasMovementFlags2, E::HasTimestamp, E::GuidBit0,
    E::HasMovementFlags, E::HasTransportData, E::GuidBit5, E::HasFallData, E::Flags,
    E::TransportGuidBit4, E::TransportGuidBit1, E::HasTransportTime2, E::TransportGuidBit0,
    E::HasVehicleId, E::TransportGuidBit6, E::TransportGuidBit2, E::TransportGuidBit3,
    E::TransportGuidBit5, E::TransportGuidBit7, E::Flags2, E::HasFallDirection, E::GuidByte4,
    E::GuidByte5, E::GuidByte1, E::GuidByte6, E::GuidByte0, E::GuidByte3, E::GuidByte2,
    E::GuidByte7, E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle, E::FallTime,
    E::FallVerticalSpeed, E::Timestamp, E::TransportPositionY, E::TransportGuidByte4,
    E::TransportTime2, E::TransportGuidByte7, E::TransportPositionO, E::TransportGuidByte6,
    E::TransportTime, E::TransportGuidByte3, E::TransportGuidByte1, E::TransportVehicleId,
    E::TransportGuidByte2, E::TransportPositionZ, E::TransportGuidByte0, E::TransportGuidByte5,
    E::TransportPositionX, E::TransportSeat, E::Pitch, E::SplineElevation, E::PositionO, E::End)

LAYOUT(MovementWaterWalkAck,
    E::PositionY, E::PositionZ, E::MovementCounter, E::PositionX, E::HasTimestamp, E::HasPitch,
    E::GuidBit5, E::GuidBit0, E::GuidBit7, E::HasOrientation, E::GuidBit1, E::HasMovementFlags2,
    E::GuidBit2, E::HasMovementFlags, E::GuidBit3, E::HasTransportData, E::GuidBit6, E::HasFallData,
    E::GuidBit4, E::HasUnknownBit, E::HasSplineElevation, E::HasSpline, E::TransportGuidBit0,
    E::TransportGuidBit1, E::TransportGuidBit6, E::TransportGuidBit2, E::TransportGuidBit7,
    E::TransportGuidBit5, E::HasTransportTime2, E::TransportGuidBit3, E::HasVehicleId,
    E::TransportGuidBit4, E::Flags, E::HasFallDirection, E::Flags2, E::GuidByte2, E::GuidByte7,
    E::GuidByte3, E::GuidByte5, E::GuidByte6, E::GuidByte0, E::GuidByte4, E::GuidByte1,
    E::TransportPositionX, E::TransportGuidByte1, E::TransportVehicleId, E::TransportGuidByte0,
    E::TransportPositionO, E::TransportGuidByte7, E::TransportPositionY, E::TransportTime2,
    E::TransportTime, E::TransportGuidByte5, E::TransportSeat, E::TransportPositionZ,
    E::TransportGuidByte3, E::TransportGuidByte2, E::TransportGuidByte6, E::TransportGuidByte4,
    E::SplineElevation, E::FallSinAngle, E::FallHorizontalSpeed, E::FallCosAngle,
    E::FallVerticalSpeed, E::FallTime, E::PositionO, E::Timestamp, E::Pitch, E::End)

LAYOUT(SplineMoveSetWalkSpeed,
    E::GuidBit0, E::GuidBit6, E::GuidBit7, E::GuidBit3, E::GuidBit5, E::GuidBit1, E::GuidBit2,
    E::GuidBit4, E::GuidByte0, E::GuidByte4, E::GuidByte7, E::GuidByte1, E::GuidByte5, E::GuidByte3,
    E::ExtraFloat, E::GuidByte6, E::GuidByte2, E::End)

LAYOUT(SplineMoveSetRunSpeed,
    E::GuidBit4, E::GuidBit0, E::GuidBit5, E::GuidBit7, E::GuidBit6, E::GuidBit3, E::GuidBit1,
    E::GuidBit2, E::GuidByte0, E::GuidByte7, E::GuidByte6, E::GuidByte5, E::GuidByte3, E::GuidByte4,
    E::ExtraFloat, E::GuidByte2, E::GuidByte1, E::End)

LAYOUT(SplineMoveSetRunBackSpeed,
    E::GuidBit1, E::GuidBit2, E::GuidBit6, E::GuidBit0, E::GuidBit3, E::GuidBit7, E::GuidBit5,
    E::GuidBit4, E::GuidByte1, E::ExtraFloat, E::GuidByte2, E::GuidByte4, E::GuidByte0,
    E::GuidByte3, E::GuidByte6, E::GuidByte5, E::GuidByte7, E::End)

LAYOUT(SplineMoveSetSwimSpeed,
    E::GuidBit4, E::GuidBit2, E::GuidBit5, E::GuidBit0, E::GuidBit7, E::GuidBit6, E::GuidBit3,
    E::GuidBit1, E::GuidByte5, E::GuidByte6, E::GuidByte1, E::GuidByte0, E::GuidByte2, E::GuidByte4,
    E::ExtraFloat, E::GuidByte7, E::GuidByte3, E::End)

LAYOUT(SplineMoveSetSwimBackSpeed,
    E::GuidBit0, E::GuidBit1, E::GuidBit3, E::GuidBit6, E::GuidBit4, E::GuidBit5, E::GuidBit7,
    E::GuidBit2, E::GuidByte5, E::GuidByte3, E::GuidByte1, E::GuidByte0, E::GuidByte7, E::GuidByte6,
    E::ExtraFloat, E::GuidByte4, E::GuidByte2, E::End)

LAYOUT(SplineMoveSetTurnRate,
    E::GuidBit2, E::GuidBit4, E::GuidBit6, E::GuidBit1, E::GuidBit3, E::GuidBit5, E::GuidBit7,
    E::GuidBit0, E::ExtraFloat, E::GuidByte1, E::GuidByte5, E::GuidByte3, E::GuidByte2,
    E::GuidByte7, E::GuidByte4, E::GuidByte6, E::GuidByte0, E::End)

LAYOUT(SplineMoveSetFlightSpeed,
    E::GuidBit7, E::GuidBit4, E::GuidBit0, E::GuidBit1, E::GuidBit3, E::GuidBit6, E::GuidBit5,
    E::GuidBit2, E::GuidByte0, E::GuidByte5, E::GuidByte4, E::GuidByte7, E::GuidByte3, E::GuidByte2,
    E::GuidByte1, E::GuidByte6, E::ExtraFloat, E::End)

LAYOUT(SplineMoveSetFlightBackSpeed,
    E::GuidBit2, E::GuidBit1, E::GuidBit6, E::GuidBit5, E::GuidBit0, E::GuidBit3, E::GuidBit4,
    E::GuidBit7, E::GuidByte5, E::ExtraFloat, E::GuidByte6, E::GuidByte1, E::GuidByte0,
    E::GuidByte2, E::GuidByte3, E::GuidByte7, E::GuidByte4, E::End)

LAYOUT(SplineMoveSetPitchRate,
    E::GuidBit3, E::GuidBit5, E::GuidBit6, E::GuidBit1, E::GuidBit0, E::GuidBit4, E::GuidBit7,
    E::GuidBit2, E::GuidByte1, E::GuidByte5, E::GuidByte7, E::GuidByte0, E::GuidByte6, E::GuidByte3,
    E::GuidByte2, E::ExtraFloat, E::GuidByte4, E::End)

LAYOUT(MoveSetWalkSpeed,
    E::GuidBit0, E::GuidBit4, E::GuidBit5, E::GuidBit2, E::GuidBit3, E::GuidBit1, E::GuidBit6,
    E::GuidBit7, E::GuidByte6, E::GuidByte1, E::GuidByte5, E::ExtraFloat, E::GuidByte2,
    E::MovementCounter, E::GuidByte4, E::GuidByte0, E::GuidByte7, E::GuidByte3, E::End)

LAYOUT(MoveSetRunSpeed,
    E::GuidBit6, E::GuidBit1, E::GuidBit5, E::GuidBit2, E::GuidBit7, E::GuidBit0, E::GuidBit3,
    E::GuidBit4, E::GuidByte5, E::GuidByte3, E::GuidByte1, E::GuidByte4, E::MovementCounter,
    E::ExtraFloat, E::GuidByte6, E::GuidByte0, E::GuidByte7, E::GuidByte2, E::End)

LAYOUT(MoveSetRunBackSpeed,
    E::GuidBit0, E::GuidBit6, E::GuidBit2, E::GuidBit1, E::GuidBit3, E::GuidBit4, E::GuidBit5,
    E::GuidBit7, E::GuidByte5, E::MovementCounter, E::ExtraFloat, E::GuidByte0, E::GuidByte4,
    E::GuidByte7, E::GuidByte3, E::GuidByte1, E::GuidByte2, E::GuidByte6, E::End)

LAYOUT(MoveSetSwimSpeed,
    E::GuidBit5, E::GuidBit4, E::GuidBit7, E::GuidBit3, E::GuidBit2, E::GuidBit0, E::GuidBit1,
    E::GuidBit6, E::GuidByte0, E::MovementCounter, E::GuidByte6, E::GuidByte3, E::GuidByte5,
    E::GuidByte2, E::ExtraFloat, E::GuidByte1, E::GuidByte7, E::GuidByte4, E::End)

LAYOUT(MoveSetSwimBackSpeed,
    E::GuidBit4, E::GuidBit2, E::GuidBit3, E::GuidBit6, E::GuidBit5, E::GuidBit1, E::GuidBit0,
    E::GuidBit7, E::MovementCounter, E::GuidByte0, E::GuidByte3, E::GuidByte4, E::GuidByte6,
    E::GuidByte5, E::GuidByte1, E::ExtraFloat, E::GuidByte7, E::GuidByte2, E::End)

LAYOUT(MoveSetTurnRate,
    E::GuidBit7, E::GuidBit2, E::GuidBit1, E::GuidBit0, E::GuidBit4, E::GuidBit5, E::GuidBit6,
    E::GuidBit3, E::GuidByte5, E::GuidByte7, E::GuidByte2, E::ExtraFloat, E::GuidByte3,
    E::GuidByte1, E::GuidByte0, E::MovementCounter, E::GuidByte6, E::GuidByte4, E::End)

LAYOUT(MoveSetFlightSpeed,
    E::GuidBit0, E::GuidBit5, E::GuidBit1, E::GuidBit6, E::GuidBit3, E::GuidBit2, E::GuidBit7,
    E::GuidBit4, E::GuidByte0, E::GuidByte1, E::GuidByte7, E::GuidByte5, E::ExtraFloat,
    E::MovementCounter, E::GuidByte2, E::GuidByte6, E::GuidByte3, E::GuidByte4, E::End)

LAYOUT(MoveSetFlightBackSpeed,
    E::GuidBit1, E::GuidBit2, E::GuidBit6, E::GuidBit4, E::GuidBit7, E::GuidBit3, E::GuidBit0,
    E::GuidBit5, E::GuidByte3, E::MovementCounter, E::GuidByte6, E::ExtraFloat, E::GuidByte1,
    E::GuidByte2, E::GuidByte4, E::GuidByte0, E::GuidByte5, E::GuidByte7, E::End)

LAYOUT(MoveSetPitchRate,
    E::GuidBit1, E::GuidBit2, E::GuidBit6, E::GuidBit7, E::GuidBit0, E::GuidBit3, E::GuidBit5,
    E::GuidBit4, E::ExtraFloat, E::GuidByte6, E::GuidByte4, E::GuidByte0, E::MovementCounter,
    E::GuidByte1, E::GuidByte2, E::GuidByte7, E::GuidByte3, E::GuidByte5, E::End)

LAYOUT(SplineMoveSetWalkMode,
    E::GuidBit7, E::GuidBit6, E::GuidBit5, E::GuidBit1, E::GuidBit3, E::GuidBit4, E::GuidBit2,
    E::GuidBit0, E::GuidByte4, E::GuidByte2, E::GuidByte1, E::GuidByte6, E::GuidByte5, E::GuidByte0,
    E::GuidByte7, E::GuidByte3, E::End)

LAYOUT(SplineMoveSetRunMode,
    E::GuidBit5, E::GuidBit6, E::GuidBit3, E::GuidBit7, E::GuidBit2, E::GuidBit0, E::GuidBit4,
    E::GuidBit1, E::GuidByte7, E::GuidByte0, E::GuidByte4, E::GuidByte6, E::GuidByte5, E::GuidByte1,
    E::GuidByte2, E::GuidByte3, E::End)

LAYOUT(SplineMoveGravityDisable,
    E::GuidBit7, E::GuidBit3, E::GuidBit4, E::GuidBit2, E::GuidBit5, E::GuidBit1, E::GuidBit0,
    E::GuidBit6, E::GuidByte7, E::GuidByte1, E::GuidByte3, E::GuidByte4, E::GuidByte6, E::GuidByte2,
    E::GuidByte5, E::GuidByte0, E::End)

LAYOUT(SplineMoveGravityEnable,
    E::GuidBit5, E::GuidBit4, E::GuidBit7, E::GuidBit1, E::GuidBit3, E::GuidBit6, E::GuidBit2,
    E::GuidBit0, E::GuidByte7, E::GuidByte3, E::GuidByte4, E::GuidByte2, E::GuidByte1, E::GuidByte6,
    E::GuidByte0, E::GuidByte5, E::End)

LAYOUT(SplineMoveSetHover,
    E::GuidBit3, E::GuidBit7, E::GuidBit0, E::GuidBit1, E::GuidBit4, E::GuidBit6, E::GuidBit2,
    E::GuidBit5, E::GuidByte2, E::GuidByte4, E::GuidByte3, E::GuidByte1, E::GuidByte7, E::GuidByte0,
    E::GuidByte5, E::GuidByte6, E::End)

LAYOUT(SplineMoveUnsetHover,
    E::GuidBit6, E::GuidBit7, E::GuidBit4, E::GuidBit0, E::GuidBit3, E::GuidBit1, E::GuidBit5,
    E::GuidBit2, E::GuidByte4, E::GuidByte5, E::GuidByte3, E::GuidByte0, E::GuidByte2, E::GuidByte7,
    E::GuidByte6, E::GuidByte1, E::End)

LAYOUT(SplineMoveStartSwim,
    E::GuidBit1, E::GuidBit6, E::GuidBit0, E::GuidBit7, E::GuidBit3, E::GuidBit5, E::GuidBit2,
    E::GuidBit4, E::GuidByte3, E::GuidByte7, E::GuidByte2, E::GuidByte5, E::GuidByte6, E::GuidByte4,
    E::GuidByte1, E::GuidByte0, E::End)

LAYOUT(SplineMoveStopSwim,
    E::GuidBit4, E::GuidBit1, E::GuidBit5, E::GuidBit3, E::GuidBit0, E::GuidBit7, E::GuidBit2,
    E::GuidBit6, E::GuidByte6, E::GuidByte0, E::GuidByte7, E::GuidByte2, E::GuidByte3, E::GuidByte1,
    E::GuidByte5, E::GuidByte4, E::End)

LAYOUT(SplineMoveSetFlying,
    E::GuidBit0, E::GuidBit4, E::GuidBit1, E::GuidBit6, E::GuidBit7, E::GuidBit2, E::GuidBit3,
    E::GuidBit5, E::GuidByte7, E::GuidByte0, E::GuidByte5, E::GuidByte6, E::GuidByte4, E::GuidByte1,
    E::GuidByte3, E::GuidByte2, E::End)

LAYOUT(SplineMoveUnsetFlying,
    E::GuidBit5, E::GuidBit0, E::GuidBit4, E::GuidBit7, E::GuidBit2, E::GuidBit3, E::GuidBit1,
    E::GuidBit6, E::GuidByte7, E::GuidByte2, E::GuidByte3, E::GuidByte4, E::GuidByte5, E::GuidByte1,
    E::GuidByte6, E::GuidByte0, E::End)

LAYOUT(SplineMoveSetWaterWalk,
    E::GuidBit6, E::GuidBit1, E::GuidBit4, E::GuidBit2, E::GuidBit3, E::GuidBit7, E::GuidBit5,
    E::GuidBit0, E::GuidByte0, E::GuidByte6, E::GuidByte3, E::GuidByte7, E::GuidByte4, E::GuidByte2,
    E::GuidByte5, E::GuidByte1, E::End)

LAYOUT(SplineMoveSetLandWalk,
    E::GuidBit5, E::GuidBit0, E::GuidBit4, E::GuidBit6, E::GuidBit7, E::GuidBit2, E::GuidBit3,
    E::GuidBit1, E::GuidByte5, E::GuidByte7, E::GuidByte3, E::GuidByte4, E::GuidByte1, E::GuidByte2,
    E::GuidByte0, E::GuidByte6, E::End)

LAYOUT(SplineMoveSetFeatherFall,
    E::GuidBit3, E::GuidBit2, E::GuidBit7, E::GuidBit5, E::GuidBit4, E::GuidBit6, E::GuidBit1,
    E::GuidBit0, E::GuidByte1, E::GuidByte4, E::GuidByte7, E::GuidByte6, E::GuidByte2, E::GuidByte0,
    E::GuidByte5, E::GuidByte3, E::End)

LAYOUT(SplineMoveSetNormalFall,
    E::GuidBit3, E::GuidBit5, E::GuidBit1, E::GuidBit0, E::GuidBit7, E::GuidBit6, E::GuidBit2,
    E::GuidBit4, E::GuidByte7, E::GuidByte6, E::GuidByte2, E::GuidByte0, E::GuidByte5, E::GuidByte4,
    E::GuidByte3, E::GuidByte1, E::End)

LAYOUT(SplineMoveRoot,
    E::GuidBit5, E::GuidBit4, E::GuidBit6, E::GuidBit1, E::GuidBit3, E::GuidBit7, E::GuidBit2,
    E::GuidBit0, E::GuidByte2, E::GuidByte1, E::GuidByte7, E::GuidByte3, E::GuidByte5, E::GuidByte0,
    E::GuidByte6, E::GuidByte4, E::End)

LAYOUT(SplineMoveUnroot,
    E::GuidBit0, E::GuidBit1, E::GuidBit6, E::GuidBit5, E::GuidBit3, E::GuidBit2, E::GuidBit7,
    E::GuidBit4, E::GuidByte6, E::GuidByte3, E::GuidByte1, E::GuidByte5, E::GuidByte2, E::GuidByte0,
    E::GuidByte7, E::GuidByte4, E::End)

LAYOUT(MoveSetCanFly,
    E::GuidBit1, E::GuidBit6, E::GuidBit5, E::GuidBit0, E::GuidBit7, E::GuidBit4, E::GuidBit2,
    E::GuidBit3, E::GuidByte6, E::GuidByte3, E::MovementCounter, E::GuidByte2, E::GuidByte1,
    E::GuidByte4, E::GuidByte7, E::GuidByte0, E::GuidByte5, E::End)

LAYOUT(MoveUnsetCanFly,
    E::GuidBit1, E::GuidBit4, E::GuidBit2, E::GuidBit5, E::GuidBit0, E::GuidBit3, E::GuidBit6,
    E::GuidBit7, E::GuidByte4, E::GuidByte6, E::MovementCounter, E::GuidByte1, E::GuidByte0,
    E::GuidByte2, E::GuidByte3, E::GuidByte5, E::GuidByte7, E::End)

LAYOUT(MoveSetHover,
    E::GuidBit1, E::GuidBit4, E::GuidBit2, E::GuidBit3, E::GuidBit0, E::GuidBit5, E::GuidBit6,
    E::GuidBit7, E::GuidByte5, E::GuidByte4, E::GuidByte1, E::GuidByte2, E::GuidByte3, E::GuidByte6,
    E::GuidByte0, E::GuidByte7, E::MovementCounter, E::End)

LAYOUT(MoveUnsetHover,
    E::GuidBit4, E::GuidBit6, E::GuidBit3, E::GuidBit1, E::GuidBit2, E::GuidBit7, E::GuidBit5,
    E::GuidBit0, E::GuidByte4, E::GuidByte5, E::GuidByte3, E::GuidByte6, E::GuidByte7, E::GuidByte1,
    E::GuidByte2, E::GuidByte0, E::MovementCounter, E::End)

LAYOUT(MoveWaterWalk,
    E::GuidBit4, E::GuidBit7, E::GuidBit6, E::GuidBit0, E::GuidBit1, E::GuidBit3, E::GuidBit5,
    E::GuidBit2, E::GuidByte0, E::GuidByte5, E::GuidByte2, E::MovementCounter, E::GuidByte7,
    E::GuidByte3, E::GuidByte4, E::GuidByte1, E::GuidByte6, E::End)

LAYOUT(MoveLandWalk,
    E::GuidBit5, E::GuidBit1, E::GuidBit6, E::GuidBit2, E::GuidBit3, E::GuidBit4, E::GuidBit0,
    E::GuidBit7, E::GuidByte6, E::GuidByte1, E::GuidByte7, E::GuidByte5, E::GuidByte4, E::GuidByte0,
    E::GuidByte3, E::GuidByte2, E::MovementCounter, E::End)

LAYOUT(MoveFeatherFall,
    E::GuidBit3, E::GuidBit1, E::GuidBit7, E::GuidBit0, E::GuidBit4, E::GuidBit2, E::GuidBit5,
    E::GuidBit6, E::GuidByte5, E::GuidByte7, E::GuidByte2, E::MovementCounter, E::GuidByte0,
    E::GuidByte3, E::GuidByte4, E::GuidByte1, E::GuidByte6, E::End)

LAYOUT(MoveNormalFall,
    E::MovementCounter, E::GuidBit3, E::GuidBit0, E::GuidBit1, E::GuidBit5, E::GuidBit7,
    E::GuidBit4, E::GuidBit6, E::GuidBit2, E::GuidByte2, E::GuidByte7, E::GuidByte1, E::GuidByte4,
    E::GuidByte5, E::GuidByte0, E::GuidByte3, E::GuidByte6, E::End)

LAYOUT(MoveRoot,
    E::GuidBit2, E::GuidBit7, E::GuidBit6, E::GuidBit0, E::GuidBit5, E::GuidBit4, E::GuidBit1,
    E::GuidBit3, E::GuidByte1, E::GuidByte0, E::GuidByte2, E::GuidByte5, E::MovementCounter,
    E::GuidByte3, E::GuidByte4, E::GuidByte7, E::GuidByte6, E::End)

LAYOUT(MoveUnroot,
    E::GuidBit0, E::GuidBit1, E::GuidBit3, E::GuidBit7, E::GuidBit5, E::GuidBit2, E::GuidBit4,
    E::GuidBit6, E::GuidByte3, E::GuidByte6, E::GuidByte1, E::MovementCounter, E::GuidByte2,
    E::GuidByte0, E::GuidByte7, E::GuidByte4, E::GuidByte5, E::End)

LAYOUT(ChangeSeatsOnControlledVehicle,
    E::PositionY, E::PositionX, E::PositionZ, E::ByteParam, E::HasMovementFlags,
    E::HasTransportData, E::GuidBit2, E::GuidBit6, E::GuidBit4, E::Guid2Bit2, E::Guid2Bit4,
    E::HasOrientation, E::HasUnknownBit, E::Guid2Bit7, E::GuidBit7, E::Guid2Bit6, E::HasTimestamp,
    E::HasSplineElevation, E::GuidBit5, E::Guid2Bit5, E::HasMovementFlags2, E::HasPitch,
    E::Guid2Bit0, E::GuidBit0, E::Guid2Bit1, E::HasFallData, E::GuidBit1, E::HasSpline, E::Flags,
    E::Guid2Bit3, E::GuidBit3, E::TransportGuidBit3, E::TransportGuidBit0, E::TransportGuidBit7,
    E::TransportGuidBit5, E::HasVehicleId, E::TransportGuidBit1, E::TransportGuidBit2,
    E::HasTransportTime2, E::TransportGuidBit4, E::TransportGuidBit6, E::Flags2,
    E::HasFallDirection, E::Guid2Byte6, E::GuidByte7, E::GuidByte5, E::Guid2Byte1, E::Guid2Byte2,
    E::GuidByte6, E::Guid2Byte5, E::Guid2Byte3, E::GuidByte3, E::Guid2Byte0, E::GuidByte0,
    E::Guid2Byte4, E::GuidByte4, E::GuidByte1, E::Guid2Byte7, E::GuidByte2, E::Pitch,
    E::FallSinAngle, E::FallCosAngle, E::FallHorizontalSpeed, E::FallTime, E::FallVerticalSpeed,
    E::TransportGuidByte2, E::TransportTime2, E::TransportVehicleId, E::TransportGuidByte0,
    E::TransportTime, E::TransportSeat, E::TransportPositionX, E::TransportPositionO,
    E::TransportGuidByte7, E::TransportGuidByte4, E::TransportGuidByte3, E::TransportGuidByte5,
    E::TransportPositionZ, E::TransportGuidByte1, E::TransportGuidByte6, E::TransportPositionY,
    E::SplineElevation, E::PositionO, E::Timestamp, E::End)

LAYOUT(CastSpellEmbeddedMovement,
    E::PositionZ, E::PositionY, E::PositionX, E::HasFallData, E::HasTimestamp, E::HasOrientation,
    E::HasUnknownBit, E::HasSpline, E::GuidBit6, E::GuidBit4, E::HasMovementFlags2, E::GuidBit3,
    E::GuidBit5, E::HasSplineElevation, E::HasPitch, E::GuidBit7, E::HasTransportData, E::GuidBit2,
    E::HasMovementFlags, E::GuidBit1, E::GuidBit0, E::TransportGuidBit6, E::TransportGuidBit2,
    E::TransportGuidBit5, E::HasTransportTime2, E::TransportGuidBit7, E::TransportGuidBit4,
    E::HasVehicleId, E::TransportGuidBit0, E::TransportGuidBit1, E::TransportGuidBit3, E::Flags2,
    E::Flags, E::HasFallDirection, E::GuidByte1, E::GuidByte4, E::GuidByte7, E::GuidByte3,
    E::GuidByte0, E::GuidByte2, E::GuidByte5, E::GuidByte6, E::TransportSeat, E::TransportPositionO,
    E::TransportTime, E::TransportGuidByte6, E::TransportGuidByte5, E::TransportVehicleId,
    E::TransportPositionX, E::TransportGuidByte4, E::TransportPositionZ, E::TransportGuidByte2,
    E::TransportGuidByte0, E::TransportTime2, E::TransportGuidByte1, E::TransportGuidByte3,
    E::TransportPositionY, E::TransportGuidByte7, E::PositionO, E::SplineElevation, E::FallTime,
    E::FallHorizontalSpeed, E::FallCosAngle, E::FallSinAngle, E::FallVerticalSpeed, E::Timestamp,
    E::Pitch, E::End)

LAYOUT(MoveGravityDisable,
    E::GuidBit0, E::GuidBit1, E::GuidBit5, E::GuidBit7, E::GuidBit6, E::GuidBit4, E::GuidBit3,
    E::GuidBit2, E::GuidByte7, E::GuidByte2, E::GuidByte0, E::MovementCounter, E::GuidByte5,
    E::GuidByte1, E::GuidByte3, E::GuidByte4, E::GuidByte6, E::End)

LAYOUT(MoveGravityEnable,
    E::GuidBit1, E::GuidBit4, E::GuidBit7, E::GuidBit5, E::GuidBit2, E::GuidBit0, E::GuidBit3,
    E::GuidBit6, E::GuidByte3, E::MovementCounter, E::GuidByte7, E::GuidByte6, E::GuidByte4,
    E::GuidByte0, E::GuidByte1, E::GuidByte5, E::GuidByte2, E::End)

LAYOUT(MoveUpdateFlightBackSpeed,
    E::PositionY, E::ExtraFloat, E::PositionX, E::PositionZ, E::GuidBit6, E::GuidBit4, E::HasPitch,
    E::GuidBit7, E::GuidBit1, E::HasTransportData, E::HasVehicleId, E::TransportGuidBit4,
    E::TransportGuidBit7, E::TransportGuidBit6, E::TransportGuidBit0, E::OneBit,
    E::TransportGuidBit2, E::TransportGuidBit3, E::TransportGuidBit5, E::HasTransportTime2,
    E::TransportGuidBit1, E::GuidBit5, E::GuidBit0, E::HasTimestamp, E::HasMovementFlags, E::Flags,
    E::GuidBit3, E::HasMovementFlags2, E::HasUnknownBit, E::HasSplineElevation, E::Flags2,
    E::GuidBit2, E::HasFallData, E::HasOrientation, E::HasFallDirection, E::FlushBits,
    E::FallSinAngle, E::FallCosAngle, E::FallVerticalSpeed, E::FallTime, E::FallHorizontalSpeed,
    E::TransportPositionX, E::TransportTime, E::TransportPositionO, E::TransportGuidByte5,
    E::TransportPositionZ, E::TransportVehicleId, E::TransportGuidByte7, E::TransportGuidByte1,
    E::TransportGuidByte3, E::TransportGuidByte6, E::TransportGuidByte0, E::TransportGuidByte2,
    E::TransportPositionY, E::TransportGuidByte4, E::GuidByte1, E::GuidByte4, E::PositionO,
    E::GuidByte5, E::Timestamp, E::SplineElevation, E::GuidByte6, E::Pitch, E::GuidByte3,
    E::GuidByte2, E::GuidByte0, E::GuidByte7, E::End)

LAYOUT(MoveSetCanTransitionBetweenSwimAndFly,
    E::GuidBit4, E::GuidBit5, E::GuidBit7, E::GuidBit0, E::GuidBit2, E::GuidBit6, E::GuidBit1,
    E::GuidBit3, E::GuidByte0, E::GuidByte6, E::GuidByte1, E::GuidByte2, E::GuidByte5, E::GuidByte4,
    E::GuidByte7, E::MovementCounter, E::GuidByte3, E::End)

LAYOUT(MoveUnsetCanTransitionBetweenSwimAndFly,
    E::GuidBit3, E::GuidBit4, E::GuidBit7, E::GuidBit0, E::GuidBit6, E::GuidBit1, E::GuidBit5,
    E::GuidBit2, E::GuidByte5, E::GuidByte2, E::GuidByte7, E::GuidByte0, E::MovementCounter,
    E::GuidByte6, E::GuidByte4, E::GuidByte3, E::GuidByte1, E::End)

// lifted from the client reader sub_14038E360; see lift_client_reader.py
LAYOUT(MovementUpdateRunBackSpeed,
    E::GuidBit1, E::GuidBit2, E::HasSplineElevation, E::GuidBit4, E::GuidBit3, E::HasFallData,
    E::GuidBit6, E::HasTimestamp, E::GuidBit0, E::HasUnknownBit, E::HasMovementFlags, E::HasPitch,
    E::HasSpline, E::GuidBit5, E::HasMovementFlags2, E::Flags2, E::HasOrientation, E::Flags,
    E::HasFallDirection, E::HasTransportData, E::TransportGuidBit5, E::HasTransportTime2,
    E::TransportGuidBit3, E::TransportGuidBit1, E::TransportGuidBit6, E::TransportGuidBit7,
    E::TransportGuidBit2, E::TransportGuidBit4, E::TransportGuidBit0, E::HasVehicleId, E::GuidBit7,
    E::FlushBits, E::TransportPositionX, E::TransportGuidByte2, E::TransportGuidByte5,
    E::TransportGuidByte4, E::TransportGuidByte6, E::TransportTime2, E::TransportGuidByte0,
    E::TransportGuidByte3, E::TransportPositionY, E::TransportGuidByte7, E::TransportVehicleId,
    E::TransportPositionZ, E::TransportTime, E::TransportSeat, E::TransportGuidByte1,
    E::TransportPositionO, E::GuidByte4, E::FallTime, E::FallHorizontalSpeed, E::FallCosAngle,
    E::FallSinAngle, E::FallVerticalSpeed, E::Timestamp, E::SplineElevation, E::GuidByte1,
    E::PositionO, E::GuidByte0, E::GuidByte5, E::GuidByte3, E::PositionX, E::PositionY, E::Pitch,
    E::GuidByte7, E::ExtraFloat, E::GuidByte2, E::GuidByte6, E::PositionZ, E::End)

// lifted from the client reader sub_14038EFE0; see lift_client_reader.py
LAYOUT(MovementUpdateWalkSpeed,
    E::HasPitch, E::HasOrientation, E::HasUnknownBit, E::GuidBit3, E::HasSplineElevation,
    E::GuidBit2, E::HasTransportData, E::TransportGuidBit6, E::TransportGuidBit3,
    E::TransportGuidBit2, E::TransportGuidBit0, E::TransportGuidBit4, E::HasTransportTime2,
    E::TransportGuidBit7, E::TransportGuidBit1, E::TransportGuidBit5, E::HasVehicleId, E::GuidBit7,
    E::GuidBit5, E::GuidBit1, E::HasFallData, E::GuidBit0, E::HasMovementFlags2, E::HasTimestamp,
    E::HasMovementFlags, E::GuidBit6, E::HasFallDirection, E::Flags2, E::Flags, E::HasSpline,
    E::GuidBit4, E::FlushBits, E::Pitch, E::TransportGuidByte6, E::TransportGuidByte0,
    E::TransportGuidByte4, E::TransportGuidByte2, E::TransportPositionX, E::TransportGuidByte7,
    E::TransportTime, E::TransportTime2, E::TransportPositionZ, E::TransportSeat,
    E::TransportGuidByte5, E::TransportVehicleId, E::TransportPositionO, E::TransportGuidByte1,
    E::TransportPositionY, E::TransportGuidByte3, E::SplineElevation, E::FallVerticalSpeed,
    E::FallHorizontalSpeed, E::FallCosAngle, E::FallSinAngle, E::FallTime, E::GuidByte1,
    E::GuidByte4, E::GuidByte2, E::GuidByte6, E::GuidByte7, E::Timestamp, E::PositionO,
    E::PositionY, E::GuidByte0, E::PositionZ, E::PositionX, E::GuidByte3, E::GuidByte5,
    E::ExtraFloat, E::End)

// lifted from the client reader sub_140382F50; see lift_client_reader.py
LAYOUT(MovementUpdateKnockBack,
    E::HasUnknownBit, E::GuidBit4, E::HasMovementFlags, E::HasPitch, E::HasTimestamp, E::GuidBit1,
    E::GuidBit0, E::GuidBit3, E::GuidBit2, E::GuidBit7, E::HasSpline, E::HasTransportData,
    E::TransportGuidBit7, E::TransportGuidBit5, E::TransportGuidBit1, E::TransportGuidBit6,
    E::HasTransportTime2, E::TransportGuidBit2, E::TransportGuidBit4, E::TransportGuidBit0,
    E::HasVehicleId, E::TransportGuidBit3, E::GuidBit5, E::HasSplineElevation, E::HasMovementFlags2,
    E::GuidBit6, E::Flags, E::HasFallData, E::HasFallDirection, E::HasOrientation, E::Flags2,
    E::FlushBits, E::PositionO, E::FallCosAngle, E::FallHorizontalSpeed, E::FallSinAngle,
    E::FallTime, E::FallVerticalSpeed, E::SplineElevation, E::GuidByte3, E::TransportGuidByte5,
    E::TransportVehicleId, E::TransportGuidByte7, E::TransportSeat, E::TransportGuidByte3,
    E::TransportGuidByte6, E::TransportPositionZ, E::TransportGuidByte1, E::TransportPositionY,
    E::TransportPositionX, E::TransportGuidByte2, E::TransportGuidByte0, E::TransportPositionO,
    E::TransportTime, E::TransportGuidByte4, E::TransportTime2, E::Pitch, E::PositionZ,
    E::Timestamp, E::PositionX, E::GuidByte4, E::GuidByte6, E::GuidByte7, E::GuidByte2,
    E::GuidByte1, E::PositionY, E::GuidByte0, E::GuidByte5, E::End)

// lifted from the client reader sub_140384210; no reference table; see lift_client_reader.py
LAYOUT(MovementUpdateTeleport,
    E::PositionZ, E::PositionY, E::PositionX, E::HasOrientation, E::HasSpline, E::HasMovementFlags,
    E::GuidBit2, E::GuidBit4, E::GuidBit6, E::HasFallData, E::GuidBit0, E::HasTransportData,
    E::GuidBit5, E::TransportGuidBit1, E::TransportGuidBit4, E::TransportGuidBit5,
    E::TransportGuidBit3, E::TransportGuidBit0, E::HasTransportTime2, E::TransportGuidBit7,
    E::TransportGuidBit6, E::HasVehicleId, E::TransportGuidBit2, E::HasUnknownBit, E::GuidBit7,
    E::GuidBit3, E::HasPitch, E::HasMovementFlags2, E::HasTimestamp, E::HasFallDirection, E::Flags2,
    E::HasSplineElevation, E::Flags, E::GuidBit1, E::FlushBits, E::GuidByte7, E::TransportGuidByte3,
    E::TransportGuidByte4, E::TransportPositionO, E::TransportVehicleId, E::TransportGuidByte1,
    E::TransportTime2, E::TransportPositionZ, E::TransportGuidByte7, E::TransportGuidByte0,
    E::TransportGuidByte6, E::TransportGuidByte5, E::TransportGuidByte2, E::TransportSeat,
    E::TransportTime, E::TransportPositionY, E::TransportPositionX, E::GuidByte6, E::Pitch,
    E::SplineElevation, E::PositionO, E::GuidByte2, E::GuidByte3, E::GuidByte1, E::FallTime,
    E::FallHorizontalSpeed, E::FallSinAngle, E::FallCosAngle, E::FallVerticalSpeed, E::GuidByte5,
    E::GuidByte4, E::Timestamp, E::GuidByte0, E::End)

// lifted from the client reader sub_14038C780; no reference table; see lift_client_reader.py
LAYOUT(MovementUpdateTurnRate,
    E::ExtraFloat, E::PositionY, E::PositionX, E::PositionZ, E::HasSpline, E::HasTransportData,
    E::TransportGuidBit0, E::HasVehicleId, E::TransportGuidBit6, E::TransportGuidBit1,
    E::TransportGuidBit2, E::TransportGuidBit4, E::TransportGuidBit7, E::HasTransportTime2,
    E::TransportGuidBit3, E::TransportGuidBit5, E::HasPitch, E::GuidBit5, E::HasOrientation,
    E::GuidBit2, E::GuidBit4, E::HasMovementFlags2, E::HasSplineElevation, E::GuidBit3,
    E::HasTimestamp, E::Flags2, E::HasMovementFlags, E::GuidBit1, E::GuidBit0, E::GuidBit6,
    E::HasFallData, E::HasUnknownBit, E::GuidBit7, E::HasFallDirection, E::Flags, E::FlushBits,
    E::TransportGuidByte2, E::TransportVehicleId, E::TransportTime2, E::TransportTime,
    E::TransportGuidByte6, E::TransportGuidByte1, E::TransportGuidByte7, E::TransportGuidByte0,
    E::TransportPositionX, E::TransportPositionO, E::TransportGuidByte3, E::TransportPositionZ,
    E::TransportSeat, E::TransportGuidByte4, E::TransportPositionY, E::TransportGuidByte5,
    E::SplineElevation, E::GuidByte2, E::PositionO, E::Pitch, E::FallCosAngle, E::FallSinAngle,
    E::FallHorizontalSpeed, E::FallTime, E::FallVerticalSpeed, E::GuidByte7, E::GuidByte3,
    E::GuidByte6, E::Timestamp, E::GuidByte4, E::GuidByte5, E::GuidByte0, E::GuidByte1, E::End)

MAP(CMSG_CAST_SPELL, CastSpellEmbeddedMovement)
MAP(CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE, ChangeSeatsOnControlledVehicle)
MAP(CMSG_DISMISS_CONTROLLED_VEHICLE, DismissControlledVehicle)
MAP(CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK, MoveForceFlightBackSpeedChangeAck)
MAP(CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK, MovementForceFlightSpeedChangeAck)
MAP(CMSG_FORCE_MOVE_ROOT_ACK, ForceMoveRootAck)
MAP(CMSG_FORCE_MOVE_UNROOT_ACK, ForceMoveUnrootAck)
MAP(CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK, MovementForceRunBackSpeedChangeAck)
MAP(CMSG_FORCE_RUN_SPEED_CHANGE_ACK, MovementForceRunSpeedChangeAck)
MAP(CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK, MoveForceSwimBackSpeedChangeAck)
MAP(CMSG_FORCE_SWIM_SPEED_CHANGE_ACK, MovementForceSwimSpeedChangeAck)
MAP(CMSG_FORCE_WALK_SPEED_CHANGE_ACK, MovementForceWalkSpeedChangeAck)
MAP(CMSG_MOVE_CHNG_TRANSPORT, MoveChngTransport)
MAP(CMSG_MOVE_FALL_LAND, MovementFallLand)
MAP(CMSG_MOVE_FALL_RESET, MovementFallReset)
MAP(CMSG_MOVE_FEATHER_FALL_ACK, MovementFeatherFallAck)
MAP(CMSG_MOVE_GRAVITY_DISABLE_ACK, MovementGravityDisableAck)
MAP(CMSG_MOVE_GRAVITY_ENABLE_ACK, MovementGravityEnableAck)
MAP(CMSG_MOVE_HOVER_ACK, MovementHoverAck)
MAP(CMSG_MOVE_JUMP, MovementJump)
MAP(CMSG_MOVE_KNOCK_BACK_ACK, MovementKnockBackAck)
MAP(CMSG_MOVE_NOT_ACTIVE_MOVER, MoveNotActiveMover)
MAP(CMSG_MOVE_SET_CAN_FLY, MovementSetCanFly)
MAP(CMSG_MOVE_SET_CAN_FLY_ACK, MovementSetCanFlyAck)
MAP(CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, MovementSetCanTransitionBetweenSwimAndFlyAck)
MAP(CMSG_MOVE_SET_COLLISION_HGT_ACK, MovementSetCollisionHeightAck)
MAP(CMSG_MOVE_SET_FACING, MovementSetFacing)
MAP(CMSG_MOVE_SET_PITCH, MovementSetPitch)
MAP(CMSG_MOVE_SET_RUN_MODE, MovementSetRunMode)
MAP(CMSG_MOVE_SET_WALK_MODE, MovementSetWalkMode)
MAP(CMSG_MOVE_SPLINE_DONE, MoveSplineDone)
MAP(CMSG_MOVE_START_ASCEND, MovementStartAscend)
MAP(CMSG_MOVE_START_BACKWARD, MovementStartBackward)
MAP(CMSG_MOVE_START_DESCEND, MovementStartDescend)
MAP(CMSG_MOVE_START_FORWARD, MovementStartForward)
MAP(CMSG_MOVE_START_PITCH_DOWN, MovementStartPitchDown)
MAP(CMSG_MOVE_START_PITCH_UP, MovementStartPitchUp)
MAP(CMSG_MOVE_START_STRAFE_LEFT, MovementStartStrafeLeft)
MAP(CMSG_MOVE_START_STRAFE_RIGHT, MovementStartStrafeRight)
MAP(CMSG_MOVE_START_SWIM, MovementStartSwim)
MAP(CMSG_MOVE_START_TURN_LEFT, MovementStartTurnLeft)
MAP(CMSG_MOVE_START_TURN_RIGHT, MovementStartTurnRight)
MAP(CMSG_MOVE_STOP, MovementStop)
MAP(CMSG_MOVE_STOP_ASCEND, MovementStopAscend)
MAP(CMSG_MOVE_STOP_PITCH, MovementStopPitch)
MAP(CMSG_MOVE_STOP_STRAFE, MovementStopStrafe)
MAP(CMSG_MOVE_STOP_SWIM, MovementStopSwim)
MAP(CMSG_MOVE_STOP_TURN, MovementStopTurn)
MAP(CMSG_MOVE_WATER_WALK_ACK, MovementWaterWalkAck)
MAP(CMSG_PET_CAST_SPELL, CastSpellEmbeddedMovement)
MAP(CMSG_USE_ITEM, CastSpellEmbeddedMovement)
MAP(MSG_MOVE_HEARTBEAT, MovementHeartBeat)
MAP(SMSG_FORCE_MOVE_ROOT, MoveRoot)
MAP(SMSG_FORCE_MOVE_UNROOT, MoveUnroot)
MAP(SMSG_MOVE_FEATHER_FALL, MoveFeatherFall)
MAP(SMSG_MOVE_GRAVITY_DISABLE, MoveGravityDisable)
MAP(SMSG_MOVE_GRAVITY_ENABLE, MoveGravityEnable)
MAP(SMSG_MOVE_LAND_WALK, MoveLandWalk)
MAP(SMSG_MOVE_NORMAL_FALL, MoveNormalFall)
MAP(SMSG_MOVE_SET_CAN_FLY, MoveSetCanFly)
MAP(SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY, MoveSetCanTransitionBetweenSwimAndFly)
MAP(SMSG_MOVE_SET_COLLISION_HGT, MovementSetCollisionHeight)
MAP(SMSG_MOVE_SET_FLIGHT_BACK_SPEED, MoveSetFlightBackSpeed)
MAP(SMSG_MOVE_SET_FLIGHT_SPEED, MoveSetFlightSpeed)
MAP(SMSG_MOVE_SET_HOVER, MoveSetHover)
MAP(SMSG_MOVE_SET_PITCH_RATE, MoveSetPitchRate)
MAP(SMSG_MOVE_SET_RUN_BACK_SPEED, MoveSetRunBackSpeed)
MAP(SMSG_MOVE_SET_RUN_SPEED, MoveSetRunSpeed)
MAP(SMSG_MOVE_SET_SWIM_BACK_SPEED, MoveSetSwimBackSpeed)
MAP(SMSG_MOVE_SET_SWIM_SPEED, MoveSetSwimSpeed)
MAP(SMSG_MOVE_SET_TURN_RATE, MoveSetTurnRate)
MAP(SMSG_MOVE_SET_WALK_SPEED, MoveSetWalkSpeed)
MAP(SMSG_MOVE_UNSET_CAN_FLY, MoveUnsetCanFly)
MAP(SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY, MoveUnsetCanTransitionBetweenSwimAndFly)
MAP(SMSG_MOVE_UNSET_HOVER, MoveUnsetHover)
MAP(SMSG_MOVE_UPDATE_COLLISION_HEIGHT, MovementUpdateCollisionHeight)
MAP(SMSG_MOVE_UPDATE_FLIGHT_BACK_SPEED, MoveUpdateFlightBackSpeed)
MAP(SMSG_MOVE_UPDATE_FLIGHT_SPEED, MovementUpdateFlightSpeed)
MAP(SMSG_MOVE_UPDATE_KNOCK_BACK, MovementUpdateKnockBack)
MAP(SMSG_MOVE_UPDATE_RUN_BACK_SPEED, MovementUpdateRunBackSpeed)
MAP(SMSG_MOVE_UPDATE_RUN_SPEED, MovementUpdateRunSpeed)
MAP(SMSG_MOVE_UPDATE_SWIM_BACK_SPEED, MoveUpdateSwimBackSpeed)
MAP(SMSG_MOVE_UPDATE_SWIM_SPEED, MovementUpdateSwimSpeed)
MAP(SMSG_MOVE_UPDATE_TELEPORT, MovementUpdateTeleport)
MAP(SMSG_MOVE_UPDATE_TURN_RATE, MovementUpdateTurnRate)
MAP(SMSG_MOVE_UPDATE_WALK_SPEED, MovementUpdateWalkSpeed)
MAP(SMSG_MOVE_WATER_WALK, MoveWaterWalk)
MAP(SMSG_PLAYER_MOVE, MovementUpdate)
MAP(SMSG_SPLINE_MOVE_FEATHER_FALL, SplineMoveSetFeatherFall)
MAP(SMSG_SPLINE_MOVE_GRAVITY_DISABLE, SplineMoveGravityDisable)
MAP(SMSG_SPLINE_MOVE_GRAVITY_ENABLE, SplineMoveGravityEnable)
MAP(SMSG_SPLINE_MOVE_LAND_WALK, SplineMoveSetLandWalk)
MAP(SMSG_SPLINE_MOVE_NORMAL_FALL, SplineMoveSetNormalFall)
MAP(SMSG_SPLINE_MOVE_ROOT, SplineMoveRoot)
MAP(SMSG_SPLINE_MOVE_SET_FLIGHT_BACK_SPEED, SplineMoveSetFlightBackSpeed)
MAP(SMSG_SPLINE_MOVE_SET_FLIGHT_SPEED, SplineMoveSetFlightSpeed)
MAP(SMSG_SPLINE_MOVE_SET_FLYING, SplineMoveSetFlying)
MAP(SMSG_SPLINE_MOVE_SET_HOVER, SplineMoveSetHover)
MAP(SMSG_SPLINE_MOVE_SET_PITCH_RATE, SplineMoveSetPitchRate)
MAP(SMSG_SPLINE_MOVE_SET_RUN_BACK_SPEED, SplineMoveSetRunBackSpeed)
MAP(SMSG_SPLINE_MOVE_SET_RUN_MODE, SplineMoveSetRunMode)
MAP(SMSG_SPLINE_MOVE_SET_RUN_SPEED, SplineMoveSetRunSpeed)
MAP(SMSG_SPLINE_MOVE_SET_SWIM_BACK_SPEED, SplineMoveSetSwimBackSpeed)
MAP(SMSG_SPLINE_MOVE_SET_SWIM_SPEED, SplineMoveSetSwimSpeed)
MAP(SMSG_SPLINE_MOVE_SET_TURN_RATE, SplineMoveSetTurnRate)
MAP(SMSG_SPLINE_MOVE_SET_WALK_MODE, SplineMoveSetWalkMode)
MAP(SMSG_SPLINE_MOVE_SET_WALK_SPEED, SplineMoveSetWalkSpeed)
MAP(SMSG_SPLINE_MOVE_START_SWIM, SplineMoveStartSwim)
MAP(SMSG_SPLINE_MOVE_STOP_SWIM, SplineMoveStopSwim)
MAP(SMSG_SPLINE_MOVE_UNROOT, SplineMoveUnroot)
MAP(SMSG_SPLINE_MOVE_UNSET_FLYING, SplineMoveUnsetFlying)
MAP(SMSG_SPLINE_MOVE_UNSET_HOVER, SplineMoveUnsetHover)
MAP(SMSG_SPLINE_MOVE_WATER_WALK, SplineMoveSetWaterWalk)
