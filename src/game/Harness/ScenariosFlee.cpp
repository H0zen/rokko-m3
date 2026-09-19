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
#include "CreatureAI.h"
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
                struct Sample { uint32 t; float d; Motion::Kind mt; };
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
                        Log("+%4ums %.1f %.1f mt=%s dSource=%.1f", s.t, x, y, Motion::KindName(s.mt), s.d);
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
        ///    behaviour's finalizer, which re-engages combat (AttackStart ->
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
                struct Sample { uint32 t; float dVictim; Motion::Kind mt; };
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
                        Log("+%4ums mt=%s dVictim=%.1f victim=%s", s.t, Motion::KindName(s.mt), s.dVictim, a->getVictim() ? "true" : "false");
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
                        // The replacement distract dwells first -- the finalizer's re-engagement
                        // only takes over when it expires -- and six seconds past the distract's
                        // end the wolf chases again, closer to the victim than the distract left
                        // it (or already in melee).
                        Sample const& f = chase->back();
                        Sample const& first = chase->front();
                        char text[128];
                        if (first.mt != Motion::Kind::AssistDistract)
                        {
                            snprintf(text, sizeof(text), "BUG(%s at +400 ms: the replacement distract did not dwell)", Motion::KindName(first.mt));
                        }
                        else if (f.mt == Motion::Kind::Chase && (f.dVictim < *atDistract - 2.0f || f.dVictim < 5.0f))
                        {
                            snprintf(text, sizeof(text), "OK(the replacement distract dwelt, then re-engaged through the supersede, %.1f -> %.1f yd)", *atDistract, f.dVictim);
                        }
                        else
                        {
                            snprintf(text, sizeof(text), "BUG(%s %.1f yd from the victim, %.1f yd at the distract)",
                                     Motion::KindName(f.mt), f.dVictim, *atDistract);
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

        /// P3-C: the combat-started event row. A creature under a distract that begins
        /// attacking drops the distract at once — the stack's chase push displaced it,
        /// while the Distract layer sits above Combat, so priority alone would keep the
        /// creature standing until the timer ran out.
        class DistractThenAttack : public Scenario
        {
        public:
            DistractThenAttack() : Scenario("distract-then-attack", 19) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float dVictim; Motion::Kind mt; };
                const float kx = SE.x + 20.0f;
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, kx, SE.y, Ground(kx, SE.y, SE.z), 3.1f);
                if (!a || !b) { Verdict("combatDropsDistract=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000);
                b->SetMaxHealth(500000); b->SetHealth(500000);
                b->setFaction(14);   // Monster: hostile to the wolf, so its AI really attacks
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid h = b->GetObjectGuid();
                auto samples = std::make_shared<std::vector<Sample> >();
                auto atAttack = std::make_shared<float>(0.0f);
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    a->GetMotionMaster()->MoveDistract(8000);
                    Log("MoveDistract(8000), mt=%s", TypeName(a));
                });
                At(2500, [this, g, h, atAttack]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { Log("ERR actors gone"); return; }
                    *atAttack = Dist2(a->Where().X(), a->Where().Y(), b->Where().X(), b->Where().Y());
                    a->AI()->AttackStart(b);   // through the recording AI to the creature's own: Unit::Attack (the row), then its chase
                    Log("AttackStart at %.1f yd, mt=%s victim=%s", *atAttack, TypeName(a), a->getVictim() ? "true" : "false");
                });
                for (uint32 i = 1; i <= 10; ++i)
                {
                    At(2500 + i * 300, [this, g, h, samples, i]()
                    {
                        Creature* a = Get(g); Creature* b = Get(h);
                        if (!a || !b) { return; }
                        Sample s;
                        s.t = i * 300;
                        s.dVictim = Dist2(a->Where().X(), a->Where().Y(), b->Where().X(), b->Where().Y());
                        s.mt = Type(a);
                        samples->push_back(s);
                        Log("+%4ums mt=%s dVictim=%.1f", s.t, Motion::KindName(s.mt), s.dVictim);
                    });
                }
                At(6000, [this, samples, atAttack]()
                {
                    std::string body;
                    if (samples->size() < 4)
                    {
                        body = "combatDropsDistract=INVALID(too few samples)";
                    }
                    else
                    {
                        // Chasing within a second of the attack start, and closer by the end.
                        Sample const& first = (*samples)[2];   // +900 ms
                        Sample const& last = samples->back();
                        char text[160];
                        if (first.mt == Motion::Kind::Chase && last.dVictim < *atAttack - 2.0f)
                        {
                            snprintf(text, sizeof(text), "combatDropsDistract=OK(chasing within a second, %.1f -> %.1f yd)", *atAttack, last.dVictim);
                        }
                        else
                        {
                            snprintf(text, sizeof(text), "combatDropsDistract=BUG(%s at +%ums, %.1f -> %.1f yd)",
                                     Motion::KindName(first.mt), first.t, *atAttack, last.dVictim);
                        }
                        body = text;
                    }
                    Verdict(body);
                });
            }
        };

        /// P4-A: two fears from two casters on an aggroed, chasing wolf. Each holds its own claim: the
        /// first aura's removal changes nothing while the second runs (the state and the
        /// flag stay, the flee continues); the second's removal ends the episode and the
        /// chase resumes (reference §3.6, §3.1.4).
        class FearTwice : public Scenario
        {
        public:
            FearTwice() : Scenario("fear-twice", 20) {}

            void Prepare() override
            {
                struct Sample { uint32 t; Motion::Kind mt; bool state; bool move; bool flag; float dVictim; };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                Creature* c = Spawn(KOBOLD, SE.x + 25.0f, SE.y + 15.0f, Ground(SE.x + 25.0f, SE.y + 15.0f, SE.z), 3.1f);
                Creature* d = Spawn(KOBOLD, SE.x - 25.0f, SE.y - 15.0f, Ground(SE.x - 25.0f, SE.y - 15.0f, SE.z), 0.0f);
                if (!a || !b || !c || !d) { Verdict("secondFearKeepsFleeing=INVALID(spawn failed)"); return; }
                Creature* actors[4] = { a, b, c, d };
                for (int i = 0; i < 4; ++i) { actors[i]->SetMaxHealth(500000); actors[i]->SetHealth(500000); }
                b->setFaction(14); c->setFaction(14); d->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), h = b->GetObjectGuid(), gc = c->GetObjectGuid(), gd = d->GetObjectGuid();
                auto afterFirst = std::make_shared<std::vector<Sample> >();
                auto afterLast = std::make_shared<std::vector<Sample> >();
                auto sample = [this, g, h](std::vector<Sample>& into, uint32 t)
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { return; }
                    Sample s;
                    s.t = t;
                    s.mt = Type(a);
                    s.state = a->Blocked(Motion::ReasonFeared);
                    s.move = a->hasUnitState(UNIT_STAT_FLEEING_MOVE);
                    s.flag = a->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING);
                    Unit* v = a->getVictim() ? a->getVictim() : b;
                    s.dVictim = Dist2(a->Where().X(), a->Where().Y(), v->Where().X(), v->Where().Y());
                    into.push_back(s);
                    Log("+%5ums mt=%s state=%d move=%d flag=%d dVictim=%.1f", t, Motion::KindName(s.mt), s.state ? 1 : 0, s.move ? 1 : 0, s.flag ? 1 : 0, s.dVictim);
                };
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h); if (!a || !b) { return; }
                    a->Attack(b, true);
                    a->AddThreat(b, 1000.0f);   // aggroed: the end-of-control rule resumes a chase only for a wolf with threat
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                    Log("Attack + MoveChase, mt=%s", TypeName(a));
                });
                At(1500, [this, g, gc]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(true, gc, 5782, 0, 0);
                    Log("fear A (5782) applied, mt=%s", TypeName(a));
                });
                At(3500, [this, g, gd]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(true, gd, 8122, 0, 0);
                    Log("fear B (8122) applied over it, mt=%s", TypeName(a));
                });
                At(5500, [this, g, gc]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(false, gc, 5782, 0, 0);
                    Log("fear A removed, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 6; ++i)
                {
                    At(5500 + i * 400, [sample, afterFirst, i]() { sample(*afterFirst, 5500 + i * 400); });
                }
                At(8500, [this, g, gd]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(false, gd, 8122, 0, 0);
                    Log("fear B removed, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 8; ++i)
                {
                    At(8500 + i * 300, [sample, afterLast, i]() { sample(*afterLast, 8500 + i * 300); });
                }
                At(11200, [this, afterFirst, afterLast]()
                {
                    std::string first, last;
                    if (afterFirst->empty() || afterLast->empty())
                    {
                        Verdict("secondFearKeepsFleeing=INVALID(no samples) | lastFearResumesChase=INVALID(no samples)");
                        return;
                    }
                    bool keptFleeing = true;
                    for (size_t k = 0; k < afterFirst->size(); ++k)
                    {
                        Sample const& s = (*afterFirst)[k];
                        if (s.mt != Motion::Kind::Fear || !s.state || !s.flag) { keptFleeing = false; }
                    }
                    char text[160];
                    if (keptFleeing)
                    {
                        first = "OK(fear B kept the wolf fleeing after fear A's removal, state and flag held)";
                    }
                    else
                    {
                        Sample const& s = (*afterFirst)[0];
                        snprintf(text, sizeof(text), "BUG(after fear A's removal: mt=%s state=%d flag=%d)", Motion::KindName(s.mt), s.state ? 1 : 0, s.flag ? 1 : 0);
                        first = text;
                    }
                    Sample const& soon = (*afterLast)[std::min<size_t>(2, afterLast->size() - 1)];   // +900 ms
                    Sample const& end = afterLast->back();
                    const bool closed = end.dVictim < (*afterLast)[0].dVictim + 1.0f || end.dVictim <= 4.0f;   // closes on the victim, or already within melee reach
                    if (soon.mt == Motion::Kind::Chase && !end.state && !end.move && !end.flag && closed)
                    {
                        snprintf(text, sizeof(text), "OK(chase back within a second of fear B's removal, state and flag clear, %.1f -> %.1f yd)", (*afterLast)[0].dVictim, end.dVictim);
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "BUG(after fear B's removal: mt=%s at +%ums, state=%d move=%d flag=%d, %.1f -> %.1f yd)",
                                 Motion::KindName(soon.mt), soon.t - 8500, end.state ? 1 : 0, end.move ? 1 : 0, end.flag ? 1 : 0, (*afterLast)[0].dVictim, end.dVictim);
                    }
                    last = text;
                    Verdict("secondFearKeepsFleeing=" + first + " | lastFearResumesChase=" + last);
                });
            }
        };

        /// P4-A: a wandering wolf with no victim, feared by a hostile caster; when the fear ends
        /// it runs home instead of resuming its wander where the fear left it (reference
        /// §3.1.6, §13.3; the caster is killed first so the wolf has nothing to attack). The
        /// wolf reaches its spawn and its wander default resumes there, so the verdict looks
        /// at the closest sample over the window rather than the last one. A wolf feared only a
        /// few yards from home can be home before the first sample; the verdict reads HOME at
        /// the removal itself as well as in the samples.
        class FearThenHome : public Scenario
        {
        public:
            FearThenHome() : Scenario("fear-then-home", 21) {}

            void Prepare() override
            {
                struct Sample { uint32 t; Motion::Kind mt; float dHome; };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("fearEndGoesHome=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000);
                k->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                const float hx = SE.x, hy = SE.y;
                auto samples = std::make_shared<std::vector<Sample> >();
                auto homeAtRemoval = std::make_shared<bool>(false);
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveRandomAroundPoint(a->Where().X(), a->Where().Y(), a->Where().Z(), 8.0f);
                    Log("MoveRandomAroundPoint(8), mt=%s", TypeName(a));
                });
                At(1500, [this, g, gk]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(true, gk, 5782, 0, 0);
                    Log("feared by the kobold, mt=%s", TypeName(a));
                });
                At(5500, [this, g, gk, hx, hy, homeAtRemoval]()
                {
                    Creature* a = Get(g); Creature* k = Get(gk); if (!a || !k) { return; }
                    k->DealDamage(k, k->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);   // nothing to attack afterwards
                    a->SetFeared(false, gk, 5782, 0, 0);
                    *homeAtRemoval = Type(a) == Motion::Kind::Home;   // a wolf feared a few yards from home is back before the first sample
                    Log("fear removed %.1f yd from home, mt=%s", Dist2(a->Where().X(), a->Where().Y(), hx, hy), TypeName(a));
                });
                for (uint32 i = 1; i <= 12; ++i)
                {
                    At(5500 + i * 400, [this, g, hx, hy, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 400;
                        s.mt = Type(a);
                        s.dHome = Dist2(a->Where().X(), a->Where().Y(), hx, hy);
                        samples->push_back(s);
                        Log("+%4ums mt=%s dHome=%.1f", s.t, Motion::KindName(s.mt), s.dHome);
                    });
                }
                At(10700, [this, samples, homeAtRemoval]()
                {
                    if (samples->size() < 3) { Verdict("fearEndGoesHome=INVALID(no samples)"); return; }
                    bool sawHome = *homeAtRemoval;
                    for (size_t k = 0; k < samples->size() && (*samples)[k].t <= 2000; ++k)
                    {
                        if ((*samples)[k].mt == Motion::Kind::Home) { sawHome = true; }
                    }
                    Sample const& first = samples->front();
                    Sample const& last = samples->back();
                    float closest = first.dHome;
                    for (size_t k = 1; k < samples->size(); ++k)
                    {
                        if ((*samples)[k].dHome < closest) { closest = (*samples)[k].dHome; }
                    }
                    const float bar = first.dHome - 2.0f > 1.0f ? first.dHome - 2.0f : 1.0f;   // arrival counts even when the fear left it near home
                    char text[192];
                    if (sawHome && closest < bar)
                    {
                        snprintf(text, sizeof(text), "fearEndGoesHome=OK(HOME within two seconds, %.1f -> closest %.1f yd from home, last mt=%s)", first.dHome, closest, Motion::KindName(last.mt));
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "fearEndGoesHome=BUG(home %s, %.1f -> closest %.1f yd from home, last mt=%s)",
                                 sawHome ? "seen" : "never seen", first.dHome, closest, Motion::KindName(last.mt));
                    }
                    Verdict(text);
                });
            }
        };

        /// P4-A: a confuse over a fear on an aggroed, chasing wolf. The confuse drives while both are
        /// held (Confused outranks Fear, reference §13.2); its removal resumes the fear; the
        /// fear's removal resumes the chase.
        class ConfuseOverFear : public Scenario
        {
        public:
            ConfuseOverFear() : Scenario("confuse-over-fear", 22) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                Creature* c = Spawn(KOBOLD, SE.x + 25.0f, SE.y + 15.0f, Ground(SE.x + 25.0f, SE.y + 15.0f, SE.z), 3.1f);
                if (!a || !b || !c) { Verdict("confuseOutranksFear=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000);
                b->SetMaxHealth(500000); b->SetHealth(500000);
                b->setFaction(14); c->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), h = b->GetObjectGuid(), gc = c->GetObjectGuid();
                auto both = std::make_shared<std::vector<Motion::Kind> >();
                auto afterConfuse = std::make_shared<std::vector<Motion::Kind> >();
                auto afterFear = std::make_shared<std::vector<Motion::Kind> >();
                auto sample = [this, g](std::vector<Motion::Kind>& into, char const* phase, uint32 t)
                {
                    Creature* a = Get(g); if (!a) { return; }
                    into.push_back(Type(a));
                    Log("%s +%4ums mt=%s", phase, t, TypeName(a));
                };
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h); if (!a || !b) { return; }
                    a->Attack(b, true);
                    a->AddThreat(b, 1000.0f);   // aggroed: the end-of-control rule resumes a chase only for a wolf with threat
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                });
                At(1500, [this, g, gc]() { Creature* a = Get(g); if (a) { a->SetFeared(true, gc, 5782, 0, 0); Log("fear applied, mt=%s", TypeName(a)); } });
                At(3500, [this, g, gc]() { Creature* a = Get(g); if (a) { a->SetConfused(true, gc, 118, 0); Log("confuse applied over it, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 5; ++i) { At(3500 + i * 400, [sample, both, i]() { sample(*both, "both", i * 400); }); }
                At(6500, [this, g, gc]() { Creature* a = Get(g); if (a) { a->SetConfused(false, gc, 118, 0); Log("confuse removed, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 5; ++i) { At(6500 + i * 400, [sample, afterConfuse, i]() { sample(*afterConfuse, "fear-only", i * 400); }); }
                At(9500, [this, g, gc]() { Creature* a = Get(g); if (a) { a->SetFeared(false, gc, 5782, 0, 0); Log("fear removed, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 5; ++i) { At(9500 + i * 300, [sample, afterFear, i]() { sample(*afterFear, "none", i * 300); }); }
                At(11300, [this, both, afterConfuse, afterFear]()
                {
                    auto all = [](std::vector<Motion::Kind> const& v, Motion::Kind t)
                    {
                        if (v.empty()) { return false; }
                        for (size_t k = 0; k < v.size(); ++k) { if (v[k] != t) { return false; } }
                        return true;
                    };
                    std::string body;
                    body += std::string("confuseOutranksFear=") + (all(*both, Motion::Kind::Confused) ? "OK(CONFUSED while both held)" : (both->empty() ? "INVALID(no samples)" : std::string("BUG(") + Motion::KindName(both->front()) + " while both held)"));
                    body += std::string(" | fearResumesAfterConfuse=") + (all(*afterConfuse, Motion::Kind::Fear) ? "OK(FLEEING after the confuse's removal)" : (afterConfuse->empty() ? "INVALID(no samples)" : std::string("BUG(") + Motion::KindName(afterConfuse->front()) + " after the confuse's removal)"));
                    const bool chase = afterFear->size() >= 3 && (*afterFear)[2] == Motion::Kind::Chase;   // +900 ms
                    body += std::string(" | chaseResumesLast=") + (chase ? "OK(CHASE within a second of the fear's removal)" : (afterFear->empty() ? "INVALID(no samples)" : std::string("BUG(") + Motion::KindName(afterFear->size() >= 3 ? (*afterFear)[2] : afterFear->back()) + " after the fear's removal)"));
                    Verdict(body);
                });
            }
        };

        /// P4-A: the low-health flee (a timed fear from the victim, spell 0) ends on its own
        /// and must leave no fleeing flag behind; today nothing cleared UNIT_FLAG_FLEEING after
        /// it. The chase resumes through the timed behaviour's own re-engagement.
        class TimedFleeCleans : public Scenario
        {
        public:
            TimedFleeCleans() : Scenario("timed-flee-cleans", 23) {}

            void Prepare() override
            {
                struct Sample
                {
                    uint32       t;
                    Motion::Kind mt;
                    bool         flag;
                    bool         state;
                    bool         timed = false;   ///< the selected fear was the timed variant (SelectedVariant() == 1)
                };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!a || !b) { Verdict("timedFleeCleans=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000);
                b->SetMaxHealth(500000); b->SetHealth(500000);
                b->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), h = b->GetObjectGuid();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h); if (!a || !b) { return; }
                    a->Attack(b, true);
                    a->AddThreat(b, 1000.0f);   // aggroed: the end-of-control rule resumes a chase only for a wolf with threat
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                });
                At(1500, [this, g, h]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(true, h, 0, 3000);   // the low-health flee's shape: the victim as the source, three seconds
                    Log("timed flee from the victim, mt=%s flag=%d", TypeName(a), a->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING) ? 1 : 0);
                });
                for (uint32 i = 1; i <= 14; ++i)
                {
                    At(1500 + i * 400, [this, g, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 400;
                        s.mt = Type(a);
                        s.timed = a->GetMotionMaster()->SelectedVariant() == 1;
                        s.flag = a->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING);
                        s.state = a->Blocked(Motion::ReasonFeared) || a->hasUnitState(UNIT_STAT_FLEEING_MOVE);
                        samples->push_back(s);
                        Log("+%4ums mt=%s flag=%d state=%d", s.t, Motion::KindName(s.mt), s.flag ? 1 : 0, s.state ? 1 : 0);
                    });
                }
                At(7400, [this, samples]()
                {
                    if (samples->empty()) { Verdict("timedFleeCleans=INVALID(no samples)"); return; }
                    bool fled = false;
                    for (size_t k = 0; k < samples->size(); ++k) { if ((*samples)[k].mt == Motion::Kind::Fear && (*samples)[k].timed) { fled = true; } }
                    Sample const& end = samples->back();
                    char text[160];
                    if (fled && end.mt == Motion::Kind::Chase && !end.flag && !end.state)
                    {
                        snprintf(text, sizeof(text), "timedFleeCleans=OK(fled, then chased with the flag and state clear)");
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "timedFleeCleans=BUG(fled=%d, end mt=%s flag=%d state=%d)", fled ? 1 : 0, Motion::KindName(end.mt), end.flag ? 1 : 0, end.state ? 1 : 0);
                    }
                    Verdict(text);
                });
            }
        };

        /// P4-A: a script's "stop whatever is moving" under a fear. The clear leaves the fear
        /// (a claim is its aura's) and cuts the chase beneath; the script's point waits under
        /// the fear and runs when the fear ends; the re-issued chase waits under the point.
        class ClearUnderFear : public Scenario
        {
        public:
            ClearUnderFear() : Scenario("clear-under-fear", 24) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                Creature* c = Spawn(KOBOLD, SE.x + 25.0f, SE.y + 15.0f, Ground(SE.x + 25.0f, SE.y + 15.0f, SE.z), 3.1f);
                if (!a || !b || !c) { Verdict("clearKeepsFear=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000);
                b->SetMaxHealth(500000); b->SetHealth(500000);
                b->setFaction(14); c->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), h = b->GetObjectGuid(), gc = c->GetObjectGuid();
                const float px = SE.x - 15.0f, py = SE.y + 10.0f;
                auto afterClear = std::make_shared<std::vector<Motion::Kind> >();
                auto afterFear = std::make_shared<std::vector<Motion::Kind> >();
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h); if (!a || !b) { return; }
                    a->Attack(b, true);
                    a->AddThreat(b, 1000.0f);   // aggroed: the end-of-control rule resumes a chase only for a wolf with threat
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                });
                At(1500, [this, g, gc]() { Creature* a = Get(g); if (a) { a->SetFeared(true, gc, 5782, 0, 0); Log("fear applied, mt=%s", TypeName(a)); } });
                At(3500, [this, g, px, py]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->Clear(false);                                        // a script's stop
                    a->GetMotionMaster()->MovePoint(1, px, py, Ground(px, py, a->Where().Z()), true);   // and its point (ScenariosPoint.cpp's call shape)
                    Log("script Clear(false) + MovePoint under the fear, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 6; ++i)
                {
                    At(3500 + i * 400, [this, g, afterClear, i]() { Creature* a = Get(g); if (a) { afterClear->push_back(Type(a)); Log("after-clear +%4ums mt=%s", i * 400, TypeName(a)); } });
                }
                At(6500, [this, g, gc]() { Creature* a = Get(g); if (a) { a->SetFeared(false, gc, 5782, 0, 0); Log("fear removed, mt=%s", TypeName(a)); } });
                for (uint32 i = 1; i <= 6; ++i)
                {
                    At(6500 + i * 300, [this, g, afterFear, i]() { Creature* a = Get(g); if (a) { afterFear->push_back(Type(a)); Log("after-fear +%4ums mt=%s", i * 300, TypeName(a)); } });
                }
                At(8600, [this, afterClear, afterFear]()
                {
                    bool keptFear = !afterClear->empty();
                    for (size_t k = 0; k < afterClear->size(); ++k) { if ((*afterClear)[k] != Motion::Kind::Fear) { keptFear = false; } }
                    const bool point = afterFear->size() >= 3 && (*afterFear)[2] == Motion::Kind::Point;   // +900 ms
                    std::string body = std::string("clearKeepsFear=") + (keptFear ? "OK(still FLEEING after a script's Clear(false))" : (afterClear->empty() ? "INVALID(no samples)" : std::string("BUG(") + Motion::KindName(afterClear->front()) + " after the clear)"));
                    body += std::string(" | pointRunsAfterFear=") + (point ? "OK(the script's point runs within a second of the fear's end)" : (afterFear->empty() ? "INVALID(no samples)" : std::string("BUG(") + Motion::KindName(afterFear->size() >= 3 ? (*afterFear)[2] : afterFear->back()) + " after the fear's end)"));
                    Verdict(body);
                });
            }
        };

        const uint32 ROOT = 745;   // Web: a plain root aura (SPELL_AURA_MOD_ROOT for about 5 s), no damage

        /// A root on a feared creature: the flee stops in place while the root lasts (the unit
        /// state is what the movement gates read) and resumes when the root ends (reference
        /// §3.6); the fear itself is not cut by the root.
        class RootUnderFear : public Scenario
        {
        public:
            RootUnderFear() : Scenario("root-under-fear", 25) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!a || !k) { Verdict("rootHoldsFear=INVALID(spawn failed) | fleeResumesAfterRoot=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000); k->SetMaxHealth(500000); k->SetHealth(500000);
                k->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), gk = k->GetObjectGuid();
                auto rooted = std::make_shared<std::vector<Pt> >();
                auto after = std::make_shared<std::vector<Pt> >();
                auto keptFear = std::make_shared<bool>(true);
                auto legAfter = std::make_shared<bool>(false);   // the behaviour laid a leg after the unroot (its move bit)
                At(500, [this, g, gk]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->SetFeared(true, gk, 5782, 0, 0);
                    Log("feared, mt=%s", TypeName(a));
                });
                At(2500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->CastSpell(a, ROOT, true);
                    Log("Web cast mid-flee, mt=%s (a projectile: the root lands on the next tick)", TypeName(a));
                });
                for (uint32 i = 1; i <= 8; ++i)
                {
                    At(2500 + i * 400, [this, g, rooted, keptFear, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        rooted->push_back(p);
                        if (Type(a) != Motion::Kind::Fear) { *keptFear = false; }
                        Log("rooted +%4ums mt=%s root state=%d at %.1f %.1f", i * 400, TypeName(a), a->IsRooted() ? 1 : 0, p.x, p.y);
                    });
                }
                At(6000, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->RemoveAurasDueToSpell(ROOT);
                    Log("root removed, mt=%s root state=%d", TypeName(a), a->IsRooted() ? 1 : 0);
                });
                for (uint32 i = 1; i <= 10; ++i)   // four seconds: the flee rests up to 1.5 s between bolts, and a bolt can be short
                {
                    At(6000 + i * 400, [this, g, after, legAfter, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        after->push_back(p);
                        const bool leg = a->hasUnitState(UNIT_STAT_FLEEING_MOVE);   // set when a leg starts, cleared by the gate
                        if (leg) { *legAfter = true; }
                        Log("after +%4ums mt=%s move=%d at %.1f %.1f", i * 400, TypeName(a), leg ? 1 : 0, p.x, p.y);
                    });
                }
                At(10300, [this, rooted, after, keptFear, legAfter]()
                {
                    if (rooted->size() < 3 || after->size() < 3) { Verdict("rootHoldsFear=INVALID(no samples) | fleeResumesAfterRoot=INVALID(no samples)"); return; }
                    const float held = Spread(*rooted);
                    const float resumed = Spread(*after);
                    char text[200];
                    if (*keptFear && held < 1.0f)
                    {
                        snprintf(text, sizeof(text), "rootHoldsFear=OK(stood within %.1f yd for 3 s, still FLEEING)", held);
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "rootHoldsFear=BUG(moved %.1f yd while rooted%s)", held, *keptFear ? "" : ", the fear was cut");
                    }
                    std::string body = text;
                    // The gate reopened when the behaviour laid a leg again (its move bit) or the wolf
                    // got away; a bolt's length and the pauses between bolts are the flee geometry's.
                    if (*legAfter || resumed > 3.0f)
                    {
                        snprintf(text, sizeof(text), " | fleeResumesAfterRoot=OK(a leg laid again, moved %.1f yd within 4 s of the unroot)", resumed);
                    }
                    else
                    {
                        snprintf(text, sizeof(text), " | fleeResumesAfterRoot=BUG(no leg laid after the unroot, moved %.1f yd)", resumed);
                    }
                    Verdict(body + text);
                });
            }
        };

        /// A root on a chasing creature: the chase stops in place while the root lasts and
        /// closes again when the root ends; the wolf's own position is what is measured, since
        /// the kobold walks up to a rooted wolf on its own.
        class RootMidChase : public Scenario
        {
        public:
            RootMidChase() : Scenario("root-mid-chase", 26) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* b = Spawn(KOBOLD, SE.x + 25.0f, SE.y, Ground(SE.x + 25.0f, SE.y, SE.z), 3.1f);
                if (!a || !b) { Verdict("rootHoldsChase=INVALID(spawn failed) | chaseResumesAfterRoot=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000); b->SetMaxHealth(500000); b->SetHealth(500000);
                b->setFaction(14);
                const ObjectGuid g = a->GetObjectGuid(), h = b->GetObjectGuid();
                auto rooted = std::make_shared<std::vector<Pt> >();
                auto after = std::make_shared<std::vector<Pt> >();
                auto keptChase = std::make_shared<bool>(true);
                auto legAfter = std::make_shared<bool>(false);   // the chase laid a leg after the unroot (its move bit)
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h); if (!a || !b) { return; }
                    a->Attack(b, true);
                    a->AddThreat(b, 1000.0f);
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                    Log("Attack + MoveChase from 25 yd, mt=%s", TypeName(a));
                });
                At(1300, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->CastSpell(a, ROOT, true);
                    Log("Web cast mid-chase, mt=%s (a projectile: the root lands on the next tick)", TypeName(a));
                });
                for (uint32 i = 1; i <= 8; ++i)
                {
                    At(1300 + i * 400, [this, g, rooted, keptChase, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        rooted->push_back(p);
                        if (Type(a) != Motion::Kind::Chase) { *keptChase = false; }
                        Log("rooted +%4ums mt=%s root state=%d at %.1f %.1f", i * 400, TypeName(a), a->IsRooted() ? 1 : 0, p.x, p.y);
                    });
                }
                At(4800, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->RemoveAurasDueToSpell(ROOT);
                    Log("root removed, mt=%s root state=%d", TypeName(a), a->IsRooted() ? 1 : 0);
                });
                for (uint32 i = 1; i <= 6; ++i)
                {
                    At(4800 + i * 400, [this, g, h, after, legAfter, i]()
                    {
                        Creature* a = Get(g); Creature* b = Get(h); if (!a || !b) { return; }
                        Pt p = { a->Where().X(), a->Where().Y(), a->Where().Z() };
                        after->push_back(p);
                        const bool leg = a->hasUnitState(UNIT_STAT_CHASE_MOVE);   // set per chase leg, cleared by the gate
                        if (leg) { *legAfter = true; }
                        Log("after +%4ums mt=%s move=%d at %.1f %.1f dVictim=%.1f", i * 400, TypeName(a), leg ? 1 : 0, p.x, p.y, Dist2(p.x, p.y, b->Where().X(), b->Where().Y()));
                    });
                }
                At(7500, [this, g, h, rooted, after, keptChase, legAfter]()
                {
                    if (rooted->size() < 3 || after->size() < 3) { Verdict("rootHoldsChase=INVALID(no samples) | chaseResumesAfterRoot=INVALID(no samples)"); return; }
                    Creature* a = Get(g); Creature* b = Get(h);
                    const float held = Spread(*rooted);
                    const float resumed = Spread(*after);
                    const float dVictim = (a && b) ? Dist2(a->Where().X(), a->Where().Y(), b->Where().X(), b->Where().Y()) : 999.0f;
                    char text[200];
                    if (*keptChase && held < 1.0f)
                    {
                        snprintf(text, sizeof(text), "rootHoldsChase=OK(stood within %.1f yd for 3 s, still CHASE)", held);
                    }
                    else
                    {
                        snprintf(text, sizeof(text), "rootHoldsChase=BUG(moved %.1f yd while rooted%s)", held, *keptChase ? "" : ", the chase was cut");
                    }
                    std::string body = text;
                    // The wolf's own movement proves the resume (the kobold walks up to a rooted wolf on
                    // its own): a chase leg laid again, or the wolf got away from where it stood.
                    if (*legAfter || resumed > 3.0f)
                    {
                        snprintf(text, sizeof(text), " | chaseResumesAfterRoot=OK(a leg laid again, moved %.1f yd after the unroot, %.1f yd from the victim)", resumed, dVictim);
                    }
                    else
                    {
                        snprintf(text, sizeof(text), " | chaseResumesAfterRoot=BUG(no leg laid after the unroot, moved %.1f yd, %.1f yd from the victim)", resumed, dVictim);
                    }
                    Verdict(body + text);
                });
            }
        };
    }

    void RegisterFleeScenarios(Runner& r)
    {
        r.Register(new FleeDriftsBack());
        r.Register(new DistractOverAssist());
        r.Register(new DistractThenAttack());
        r.Register(new FearTwice());
        r.Register(new FearThenHome());
        r.Register(new ConfuseOverFear());
        r.Register(new TimedFleeCleans());
        r.Register(new ClearUnderFear());
        r.Register(new RootUnderFear());
        r.Register(new RootMidChase());
    }
}
