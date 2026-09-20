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

#include "BareMap.h"

// The Map constructor used to decide the harness's bare map by comparing
// Movement.HarnessBareMap against its own id and nothing else:
//
//     m_bare(InstanceId == 0 && sWorld.getConfig(CONFIG_UINT32_MOVEMENT_HARNESS_BARE_MAP) == id)
//
// The option's default is 0 and its documented meaning has always been "none" -- but map 0 is
// Eastern Kingdoms. Every server that never set the option therefore ran with the whole
// continent stripped of creature, gameobject and corpse spawns, which is how the live test of
// 2026-09-20 found Stormwind and Ironforge deserted while Darnassus and Orgrimmar (map 1) were
// full. The packet log settled it before the code did: 0 bytes of SMSG_UPDATE_OBJECT in
// Ironforge and 3.4 KB in Stormwind against 227 KB in Feralas and 131 KB in Orgrimmar.
//
// Transports.cpp had read the same option correctly all along (`if (bareMap && ...)`), so the
// two call sites disagreed about what 0 meant. Both now go through MapIsBare/BareMapConfigured,
// and these cases pin the meaning of 0 so it cannot drift back.

TEST(BareMapZeroIsOffNotEasternKingdoms)
{
    // The default configuration. Nothing may be bare -- least of all map 0.
    CHECK(!MapIsBare(0, 0, 0));      // Eastern Kingdoms on a stock server
    CHECK(!MapIsBare(0, 1, 0));      // Kalimdor
    CHECK(!MapIsBare(0, 530, 0));    // Outland
    CHECK(!MapIsBare(0, 571, 0));    // Northrend
    CHECK(!BareMapConfigured(0));
}

TEST(BareMapNamesExactlyOneMap)
{
    // What the harness's own launcher sets: map 1 and only map 1.
    CHECK(BareMapConfigured(1));
    CHECK(MapIsBare(1, 1, 0));
    CHECK(!MapIsBare(1, 0, 0));
    CHECK(!MapIsBare(1, 530, 0));

    // And any other map id the option might name.
    CHECK(MapIsBare(571, 571, 0));
    CHECK(!MapIsBare(571, 1, 0));
}

TEST(BareMapNeverAppliesToAnInstance)
{
    // Only the continent-level map is ever blanked: an instance of the same id carries its own
    // spawns, as it did before the harness existed.
    CHECK(MapIsBare(1, 1, 0));
    CHECK(!MapIsBare(1, 1, 1));
    CHECK(!MapIsBare(1, 1, 4271));
}
