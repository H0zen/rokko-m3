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
 * @file SecretGen.cpp
 * @brief Command line over mangos::keys::EnsureKeypair.
 *
 * The minting itself moved to KeyMint so the baker's "Patch client" stage could
 * do the same job from inside one window -- which is where an operator now does
 * all of this. This command remains for scripted deployments and for minting a
 * pair somewhere other than a bake's output directory.
 */

#include "KeyMint.h"

#include <cstdio>
#include <string>

namespace
{
    void Usage()
    {
        std::printf(
            "mep mint -- mint the keypair for the 4.3.4 second world stream\n"
            "\n"
            "  mep mint [--out-dir DIR] [--force]\n"
            "\n"
            "Writes two files:\n"
            "\n"
            "  client.secret   modulus + digest. Feed it to the client patcher; it\n"
            "                  ends up in every client binary. Not secret.\n"
            "\n"
            "  server.secret   the above plus the private exponent. Put it in\n"
            "                  <DataDir>/keys, where the server reads it from, and\n"
            "                  keep it readable only by the account the server runs\n"
            "                  as. Anyone holding it can redirect your players.\n"
            "\n"
            "Options:\n"
            "  --out-dir DIR   where to write both files (default: current directory)\n"
            "  --force         replace files that already exist\n"
            "  --help          this text\n"
            "\n"
            "An existing pair is kept as it is; nothing is rewritten without --force,\n"
            "because regenerating invalidates every client patched with the previous\n"
            "key.\n"
            "\n"
            "mep bake does all of this from its \"Patch client\" box, into\n"
            "<output>/keys, and patches the client in the same pass.\n");
    }
}

int MintMain(int argc, char** argv)
{
    std::string outDir;
    bool        force = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            Usage();
            return 0;
        }
        if (arg == "--force")
        {
            force = true;
            continue;
        }
        if (arg == "--out-dir")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "mep mint: --out-dir needs a directory\n");
                return 2;
            }
            outDir = argv[++i];
            continue;
        }

        std::fprintf(stderr, "mep mint: unknown argument '%s'\n", arg.c_str());
        Usage();
        return 2;
    }

    const mangos::keys::MintOutcome minted = mangos::keys::EnsureKeypair(outDir, force);
    if (!minted.ok)
    {
        std::fprintf(stderr, "mep mint: %s\n", minted.error.c_str());
        return 1;
    }

    if (!minted.generated)
    {
        std::printf("mep mint: %s and %s already exist and were left alone.\n",
                    minted.clientSecret.c_str(), minted.serverSecret.c_str());
        std::printf("Pass --force to mint a new pair -- every client patched with the\n"
                    "current key then stops being able to log in.\n");
        return 0;
    }

    std::printf("mep mint: wrote %s and %s\n", minted.clientSecret.c_str(),
                minted.serverSecret.c_str());
    std::printf("\n");
    std::printf("  server reads:  %s (from its DataDir/keys)\n", minted.serverSecret.c_str());
    std::printf("  client patch:  %s\n", minted.clientSecret.c_str());
    std::printf("\n");
    std::printf("Every client has to be patched from this client.secret before it can\n");
    std::printf("enter the world. Restrict server.secret to the server account.\n");

    return 0;
}
