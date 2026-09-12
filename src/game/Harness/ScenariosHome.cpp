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
#include "Log.h"

#include <cstdio>
#include <memory>
#include <vector>

// The home family of the old harness: S3 kill-mid-home, S10 stun-mid-home.
// Coordinates are the old file's Mulgore points.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;

        struct Pt { float x, y, z; };
        const Pt SC = { -3045.2f, -437.2f, 46.1f };
        const Pt T3 = { -3045.2f, -397.2f, 46.1f };

        /// S3: a creature killed mid-MoveHome must leave a corpse that stays put
        /// (B6): the death must not leave the spline still driving it around.
        class KilledMidHome : public Scenario
        {
        public:
            KilledMidHome() : Scenario("killed-mid-home", 3) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float x, y, z; bool dead; MovementGeneratorType mt; float moved; };
                Creature* a = Spawn(WOLF, SC.x, SC.y, SC.z, 0.0f);
                if (!a) { Verdict("B6=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                auto killAt = std::make_shared<Pt>();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MovePoint(5, T3.x, T3.y, Ground(T3.x, T3.y, T3.z), true);
                    Log("MoveTo(5) 40 yd out, mt=%s", TypeName(a));
                });
                At(8500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    const float x = a->Where().X(), y = a->Where().Y();
                    a->GetMotionMaster()->MoveTargetedHome();
                    Log("MoveHome from %.1f %.1f (%.1f yd from home) mt=%s", x, y, Dist2(x, y, SC.x, SC.y), TypeName(a));
                });
                At(9700, [this, g, killAt]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    killAt->x = a->Where().X(); killAt->y = a->Where().Y(); killAt->z = a->Where().Z();
                    Log("KILL at %.2f %.2f %.2f mt=%s (%.1f yd from home)", killAt->x, killAt->y, killAt->z, TypeName(a), Dist2(killAt->x, killAt->y, SC.x, SC.y));
                    a->DealDamage(a, a->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
                });
                for (uint32 i = 1; i <= 15; ++i)
                {
                    At(9700 + i * 200, [this, g, killAt, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 200;
                        s.x = a->Where().X(); s.y = a->Where().Y(); s.z = a->Where().Z();
                        s.dead = !a->IsAlive();
                        s.mt = Type(a);
                        s.moved = Dist2(s.x, s.y, killAt->x, killAt->y);
                        samples->push_back(s);
                        Log("+%4ums %.2f %.2f %.2f dead=%s mt=%s movedSinceKill=%.2f dHome=%.2f",
                            s.t, s.x, s.y, s.z, s.dead ? "true" : "false", Harness::TypeName(s.mt), s.moved, Dist2(s.x, s.y, SC.x, SC.y));
                    });
                }
                At(13000, [this, samples]()
                {
                    if (samples->empty()) { Verdict("INVALID(no samples)"); return; }
                    Sample const& f = samples->back();
                    std::string b6;
                    if (!f.dead)
                    {
                        b6 = "INVALID(kill did not take)";
                    }
                    else if (f.moved > 1.0f)
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "BUG(corpse kept moving %.1f yd after death)", f.moved);
                        b6 = text;
                    }
                    else
                    {
                        b6 = "OK(corpse stayed put)";
                    }
                    Verdict("B6=" + b6);
                });
            }
        };

        /// S10: a stop mid-MoveHome (what a stun/root does) must not read as
        /// "reached home" and must not park the creature short of it forever.
        class StunMidHome : public Scenario
        {
        public:
            StunMidHome() : Scenario("stun-mid-home", 9) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float dHome; MovementGeneratorType mt; };
                Creature* a = Spawn(WOLF, SC.x, SC.y, SC.z, 0.0f);
                if (!a) { Verdict("stunMidHome=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MovePoint(5, T3.x, T3.y, Ground(T3.x, T3.y, T3.z), true);
                });
                At(8500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveTargetedHome();
                    const float x = a->Where().X(), y = a->Where().Y();
                    Log("MoveHome from %.1f yd out, mt=%s", Dist2(x, y, SC.x, SC.y), TypeName(a));
                });
                At(9500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->StopMoving();
                    const float x = a->Where().X(), y = a->Where().Y();
                    Log("STOP (StopMoving - what a stun/root does; a homing creature is immune to the aura itself) at %.1f yd from home, mt=%s", Dist2(x, y, SC.x, SC.y), TypeName(a));
                });
                for (uint32 i = 1; i <= 24; ++i)
                {
                    At(9500 + i * 500, [this, g, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 500;
                        const float x = a->Where().X(), y = a->Where().Y();
                        s.dHome = Dist2(x, y, SC.x, SC.y);
                        s.mt = Type(a);
                        samples->push_back(s);
                        if (i % 2 == 0)
                        {
                            Log("+%5ums dHome=%.1f mt=%s", s.t, s.dHome, Harness::TypeName(s.mt));
                        }
                    });
                }
                At(22000, [this, samples]()
                {
                    if (samples->empty()) { Verdict("INVALID"); return; }
                    Sample const& f = samples->back();
                    float closest = 999.0f;
                    for (size_t k = 0; k < samples->size(); ++k)
                    {
                        if ((*samples)[k].dHome < closest) { closest = (*samples)[k].dHome; }
                    }
                    std::string v;
                    if (closest < 4.0f && f.mt != HOME_MOTION_TYPE)
                    {
                        char text[128];
                        snprintf(text, sizeof(text), "OK(resumed after the stop, reached home (closest %.1f yd) and finished homing)", closest);
                        v = text;
                    }
                    else if (closest < 4.0f)
                    {
                        v = "BUG(at home but the home leg never finished: arrival not recognised)";
                    }
                    else if (f.mt != HOME_MOTION_TYPE)
                    {
                        char text[128];
                        snprintf(text, sizeof(text), "BUG(home leg ended by the stop: closest %.1f yd from home, mt=%s)", closest, Harness::TypeName(f.mt));
                        v = text;
                    }
                    else
                    {
                        char text[64];
                        snprintf(text, sizeof(text), "PARTIAL(still homing, %.1f yd)", f.dHome);
                        v = text;
                    }
                    Verdict("stunMidHome=" + v);
                });
            }
        };
    }

    void RegisterHomeScenarios(Runner& r)
    {
        r.Register(new KilledMidHome());
        r.Register(new StunMidHome());
    }
}
