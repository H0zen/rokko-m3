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
#include <string>
#include <memory>
#include <vector>

// The flee family of the old harness: S6 flee-drifts-back, plus P3-B's
// distract-over-assist, the rest of the flee-for-assistance chain. Coordinates
// are the old file's Mulgore points.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;

        struct Pt { float x, y, z; };
        const Pt SE = { -3200.0f, -300.0f, 47.0f };

        /// S6: fleeing beyond the quiet band must drift BACK toward the source
        /// (B10), not keep running away from it (the bearing must be reversed,
        /// not merely reflected).
        class FleeDriftsBack : public Scenario
        {
        public:
            FleeDriftsBack() : Scenario("flee-drifts-back", 5) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float d; MovementGeneratorType mt; };
                const float ex = SE.x + 60.0f;   // source 60 yd east: caster->mover bearing = pi, the axis where a reflection is a no-op
                const float d0 = 60.0f;
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, ex, SE.y, Ground(ex, SE.y, SE.z), 3.1f);
                if (!a || !b) { Verdict("B10=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid h = b->GetObjectGuid();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g, h, d0]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { return; }
                    a->GetMotionMaster()->MoveFleeing(b, 7000);
                    Log("MoveFleeing from a source %.0f yd east, mt=%s", d0, TypeName(a));
                });
                for (uint32 i = 1; i <= 12; ++i)
                {
                    At(500 + i * 500, [this, g, h, samples, i]()
                    {
                        Creature* a = Get(g); Creature* b = Get(h);
                        if (!a || !b) { return; }
                        Sample s;
                        s.t = i * 500;
                        const float x = a->Where().X(), y = a->Where().Y();
                        const float bx = b->Where().X(), by = b->Where().Y();
                        s.d = Dist2(x, y, bx, by);
                        s.mt = Type(a);
                        samples->push_back(s);
                        Log("+%4ums %.1f %.1f mt=%s dSource=%.1f", s.t, x, y, Harness::TypeName(s.mt), s.d);
                    });
                }
                At(7500, [this, samples, d0]()
                {
                    if (samples->empty()) { Verdict("INVALID(no samples)"); return; }
                    Sample const& f = samples->back();
                    float mn = 999.0f;
                    for (size_t k = 0; k < samples->size(); ++k)
                    {
                        if ((*samples)[k].d < mn) { mn = (*samples)[k].d; }
                    }
                    std::string b10;
                    if (mn < d0 - 3.0f)
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "OK(drifted back toward the source, closest %.1f yd)", mn);
                        b10 = text;
                    }
                    else if (f.d > d0 + 3.0f)
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "BUG(kept fleeing AWAY to %.1f yd: bearing reflected, not reversed)", f.d);
                        b10 = text;
                    }
                    else
                    {
                        char text[48];
                        snprintf(text, sizeof(text), "PARTIAL(dist %.1f)", f.d);
                        b10 = text;
                    }
                    Verdict("B10=" + b10);
                });
            }
        };

        /// P3-B: the rest of the flee-for-assistance chain, in two phases on one
        /// creature that has a victim.
        ///
        /// 1. A distract superseding an assistance distract runs the displaced
        ///    generator's finalizer, which re-engages combat (AttackStart ->
        ///    MoveChase) from inside the request that displaced it. The nested
        ///    chase must not take the binding the outer request stamped, or the
        ///    new distract is selected with no behaviour at all and the creature
        ///    stands there for good. The superseding call is a second assistance
        ///    distract rather than a plain MoveDistract, because Distract is
        ///    self-expiring: the nested chase cancels a plain one outright, and a
        ///    cancelled entry has no binding left to steal.
        /// 2. A death over a walking assistance run must leave a corpse where it
        ///    fell and nothing in the arbiter but the idle default -- including the
        ///    assistance distract the run's own finalizer asks for while dying.
        class DistractOverAssist : public Scenario
        {
        public:
            DistractOverAssist() : Scenario("distract-over-assist", 17) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float dVictim; MovementGeneratorType mt; };
                struct Corpse { uint32 t; float moved; bool dead; uint32 held; bool idleOnly; };
                const float kx = SE.x + 25.0f;
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, kx, SE.y, Ground(kx, SE.y, SE.z), 3.1f);
                if (!a || !b) { Verdict("reengage=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000);
                b->SetMaxHealth(500000); b->SetHealth(500000);
                b->setFaction(14);   // Monster: hostile to the wolf, so the finalizer's AttackStart really engages
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid h = b->GetObjectGuid();
                auto chase = std::make_shared<std::vector<Sample> >();
                auto corpse = std::make_shared<std::vector<Corpse> >();
                auto atDistract = std::make_shared<float>(0.0f);
                auto killAt = std::make_shared<Pt>();
                auto reengage = std::make_shared<std::string>();
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { Log("ERR actors gone"); return; }
                    const bool ok = a->Attack(b, true);   // Unit::Attack sets the victim directly, bypassing the AI's refusal
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                    Log("Attack=%s + MoveChase %.1f yd away, mt=%s", ok ? "true" : "false",
                        Dist2(a->Where().X(), a->Where().Y(), b->Where().X(), b->Where().Y()), TypeName(a));
                });
                At(1500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    a->GetMotionMaster()->MoveSeekAssistanceDistract(4000);
                    Log("MoveSeekAssistanceDistract(4000), mt=%s victim=%s", TypeName(a), a->getVictim() ? "true" : "false");
                });
                At(1600, [this, g, h, atDistract]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { Log("ERR actors gone"); return; }
                    if (!a->getVictim()) { a->Attack(b, true); }
                    *atDistract = Dist2(a->Where().X(), a->Where().Y(), b->Where().X(), b->Where().Y());
                    a->GetMotionMaster()->MoveSeekAssistanceDistract(1500);
                    Log("MoveSeekAssistanceDistract(1500) over it at %.1f yd, mt=%s victim=%s",
                        *atDistract, TypeName(a), a->getVictim() ? "true" : "false");
                });
                for (uint32 i = 1; i <= 20; ++i)
                {
                    At(1600 + i * 400, [this, g, h, chase, i]()
                    {
                        Creature* a = Get(g); Creature* b = Get(h);
                        if (!a || !b) { return; }
                        Sample s;
                        s.t = i * 400;
                        s.dVictim = Dist2(a->Where().X(), a->Where().Y(), b->Where().X(), b->Where().Y());
                        s.mt = Type(a);
                        chase->push_back(s);
                        Log("+%4ums mt=%s dVictim=%.1f victim=%s", s.t, Harness::TypeName(s.mt), s.dVictim, a->getVictim() ? "true" : "false");
                    });
                }
                At(10000, [this, g, chase, atDistract, reengage]()
                {
                    if (chase->empty())
                    {
                        *reengage = "INVALID(no samples)";
                    }
                    else
                    {
                        // Six seconds past the distract's end: chasing again, and closer to
                        // the victim than the distract left it (or already in melee).
                        Sample const& f = chase->back();
                        char text[128];
                        if (f.mt == CHASE_MOTION_TYPE && (f.dVictim < *atDistract - 2.0f || f.dVictim < 5.0f))
                        {
                            snprintf(text, sizeof(text), "OK(re-engaged through the supersede, %.1f -> %.1f yd)", *atDistract, f.dVictim);
                        }
                        else
                        {
                            snprintf(text, sizeof(text), "BUG(%s %.1f yd from the victim, %.1f yd at the distract)",
                                     Harness::TypeName(f.mt), f.dVictim, *atDistract);
                        }
                        *reengage = text;
                    }
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    const float tx = a->Where().X() - 30.0f, ty = a->Where().Y();
                    Load(tx, ty);
                    a->GetMotionMaster()->MoveSeekAssistance(tx, ty, Ground(tx, ty, a->Where().Z()));
                    Log("MoveSeekAssistance 30 yd west, mt=%s", TypeName(a));
                });
                At(12000, [this, g, killAt]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    killAt->x = a->Where().X(); killAt->y = a->Where().Y(); killAt->z = a->Where().Z();
                    Log("KILL at %.2f %.2f %.2f mt=%s", killAt->x, killAt->y, killAt->z, TypeName(a));
                    a->DealDamage(a, a->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
                });
                for (uint32 i = 1; i <= 15; ++i)
                {
                    At(12000 + i * 200, [this, g, killAt, corpse, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        std::vector<Motion::Held> model = a->GetMotionMaster()->Arbiter().Contents();
                        Corpse c;
                        c.t = i * 200;
                        c.moved = Dist2(a->Where().X(), a->Where().Y(), killAt->x, killAt->y);
                        c.dead = !a->IsAlive();
                        c.held = uint32(model.size());
                        c.idleOnly = model.size() == 1 && model[0].kind == Motion::Kind::Idle;
                        corpse->push_back(c);
                        Log("+%4ums dead=%s movedSinceKill=%.2f held=%u%s", c.t, c.dead ? "true" : "false",
                            c.moved, c.held, c.idleOnly ? " (idle default alone)" : "");
                    });
                }
                At(15400, [this, corpse, reengage]()
                {
                    std::string death;
                    if (corpse->empty())
                    {
                        death = "INVALID(no samples)";
                    }
                    else
                    {
                        float drift = 0.0f;
                        bool dead = true;
                        bool idleOnly = true;
                        uint32 held = 0;
                        for (size_t k = 0; k < corpse->size(); ++k)
                        {
                            Corpse const& c = (*corpse)[k];
                            if (c.moved > drift) { drift = c.moved; }
                            if (!c.dead) { dead = false; }
                            if (!c.idleOnly) { idleOnly = false; held = c.held; }
                        }
                        char text[128];
                        if (!dead)
                        {
                            death = "INVALID(kill did not take)";
                        }
                        else if (drift > 1.0f)
                        {
                            snprintf(text, sizeof(text), "BUG(corpse kept moving %.1f yd after death)", drift);
                            death = text;
                        }
                        else if (!idleOnly)
                        {
                            snprintf(text, sizeof(text), "BUG(%u entries held after death, not the idle default alone)", held);
                            death = text;
                        }
                        else
                        {
                            death = "OK(corpse stayed put, idle default alone)";
                        }
                    }
                    Verdict("reengage=" + *reengage + " | deathOverAssist=" + death);
                });
            }
        };
    }

    void RegisterFleeScenarios(Runner& r)
    {
        r.Register(new FleeDriftsBack());
        r.Register(new DistractOverAssist());
    }
}
