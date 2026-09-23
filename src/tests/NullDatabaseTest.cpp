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

/// Decoupling D1: a Database that was never Initialize()d refuses instead of crashing.
/// Before this test, Query() on such a database indexed an empty connection vector
/// (Database.cpp:314) and locked the garbage it read (Database.h:185).

#include "TestHarness.h"
#include "Database/DatabaseEnv.h"

#include <string>

TEST(NullDatabase_IsFalseBeforeInitialize)
{
    DatabaseMysql db;
    CHECK(!db);
}

TEST(NullDatabase_QueryAnswersNull)
{
    DatabaseMysql db;
    CHECK(db.Query("SELECT 1") == NULL);
    CHECK(db.QueryNamed("SELECT 1") == NULL);
    CHECK(db.PQuery("SELECT %u", 1u) == NULL);
    CHECK(db.PQueryNamed("SELECT %u", 1u) == NULL);
}

TEST(NullDatabase_ExecuteAndAsyncAnswerFalse)
{
    DatabaseMysql db;
    CHECK(!db.Execute("SELECT 1"));
    CHECK(!db.PExecute("SELECT %u", 1u));
    CHECK(!db.AsyncQuery([](QueryResult*) {}, "SELECT 1"));
    CHECK(!db.AsyncPQuery([](QueryResult*) {}, "SELECT %u", 1u));
}

// Debate F6: two more public paths index the empty pool. escape_string locks
// m_pQueryConnections[0] for any non-empty string (Database.cpp:296); Ping locks m_pAsyncConn
// and then the pool by its default size of one (Database.cpp:322-328).
TEST(NullDatabase_EscapeLeavesTheStringUntouched)
{
    DatabaseMysql db;
    std::string value("it's");
    db.escape_string(value);
    CHECK_STR(value, "it's");
}

TEST(NullDatabase_PingReturns)
{
    DatabaseMysql db;
    db.Ping();
    CHECK(!db);
}
