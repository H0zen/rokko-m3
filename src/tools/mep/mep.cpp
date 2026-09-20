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

/**
 * @file mep.cpp
 * @brief mep -- MaNGOS extractor and patcher: one binary, three subcommands.
 *
 * They were three executables (mangos-extractor, mangos-patch, secret-gen) for
 * one workflow that always runs in one order: bake the client's data, mint the
 * realm keypair, write the public half into the client. Three binaries meant
 * three --help texts, three places to install, and an operator free to run last
 * week's patcher against this week's key.
 */

#include <cstdio>
#include <cstring>
#include <vector>

/// Extractor.cpp -- bakes a WoW client into the caches mangosd reads.
int BakeMain(int argc, char** argv);
/// patch/PatchMain.cpp -- writes a realm's public key into a client binary.
int PatchMain(int argc, char** argv);
/// MintMain.cpp -- mints the realm keypair the other two hand around.
int MintMain(int argc, char** argv);

namespace
{
    void Usage()
    {
        std::printf(
            "mep -- MaNGOS extractor and patcher\n"
            "\n"
            "  usage: mep <command> [option ...]\n"
            "\n"
            "  bake    read a 4.3.4 client and write the caches mangosd reads\n"
            "          (maps, DBC/DB2 stores, tiles, navmesh). This is what runs\n"
            "          when mep is started with no command at all.\n"
            "  mint    generate a realm keypair: server.secret and client.secret\n"
            "  patch   write a client.secret's public half into a Wow.exe\n"
            "\n"
            "  mep <command> --help  for that command's own options\n"
            "\n"
            "`bake` does all three from its \"Patch client\" box, which is the\n"
            "path that cannot mix a client with the wrong generation of key.\n");
    }
}

int main(int argc, char** argv)
{
    // A bare `mep`, or `mep -d /wow`, is the baker: it is what the window
    // launches and what an operator runs, and making them all type `bake` would
    // buy nothing. A command is only looked for in the first argument.
    if (argc < 2 || argv[1][0] == '-')
    {
        if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0))
        {
            Usage();
            return 0;
        }
        return BakeMain(argc, argv);
    }

    // The subcommand itself is dropped, but argv[0] is kept: the baker resolves
    // vessels.txt and offmesh.txt relative to its own executable.
    std::vector<char*> forwarded;
    forwarded.push_back(argv[0]);
    for (int i = 2; i < argc; ++i)
    {
        forwarded.push_back(argv[i]);
    }

    const int count = int(forwarded.size());
    char** args = forwarded.data();

    if (std::strcmp(argv[1], "bake") == 0)
    {
        return BakeMain(count, args);
    }
    // `patch` is BOTH the PE writer's name and one of the baker's components,
    // and the difference is the next word: the writer is only ever called as
    // `mep patch verify|apply|selftest`. Anything else -- `mep patch --src ...
    // --client ...`, which is what the window emits when only its "Patch client"
    // box is ticked -- is the baker, and it keeps the word, because dropping it
    // is what turned a patch-only run into a full bake.
    if (std::strcmp(argv[1], "patch") == 0)
    {
        const bool writerVerb = count > 1 &&
                                (std::strcmp(args[1], "verify") == 0 ||
                                 std::strcmp(args[1], "apply") == 0 ||
                                 std::strcmp(args[1], "selftest") == 0);
        if (writerVerb)
        {
            return PatchMain(count, args);
        }
        return BakeMain(argc, argv);
    }
    if (std::strcmp(argv[1], "mint") == 0)
    {
        return MintMain(count, args);
    }
    if (std::strcmp(argv[1], "help") == 0)
    {
        Usage();
        return 0;
    }

    // Not a command and not an option: a component name for the baker
    // (`mep maps`, `mep nav`), which is how the extractor has always been run.
    return BakeMain(argc, argv);
}
