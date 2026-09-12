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

// mangos-loadtest -- drives synthetic 4.3.4 sessions against a running mangosd.
//
// Phase 1 is one client that logs in and holds the session. That is the piece
// that de-risks the rest: it proves this side of the protocol against the real
// server, and everything a larger harness would do is that sequence repeated.
//
// The session key is not negotiated. Rather than reimplement SRP-6 and a second
// server's protocol, the harness derives a key from the account name and the
// operator writes that same value into `account.sessionkey` -- `--emit-sql`
// prints the statement. A derived key means no state travels between the two
// steps: run the SQL once, then run any number of logins, in any order, from any
// machine, with nothing to keep in sync.
//
// Nothing here weakens a check. The world server still verifies
// SHA-1(account || 0 || client seed || server seed || K) on the first stream and
// SHA-1(account || K || server seed) on the second, against the row it reads
// from its own database. It cannot tell this client from a real one, which is
// the point.

#include "AckEngine.hpp"
#include "Replay.hpp"
#include "SyntheticClient.hpp"

#include "Auth/Sha1.h"
#include "Opcodes.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    void Usage()
    {
        std::printf(
            "mangos-loadtest -- synthetic 4.3.4 client\n"
            "\n"
            "  --account NAME       account to log in as (upper case, as realmd stores it)\n"
            "  --character GUID     `characters`.`guid` of a character on that account\n"
            "  --host ADDRESS       world server address        (default 127.0.0.1)\n"
            "  --port N             first world stream          (default 8085)\n"
            "  --stream-port N      second world stream         (default 8086)\n"
            "  --build N            client build to claim       (default 15595)\n"
            "  --hold SECONDS       stay in world afterwards    (default 0)\n"
            "  --key HEX            session key, 80 hex digits; overrides the derived one\n"
            "  --emit-sql           print the SQL that plants the derived key and the client OS, and exit\n"
            "  --verbose            trace each milestone as it is reached\n"
            "\n"
            "Protocol peer (movement P0-B):\n"
            "  --replay FILE        judge a capture file offline: decode every line the wire knows -- a registry layout or a family -- re-encode, compare bytes; exit 0 when clean\n"
            "  --walk SECONDS       walk straight ahead for SECONDS once in the world\n"
            "  --heading DEGREES    walk in this direction instead of the character's facing\n"
            "  --return             walk the same time back, so the character ends where it began\n"
            "  --ack MODE           answer movement changes: immediate | delay:MS | mismatch | stale | wrongvalue | drop\n"
            "  --observe GUID       count relayed movement of this mover\n"
            "  --select GUID        select and walk as this guid regardless of grants (the authority-violation scenario)\n"
            "  --expect a,b,c       require the run to have seen these: teleport | knockback | splines |\n"
            "                       resend | resync | kick | updates:N | noupdates\n"
            "                       (updates:N and noupdates read --pair's observer, or this run's own\n"
            "                       --observe when it has no --pair)\n"
            "  --pair ACCOUNT:GUID  run a second, observing session of that character alongside,\n"
            "                       watching this one, and print the relay verdict\n"
            "\n"
            "Typical use:\n"
            "  mangos-loadtest --account LOAD01 --emit-sql | mysql -u root realmd\n"
            "  mangos-loadtest --account LOAD01 --character 9 --hold 60 --verbose\n"
            "  mangos-loadtest --account RNDBOT0 --character 46 --walk 6 --return --hold 12 --pair RNDBOT1:47\n");
    }

    /**
     * @brief A 40-byte session key derived from the account name alone.
     *
     * Deterministic on purpose: the value must be identical in the SQL that
     * plants it and in the login that uses it, and deriving it means those two
     * runs need share nothing -- no file, no argument, no ordering.
     *
     * It is not a secret and is not meant to be one. These are throwaway load-test
     * accounts on a server the operator already controls; anything stronger would
     * add a key-distribution problem to a harness in order to protect accounts
     * that exist to be logged into by a program.
     */
    std::string DeriveSessionKeyHex(const std::string& account)
    {
        std::string hex;
        hex.reserve(80);

        // Two SHA-1s give the 40 bytes realmd's own key is; the counter keeps the
        // halves from being the same digest.
        for (uint32 block = 0; block < 2; ++block)
        {
            Sha1Hash sha;
            sha.UpdateData(std::string("mangos-loadtest/v1"));
            sha.UpdateData(account);
            sha.UpdateData(reinterpret_cast<const uint8*>(&block), sizeof(block));
            sha.Finalize();

            char byteText[3];
            for (size_t i = 0; i < 20; ++i)
            {
                std::snprintf(byteText, sizeof(byteText), "%.2X", sha.GetDigest()[i]);
                hex += byteText;
            }
        }
        return hex;
    }

    bool WantsValue(int argc, int index, const char* flag)
    {
        if (index + 1 < argc)
        {
            return true;
        }
        std::fprintf(stderr, "%s needs a value\n", flag);
        return false;
    }
}

int main(int argc, char** argv)
{
    loadtest::Config config;
    std::string keyHex;
    bool emitSql = false;
    std::string replayPath;
    std::string pairAccount;
    uint64      pairGuid = 0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            Usage();
            return 0;
        }
        else if (arg == "--emit-sql")   { emitSql = true; }
        else if (arg == "--replay")
        {
            if (!WantsValue(argc, i, "--replay")) { return 2; }
            replayPath = argv[++i];
        }
        else if (arg == "--verbose")    { config.verbose = true; }
        else if (arg == "--account")
        {
            if (!WantsValue(argc, i, "--account")) { return 2; }
            config.account = argv[++i];
        }
        else if (arg == "--character")
        {
            if (!WantsValue(argc, i, "--character")) { return 2; }
            config.characterGuid = std::strtoull(argv[++i], NULL, 10);
        }
        else if (arg == "--host")
        {
            if (!WantsValue(argc, i, "--host")) { return 2; }
            config.host = argv[++i];
        }
        else if (arg == "--port")
        {
            if (!WantsValue(argc, i, "--port")) { return 2; }
            config.port = uint16(std::strtoul(argv[++i], NULL, 10));
        }
        else if (arg == "--stream-port")
        {
            if (!WantsValue(argc, i, "--stream-port")) { return 2; }
            config.streamPort = uint16(std::strtoul(argv[++i], NULL, 10));
        }
        else if (arg == "--build")
        {
            if (!WantsValue(argc, i, "--build")) { return 2; }
            config.build = uint16(std::strtoul(argv[++i], NULL, 10));
        }
        else if (arg == "--hold")
        {
            if (!WantsValue(argc, i, "--hold")) { return 2; }
            config.holdSeconds = uint32(std::strtoul(argv[++i], NULL, 10));
        }
        else if (arg == "--key")
        {
            if (!WantsValue(argc, i, "--key")) { return 2; }
            keyHex = argv[++i];
        }
        else if (arg == "--walk")
        {
            if (!WantsValue(argc, i, "--walk")) { return 2; }
            config.script.walk.seconds = uint32(std::strtoul(argv[++i], NULL, 10));
        }
        else if (arg == "--heading")
        {
            if (!WantsValue(argc, i, "--heading")) { return 2; }
            config.script.walk.headingSet = true;
            config.script.walk.heading = float(std::strtod(argv[++i], NULL) * 3.14159265358979323846 / 180.0);
        }
        else if (arg == "--return")     { config.script.walk.returnHome = true; }
        else if (arg == "--ack")
        {
            if (!WantsValue(argc, i, "--ack")) { return 2; }
            const std::string mode = argv[++i];
            if (mode == "immediate")            { config.script.ack.mode = loadtest::AckMode::Immediate; }
            else if (mode == "mismatch")        { config.script.ack.mode = loadtest::AckMode::Mismatch; }
            else if (mode == "drop")            { config.script.ack.mode = loadtest::AckMode::Drop; }
            else if (mode == "stale")           { config.script.ack.mode = loadtest::AckMode::Stale; }
            else if (mode == "wrongvalue")      { config.script.ack.mode = loadtest::AckMode::WrongValue; }
            else if (mode.compare(0, 6, "delay:") == 0)
            {
                config.script.ack.mode = loadtest::AckMode::Delay;
                config.script.ack.delayMs = uint32(std::strtoul(mode.c_str() + 6, NULL, 10));
            }
            else
            {
                std::fprintf(stderr, "--ack wants immediate, delay:MS, mismatch, stale, wrongvalue or drop\n");
                return 2;
            }
        }
        else if (arg == "--expect")
        {
            if (!WantsValue(argc, i, "--expect")) { return 2; }
            const std::string list = argv[++i];
            size_t at = 0;
            while (at <= list.size())
            {
                const size_t comma = list.find(',', at);
                const size_t end = comma == std::string::npos ? list.size() : comma;
                const std::string what = list.substr(at, end - at);
                if (!what.empty()) { config.script.expect.push_back(what); }
                if (comma == std::string::npos) { break; }
                at = comma + 1;
            }
        }
        else if (arg == "--observe")
        {
            if (!WantsValue(argc, i, "--observe")) { return 2; }
            config.script.observeGuid = std::strtoull(argv[++i], NULL, 10);
        }
        else if (arg == "--select")
        {
            if (!WantsValue(argc, i, "--select")) { return 2; }
            config.script.selectGuid = std::strtoull(argv[++i], NULL, 10);
        }
        else if (arg == "--pair")
        {
            if (!WantsValue(argc, i, "--pair")) { return 2; }
            const std::string spec = argv[++i];
            const size_t colon = spec.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size())
            {
                std::fprintf(stderr, "--pair wants ACCOUNT:GUID\n");
                return 2;
            }
            pairAccount = spec.substr(0, colon);
            pairGuid = std::strtoull(spec.c_str() + colon + 1, NULL, 10);
        }
        else
        {
            std::fprintf(stderr, "unknown option: %s\n\n", arg.c_str());
            Usage();
            return 2;
        }
    }

    if (!replayPath.empty())
    {
        // Offline: no server, no account. Every line of the capture through the
        // codec the wire has for it -- a registry layout or a family -- and back,
        // judged by the bytes.
        std::ifstream capture(replayPath.c_str());
        if (!capture)
        {
            std::fprintf(stderr, "cannot open %s\n", replayPath.c_str());
            return 2;
        }
        const loadtest::ReplayReport report = loadtest::Replay(capture);
        loadtest::PrintReplay(report, stdout);
        return report.Clean() ? 0 : 1;
    }

    if (config.account.empty())
    {
        std::fprintf(stderr, "--account is required\n\n");
        Usage();
        return 2;
    }

    // The account name is hashed into both proofs verbatim, and realmd stores it
    // upper case -- so a lower-case argument fails as a wrong password would,
    // with nothing to say why. Normalise it here instead.
    for (size_t i = 0; i < config.account.size(); ++i)
    {
        config.account[i] = char(std::toupper(static_cast<unsigned char>(config.account[i])));
    }

    for (size_t i = 0; i < pairAccount.size(); ++i)
    {
        pairAccount[i] = char(std::toupper(static_cast<unsigned char>(pairAccount[i])));
    }
    // A walk needs the session to outlive it, with room for the lead and the stop.
    if (config.script.walk.seconds > 0)
    {
        const uint32 legs = config.script.walk.returnHome ? 2 : 1;
        const uint32 needed = config.script.walk.seconds * legs + config.script.walk.leadMs / 1000 + 3;
        if (config.holdSeconds < needed)
        {
            config.holdSeconds = needed;
        }
    }

    if (keyHex.empty())
    {
        keyHex = DeriveSessionKeyHex(config.account);
    }
    if (keyHex.size() != 80)
    {
        std::fprintf(stderr, "a session key is 40 bytes, so 80 hex digits (got %u)\n",
                     uint32(keyHex.size()));
        return 2;
    }

    if (emitSql)
    {
        // `sessionkey` is what the world server reads to verify the login proof,
        // and it is the only column that has to change: the account row itself is
        // the operator's to create, with whatever password realmd wants.
        std::printf("UPDATE `account` SET `sessionkey` = '%s' WHERE `username` = '%s';\n",
                    keyHex.c_str(), config.account.c_str());
        // The world gateway refuses any client OS but Win/OSX (AUTH_REJECT, 0x0E),
        // and an account that never went through realmd has an empty one.
        std::printf("UPDATE `account` SET `os` = 'Win' WHERE `username` = '%s';\n",
                    config.account.c_str());
        std::printf("-- then, against the character database, to find a guid to log in:\n");
        std::printf("--   SELECT `guid`, `name` FROM `characters` WHERE `account` = "
                    "(SELECT `id` FROM `realmd`.`account` WHERE `username` = '%s');\n",
                    config.account.c_str());
        return 0;
    }

    config.sessionKey.SetHexStr(keyHex.c_str());

    std::string error;
    if (!loadtest::InitSockets(error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    std::printf("logging in as '%s' to %s:%u / :%u\n",
                config.account.c_str(), config.host.c_str(),
                uint32(config.port), uint32(config.streamPort));

    // The paired session logs in first and simply watches. The primary waits
    // until it is in the world (or has failed to get there) before logging in
    // itself: the redirect registry serialises logins from one address, and a
    // cold grid load can outlast the walk's lead. Its own key is derived from
    // its account name like the primary's.
    loadtest::Result pairResult;
    std::thread pairThread;
    std::mutex pairMutex;
    std::condition_variable pairSettled;
    bool pairReady = false;
    const auto settle = [&pairMutex, &pairSettled, &pairReady]()
    {
        {
            std::lock_guard<std::mutex> lock(pairMutex);
            pairReady = true;
        }
        pairSettled.notify_all();
    };
    if (!pairAccount.empty())
    {
        loadtest::Config pairConfig = config;
        pairConfig.account = pairAccount;
        pairConfig.sessionKey.SetHexStr(DeriveSessionKeyHex(pairAccount).c_str());
        pairConfig.characterGuid = pairGuid;
        pairConfig.script = loadtest::PeerScript();
        pairConfig.script.observeGuid = config.characterGuid;
        pairConfig.onInWorld = settle;
        // It entered the world first and must still be watching at the walker's
        // last stop, so it holds a little longer than the primary.
        pairConfig.holdSeconds = config.holdSeconds + 3;
        std::printf("pairing '%s' (character %llu), watching character %llu\n",
                    pairAccount.c_str(), static_cast<unsigned long long>(pairGuid),
                    static_cast<unsigned long long>(config.characterGuid));
        pairThread = std::thread([pairConfig, &pairResult, &settle]()
        {
            loadtest::SyntheticClient pair(pairConfig);
            pairResult = pair.Run();
            settle();   // a login that never reached the world settles here
        });

        std::unique_lock<std::mutex> lock(pairMutex);
        if (!pairSettled.wait_for(lock, std::chrono::seconds(30), [&pairReady]() { return pairReady; }))
        {
            std::printf("the observer has not reached the world after 30 s; going on without it\n");
        }
    }

    loadtest::SyntheticClient client(config);
    const loadtest::Result result = client.Run();
    if (pairThread.joinable())
    {
        pairThread.join();
    }
    loadtest::ShutdownSockets();

    // Reaching the world is not the whole of it: a session dropped during the
    // hold has an error and the world stage both, and reporting that as OK would
    // hide exactly the failure a long hold exists to catch -- unless the kick was
    // the expectation, when the server closing the socket is the run's proper end.
    const bool kickExpected = std::find(config.script.expect.begin(), config.script.expect.end(), "kick") != config.script.expect.end();
    const bool ok = result.Reached(loadtest::Stage::InWorld) && (result.error.empty() || (kickExpected && result.peer.kicked));

    std::printf("\n%s: %s\n", ok ? "OK" : "FAILED", loadtest::StageName(result.stage));
    if (!result.error.empty())
    {
        std::printf("  %s\n", result.error.c_str());
    }

    if (result.msToAuth > 0.0)
    {
        std::printf("  authenticated   %8.1f ms\n", result.msToAuth);
    }
    if (result.msToRedirect > 0.0)
    {
        std::printf("  redirected      %8.1f ms\n", result.msToRedirect);
    }
    if (result.msToStream1 > 0.0)
    {
        std::printf("  stream 1 live   %8.1f ms\n", result.msToStream1);
    }
    if (result.msToWorld > 0.0)
    {
        std::printf("  in world        %8.1f ms\n", result.msToWorld);
    }

    // The split this harness exists to measure. Today only 19 opcodes are gated
    // onto the second stream, so a login is expected to be lopsided; printing it
    // per run is what makes a change in the routing policy visible. The first
    // pair is what the server sent to each stream; the second is what this
    // client sent on each, which is what shows the routing the client's send
    // router prescribes.
    std::printf("  stream 0        %u packet(s), %llu byte(s)\n",
                result.packetsOnStream0,
                static_cast<unsigned long long>(result.bytesOnStream0));
    std::printf("  stream 1        %u packet(s), %llu byte(s)\n",
                result.packetsOnStream1,
                static_cast<unsigned long long>(result.bytesOnStream1));
    std::printf("  sent on stream 0  %u packet(s), %llu byte(s)\n",
                result.sentOnStream0,
                static_cast<unsigned long long>(result.sentBytesOnStream0));
    std::printf("  sent on stream 1  %u packet(s), %llu byte(s)\n",
                result.sentOnStream1,
                static_cast<unsigned long long>(result.sentBytesOnStream1));

    const loadtest::PeerReport& peer = result.peer;
    std::printf("PEER timesync answered=%u control granted=%u revoked=%u selects=%u notactive=%u mover=" UI64FMTD " other=%u\n",
                peer.timeSyncsAnswered, peer.controlGranted, peer.controlRevoked, peer.selectsSent, peer.notActiveSent, peer.moverGuid, peer.otherPackets);
    std::printf("PEER walk start=%u heartbeats=%u stop=%u relocations=%u final=%.1f %.1f %.1f lastTime=%u\n",
                peer.walkStarts, peer.walkHeartbeats, peer.walkStops, peer.relocations,
                peer.walkFinal.x, peer.walkFinal.y, peer.walkFinal.z, peer.walkLastTime);
    std::printf("PEER acks sent=%u dropped=%u pending=%u unregistered=",
                peer.acksSent, peer.acksDropped, peer.acksPending);
    for (std::map<uint16, uint32>::const_iterator it = peer.unregisteredChanges.begin();
         it != peer.unregisteredChanges.end(); ++it)
    {
        std::printf("0x%.4X:%u ", uint32(it->first), it->second);
    }
    std::printf("\n");
    std::printf("PEER teleports=%u/%u knockbacks=%u/%u activeMover=%u splines=%u\n",
                peer.teleports, peer.teleportAcks, peer.knockBacks, peer.knockBackAcks,
                peer.activeMoverSets, peer.monsterMoves);
    std::printf("PEER decodefail ");
    for (std::map<uint16, uint32>::const_iterator it = peer.decodeFailures.begin();
         it != peer.decodeFailures.end(); ++it)
    {
        std::printf("0x%.4X:%u ", uint32(it->first), it->second);
    }
    std::printf("\n");

    bool verdictsOk = true;
    // The verdicts judge what Serve did, and a plain login (no hold) never
    // served: it keeps exiting 0 on reaching the world, as it did before the peer.
    if (config.holdSeconds == 0)
    {
        std::printf("PEER VERDICT timesync SKIPPED (no hold)\n");
    }
    else
    {
        // The server sends a time-sync request at login and every ten seconds;
        // any hold long enough to answer one proves the responder.
        const bool timesyncOk = peer.timeSyncsAnswered >= 1;
        std::printf("PEER VERDICT timesync %s (answered %u)\n", timesyncOk ? "OK" : "BUG",
                    peer.timeSyncsAnswered);
        verdictsOk = verdictsOk && timesyncOk;
    }

    if (config.script.walk.seconds > 0)
    {
        // One stop per leg, and one start per leg PLUS one per relocation: a
        // teleport that lands mid-leg ends that leg where it stands, sends no
        // stop for it (a stop from the old place would be a lie about where the
        // mover is), and opens a fresh leg with a fresh start -- so the leg that
        // was interrupted pays its stop only when the restarted leg finishes.
        // starts == legs + relocations and stops == legs is that arithmetic;
        // without the relocations term an ordinary teleport read as a lost walk.
        // Heartbeats at three quarters of the nominal cadence or better: a real
        // client sends no overdue heartbeat retroactively, so the slack is what
        // keeps a stall of this process from reading as the server losing the walk.
        const uint32 legs = config.script.walk.returnHome ? 2 : 1;
        const uint32 nominal = config.script.walk.seconds * 1000 / config.script.walk.heartbeatMs * legs;
        const bool walkOk = peer.walkStarts == legs + peer.relocations && peer.walkStops == legs &&
                            peer.walkHeartbeats * 4 >= nominal * 3;
        std::printf("PEER VERDICT walk %s (start %u, heartbeats %u of %u nominal, stop %u, legs %u, relocations %u)\n",
                    walkOk ? "OK" : "BUG", peer.walkStarts, peer.walkHeartbeats, nominal,
                    peer.walkStops, legs, peer.relocations);
        verdictsOk = verdictsOk && walkOk;
    }

    if (config.holdSeconds > 0)
    {
        // Every change the server sent must have decoded with its registry layout,
        // and the only changes still allowed to have no layout are the two rate
        // changes no source has an ack layout for. The knockback and the teleport
        // were exempt until P1-C: they have families now, the peer answers them
        // from their own codecs, and a change with neither a layout nor a family
        // is a defect again. This verdict covers every packet the peer decodes,
        // not only the changes -- nobody should narrow it later. A delayed ack
        // still pending when the hold ended is a change that went unanswered.
        bool acksOk = peer.decodeFailures.empty() && peer.acksPending == 0;
        for (std::map<uint16, uint32>::const_iterator it = peer.unregisteredChanges.begin();
             it != peer.unregisteredChanges.end(); ++it)
        {
            const uint16 op = it->first;
            if (op != SMSG_MOVE_SET_TURN_RATE && op != SMSG_MOVE_SET_PITCH_RATE)
            {
                acksOk = false;
            }
        }
        std::printf("PEER VERDICT acks %s (sent %u, dropped %u, pending %u)\n", acksOk ? "OK" : "BUG",
                    peer.acksSent, peer.acksDropped, peer.acksPending);
        verdictsOk = verdictsOk && acksOk;
    }

    // --expect names what the run must actually have seen. A family the server
    // never sent leaves its counters at zero, which reads as "the codec works"
    // to anyone skimming the PEER line; naming it makes the absence a BUG.
    for (size_t i = 0; i < config.script.expect.size(); ++i)
    {
        const std::string& what = config.script.expect[i];
        bool ok = false;
        if (what == "teleport")
        {
            ok = peer.teleports >= 1 && peer.teleportAcks == peer.teleports;
            std::printf("PEER VERDICT teleport %s (%u seen, %u acked, at %.1f %.1f %.1f)\n", ok ? "OK" : "BUG",
                        peer.teleports, peer.teleportAcks, peer.teleportFinal.x, peer.teleportFinal.y, peer.teleportFinal.z);
        }
        else if (what == "knockback")
        {
            ok = peer.knockBacks >= 1 && peer.knockBackAcks == peer.knockBacks;
            std::printf("PEER VERDICT knockback %s (%u seen, %u acked)\n", ok ? "OK" : "BUG", peer.knockBacks, peer.knockBackAcks);
        }
        else if (what == "splines")
        {
            ok = peer.monsterMoves >= 1 && peer.decodeFailures.count(SMSG_MONSTER_MOVE) == 0
                 && peer.decodeFailures.count(SMSG_MONSTER_MOVE_TRANSPORT) == 0;
            std::printf("PEER VERDICT splines %s (%u monster moves decoded exact)\n", ok ? "OK" : "BUG", peer.monsterMoves);
        }
        else if (what == "resend")
        {
            // Every speed change the server sent was answered wrong and resent once, and
            // not again: each speed opcode seen exactly twice.
            uint32 twice = 0, other = 0;
            for (std::map<uint16, uint32>::const_iterator it = peer.changesSeen.begin(); it != peer.changesSeen.end(); ++it)
            {
                if (!loadtest::IsSpeedChange(it->first)) { continue; }
                if (it->second == 2) { ++twice; } else { ++other; }
            }
            ok = twice >= 7 && other == 0;
            std::printf("PEER VERDICT resend %s (%u speed changes seen twice, %u otherwise)\n", ok ? "OK" : "BUG", twice, other);
        }
        else if (what == "resync")
        {
            // Under the timeout ladder with every ack dropped: the original, its resend,
            // the resync's reissue -- at least three of each speed change -- and the
            // teleport that snaps the client.
            uint32 thrice = 0;
            for (std::map<uint16, uint32>::const_iterator it = peer.changesSeen.begin(); it != peer.changesSeen.end(); ++it)
            {
                if (loadtest::IsSpeedChange(it->first) && it->second >= 3) { ++thrice; }
            }
            ok = thrice >= 7 && peer.teleports >= 1;
            std::printf("PEER VERDICT resync %s (%u speed changes seen three times or more, %u teleports)\n", ok ? "OK" : "BUG", thrice, peer.teleports);
        }
        else if (what == "kick")
        {
            ok = peer.kicked;
            std::printf("PEER VERDICT kick %s (%s)\n", ok ? "OK" : "BUG", peer.kicked ? "the server closed the socket during the hold" : "still connected when the hold ended");
        }
        else if (what.compare(0, 8, "updates:") == 0)
        {
            const uint32 want = uint32(std::strtoul(what.c_str() + 8, NULL, 10));
            // --pair's observer, or (with no --pair) this run's own --observe: the
            // launcher runs the observer as a separate process alongside a walker
            // triggered by the console, and needs that process's own verdict.
            if (pairAccount.empty() && config.script.observeGuid == 0)
            {
                std::printf("PEER VERDICT updates SETUP (no --pair and not observing)\n");
            }
            else
            {
                const std::map<uint16, uint32>& forms = pairAccount.empty() ? peer.observerForms : pairResult.peer.observerForms;
                uint32 seen = 0;
                for (std::map<uint16, uint32>::const_iterator it = forms.begin(); it != forms.end(); ++it)
                {
                    seen += it->second;
                }
                ok = seen >= want;
                std::printf("PEER VERDICT updates %s (the observer saw %u observer forms for the walker, wanted %u)\n", ok ? "OK" : "BUG", seen, want);
            }
        }
        else if (what == "noupdates")
        {
            if (pairAccount.empty() && config.script.observeGuid == 0)
            {
                std::printf("PEER VERDICT noupdates SETUP (no --pair and not observing)\n");
            }
            else
            {
                const std::map<uint16, uint32>& forms = pairAccount.empty() ? peer.observerForms : pairResult.peer.observerForms;
                ok = forms.empty();
                std::printf("PEER VERDICT noupdates %s (the observer saw %u observer forms for the walker)\n", ok ? "OK" : "BUG", uint32(forms.size()));
            }
        }
        else if (what.compare(0, 8, "control:") == 0)
        {
            const uint32 want = uint32(std::strtoul(what.c_str() + 8, NULL, 10));
            ok = peer.controlGranted == want;
            std::printf("PEER VERDICT control %s (%u grants seen, wanted %u; %u revoked, %u selects sent)\n", ok ? "OK" : "BUG",
                        peer.controlGranted, want, peer.controlRevoked, peer.selectsSent);
        }
        else if (what.compare(0, 12, "activemover:") == 0)
        {
            const uint32 want = uint32(std::strtoul(what.c_str() + 12, NULL, 10));
            ok = peer.activeMoverSets == want;
            std::printf("PEER VERDICT activemover %s (%u SMSG_MOVE_SET_ACTIVE_MOVER decoded, wanted %u)\n", ok ? "OK" : "BUG", peer.activeMoverSets, want);
        }
        else if (what.compare(0, 6, "mover:") == 0)
        {
            const uint64 want = std::strtoull(what.c_str() + 6, NULL, 10);
            ok = peer.moverGuid == want;
            std::printf("PEER VERDICT mover %s (the walker ended as " UI64FMTD ", wanted " UI64FMTD ")\n", ok ? "OK" : "BUG", peer.moverGuid, want);
        }
        else if (what == "relayed" || what == "norelay")
        {
            if (pairAccount.empty() && config.script.observeGuid == 0)
            {
                std::printf("PEER VERDICT %s SETUP (no --pair and not observing)\n", what.c_str());
            }
            else
            {
                const uint32 seen = pairAccount.empty() ? peer.observedTarget : pairResult.peer.observedTarget;
                ok = what == "relayed" ? seen >= 1 : seen == 0;
                std::printf("PEER VERDICT %s %s (the observer saw %u relayed statuses of the target)\n", what.c_str(), ok ? "OK" : "BUG", seen);
            }
        }
        else
        {
            std::printf("PEER VERDICT %s BUG (unknown expectation)\n", what.c_str());
        }
        verdictsOk = verdictsOk && ok;
    }

    // The server relays movement only inside the observer's visibility range
    // (Visibility.Distance.Continents, 90 yd by default), and the walker leads
    // away by speed x seconds. A pair placed further apart than that allows, or
    // on different maps, is a setup fault, not a relay verdict.
    const float kVisibilityYards = 90.0f;
    // Consecutive relayed timestamps of one mover should advance at the rate
    // their packets arrive: heartbeats arrive every 500 ms, the server relays
    // on its map tick (up to 100 ms), and the peer polls on its own; 250 ms
    // covers all three with room, and a stale or unrebased stamp is seconds off.
    const uint32 kRelaySkewToleranceMs = 250;
    const bool  pairIn = pairResult.Reached(loadtest::Stage::InWorld) && pairResult.error.empty();
    const float sx = pairResult.worldPos.x - result.worldPos.x;
    const float sy = pairResult.worldPos.y - result.worldPos.y;
    const float apart = std::sqrt(sx * sx + sy * sy);
    const float reach = config.script.walk.speed * float(config.script.walk.seconds);
    const bool  colocated = pairResult.mapId == result.mapId && apart + reach <= kVisibilityYards;

    if (!pairAccount.empty() && config.script.walk.seconds == 0)
    {
        std::printf("PEER VERDICT relay SKIPPED (no walk)\n");
    }
    else if (!pairAccount.empty() && pairIn && !colocated)
    {
        std::printf("PEER VERDICT relay SETUP (observer on map %u, %.1f yd from the walker on map %u, "
                    "whose walk reaches %.1f yd; both must fit inside %.0f yd on one map)\n",
                    pairResult.mapId, apart, result.mapId, reach, kVisibilityYards);
        verdictsOk = false;
    }
    else if (!pairAccount.empty())
    {
        const loadtest::PeerReport& seen = pairResult.peer;
        const float dx = seen.lastTargetObservation.pos.x - peer.walkFinal.x;
        const float dy = seen.lastTargetObservation.pos.y - peer.walkFinal.y;
        const float gap = std::sqrt(dx * dx + dy * dy);
        // The observer must have seen the walker's relayed movement, its last
        // sighting must be where the walker says it stopped, and consecutive
        // relayed timestamps must have advanced at wall-clock rate.
        const bool relayOk = pairIn && seen.observedTarget >= 2 && gap <= 3.0f
                              && seen.relayMaxSkew <= kRelaySkewToleranceMs;
        std::printf("PEER pair stage=%s error=%s observedTarget=%u observedOthers=%u lastSeen=%.1f %.1f gap=%.1f maxSkew=%u over %u\n",
                    loadtest::StageName(pairResult.stage),
                    pairResult.error.empty() ? "-" : pairResult.error.c_str(),
                    seen.observedTarget, seen.observedOthers,
                    seen.lastTargetObservation.pos.x, seen.lastTargetObservation.pos.y, gap,
                    seen.relayMaxSkew, seen.relaySkewSamples);
        std::printf("PEER VERDICT relay %s\n", relayOk ? "OK" : "BUG");
        verdictsOk = verdictsOk && relayOk;
    }

    return (ok && verdictsOk) ? 0 : 1;
}
