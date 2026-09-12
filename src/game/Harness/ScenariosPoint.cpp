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

// The point family of the old harness: S5 point-inform-after-interrupt, S9
// long-point, S15 unreachable-point. Coordinates are the old file's Mulgore
// points.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;

        struct Pt { float x, y, z; };
        const Pt SD = { -3122.6f, -261.3f, 46.0f };
        const Pt A4 = { -3257.5f, -351.6f, 47.8f };
        const Pt B4 = { -2891.0f, -416.7f, 47.9f };

        /// S5: a POINT leg interrupted by a real chase and then cleared must not
        /// spuriously inform for the point it never reached (B4).
        class PointInformAfterInterrupt : public Scenario
        {
        public:
            PointInformAfterInterrupt() : Scenario("point-inform-after-interrupt", 4) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SD.x, SD.y, SD.z, 0.0f);
                Creature* b = Spawn(KOBOLD, SD.x + 8.0f, SD.y, Ground(SD.x + 8.0f, SD.y, SD.z), 3.1f);
                if (!a || !b) { Verdict("B4=INVALID(spawn failed)"); return; }
                const size_t mark = Informs().size();
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid h = b->GetObjectGuid();
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MovePoint(55, SD.x - 40.0f, SD.y, Ground(SD.x - 40.0f, SD.y, SD.z), true);
                    Log("MoveTo(55) 40 yd west, mt=%s", TypeName(a));
                });
                At(2500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { return; }
                    const float x = a->Where().X(), y = a->Where().Y();
                    a->Attack(b, true);   // real chase (victim set): interrupts the leg, arrives 8 yd on, stops
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);
                    Log("MoveChase over the leg at %.1f %.1f (%.1f yd short of point 55) mt=%s", x, y, Dist2(x, y, SD.x - 40.0f, SD.y), TypeName(a));
                });
                At(6500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { return; }
                    const float x = a->Where().X(), y = a->Where().Y();
                    const float bx = b->Where().X(), by = b->Where().Y();
                    a->GetMotionMaster()->Clear(false);   // pops chase + point with the spline finalized (chase arrived): Point::Finalize
                    Log("MoveClear at %.1f %.1f (%.1f yd short of point 55, %.1f yd from kobold) mt=%s", x, y, Dist2(x, y, SD.x - 40.0f, SD.y), Dist2(x, y, bx, by), TypeName(a));
                });
                At(8000, [this, mark]()
                {
                    bool spurious = false;
                    float shortBy = 0.0f;
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.type == POINT_MOTION_TYPE && r.id == 55)
                        {
                            spurious = true;
                            shortBy = Dist2(r.x, r.y, SD.x - 40.0f, SD.y);
                        }
                    }
                    std::string b4;
                    if (spurious)
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "BUG(POINT 55 inform fired %.1f yd short of the point)", shortBy);
                        b4 = text;
                    }
                    else
                    {
                        b4 = "OK(no inform for the cut leg)";
                    }
                    Verdict("B4=" + b4);
                });
            }
        };

        /// S9: a 372-yard MovePoint must actually reach its real point and inform
        /// there, not truncate the leg and read a partial route as arrival.
        class LongPoint : public Scenario
        {
        public:
            LongPoint() : Scenario("long-point", 8) {}

            void Prepare() override
            {
                struct Sample { uint32 t; float d; MovementGeneratorType mt; };
                Creature* a = Spawn(WOLF, A4.x, A4.y, A4.z, 0.0f);
                if (!a) { Verdict("longMovePoint=INVALID(spawn failed)"); return; }
                Load(B4.x, B4.y);
                Load((A4.x + B4.x) / 2.0f, (A4.y + B4.y) / 2.0f);
                const size_t mark = Informs().size();
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                auto samples = std::make_shared<std::vector<Sample> >();
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MovePoint(88, B4.x, B4.y, B4.z, true);
                    Log("MoveTo(88) %.0f yd away, mt=%s", Dist2(A4.x, A4.y, B4.x, B4.y), TypeName(a));
                });
                for (uint32 i = 1; i <= 20; ++i)
                {
                    At(500 + i * 5000, [this, g, samples, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Sample s;
                        s.t = i * 5;
                        s.d = Dist2(a->Where().X(), a->Where().Y(), B4.x, B4.y);
                        s.mt = Type(a);
                        samples->push_back(s);
                        Log("+%3us dist=%.1f mt=%s", s.t, s.d, Harness::TypeName(s.mt));
                    });
                }
                At(500 + 21 * 5000, [this, low, samples, mark]()
                {
                    bool haveInform = false;
                    Inform lastInform = Inform();
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.type == POINT_MOTION_TYPE && r.id == 88)
                        {
                            lastInform = r;
                            haveInform = true;
                        }
                    }
                    Sample const* f = samples->empty() ? NULL : &samples->back();
                    std::string v;
                    if (haveInform)
                    {
                        const float shortBy = Dist2(lastInform.x, lastInform.y, B4.x, B4.y);
                        char text[128];
                        if (shortBy < 6.0f)
                        {
                            snprintf(text, sizeof(text), "OK(POINT 88 informed %.1f yd from the point)", shortBy);
                        }
                        else
                        {
                            snprintf(text, sizeof(text), "BUG(POINT 88 informed %.0f yd SHORT of the point - truncated leg read as arrival)", shortBy);
                        }
                        v = text;
                    }
                    else if (f && f->d < 6.0f)
                    {
                        v = "PARTIAL(arrived but no inform)";
                    }
                    else
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "BUG(no inform, still %.0f yd away)", f ? f->d : -1.0f);
                        v = text;
                    }
                    Verdict("longMovePoint=" + v);
                });
            }
        };

        /// S15: a point just above the mesh, out of the router's reach: seven
        /// probes sweeping the offset window must each still inform.
        class UnreachablePoint : public Scenario
        {
        public:
            UnreachablePoint() : Scenario("unreachable-point", 13) {}

            void Prepare() override
            {
                static const float OFFS[7] = { 6.95f, 7.05f, 7.15f, 7.25f, 7.35f, 7.45f, 7.55f };
                struct Probe
                {
                    uint32 id; float off; float gx; float d; std::string mt; bool informed; size_t mark;
                };
                Creature* a = Spawn(WOLF, A4.x, A4.y, A4.z, 0.0f);
                if (!a) { Verdict("unreachablePoint=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                const uint32 low = a->GetGUIDLow();
                auto probes = std::make_shared<std::vector<Probe> >();
                for (uint32 k = 1; k <= 7; ++k)
                {
                    Probe pr;
                    pr.id = 90 + k;
                    pr.off = OFFS[k - 1];
                    pr.gx = A4.x + ((k % 2 == 1) ? 30.0f : -30.0f);
                    pr.d = -1.0f;
                    pr.mt = "?";
                    pr.informed = false;
                    pr.mark = 0;
                    probes->push_back(pr);
                }
                for (uint32 k = 1; k <= 7; ++k)
                {
                    At(500 + (k - 1) * 12000, [this, g, probes, k]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Probe& pr = (*probes)[k - 1];
                        pr.mark = Informs().size();
                        const float gz = Ground(pr.gx, A4.y, A4.z) + pr.off;
                        a->GetMotionMaster()->MovePoint(pr.id, pr.gx, A4.y, gz, true);
                        Log("probe %u: MoveTo(%u) 60 yd away, %.2f yd above the ground (z=%.1f), mt=%s", k, pr.id, pr.off, gz, TypeName(a));
                    });
                    At(500 + (k - 1) * 12000 + 11000, [this, g, low, probes, k]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        Probe& pr = (*probes)[k - 1];
                        for (size_t idx = pr.mark; idx < Informs().size(); ++idx)
                        {
                            Inform const& r = Informs()[idx];
                            if (r.guidLow == low && r.type == POINT_MOTION_TYPE && r.id == pr.id) { pr.informed = true; }
                        }
                        pr.d = Dist2(a->Where().X(), a->Where().Y(), pr.gx, A4.y);
                        pr.mt = TypeName(a);
                        Log("probe %u +11s: dist=%.1f mt=%s inform=%s", k, pr.d, pr.mt.c_str(), pr.informed ? "yes" : "NO");
                    });
                }
                At(500 + 7 * 12000, [this, probes]()
                {
                    std::vector<std::string> bad;
                    for (size_t k = 0; k < probes->size(); ++k)
                    {
                        Probe const& pr = (*probes)[k];
                        if (!pr.informed)
                        {
                            char text[64];
                            snprintf(text, sizeof(text), "%.2f yd (dist %.1f, mt=%s)", pr.off, pr.d, pr.mt.c_str());
                            bad.push_back(text);
                        }
                    }
                    std::string body;
                    if (bad.empty())
                    {
                        char text[48];
                        snprintf(text, sizeof(text), "OK(all %u probes informed)", uint32(probes->size()));
                        body = text;
                    }
                    else
                    {
                        std::string joined;
                        for (size_t k = 0; k < bad.size(); ++k)
                        {
                            if (k) { joined += "; "; }
                            joined += bad[k];
                        }
                        char text[64];
                        snprintf(text, sizeof(text), "BUG(%u of %u probes never informed - zero-length re-lay loop at: ", uint32(bad.size()), uint32(probes->size()));
                        body = std::string(text) + joined + ")";
                    }
                    Verdict("unreachablePoint=" + body);
                });
            }
        };
    }

    void RegisterPointScenarios(Runner& r)
    {
        r.Register(new PointInformAfterInterrupt());
        r.Register(new LongPoint());
        r.Register(new UnreachablePoint());
    }
}
