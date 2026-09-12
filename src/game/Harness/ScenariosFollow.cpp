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

// The follow family of the old harness: S13 follow-survives-knockback, S4
// long-follow. Coordinates are the old file's Mulgore points.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;
        const uint32 STRIDER = 2955;

        struct Pt { float x, y, z; };
        const Pt SA = { -3257.5f, -351.6f, 47.8f };
        const Pt A4 = { -3257.5f, -351.6f, 47.8f };
        const Pt B4 = { -2891.0f, -416.7f, 47.9f };
        const Pt A5 = { -3122.6f, -261.3f, 46.0f };   // = P0
        const Pt B5 = { -3045.2f, -437.2f, 46.1f };   // = SC

        /// S13: a knock-back (MoveJump) landing mid-follow must not break the
        /// follow: it must still be following and closing in afterwards.
        class FollowSurvivesKnockback : public Scenario
        {
        public:
            FollowSurvivesKnockback() : Scenario("follow-survives-knockback", 12) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float d; MovementGeneratorType mt; };
                const float fx = SA.x + 25.0f, fy = SA.y;
                Creature* a = Spawn(WOLF, SA.x, SA.y, SA.z, 0.0f);
                Creature* b = Spawn(KOBOLD, fx, fy, Ground(fx, fy, SA.z), 3.1f);
                if (!a || !b) { Verdict("followKnockback=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid h = b->GetObjectGuid();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { return; }
                    a->GetMotionMaster()->MoveFollow(b, 1.0f, 0.0f);
                    Log("MoveFollow, mt=%s", TypeName(a));
                });
                At(2000, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    const float x = a->Where().X(), y = a->Where().Y(), z = a->Where().Z();
                    a->GetMotionMaster()->MoveJump(x, y + 8.0f, Ground(x, y + 8.0f, z), 7.5f, 4.0f, 73);
                    Log("MoveJump 8 yd sideways over the follow, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 10; ++i)
                {
                    At(2000 + i * 1000, [this, g, h, samples, i]()
                    {
                        Creature* a = Get(g); Creature* b = Get(h);
                        if (!a || !b) { return; }
                        Sample s;
                        s.t = i;
                        const float x = a->Where().X(), y = a->Where().Y();
                        const float bx = b->Where().X(), by = b->Where().Y();
                        s.d = Dist2(x, y, bx, by);
                        s.mt = Type(a);
                        samples->push_back(s);
                        Log("+%2us dTarget=%.1f mt=%s", s.t, s.d, Harness::TypeName(s.mt));
                    });
                }
                At(12500, [this, samples]()
                {
                    if (samples->empty()) { Verdict("INVALID"); return; }
                    Sample const& f = samples->back();
                    Sample const& first = (*samples)[0];
                    std::string v;
                    if (f.mt == FOLLOW_MOTION_TYPE && f.d < first.d - 6.0f)
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "OK(follow survived the effect and closed in %.0f -> %.0f yd)", first.d, f.d);
                        v = text;
                    }
                    else
                    {
                        char text[128];
                        snprintf(text, sizeof(text), "BUG(follow lost or stalled: mt=%s, %.1f -> %.1f yd from target)", Harness::TypeName(f.mt), first.d, f.d);
                        v = text;
                    }
                    Verdict("followKnockback=" + v);
                });
            }
        };

        /// S4: a 372-yard MoveFollow across open Mulgore must actually close the
        /// gap (B8), proven against a 192-yard control pair over the same window.
        class LongFollow : public Scenario
        {
        public:
            LongFollow() : Scenario("long-follow", 16) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float d4, m4, d5, m5; MovementGeneratorType mt4, mt5; };
                const float mx = (A4.x + B4.x) / 2.0f, my = (A4.y + B4.y) / 2.0f;
                Load(A4.x, A4.y);
                Load(B4.x, B4.y);
                Load(mx, my);
                Load(A5.x, A5.y);
                Load(B5.x, B5.y);
                Creature* a4 = Spawn(STRIDER, A4.x, A4.y, A4.z, 0.0f);
                Creature* b4 = Spawn(STRIDER, B4.x, B4.y, B4.z, 0.0f);
                Spawn(STRIDER, mx, my, Ground(mx, my, 47.8f), 0.0f);   // l4: grid loader on the mid-point, never queried again
                Creature* a5 = Spawn(STRIDER, A5.x, A5.y, A5.z, 0.0f);
                Creature* b5 = Spawn(STRIDER, B5.x, B5.y, B5.z, 0.0f);
                if (!a4 || !b4 || !a5 || !b5) { Verdict("B8=INVALID(spawn failed)"); return; }
                const ObjectGuid gA4 = a4->GetObjectGuid();
                const ObjectGuid gB4 = b4->GetObjectGuid();
                const ObjectGuid gA5 = a5->GetObjectGuid();
                const ObjectGuid gB5 = b5->GetObjectGuid();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, gA4, gB4, gA5, gB5]()
                {
                    Creature* a4 = Get(gA4); Creature* b4 = Get(gB4); Creature* a5 = Get(gA5); Creature* b5 = Get(gB5);
                    if (!a4 || !b4 || !a5 || !b5) { Log("ERR actors gone"); return; }
                    a4->GetMotionMaster()->MoveFollow(b4, 1.0f, 0.0f);
                    a5->GetMotionMaster()->MoveFollow(b5, 1.0f, 0.0f);
                    Log("MoveFollow: long=%.0f yd mt=%s | control=%.0f yd mt=%s",
                        Dist2(A4.x, A4.y, B4.x, B4.y), TypeName(a4), Dist2(A5.x, A5.y, B5.x, B5.y), TypeName(a5));
                });
                for (uint32 i = 1; i <= 80; ++i)
                {
                    At(500 + i * 3000, [this, gA4, gB4, gA5, gB5, samples, i]()
                    {
                        Creature* a4 = Get(gA4); Creature* b4 = Get(gB4); Creature* a5 = Get(gA5); Creature* b5 = Get(gB5);
                        if (!a4 || !b4 || !a5 || !b5) { Log("ERR actors gone"); return; }
                        Sample s;
                        s.t = i * 3;
                        const float x = a4->Where().X(), y = a4->Where().Y();
                        const float bx = b4->Where().X(), by = b4->Where().Y();
                        const float cx = a5->Where().X(), cy = a5->Where().Y();
                        const float dx = b5->Where().X(), dy = b5->Where().Y();
                        s.d4 = Dist2(x, y, bx, by); s.m4 = Dist2(x, y, A4.x, A4.y);
                        s.d5 = Dist2(cx, cy, dx, dy); s.m5 = Dist2(cx, cy, A5.x, A5.y);
                        s.mt4 = Type(a4); s.mt5 = Type(a5);
                        samples->push_back(s);
                        Log("+%3us long: dist=%.1f moved=%.1f mt=%s | control: dist=%.1f moved=%.1f mt=%s",
                            s.t, s.d4, s.m4, Harness::TypeName(s.mt4), s.d5, s.m5, Harness::TypeName(s.mt5));
                    });
                }
                At(500 + 81 * 3000, [this, samples]()
                {
                    if (samples->empty()) { Verdict("INVALID(no samples)"); return; }
                    Sample const& f = samples->back();
                    std::string b8;
                    if (f.d4 < 10.0f)
                    {
                        b8 = "OK(372 yd follow arrived)";
                    }
                    else if (f.m4 < 5.0f)
                    {
                        b8 = "BUG(follower never moved: NOPATH)";
                    }
                    else if (f.m4 > 200.0f)
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "PROGRESSING(moved %.0f yd, %.0f yd left - window too short)", f.m4, f.d4);
                        b8 = text;
                    }
                    else
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "PARTIAL(moved %.0f yd, %.0f yd left)", f.m4, f.d4);
                        b8 = text;
                    }
                    std::string ctl;
                    if (f.d5 < 10.0f)
                    {
                        ctl = "OK";
                    }
                    else
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "FAILED(moved %.0f, %.0f left) - control invalid", f.m5, f.d5);
                        ctl = text;
                    }
                    Verdict("B8=" + b8 + " | control(192yd)=" + ctl);
                });
            }
        };
    }

    void RegisterFollowScenarios(Runner& r)
    {
        r.Register(new FollowSurvivesKnockback());
        r.Register(new LongFollow());
    }
}
