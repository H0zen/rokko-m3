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
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Log.h"
#include "movement/typedefs.h"
#include "movement/JumpArc.h"
#include "movement/MoveSpline.h"

#include <cmath>
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
                    SelfCast(a, STUN);
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

        /**
         * S72 (order 73): HEROIC LEAP'S ARC (live test 2026-09-22, B4 -- "travels a straight
         * path, not an arc"), and the general rule behind it.
         *
         * WHAT THE CAPTURE PROVED AND WHAT IT DID NOT. Nothing is lost on the wire: all three
         * leaps of that session went out with bit 25 set, a vertical acceleration and an
         * effectStart of 0, and inverting `amplitude*8/T^2` gives 2.500 yd every time -- the
         * literal `MoveJump(..., 2.5f)` of Spell::EffectJump. The defect is the number, and
         * more precisely what the number MEANS: SetParabolic's amplitude is a bow about the
         * CHORD, so the chord's own slope competes with it. Measured from the capture: +1.94 yd
         * of rise over the flat 33.4 yd leap, and on the 12.3 yd downhill leap the character
         * never left the ground at all, because the launch vertical velocity came out negative.
         *
         * WHAT THIS SCENARIO DRIVES. Not the spell -- a jump spell needs a destination in the
         * cast's targets and a caster the client is steering, neither of which a headless
         * creature has. It drives the two lines Spell::EffectJump ENDS with, with the same
         * inputs read out of the same DBC rows: spell 6544's own Spell.dbc Speed (35.0) and its
         * SpellEffect.dbc effect 1 (EffectMiscValue 25, EffectMiscValueB 100 -- the 2.5 yd floor
         * and 10 yd ceiling, in tenths of a yard). If those rows ever stop saying that, the
         * verdicts say INVALID and name what they found instead, rather than passing on a
         * fixture that no longer matches the spell.
         *
         * THE THREE LEGS, each measured as the greatest rise above ITS OWN launch point:
         *   A  40 yd on the level        -- the floor is passed, so the apex scales with the
         *                                   leap at last: 3.15 yd where 2.5 was sent before.
         *   B  33.43 yd, 1.20 yd down    -- the capture's own flat leap: 2.50 yd where the old
         *                                   constant delivered 1.936.
         *   C  22.85 yd, 12.30 yd down   -- the capture's own downhill leap: 2.50 yd where the
         *                                   old constant delivered NOTHING. This is the leg the
         *                                   fix exists for, and the one that cannot be bought
         *                                   with a bigger constant alone.
         *
         * Restore the 2.5 f and all three go BUG with the numbers above printed against them.
         */
        class HeroicLeapArc : public Scenario
        {
        public:
            HeroicLeapArc() : Scenario("heroic-leap-arc", 73) {}

            /// Spell 6544, Heroic Leap: the leap whose arc the live test called straight.
            static const uint32 LEAP = 6544;

            /// One leg: what was asked for, and what the rendered path did.
            struct Leg
            {
                bool  ran = false;        ///< the launch step resolved the wolf and called MoveJump
                bool  accepted = false;   ///< MoveJump returned true
                float horizontal = 0.0f;  ///< the leg's ground distance, as asked for
                float deltaZ = 0.0f;      ///< destination Z minus launch Z, as asked for
                float launchX = 0.0f;     ///< where it left from, read at the launch
                float launchY = 0.0f;
                float launchZ = 0.0f;
                float apexWanted = 0.0f;  ///< the clearance the spell's own pair asks for here
                float amplitude = 0.0f;   ///< what was handed to MoveJump
                float apexOld = 0.0f;     ///< what the old fixed 2.5 f would have rendered
                uint32 samples = 0;
                uint32 midAir = 0;        ///< samples strictly between the launch and the landing
                float rise = 0.0f;        ///< the greatest sampled height above launchZ
                float bestFraction = 0.0f;///< how far along the leg that greatest height was
                float maxError = 0.0f;    ///< worst |sampled - the arc's own value at that fraction|
                float worstAt = 0.0f;     ///< the fraction the worst error was at
            };

            void Prepare() override
            {
                struct St
                {
                    bool   dbcRan = false;
                    float  speed = 0.0f;
                    int32  misc0 = 0;
                    int32  misc1 = 0;
                    Leg    level;      ///< A: 40 yd, flat
                    Leg    flat;       ///< B: the capture's 33.43 yd leap, 1.20 down
                    Leg    down;       ///< C: the capture's 22.85 yd leap, 12.30 down
                };

                Creature* a = Spawn(WOLF, P0.x, P0.y, Ground(P0.x, P0.y, P0.z), 0.0f);
                if (!a) { Verdict(Invalid("spawn failed")); return; }
                Silence(a);
                // Idle underneath, so nothing walks him between the legs: a silenced creature
                // still falls back on its factory default when an Effect ends, and a wolf 96 yd
                // from its spawn walks home -- which would move the launch point out from under
                // the next leg and put a live leg under an arc that is meant to be measured on
                // its own.
                a->GetMotionMaster()->MoveIdle();
                const ObjectGuid g = a->GetObjectGuid();
                auto st = std::make_shared<St>();

                // The two DBC rows Spell::EffectJump reads, read here exactly as it reads them.
                At(200, [this, st]()
                {
                    SpellEntry const* info = sSpellStore.LookupEntry(LEAP);
                    SpellEffectEntry const* effect = GetSpellEffectEntry(LEAP, EFFECT_INDEX_1);
                    st->dbcRan = true;
                    st->speed = info ? (info->Speed ? info->Speed : 27.0f) : 0.0f;
                    st->misc0 = effect ? effect->EffectMiscValue_0 : -1;
                    st->misc1 = effect ? effect->EffectMiscValue_1 : -1;
                    Log("spell %u: Spell.dbc Speed %.2f, SpellEffect.dbc effect 1 = %d (Effect), MiscValue %d, MiscValueB %d -> floor %.2f yd, ceiling %.2f yd",
                        LEAP, st->speed, effect ? int32(effect->Effect) : -1, st->misc0, st->misc1,
                        st->misc0 > 0 ? st->misc0 * 0.1f : 0.5f, st->misc1 > 0 ? st->misc1 * 0.1f : 1000.0f);
                });

                Launch(500, g, st, &St::level, 40.00f, 0.00f, "A, 40 yd on the level");
                Watch(500, 1900, g, st, &St::level);
                Launch(2400, g, st, &St::flat, 33.43f, -1.20f, "B, the capture's flat 33.43 yd leap");
                Watch(2400, 3700, g, st, &St::flat);
                Launch(4200, g, st, &St::down, 22.85f, -12.30f, "C, the capture's downhill 22.85 yd leap");
                Watch(4200, 5400, g, st, &St::down);

                At(5800, [this, st]()
                {
                    if (!st->dbcRan || st->speed <= 0.0f || st->misc0 < 0 || st->misc1 < 0)
                    {
                        Verdict(Invalid("spell 6544's Spell.dbc / SpellEffect.dbc rows did not read"));
                        return;
                    }
                    if (st->misc0 != 25 || st->misc1 != 100 || std::fabs(st->speed - 35.0f) > 0.01f)
                    {
                        char why[288];
                        snprintf(why, sizeof(why), "spell %u reads Speed %.2f, MiscValue %d, MiscValueB %d here; 4.3.4's rows are 35.00, 25 and 100, so the fixture is not the spell it claims",
                                 LEAP, st->speed, st->misc0, st->misc1);
                        Verdict(Invalid(why));
                        return;
                    }
                    char level[352], flat[352], down[384];
                    // Legs A and B run long enough (1 143 ms and 956 ms) that a 400 ms
                    // relocation lands within 80 ms of their apex; leg C, at 742 ms, peaks
                    // 216 ms in and is never relocated there, so its apex rests on the shape
                    // check and its own verdict reads the RISE.
                    Read(st->level, "A", true, level, sizeof(level));
                    Read(st->flat, "B", true, flat, sizeof(flat));
                    Read(st->down, "C", false, down, sizeof(down));
                    Verdict(std::string("levelLeapScalesWithItsLength=") + level +
                            " | flatLeapReachesTheDerivedApex=" + flat +
                            " | downhillLeapRisesBeforeItFalls=" + down);
                });
            }

        private:
            static std::string Invalid(char const* why)
            {
                std::string w = std::string("INVALID(") + why + ")";
                return "levelLeapScalesWithItsLength=" + w +
                       " | flatLeapReachesTheDerivedApex=" + w +
                       " | downhillLeapRisesBeforeItFalls=" + w;
            }

            /// Spell::EffectJump's last five lines, with a destination built from where the wolf
            /// actually stands so nothing here depends on the map being level.
            template <class St>
            void Launch(uint32 at, ObjectGuid g, std::shared_ptr<St> st, Leg St::* which,
                        float horizontal, float deltaZ, char const* what)
            {
                At(at, [this, g, st, which, horizontal, deltaZ, what, at]()
                {
                    Creature* c = Get(g); if (!c) { Log("ERR wolf gone"); return; }
                    Leg& leg = st.get()->*which;
                    const float x = c->Where().X() + horizontal;
                    const float y = c->Where().Y();
                    const float z = c->Where().Z() + deltaZ;
                    Load(x, y);
                    leg.ran = true;
                    leg.horizontal = horizontal;
                    leg.deltaZ = deltaZ;
                    leg.launchX = c->Where().X();
                    leg.launchY = c->Where().Y();
                    leg.launchZ = c->Where().Z();
                    const float minHeight = st->misc0 > 0 ? st->misc0 * 0.1f : 0.5f;
                    const float maxHeight = st->misc1 > 0 ? st->misc1 * 0.1f : 1000.0f;
                    const float length = std::sqrt(horizontal * horizontal + deltaZ * deltaZ);
                    const float duration = st->speed > 0.0f ? length / st->speed : 0.0f;
                    leg.apexWanted = Movement::JumpArc::ApexForFlight(minHeight, maxHeight, duration,
                                                                      float(Movement::gravity));
                    leg.amplitude = Movement::JumpArc::AmplitudeForLeg(minHeight, maxHeight, length,
                                                                       st->speed, deltaZ,
                                                                       float(Movement::gravity));
                    leg.apexOld = Movement::JumpArc::ApexOfAmplitude(2.5f, deltaZ);
                    leg.accepted = c->GetMotionMaster()->MoveJump(x, y, z, c->Where().Facing(),
                                                                  st->speed, leg.amplitude, NULL);
                    Log("%4ums leg %s: %.2f yd out, %.2f of fall, %.3f s at %.1f yd/s -> apex wanted %.3f, amplitude %.3f (the old 2.5 f would have risen %.3f), accepted=%d",
                        at, what, horizontal, -deltaZ, duration, st->speed, leg.apexWanted,
                        leg.amplitude, leg.apexOld, leg.accepted ? 1 : 0);
                });
            }

            /// The rendered path, sampled at the runner's own 100 ms cadence and checked AGAINST
            /// ITS OWN FRACTION OF THE LEG rather than against a clock.
            ///
            /// The server relocates a spline-moved unit once per POSITION_UPDATE_DELAY (400 ms,
            /// Unit.cpp:6974), so a 742 ms leap is only ever seen in three places and its apex --
            /// 216 ms in -- falls between two of them. Reading how high it got at a fixed moment
            /// would therefore measure the relocation cadence and not the arc. How far ALONG the
            /// leg it is, though, is exact at every sample, and the arc's height is a function of
            /// that alone (Movement::JumpArc::RiseAtFraction), so every sample is compared
            /// against the height the requested parabola has where the wolf actually stands. The
            /// worst disagreement over the leg is the reading that says whether the client will
            /// be drawing the arc that was asked for.
            template <class St>
            void Watch(uint32 from, uint32 to, ObjectGuid g, std::shared_ptr<St> st, Leg St::* which)
            {
                for (uint32 t = from + 100; t <= to; t += 100)
                {
                    At(t, [this, g, st, which, t, from]()
                    {
                        Creature* c = Get(g); if (!c) { return; }
                        Leg& leg = st.get()->*which;
                        if (!leg.ran || leg.horizontal <= 0.0f) { return; }
                        const float up = c->Where().Z() - leg.launchZ;
                        const float along = Dist2(c->Where().X(), c->Where().Y(), leg.launchX, leg.launchY);
                        const float u = along / leg.horizontal;
                        const float want = Movement::JumpArc::RiseAtFraction(leg.amplitude, leg.deltaZ, u);
                        const float err = std::fabs(up - want);
                        ++leg.samples;
                        if (u > 0.001f && u < 0.999f) { ++leg.midAir; }
                        if (up > leg.rise) { leg.rise = up; leg.bestFraction = u; }
                        if (err > leg.maxError) { leg.maxError = err; leg.worstAt = u; }
                        Log("+%4ums %.1f%% along: z %+.3f above the launch, the arc says %+.3f (err %.3f); best %+.3f",
                            t - from, u * 100.0f, up, want, err, leg.rise);
                    });
                }
            }

            /// One leg's verdict, in three readings that fail for three different reasons: the
            /// SHAPE (every sample against the arc at its own fraction of the leg), the RISE (the
            /// character left the ground at all), and -- only where a relocation lands near the
            /// top -- the APEX against the number the spell's own data asks for.
            void Read(Leg const& leg, char const* name, bool apexSampled, char* out, size_t size) const
            {
                if (!leg.ran)
                {
                    snprintf(out, size, "INVALID(leg %s never ran: the wolf went unresolvable)", name);
                    return;
                }
                if (!leg.accepted)
                {
                    snprintf(out, size, "INVALID(leg %s: MoveJump refused the arc, so nothing was rendered)", name);
                    return;
                }
                if (leg.samples < 8 || leg.midAir < 1)
                {
                    snprintf(out, size, "INVALID(leg %s: %u samples, %u of them mid-air)", name, leg.samples, leg.midAir);
                    return;
                }
                if (leg.maxError > 0.05f)
                {
                    snprintf(out, size, "BUG(leg %s: the rendered path is not the arc that was asked for -- %.3f yd off it at %.0f%% along, with amplitude %.3f and deltaZ %.2f)",
                             name, leg.maxError, leg.worstAt * 100.0f, leg.amplitude, leg.deltaZ);
                    return;
                }
                if (leg.rise <= 0.05f)
                {
                    snprintf(out, size, "BUG(leg %s: %.2f yd out and %.2f down, the character NEVER ROSE -- greatest height above the launch %+.3f yd, where the spell's own data asks for %.3f; amplitude %.3f was sent)",
                             name, leg.horizontal, -leg.deltaZ, leg.rise, leg.apexWanted, leg.amplitude);
                    return;
                }
                if (apexSampled && std::fabs(leg.rise - leg.apexWanted) > 0.15f)
                {
                    snprintf(out, size, "BUG(leg %s: the highest relocation put him %.3f yd above the launch, not the %.3f the spell's own pair asks for over this flight; amplitude %.3f, deltaZ %.2f)",
                             name, leg.rise, leg.apexWanted, leg.amplitude, leg.deltaZ);
                    return;
                }
                if (apexSampled)
                {
                    snprintf(out, size, "OK(%.2f yd out and %.2f down: the rendered path held to the requested arc within %.3f yd over all %u samples, and the highest relocation -- %.0f%% along -- put him %.3f yd above the launch against the %.3f the spell's own SpellEffect.dbc pair asks for. The fixed 2.5 f would have risen %.3f)",
                             leg.horizontal, -leg.deltaZ, leg.maxError, leg.samples,
                             leg.bestFraction * 100.0f, leg.rise, leg.apexWanted, leg.apexOld);
                    return;
                }
                snprintf(out, size, "OK(%.2f yd out and %.2f down: he ROSE %.3f yd above the launch %.0f%% along, where the fixed 2.5 f rose %.3f -- nothing at all. The rendered path held to the requested arc within %.3f yd over all %u samples, so the %.3f yd apex it reaches between two 400 ms relocations is the arc's own)",
                         leg.horizontal, -leg.deltaZ, leg.rise, leg.bestFraction * 100.0f,
                         leg.apexOld, leg.maxError, leg.samples, leg.apexWanted);
            }
        };
    }

    void RegisterJumpScenarios(Runner& r)
    {
        r.Register(new JumpOverPoint());
        r.Register(new JumpOverChase());
        r.Register(new StunMidJump());
        r.Register(new BackToBackJumps());
        // Order 73, behind the smooth family's 72 and clear of the 900 player block: the arc a
        // jump spell asks for, against the arc the client will draw (live test 2026-09-22, B4).
        r.Register(new HeroicLeapArc());
    }
}
