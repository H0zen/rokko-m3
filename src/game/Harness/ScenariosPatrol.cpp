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

#include "Scenario.h"
#include "Harness.h"
#include "Creature.h"
#include "MotionMaster.h"
#include "WaypointManager.h"
#include "movement/MoveSpline.h"
#include "Log.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// The patrol family of the old harness: S7 patrol-square, S8 patrol-lifted,
// S19 stun-mid-patrol. Coordinates are the old file's Mulgore points.
namespace Harness
{
    namespace
    {
        const uint32 CHICKEN = 621;
        const uint32 STUN = 5211;   // Bash: a plain stun aura
        const uint32 MOUSE_ENTRY = 6271;

        struct Pt { float x, y, z; };
        const Pt P0 = { -3122.6f, -261.3f, 46.0f };    // the template square's near corner
        const Pt P0_FAR = { -3152.6f, -231.3f, 46.0f };   // the square's far (diagonal) corner

        /// The nodes an S7/S19 sample recorded, comma-joined, as the old Lua's
        /// table.concat(reached, ",") did.
        std::string JoinNodes(std::vector<uint32> const& nodes)
        {
            std::string out;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (i)
                {
                    out += ",";
                }
                char buf[16];
                snprintf(buf, sizeof(buf), "%u", nodes[i]);
                out += buf;
            }
            return out;
        }

        /// S7: a fresh chicken on the old four-node template square (an external
        /// path, no database row) must still patrol it normally. B3 has no
        /// headless trigger (Eluna defers MovementInform on purpose, EventAI's
        /// REACHED_WAYPOINT has no trigger site, SD3 escorts need a player); this
        /// scenario only proves ordinary patrols still work.
        class PatrolSquare : public Scenario
        {
        public:
            PatrolSquare() : Scenario("patrol-square", 6) {}

            void Prepare() override
            {
                Creature* a = Spawn(CHICKEN, P0.x, P0.y, P0.z, 0.0f);
                if (!a) { Verdict("patrol=INVALID(spawn failed)"); return; }
                Load(P0_FAR.x, P0_FAR.y);
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                auto reached = std::make_shared<std::vector<uint32> >();
                At(500, [this, g, low]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveWaypoint(kExternalPath, PATH_FROM_EXTERNAL);
                    Log("chicken guid=%u MoveWaypoint on the template square, mt=%s", low, TypeName(a));
                });
                for (uint32 i = 1; i <= 40; ++i)
                {
                    At(500 + i * 1000, [this, g, reached, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const float px = a->Where().X(), py = a->Where().Y();
                        const uint32 wp = Node(a);
                        if (reached->empty() || reached->back() != wp)
                        {
                            reached->push_back(wp);
                            Log("+%3us at %.1f %.1f mt=%s reached node %u", i, px, py, TypeName(a), wp);
                        }
                    });
                }
                At(41500, [this, reached]()
                {
                    const uint32 n = uint32(reached->size());
                    std::string body;
                    if (n >= 4)
                    {
                        body = "patrol=OK(walked the square)";
                    }
                    else
                    {
                        char text[64];
                        snprintf(text, sizeof(text), "patrol=BROKEN(only %u node changes in 40 s)", n);
                        body = text;
                    }
                    body += " (nodes in order: " + JoinNodes(*reached) + ")";
                    Verdict(body);
                });
            }
        };

        /// S8: a patroller of our own on Mouse's four nodes (the world's Mouse,
        /// guid 261361, mirrored as an external path: the harness map is bare)
        /// lifted sixty yards off the mesh, where there is no navmesh under it.
        /// B7 asks whether the patrol behaviour ever lays a leg back down once
        /// its destination node turns out unreachable.
        class PatrolLifted : public Scenario
        {
        public:
            PatrolLifted() : Scenario("patrol-lifted", 7) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float z; uint32 node; Motion::Kind mt; };
                Creature* c = Spawn(MOUSE_ENTRY, -2986.64f, -329.723f, 54.0748f, 0.0f);
                if (!c) { Verdict("B7=INVALID(spawn failed)"); return; }
                c->GetMotionMaster()->MoveWaypoint(kMousePath, PATH_FROM_EXTERNAL);
                const ObjectGuid g = c->GetObjectGuid();
                const float x = c->Where().X(), y = c->Where().Y(), z = c->Where().Z(), o = c->Where().Facing();
                const float z0 = z + 60.0f;
                c->NearTeleportTo(x, y, z0, o);
                Log("lifted patroller to %.1f %.1f %.1f (ground %.1f) mt=%s", x, y, z0, z, TypeName(c));
                auto samples = std::make_shared<std::vector<Sample> >();
                for (uint32 i = 1; i <= 40; ++i)
                {
                    At(i * 1000, [this, g, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 1000;
                        const float px = a->Where().X(), py = a->Where().Y();
                        s.z = a->Where().Z();
                        s.mt = Type(a);
                        s.node = Node(a);
                        samples->push_back(s);
                        Log("+%5ums %.1f %.1f z=%.1f mt=%s lastWP=%u", s.t, px, py, s.z, Motion::KindName(s.mt), s.node);
                    });
                }
                At(41000, [this, samples, z0]()
                {
                    if (samples->empty())
                    {
                        Verdict("INVALID(no samples)");
                    }
                    else
                    {
                        Sample const& f = samples->back();
                        std::string b7;
                        if (f.z < z0 - 20.0f)
                        {
                            char text[96];
                            snprintf(text, sizeof(text), "OK(walked back down to z=%.1f: a leg was laid after one lap)", f.z);
                            b7 = text;
                        }
                        else
                        {
                            char text[112];
                            snprintf(text, sizeof(text), "BUG(still hanging at z=%.1f after 40 s: every node skipped as unreachable)", f.z);
                            b7 = text;
                        }
                        Verdict("B7=" + b7 + " | decisive count = MVTRACE dead-node lines");
                    }
                    // A spawn of our own now (the harness map is bare): the runner's
                    // sweep despawns it, no restore needed.
                });
            }
        };

        /// S19: a stun on a patrolling creature must not park it for 3 minutes.
        /// The waypoint generator used to infer "a player stopped me" from
        /// IsStopped(), which every StopMoving() sets, so a self-cast stun (no
        /// combat, no generator change) parked the patrol for
        /// STOP_TIME_FOR_PLAYER. Claim: after the stun wears off (2 s) the
        /// patrol moves on within 10 s and reaches its next node.
        class StunMidPatrol : public Scenario
        {
        public:
            StunMidPatrol() : Scenario("stun-mid-patrol", 15) {}

            void Prepare() override
            {
                struct StunAt { bool set; float x, y; uint32 node; };
                struct Sample { uint32 t; float d; uint32 node; Motion::Kind mt; };
                Creature* a = Spawn(CHICKEN, P0.x, P0.y, P0.z, 0.0f);
                if (!a) { Verdict("stunnedPatrol=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                auto stunAt = std::make_shared<StunAt>();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveWaypoint(kExternalPath, PATH_FROM_EXTERNAL);
                    Log("chicken MoveWaypoint on the template square, mt=%s", TypeName(a));
                });
                At(6000, [this, g, stunAt]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    const float x = a->Where().X(), y = a->Where().Y();
                    stunAt->set = true;
                    stunAt->x = x; stunAt->y = y;
                    stunAt->node = Node(a);
                    SelfCast(a, STUN);
                    Log("+6s self-stun (%u) mid-leg at %.1f %.1f, node %u, mt=%s", STUN, x, y, stunAt->node, TypeName(a));
                });
                for (uint32 i = 1; i <= 30; ++i)
                {
                    At(6000 + i * 1000, [this, g, stunAt, samples, i]()
                    {
                        Creature* a = Get(g); if (!a || !stunAt->set) { return; }
                        Sample s;
                        s.t = i;
                        const float x = a->Where().X(), y = a->Where().Y();
                        s.d = Dist2(x, y, stunAt->x, stunAt->y);
                        s.node = Node(a);
                        s.mt = Type(a);
                        samples->push_back(s);
                        if (i % 5 == 0)
                        {
                            Log("+%2us after the stun: %.1f yd from the stun spot, node %u, mt=%s", i, s.d, s.node, Motion::KindName(s.mt));
                        }
                    });
                }
                At(37500, [this, stunAt, samples]()
                {
                    if (!stunAt->set) { Verdict("INVALID"); return; }
                    bool moved = false;
                    for (size_t k = 0; k < samples->size(); ++k)
                    {
                        Sample const& s = (*samples)[k];
                        if (s.t <= 12 && s.d > 2.0f) { moved = true; }
                    }
                    const bool hasLast = !samples->empty();
                    const uint32 lastNode = hasLast ? samples->back().node : stunAt->node;
                    char const* lastMt = hasLast ? Motion::KindName(samples->back().mt) : "?";
                    const bool advanced = hasLast && lastNode != stunAt->node;
                    std::string v;
                    if (moved && advanced)
                    {
                        char text[160];
                        snprintf(text, sizeof(text), "OK(moved again within 10 s of the stun and reached node %u; mt=%s)", lastNode, lastMt);
                        v = text;
                    }
                    else
                    {
                        char text[192];
                        snprintf(text, sizeof(text), "BUG(%s after the stun; node %u -> %u, mt=%s)",
                                 moved ? "moved but never reached the next node" : "no movement for 10 s",
                                 stunAt->node, lastNode, lastMt);
                        v = text;
                    }
                    Verdict("stunnedPatrol=" + v);
                });
            }
        };

        /// P3-C: a node hook that despawns its walker from inside the inform, while the
        /// patrol behaviour's Tick is still on the stack. P3-B defers a finished
        /// behaviour's destruction to the end of the outermost commit for exactly this;
        /// no scenario drove it until now. An external path reports through
        /// WaypointPathInform instead of MovementInform, so the hook listens on both.
        class DespawnAtNode : public Scenario
        {
        public:
            DespawnAtNode() : Scenario("despawn-at-node", 18) {}

            void OnInform(Creature* creature, Motion::Kind kind, uint32 id) override
            {
                if (creature && creature->GetObjectGuid() == m_walker && kind == Motion::Kind::Patrol && id == 2 && creature->IsAlive())
                {
                    OnNodeTwo(creature);
                }
            }

            void OnPathInform(Creature* creature, uint32 pathId, Motion::PathEvent event, uint32 node) override
            {
                if (creature && creature->GetObjectGuid() == m_walker && pathId == uint32(kExternalPath) && event == Motion::PathEvent::NodeReached && node == 2 && creature->IsAlive())
                {
                    OnNodeTwo(creature);
                }
            }

            void Prepare() override
            {
                Creature* a = Spawn(CHICKEN, P0.x, P0.y, P0.z, 0.0f);
                if (!a) { Verdict("despawnAtNode=INVALID(spawn failed)"); return; }
                Load(P0_FAR.x, P0_FAR.y);
                const ObjectGuid g = a->GetObjectGuid();
                m_walker = g;   // OnInform fires for every recording actor: only this scenario's walker despawns
                auto informedAt = std::make_shared<uint32>(0);
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveWaypoint(kExternalPath, PATH_FROM_EXTERNAL);
                    Log("MoveWaypoint on the template square, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 40; ++i)
                {
                    At(500 + i * 500, [this, g, informedAt, i]()
                    {
                        if (*informedAt) { return; }
                        for (size_t k = 0; k < Informs().size(); ++k)
                        {
                            if (((Informs()[k].event == Inform::Event::Inform && Informs()[k].kind == Motion::Kind::Patrol) ||
                                 (Informs()[k].event == Inform::Event::PathInform && Informs()[k].pathId == uint32(kExternalPath) && Informs()[k].pathEvent == Motion::PathEvent::NodeReached)) &&
                                Informs()[k].id == 2)
                            {
                                *informedAt = 500 + i * 500;
                                Creature* a = Get(g);
                                Log("+%ums node 2 informed; the walker is %s", *informedAt,
                                    a ? (a->IsAlive() ? "still alive" : "dead, still on the map") : "gone");
                                return;
                            }
                        }
                    });
                }
                At(21000, [this, g, informedAt]()
                {
                    Creature* a = Get(g);
                    std::string body;
                    if (!*informedAt)
                    {
                        body = "despawnAtNode=BROKEN(node 2 never informed in 20 s)";
                    }
                    else if (a && a->IsAlive())
                    {
                        body = "despawnAtNode=BUG(still alive after the despawn from the node hook)";
                    }
                    else
                    {
                        // A manual-despawn summon stays on the map, dead, until the runner's sweep:
                        // the despawn from inside the hook is proven by the death, not by removal.
                        body = a ? "despawnAtNode=OK(despawned from inside the node 2 hook: dead, kept by the map until the sweep)"
                                 : "despawnAtNode=OK(despawned from inside the node 2 hook, walker gone)";
                    }
                    Verdict(body);
                });
            }

        private:
            /// The hook body, shared by the internal and the external path: node 2's arrival,
            /// while the patrol behaviour's Tick is still on the stack.
            void OnNodeTwo(Creature* creature)
            {
                Log("node %u inform: ForcedDespawn from inside the hook, mt=%s", 2u, TypeName(creature));
                creature->ForcedDespawn();
            }

            ObjectGuid m_walker;   ///< the walker this run spawned; the despawn hook acts on it alone
        };

        /// S67: a leg to the ground the unit is already standing on.
        ///
        /// The user's capture (server-release/mvcapture.log) carries 9 364 of them: a spline
        /// 1 ms long and 0.000 yd across, one SMSG_MONSTER_MOVE to every observer for a step
        /// of nothing. They are not the fear's collapsing draw that #114 fixed -- 9 223 of the
        /// 9 348 exactly-zero ones carry no final facing at all, and they cluster on units
        /// whose DATABASE PATH is degenerate:
        ///
        ///   creature_movement 127332 (entry 3296)  ONE node, at its own spawn point: 2 427
        ///                                          zero legs in an unbroken run, and nothing
        ///                                          else on the wire from that guid, ever.
        ///   creature_movement 318624 (entry 51346) points 2 and 3 the same coordinate: one
        ///   creature_movement 236808 (entry 42548) zero leg per lap, between the two real
        ///                                          ones and the one home again.
        ///
        /// The patrol is doing nothing wrong -- it walks the nodes it was given, and a node it
        /// is already standing on is still a node, with its script, its emote and its wait.
        /// What is wrong is putting a packet on the wire to say the unit is where it is. Over
        /// the retail movement corpus (peer/retail-fear-movement-2026-09-20.md) retail ends a
        /// move with a type-1 stop and otherwise sends NOTHING when there is nowhere to go; it
        /// has no leg shorter than 2.62 yd anywhere in it.
        ///
        /// Both fixtures are the database shapes above, and both categories also check that
        /// the patrol still WALKS: a fix that silences the wire by parking the walker would
        /// pass the leg count and fail the node count.
        class PatrolZeroLengthLegs : public Scenario
        {
        public:
            PatrolZeroLengthLegs() : Scenario("patrol-zero-length-legs", 67) {}

            void Prepare() override
            {
                /// A leg covering less than this moved the unit nowhere. Not a minimum leg
                /// length: the legs in question are 0.000 yd, and the shortest thing the
                /// kernel lays on purpose (a patrol node a yard off, the chase's last step)
                /// is two orders of magnitude above it.
                const float kNowhere = 0.05f;
                const uint32 kStart = 300;
                const uint32 kSamples = 300;          // 30 s at the 100 ms cadence: about 2.5 laps
                const uint32 kVerdictAt = kStart + kSamples * 100 + 700;

                /// One walker's leg tally. A launched spline takes a fresh id, so a new id at
                /// a sample is a leg laid since the last one; its length is the wire's own
                /// number, Duration() x Velocity().
                struct Walk
                {
                    ObjectGuid guid;
                    uint32 lastId;
                    bool   haveId;
                    uint32 legs;        ///< legs laid in the window
                    uint32 nowhere;     ///< of those, ones that covered nothing
                    uint32 logged;
                    float  shortest;
                    int32  shortestMs;
                    std::vector<uint32> nodes;   ///< the node ids reached, in order, deduplicated
                };

                Creature* stand = Spawn(MOUSE_ENTRY, P0.x, P0.y, P0.z, 0.0f);
                Creature* loop = Spawn(MOUSE_ENTRY, P0.x, P0.y, P0.z, 0.0f);
                if (!stand || !loop)
                {
                    Verdict("standstillPath=INVALID(spawn failed) | coincidentNode=INVALID(spawn failed)");
                    return;
                }
                auto ws = std::make_shared<Walk>();
                auto wl = std::make_shared<Walk>();
                for (auto const& w : { ws, wl })
                {
                    w->lastId = 0;
                    w->haveId = false;
                    w->legs = w->nowhere = w->logged = 0;
                    w->shortest = 1.0e9f;
                    w->shortestMs = 0;
                }
                ws->guid = stand->GetObjectGuid();
                wl->guid = loop->GetObjectGuid();

                At(kStart, [this, ws, wl]()
                {
                    Creature* s = Get(ws->guid); Creature* l = Get(wl->guid); if (!s || !l) { return; }
                    // Exactly on the one node, so the leg the patrol prepares for it is a
                    // leg to the unit's own feet however the router answers.
                    s->NearTeleportTo(P0.x, P0.y, P0.z, 0.0f);
                    s->GetMotionMaster()->MoveWaypoint(kStandstillPath, PATH_FROM_EXTERNAL);
                    // The lap walker starts at node 1 too, but its coincident pair is nodes
                    // 3 and 4, which it reaches by walking.
                    l->GetMotionMaster()->MoveWaypoint(kCoincidentPath, PATH_FROM_EXTERNAL);
                    Log("standstill walker on its single node at %.1f %.1f, mt=%s; lap walker on the coincident square, mt=%s",
                        P0.x, P0.y, TypeName(s), TypeName(l));
                });

                for (uint32 i = 1; i <= kSamples; ++i)
                {
                    At(kStart + i * 100, [this, ws, wl, i, kNowhere]()
                    {
                        Sample(ws, "standstill", i * 100, kNowhere);
                        Sample(wl, "lap", i * 100, kNowhere);
                    });
                }

                At(kVerdictAt, [this, ws, wl, kNowhere, kSamples]()
                {
                    char one[340], lap[340];
                    // The standstill path: one node under the walker's feet. Every leg it lays
                    // is a leg to nowhere, so the honest reading is the leg count itself.
                    if (ws->nowhere)
                    {
                        snprintf(one, sizeof(one),
                                 "BUG(%u legs in %u s on a ONE-node path the walker is standing on, %u of them covering nothing; the shortest %.4f yd in %d ms -- retail sends no leg at all when there is nowhere to go)",
                                 ws->legs, kSamples / 10, ws->nowhere, ws->shortest, ws->shortestMs);
                    }
                    else if (ws->legs)
                    {
                        snprintf(one, sizeof(one),
                                 "INVALID(%u legs on the one-node path but none of them degenerate: the walker was not standing on its node)", ws->legs);
                    }
                    else
                    {
                        snprintf(one, sizeof(one),
                                 "OK(no leg laid in %u s on a ONE-node path the walker is standing on; it reached node %s and stayed)",
                                 kSamples / 10, ws->nodes.empty() ? "(none)" : JoinNodes(ws->nodes).c_str());
                    }
                    // The lap: the walker must still get round, so the node count is half the
                    // claim. Four node changes is one full lap of the four-node path.
                    const uint32 hops = uint32(wl->nodes.size());
                    if (hops < 4)
                    {
                        snprintf(lap, sizeof(lap),
                                 "BROKEN(the lap walker only reached %u node(s) in %u s: %s)",
                                 hops, kSamples / 10, JoinNodes(wl->nodes).c_str());
                    }
                    else if (wl->nowhere)
                    {
                        snprintf(lap, sizeof(lap),
                                 "BUG(%u of %u legs covered nothing over %u node arrivals; the shortest %.4f yd in %d ms -- nodes 3 and 4 are the same point, and the second one costs a packet)",
                                 wl->nowhere, wl->legs, hops, wl->shortest, wl->shortestMs);
                    }
                    else
                    {
                        snprintf(lap, sizeof(lap),
                                 "OK(none of %u legs covered nothing over %u node arrivals: %s)",
                                 wl->legs, hops, JoinNodes(wl->nodes).c_str());
                    }
                    Verdict(std::string("standstillPath=") + one + " | coincidentNode=" + lap);
                });
            }

        private:
            /// One 100 ms sample of one walker: a fresh spline id is a leg, and the node it
            /// last reached is recorded when it changes.
            template <class W>
            void Sample(W const& w, char const* who, uint32 t, float nowhere)
            {
                Creature* c = Get(w->guid);
                if (!c) { return; }
                const uint32 node = Node(c);
                if (node && (w->nodes.empty() || w->nodes.back() != node)) { w->nodes.push_back(node); }
                Movement::MoveSpline const& s = *c->movespline;
                if (!s.Initialized()) { return; }
                const uint32 id = s.GetId();
                if (w->haveId && id == w->lastId) { return; }
                w->haveId = true;
                w->lastId = id;
                const int32 ms = s.Duration();
                if (ms <= 0 || s.Cut()) { return; }          // a stop is not a leg
                const float len = float(ms) * s.Velocity() / 1000.0f;
                ++w->legs;
                if (len >= nowhere) { return; }
                ++w->nowhere;
                if (len < w->shortest) { w->shortest = len; w->shortestMs = ms; }
                if (++w->logged <= 8)
                {
                    Log("+%5ums %s leg to nowhere %u: %.4f yd in %d ms at node %u", t, who, w->nowhere, len, ms, node);
                }
            }
        };
    }

    void RegisterPatrolScenarios(Runner& r)
    {
        r.Register(new PatrolSquare());
        r.Register(new PatrolLifted());
        r.Register(new StunMidPatrol());
        r.Register(new DespawnAtNode());
        r.Register(new PatrolZeroLengthLegs());
    }
}
