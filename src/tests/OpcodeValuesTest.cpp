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

// Opcode values, pinned to the numbers the 4.3.4 client actually uses.
//
// These are literals on purpose. The enum is the thing under test, so deriving
// the expected value from the enum would assert nothing. Each number below was
// taken from two independent sources that agree and neither of which annotates
// doubt: the dispatch table reversed from Wow-64.exe, and WowPacketParser's
// direction-keyed 4.3.4 table. See src/tests/opcode_denylist.txt for the
// parallel list of names still carrying a wrong value by the same method.

#include "TestHarness.h"

#include "Opcodes.h"

TEST(OpcodeValues_calendar_requests_match_the_client)
{
    // Eleven handlers were registered against values the client never sends, so
    // every calendar operation was rejected as an unknown opcode before it
    // reached any handler. The calendar LIST worked throughout, because
    // CMSG_CALENDAR_GET_CALENDAR (0x2814) was already correct -- which is why
    // this looked like a partly-working feature rather than a dead one.
    CHECK_EQ(int(CMSG_CALENDAR_ARENA_TEAM),            0x0204);
    CHECK_EQ(int(CMSG_CALENDAR_COPY_EVENT),            0x0207);
    CHECK_EQ(int(CMSG_CALENDAR_EVENT_RSVP),            0x0227);
    CHECK_EQ(int(CMSG_CALENDAR_UPDATE_EVENT),          0x2114);
    CHECK_EQ(int(CMSG_CALENDAR_EVENT_STATUS),          0x2D24);
    CHECK_EQ(int(CMSG_CALENDAR_EVENT_REMOVE_INVITE),   0x4337);
    CHECK_EQ(int(CMSG_CALENDAR_GUILD_FILTER),          0x4A16);
    CHECK_EQ(int(CMSG_CALENDAR_COMPLAIN),              0x4C36);
    CHECK_EQ(int(CMSG_CALENDAR_GET_EVENT),             0x6416);
    CHECK_EQ(int(CMSG_CALENDAR_REMOVE_EVENT),          0x6636);
    CHECK_EQ(int(CMSG_CALENDAR_EVENT_MODERATOR_STATUS), 0x6B35);

    // A twelfth dead handler: missed by the original name-keyed sweep because
    // the client's slot table spells this SIGN_UP (OpcodeSlots.inc) while the
    // enum here spells it SIGNUP, so a name match between the two never fired.
    CHECK_EQ(int(CMSG_CALENDAR_EVENT_SIGNUP),          0x6606);

    // Already correct before this campaign. Pinned so a future "tidy-up" of the
    // calendar block cannot renumber the one part that works.
    CHECK_EQ(int(CMSG_CALENDAR_GET_CALENDAR),          0x2814);
}

TEST(OpcodeValues_calendar_replies_match_the_client)
{
    // Thirteen replies were registered against values the client's dispatcher never routes to
    // a calendar reader, so every one of these packets was silently discarded on arrival. Traced
    // each reader directly out of the client's own decompiled parser (task-4-traces.md) rather
    // than trusting a single source; every value below is corroborated by two independent
    // sources that agree.
    CHECK_EQ(int(SMSG_CALENDAR_SEND_EVENT),                    0x0C35);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_INVITE),                  0x4E16);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_INVITE_REMOVED),          0x0725);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_STATUS),                  0x2A27);
    CHECK_EQ(int(SMSG_CALENDAR_COMMAND_RESULT),                0x6F36);
    CHECK_EQ(int(SMSG_CALENDAR_RAID_LOCKOUT_ADDED),            0x2305);
    CHECK_EQ(int(SMSG_CALENDAR_RAID_LOCKOUT_REMOVED),          0x2E25);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_INVITE_ALERT),            0x2A05);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_INVITE_REMOVED_ALERT),    0x2617);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_REMOVED_ALERT),           0x6D35);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_UPDATED_ALERT),           0x0907);
    CHECK_EQ(int(SMSG_CALENDAR_EVENT_MODERATOR_STATUS_ALERT),  0x6B06);
    CHECK_EQ(int(SMSG_CALENDAR_CLEAR_PENDING_ACTION),          0x2106);

    // Already correct before this campaign. Pinned so a future "tidy-up" of the calendar block
    // cannot renumber the two parts that work.
    CHECK_EQ(int(SMSG_CALENDAR_SEND_CALENDAR),                 0x6805);
    CHECK_EQ(int(SMSG_CALENDAR_SEND_NUM_PENDING),              0x0C17);

    // The mass-invite pair, adopted from TrinityCore rather than proved.
    //
    // These two are the one place in this campaign where the client could not
    // settle the question. Both values reach the SAME reader (sub_1407AE660 --
    // 0x4A26 jumps into the 0x0615 case via LABEL_29), and our two builders
    // write an identical wire shape: uint32 count, then N x (packed GUID,
    // uint8). Nothing about the payload distinguishes a guild roster from an
    // arena roster, so the trace that settled the other thirteen is silent here.
    //
    // WowPacketParser annotates both as "may be swapped with" the other, which
    // is agreement that it does not know. TrinityCore ships this mapping and is
    // in production, which is corroboration rather than proof.
    //
    // Adopted deliberately, because leaving them wrong guaranteed mass invite
    // stayed broken, while a wrong guess here cannot desync or freeze anything
    // -- the shapes are identical, so the worst case is one roster populating
    // the wrong panel, which is visible in seconds and fixed by swapping these
    // two numbers.
    //
    // SETTLED ON THE WIRE, 2026-08-31. Guild mass invite was driven from a live
    // client: SMSG_CALENDAR_FILTER_GUILD went out at 0x4A26 and the client
    // populated the roster from it. That confirms the guild half by observation,
    // and the arena half follows by elimination -- the pair has only two
    // possible arrangements, so fixing one fixes the other. These are no longer
    // adopted values; they are measured ones.
    CHECK_EQ(int(SMSG_CALENDAR_FILTER_GUILD),                  0x4A26);
    CHECK_EQ(int(SMSG_CALENDAR_ARENA_TEAM),                    0x0615);
}

TEST(OpcodeValues_player_feedback_replies_match_the_client)
{
    // Five names off the denylist. Each value is corroborated the usual two
    // ways -- the dispatch table reversed from Wow-64.exe and WowPacketParser's
    // direction-keyed table -- and each was then traced into the client's own
    // decompiled reader, which is what settled the payloads the senders write.
    //
    // SMSG_SET_FACTION_NOT_VISIBLE was not merely wrong, it was 0x0000, and
    // OpcodeTable.cpp registered it: value zero was a live server-side row.
    CHECK_EQ(int(SMSG_TALENTS_INVOLUNTARILY_RESET),     0x2C27);
    CHECK_EQ(int(SMSG_SET_FACTION_ATWAR),               0x4216);
    CHECK_EQ(int(SMSG_SET_FACTION_NOT_VISIBLE),         0x6737);
    CHECK_EQ(int(SMSG_QUEST_FORCE_REMOVED),             0x6605);
    CHECK_EQ(int(SMSG_BINDZONEREPLY),                   0x4C34);

    // The already-correct half of the pair sub_14072FFF0 serves. That one
    // reader branches on the opcode itself -- 0x2525 sets the visible bit,
    // 0x6737 clears it -- so the value below is what corroborates the value
    // above: they are two arms of a single client function, and this one has
    // been right, and in use, all along.
    CHECK_EQ(int(SMSG_SET_FACTION_VISIBLE),             0x2525);
}

TEST(OpcodeValues_spell_resist_matches_the_client)
{
    // Corroborated three ways, which is one more than the rule requires: the
    // dispatch table reversed from Wow-64.exe, WowPacketParser's direction-keyed
    // table, and TrinityCore's own enum -- all three say 0x0426.
    //
    // The payload agrees across two of those independently. The client's reader
    // takes two flat eight-byte GUIDs (sub_1405A7D80 reads 8 bytes; these are
    // NOT packed), a uint32 spell id and a trailing byte it discards.
    // TrinityCore sizes the same packet 8+8+4+1 and calls the byte a log-format
    // selector. Nothing had to be guessed.
    CHECK_EQ(int(SMSG_PROCRESIST), 0x0426);

    // Not to be confused with the miss log, which reports a spell that never
    // landed. That one has always been correct and in use; pinned so the two
    // cannot be conflated by a later tidy-up.
    CHECK_EQ(int(SMSG_SPELLLOGMISS), 0x0625);
}

TEST(OpcodeValues_movement_changes_match_the_client)
{
    // Two client-binary sources per value, as the file header requires. For an
    // SMSG: the dispatch table (client_opcode_table_434.tsv; the reader address
    // is quoted so the row can be found again). For a CMSG: the client's own send
    // router, in tree as src/proto/OpcodeSlots.inc, which lists exactly what the
    // client can transmit. The Cataclysm Preservation Project's enum agrees with
    // every value below; WowPacketParser's 4.3.4 table is where that enum came from.
    //
    // Six speed-change acks carried WotLK values that no 4.3.4 source lists and
    // the send router does not know, so the client's real ack arrived as an
    // unknown opcode. Their handler rows in OpcodeTable.cpp are commented out and
    // stay so: binding them is P2's, with the handlers rewritten for the 4.3.4
    // layout (today's read a WotLK packed guid).
    CHECK_EQ(int(CMSG_FORCE_WALK_SPEED_CHANGE_ACK),        0x7210);   // SLOT(0x7210, 1, 0)
    CHECK_EQ(int(CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK),    0x3216);   // SLOT(0x3216, 1, 0)
    CHECK_EQ(int(CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK),   0x7A16);   // SLOT(0x7A16, 1, 0)
    CHECK_EQ(int(CMSG_FORCE_TURN_RATE_CHANGE_ACK),         0x7316);   // SLOT(0x7316, 1, 0)
    CHECK_EQ(int(CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK), 0x310E);   // SLOT(0x310E, 1, 0)
    CHECK_EQ(int(CMSG_FORCE_PITCH_RATE_CHANGE_ACK),        0x3100);   // SLOT(0x3100, 1, 0)
    // The three the login already exercises, pinned so the family reads as one.
    CHECK_EQ(int(CMSG_FORCE_RUN_SPEED_CHANGE_ACK),         0x7818);   // SLOT(0x7818, 1, 0)
    CHECK_EQ(int(CMSG_FORCE_SWIM_SPEED_CHANGE_ACK),        0x7A10);   // SLOT(0x7A10, 1, 0)
    CHECK_EQ(int(CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK),      0x7314);   // SLOT(0x7314, 1, 0)

    // Off the denylist: the swim/fly transition pair and its ack.
    CHECK_EQ(int(SMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY),     0x59A2);   // reader sub_1403814B0
    CHECK_EQ(int(SMSG_MOVE_UNSET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY),   0x7DB2);   // reader sub_140382D00
    CHECK_EQ(int(CMSG_MOVE_SET_CAN_TRANSITION_BETWEEN_SWIM_AND_FLY_ACK), 0x3014);   // SLOT(0x3014, 1, 0)

    // Off the denylist: the flight spline sync, at the client's dispatch value (the taxi retail
    // notes, item 15: reader sub_14021FA60; OpcodeSlots.inc carries the same slot). The sender
    // stays off; the tree carried 0x1389 with no verification.
    CHECK_EQ(int(SMSG_FLIGHT_SPLINE_SYNC),            0x0924);

    // The observer-side speed updates this server has never sent (§7's per-change
    // matrix names them), the collision-height update, and the mover handover.
    // 0x14A6 was carried as CMSG_QUERY_VEHICLE_STATUS, a name no 4.3.4 source
    // has; the client dispatches that value as the run-speed update.
    CHECK_EQ(int(SMSG_MOVE_UPDATE_WALK_SPEED),        0x54A2);   // reader sub_1403994F0
    CHECK_EQ(int(SMSG_MOVE_UPDATE_RUN_SPEED),         0x14A6);   // reader sub_140394F20
    CHECK_EQ(int(SMSG_MOVE_UPDATE_RUN_BACK_SPEED),    0x3DA6);   // reader sub_140397B80
    CHECK_EQ(int(SMSG_MOVE_UPDATE_SWIM_SPEED),        0x59B5);   // reader sub_140393A10
    CHECK_EQ(int(SMSG_MOVE_UPDATE_SWIM_BACK_SPEED),   0x30B5);   // reader sub_140390F70
    CHECK_EQ(int(SMSG_MOVE_UPDATE_FLIGHT_SPEED),      0x30B1);   // reader sub_140396770
    CHECK_EQ(int(SMSG_MOVE_UPDATE_FLIGHT_BACK_SPEED), 0x74A0);   // reader sub_140391E10
    CHECK_EQ(int(SMSG_MOVE_UPDATE_COLLISION_HEIGHT),  0x59A3);   // reader sub_1403990B0
    CHECK_EQ(int(SMSG_MOVE_SET_ACTIVE_MOVER),         0x11B3);   // reader sub_14037F8D0
}

TEST(OpcodeValues_movement_updates_lifted_in_p2a)
{
    // Three more observer updates the matrix had no opcode name for at all: the client
    // dispatches them, but the reference project has neither table nor opcode case for
    // them (gen_movement_layouts.py's ADDED). Two sources per value, both client-binary
    // and both quoted for traceability: the dispatch table's own reader column
    // (client_opcode_table_434.tsv; a thin constructor -- P1-C's rule) and the field
    // reader lift_client_reader.py actually lifted, which that constructor's one call
    // resolves to.
    CHECK_EQ(int(SMSG_MOVE_UPDATE_TELEPORT),  0x50B2);   // dispatch reader sub_1403975E0; field reader sub_140384210
    CHECK_EQ(int(SMSG_MOVE_UPDATE_TURN_RATE), 0x5DA1);   // dispatch reader sub_140397590; field reader sub_14038C780
    // PitchRate's value is pinned the same way, but its lift is BLOCKED: sub_14037B330
    // takes a gate bit as `(unsigned __int8)~v85 >> 7`, a bitwise-complement the lifter's
    // shift-register tracker does not parse. No registry row for it (see MovementLayouts.inc).
    CHECK_EQ(int(SMSG_MOVE_UPDATE_PITCH_RATE), 0x1DB5);  // dispatch reader sub_140383A60; field reader sub_14037B330 (BLOCKED)
}
