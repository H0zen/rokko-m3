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

// One synthetic 4.3.4 client: both world streams, from the TCP connect to a
// character standing in the world.
//
// What it is for. This fork routes a session's traffic across two sockets, and
// today only 19 opcodes are gated onto the second one -- a measured 98.9 / 1.1
// split. Deciding what SHOULD go where needs a way to put a known number of real
// sessions on a server and watch what the split does to them, and no such thing
// exists: a WoW client cannot be scripted, and the tree's only socket test
// (NetworkStressTest) speaks bytes rather than the protocol. This is the client
// half of that instrument.
//
// It is NOT a bot. It logs in, holds the session, answers what must be answered,
// and reports where it got to. Nothing here plays the game.
//
// Why realmd is skipped. The login server exists to agree a session key with the
// client over SRP-6 and write it into `account.sessionkey`. A synthetic client
// can simply be TOLD that key: with the row already carrying it, CMSG_AUTH_SESSION
// proves possession exactly as a real client's would, over the same SHA-1 and the
// same secret. That removes an entire protocol and an entire server from the
// harness without weakening a single check the world server performs.

#include "Auth/AuthCrypt.h"
#include "Auth/BigNumber.h"
#include "Framing.hpp"
#include "Peer.hpp"
#include "wire/MovementStatus.h"
#include "Platform/Define.h"
#include "Socket.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace loadtest
{
    /// How far a client got. Ordered: each stage implies the ones before it.
    enum class Stage
    {
        Start,
        Connected0,          ///< TCP up on the first world port
        Challenged0,         ///< banner + SMSG_AUTH_CHALLENGE read
        Authenticated,       ///< SMSG_AUTH_RESPONSE said AUTH_OK
        CharListed,          ///< SMSG_CHAR_ENUM came back
        LoginSent,           ///< CMSG_PLAYER_LOGIN is with the server
        Redirected,          ///< SMSG_CONNECT_TO arrived on stream 0
        Connected1,          ///< TCP up on the second world port
        StreamLive,          ///< SMSG_RESUME_COMMS: the client owns stream 1
        InWorld              ///< SMSG_LOGIN_VERIFY_WORLD: the character is placed
    };

    const char* StageName(Stage stage);

    struct Config
    {
        std::string host       = "127.0.0.1";
        uint16      port       = 8085;
        uint16      streamPort = 8086;

        /// Upper case, as realmd stores it: the account name is hashed into both
        /// login proofs verbatim, so its case is part of the secret.
        std::string account;
        BigNumber   sessionKey;

        /// `characters.guid` of a character on that account. Phase 1 does not
        /// parse SMSG_CHAR_ENUM (4.3.4 packs it into a bit-stream that would be a
        /// project of its own); the guid is supplied and the enum is exchanged
        /// only to prove the round trip.
        uint64 characterGuid = 0;

        uint16 build = 15595;

        /// Seconds to hold the session after entering the world, pinging as a
        /// real client does. Zero logs in and leaves.
        uint32 holdSeconds = 0;

        bool verbose = false;

        /// What to do once in the world (movement P0-B). By default the session
        /// walks nothing and watches nobody, but it does what a real client does
        /// regardless: answers time-sync requests from its tick clock and acks any
        /// server-initiated movement change whose layout the registry knows.
        PeerScript script;

        /// Called once the session is in the world, before any hold. The CLI's
        /// paired run uses it to log the walker in only once the observer is
        /// watching.
        std::function<void()> onInWorld;
    };

    /// What one run produced, whether or not it finished.
    struct Result
    {
        Stage       stage = Stage::Start;
        std::string error;

        /// Milliseconds from the first connect to each milestone.
        double msToAuth     = 0.0;
        double msToRedirect = 0.0;
        double msToStream1  = 0.0;
        double msToWorld    = 0.0;

        uint32 packetsOnStream0 = 0;
        uint32 packetsOnStream1 = 0;
        uint64 bytesOnStream0   = 0;
        uint64 bytesOnStream1   = 0;

        uint32 sentOnStream0 = 0;
        uint32 sentOnStream1 = 0;
        uint64 sentBytesOnStream0 = 0;
        uint64 sentBytesOnStream1 = 0;

        /// Where SMSG_LOGIN_VERIFY_WORLD put the character.
        uint32     mapId = 0;
        Wire::Vec4 worldPos;

        PeerReport peer;

        bool Reached(Stage wanted) const { return stage >= wanted; }
    };

    class SyntheticClient
    {
        public:

            explicit SyntheticClient(const Config& config);

            /// Runs the whole sequence. Never throws; failures land in Result.
            Result Run();

        private:

            /// One of the client's two sockets, with the cipher that keys it.
            struct Stream
            {
                Socket            socket;
                ServerFrameReader reader;
                AuthCrypt         crypt;
                bool              armed = false;

                std::vector<WorldPacket> inbox;
                uint32 packets = 0;
                uint64 bytes   = 0;

                uint32 sent      = 0;
                uint64 sentBytes = 0;
            };

            bool OpenStreamZero(std::string& error);
            bool Authenticate(std::string& error);
            bool ListCharacters(std::string& error);
            bool SendPlayerLogin(std::string& error);
            bool AwaitRedirect(std::string& error);
            bool OpenStreamOne(std::string& error);
            bool AwaitWorld(std::string& error);
            /// Holds the session for `holdSeconds`, answering what must be answered,
            /// walking if scripted, acking changes under the policy, and recording
            /// what it sees. Pings at the real client's cadence throughout.
            bool Serve(std::string& error);

            /// One inbound packet during Serve. False only on a send failure.
            /// `walker` is the mover this session reports: a teleport moves it.
            bool Dispatch(WorldPacket& packet, class AckEngine& acks, class Walker& walker,
                          uint32 nowTicks, std::string& error);

            /// Sends on `stream`, enciphered iff that stream has armed.
            bool Send(Stream& stream, const WorldPacket& packet, std::string& error);

            /// Which stream a post-login send belongs on, by the client's own
            /// send router (OpcodeSlots.inc, send_slot).
            Stream& StreamFor(uint16 opcode);

            /**
             * @brief Pump until `wanted` shows up on `stream`, or the deadline passes.
             *
             * @param found receives the packet, so its payload can be read
             * @return false on socket error, decode failure, or timeout
             */
            bool Await(Stream& stream, uint16 wanted, int timeoutMs,
                       WorldPacket& found, std::string& error);

            /// Reads whatever is available on both streams without waiting for any
            /// particular packet. Keeps the inboxes drained during a hold.
            bool Pump(int timeoutMs, std::string& error);

            bool Drain(Stream& stream, int timeoutMs, std::string& error);

            /// The 32 challenge bytes and the 4-byte nonce out of one
            /// SMSG_AUTH_CHALLENGE payload.
            static bool ReadChallenge(const WorldPacket& packet,
                                      uint8 (&seed)[AuthCrypt::SeedLength],
                                      uint32& nonce);

            /// Finds the status byte in whichever of SMSG_AUTH_RESPONSE's three
            /// shapes arrived. Its offset is not fixed; see the definition.
            static bool ReadAuthStatus(const WorldPacket& packet, uint8& status,
                                       std::string& error);

            double ElapsedMs() const;
            void   Trace(const char* what, ...) const;

            Config m_config;
            Stream m_stream0;
            Stream m_stream1;

            /// This connection's own SMSG_AUTH_CHALLENGE nonce, per stream. Both
            /// proofs are bound to the seed of the socket they are sent on.
            uint32 m_serverSeed0 = 0;
            uint32 m_serverSeed1 = 0;
            uint32 m_clientSeed  = 0;

            uint8 m_challengeSeed1[AuthCrypt::SeedLength] = {};

            std::chrono::steady_clock::time_point m_started;
            Result m_result;
    };
}
