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

// The wander family of the old harness: S17 slope-wanderer (a regression
// guard, not red/green). Coordinates are the old file's Mulgore points.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;

        struct Pt { float x, y, z; };
        const Pt A4 = { -3257.5f, -351.6f, 47.8f };

        /// S17: a wanderer that ignores pathfinding on a slope must keep
        /// wandering (not stall) and its hops must track the terrain height.
        /// |dz| is reported, not gated: Eluna's GetHeight is terrain-only, so a
        /// wolf on a rock reads as "floating".
        class SlopeWanderer : public Scenario
        {
        public:
            SlopeWanderer() : Scenario("slope-wanderer", 14) {}

            void Prepare() override
            {
                struct State { uint32 hops; float maxdz; bool haveLast; float lastX, lastY; };
                Creature* a = Spawn(WOLF, A4.x, A4.y, Ground(A4.x, A4.y, A4.z), 0.0f);
                if (!a) { Verdict("slopeWanderer=INVALID(spawn failed)"); return; }
                const ObjectGuid g = a->GetObjectGuid();
                auto st = std::make_shared<State>();
                st->hops = 0; st->maxdz = 0.0f; st->haveLast = false; st->lastX = 0.0f; st->lastY = 0.0f;
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->addUnitState(UNIT_STAT_IGNORE_PATHFINDING);
                    a->GetMotionMaster()->MoveRandomAroundPoint(a->Where().X(), a->Where().Y(), a->Where().Z(), 15.0f);
                    Log("wander radius 15 with IGNORE_PATHFINDING on the A4 slope, mt=%s", TypeName(a));
                });
                for (uint32 i = 1; i <= 60; ++i)
                {
                    At(500 + i * 500, [this, g, st, i]()
                    {
                        Creature* a = Get(g); if (!a) { return; }
                        const float x = a->Where().X(), y = a->Where().Y(), z = a->Where().Z();
                        const float dz = z - Ground(x, y, z);
                        const float adz = dz < 0.0f ? -dz : dz;
                        if (st->haveLast && Dist2(x, y, st->lastX, st->lastY) > 0.3f) { ++st->hops; }
                        if (st->hops > 0 && adz > st->maxdz) { st->maxdz = adz; }
                        st->lastX = x; st->lastY = y; st->haveLast = true;
                        if (i % 10 == 0)
                        {
                            Log("+%2us at %.1f %.1f z=%.1f dz=%+.2f moves=%u mt=%s", i / 2, x, y, z, dz, st->hops, TypeName(a));
                        }
                    });
                }
                At(500 + 61 * 500, [this, st]()
                {
                    std::string v;
                    if (st->hops >= 2)
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "OK(kept wandering, %u moves, max |z - terrain| %.2f yd)", st->hops, st->maxdz);
                        v = text;
                    }
                    else
                    {
                        char text[96];
                        snprintf(text, sizeof(text), "BUG(stalled: %u moves in 30 s, max |z - terrain| %.2f yd)", st->hops, st->maxdz);
                        v = text;
                    }
                    Verdict("slopeWanderer=" + v);
                });
            }
        };
    }

    void RegisterWanderScenarios(Runner& r)
    {
        r.Register(new SlopeWanderer());
    }
}
