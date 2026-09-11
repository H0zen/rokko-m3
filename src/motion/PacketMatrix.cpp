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

#include "PacketMatrix.h"
#include "Opcodes.h"

namespace Motion
{
    namespace
    {
        const char* const kFilled = "";
        const char* const kNoRateAck = "the client's ack (CMSG_FORCE_*_RATE_CHANGE_ACK) has no registry layout: a client writer, not liftable";
        const char* const kNoPitchRateObserver = "the client's ack (CMSG_FORCE_PITCH_RATE_CHANGE_ACK) has no registry layout: a client writer, not liftable; its observer's own reader (sub_14037B330) defeated the lifter (Task 6: a bitwise-complement gate bit, `~expr >> 7`, that lift_client_reader.py cannot follow) and has no layout either";
        const char* const kNoSplineFlag = "4.3.4 has no spline form of this change";
        const char* const kNoSplineValue = "creatures have no packet for this: height is a player thing, a knock-back is a spline jump (P3), a teleport is an object update";
        const char* const kServerOnly = "server-driven only (design v2 6.4): no mover packet, no ack, no observer update";

        const MatrixRow kRows[] =
        {
            // type, apply, mover, ack, observer, spline, note
            { ChangeType::WalkSpeed,       true,  SMSG_MOVE_SET_WALK_SPEED,        CMSG_FORCE_WALK_SPEED_CHANGE_ACK,        SMSG_MOVE_UPDATE_WALK_SPEED,        SMSG_SPLINE_MOVE_SET_WALK_SPEED,        kFilled },
            { ChangeType::RunSpeed,        true,  SMSG_MOVE_SET_RUN_SPEED,         CMSG_FORCE_RUN_SPEED_CHANGE_ACK,         SMSG_MOVE_UPDATE_RUN_SPEED,         SMSG_SPLINE_MOVE_SET_RUN_SPEED,         kFilled },
            { ChangeType::RunBackSpeed,    true,  SMSG_MOVE_SET_RUN_BACK_SPEED,    CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK,    SMSG_MOVE_UPDATE_RUN_BACK_SPEED,    SMSG_SPLINE_MOVE_SET_RUN_BACK_SPEED,    kFilled },
            { ChangeType::SwimSpeed,       true,  SMSG_MOVE_SET_SWIM_SPEED,        CMSG_FORCE_SWIM_SPEED_CHANGE_ACK,        SMSG_MOVE_UPDATE_SWIM_SPEED,        SMSG_SPLINE_MOVE_SET_SWIM_SPEED,        kFilled },
            { ChangeType::SwimBackSpeed,   true,  SMSG_MOVE_SET_SWIM_BACK_SPEED,   CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK,   SMSG_MOVE_UPDATE_SWIM_BACK_SPEED,   SMSG_SPLINE_MOVE_SET_SWIM_BACK_SPEED,   kFilled },
            { ChangeType::TurnRate,        true,  SMSG_MOVE_SET_TURN_RATE,         0,                                       SMSG_MOVE_UPDATE_TURN_RATE,         SMSG_SPLINE_MOVE_SET_TURN_RATE,         kNoRateAck },
            { ChangeType::FlightSpeed,     true,  SMSG_MOVE_SET_FLIGHT_SPEED,      CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK,      SMSG_MOVE_UPDATE_FLIGHT_SPEED,      SMSG_SPLINE_MOVE_SET_FLIGHT_SPEED,      kFilled },
            { ChangeType::FlightBackSpeed, true,  SMSG_MOVE_SET_FLIGHT_BACK_SPEED, CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK, SMSG_MOVE_UPDATE_FLIGHT_BACK_SPEED, SMSG_SPLINE_MOVE_SET_FLIGHT_BACK_SPEED, kFilled },
            { ChangeType::PitchRate,       true,  SMSG_MOVE_SET_PITCH_RATE,        0,                                       0 /* BLOCKED, Task 6 */,            SMSG_SPLINE_MOVE_SET_PITCH_RATE,        kNoPitchRateObserver },

            { ChangeType::Root,            true,  SMSG_FORCE_MOVE_ROOT,            CMSG_FORCE_MOVE_ROOT_ACK,                SMSG_FORCE_MOVE_ROOT,               SMSG_SPLINE_MOVE_ROOT,                  kFilled },
            { ChangeType::Root,            false, SMSG_FORCE_MOVE_UNROOT,          CMSG_FORCE_MOVE_UNROOT_ACK,              SMSG_FORCE_MOVE_UNROOT,             SMSG_SPLINE_MOVE_UNROOT,                kFilled },
            { ChangeType::CanFly,          true,  SMSG_MOVE_SET_CAN_FLY,           CMSG_MOVE_SET_CAN_FLY_ACK,               SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_SET_FLYING,            kFilled },
            { ChangeType::CanFly,          false, SMSG_MOVE_UNSET_CAN_FLY,         CMSG_MOVE_SET_CAN_FLY_ACK,               SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_UNSET_FLYING,          kFilled },
            { ChangeType::WaterWalk,       true,  SMSG_MOVE_WATER_WALK,            CMSG_MOVE_WATER_WALK_ACK,                SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_WATER_WALK,            kFilled },
            { ChangeType::WaterWalk,       false, SMSG_MOVE_LAND_WALK,             CMSG_MOVE_WATER_WALK_ACK,                SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_LAND_WALK,             kFilled },
            { ChangeType::FeatherFall,     true,  SMSG_MOVE_FEATHER_FALL,          CMSG_MOVE_FEATHER_FALL_ACK,              SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_FEATHER_FALL,          kFilled },
            { ChangeType::FeatherFall,     false, SMSG_MOVE_NORMAL_FALL,           CMSG_MOVE_FEATHER_FALL_ACK,              SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_NORMAL_FALL,           kFilled },
            { ChangeType::Hover,           true,  SMSG_MOVE_SET_HOVER,             CMSG_MOVE_HOVER_ACK,                     SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_SET_HOVER,             kFilled },
            { ChangeType::Hover,           false, SMSG_MOVE_UNSET_HOVER,           CMSG_MOVE_HOVER_ACK,                     SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_UNSET_HOVER,           kFilled },
            { ChangeType::GravityDisabled, true,  SMSG_MOVE_GRAVITY_DISABLE,       CMSG_MOVE_GRAVITY_DISABLE_ACK,           SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_GRAVITY_DISABLE,       kFilled },
            { ChangeType::GravityDisabled, false, SMSG_MOVE_GRAVITY_ENABLE,        CMSG_MOVE_GRAVITY_ENABLE_ACK,            SMSG_PLAYER_MOVE,                   SMSG_SPLINE_MOVE_GRAVITY_ENABLE,        kFilled },
            { ChangeType::CanTransitionSwimFly, true,  SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY,   CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, SMSG_PLAYER_MOVE, 0, kNoSplineFlag },
            { ChangeType::CanTransitionSwimFly, false, SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY, CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, SMSG_PLAYER_MOVE, 0, kNoSplineFlag },

            { ChangeType::CollisionHeight, true,  SMSG_MOVE_SET_COLLISION_HGT, CMSG_MOVE_SET_COLLISION_HGT_ACK, SMSG_MOVE_UPDATE_COLLISION_HEIGHT, 0, kNoSplineValue },
            { ChangeType::KnockBack,       true,  SMSG_MOVE_KNOCK_BACK,        CMSG_MOVE_KNOCK_BACK_ACK,        SMSG_MOVE_UPDATE_KNOCK_BACK,       0, kNoSplineValue },
            { ChangeType::Teleport,        true,  SMSG_MOVE_TELEPORT,          CMSG_MOVE_TELEPORT_ACK,          SMSG_MOVE_UPDATE_TELEPORT,         0, kNoSplineValue },

            { ChangeType::Gait,            true,  0, 0, 0, SMSG_SPLINE_MOVE_SET_WALK_MODE, kServerOnly },
            { ChangeType::Gait,            false, 0, 0, 0, SMSG_SPLINE_MOVE_SET_RUN_MODE,  kServerOnly },
            { ChangeType::Swim,            true,  0, 0, 0, SMSG_SPLINE_MOVE_START_SWIM,    kServerOnly },
            { ChangeType::Swim,            false, 0, 0, 0, SMSG_SPLINE_MOVE_STOP_SWIM,     kServerOnly },
        };

        const size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

        bool HasApplyPair(ChangeType type)
        {
            switch (type)
            {
                case ChangeType::Root: case ChangeType::CanFly: case ChangeType::WaterWalk: case ChangeType::FeatherFall:
                case ChangeType::Hover: case ChangeType::GravityDisabled: case ChangeType::CanTransitionSwimFly:
                case ChangeType::Gait: case ChangeType::Swim:
                    return true;
                default:
                    return false;
            }
        }
    }

    MatrixRow const* RowFor(ChangeType type, bool apply)
    {
        if (type == ChangeType::None || type == ChangeType::Count) { return NULL; }
        const bool wanted = HasApplyPair(type) ? apply : true;
        for (size_t i = 0; i < kRowCount; ++i)
        {
            if (kRows[i].type == type && kRows[i].apply == wanted) { return &kRows[i]; }
        }
        return NULL;
    }

    size_t MatrixSize() { return kRowCount; }

    MatrixRow const& MatrixRowAt(size_t index) { return kRows[index]; }

    MatrixRow const* RowForOpcode(uint16 opcode, bool* isSpline)
    {
        if (!opcode) { return NULL; }
        for (size_t i = 0; i < kRowCount; ++i)
        {
            if (kRows[i].mover == opcode)  { if (isSpline) { *isSpline = false; } return &kRows[i]; }
            if (kRows[i].spline == opcode) { if (isSpline) { *isSpline = true; }  return &kRows[i]; }
        }
        return NULL;
    }

    MatrixRow const* RowForAck(uint16 opcode)
    {
        if (!opcode) { return NULL; }
        for (size_t i = 0; i < kRowCount; ++i)
        {
            if (kRows[i].ack == opcode) { return &kRows[i]; }
        }
        return NULL;
    }
}
