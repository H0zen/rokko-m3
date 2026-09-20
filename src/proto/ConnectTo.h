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

#include "Auth/BigNumber.h"
#include "Crypto/Rsa.h"
#include "LinkSlot.h"
#include "Platform/Define.h"
#include "WorldPacket.h"

#include <array>
#include <string>
#include <vector>

namespace proto
{
    /// Address families the client's redirect deserialiser understands.
    enum class RedirectFamily : uint8
    {
        IPv4 = 1,
        IPv6 = 2
    };

    /**
     * @brief Builds the signed SMSG_CONNECT_TO that opens the second stream.
     *
     * The client will not open stream 1 for an unsigned redirect. It holds an
     * RSA-2048 public key and runs a raw public operation on the packet's
     * 256-byte field, P = C^e mod n, then checks the recovered plaintext's
     * internal structure against constants baked into the image. Only the holder
     * of d can produce a C that recovers to a structure that passes -- which is
     * the whole point of the mechanism, and the reason the modulus in the client
     * has to be replaced with one whose private half we hold. A client running
     * the stock modulus refuses every redirect we can sign, and then holds all
     * 729 of its stream-1 opcodes forever.
     *
     * What the recovered plaintext has to contain is not a format we chose:
     *
     *   - a 142-byte block of the digits of pi in BCD, which the client
     *     regenerates from an internal spigot and compares byte for byte;
     *   - a control byte of 0x2A;
     *   - the destination address, its family, and the port, in a fixed layout;
     *   - two HMAC-SHA1 tags over that layout, keyed with a 64-byte constant.
     *
     * The address is the one field that is easy to get wrong in a way that looks
     * like a signing bug: it is the destination the CLIENT must connect to --
     * this server's own advertised address -- and never the client's own address.
     * Writing the peer address here makes the client connect to itself, and the
     * symptom is an accept on :8086 that never happens.
     */
    class RedirectSigner
    {
        public:

            RedirectSigner();

            /**
             * @brief Install the keypair from a server.secret written by 'mep mint'.
             *
             * The file holds `Modulus`, `PrivateExponent` and `AuthBlob` as hex,
             * one per line. It exists as a file rather than as configuration
             * because its three values are only meaningful together and only
             * alongside the client.secret minted with them: transcribing them
             * one at a time into mangosd.conf is an invitation to mix two
             * generations, which produces a realm where every login hangs with
             * nothing in any log to say why.
             *
             * @return false if the file is missing, malformed, or holds a
             *         keypair that cannot sign a redirect this client accepts.
             */
            bool LoadFromFile(const std::string& path);

            /**
             * @brief Install the keypair directly.
             *
             * @param modulusHex          n, big-endian hex. Must match the modulus
             *                            patched into every client that connects.
             * @param privateExponentHex  d, big-endian hex. Secret.
             * @param authBlobHex         73-byte deployment blob, hex. Empty
             *                            means 73 zero bytes.
             * @return false if a field is malformed or the wrong length.
             */
            bool Load(const std::string& modulusHex,
                      const std::string& privateExponentHex,
                      const std::string& authBlobHex);

            /**
             * @brief Install the keypair with its CRT parameters (PKCS#1 names).
             *
             * With the five present the server signs each redirect with two
             * half-size exponentiations (about four times faster); all five empty
             * means the plain path. Either way a probe is signed and recovered
             * before the key is accepted, so a wrong Coefficient is refused the
             * same way as a wrong PrivateExponent.
             */
            bool Load(const std::string& modulusHex,
                      const std::string& privateExponentHex,
                      const std::string& authBlobHex,
                      const std::string& prime1Hex,
                      const std::string& prime2Hex,
                      const std::string& exponent1Hex,
                      const std::string& exponent2Hex,
                      const std::string& coefficientHex);

            bool IsLoaded() const { return m_loaded; }

            /// Whether the loaded key carries its primes (the fast path).
            bool HasCrtParameters() const { return m_loaded && m_key.HasCrt(); }

            /**
             * @brief The 20-byte reference digest this auth blob requires.
             *
             * The client compares its baked-in digest against an HMAC over the
             * auth blob, so the two have to be patched as a pair. Printing this at
             * startup is what lets an operator patch a client to match the running
             * configuration instead of guessing.
             */
            std::string ExpectedClientDigest() const;

            /**
             * @brief Build the SMSG_CONNECT_TO payload for one redirect.
             *
             * @param address Destination address bytes in network order: 4 for
             *                IPv4, 16 for IPv6. This is where the client will
             *                connect, so it must be reachable from the client.
             * @param family  Which of the two the bytes are.
             * @param port    Destination TCP port.
             * @param target  Which logical stream is being opened. The client
             *                hard-refuses any value but Zero or One.
             * @param out     Receives the finished packet.
             * @return false if unloaded, or if the plaintext does not fit under n.
             */
            bool BuildConnectTo(const std::vector<uint8>& address,
                                RedirectFamily family,
                                uint16 port,
                                LinkSlot target,
                                WorldPacket& out) const;

            /// The 258-byte struct the client reconstructs from the plaintext.
            typedef std::array<uint8, 258> Block;

            /// Serialise the struct into the 256-byte RSA plaintext, and back. Public
            /// so that a test can play the client: recover the field and reassemble.
            static std::array<uint8, 256> BlockToPlaintext(const Block& d);
            static Block PlaintextToBlock(const std::array<uint8, 256>& pt);

        private:

            /// The realm's key (src/shared/Crypto): raw RSA-2048, blinded, CRT when
            /// the file carries the primes. Not copyable -- neither is this class.
            MaNGOS::Crypto::RsaPrivateKey m_key;

            std::array<uint8, 73> m_authBlob;

            bool m_loaded;
    };
}
