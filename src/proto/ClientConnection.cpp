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

#include <string>
#include <cstdint>
#include <utility>
#include <vector>
#include <memory>
#include <mutex>
#include "ClientConnection.h"

#include "Auth/BigNumber.h"
#include "Auth/Sha1.h"
#include "Crypto/SystemRandom.h"
#include "Log/Log.h"
#include "SessionLinks.h"
#include "Utilities/ByteBuffer.h"

#include <cstring>

namespace proto
{
    namespace
    {
        /// Number of bytes in the client's SHA-1 login proof.
        const size_t AUTH_DIGEST_SIZE = 20;

        // The handful of transport opcodes this file speaks. Re-derived from
        // (grepped out of, never copied by memory from) this fork's own
        // src/game/Server/Opcodes.h -- proto does not link game, so these are
        // proto-local constants rather than references into that table. Every
        // value carries the exact line it was read from, on 2026-07-24:
        //   MSG_WOW_CONNECTION   Opcodes.h:55   (0x4F57)
        //   SMSG_AUTH_CHALLENGE  Opcodes.h:56   (0x4542)
        //   CMSG_AUTH_SESSION    Opcodes.h:57   (0x0449)
        //   SMSG_AUTH_RESPONSE   Opcodes.h:58   (0x5DB6)
        //   CMSG_PING            Opcodes.h:555  (0x444D)
        //   SMSG_PONG            Opcodes.h:556  (0x4D42)
        //   CMSG_KEEP_ALIVE      Opcodes.h:1119 (0x0015)
        //
        // CMSG_AUTH_CONTINUED_SESSION is the exception: Opcodes.h carried a
        // placeholder for it (0x1513, from an older list), so its value comes
        // from the client's own send router by way of OpcodeSlots.inc:85 --
        // and the live wire confirmed it on 2026-08-30, a 36-byte payload under
        // 0x044D landing on the stream-1 port. Opcodes.h now agrees.
        const uint16 MSG_WOW_CONNECTION  = 0x4F57;
        const uint16 SMSG_AUTH_CHALLENGE = 0x4542;
        const uint16 CMSG_AUTH_SESSION   = 0x0449;
        const uint16 CMSG_AUTH_CONTINUED_SESSION = 0x044D;
        // SMSG_RESUME_COMMS carried a placeholder too (0x1512); 0x0140 is the
        // value in the client's connection-control set, the eight opcodes it
        // dispatches ahead of every gate. Opcodes.h now agrees.
        const uint16 SMSG_RESUME_COMMS   = 0x0140;

        /// What the client puts in CMSG_AUTH_CONTINUED_SESSION: the connect-to
        /// key it was redirected with, the proof-of-work counter it searched for,
        /// and a 20-byte SHA-1 (Wow-64.exe 15595, sub_1400AA560). Checked, not
        /// parsed -- proto reads none of it.
        const size_t CONTINUED_SESSION_SIZE = 8 + 8 + 20;
        const uint16 SMSG_AUTH_RESPONSE  = 0x5DB6;
        [[maybe_unused]] const uint16 CMSG_PING           = 0x444D;
        [[maybe_unused]] const uint16 SMSG_PONG           = 0x4D42;
        [[maybe_unused]] const uint16 CMSG_KEEP_ALIVE     = 0x0015;

        /**
         * @brief Draw the server authentication nonce from the cryptographic RNG.
         *
         * WorldSocket's constructor drew this from rand32() (the general-purpose
         * PRNG). The value is hashed into the client's proof, so a predictable
         * seed narrows the search space for anyone replaying a captured login --
         * the OS CSPRNG is the strictly-safer port.
         */
        uint32 MakeAuthSeed()
        {
            BigNumber seed;
            seed.SetRand(32);
            return seed.AsDword();
        }
    }

    std::atomic<uint32> ClientConnection::s_openConnections{0};

    ClientConnection::ClientConnection(IWorldGateway& gateway, RedirectRegistry& redirects,
                                       const EndpointPolicy& policy)
        : m_gateway(gateway),
          m_redirects(redirects),
          m_policy(policy),
          m_role(policy.role),
          m_challengeSeed{},
          m_bannerDone(false),
          m_codec(),
          m_seed(MakeAuthSeed()),
          m_session(INVALID_SESSION_ID),
          m_traceSession(INVALID_SESSION_ID),
          m_closed(false)
    {
        s_openConnections.fetch_add(1, std::memory_order_relaxed);
    }

    ClientConnection::~ClientConnection()
    {
        s_openConnections.fetch_sub(1, std::memory_order_relaxed);
    }

    void ClientConnection::AppendBanner(std::vector<uint8_t>& wire)
    {
        // MSG_WOW_CONNECTION (WorldSocket.cpp:360-363). The string looks
        // truncated -- it is the shipped byte sequence, and it is only missing
        // its first two letters because those two letters ARE the opcode: 0x4F57
        // is "WO", so the framed packet reads as the whole sentence on the wire.
        // Do not "fix" it.
        WorldPacket connection(MSG_WOW_CONNECTION, 46);
        connection << std::string("RLD OF WARCRAFT CONNECTION - SERVER TO CLIENT");

        m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed),
                              connection, false, TraceSlot());
        const std::vector<uint8> encoded =
            PacketCodec::Encode(connection, PacketCodec::HeaderEncryptor());
        wire.insert(wire.end(), encoded.begin(), encoded.end());
    }

    void ClientConnection::AppendAuthChallenge(std::vector<uint8_t>& wire)
    {
        // SMSG_AUTH_CHALLENGE (37-byte payload, WorldSocket.cpp:371-378): eight
        // uint32s, then the server seed, then a trailing uint8(1).
        //
        // Those 32 bytes are not padding, though stream 0 may leave them zero.
        // The client keeps them (Wow-64.exe 15595, sub_1400AA560) and, on a
        // REDIRECTED connection, keys that stream's ciphers from them instead of
        // from the pair in its image. So m_challengeSeed is what we sent and what
        // we must key with; it stays zero on stream 0, where the client ignores
        // it, leaving that handshake byte-for-byte what it always was.
        //
        // The trailing byte is the client's proof-of-work difficulty: it hashes
        // its account name, these 32 bytes and a counter until the digest ends in
        // that many zero bits. One bit costs it nothing and is what 4.3.4 shipped.
        WorldPacket challenge(SMSG_AUTH_CHALLENGE, 37);
        challenge.append(m_challengeSeed.data(), m_challengeSeed.size());
        challenge << m_seed;
        challenge << uint8(1);

        m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed),
                              challenge, false, TraceSlot());
        const std::vector<uint8> encoded =
            PacketCodec::Encode(challenge, PacketCodec::HeaderEncryptor());
        wire.insert(wire.end(), encoded.begin(), encoded.end());
    }

    bool ClientConnection::ClaimRedirect()
    {
        RedirectTicket ticket;
        if (!m_redirects.Claim(m_address, ticket))
        {
            return false;
        }

        m_session = ticket.session;
        m_traceSession.store(ticket.session, std::memory_order_relaxed);
        m_links       = ticket.links;
        m_redirectKey = ticket.sessionKey;
        m_redirectAccount    = ticket.accountName;
        m_redirectGeneration = ticket.generation;
        return true;
    }

    std::vector<uint8_t> ClientConnection::onConnect()
    {
        std::vector<uint8_t> wire;

        if (m_role == ConnRole::Staging1)
        {
            // Nothing identifies this socket but the address it came from, and
            // the ticket that address matches was opened when the redirect went
            // out. No ticket means nobody asked this client for a second stream.
            if (!ClaimRedirect())
            {
                sLog.outError("proto: unexpected connection on the stream-1 port from %s, "
                              "no redirect is in flight for that address",
                              m_address.c_str());
                Close();
                return wire;
            }

            AppendBanner(wire);

            if (m_policy.armRedirectedCrypto)
            {
                // Fresh per connection, because these bytes become this stream's
                // cipher keys (with the session key, which stream 0 already used
                // for its own pair). Reusing one table across both streams of a
                // session would run two keystreams from the same key material.
                MaNGOS::Crypto::SystemRandom::Instance().FillExact(m_challengeSeed.data(),
                                                                   m_challengeSeed.size());
                AppendAuthChallenge(wire);
            }

            return wire;
        }

        // Fire-and-continue: WorldSocket::open() sends both packets back to back
        // and does not wait for the client's own MSG_WOW_CONNECTION in between
        // (WorldSocket.cpp:360-383). This is Cata-only transport scaffolding with
        // no equivalent in any sibling fork; reproduce it byte-for-byte or the
        // 15595 client hangs at "Connecting". Neither packet is encrypted: the
        // crypt is not armed yet, and the client cannot key its own cipher until
        // it has the challenge.
        AppendBanner(wire);
        AppendAuthChallenge(wire);

        return wire;
    }

    void ClientConnection::ArmRedirectedCrypto()
    {
        BigNumber key = m_redirectKey;

        std::lock_guard<std::mutex> lock(m_cryptSendLock);
        m_crypt.Init(&key, m_challengeSeed.data());
        m_codec.SetHeaderDecryptor(
            [this](uint8* header, size_t len) { m_crypt.DecryptRecv(header, len); });
    }

    bool ClientConnection::VerifyContinuedSession(WorldPacket& packet)
    {
        // What the client sends on the second stream, and the only thing on this
        // socket that says who is on it (Wow-64.exe 15595, sub_1400AA560):
        //
        //   uint64  the connect-to key the redirect carried, echoed back
        //   uint64  the counter it searched for, to satisfy the challenge's
        //           proof-of-work difficulty
        //   uint8   digest[20]
        //
        // The digest is SHA-1 over the account name, the 40-byte session key and
        // the 4-byte seed this connection put in its own SMSG_AUTH_CHALLENGE, in
        // that order -- the same shape as the stream-0 login proof, and the same
        // secret behind it. A ticket is claimed by address alone, so without this
        // the second stream belongs to whoever reaches the port first from that
        // address; with it, only the client holding the session key can take it.
        uint64 key = 0;
        uint64 counter = 0;
        uint8  digest[AUTH_DIGEST_SIZE];

        try
        {
            packet >> key;
            packet >> counter;

            // The twenty digest bytes are interleaved on the wire, the same
            // trick CMSG_AUTH_SESSION plays on stream 0 -- this is a different
            // order, and it is wire truth rather than a choice. Read out of
            // three live handshakes on 2026-08-30: the bytes the client sent
            // were a permutation of the digest computed below, and two of the
            // three had twenty distinct bytes, which pins every index.
            packet >> digest[5];
            packet >> digest[2];
            packet >> digest[6];
            packet >> digest[10];
            packet >> digest[8];
            packet >> digest[17];
            packet >> digest[11];
            packet >> digest[15];
            packet >> digest[7];
            packet >> digest[1];
            packet >> digest[4];
            packet >> digest[16];
            packet >> digest[0];
            packet >> digest[12];
            packet >> digest[14];
            packet >> digest[13];
            packet >> digest[18];
            packet >> digest[9];
            packet >> digest[19];
            packet >> digest[3];
        }
        catch (ByteBufferException&)
        {
            sLog.outError("proto: truncated CMSG_AUTH_CONTINUED_SESSION from %s",
                          m_address.c_str());
            return false;
        }

        BigNumber sessionKey = m_redirectKey;

        Sha1Hash sha;
        sha.UpdateData(m_redirectAccount);
        sha.UpdateBigNumbers(&sessionKey, NULL);
        sha.UpdateData(reinterpret_cast<const uint8*>(&m_seed), sizeof(m_seed));
        sha.Finalize();

        if (std::memcmp(sha.GetDigest(), digest, AUTH_DIGEST_SIZE) != 0)
        {
            sLog.outError("proto: bad second-stream proof for account '%s' from %s; "
                          "refusing the stream",
                          m_redirectAccount.c_str(), m_address.c_str());
            return false;
        }

        return true;
    }

    void ClientConnection::SendResumeComms()
    {
        // The packet that makes the second stream real, on the client's side.
        //
        // Until this arrives the client keeps the new socket in a staging slot,
        // where sub_1400A9B60 refuses every ordinary packet with disconnect
        // reason 3 and closes it -- only the eight connection-control opcodes
        // reach a staging connection at all. SMSG_RESUME_COMMS is the one that
        // ends that state: sub_1400AC1D0 moves the connection out of staging and
        // into stream 1, carries the banner flag across, retires whatever held
        // that slot, and flushes the queue.
        //
        // That queue is the other half of it. The client marks stream 1 pending
        // at construction (NetClient + 34505 = 1), so from the first second of
        // the session every opcode its router sends on stream 1 is queued rather
        // than transmitted -- CMSG_LOGOUT_REQUEST among them, which is why a
        // session that never sees this packet has a Logout button that does
        // nothing at all. Nothing is lost; it is all delivered by the flush.
        //
        // Empty by design: sub_1400AC1D0 reads no payload, only the connection
        // the packet arrived on.
        WorldPacket resume(SMSG_RESUME_COMMS, 0);
        SendPacket(resume);
    }

    bool ClientConnection::PromoteToSlotOne()
    {
        const std::shared_ptr<SessionLinks> links = m_links.lock();
        if (!links)
        {
            // The session went away while its redirect was in flight. There is
            // nothing for this connection to become.
            sLog.outError("proto: stream 1 arrived from %s for a session that is gone",
                          m_address.c_str());
            return false;
        }

        // SendResumeComms runs inside the attach, after the generation is known
        // good and before the slot goes live. Announcing any earlier hands the
        // client a socket it may adopt and we then close: the promotion clears
        // its pending flag and flushes everything it had queued for stream 1
        // onto a connection that is about to die, and those packets are gone.
        // Announcing any later lets the world's held traffic reach a socket the
        // client still parks in staging, where it refuses all of it.
        if (!links->AttachSlotOne(std::static_pointer_cast<ClientConnection>(shared_from_this()),
                                  m_redirectGeneration,
                                  [this]() { SendResumeComms(); }))
        {
            // A redirect issued after this one superseded it; the live stream is
            // the newer socket's. This one answered too late to be anything.
            sLog.outError("proto: stream 1 from %s answers a superseded redirect for "
                          "session %u; refusing it", m_address.c_str(), m_session);
            return false;
        }
        m_role = ConnRole::Live1;

        DEBUG_LOG("proto: stream 1 live for session %u from %s",
                  m_session, m_address.c_str());
        return true;
    }

    std::vector<uint8_t> ClientConnection::onData(const uint8_t* data, size_t len)
    {
        // Each packet is handled as it completes, before the next header in this
        // same buffer is decoded. HandleAuthSession() arms the header cipher, and
        // the client enciphers everything after CMSG_AUTH_SESSION -- which TCP
        // routinely delivers in one read with the packets that follow it.
        size_t decoded = 0;
        bool   fatal   = false;

        // A short read anywhere below unwinds onto a network worker thread, where
        // nothing else would catch it and the process would abort. Dropping the
        // peer has to be the worst a malformed packet can do.
        DecodeStatus status = DecodeStatus::Ok;
        try
        {
            status = m_codec.Feed(data, len,
                [&](WorldPacket&& packet) -> bool
                {
                    ++decoded;
                    // Before the move, which empties it.
                    m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed),
                                          packet, true, TraceSlot());
                    if (!HandlePacket(std::move(packet)))
                    {
                        fatal = true;
                        return false;
                    }
                    return true;
                });
        }
        catch (ByteBufferException&)
        {
            sLog.outError("proto: short read handling packet from %s, dropping",
                          m_address.c_str());
            Close();
            return std::vector<uint8_t>();
        }

        if (status == DecodeStatus::Malformed)
        {
            // Print the header that failed, not just the fact that one did: a
            // desynchronised header cipher and a peer talking a different
            // protocol produce the same symptom and need opposite fixes.
            const PacketCodec::Failure& bad = m_codec.LastFailure();

            sLog.outError("proto: malformed packet framing from %s, dropping "
                          "(header %.2X %.2X %.2X %.2X %.2X %.2X -> size=%u "
                          "cmd=0x%.4X, header cipher %s, %u byte(s) read, "
                          "%u packet(s) decoded before it)",
                          m_address.c_str(),
                          bad.header[0], bad.header[1], bad.header[2],
                          bad.header[3], bad.header[4], bad.header[5],
                          bad.size, bad.cmd,
                          bad.decrypted ? "armed" : "not armed",
                          uint32(len), uint32(decoded));
            Close();
            return std::vector<uint8_t>();
        }

        // A handler that refused the packet has already said why; it only
        // remains to drop the peer.
        if (fatal)
        {
            Close();
        }

        // Everything this class sends goes through SendPacket() (and therefore the
        // transport's Sender), because a reply may be produced on the world thread
        // long after this call returned. Nothing is ever returned inline.
        return std::vector<uint8_t>();
    }

    void ClientConnection::HandleWowConnection(WorldPacket& packet)
    {
        // WorldSocket.cpp's HandleWowConnection (:398-403): read and ignored,
        // no validation, no state gate. The client's copy of this handshake
        // string arrives whenever it arrives -- onConnect() has already sent
        // the challenge unconditionally by this point.
        std::string clientToServerMsg;
        packet >> clientToServerMsg;
    }

    bool ClientConnection::HandlePacket(WorldPacket&& packet)
    {
        const uint16 opcode = uint16(packet.GetOpcode());

        if (opcode == MSG_WOW_CONNECTION)
        {
            HandleWowConnection(packet);

            m_bannerDone = true;
            return true;
        }

        if (m_role == ConnRole::Staging1)
        {
            // The client frames packets on this socket only once it has adopted
            // it as a stream, so this packet is the promotion happening. It is
            // CMSG_AUTH_CONTINUED_SESSION, and it arrives IN CLEAR: the client
            // sends it and only then arms its ciphers (Wow-64.exe 15595,
            // sub_1400AA560 -- send at sub_1401A9920, key schedule at
            // sub_1401A78D0 three lines later), exactly as it does with
            // CMSG_AUTH_SESSION on stream 0. Arming any earlier decrypts a
            // plaintext header into noise and drops the socket, which the client
            // answers by giving up on the second stream:
            //   "malformed packet framing ... header 6F C6 E3 16 32 58 ->
            //    size=28614 cmd=0x583216E3, header cipher armed, 42 byte(s) read"
            // -- 42 bytes being this packet, whose true header is
            // 00 28 4D 04 00 00: 36 payload bytes under opcode 0x044D.
            //
            // Arm before promoting, not after: promotion hands the session its
            // second stream, and the world may write to it from its own thread
            // the moment it has one.
            // This packet is the handshake or it is nothing. The client sends
            // CMSG_AUTH_CONTINUED_SESSION and nothing else on a socket it still
            // holds in staging -- its own router cannot even reach the socket
            // until the promotion below moves it out. So a peer framing anything
            // else here is not the client we redirected, and letting it through
            // would arm the cipher early: the genuine continued session would
            // then be decrypted as noise and the stream lost, which is the exact
            // failure this handshake was built to end. A redirect ticket is
            // claimed by address alone, so this is reachable by a second peer
            // behind one address, not only by a broken client.
            if (opcode != CMSG_AUTH_CONTINUED_SESSION ||
                packet.size() != CONTINUED_SESSION_SIZE)
            {
                sLog.outError("proto: opcode 0x%.4X (%u bytes) on the staging stream from %s, "
                              "expected CMSG_AUTH_CONTINUED_SESSION; dropping",
                              opcode, uint32(packet.size()), m_address.c_str());
                return false;
            }

            // Before the cipher is armed and before the stream is announced:
            // nothing is committed to a peer that cannot prove it holds the
            // session key.
            if (!VerifyContinuedSession(packet))
            {
                return false;
            }

            if (m_policy.armRedirectedCrypto)
            {
                ArmRedirectedCrypto();
            }

            // The resume rides inside the promotion, between the generation
            // check and the moment the slot goes live -- see PromoteToSlotOne.
            if (!PromoteToSlotOne())
            {
                return false;
            }

            // Transport, not game: the payload is the connect-to key we signed
            // into the redirect, the client's proof-of-work counter, and
            // SHA-1(account || session key || server seed). The world has no
            // handler for it and needs none -- the ticket the socket claimed
            // already bound it to a session and a key.
            //
            // The digest is not checked here because proto does not know the
            // account name; carrying it on the ticket would let this verify the
            // client rather than trust the address it came from.
            return true;
        }

        if (opcode == CMSG_AUTH_SESSION)
        {
            if (m_role != ConnRole::Live0)
            {
                sLog.outError("proto: CMSG_AUTH_SESSION on a redirected connection from %s",
                              m_address.c_str());
                return false;
            }

            if (m_session != INVALID_SESSION_ID)
            {
                sLog.outError("proto: repeated CMSG_AUTH_SESSION from %s",
                              m_address.c_str());
                return false;
            }

            return HandleAuthSession(packet);
        }

        if (m_session == INVALID_SESSION_ID)
        {
            sLog.outError("proto: opcode 0x%.4X from unauthenticated peer %s",
                          opcode, m_address.c_str());
            return false;
        }

        // Record which wire this came in on. Only a reply the client times
        // per-connection needs it, but the connection is the last place that
        // knows: past here a packet is just bytes in a session's mailbox.
        packet.SetStream(TraceSlot());

        m_gateway.Deliver(m_session, std::move(packet));
        return true;
    }

    bool ClientConnection::HandleAuthSession(WorldPacket& packet)
    {
        AuthRequest request;
        request.peerAddress = m_address;

        uint16 clientBuild = 0;

        // Cata's CMSG_AUTH_SESSION scrambles the client's SHA-1 digest bytes and
        // interleaves them with the rest of the fields. Moved verbatim from
        // WorldSocket::HandleAuthSession (WorldSocket.cpp:986-1028) -- this
        // exact read order is wire truth, not a style choice.
        try
        {
            packet.read_skip<uint32>();
            packet.read_skip<uint32>();
            packet.read_skip<uint8>();
            packet >> request.digest[10];
            packet >> request.digest[18];
            packet >> request.digest[12];
            packet >> request.digest[5];
            packet.read_skip<uint64>();
            packet >> request.digest[15];
            packet >> request.digest[9];
            packet >> request.digest[19];
            packet >> request.digest[4];
            packet >> request.digest[7];
            packet >> request.digest[16];
            packet >> request.digest[3];
            packet >> clientBuild;
            packet >> request.digest[8];
            packet.read_skip<uint32>();
            packet.read_skip<uint8>();
            packet >> request.digest[17];
            packet >> request.digest[6];
            packet >> request.digest[0];
            packet >> request.digest[1];
            packet >> request.digest[11];
            packet >> request.clientSeed;
            packet >> request.digest[2];
            packet.read_skip<uint32>();
            packet >> request.digest[14];
            packet >> request.digest[13];

            uint32 addonSize = 0;
            packet >> addonSize;                        // addon data size

            request.addonData.resize(addonSize);
            if (addonSize > 0)
            {
                packet.read(request.addonData.data(), addonSize);
            }

            uint8 nameLenHigh = 0;
            uint8 nameLenLow  = 0;
            packet >> nameLenHigh;
            packet >> nameLenLow;

            const uint8 accNameLen = uint8((nameLenHigh << 5) | (nameLenLow >> 3));
            request.account = packet.ReadString(accNameLen);
        }
        catch (ByteBufferException&)
        {
            sLog.outError("proto: truncated CMSG_AUTH_SESSION from %s",
                          m_address.c_str());
            return false;
        }

        request.build = clientBuild;

        // Policy and persistence: account row, bans, IP lock, allowed build,
        // security level, client OS. None of it belongs on this side of the seam.
        const AuthLookup lookup = m_gateway.LookupAccount(request);

        if (lookup.status != AuthStatus::Ok)
        {
            SendAuthStatus(lookup.status);
            sLog.outError("proto: login refused for account '%s' from %s (code 0x%.2X)",
                          request.account.c_str(), m_address.c_str(),
                          uint32(lookup.status));
            return false;
        }

        // Cryptography stays on this side. The client proves it holds the same
        // session key realmd handed it, over both halves of the nonce.
        // WorldSocket.cpp:1194-1206, moved verbatim.
        BigNumber sessionKey = lookup.sessionKey;
        const uint32 zero       = 0;
        const uint32 clientSeed = request.clientSeed;
        const uint32 serverSeed = m_seed;

        Sha1Hash sha;
        sha.UpdateData(request.account);
        sha.UpdateData(reinterpret_cast<const uint8*>(&zero), 4);
        sha.UpdateData(reinterpret_cast<const uint8*>(&clientSeed), 4);
        sha.UpdateData(reinterpret_cast<const uint8*>(&serverSeed), 4);
        sha.UpdateBigNumbers(&sessionKey, NULL);
        sha.Finalize();

        if (std::memcmp(sha.GetDigest(), request.digest, AUTH_DIGEST_SIZE) != 0)
        {
            SendAuthStatus(AuthStatus::Failed);
            sLog.outError("proto: bad login proof for account '%s' from %s",
                          request.account.c_str(), m_address.c_str());
            return false;
        }

        // Arm the cipher BEFORE the world is told, because the world answers with
        // SMSG_AUTH_RESPONSE (or a queue position) the moment it accepts the
        // session (WorldSession::SendAuthWaitQue), and that reply must already be
        // encrypted. WorldSocket.cpp arms at :1235, right after the proof check
        // and before the WorldSession is constructed.
        m_crypt.Init(&sessionKey);
        m_codec.SetHeaderDecryptor(
            [this](uint8* header, size_t len) { m_crypt.DecryptRecv(header, len); });

        // Hand the world a share of our own lifetime. net::ISession is held by
        // shared_ptr from the moment the transport accepts, so this is well
        // formed here, and it is what allows a WorldSession to outlive its socket
        // long enough to unwind the player from the map.
        std::shared_ptr<IClientLink> link =
            std::static_pointer_cast<ClientConnection>(shared_from_this());

        const SessionId session = m_gateway.Attach(request, link, lookup.context);
        if (session == INVALID_SESSION_ID)
        {
            SendAuthStatus(AuthStatus::SystemError);
            return false;
        }

        m_session = session;
        m_traceSession.store(session, std::memory_order_relaxed);

        DEBUG_LOG("proto: account '%s' authenticated from %s",
                  request.account.c_str(), m_address.c_str());
        return true;
    }

    void ClientConnection::SendAuthStatus(AuthStatus status)
    {
        // Cata's failure SMSG_AUTH_RESPONSE is bit-packed: two false bits (the
        // has-queue-data / has-account-data flags) ahead of the status byte,
        // repeated identically at WorldSocket.cpp:1038-1041, :1074-1077,
        // :1111-1114 and :1208-1211. This is NOT MangosTwo's plain byte stream.
        WorldPacket packet(SMSG_AUTH_RESPONSE, 2);
        packet.WriteBit(false);
        packet.WriteBit(false);
        packet << uint8(status);
        SendPacket(packet);
    }

    void ClientConnection::SendPacket(const WorldPacket& packet)
    {
        if (m_closed.load(std::memory_order_acquire) || !m_sender)
        {
            return;
        }

        m_gateway.TracePacket(m_traceSession.load(std::memory_order_relaxed), packet, false,
                              TraceSlot());

        // The cipher is a stream, so order matters twice over: two threads must
        // not encrypt concurrently, and the bytes must reach the transport in the
        // same order they were enciphered. The lock used to cover only the first
        // of those. Thread A would take the keystream for its header, thread B
        // would take the next stretch for its own, and then B could reach the
        // send first -- putting the two packets on the wire in the order the
        // keystream says they are not. The client decrypts strictly in arrival
        // order, so it reads A's bytes against B's keystream, sees a header that
        // decodes to nothing, and drops the connection. That is the same symptom
        // as a dozen unrelated faults, and it would strike only under concurrent
        // sends on one connection -- which the world thread and the network
        // threads do to a busy session constantly.
        //
        // So the send stays inside the lock. It is a queue append
        // (SendChannel::post -> ConnCtx::enqueue), not a blocking write, so this
        // holds the lock for an append and never waits on the network.
        std::lock_guard<std::mutex> lock(m_cryptSendLock);

        const std::vector<uint8_t> wire = PacketCodec::Encode(packet,
            [this](uint8* header, size_t len)
            {
                if (m_crypt.IsInitialized())
                {
                    m_crypt.EncryptSend(header, len);
                }
            });

        m_sender(wire.data(), wire.size());
    }

    void ClientConnection::Close()
    {
        m_closed.store(true, std::memory_order_release);
        if (m_closer)
        {
            m_closer();
        }
    }

    void ClientConnection::onClose()
    {
        m_closed.store(true, std::memory_order_release);

        // Losing the second stream is a fault to recover from, not the end of the
        // session: the player is still connected, still in the world, and still
        // talking on stream 0. Detaching here would log them out for a broken
        // socket that the session is about to ask the client to re-open.
        if (m_role != ConnRole::Live0)
        {
            if (const std::shared_ptr<SessionLinks> links = m_links.lock())
            {
                links->DetachSlotOne(this);
            }
            m_links.reset();

            if (m_role == ConnRole::Live1)
            {
                // Not an error here, and it used to say it was on every single
                // logout: a client leaving closes both of its sockets, so this
                // fires on the most ordinary event there is. Whether losing the
                // stream MATTERS is a question about the session -- is there
                // still a player in the world who needs it? -- and proto cannot
                // see that. WorldSession::UpdateSecondStream can, and already
                // logs the real error ("lost its second world stream; asking the
                // client to open it again") on exactly the path where it is one.
                DEBUG_LOG("proto: stream 1 closed for session %u (%s)",
                          m_session, m_address.c_str());
            }

            m_session = INVALID_SESSION_ID;
            m_traceSession.store(INVALID_SESSION_ID, std::memory_order_relaxed);
            return;
        }

        if (m_session != INVALID_SESSION_ID)
        {
            m_gateway.Detach(m_session);
            m_session = INVALID_SESSION_ID;
        }
        m_traceSession.store(INVALID_SESSION_ID, std::memory_order_relaxed);
    }
}
