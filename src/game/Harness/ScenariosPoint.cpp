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
#include "Vehicle.h"
#include "Log.h"
#include "movement/MoveSpline.h"
#include "Utilities/MathDefines.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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

        /// The shorter way round between two facings, in radians.
        float AngleDiff(float a, float b)
        {
            float d = fabsf(a - b);
            while (d > M_PI_F) { d = fabsf(d - 2.0f * M_PI_F); }
            return d;
        }
        const Pt SD = { -3122.6f, -261.3f, 46.0f };
        const Pt A4 = { -3257.5f, -351.6f, 47.8f };
        const Pt B4 = { -2891.0f, -416.7f, 47.9f };

        /// S62 (the live test of 2026-09-20): boarding a vehicle, which nobody could do. Clicking
        /// a vehicle sends CMSG_SPELLCLICK and the server casts whatever `npc_spellclick_spells`
        /// names -- and for most vehicles that is a DUMMY meant for a script this core does not
        /// carry (the Darnassus Love Boat's 69341 is a periodic dummy, the Burning Debris' 73677 a
        /// plain dummy), so the click did nothing at all. The seating itself is done by a spell
        /// carrying SPELL_AURA_CONTROL_VEHICLE, which HandleAuraControlVehicle turns into a Board;
        /// retail's own is 46598, and HandleSpellClick now falls back to it when the table seated
        /// nobody and the hull has a seat a player may enter and leave.
        ///
        /// This runs that seating on a real world vehicle: the boat is spawned from its own entry,
        /// so its seats come from Vehicle.dbc exactly as they do live.
        class VehicleBoarding : public Scenario
        {
        public:
            VehicleBoarding() : Scenario("vehicle-boarding", 62) {}

            void Prepare() override
            {
                const uint32 LOVE_BOAT = 37980;
                const uint32 RIDE = 46598;   // SPELL_RIDE_VEHICLE_HARDCODED
                Creature* boat = Spawn(LOVE_BOAT, SD.x + 20.0f, SD.y, Ground(SD.x + 20.0f, SD.y, SD.z), 0.0f);
                Creature* rider = Spawn(WOLF, SD.x + 16.0f, SD.y, Ground(SD.x + 16.0f, SD.y, SD.z), 0.0f);
                if (!boat || !rider) { Verdict("isVehicle=INVALID(spawn failed) | boards=INVALID(spawn failed) | ridesTheHull=INVALID(spawn failed) | leaves=INVALID(spawn failed)"); return; }
                Silence(boat);
                Silence(rider);
                const ObjectGuid b = boat->GetObjectGuid();
                const ObjectGuid g = rider->GetObjectGuid();

                struct St { bool isVehicle, aboard, onThisHull, left; };
                auto st = std::make_shared<St>();
                st->isVehicle = st->aboard = st->onThisHull = st->left = false;

                At(500, [this, g, b, st, RIDE]()
                {
                    Creature* boat = Get(b); Creature* rider = Get(g);
                    if (!boat || !rider) { return; }
                    st->isVehicle = boat->IsVehicle();
                    rider->CastSpell(boat, RIDE, true);
                    Log("cast %u at the boat (IsVehicle=%d)", RIDE, st->isVehicle ? 1 : 0);
                });
                At(1500, [this, g, b, st]()
                {
                    Creature* rider = Get(g);
                    if (!rider) { return; }
                    TransportInfo* ti = rider->GetTransportInfo();
                    st->aboard = ti != NULL;
                    st->onThisHull = ti && ti->GetTransportGuid() == b;
                    Log("aboard=%d, on this hull=%d", st->aboard ? 1 : 0, st->onThisHull ? 1 : 0);
                });
                At(2000, [this, g, RIDE]()
                {
                    if (Creature* rider = Get(g)) { rider->RemoveAurasDueToSpell(RIDE); Log("aura %u removed", RIDE); }
                });
                At(3000, [this, g, st]()
                {
                    Creature* rider = Get(g);
                    if (!rider) { return; }
                    st->left = rider->GetTransportInfo() == NULL;
                    Log("after the aura went: aboard=%d", st->left ? 0 : 1);
                });
                At(3500, [this, st]()
                {
                    Verdict(std::string("isVehicle=") + (st->isVehicle ? "OK" : "BUG(the boat is not a vehicle)") +
                            " | boards=" + (st->aboard ? "OK" : "BUG(the ride aura seated nobody)") +
                            " | ridesTheHull=" + (st->onThisHull ? "OK" : "BUG(aboard something else)") +
                            " | leaves=" + (st->left ? "OK" : "BUG(still aboard after the aura went)"));
                });
            }
        };

        /// S5: a POINT leg interrupted by a real chase and then cleared must not
        /// spuriously inform for the point it never reached (B4). roamingBits and
        /// projection (task 5) read the shell's own bookkeeping: the roaming leg latch
        /// is set while the point leg runs (mid-leg, before the Attack+MoveChase at
        /// 2.5 s), and Type() reads POINT while the point is the selection. The
        /// Attack+MoveChase at 2.5 s does not in fact win the claim against a Point
        /// already laying a leg here (Type() stays POINT straight through to the
        /// explicit Clear() at 6.5 s -- the log's own "5.9 yd short... 42.2 yd from
        /// kobold" already said so, unchanged from the record), so "after the point
        /// finished" is read where the point actually does relinquish control: the
        /// same world tick as Clear(), before the reselected default (Random, which
        /// reuses the same latch for its own wander) has had a tick to run -- confirmed
        /// empirically: the roaming leg latch reads 0 in that same tick and 1 by the next one.
        /// Type() itself flips to RANDOM synchronously within Clear(), one tick ahead
        /// of the roaming leg latch, so it cannot serve as this category's own "after" read.
        class PointInformAfterInterrupt : public Scenario
        {
        public:
            PointInformAfterInterrupt() : Scenario("point-inform-after-interrupt", 4) {}

            void Prepare() override
            {
                Creature* a = Spawn(WOLF, SD.x, SD.y, SD.z, 0.0f);
                Creature* b = Spawn(KOBOLD, SD.x + 8.0f, SD.y, Ground(SD.x + 8.0f, SD.y, SD.z), 3.1f);
                if (!a || !b) { Verdict("B4=INVALID(spawn failed) | roamingBits=INVALID(spawn failed) | projection=INVALID(spawn failed)"); return; }
                const size_t mark = Informs().size();
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid h = b->GetObjectGuid();
                auto roamingAtPoint = std::make_shared<bool>(false);
                auto projectionAtPoint = std::make_shared<bool>(false);
                auto roamingAfterClear = std::make_shared<bool>(false);
                auto sampledBefore = std::make_shared<bool>(false);
                auto sampledAfter = std::make_shared<bool>(false);
                At(500, [this, g]()
                {
                    Creature* a = Get(g); if (!a) { return; }
                    a->GetMotionMaster()->MovePoint(55, SD.x - 40.0f, SD.y, Ground(SD.x - 40.0f, SD.y, SD.z), true);
                    Log("MoveTo(55) 40 yd west, mt=%s", TypeName(a));
                });
                At(1500, [this, g, roamingAtPoint, projectionAtPoint, sampledBefore]()
                {
                    // Silent (no Log line added): the record's log for this scenario stays
                    // byte-identical, and the verdict body itself carries the evidence.
                    Creature* a = Get(g); if (!a) { return; }
                    *roamingAtPoint = a->GetMotionMaster()->Latches().roamingLeg;
                    *projectionAtPoint = Type(a) == Motion::Kind::Point;
                    *sampledBefore = true;
                });
                At(2500, [this, g, h]()
                {
                    Creature* a = Get(g); Creature* b = Get(h);
                    if (!a || !b) { return; }
                    const float x = a->Where().X(), y = a->Where().Y();
                    a->Attack(b, true);   // a real chase request (victim set), against a still-running point leg:
                    a->GetMotionMaster()->MoveChase(b, 0.0f, 0.0f);   // does not win the claim here (task 5's roamingBits/projection confirm Type() stays POINT to the Clear() below) -- B4 covers the cut leg regardless
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
                At(6500, [this, g, roamingAfterClear, sampledAfter]()   // same world tick, right after Clear() above (Timeline.cpp: same-`at` steps run in registration order within one Advance) -- before Random's own Activate (next tick) can reclaim the bit; silent, same reason as the mid-leg sample
                {
                    Creature* a = Get(g); if (!a) { return; }
                    *roamingAfterClear = a->GetMotionMaster()->Latches().roamingLeg;
                    *sampledAfter = true;
                });
                At(8000, [this, mark, roamingAtPoint, projectionAtPoint, roamingAfterClear, sampledBefore, sampledAfter]()
                {
                    bool spurious = false;
                    float shortBy = 0.0f;
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.kind == Motion::Kind::Point && r.id == 55)
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
                    std::string roamingBits;
                    std::string projection;
                    if (!*sampledBefore || !*sampledAfter)
                    {
                        roamingBits = "INVALID(no samples)";
                        projection = "INVALID(no samples)";
                    }
                    else
                    {
                        const bool okRoam = *roamingAtPoint && !*roamingAfterClear;
                        char rtext[130];
                        snprintf(rtext, sizeof(rtext), "%s(midLeg=%d afterClear=%d)", okRoam ? "OK" : "BUG", *roamingAtPoint ? 1 : 0, *roamingAfterClear ? 1 : 0);
                        roamingBits = rtext;
                        projection = *projectionAtPoint ? "OK(POINT while selected)" : "BUG(not POINT while selected)";
                    }
                    Verdict("B4=" + b4 + " | roamingBits=" + roamingBits + " | projection=" + projection);
                });
            }
        };

        /// S9: a 372-yard MovePoint must actually reach its real point and inform
        /// there, not truncate the leg and read a partial route as arrival.
        /// roamingBits and projection (task 5) read the shell's own bookkeeping: the
        /// "while running" half comes from the same 5 s samples the distance check
        /// already takes (mt and the roaming bit, side by side); the "after" half
        /// cannot wait for a later coarse sample -- this wolf's own default motion
        /// (creature_template.MovementType 1, Random) reclaims the roaming leg latch
        /// for its own wander the moment it is reselected, so a sample taken seconds
        /// later would read true again for an unrelated reason. OnInform (Scenario.h:105)
        /// fires synchronously from inside NativeBehaviour::PerformOutcome, which clears
        /// the roaming pair (MotionMaster::WriteRoaming(outcome.roaming)) before it walks the effects that
        /// deliver the inform -- so reading the bit from inside the POINT 88 callback
        /// catches the point's own release before any later reselect can touch it.
        class LongPoint : public Scenario
        {
        public:
            LongPoint() : Scenario("long-point", 8) {}

            void Prepare() override
            {
                m_informedAt88 = false;
                m_roamingAtInform = false;
                struct Sample { uint32 t; float d; Motion::Kind mt; bool roaming; };
                Creature* a = Spawn(WOLF, A4.x, A4.y, A4.z, 0.0f);
                if (!a) { Verdict("longMovePoint=INVALID(spawn failed) | roamingBits=INVALID(spawn failed) | projection=INVALID(spawn failed)"); return; }
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
                        s.roaming = a->GetMotionMaster()->Latches().roamingLeg;   // captured silently: the record's log line for this scenario stays byte-identical
                        samples->push_back(s);
                        Log("+%3us dist=%.1f mt=%s", s.t, s.d, Motion::KindName(s.mt));
                    });
                }
                At(500 + 21 * 5000, [this, low, samples, mark]()
                {
                    bool haveInform = false;
                    Inform lastInform = Inform();
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.kind == Motion::Kind::Point && r.id == 88)
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
                    bool foundPoint = false;
                    bool roamingWhilePoint = false;
                    for (size_t k = 0; k < samples->size(); ++k)
                    {
                        Sample const& s = (*samples)[k];
                        if (s.mt == Motion::Kind::Point)
                        {
                            foundPoint = true;
                            if (s.roaming) { roamingWhilePoint = true; }
                        }
                    }
                    std::string roamingBits;
                    std::string projection;
                    if (!foundPoint)
                    {
                        roamingBits = "INVALID(no POINT sample)";
                        projection = "INVALID(no POINT sample)";
                    }
                    else
                    {
                        projection = "OK(POINT while selected)";
                        if (!m_informedAt88)
                        {
                            roamingBits = "INVALID(never informed)";
                        }
                        else
                        {
                            const bool ok = roamingWhilePoint && !m_roamingAtInform;
                            char text[140];
                            snprintf(text, sizeof(text), "%s(whileRunning=%d atFinish=%d)", ok ? "OK" : "BUG", roamingWhilePoint ? 1 : 0, m_roamingAtInform ? 1 : 0);
                            roamingBits = text;
                        }
                    }
                    Verdict("longMovePoint=" + v + " | roamingBits=" + roamingBits + " | projection=" + projection);
                });
            }

            /// The recording hook (Scenario.h:105), fired synchronously from inside
            /// NativeBehaviour::PerformOutcome for the POINT 88 inform: MotionMaster::WriteRoaming(outcome.roaming)
            /// (PerformOutcome, before the effects loop) has already cleared the roaming pair
            /// by the time this runs, so this is the earliest possible read of the point's own
            /// release, before the wolf's default Random re-claims the same bit for its own
            /// wander leg.
            void OnInform(Creature* creature, Motion::Kind kind, uint32 id) override
            {
                if (kind != Motion::Kind::Point || id != 88 || m_informedAt88) { return; }
                m_roamingAtInform = creature->GetMotionMaster()->Latches().roamingLeg;
                m_informedAt88 = true;
            }

        private:
            bool m_informedAt88;
            bool m_roamingAtInform;
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
                            if (r.guidLow == low && r.kind == Motion::Kind::Point && r.id == pr.id) { pr.informed = true; }
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

        /// S61 (the live test of 2026-09-20): Distract is cast at a SPOT on the ground (target
        /// mask 0x40, implicit target 16: every enemy within the 10 yd circle), and each creature
        /// it catches must turn to face the circle's centre and stand there for its duration.
        /// Live, the creature stood but kept looking the way it had been walking.
        ///
        /// The cast must be the real one: EffectDistract reads its own m_targets destination, so
        /// a scenario that merely calls SetFacingTo + MoveDistract by hand passes either way and
        /// proves nothing. It must also catch the creature MID-LEG, which is the other half of
        /// the bug: installing the distract suspends the leg that ran, and a suspended behaviour
        /// stops the mover (NativeBehaviour::PerformOutcome). StopMoving freezes the unit at the
        /// spline's position AND its orientation -- and a facing-only spline carries its angle at
        /// the END, so a turn launched an instant earlier is still at its initial orientation and
        /// the stop cancels it. Hence the order in EffectDistract: distract first, turn after.
        class DistractTurnsToTheSpot : public Scenario
        {
        public:
            DistractTurnsToTheSpot() : Scenario("distract-turns-to-the-spot", 61) {}

            void Prepare() override
            {
                struct St
                {
                    float wanted;      ///< the bearing from the creature to the circle centre
                    float atHalf;      ///< its facing half a second after the cast
                    float atTwo;       ///< and two seconds after it
                    bool  moving;      ///< a leg really was under way when the circle landed
                    bool  distracted;  ///< the effect really did land (it is a Distract now)
                };
                const uint32 DISTRACT = 1725;
                Creature* a = Spawn(WOLF, SD.x, SD.y, Ground(SD.x, SD.y, SD.z), 0.0f);
                Creature* rogue = Spawn(KOBOLD, SD.x + 25.0f, SD.y, Ground(SD.x + 25.0f, SD.y, SD.z), 3.1f);
                if (!a || !rogue) { Verdict("distractLanded=INVALID(spawn failed) | turnsToTheSpot=INVALID(spawn failed) | holdsTheTurn=INVALID(spawn failed)"); return; }
                Silence(a);
                Silence(rogue);
                // A human faction on the caster: Distract picks its targets as enemies around
                // the circle, and two neutral creatures are no enemies of one another. This is
                // the rogue the live cast came from, standing in for her.
                a->setFaction(14);       // Monster
                rogue->setFaction(1);    // Human: an enemy of Monster, as the live rogue was
                const ObjectGuid g = a->GetObjectGuid();
                const ObjectGuid r = rogue->GetObjectGuid();
                auto st = std::make_shared<St>();
                st->wanted = st->atHalf = st->atTwo = 0.0f;
                st->moving = st->distracted = false;

                // A leg first: live, the creature had been walking when the circle caught it.
                At(500, [this, g]()
                {
                    if (Creature* a = Get(g))
                    {
                        a->GetMotionMaster()->MovePoint(61, SD.x - 30.0f, SD.y, Ground(SD.x - 30.0f, SD.y, SD.z), true);
                        Log("walking west from (%.1f, %.1f)", a->Where().X(), a->Where().Y());
                    }
                });
                // The circle lands 8 yd NORTH of the creature, across its direction of travel,
                // so the turn it owes is a quarter turn and unmistakable.
                At(1500, [this, g, r, st, DISTRACT]()
                {
                    Creature* a = Get(g); Creature* rogue = Get(r);
                    if (!a || !rogue) { return; }
                    st->moving = !a->movespline->Finalized();
                    const float spotX = a->Where().X();
                    const float spotY = a->Where().Y() + 4.0f;
                    const float spotZ = Ground(spotX, spotY, SD.z);
                    st->wanted = a->Where().BearingTo(Geometry::Vector2(spotX, spotY));
                    rogue->CastSpell(spotX, spotY, spotZ, DISTRACT, true);
                    Log("cast %u at the spot (%.1f, %.1f), 4 yd off, inside its 10 yd circle; moving=%d, "
                        "wanted facing %.2f rad, facing now %.2f",
                        DISTRACT, spotX, spotY, st->moving ? 1 : 0, st->wanted, a->Where().Facing());
                });
                At(2000, [this, g, st]()
                {
                    if (Creature* a = Get(g))
                    {
                        st->distracted = strcmp(TypeName(a), "Distract") == 0;
                        st->atHalf = a->Where().Facing();
                        Log("+500ms after the cast: facing %.2f rad, mt=%s", st->atHalf, TypeName(a));
                    }
                });
                At(3500, [this, g, st]()
                {
                    if (Creature* a = Get(g)) { st->atTwo = a->Where().Facing(); Log("+2000ms after the cast: facing %.2f rad, mt=%s", st->atTwo, TypeName(a)); }
                });
                At(4000, [this, st]()
                {
                    const float tol = 0.20f;   // a fifth of a radian: a turn either happened or it did not
                    const char* landed = st->distracted ? "OK" : "BUG(the circle never caught it)";
                    const char* turned = !st->moving ? "INVALID(no leg was under way)"
                                                     : (AngleDiff(st->atHalf, st->wanted) <= tol ? "OK" : "BUG(kept the walking facing)");
                    const char* held = AngleDiff(st->atTwo, st->wanted) <= tol ? "OK" : "BUG(turned away again)";
                    Verdict(std::string("distractLanded=") + landed + " | turnsToTheSpot=" + turned + " | holdsTheTurn=" + held);
                });
            }
        };


    void RegisterPointScenarios(Runner& r)
    {
        r.Register(new PointInformAfterInterrupt());
        r.Register(new LongPoint());
        r.Register(new UnreachablePoint());
        r.Register(new DistractTurnsToTheSpot());
        r.Register(new VehicleBoarding());
    }
}
