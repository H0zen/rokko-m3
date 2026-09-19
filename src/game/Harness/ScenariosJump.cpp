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

// The jump family of the old harness: S1 jump-over-point, S2 jump-over-chase,
// S11 stun-mid-jump, S12 back-to-back-jumps. Coordinates are the old file's
// Mulgore points.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;
        const uint32 STUN = 5211;   // Bash: a plain stun aura

        struct Pt { float x, y, z; };
        const Pt P0 = { -3122.6f, -261.3f, 46.0f };
        const Pt T1 = { -3092.6f, -261.3f, 46.0f };
        const Pt SA = { -3257.5f, -351.6f, 47.8f };
        const Pt SB = { -3228.0f, -351.6f, 47.8f };
        const Pt SD = { -3122.6f, -261.3f, 46.0f };
        const Pt SE = { -3200.0f, -300.0f, 47.0f };

        /// S1: a MoveJump launched over a live MovePoint leg must complete (B1), the point
        /// leg beneath must resume (B5), and the point's inform must fire at the real
        /// point or not at all (B4).
        class JumpOverPoint : public Scenario
        {
        public:
            JumpOverPoint() : Scenario("jump-over-point", 1) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float x, y, z; Motion::Kind mt; float dJ, dPre; };
                Creature* a = Spawn(WOLF, P0.x, P0.y, P0.z, 0.0f);
                if (!a) { Verdict("B1=INVALID(spawn failed)"); return; }
                Load(T1.x, T1.y);
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                Log("spawned wolf guid=%u at %.1f %.1f %.1f mt=%s", low, P0.x, P0.y, P0.z, TypeName(a));
                // Shared between the steps: the pre-jump spot and the samples.
                auto pre = std::make_shared<Pt>();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    a->GetMotionMaster()->MovePoint(77, T1.x, T1.y, Ground(T1.x, T1.y, T1.z), true);
                    Log("MovePoint(77) -> %.1f %.1f mt=%s", T1.x, T1.y, TypeName(a));
                });
                At(1700, [this, g, pre]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    pre->x = a->Where().X(); pre->y = a->Where().Y(); pre->z = a->Where().Z();
                    Log("pre-jump at %.2f %.2f %.2f mt=%s (moved %.1f yd from spawn)", pre->x, pre->y, pre->z, TypeName(a), Dist2(pre->x, pre->y, P0.x, P0.y));
                    a->GetMotionMaster()->MoveJump(P0.x, P0.y, P0.z, 7.5f, 5.0f, 99);
                    Log("MoveJump -> %.1f %.1f %.1f (%.1f yd) mt=%s", P0.x, P0.y, P0.z, Dist2(pre->x, pre->y, P0.x, P0.y), TypeName(a));
                });
                for (uint32 i = 1; i <= 16; ++i)
                {
                    At(1700 + i * 200, [this, g, pre, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 200; s.x = a->Where().X(); s.y = a->Where().Y(); s.z = a->Where().Z(); s.mt = Type(a);
                        s.dJ = Dist2(s.x, s.y, P0.x, P0.y); s.dPre = Dist2(s.x, s.y, pre->x, pre->y);
                        samples->push_back(s);
                        Log("+%4ums %.2f %.2f %.2f mt=%s dJump=%.2f dPre=%.2f", s.t, s.x, s.y, s.z, Motion::KindName(s.mt), s.dJ, s.dPre);
                    });
                }
                At(5200, [this, low, samples]()
                {
                    if (samples->empty()) { Verdict("INVALID(no samples)"); return; }
                    float early = 0.0f; bool reached = false;
                    for (size_t k = 0; k < samples->size(); ++k)
                    {
                        Sample const& s = (*samples)[k];
                        if (s.t <= 800 && s.dPre > early) { early = s.dPre; }
                        if (s.dJ < 2.0f) { reached = true; }
                    }
                    Sample const& f = samples->back();
                    std::string b1 = reached ? "OK(jump completed server-side)" : (early < 1.0f ? "BUG(spline killed at launch: never left pre-jump spot)" : "PARTIAL");
                    std::string b5 = f.mt == Motion::Kind::Point ? "OK(point leg resumed)" : std::string("BUG(stack reset to ") + Motion::KindName(f.mt) + ")";
                    std::string b4 = "OK(no inform)";
                    for (size_t k = 0; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.kind == Motion::Kind::Point && r.id == 77)
                        {
                            const float shortBy = Dist2(r.x, r.y, T1.x, T1.y);
                            char text[96];
                            snprintf(text, sizeof(text), "BUG(POINT 77 inform fired %.1f yd short of target)", shortBy);
                            b4 = shortBy > 3.0f ? text : "OK(real arrival)";
                        }
                    }
                    Verdict("B1=" + b1 + " | B5=" + b5 + " | B4=" + b4);
                });
            }
        };

        /// S2: a MoveJump launched over a live MoveChase leg with a real victim
        /// must complete server-side (B1), as players see it.
        class JumpOverChase : public Scenario
        {
        public:
            JumpOverChase() : Scenario("jump-over-chase", 2) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float x, y, z; Motion::Kind mt; float dJ, dPre, dV; };
                Creature* a = Spawn(WOLF, SA.x, SA.y, SA.z, 0.0f);
                Creature* b = Spawn(KOBOLD, SB.x, SB.y, Ground(SB.x, SB.y, SB.z), 3.1f);
                if (!a || !b) { Verdict("B1=INVALID(spawn failed)"); return; }
                a->SetMaxHealth(500000); a->SetHealth(500000);
                b->SetMaxHealth(500000); b->SetHealth(500000);
                b->setFaction(14);   // Monster: hostile to the wolf so AttackStart really engages
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid h = b->GetObjectGuid();
                auto pre = std::make_shared<Pt>();
                auto jump = std::make_shared<Pt>();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { Log("ERR actors gone"); return; }
                    bool ok = a->Attack(b, true);   // Unit::Attack sets the victim directly, bypassing the AI's refusal
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                    Log("Attack=%s + MoveChase: wolf mt=%s victim=%s", ok ? "true" : "false", TypeName(a), a->getVictim() ? "true" : "false");
                });
                At(1700, [this, g, h, pre, jump]()
                {
                    Creature* a = Get(g); if (!a) { Log("ERR wolf gone"); return; }
                    pre->x = a->Where().X(); pre->y = a->Where().Y(); pre->z = a->Where().Z();
                    Creature* b = Get(h);
                    float bx = 0.0f, by = 0.0f;
                    if (b) { bx = b->Where().X(); by = b->Where().Y(); }
                    Log("pre-jump at %.2f %.2f %.2f mt=%s victim=%s (moved %.1f yd from spawn, %.1f yd to kobold)",
                        pre->x, pre->y, pre->z, TypeName(a), a->getVictim() ? "true" : "false",
                        Dist2(pre->x, pre->y, SA.x, SA.y), Dist2(pre->x, pre->y, bx, by));
                    jump->x = pre->x; jump->y = pre->y + 12.0f; jump->z = Ground(jump->x, jump->y, pre->z);
                    a->GetMotionMaster()->MoveJump(jump->x, jump->y, jump->z, 7.5f, 5.0f, 98);
                    Log("MoveJump 12 yd sideways -> %.1f %.1f %.1f mt=%s", jump->x, jump->y, jump->z, TypeName(a));
                });
                for (uint32 i = 1; i <= 16; ++i)
                {
                    At(1700 + i * 200, [this, g, h, jump, pre, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 200; s.x = a->Where().X(); s.y = a->Where().Y(); s.z = a->Where().Z(); s.mt = Type(a);
                        Creature* b = Get(h);
                        float bx = 0.0f, by = 0.0f;
                        if (b) { bx = b->Where().X(); by = b->Where().Y(); }
                        s.dJ = Dist2(s.x, s.y, jump->x, jump->y);
                        s.dPre = Dist2(s.x, s.y, pre->x, pre->y);
                        s.dV = Dist2(s.x, s.y, bx, by);
                        samples->push_back(s);
                        Log("+%4ums %.2f %.2f %.2f mt=%s victim=%s dJump=%.2f dPre=%.2f dKobold=%.2f",
                            s.t, s.x, s.y, s.z, Motion::KindName(s.mt), a->getVictim() ? "true" : "false", s.dJ, s.dPre, s.dV);
                    });
                }
                At(5200, [this, samples]()
                {
                    if (samples->empty()) { Verdict("INVALID(no samples)"); return; }
                    float early = 0.0f; bool reached = false;
                    for (size_t k = 0; k < samples->size(); ++k)
                    {
                        Sample const& s = (*samples)[k];
                        if (s.t <= 800 && s.dPre > early) { early = s.dPre; }
                        if (s.dJ < 2.0f) { reached = true; }
                    }
                    Sample const& f = samples->back();
                    std::string b1 = reached ? "OK(jump completed server-side)" : (early < 1.0f ? "BUG(spline killed at launch: chase re-laid from pre-jump spot)" : "PARTIAL");
                    char dv[32];
                    snprintf(dv, sizeof(dv), "%.1f", f.dV);
                    Verdict("B1=" + b1 + " | final mt=" + Motion::KindName(f.mt) + " dKobold=" + dv);
                });
            }
        };

        /// S11: a stun landing mid-jump must not spuriously inform EFFECT 66; the
        /// point leg beneath the jump must resume once the stun wears off.
        class StunMidJump : public Scenario
        {
        public:
            StunMidJump() : Scenario("stun-mid-jump", 10) {}

            void Prepare() override
            {
                const size_t mark = Informs().size();
                Creature* a = Spawn(WOLF, SD.x, SD.y, SD.z, 0.0f);
                if (!a) { Verdict("stunMidJump=INVALID(spawn failed)"); return; }
                Load(SD.x - 30.0f, SD.y);
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                auto jump = std::make_shared<Pt>();
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MovePoint(1, SD.x - 30.0f, SD.y, Ground(SD.x - 30.0f, SD.y, SD.z), true);
                });
                At(2000, [this, g, jump]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    const float x = a->Where().X(), y = a->Where().Y(), z = a->Where().Z();
                    jump->x = x; jump->y = y + 12.0f;
                    a->GetMotionMaster()->MoveJump(x, y + 12.0f, Ground(x, y + 12.0f, z), 7.5f, 5.0f, 66);
                    Log("MoveJump 12 yd over the point leg, mt=%s", TypeName(a));
                });
                At(2400, [this, g, jump]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->CastSpell(a, STUN, true);
                    const float x = a->Where().X(), y = a->Where().Y();
                    Log("STUN 0.4 s into the jump at %.1f %.1f (%.1f yd short of the jump point) mt=%s", x, y, Dist2(x, y, jump->x, jump->y), TypeName(a));
                });
                At(9000, [this, g, low, jump, mark]()
                {
                    Creature* a = Get(g);
                    bool fired = false;
                    Inform lastFired = Inform();
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.kind == Motion::Kind::Effect && r.id == 66) { lastFired = r; fired = true; }
                    }
                    char const* mt = a ? TypeName(a) : "?";
                    std::string v;
                    if (fired)
                    {
                        const float shortBy = Dist2(lastFired.x, lastFired.y, jump->x, jump->y);
                        if (shortBy < 2.5f)
                        {
                            v = "OK(the jump landed despite the stun and informed at the landing)";
                        }
                        else
                        {
                            char text[128];
                            snprintf(text, sizeof(text), "BUG(EFFECT 66 inform fired %.1f yd short of the jump point: the stun cut the jump)", shortBy);
                            v = text;
                        }
                    }
                    else
                    {
                        v = "BUG(no EFFECT inform at all: the jump's end was lost)";
                    }
                    Verdict("stunMidJump=" + v + " | after the stun mt=" + mt + " (POINT = the leg beneath resumed)");
                });
            }
        };

        /// S12: two jumps launched back to back; the second must complete and
        /// inform despite being cut short by the first.
        class BackToBackJumps : public Scenario
        {
        public:
            BackToBackJumps() : Scenario("back-to-back-jumps", 11) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float dJ1, dJ2; Motion::Kind mt; };
                Creature* a = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                if (!a) { Verdict("backToBackJumps=INVALID(spawn failed)"); return; }
                const Pt j1 = { SE.x + 12.0f, SE.y, SE.z };
                const Pt j2 = { SE.x + 12.0f, SE.y + 12.0f, SE.z };
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                const size_t mark = Informs().size();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g, j1]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveJump(j1.x, j1.y, Ground(j1.x, j1.y, SE.z), 7.5f, 5.0f, 71);
                    Log("jump 1 launched");
                });
                At(800, [this, g, j2]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MoveJump(j2.x, j2.y, Ground(j2.x, j2.y, SE.z), 7.5f, 5.0f, 72);
                    Log("jump 2 launched 0.3 s later, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 10; ++i)
                {
                    At(800 + i * 400, [this, g, j1, j2, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 400;
                        const float x = a->Where().X(), y = a->Where().Y();
                        s.dJ1 = Dist2(x, y, j1.x, j1.y); s.dJ2 = Dist2(x, y, j2.x, j2.y); s.mt = Type(a);
                        samples->push_back(s);
                        Log("+%4ums dJ1=%.1f dJ2=%.1f mt=%s", s.t, s.dJ1, s.dJ2, Motion::KindName(s.mt));
                    });
                }
                At(5500, [this, low, samples, mark]()
                {
                    float mn = 999.0f;
                    for (size_t k = 0; k < samples->size(); ++k)
                    {
                        if ((*samples)[k].dJ2 < mn) { mn = (*samples)[k].dJ2; }
                    }
                    bool informed = false;
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.kind == Motion::Kind::Effect && r.id == 72) { informed = true; }
                    }
                    std::string v;
                    if (mn < 2.5f && informed)
                    {
                        v = "OK(second jump completed and informed)";
                    }
                    else if (mn < 2.5f)
                    {
                        v = "BUG(second jump completed but never informed)";
                    }
                    else
                    {
                        char text[64];
                        snprintf(text, sizeof(text), "BUG(second jump never completed: closest %.1f yd)", mn);
                        v = text;
                    }
                    Verdict("backToBackJumps=" + v);
                });
            }
        };
    }

    void RegisterJumpScenarios(Runner& r)
    {
        r.Register(new JumpOverPoint());
        r.Register(new JumpOverChase());
        r.Register(new StunMidJump());
        r.Register(new BackToBackJumps());
    }
}
