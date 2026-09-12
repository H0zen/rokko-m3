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

// The flee family of the old harness: S6 flee-drifts-back. Coordinates are
// the old file's Mulgore points.
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
    }

    void RegisterFleeScenarios(Runner& r)
    {
        r.Register(new FleeDriftsBack());
    }
}
