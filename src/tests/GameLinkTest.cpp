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

/// Decoupling D1: the test binary links the game library. The proof is a Creature on the
/// stack -- the first object in src/game that any test could ever construct. It is never
/// Create()d, never added to a map, and must leave without reaching a singleton that needs
/// a loaded world. Whatever it does reach is named in the PR: that reach is the next seam.

#include "TestHarness.h"
#include "Creature.h"
#include "Database/DatabaseEnv.h"   // WorldDatabase, CharacterDatabase, LoginDatabase (F13)
#include "World.h"                  // realmID (F13)

TEST(GameLink_CreatureOnTheStackIsNotInWorld)
{
    Creature creature(CREATURE_SUBTYPE_GENERIC);
    CHECK(!creature.IsInWorld());
    CHECK_EQ(int(creature.GetTypeId()), int(TYPEID_UNIT));
    CHECK(creature.GetSubtype() == CREATURE_SUBTYPE_GENERIC);
    // Not asked: GetObjectGuid() reads through m_uint32Values (Object.h:278,377), NULL until
    // Create(); GetMap() asserts m_currMap (Object.h:770). A bare object has neither.
}

TEST(GameLink_TheGlobalsComeFromTheLibrary)
{
    // The four symbols mangosd used to define. Linking this file proves they are in libgame.
    CHECK(!WorldDatabase);
    CHECK(!CharacterDatabase);
    CHECK(!LoginDatabase);
    CHECK_EQ(realmID, 0u);
}
