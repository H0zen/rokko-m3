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
        const uint32 MOUSE_LOW = 261361;
        const uint32 MOUSE_ENTRY = 6271;

        struct Pt { float x, y, z; };
        const Pt P0 = { -3122.6f, -261.3f, 46.0f };    // the template square's near corner
        const Pt P0_FAR = { -3152.6f, -231.3f, 46.0f };   // the square's far (diagonal) corner
        const Pt MOUSE_POS = { -2992.5f, -335.9f, 0.0f };   // the Mouse's grid, for Load before Find

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

        /// S8: the world's own patroller (Mouse, guid 261361, 4 nodes near
        /// -2993 -336) lifted sixty yards off the mesh, where there is no
        /// navmesh under it. B7 asks whether the waypoint generator ever lays a
        /// leg back down once its destination node turns out unreachable.
        class PatrolLifted : public Scenario
        {
        public:
            PatrolLifted() : Scenario("patrol-lifted", 7) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float z; uint32 node; MovementGeneratorType mt; };
                // Find only sees creatures in loaded grids; the Mouse's grid must be
                // forced in before the lookup.
                Load(MOUSE_POS.x, MOUSE_POS.y);
                Creature* c = Find(MOUSE_LOW, MOUSE_ENTRY);
                if (!c)
                {
                    Log("ERR patroller %u not found", MOUSE_LOW);
                    Verdict("B7=INVALID(patroller not found)");
                    return;
                }
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
                        Log("+%5ums %.1f %.1f z=%.1f mt=%s lastWP=%u", s.t, px, py, s.z, Harness::TypeName(s.mt), s.node);
                    });
                }
                At(41000, [this, samples, z0, g, x, y, z, o]()
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
                    // Put the Mouse back where it stood before the lift: Find handed
                    // back a world creature, not a spawn of ours, so the runner's
                    // sweep restores its AI and active flag but never its position or
                    // its generator's node. MotionMaster::Initialize rebuilds a WAYPOINT
                    // creature's default path from its first node, so the Mouse resumes
                    // its own patrol rather than picking up at the node the lifted run
                    // left it on.
                    if (Creature* m = Get(g))
                    {
                        m->NearTeleportTo(x, y, z, o);
                        m->GetMotionMaster()->Initialize();
                        Log("restored to %.1f %.1f %.1f", x, y, z);
                    }
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
                struct Sample { uint32 t; float d; uint32 node; MovementGeneratorType mt; };
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
                    a->CastSpell(a, STUN, true);
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
                            Log("+%2us after the stun: %.1f yd from the stun spot, node %u, mt=%s", i, s.d, s.node, Harness::TypeName(s.mt));
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
                    char const* lastMt = hasLast ? Harness::TypeName(samples->back().mt) : "?";
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
    }

    void RegisterPatrolScenarios(Runner& r)
    {
        r.Register(new PatrolSquare());
        r.Register(new PatrolLifted());
        r.Register(new StunMidPatrol());
    }
}
