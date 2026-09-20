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

#pragma once

#include <string>

/**
 * @file KeyMint.h
 * @brief Minting the keypair that admits a client to the second world stream.
 *
 * A 4.3.4 client will not open its second world connection just because it is
 * asked to. It verifies the instruction against an RSA public key compiled into
 * its own binary, and nobody outside Blizzard holds the private half of the
 * shipped one. So a server that wants the second stream substitutes a key it
 * owns: the public half is patched into every client, the private half stays
 * here.
 *
 * This used to live inside secret-gen's main(). It is a library now because the
 * baker mints the same pair from its "Patch client" stage, and two
 * implementations of one keypair is how a realm ends up with a client patched
 * from one generation and a server running another -- which is a login that
 * hangs at the loading screen with nothing in any log to say why.
 */
namespace mangos::keys
{
    /// Written into the key directory, and named the same by both front ends.
    extern const char* const kClientSecretName;   ///< "client.secret"
    extern const char* const kServerSecretName;   ///< "server.secret"

    struct MintOutcome
    {
        bool ok = false;
        /// False when a usable pair was already there and was kept.
        bool generated = false;
        std::string clientSecret;   ///< full path, valid when ok
        std::string serverSecret;   ///< full path, valid when ok
        std::string error;
    };

    /**
     * @brief Make sure @p dir holds one consistent keypair, minting it if not.
     *
     * An existing pair is KEPT, and that is the important half of the contract:
     * every client already patched carries the public half of whatever is on
     * disk, so replacing it silently would lock all of them out of the realm
     * with the only key that admits them gone. Re-running the baker therefore
     * costs nothing and changes nothing.
     *
     * Half a pair -- one file present, the other missing -- is refused rather
     * than completed, because the missing half cannot be derived from the one
     * that is there and inventing a new one produces exactly the mismatch this
     * whole file exists to prevent.
     *
     * @param dir    directory to hold both files; created if absent.
     * @param force  mint a new pair even over an existing one. Invalidates
     *               every client patched with the previous key.
     */
    MintOutcome EnsureKeypair(const std::string& dir, bool force);
}
