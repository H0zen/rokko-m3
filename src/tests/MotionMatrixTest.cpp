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

#include "TestHarness.h"

#include "Opcodes.h"
#include "Change.h"
#include "PacketMatrix.h"
#include "wire/MovementFamilies.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Motion;

namespace
{
    std::string Joined(std::vector<std::string> v)
    {
        std::sort(v.begin(), v.end());
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { s += v[i]; s += ' '; }
        return s;
    }
}

TEST(MotionMatrix_has_thirty_rows_and_every_named_cell_is_known_to_the_wire)
{
    CHECK_EQ(MatrixSize(), size_t(30));
    for (size_t i = 0; i < MatrixSize(); ++i)
    {
        MatrixRow const& row = MatrixRowAt(i);
        if (row.mover)    { CHECK(Wire::IsKnown(row.mover)); }
        if (row.ack)      { CHECK(Wire::IsKnown(row.ack)); }
        if (row.observer) { CHECK(Wire::IsKnown(row.observer)); }
        if (row.spline)   { CHECK(Wire::IsKnown(row.spline)); }
        CHECK(row.note != NULL);
        const bool anyEmpty = !row.mover || !row.ack || !row.observer || !row.spline;
        CHECK_EQ(std::string(row.note).empty(), !anyEmpty);
    }
}

TEST(MotionMatrix_the_empty_cells_are_exactly_the_pinned_ones)
{
    std::vector<std::string> empty;
    for (size_t i = 0; i < MatrixSize(); ++i)
    {
        MatrixRow const& row = MatrixRowAt(i);
        const std::string name = std::string(ChangeName(row.type)) + (row.apply ? "+" : "-");
        if (!row.mover)    { empty.push_back(name + ":mover"); }
        if (!row.ack)      { empty.push_back(name + ":ack"); }
        if (!row.observer) { empty.push_back(name + ":observer"); }
        if (!row.spline)   { empty.push_back(name + ":spline"); }
    }
    std::vector<std::string> pinned;
    pinned.push_back("TurnRate+:ack");                                              // client writer, not liftable
    pinned.push_back("PitchRate+:ack");  pinned.push_back("PitchRate+:observer");   // ack: client writer; observer:
                                                                                     // its reader is BLOCKED (Task 6)
    pinned.push_back("CanTransitionSwimFly+:spline"); pinned.push_back("CanTransitionSwimFly-:spline");
    pinned.push_back("CollisionHeight+:spline"); pinned.push_back("KnockBack+:spline"); pinned.push_back("Teleport+:spline");
    const char* serverOnly[] = { "Gait+", "Gait-", "Swim+", "Swim-" };
    for (size_t i = 0; i < 4; ++i)
    {
        pinned.push_back(std::string(serverOnly[i]) + ":mover");
        pinned.push_back(std::string(serverOnly[i]) + ":ack");
        pinned.push_back(std::string(serverOnly[i]) + ":observer");
    }
    CHECK_STR(Joined(empty), Joined(pinned));
}

TEST(MotionMatrix_speed_rows_follow_the_reference_table_in_move_type_order)
{
    // CPP MovementPacketSender.cpp:29-40: {spline, mover, observer} per UnitMoveType. CPP's
    // own table has 0 for both rate observers -- neither is in its source at all -- but
    // Task 6 lifted TurnRate's from the client's own reader (PitchRate's is BLOCKED; see
    // gen_movement_layouts.py's ADDED comment), so that cell no longer follows CPP here.
    const uint16 reference[9][3] =
    {
        { SMSG_SPLINE_MOVE_SET_WALK_SPEED,        SMSG_MOVE_SET_WALK_SPEED,        SMSG_MOVE_UPDATE_WALK_SPEED },
        { SMSG_SPLINE_MOVE_SET_RUN_SPEED,         SMSG_MOVE_SET_RUN_SPEED,         SMSG_MOVE_UPDATE_RUN_SPEED },
        { SMSG_SPLINE_MOVE_SET_RUN_BACK_SPEED,    SMSG_MOVE_SET_RUN_BACK_SPEED,    SMSG_MOVE_UPDATE_RUN_BACK_SPEED },
        { SMSG_SPLINE_MOVE_SET_SWIM_SPEED,        SMSG_MOVE_SET_SWIM_SPEED,        SMSG_MOVE_UPDATE_SWIM_SPEED },
        { SMSG_SPLINE_MOVE_SET_SWIM_BACK_SPEED,   SMSG_MOVE_SET_SWIM_BACK_SPEED,   SMSG_MOVE_UPDATE_SWIM_BACK_SPEED },
        { SMSG_SPLINE_MOVE_SET_TURN_RATE,         SMSG_MOVE_SET_TURN_RATE,         SMSG_MOVE_UPDATE_TURN_RATE },
        { SMSG_SPLINE_MOVE_SET_FLIGHT_SPEED,      SMSG_MOVE_SET_FLIGHT_SPEED,      SMSG_MOVE_UPDATE_FLIGHT_SPEED },
        { SMSG_SPLINE_MOVE_SET_FLIGHT_BACK_SPEED, SMSG_MOVE_SET_FLIGHT_BACK_SPEED, SMSG_MOVE_UPDATE_FLIGHT_BACK_SPEED },
        { SMSG_SPLINE_MOVE_SET_PITCH_RATE,        SMSG_MOVE_SET_PITCH_RATE,        0 },
    };
    for (uint8 mt = 0; mt < 9; ++mt)
    {
        MatrixRow const* row = RowFor(SpeedChangeType(mt), true);
        REQUIRE(row != NULL);
        CHECK_EQ(row->spline, reference[mt][0]);
        CHECK_EQ(row->mover, reference[mt][1]);
        CHECK_EQ(row->observer, reference[mt][2]);
        CHECK_EQ(SpeedIndex(row->type), int(mt));
    }
    CHECK(IsSpeed(ChangeType::PitchRate));
    CHECK(!IsSpeed(ChangeType::Root));
    CHECK_EQ(SpeedIndex(ChangeType::Root), -1);
    CHECK(SpeedChangeType(9) == ChangeType::None);
    CHECK_STR(ChangeName(ChangeType::KnockBack), "KnockBack");
}

TEST(MotionMatrix_flag_value_and_event_rows_follow_the_writers_and_handlers)
{
    // A second, independent transcription of the twenty-one non-speed rows, deliberately
    // not read from PacketMatrix.cpp: a swapped cell there would pass the test above (it
    // only asks Wire::IsKnown) and would pass this one too if it copied the same table.
    //
    // mover: this tree's writers -- Unit.cpp BuildForceMoveRootPacket/BuildMoveSetCanFlyPacket/
    //   BuildMoveWaterWalkPacket/BuildMoveFeatherFallPacket/BuildMoveHoverPacket (:7294-7408),
    //   Unit::SendCollisionHeightUpdate (:7433), WorldSession::SendKnockBack
    //   (MovementHandler.cpp:681), Player::SendTeleportPacket (Player.cpp:1625); and, for the
    //   two rows this tree has no writer for, CPP MovementPacketSender.cpp:196 (gravity: apply
    //   -> SMSG_MOVE_GRAVITY_DISABLE, unapply -> SMSG_MOVE_GRAVITY_ENABLE -- the tree's own
    //   BuildMoveLevitatePacket sends the opposite pair, which is WriterShadow's KnownDifferent
    //   case) and :223 (can-transition).
    // ack: the registry's ack rows, wire/MovementLayouts.inc (MAP(CMSG_..._ACK, ...)).
    // observer: root rebroadcasts as itself (CPP Unit.cpp:11210); the other six flag families
    //   answer with the generic SMSG_MOVE_UPDATE, SMSG_PLAYER_MOVE (CPP
    //   MovementPacketSender.cpp:240); collision/knock-back/teleport each have their own
    //   SMSG_MOVE_UPDATE_* (CPP Handlers/MovementHandler.cpp:253 for teleport's).
    // spline: this tree's creature senders, CreatureMovement.cpp SetRoot/SetCanFly/SetWaterWalk/
    //   SetFeatherFall/SetHover/SetLevitate/SetWalk/SetSwim (:55-421); 0 where creatures have no
    //   packet for the change (can-transition, collision, knock-back, teleport), and 0 for the
    //   mover/ack/observer of Gait/Swim, which are server-driven only.
    struct ReferenceRow
    {
        ChangeType type;
        bool       apply;
        uint16     mover;
        uint16     ack;
        uint16     observer;
        uint16     spline;
    };
    const ReferenceRow reference[] =
    {
        { ChangeType::Root,            true,  SMSG_FORCE_MOVE_ROOT,     CMSG_FORCE_MOVE_ROOT_ACK,     SMSG_FORCE_MOVE_ROOT,     SMSG_SPLINE_MOVE_ROOT },
        { ChangeType::Root,            false, SMSG_FORCE_MOVE_UNROOT,   CMSG_FORCE_MOVE_UNROOT_ACK,   SMSG_FORCE_MOVE_UNROOT,   SMSG_SPLINE_MOVE_UNROOT },

        { ChangeType::CanFly,          true,  SMSG_MOVE_SET_CAN_FLY,    CMSG_MOVE_SET_CAN_FLY_ACK,    SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_SET_FLYING },
        { ChangeType::CanFly,          false, SMSG_MOVE_UNSET_CAN_FLY,  CMSG_MOVE_SET_CAN_FLY_ACK,    SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_UNSET_FLYING },

        { ChangeType::WaterWalk,       true,  SMSG_MOVE_WATER_WALK,     CMSG_MOVE_WATER_WALK_ACK,     SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_WATER_WALK },
        { ChangeType::WaterWalk,       false, SMSG_MOVE_LAND_WALK,      CMSG_MOVE_WATER_WALK_ACK,     SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_LAND_WALK },

        { ChangeType::FeatherFall,     true,  SMSG_MOVE_FEATHER_FALL,   CMSG_MOVE_FEATHER_FALL_ACK,   SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_FEATHER_FALL },
        { ChangeType::FeatherFall,     false, SMSG_MOVE_NORMAL_FALL,    CMSG_MOVE_FEATHER_FALL_ACK,   SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_NORMAL_FALL },

        { ChangeType::Hover,           true,  SMSG_MOVE_SET_HOVER,      CMSG_MOVE_HOVER_ACK,          SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_SET_HOVER },
        { ChangeType::Hover,           false, SMSG_MOVE_UNSET_HOVER,    CMSG_MOVE_HOVER_ACK,          SMSG_PLAYER_MOVE,         SMSG_SPLINE_MOVE_UNSET_HOVER },

        // The mover cells here are CPP's, not this tree's BuildMoveLevitatePacket, which
        // sends the opposite pair (WriterShadow_known_different_rows_count_apart).
        { ChangeType::GravityDisabled, true,  SMSG_MOVE_GRAVITY_DISABLE, CMSG_MOVE_GRAVITY_DISABLE_ACK, SMSG_PLAYER_MOVE,       SMSG_SPLINE_MOVE_GRAVITY_DISABLE },
        { ChangeType::GravityDisabled, false, SMSG_MOVE_GRAVITY_ENABLE,  CMSG_MOVE_GRAVITY_ENABLE_ACK,  SMSG_PLAYER_MOVE,       SMSG_SPLINE_MOVE_GRAVITY_ENABLE },

        // No writer in this tree for either cell: both are CPP's (:223).
        { ChangeType::CanTransitionSwimFly, true,  SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY,   CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, SMSG_PLAYER_MOVE, 0 },
        { ChangeType::CanTransitionSwimFly, false, SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY, CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK, SMSG_PLAYER_MOVE, 0 },

        { ChangeType::CollisionHeight,  true, SMSG_MOVE_SET_COLLISION_HGT, CMSG_MOVE_SET_COLLISION_HGT_ACK, SMSG_MOVE_UPDATE_COLLISION_HEIGHT, 0 },
        { ChangeType::KnockBack,        true, SMSG_MOVE_KNOCK_BACK,        CMSG_MOVE_KNOCK_BACK_ACK,        SMSG_MOVE_UPDATE_KNOCK_BACK,       0 },
        { ChangeType::Teleport,         true, SMSG_MOVE_TELEPORT,          CMSG_MOVE_TELEPORT_ACK,          SMSG_MOVE_UPDATE_TELEPORT,         0 },

        { ChangeType::Gait,             true,  0, 0, 0, SMSG_SPLINE_MOVE_SET_WALK_MODE },
        { ChangeType::Gait,             false, 0, 0, 0, SMSG_SPLINE_MOVE_SET_RUN_MODE },
        { ChangeType::Swim,             true,  0, 0, 0, SMSG_SPLINE_MOVE_START_SWIM },
        { ChangeType::Swim,             false, 0, 0, 0, SMSG_SPLINE_MOVE_STOP_SWIM },
    };
    for (size_t i = 0; i < sizeof(reference) / sizeof(reference[0]); ++i)
    {
        ReferenceRow const& expect = reference[i];
        MatrixRow const* row = RowFor(expect.type, expect.apply);
        REQUIRE(row != NULL);
        CHECK_EQ(row->mover, expect.mover);
        CHECK_EQ(row->ack, expect.ack);
        CHECK_EQ(row->observer, expect.observer);
        CHECK_EQ(row->spline, expect.spline);
    }
}

TEST(MotionMatrix_mover_and_spline_opcodes_are_unique_and_found_back)
{
    for (size_t i = 0; i < MatrixSize(); ++i)
    {
        MatrixRow const& row = MatrixRowAt(i);
        bool isSpline = true;
        if (row.mover)
        {
            MatrixRow const* found = RowForOpcode(row.mover, &isSpline);
            CHECK(found == &row);
            CHECK(!isSpline);
        }
        if (row.spline)
        {
            MatrixRow const* found = RowForOpcode(row.spline, &isSpline);
            CHECK(found == &row);
            CHECK(isSpline);
        }
    }
    CHECK(RowForOpcode(0, NULL) == NULL);
    CHECK(RowForOpcode(SMSG_MOVE_UPDATE_RUN_SPEED, NULL) == NULL);   // an observer cell is not a writer's opcode
    CHECK(RowFor(ChangeType::None, true) == NULL);
    CHECK(RowFor(ChangeType::Count, true) == NULL);
    // Rows without an apply pair answer either apply.
    CHECK(RowFor(ChangeType::RunSpeed, false) == RowFor(ChangeType::RunSpeed, true));
    CHECK(RowFor(ChangeType::Root, false) != RowFor(ChangeType::Root, true));
}
