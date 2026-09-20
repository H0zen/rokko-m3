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
#include "WorldNetwork.h"

#include "ClientConnection.h"
#include "World.h"
#include "Config/Config.h"
#include "Log/Log.h"
#include "OpcodeTable.h"
#include "ServerRouting.h"
#include "SessionLinks.h"

#include <chrono>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace
{
    /// How long a client has to come back on the second port before the pending
    /// redirect is discarded. It is a TCP connect away, so this is generous.
    const uint32 DEFAULT_REDIRECT_TIMEOUT_MS = 15000;

    proto::EndpointPolicy MakePolicy(proto::ConnRole role)
    {
        proto::EndpointPolicy policy;
        policy.role = role;
        policy.armRedirectedCrypto =
            sConfig.GetBoolDefault("Redirect.Stream1Crypt", true);
        return policy;
    }
}

WorldNetwork::WorldNetwork()
    : m_streamPort(0),
      m_gateway(),
      m_redirectTimeout(sConfig.GetIntDefault("Redirect.Timeout", DEFAULT_REDIRECT_TIMEOUT_MS)),
      m_redirects(m_redirectTimeout),
      m_listener(m_gateway, m_redirects, MakePolicy(proto::ConnRole::Live0)),
      m_streamListener(m_gateway, m_redirects, MakePolicy(proto::ConnRole::Staging1))
{
}

WorldNetwork::~WorldNetwork()
{
    Stop();
}

bool WorldNetwork::LoadRedirectConfiguration(const std::string& bindIp)
{
    // Baked data, not configuration: it is written into DataDir by the same run
    // of mep that bakes the tiles, and it is read from there the way a .tile is.
    // It used to be Redirect.SecretFile, a path an operator could point anywhere
    // -- including at last generation's key, which patches no client anyone has.
    const std::string secretFile = sWorld.GetDataPath() + "keys/server.secret";

    if (!m_signer.LoadFromFile(secretFile))
    {
        sLog.outError("No redirect keypair at '%s'. Without one no client can be sent "
                      "to the second world stream, and no client can enter the world.",
                      secretFile.c_str());
        sLog.outError("Run 'mep bake' against a 4.3.4 client: it mints the pair into "
                      "<DataDir>/keys and patches your client from the client.secret "
                      "beside it. 'mep mint --out-dir <DataDir>/keys' does the key "
                      "alone. Or correct DataDir in mangosd.conf.");
        return false;
    }

    // The address the client is told to connect to. It must be reachable from
    // where the client is, which is why a wildcard bind cannot supply it: the
    // server has no way to know which of its interfaces the client used.
    std::string advertised = sConfig.GetStringDefault("Redirect.Address", "");
    if (advertised.empty())
    {
        advertised = bindIp;
    }

    if (advertised.empty() || advertised == "0.0.0.0")
    {
        sLog.outError("Redirect.Address is not set and BindIP is a wildcard, so there "
                      "is no address to send clients to for the second world stream. "
                      "Set Redirect.Address to the address clients reach this server on.");
        return false;
    }

    in_addr parsed;
    if (inet_pton(AF_INET, advertised.c_str(), &parsed) != 1)
    {
        sLog.outError("Redirect.Address '%s' is not a valid IPv4 address",
                      advertised.c_str());
        return false;
    }

    const uint8* bytes = reinterpret_cast<const uint8*>(&parsed);
    m_advertisedAddress.assign(bytes, bytes + sizeof(parsed));

    // The client checks the auth blob against a digest baked into its own image,
    // so the two are patched as a pair. Printing it is what lets an operator
    // match a client to the running configuration rather than guess at it.
    sLog.outString("World: second stream at %s:%u, client digest %s",
                   advertised.c_str(), uint32(m_streamPort),
                   m_signer.ExpectedClientDigest().c_str());

    return true;
}

bool WorldNetwork::Start(uint16 port, uint16 streamPort, const std::string& bindIp)
{
    // Must happen before the listener opens: opcodeTable is a plain array with
    // static storage, so until this runs every entry is name = nullptr,
    // handler = nullptr. A connection arriving first would dispatch through a
    // null handler.
    //
    // WorldSocketMgr's constructor used to make this call (WorldSocketMgr.cpp:76).
    // That class is gone, and its replacement (proto::Listener) sits on the far
    // side of the networking boundary and must not know game opcodes exist -- so
    // the call belongs here, on the game side, which is the last place that owns
    // both.
    InitializeOpcodes();

    // Which opcodes the second stream carries beyond the ones the client demands
    // (proto::PrefersStreamOne). Configurable because the cost of a wrong
    // judgement is paid on a live realm and the symptom -- a loot window that
    // does not open -- deserves an answer that is not a rebuild.
    const bool extraRouting = sConfig.GetBoolDefault("SecondStream.ExtraRouting", true);
    proto::SetStreamOnePolicyEnabled(extraRouting);
    if (!extraRouting)
    {
        sLog.outString("World: second stream carries only the opcodes the client "
                       "requires (SecondStream.ExtraRouting = 0)");
    }

    m_streamPort = streamPort;

    if (!LoadRedirectConfiguration(bindIp))
    {
        return false;
    }

    if (port == streamPort)
    {
        sLog.outError("WorldServerPort and WorldServerStreamPort are both %u; the two "
                      "world streams need two ports", uint32(port));
        return false;
    }

    if (!m_listener.Start(port, bindIp))
    {
        sLog.outError("Failed to bind the world listener to %s:%u",
                      bindIp.empty() ? "0.0.0.0" : bindIp.c_str(), uint32(port));
        return false;
    }

    if (!m_streamListener.Start(streamPort, bindIp))
    {
        sLog.outError("Failed to bind the second world stream to %s:%u",
                      bindIp.empty() ? "0.0.0.0" : bindIp.c_str(), uint32(streamPort));
        m_listener.Stop();
        return false;
    }

    return true;
}

void WorldNetwork::Stop()
{
    m_streamListener.Stop();
    m_listener.Stop();
}

uint32 WorldNetwork::GetOpenConnectionCount() const
{
    return proto::ClientConnection::GetOpenConnectionCount();
}

RedirectRequest WorldNetwork::RequestSecondStream(const proto::RedirectTicket& ticket)
{
    if (!m_signer.IsLoaded() || !ticket.links)
    {
        return RedirectRequest::Failed;
    }

    // The client refuses a redirect while one is already staged for that stream,
    // and two clients behind one address cannot be told apart when they come
    // back. Both cases are the registry declining to open a second ticket.
    if (!m_redirects.Open(ticket))
    {
        return RedirectRequest::Busy;
    }

    WorldPacket packet;
    if (!m_signer.BuildConnectTo(m_advertisedAddress, proto::RedirectFamily::IPv4,
                                 m_streamPort, proto::LinkSlot::One, packet))
    {
        m_redirects.Cancel(ticket.session);
        return RedirectRequest::Failed;
    }

    // On the stream the client already has -- the redirect is what creates the
    // other one, so it cannot travel on it.
    ticket.links->SendOn(proto::LinkSlot::Zero, packet);
    return RedirectRequest::Sent;
}

void WorldNetwork::CancelSecondStream(proto::SessionId session)
{
    m_redirects.Cancel(session);
}

void WorldNetwork::ExpirePendingRedirects()
{
    m_redirects.ExpireStale();
}
