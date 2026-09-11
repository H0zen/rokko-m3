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

#include "SyntheticClient.hpp"

#include "AckEngine.hpp"
#include "TimeSync.hpp"
#include "Walker.hpp"
#include "wire/KnockBackCodec.h"
#include "wire/MovementCodec.h"
#include "wire/MovementFamilies.h"
#include "wire/MovementSequences.h"
#include "wire/MoverCodec.h"
#include "wire/TeleportCodec.h"

#include "Auth/Sha1.h"
#include "LinkSlot.h"
#include "OpcodeSlots.h"
#include "Opcodes.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

namespace loadtest
{
    namespace
    {
        /// Long enough that a cold server loading a map does not read as a hang,
        /// short enough that a genuinely stuck handshake ends the run.
        const int STAGE_TIMEOUT_MS   = 30000;
        const int CONNECT_TIMEOUT_MS = 5000;
        const int POLL_MS            = 50;

        /// SMSG_AUTH_RESPONSE's success code (SharedDefines.h, AUTH_OK).
        const uint8 AUTH_OK          = 0x0C;
        const uint8 AUTH_WAIT_QUEUE  = 0x1B;

        const size_t AUTH_DIGEST_SIZE = 20;

        /// Payload length of CMSG_AUTH_CONTINUED_SESSION: the echoed connect-to
        /// key, the proof-of-work counter, and the digest. The server checks this
        /// exact size before it reads a byte.
        const size_t CONTINUED_SESSION_SIZE = 8 + 8 + AUTH_DIGEST_SIZE;

        /**
         * Where each digest byte sits in CMSG_AUTH_CONTINUED_SESSION.
         *
         * Wire truth, not a choice: the client interleaves the twenty bytes, and
         * this order was read off three live handshakes on 2026-08-30. It is the
         * same order ClientConnection::VerifyContinuedSession reads them back in,
         * and RedirectStreamTest pins it against a captured packet.
         */
        const uint8 CONTINUED_DIGEST_ORDER[AUTH_DIGEST_SIZE] =
        {
            5, 2, 6, 10, 8, 17, 11, 15, 7, 1, 4, 16, 0, 12, 14, 13, 18, 9, 19, 3
        };

        /// The byte orders CMSG_PLAYER_LOGIN's packed guid uses, mirroring
        /// CharacterHandler.cpp's ReadGuidMask/ReadGuidBytes exactly.
        const uint8 LOGIN_MASK_ORDER[8]  = { 2, 3, 0, 6, 4, 5, 1, 7 };
        const uint8 LOGIN_BYTE_ORDER[8]  = { 2, 7, 0, 3, 5, 6, 1, 4 };

        /// SHA-1 over the account name, the 40-byte session key and a 4-byte
        /// server nonce -- the second stream's proof (Wow-64.exe, sub_1400AA560).
        void StreamProof(const std::string& account, const BigNumber& sessionKey,
                         uint32 serverSeed, uint8 (&digest)[AUTH_DIGEST_SIZE])
        {
            BigNumber key = sessionKey;

            Sha1Hash sha;
            sha.UpdateData(account);
            sha.UpdateBigNumbers(&key, NULL);
            sha.UpdateData(reinterpret_cast<const uint8*>(&serverSeed), sizeof(serverSeed));
            sha.Finalize();

            std::memcpy(digest, sha.GetDigest(), AUTH_DIGEST_SIZE);
        }

        /// The first stream's proof, over both halves of the nonce
        /// (ClientConnection::HandleAuthSession).
        void LoginProof(const std::string& account, const BigNumber& sessionKey,
                        uint32 clientSeed, uint32 serverSeed,
                        uint8 (&digest)[AUTH_DIGEST_SIZE])
        {
            BigNumber key = sessionKey;
            const uint32 zero = 0;

            Sha1Hash sha;
            sha.UpdateData(account);
            sha.UpdateData(reinterpret_cast<const uint8*>(&zero), 4);
            sha.UpdateData(reinterpret_cast<const uint8*>(&clientSeed), 4);
            sha.UpdateData(reinterpret_cast<const uint8*>(&serverSeed), 4);
            sha.UpdateBigNumbers(&key, NULL);
            sha.Finalize();

            std::memcpy(digest, sha.GetDigest(), AUTH_DIGEST_SIZE);
        }
    }

    const char* StageName(Stage stage)
    {
        switch (stage)
        {
            case Stage::Start:         return "start";
            case Stage::Connected0:    return "connected (stream 0)";
            case Stage::Challenged0:   return "challenged (stream 0)";
            case Stage::Authenticated: return "authenticated";
            case Stage::CharListed:    return "character list received";
            case Stage::LoginSent:     return "player login sent";
            case Stage::Redirected:    return "redirect received";
            case Stage::Connected1:    return "connected (stream 1)";
            case Stage::StreamLive:    return "stream 1 live";
            case Stage::InWorld:       return "in world";
        }
        return "?";
    }

    SyntheticClient::SyntheticClient(const Config& config)
        : m_config(config),
          m_started(std::chrono::steady_clock::now())
    {
    }

    double SyntheticClient::ElapsedMs() const
    {
        const std::chrono::duration<double, std::milli> delta =
            std::chrono::steady_clock::now() - m_started;
        return delta.count();
    }

    void SyntheticClient::Trace(const char* what, ...) const
    {
        if (!m_config.verbose)
        {
            return;
        }

        char line[512];
        va_list args;
        va_start(args, what);
        vsnprintf(line, sizeof(line), what, args);
        va_end(args);

        std::printf("  [%8.1f ms] %s\n", ElapsedMs(), line);
        std::fflush(stdout);
    }

    Result SyntheticClient::Run()
    {
        m_started = std::chrono::steady_clock::now();

        struct Step
        {
            bool (SyntheticClient::*run)(std::string&);
            Stage reached;
        };

        // Every step advances exactly one stage, so a failure names the last
        // thing that worked rather than "login failed".
        const Step steps[] =
        {
            { &SyntheticClient::OpenStreamZero,  Stage::Challenged0    },
            { &SyntheticClient::Authenticate,    Stage::Authenticated  },
            { &SyntheticClient::ListCharacters,  Stage::CharListed     },
            { &SyntheticClient::SendPlayerLogin, Stage::LoginSent      },
            { &SyntheticClient::AwaitRedirect,   Stage::Redirected     },
            { &SyntheticClient::OpenStreamOne,   Stage::StreamLive     },
            { &SyntheticClient::AwaitWorld,      Stage::InWorld        },
        };

        for (const Step& step : steps)
        {
            std::string error;
            if (!(this->*step.run)(error))
            {
                m_result.error = error;
                break;
            }
            m_result.stage = step.reached;
        }

        if (m_result.stage == Stage::InWorld && m_config.onInWorld)
        {
            m_config.onInWorld();
        }

        if (m_result.stage == Stage::InWorld && m_config.holdSeconds > 0)
        {
            std::string error;
            if (!Serve(error))
            {
                m_result.error = error;
            }
        }

        m_result.packetsOnStream0 = m_stream0.packets;
        m_result.packetsOnStream1 = m_stream1.packets;
        m_result.bytesOnStream0   = m_stream0.bytes;
        m_result.bytesOnStream1   = m_stream1.bytes;

        m_result.sentOnStream0      = m_stream0.sent;
        m_result.sentOnStream1      = m_stream1.sent;
        m_result.sentBytesOnStream0 = m_stream0.sentBytes;
        m_result.sentBytesOnStream1 = m_stream1.sentBytes;
        return m_result;
    }

    bool SyntheticClient::Send(Stream& stream, const WorldPacket& packet,
                               std::string& error)
    {
        // AuthCrypt is named from the server's side, so the client enciphers what
        // it SENDS with DecryptRecv: that applies the very keystream the server
        // will apply to decrypt the header. Getting this backwards keys the two
        // directions off each other's halves and every header is noise.
        HeaderCipher cipher;
        if (stream.armed)
        {
            cipher = [&stream](uint8* header, size_t len)
            {
                stream.crypt.DecryptRecv(header, len);
            };
        }

        const std::vector<uint8> wire = EncodeClientPacket(packet, cipher);
        if (!stream.socket.SendAll(wire.data(), wire.size(), error))
        {
            return false;
        }
        ++stream.sent;
        stream.sentBytes += wire.size();
        return true;
    }

    // The client routes what it sends by opcode, not by socket: its send router
    // (OpcodeSlots.inc, send_slot) puts time-sync answers, movement and acks on the
    // second stream and the ping on the first. The server accepts either today;
    // following the client is what makes this peer a regression net for a server
    // that one day cares about cross-stream order. Stream 1 is live and keyed
    // from SMSG_RESUME_COMMS on, which is before Serve ever runs.
    SyntheticClient::Stream& SyntheticClient::StreamFor(uint16 opcode)
    {
        return proto::SendSlotOf(opcode) == proto::LinkSlot::One ? m_stream1 : m_stream0;
    }

    bool SyntheticClient::Drain(Stream& stream, int timeoutMs, std::string& error)
    {
        std::vector<uint8> raw;
        if (!stream.socket.Recv(raw, timeoutMs, error))
        {
            return false;
        }
        if (raw.empty())
        {
            return true;
        }

        stream.bytes += raw.size();

        std::vector<WorldPacket> packets;
        if (!stream.reader.Feed(raw.data(), raw.size(), packets, error))
        {
            return false;
        }

        stream.packets += uint32(packets.size());
        for (WorldPacket& packet : packets)
        {
            // Only through the login sequence: a session standing in the world
            // receives hundreds of packets a minute, and tracing those would bury
            // the handshake this is here to make legible.
            if (m_config.verbose && m_result.stage < Stage::InWorld)
            {
                Trace("<- 0x%.4X (%u bytes) on stream %d",
                      packet.GetOpcode(), uint32(packet.size()),
                      &stream == &m_stream1 ? 1 : 0);
            }
            stream.inbox.push_back(std::move(packet));
        }
        return true;
    }

    bool SyntheticClient::Await(Stream& stream, uint16 wanted, int timeoutMs,
                                WorldPacket& found, std::string& error)
    {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        for (;;)
        {
            for (size_t i = 0; i < stream.inbox.size(); ++i)
            {
                if (stream.inbox[i].GetOpcode() == wanted)
                {
                    found = stream.inbox[i];
                    stream.inbox.erase(stream.inbox.begin() + ptrdiff_t(i));
                    return true;
                }
            }

            if (std::chrono::steady_clock::now() >= deadline)
            {
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                              "timed out waiting for opcode 0x%.4X", wanted);
                error = detail;
                return false;
            }

            if (!Drain(stream, POLL_MS, error))
            {
                return false;
            }
        }
    }

    bool SyntheticClient::Pump(int timeoutMs, std::string& error)
    {
        if (!Drain(m_stream0, timeoutMs, error))
        {
            return false;
        }
        if (m_stream1.socket.IsOpen() && !Drain(m_stream1, 0, error))
        {
            return false;
        }
        return true;
    }

    bool SyntheticClient::ReadChallenge(const WorldPacket& packet,
                                        uint8 (&seed)[AuthCrypt::SeedLength],
                                        uint32& nonce)
    {
        // 37 bytes: the 32-byte cipher seed table, this connection's 4-byte
        // nonce, then the proof-of-work difficulty. Stream 0 leaves the table
        // zero and the client ignores it; stream 1 keys its ciphers from it.
        if (packet.size() < AuthCrypt::SeedLength + sizeof(uint32))
        {
            return false;
        }

        std::memcpy(seed, packet.contents(), AuthCrypt::SeedLength);
        std::memcpy(&nonce, packet.contents() + AuthCrypt::SeedLength, sizeof(nonce));
        return true;
    }

    bool SyntheticClient::ReadAuthStatus(const WorldPacket& packet, uint8& status,
                                         std::string& error)
    {
        // SMSG_AUTH_RESPONSE has three shapes, and the status byte sits at a
        // different offset in each -- so the flags have to be read before it can
        // be found at all. Reading it at a fixed offset finds the first byte of a
        // uint32(0) and reports a healthy login as "refused, status 0x00".
        //
        //   proto's own refusal (ClientConnection::SendAuthStatus)
        //       bit false, bit false                        -> flags 0x00
        //       uint8 status                                     payload 2
        //
        //   accepted (WorldSessionMgr.cpp, AddSession)
        //       bit false (queue), bit true (account info)  -> flags 0x40
        //       uint32, uint8 expansion, uint32, uint8 expansion,
        //       uint32, uint8 billing flags, uint8 status        payload 17
        //
        //   queued (WorldSessionMgr.cpp, AddQueuedSession)
        //       bit true (queue), bit false (unk), bit true (account info)
        //                                                   -> flags 0xA0
        //       ... same block ..., uint8 status, uint32 position payload 21
        //
        // Note the middle shape writes TWO flag bits where the queued one writes
        // three, so "has account info" is bit 6 in one and bit 5 in the other.
        // That is the server's own inconsistency, not a reading of it; both are
        // covered below because the queue bit is bit 7 in both and is tested
        // first.
        if (packet.empty())
        {
            error = "SMSG_AUTH_RESPONSE carried no payload";
            return false;
        }

        const uint8  flags = packet.contents()[0];
        const bool   queued = (flags & 0x80) != 0;
        const bool   hasAccountInfo = queued ? ((flags & 0x20) != 0)
                                             : ((flags & 0x40) != 0);

        // The account-info block is a fixed 15 bytes between the flags and the
        // status: uint32 + uint8 + uint32 + uint8 + uint32 + uint8.
        const size_t offset = hasAccountInfo ? 16 : 1;

        if (packet.size() <= offset)
        {
            char detail[128];
            std::snprintf(detail, sizeof(detail),
                          "SMSG_AUTH_RESPONSE is %u byte(s), too short for its own "
                          "shape (flags 0x%.2X)", uint32(packet.size()), flags);
            error = detail;
            return false;
        }

        status = packet.contents()[offset];
        return true;
    }

    bool SyntheticClient::OpenStreamZero(std::string& error)
    {
        if (!m_stream0.socket.Connect(m_config.host, m_config.port,
                                      CONNECT_TIMEOUT_MS, error))
        {
            return false;
        }
        m_result.stage = Stage::Connected0;
        Trace("connected to %s:%u", m_config.host.c_str(), uint32(m_config.port));

        // The server writes its banner and its challenge back to back on accept,
        // without waiting for ours (ClientConnection::onConnect).
        WorldPacket challenge;
        if (!Await(m_stream0, SMSG_AUTH_CHALLENGE, STAGE_TIMEOUT_MS, challenge, error))
        {
            return false;
        }

        uint8 ignored[AuthCrypt::SeedLength];
        if (!ReadChallenge(challenge, ignored, m_serverSeed0))
        {
            error = "SMSG_AUTH_CHALLENGE is too short to carry a seed";
            return false;
        }

        Trace("challenge received, server seed 0x%.8X", m_serverSeed0);
        return true;
    }

    bool SyntheticClient::Authenticate(std::string& error)
    {
        const std::vector<uint8> banner = ClientBanner();
        if (!m_stream0.socket.SendAll(banner.data(), banner.size(), error))
        {
            return false;
        }

        // Any value the server has not seen; it is hashed into the proof, so it
        // only has to be ours and known to us.
        BigNumber clientSeed;
        clientSeed.SetRand(32);
        m_clientSeed = clientSeed.AsDword();

        uint8 digest[AUTH_DIGEST_SIZE];
        LoginProof(m_config.account, m_config.sessionKey, m_clientSeed,
                   m_serverSeed0, digest);

        // Cata interleaves the digest with the rest of the fields, and the read
        // order in ClientConnection::HandleAuthSession is wire truth. This writes
        // the mirror of it, field for field and skip for skip.
        WorldPacket packet(CMSG_AUTH_SESSION, 128);
        packet << uint32(0);                                 // skipped
        packet << uint32(0);                                 // skipped
        packet << uint8(0);                                  // skipped
        packet << digest[10] << digest[18] << digest[12] << digest[5];
        packet << uint64(0);                                 // skipped
        packet << digest[15] << digest[9] << digest[19] << digest[4];
        packet << digest[7] << digest[16] << digest[3];
        packet << uint16(m_config.build);
        packet << digest[8];
        packet << uint32(0);                                 // skipped
        packet << uint8(0);                                  // skipped
        packet << digest[17] << digest[6] << digest[0] << digest[1] << digest[11];
        packet << uint32(m_clientSeed);
        packet << digest[2];
        packet << uint32(0);                                 // skipped
        packet << digest[14] << digest[13];
        packet << uint32(0);                                 // addon block: none

        // The name's length is split across two bytes and reassembled as
        // (high << 5) | (low >> 3). Anything else truncates the account name,
        // and the proof is over the name -- so it fails as a bad password would.
        const std::string& account = m_config.account;
        packet << uint8(account.size() >> 5);
        packet << uint8((account.size() & 0x1F) << 3);
        if (!account.empty())
        {
            packet.append(reinterpret_cast<const uint8*>(account.data()), account.size());
        }

        // In clear, and only THEN is the cipher armed: the client sends this
        // packet before it keys anything, exactly as it does with
        // CMSG_AUTH_CONTINUED_SESSION on the second stream.
        if (!Send(m_stream0, packet, error))
        {
            return false;
        }

        BigNumber key = m_config.sessionKey;
        m_stream0.crypt.Init(&key);                          // the shipped seed pair
        m_stream0.armed = true;
        m_stream0.reader.SetCipher([this](uint8* header, size_t len)
        {
            m_stream0.crypt.EncryptSend(header, len);
        });

        WorldPacket response;
        if (!Await(m_stream0, SMSG_AUTH_RESPONSE, STAGE_TIMEOUT_MS, response, error))
        {
            return false;
        }

        uint8 status = 0;
        if (!ReadAuthStatus(response, status, error))
        {
            return false;
        }

        // A queue position instead of AUTH_OK is not a failure of the harness --
        // it is the server saying it is full -- so it is reported as itself.
        if (status == AUTH_WAIT_QUEUE)
        {
            error = "the server put this session in its login queue";
            return false;
        }
        if (status != AUTH_OK)
        {
            char detail[96];
            std::snprintf(detail, sizeof(detail),
                          "the server refused the login (status 0x%.2X)", status);
            error = detail;
            return false;
        }

        m_result.msToAuth = ElapsedMs();
        Trace("authenticated as '%s'", m_config.account.c_str());
        return true;
    }

    bool SyntheticClient::ListCharacters(std::string& error)
    {
        // Sent for fidelity and as a round-trip check on the armed cipher: if the
        // header keys were wrong, this is where it shows, with a clean opcode
        // rather than at the far end of the redirect.
        //
        // The reply is deliberately not parsed. 4.3.4 packs SMSG_CHAR_ENUM into a
        // bit-stream of per-character masks that would be a project of its own,
        // and the guid it would yield is one SQL query away instead.
        WorldPacket request(CMSG_CHAR_ENUM, 0);
        if (!Send(m_stream0, request, error))
        {
            return false;
        }

        WorldPacket enumeration;
        if (!Await(m_stream0, SMSG_CHAR_ENUM, STAGE_TIMEOUT_MS, enumeration, error))
        {
            return false;
        }

        Trace("character list received (%u bytes)", uint32(enumeration.size()));
        return true;
    }

    bool SyntheticClient::SendPlayerLogin(std::string& error)
    {
        if (m_config.characterGuid == 0)
        {
            error = "no character guid given; pass --character";
            return false;
        }

        // The packed guid CharacterHandler.cpp reads: a bitmask in one byte order
        // saying which of the eight bytes are non-zero, then those bytes in a
        // different order, each XORed with 1.
        const uint64 guid = m_config.characterGuid;

        WorldPacket packet(CMSG_PLAYER_LOGIN, 16);
        for (size_t i = 0; i < 8; ++i)
        {
            packet.WriteBit(uint8(guid >> (8 * LOGIN_MASK_ORDER[i])) != 0);
        }
        for (size_t i = 0; i < 8; ++i)
        {
            const uint8 byte = uint8(guid >> (8 * LOGIN_BYTE_ORDER[i]));
            if (byte != 0)
            {
                packet << uint8(byte ^ 1);
            }
        }

        if (!Send(m_stream0, packet, error))
        {
            return false;
        }

        Trace("player login sent for guid %llu",
              static_cast<unsigned long long>(guid));
        return true;
    }

    bool SyntheticClient::AwaitRedirect(std::string& error)
    {
        // The server does not run this login. It holds the packet and asks the
        // client for a second stream first (WorldSession::BeginSecondStream), so
        // what comes back is a redirect and not a world.
        WorldPacket connectTo;
        if (!Await(m_stream0, SMSG_CONNECT_TO, STAGE_TIMEOUT_MS, connectTo, error))
        {
            return false;
        }

        // The payload is an RSA-signed blob the real client verifies against the
        // modulus patched into its image. Nothing here needs to open it: the
        // destination it names is the port this harness was already told about,
        // and forging one would test our own signer rather than the server.
        m_result.msToRedirect = ElapsedMs();
        Trace("redirect received (%u bytes)", uint32(connectTo.size()));
        return true;
    }

    bool SyntheticClient::OpenStreamOne(std::string& error)
    {
        if (!m_stream1.socket.Connect(m_config.host, m_config.streamPort,
                                      CONNECT_TIMEOUT_MS, error))
        {
            return false;
        }
        m_result.stage = Stage::Connected1;
        Trace("connected to %s:%u (stream 1)",
              m_config.host.c_str(), uint32(m_config.streamPort));

        WorldPacket challenge;
        if (!Await(m_stream1, SMSG_AUTH_CHALLENGE, STAGE_TIMEOUT_MS, challenge, error))
        {
            return false;
        }
        if (!ReadChallenge(challenge, m_challengeSeed1, m_serverSeed1))
        {
            error = "the second stream's challenge is too short to carry a seed";
            return false;
        }

        const std::vector<uint8> banner = ClientBanner();
        if (!m_stream1.socket.SendAll(banner.data(), banner.size(), error))
        {
            return false;
        }

        uint8 digest[AUTH_DIGEST_SIZE];
        StreamProof(m_config.account, m_config.sessionKey, m_serverSeed1, digest);

        WorldPacket handshake(CMSG_AUTH_CONTINUED_SESSION, CONTINUED_SESSION_SIZE);
        handshake << uint64(0);         // the connect-to key, echoed; unread here
        handshake << uint64(0);         // the proof-of-work counter; unread here
        for (size_t i = 0; i < AUTH_DIGEST_SIZE; ++i)
        {
            handshake << digest[CONTINUED_DIGEST_ORDER[i]];
        }

        // In clear, like the first stream's login, and the ciphers are keyed only
        // afterwards -- and from the 32 challenge bytes rather than the constants
        // in the client image, which is what makes this stream's keystream
        // different from stream 0's under the same session key.
        if (!Send(m_stream1, handshake, error))
        {
            return false;
        }

        BigNumber key = m_config.sessionKey;
        m_stream1.crypt.Init(&key, m_challengeSeed1);
        m_stream1.armed = true;
        m_stream1.reader.SetCipher([this](uint8* header, size_t len)
        {
            m_stream1.crypt.EncryptSend(header, len);
        });

        WorldPacket resume;
        if (!Await(m_stream1, SMSG_RESUME_COMMS, STAGE_TIMEOUT_MS, resume, error))
        {
            return false;
        }

        m_result.msToStream1 = ElapsedMs();
        Trace("stream 1 live");
        return true;
    }

    bool SyntheticClient::AwaitWorld(std::string& error)
    {
        // With the second stream live the server requeues the login it held, so
        // the character now actually enters the world. SMSG_LOGIN_VERIFY_WORLD is
        // not one of the gated opcodes, so it comes back on stream 0.
        WorldPacket verify;
        if (!Await(m_stream0, SMSG_LOGIN_VERIFY_WORLD, STAGE_TIMEOUT_MS, verify, error))
        {
            return false;
        }

        // uint32 map, then x, y, z, o: where the character now stands, which is
        // where a scripted walk starts from. A short packet must not be walked
        // from -- that would carry the character to the origin and save it there.
        if (verify.size() < 20)
        {
            char detail[96];
            std::snprintf(detail, sizeof(detail),
                          "SMSG_LOGIN_VERIFY_WORLD is %u byte(s), not the 20 a position needs",
                          uint32(verify.size()));
            error = detail;
            return false;
        }
        m_result.mapId      = verify.read<uint32>(0);
        m_result.worldPos.x = verify.read<float>(4);
        m_result.worldPos.y = verify.read<float>(8);
        m_result.worldPos.z = verify.read<float>(12);
        m_result.worldPos.o = verify.read<float>(16);
        Trace("placed on map %u at %.1f %.1f %.1f facing %.2f", m_result.mapId,
              m_result.worldPos.x, m_result.worldPos.y, m_result.worldPos.z,
              m_result.worldPos.o);

        m_result.msToWorld = ElapsedMs();
        Trace("in world");
        return true;
    }

    bool SyntheticClient::Dispatch(WorldPacket& packet, AckEngine& acks, Walker& walker,
                                   uint32 nowTicks, std::string& error)
    {
        PeerReport& report = m_result.peer;
        const uint16 opcode = packet.GetOpcode();

        switch (opcode)
        {
            case SMSG_TIME_SYNC_REQ:
            {
                uint32 counter = 0;
                if (!ReadTimeSyncRequest(packet, counter))
                {
                    ++report.decodeFailures[opcode];
                    return true;
                }
                const WorldPacket reply = MakeTimeSyncResponse(counter, nowTicks);
                if (!Send(StreamFor(reply.GetOpcode()), reply, error))
                {
                    return false;
                }
                ++report.timeSyncsAnswered;
                Trace("time sync %u answered at %u", counter, nowTicks);
                return true;
            }

            case SMSG_PLAYER_MOVE:
            {
                Wire::MovementStatus status;
                Wire::DecodeResult const decoded = Wire::Decode(packet, Wire::SequenceFor(SMSG_PLAYER_MOVE), status);
                if (!decoded.ok() || decoded.consumed != packet.size())
                {
                    ++report.decodeFailures[opcode];
                    return true;
                }
                Observation seen;
                seen.guid = status.guid;
                seen.pos = status.pos;
                seen.time = status.time;
                seen.atTicks = nowTicks;
                if (m_config.script.observeGuid != 0 && status.guid == m_config.script.observeGuid)
                {
                    // Only the target's own observations feed the skew: other
                    // movers interleave and would compare unrelated timestamps.
                    // A status with no timestamp decodes time as 0, which would
                    // read as skew unrelated to the relay -- so the sample is
                    // taken only when this status actually carries one.
                    if (report.observedTarget > 0 && status.has.timestamp)
                    {
                        const uint32 skew = RelaySkew(report.lastTargetObservation, seen);
                        if (skew > report.relayMaxSkew)
                        {
                            report.relayMaxSkew = skew;
                        }
                        ++report.relaySkewSamples;
                    }
                    ++report.observedTarget;
                    report.lastTargetObservation = seen;
                    Trace("saw the target at %.1f %.1f %.1f (time %u)",
                          seen.pos.x, seen.pos.y, seen.pos.z, seen.time);
                }
                else
                {
                    ++report.observedOthers;
                }
                return true;
            }

            case SMSG_CLIENT_CONTROL_UPDATE:
                ++report.controlUpdates;
                return true;

            case SMSG_MOVE_TELEPORT:
            {
                Wire::Teleport t;
                packet.rpos(0);
                packet.ResetBitReader();
                Wire::DecodeResult const decoded = Wire::DecodeTeleport(packet, t);
                if (!decoded.ok() || decoded.consumed != packet.size())
                {
                    ++report.decodeFailures[opcode];
                    return true;
                }
                ++report.teleports;
                // A real client answers with its own clock, then is at the new place.
                Wire::TeleportAck ack;
                ack.counter = t.counter;
                ack.time = nowTicks;
                ack.guid = t.guid;
                WorldPacket reply(CMSG_MOVE_TELEPORT_ACK, 16);
                Wire::EncodeTeleportAck(reply, ack);
                if (!Send(StreamFor(CMSG_MOVE_TELEPORT_ACK), reply, error)) { return false; }
                ++report.teleportAcks;
                report.teleportFinal = t.pos;
                walker.Relocate(t.pos);
                acks.SetMover(walker.Status());
                Trace("teleported to %.1f %.1f %.1f, acked counter %u", t.pos.x, t.pos.y, t.pos.z, t.counter);
                return true;
            }

            case SMSG_MOVE_KNOCK_BACK:
            {
                Wire::KnockBack k;
                packet.rpos(0);
                packet.ResetBitReader();
                Wire::DecodeResult const decoded = Wire::DecodeKnockBack(packet, k);
                if (!decoded.ok() || decoded.consumed != packet.size())
                {
                    ++report.decodeFailures[opcode];
                    return true;
                }
                ++report.knockBacks;
                // The ack is the mover's status carrying the knock as its fall block
                // (CMSG_MOVE_KNOCK_BACK_ACK is a registered layout).
                Wire::MovementStatus reply = walker.Status();
                reply.counter = k.counter;
                // Stamped from the peer's own clock, like the time-sync and
                // teleport replies: an ack with time 0 would relay to a paired
                // observer as an SMSG_MOVE_UPDATE_KNOCK_BACK out of any order.
                reply.has.timestamp = true;
                reply.time = nowTicks;
                reply.fall.present = true;
                reply.fall.hasDirection = true;
                reply.fall.time = 0;
                reply.fall.vertical = k.vertical;
                reply.fall.horizontal = k.horizontal;
                reply.fall.cosAngle = k.directionX;
                reply.fall.sinAngle = k.directionY;
                WorldPacket ackPacket(CMSG_MOVE_KNOCK_BACK_ACK, 64);
                Wire::Encode(ackPacket, Wire::SequenceFor(CMSG_MOVE_KNOCK_BACK_ACK), reply);
                if (!Send(StreamFor(CMSG_MOVE_KNOCK_BACK_ACK), ackPacket, error)) { return false; }
                ++report.knockBackAcks;
                Trace("knocked back (h %.1f v %.1f), acked counter %u", k.horizontal, k.vertical, k.counter);
                return true;
            }

            case SMSG_MOVE_SET_ACTIVE_MOVER:
            {
                Wire::ActiveMover m;
                packet.rpos(0);
                packet.ResetBitReader();
                Wire::DecodeResult const decoded = Wire::DecodeActiveMover(packet, SMSG_MOVE_SET_ACTIVE_MOVER, m);
                if (!decoded.ok() || decoded.consumed != packet.size()) { ++report.decodeFailures[opcode]; }
                else { ++report.activeMoverSets; }
                return true;
            }

            case SMSG_MONSTER_MOVE:
            case SMSG_MONSTER_MOVE_TRANSPORT:
            {
                const Wire::Verdict v = Wire::Judge(opcode, packet, false);
                if (!v.decoded || !v.exact) { ++report.decodeFailures[opcode]; }
                else { ++report.monsterMoves; }
                return true;
            }

            case SMSG_MOVE_UPDATE_WALK_SPEED:
            case SMSG_MOVE_UPDATE_RUN_SPEED:
            case SMSG_MOVE_UPDATE_RUN_BACK_SPEED:
            case SMSG_MOVE_UPDATE_SWIM_SPEED:
            case SMSG_MOVE_UPDATE_SWIM_BACK_SPEED:
            case SMSG_MOVE_UPDATE_TURN_RATE:
            case SMSG_MOVE_UPDATE_FLIGHT_SPEED:
            case SMSG_MOVE_UPDATE_FLIGHT_BACK_SPEED:
            case SMSG_MOVE_UPDATE_COLLISION_HEIGHT:
            case SMSG_MOVE_UPDATE_KNOCK_BACK:
            case SMSG_MOVE_UPDATE_TELEPORT:
            {
                // What the server tells everyone but the mover once the mover acked
                // (design v2 §7's observer column): counted for the observed mover.
                Wire::MovementStatus status;
                Wire::DecodeResult const decoded = Wire::Decode(packet, Wire::SequenceFor(opcode), status);
                if (!decoded.ok() || decoded.consumed != packet.size())
                {
                    ++report.decodeFailures[opcode];
                    return true;
                }
                if (m_config.script.observeGuid != 0 && status.guid == m_config.script.observeGuid)
                {
                    ++report.observerForms[opcode];
                }
                else
                {
                    ++report.observedOthers;
                }
                return true;
            }

            default:
                if (acks.IsChange(opcode))
                {
                    // Root and unroot reach observers under the mover's opcode: peek at the guid
                    // before handing the packet to the ack engine, which would answer for
                    // someone else's change.
                    if (m_config.script.observeGuid != 0 && (opcode == SMSG_FORCE_MOVE_ROOT || opcode == SMSG_FORCE_MOVE_UNROOT))
                    {
                        WorldPacket peek(packet);
                        peek.rpos(0);
                        peek.ResetBitReader();
                        Wire::MovementStatus status;
                        Wire::DecodeResult const decoded = Wire::Decode(peek, Wire::SequenceFor(opcode), status);
                        if (decoded.ok() && status.guid == m_config.script.observeGuid)
                        {
                            ++report.observerForms[opcode];
                            return true;
                        }
                    }
                    acks.Plan(packet, nowTicks);
                    return true;
                }
                ++report.otherPackets;
                return true;
        }
    }

    bool SyntheticClient::Serve(std::string& error)
    {
        // A real client pings roughly every 30 seconds, and the server kicks a
        // session that pings faster than that too often
        // (WorldSession::HandlePingOpcode, CONFIG_UINT32_MAX_OVERSPEED_PINGS).
        // Holding a session therefore means keeping that cadence, not merely
        // leaving a socket open.
        const std::chrono::seconds hold(m_config.holdSeconds);
        const std::chrono::steady_clock::time_point until =
            std::chrono::steady_clock::now() + hold;

        std::chrono::steady_clock::time_point nextPing =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        uint32 pingId = 0;

        ClientClock clock;
        Walker walker(m_config.script.walk, m_config.characterGuid, m_result.worldPos);
        AckEngine acks(m_config.script.ack, [](uint16 opcode) { return Wire::SequenceFor(opcode); });

        // Walker and AckEngine tally into their own locals as Serve runs; nothing
        // copies those tallies into m_result.peer until this runs. A kick (the
        // server closing the socket mid-hold, under Movement.AckTimeout's last
        // rung) exits through one of the early `return false`s below, not the
        // loop's normal fall-through -- so every return, not just that one, must
        // call this, or a kicked run reports acksSent=0/changesSeen={} even
        // though the engine watched the whole resend/resync ladder happen.
        const auto finish = [this, &walker, &acks]()
        {
            PeerReport& report = m_result.peer;
            report.walkStarts = walker.Starts();
            report.walkHeartbeats = walker.Heartbeats();
            report.walkStops = walker.Stops();
            report.relocations = walker.Relocations();
            report.walkFinal = walker.Position();
            report.walkLastTime = walker.LastStampedTime();
            report.acksSent = acks.Sent();
            report.acksDropped = acks.Dropped();
            report.acksPending = acks.PendingCount();
            report.unregisteredChanges = acks.Unregistered();
            report.changesSeen = acks.ChangesSeen();
            for (std::map<uint16, uint32>::const_iterator it = acks.DecodeFailures().begin();
                 it != acks.DecodeFailures().end(); ++it)
            {
                report.decodeFailures[it->first] += it->second;
            }

            Trace("held for %u s: %u time syncs, walk %u/%u/%u, saw target %u times",
                  m_config.holdSeconds, report.timeSyncsAnswered, report.walkStarts,
                  report.walkHeartbeats, report.walkStops, report.observedTarget);

            // Diagnostic, not gated on --verbose: Task 8's launcher drives the walker
            // and the observer as separate processes and reads this stdout, the same
            // way it reads the PEER VERDICT lines.
            std::printf("observer forms: ");
            for (std::map<uint16, uint32>::const_iterator it = report.observerForms.begin();
                 it != report.observerForms.end(); ++it)
            {
                std::printf("0x%.4X=%u ", uint32(it->first), it->second);
            }
            std::printf("\n");
            std::printf("changes seen: ");
            for (std::map<uint16, uint32>::const_iterator it = report.changesSeen.begin();
                 it != report.changesSeen.end(); ++it)
            {
                std::printf("0x%.4X=%u ", uint32(it->first), it->second);
            }
            std::printf("\n");
        };

        while (std::chrono::steady_clock::now() < until)
        {
            if (!Pump(POLL_MS, error))
            {
                if (m_stream0.socket.PeerClosed() || m_stream1.socket.PeerClosed())
                {
                    m_result.peer.kicked = true;
                }
                finish();
                return false;
            }
            const uint32 now = clock.Ticks();

            Stream* streams[] = { &m_stream0, &m_stream1 };
            for (Stream* stream : streams)
            {
                for (WorldPacket& packet : stream->inbox)
                {
                    if (!Dispatch(packet, acks, walker, now, error))
                    {
                        finish();
                        return false;
                    }
                }
                stream->inbox.clear();
            }

            for (const WorldPacket& packet : walker.Advance(now))
            {
                if (!Send(StreamFor(packet.GetOpcode()), packet, error))
                {
                    finish();
                    return false;
                }
            }
            // An ack sent this tick carries where the mover is this tick.
            acks.SetMover(walker.Status());
            for (const WorldPacket& packet : acks.Due(now))
            {
                if (!Send(StreamFor(packet.GetOpcode()), packet, error))
                {
                    finish();
                    return false;
                }
            }

            if (std::chrono::steady_clock::now() >= nextPing)
            {
                WorldPacket ping(CMSG_PING, 8);
                ping << uint32(++pingId);
                ping << uint32(50);                          // reported latency
                if (!Send(StreamFor(CMSG_PING), ping, error))
                {
                    finish();
                    return false;
                }
                nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                Trace("ping %u", pingId);
            }
        }

        finish();
        return true;
    }
}
