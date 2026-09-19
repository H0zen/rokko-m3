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
#include "TemporarySummon.h"
#include "MotionMaster.h"
#include "movement/MoveSpline.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

// The simple-move scenarios (P5-B family 1, orders 36-44): the three rule changes the
// natives brought with them (orders 36-41) -- a knockback arc is refused on a rooted unit
// and accepted under a stun, and the charge is a kernel point that tracks its target --
// plus three compatibility scenarios (orders 42-44): an AI inform that reenters the
// facade mid-outcome, a flyer's fall at death, and a point requested while a root already
// holds the unit. Every scenario spawns its own actors at SE and ends them. Pt, Spread and
// Dist2 are the shared ones.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;
        const uint32 FLYER = 1512;     // Duskbat: InhabitType 5 (ground+air), no AIName, no vehicle --
                                       // Creature::CanFly() reads the template flag alone (queried
                                       // 2026-09-16: creature_template where InhabitType & 4 and
                                       // MinLevel < 20, ordered by Entry -- the first plain bird/bat
                                       // with no AIName and VehicleTemplateId 0)
        const Pt SE = { -3200.0f, -300.0f, 47.0f };
        const uint32 ROOT = 745;      // Web: a plain root aura, about 5 s, no damage
        const uint32 STUN = 5211;     // Bash: a plain stun aura
        const uint32 CHARGE = 100;    // Charge: the spell whose EffectCharge the kernel now serves
        const float  CHARGE_SPEED = 24.0f;

        /// One position sample with the facade reads a category needs.
        struct Sample
        {
            uint32       t;
            float        x, y, z;
            Motion::Kind mt;
        };

        /// True when the wolf saw this inform since `mark`.
        bool Informed(std::vector<Inform> const& informs, size_t mark, uint32 low, Motion::Kind kind, uint32 id)
        {
            for (size_t k = mark; k < informs.size(); ++k)
            {
                Inform const& r = informs[k];
                if (r.guidLow == low && r.kind == kind && r.id == id)
                {
                    return true;
                }
            }
            return false;
        }

        /// S36: a knockback arc requested on a rooted unit is refused (reference: a root
        /// denies displacement). MoveJump returns false, no spline is laid, the point leg
        /// beneath keeps standing where the root caught it, nothing informs -- and the point
        /// resumes on its own once the root ends, proving the refusal held nothing.
        class EffectRefusedUnderRoot : public Scenario
        {
        public:
            EffectRefusedUnderRoot() : Scenario("effect-refused-under-root", 36) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                if (!w) { Verdict("refused=INVALID(spawn failed) | noSpline=INVALID(spawn failed) | noInform=INVALID(spawn failed) | pointResumes=INVALID(spawn failed)"); return; }
                const ObjectGuid g = w->GetObjectGuid();
                const uint32 low = w->GetGUIDLow();
                const size_t mark = Informs().size();
                auto accepted = std::make_shared<bool>(true);
                auto called = std::make_shared<bool>(false);
                auto jump = std::make_shared<Pt>();
                auto rooted = std::make_shared<std::vector<Sample> >();
                auto freed = std::make_shared<std::vector<Sample> >();
                // The premise: a jump refused for a reason other than the root (the Web missed,
                // resisted, whatever) would still read accepted=false and BUG would blame the
                // kernel for a setup failure. Read at the jump's own moment.
                auto rootLanded = std::make_shared<bool>(false);
                At(500, [this, g]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    w->GetMotionMaster()->MovePoint(1, SE.x + 25.0f, SE.y, Ground(SE.x + 25.0f, SE.y, SE.z), true);
                    Log("MovePoint(1) 25 yd east, mt=%s", TypeName(w));
                });
                At(1500, [this, g]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    w->CastSpell(w, ROOT, true);
                    Log("Web on the moving wolf at %.1f %.1f, rooted=%d mt=%s", w->Where().X(), w->Where().Y(), w->IsRooted() ? 1 : 0, TypeName(w));
                });
                At(2000, [this, g, accepted, called, jump, rootLanded]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    *rootLanded = w->IsRooted();
                    const float x = w->Where().X(), y = w->Where().Y(), z = w->Where().Z();
                    jump->x = x; jump->y = y + 12.0f; jump->z = Ground(x, y + 12.0f, z);
                    *accepted = w->GetMotionMaster()->MoveJump(jump->x, jump->y, jump->z, 7.5f, 5.0f, 66);
                    *called = true;
                    Log("MoveJump(66) 12 yd sideways on the rooted wolf: accepted=%d rooted=%d mt=%s", *accepted ? 1 : 0, *rootLanded ? 1 : 0, TypeName(w));
                });
                for (uint32 t = 2000; t <= 4000; t += 250)   // under the root: nothing moves, the point stays selected
                {
                    At(t, [this, g, rooted, jump, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        Sample s; s.t = t; s.x = w->Where().X(); s.y = w->Where().Y(); s.z = w->Where().Z(); s.mt = Type(w);
                        rooted->push_back(s);
                        Log("rooted +%5ums %.2f %.2f mt=%s rooted=%d dJump=%.2f spline=%d", s.t, s.x, s.y, Motion::KindName(s.mt),
                            w->IsRooted() ? 1 : 0, Dist2(s.x, s.y, jump->x, jump->y), w->movespline->Finalized() ? 0 : 1);
                    });
                }
                for (uint32 t = 7000; t <= 9000; t += 250)   // the Web is out (cast at 1.5 s, about 5 s): the point runs again
                {
                    At(t, [this, g, freed, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        Sample s; s.t = t; s.x = w->Where().X(); s.y = w->Where().Y(); s.z = w->Where().Z(); s.mt = Type(w);
                        freed->push_back(s);
                        Log("free +%5ums %.2f %.2f mt=%s rooted=%d", s.t, s.x, s.y, Motion::KindName(s.mt), w->IsRooted() ? 1 : 0);
                    });
                }
                At(9500, [this, low, mark, accepted, called, rooted, freed, rootLanded]()
                {
                    if (!*called || rooted->size() < 5 || freed->size() < 5)
                    {
                        Verdict("refused=INVALID(no samples) | noSpline=INVALID(no samples) | noInform=INVALID(no samples) | pointResumes=INVALID(no samples)");
                        return;
                    }
                    if (!*rootLanded)
                    {
                        Verdict("refused=INVALID(the Web never landed) | noSpline=INVALID(the Web never landed) | noInform=INVALID(the Web never landed) | pointResumes=INVALID(the Web never landed)");
                        return;
                    }
                    std::vector<Pt> held;
                    bool point = true;
                    for (size_t k = 0; k < rooted->size(); ++k)
                    {
                        Sample const& s = (*rooted)[k];
                        Pt p = { s.x, s.y, s.z };
                        held.push_back(p);
                        if (s.mt != Motion::Kind::Point) { point = false; }
                    }
                    std::vector<Pt> after;
                    for (size_t k = 0; k < freed->size(); ++k)
                    {
                        Sample const& s = (*freed)[k];
                        Pt p = { s.x, s.y, s.z };
                        after.push_back(p);
                    }
                    const float drift = Spread(held);
                    const float moved = Spread(after);
                    const bool informed = Informed(Informs(), mark, low, Motion::Kind::Effect, 66);
                    char text[420];
                    char noSpline[140];
                    if (drift < 0.5f && point) { snprintf(noSpline, sizeof(noSpline), "OK(stood within %.2f yd, POINT throughout)", drift); }
                    else { snprintf(noSpline, sizeof(noSpline), "BUG(drifted %.2f yd%s)", drift, point ? "" : " and lost the point"); }
                    char resumes[140];
                    if (moved > 3.0f) { snprintf(resumes, sizeof(resumes), "OK(moved %.1f yd once the Web ended)", moved); }
                    else { snprintf(resumes, sizeof(resumes), "BUG(moved only %.1f yd after the Web ended)", moved); }
                    snprintf(text, sizeof(text), "refused=%s | noSpline=%s | noInform=%s | pointResumes=%s",
                             *accepted ? "BUG(MoveJump returned true on a rooted unit)" : "OK(MoveJump returned false)",
                             noSpline,
                             informed ? "BUG(EFFECT 66 informed after a refusal)" : "OK(no EFFECT 66 inform)",
                             resumes);
                    Verdict(text);
                });
            }
        };

        /// S37: only a root refuses an arc. A stun does not: it stops a creature, it does not
        /// deny displacement, so a jump requested on a stunned wolf launches, lands, and informs
        /// once the stun lets the behaviour tick again (S11 proves the same for a stun that
        /// lands mid-flight).
        class EffectLaunchesUnderStun : public Scenario
        {
        public:
            EffectLaunchesUnderStun() : Scenario("effect-launches-under-stun", 37) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                if (!w) { Verdict("accepted=INVALID(spawn failed) | lands=INVALID(spawn failed) | informs=INVALID(spawn failed)"); return; }
                const ObjectGuid g = w->GetObjectGuid();
                const uint32 low = w->GetGUIDLow();
                const size_t mark = Informs().size();
                auto accepted = std::make_shared<bool>(false);
                auto called = std::make_shared<bool>(false);
                auto jump = std::make_shared<Pt>();
                auto closest = std::make_shared<float>(999.0f);
                // The premise: without it "accepted=OK" is vacuous (an unstunned wolf jumping
                // proves nothing). Read at the MoveJump moment and again at the first sample
                // after it; either reading false means the Bash never applied its state.
                auto stunAtJump = std::make_shared<bool>(false);
                auto stunAtFirst = std::make_shared<bool>(false);
                At(1000, [this, g]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    w->CastSpell(w, STUN, true);
                    Log("Bash on the standing wolf: stun=%d rooted=%d mt=%s", w->hasUnitState(UNIT_STAT_STUNNED) ? 1 : 0, w->IsRooted() ? 1 : 0, TypeName(w));
                });
                At(1500, [this, g, accepted, called, jump, stunAtJump]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    *stunAtJump = w->hasUnitState(UNIT_STAT_STUNNED);
                    const float x = w->Where().X(), y = w->Where().Y(), z = w->Where().Z();
                    jump->x = x + 12.0f; jump->y = y; jump->z = Ground(x + 12.0f, y, z);
                    *accepted = w->GetMotionMaster()->MoveJump(jump->x, jump->y, jump->z, 7.5f, 5.0f, 67);
                    *called = true;
                    Log("MoveJump(67) 12 yd on the stunned wolf: accepted=%d stun=%d mt=%s", *accepted ? 1 : 0, *stunAtJump ? 1 : 0, TypeName(w));
                });
                for (uint32 t = 1700; t <= 5000; t += 200)
                {
                    At(t, [this, g, jump, closest, stunAtFirst, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        if (t == 1700) { *stunAtFirst = w->hasUnitState(UNIT_STAT_STUNNED); }
                        const float d = Dist2(w->Where().X(), w->Where().Y(), jump->x, jump->y);
                        if (d < *closest) { *closest = d; }
                        Log("+%5ums %.2f %.2f mt=%s stun=%d dJump=%.2f spline=%d", t, w->Where().X(), w->Where().Y(), TypeName(w),
                            w->hasUnitState(UNIT_STAT_STUNNED) ? 1 : 0, d, w->movespline->Finalized() ? 0 : 1);
                    });
                }
                for (uint32 t = 5500; t <= 8500; t += 500)   // the Bash runs out here: the Effect ticks again and finishes
                {
                    At(t, [this, g, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        Log("after +%5ums mt=%s stun=%d", t, TypeName(w), w->hasUnitState(UNIT_STAT_STUNNED) ? 1 : 0);
                    });
                }
                At(9000, [this, low, mark, accepted, called, closest, stunAtJump, stunAtFirst]()
                {
                    if (!*called) { Verdict("accepted=INVALID(never called) | lands=INVALID(never called) | informs=INVALID(never called)"); return; }
                    if (!*stunAtJump || !*stunAtFirst)
                    {
                        Verdict("accepted=INVALID(the Bash did not stun) | lands=INVALID(the Bash did not stun) | informs=INVALID(the Bash did not stun)");
                        return;
                    }
                    const bool informed = Informed(Informs(), mark, low, Motion::Kind::Effect, 67);
                    char lands[140];
                    if (*closest < 2.5f) { snprintf(lands, sizeof(lands), "OK(came within %.2f yd of the jump point)", *closest); }
                    else { snprintf(lands, sizeof(lands), "BUG(closest %.2f yd: the arc never played)", *closest); }
                    char text[380];
                    snprintf(text, sizeof(text), "accepted=%s | lands=%s | informs=%s",
                             *accepted ? "OK(MoveJump returned true under a stun)" : "BUG(MoveJump refused a stunned unit)",
                             lands,
                             informed ? "OK(EFFECT 67 informed)" : "BUG(no EFFECT 67 inform)");
                    Verdict(text);
                });
            }
        };

        /// S38: the charge is a kernel point that follows its target. A kobold walks away; the
        /// charging wolf re-lays its leg onto the target's contact point as it drifts, arrives
        /// at the kobold wherever it has got to, and the chase the spell's Attack installed
        /// takes over the moment the charge ends (the point was requested with resumeCombat).
        class ChargeTracksTarget : public Scenario
        {
        public:
            ChargeTracksTarget() : Scenario("charge-tracks-target", 38) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 16.0f, SE.y, Ground(SE.x + 16.0f, SE.y, SE.z), 0.0f);
                if (!w || !k) { Verdict("chargeSelected=INVALID(spawn failed) | arrivesAtTarget=INVALID(spawn failed) | relays=INVALID(spawn failed) | chaseAfter=INVALID(spawn failed)"); return; }
                w->SetMaxHealth(500000); w->SetHealth(500000);
                k->SetMaxHealth(500000); k->SetHealth(500000);
                k->setFaction(14);   // Monster: hostile to the wolf so the spell's Attack really engages (S2's idiom)
                const ObjectGuid g = w->GetObjectGuid(), gk = k->GetObjectGuid();
                auto spell = std::make_shared<bool>(false);
                auto selected = std::make_shared<bool>(false);
                auto closest = std::make_shared<float>(999.0f);
                auto goals = std::make_shared<std::vector<Pt> >();
                auto chased = std::make_shared<bool>(false);
                At(500, [this, gk]()
                {
                    Creature* k = Get(gk); if (!k) { return; }
                    k->SetWalk(true);
                    k->GetMotionMaster()->MovePoint(2, SE.x + 46.0f, SE.y, Ground(SE.x + 46.0f, SE.y, SE.z), true);
                    Log("the kobold walks 30 yd east, mt=%s", TypeName(k));
                });
                At(1000, [this, g, gk]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    // The combat entry the charge suspends. A creature's Unit::Attack sets the victim
                    // and nothing more -- the chase comes from the AI's AttackStart, and the AI only
                    // engages (and stops evading) once the target is on its threat list, which is what
                    // a real caster's damage would have put there.
                    w->AddThreat(k, 1000.0f);
                    Log("threat on the kobold: mt=%s victim=%d", TypeName(w), w->getVictim() ? 1 : 0);
                });
                At(1300, [this, g, gk]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    char const* before = TypeName(w);
                    if (Type(w) == Motion::Kind::Chase)
                    {
                        Log("the AI engaged on its own, mt=%s", before);
                        return;
                    }
                    w->Attack(k, true);
                    w->GetMotionMaster()->MoveChase(k, 0.0f, 0.0f);
                    Log("the AI had not engaged yet (mt=%s): Attack + MoveChase called directly, mt=%s", before, TypeName(w));
                });
                At(1500, [this, g, gk, spell]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    w->CastSpell(k, CHARGE, true);
                    *spell = true;
                    Log("CastSpell(Charge 100) at %.1f yd: wolf mt=%s victim=%d", Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y()),
                        TypeName(w), w->getVictim() ? 1 : 0);
                });
                At(1600, [this, g, gk, spell]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    if (Type(w) == Motion::Kind::Point)
                    {
                        Log("the Charge spell laid the kernel point itself, mt=%s", TypeName(w));
                        return;
                    }
                    // The spell's own checks refused a creature caster (range, power, target flags):
                    // the facade entry points the effect calls are exercised directly instead.
                    *spell = false;
                    w->GetMotionMaster()->MoveCharge(k, CHARGE_SPEED);
                    w->Attack(k, true);
                    Log("the Charge spell was refused (mt=%s): MoveCharge + Attack called directly", TypeName(w));
                });
                At(1800, [this, g, selected]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    *selected = Type(w) == Motion::Kind::Point;
                    Log("charge selected: mt=%s", TypeName(w));
                });
                for (uint32 t = 1800; t <= 4000; t += 200)
                {
                    At(t, [this, g, gk, closest, goals, t]()
                    {
                        Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                        const float d = Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y());
                        if (d < *closest) { *closest = d; }
                        // The re-lay count is the native's own (PointBehaviour::Relays) and the facade
                        // does not expose it; the spline's final destination is the same evidence read
                        // from outside -- one distinct destination per leg the charge laid.
                        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
                        // Only while the charge is the selection: the chase that follows lays goals of its own.
                        const bool live = Type(w) == Motion::Kind::Point && w->GetMotionMaster()->GetDestination(gx, gy, gz);
                        if (live && (goals->empty() || Dist2(gx, gy, goals->back().x, goals->back().y) > 1.0f))
                        {
                            Pt p = { gx, gy, gz };
                            goals->push_back(p);
                        }
                        Log("+%5ums wolf %.2f %.2f mt=%s | kobold %.2f %.2f mt=%s | dTarget=%.2f goal=%s %.2f %.2f",
                            t, w->Where().X(), w->Where().Y(), TypeName(w), k->Where().X(), k->Where().Y(), TypeName(k),
                            d, live ? "live" : "none", live ? gx : 0.0f, live ? gy : 0.0f);
                    });
                }
                for (uint32 t = 4200; t <= 5000; t += 200)
                {
                    At(t, [this, g, chased, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        if (Type(w) == Motion::Kind::Chase) { *chased = true; }
                        Log("after +%5ums mt=%s victim=%d", t, TypeName(w), w->getVictim() ? 1 : 0);
                    });
                }
                At(5500, [this, spell, selected, closest, goals, chased]()
                {
                    // The charge ends at the contact point (3.666 yd plus the two reaches) of the
                    // kobold's PLACEMENT, and a spline-moved unit's placement is written once per
                    // POSITION_UPDATE_DELAY (400 ms): a leg that ended past the tolerance from the fresh
                    // point is re-laid at once, but the fresh point itself trails the true position by
                    // up to one update of walking (about 2.3 yd here). So: the contact reach plus that
                    // lag, 7 yd; a sample can read 3.67 when the placement happens to be current.
                    char arrives[160];
                    if (*closest < 7.0f) { snprintf(arrives, sizeof(arrives), "OK(closed to %.2f yd of the walking kobold)", *closest); }
                    else { snprintf(arrives, sizeof(arrives), "BUG(never closer than %.2f yd)", *closest); }
                    char relays[160];
                    snprintf(relays, sizeof(relays), "OK(%u distinct destinations laid, charge from %s)",
                             static_cast<uint32>(goals->size()), *spell ? "the Charge spell" : "MoveCharge directly");
                    char text[420];
                    snprintf(text, sizeof(text), "chargeSelected=%s | arrivesAtTarget=%s | relays=%s | chaseAfter=%s",
                             *selected ? "OK(POINT while the charge runs)" : "BUG(no point behaviour after the charge)",
                             arrives, relays,
                             *chased ? "OK(CHASE took over when the charge ended)" : "BUG(no chase after the charge)");
                    Verdict(text);
                });
            }
        };

        /// S39: a root landing mid-charge holds the charge where it stands -- the point is
        /// paused by the block, never finished -- and the wolf closes the rest of the way
        /// once the root ends.
        class ChargeStopsOnRoot : public Scenario
        {
        public:
            ChargeStopsOnRoot() : Scenario("charge-stops-on-root", 39) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 25.0f, SE.y, Ground(SE.x + 25.0f, SE.y, SE.z), 3.1f);
                if (!w || !k) { Verdict("stands=INVALID(spawn failed) | chargeHeld=INVALID(spawn failed) | movesAfterRoot=INVALID(spawn failed)"); return; }
                const ObjectGuid g = w->GetObjectGuid(), gk = k->GetObjectGuid();
                auto held = std::make_shared<std::vector<Sample> >();
                auto atFour = std::make_shared<float>(-1.0f);
                auto atEight = std::make_shared<float>(-1.0f);
                At(500, [this, g, gk]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    w->GetMotionMaster()->MoveCharge(k, CHARGE_SPEED);
                    Log("MoveCharge at the kobold %.1f yd away, mt=%s", Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y()), TypeName(w));
                });
                At(1000, [this, g]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    w->CastSpell(w, ROOT, true);
                    Log("Web mid-charge at %.1f %.1f, rooted=%d mt=%s", w->Where().X(), w->Where().Y(), w->IsRooted() ? 1 : 0, TypeName(w));
                });
                for (uint32 t = 1500; t <= 4000; t += 250)
                {
                    At(t, [this, g, gk, held, t]()
                    {
                        Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                        Sample s; s.t = t; s.x = w->Where().X(); s.y = w->Where().Y(); s.z = w->Where().Z(); s.mt = Type(w);
                        held->push_back(s);
                        Log("rooted +%5ums %.2f %.2f mt=%s rooted=%d spline=%d dTarget=%.2f", s.t, s.x, s.y, Motion::KindName(s.mt),
                            w->IsRooted() ? 1 : 0, w->movespline->Finalized() ? 0 : 1,
                            Dist2(s.x, s.y, k->Where().X(), k->Where().Y()));
                    });
                }
                At(4000, [this, g, gk, atFour]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    *atFour = Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y());
                });
                for (uint32 t = 6000; t <= 8000; t += 500)
                {
                    At(t, [this, g, gk, t]()
                    {
                        Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                        Log("free +%5ums %.2f %.2f mt=%s rooted=%d dTarget=%.2f", t, w->Where().X(), w->Where().Y(), TypeName(w),
                            w->IsRooted() ? 1 : 0, Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y()));
                    });
                }
                At(8000, [this, g, gk, atEight]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    *atEight = Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y());
                });
                At(8500, [this, held, atFour, atEight]()
                {
                    if (held->size() < 5 || *atFour < 0.0f || *atEight < 0.0f)
                    {
                        Verdict("stands=INVALID(no samples) | chargeHeld=INVALID(no samples) | movesAfterRoot=INVALID(no samples)");
                        return;
                    }
                    std::vector<Pt> pts;
                    bool point = true;
                    Motion::Kind lost = Motion::Kind::Idle;
                    for (size_t k = 0; k < held->size(); ++k)
                    {
                        Sample const& s = (*held)[k];
                        Pt p = { s.x, s.y, s.z };
                        pts.push_back(p);
                        if (s.mt != Motion::Kind::Point && point) { point = false; lost = s.mt; }
                    }
                    const float drift = Spread(pts);
                    char stands[140];
                    if (drift < 0.5f) { snprintf(stands, sizeof(stands), "OK(stood within %.2f yd under the Web)", drift); }
                    else { snprintf(stands, sizeof(stands), "BUG(slid %.2f yd under the Web)", drift); }
                    char chargeHeld[160];
                    if (point) { snprintf(chargeHeld, sizeof(chargeHeld), "OK(POINT throughout: the block paused the charge, never finished it)"); }
                    else { snprintf(chargeHeld, sizeof(chargeHeld), "BUG(the charge was lost under the root: mt=%s)", Motion::KindName(lost)); }
                    char moves[160];
                    if (*atEight < *atFour) { snprintf(moves, sizeof(moves), "OK(%.1f yd out at 4 s, %.1f yd at 8 s)", *atFour, *atEight); }
                    else { snprintf(moves, sizeof(moves), "BUG(%.1f yd out at 4 s, %.1f yd at 8 s: never closed)", *atFour, *atEight); }
                    char text[460];
                    snprintf(text, sizeof(text), "stands=%s | chargeHeld=%s | movesAfterRoot=%s", stands, chargeHeld, moves);
                    Verdict(text);
                });
            }
        };

        /// S40: the charge's target vanishes mid-flight. The native ends TargetLost on its next
        /// tick: the point is retired without an inform, and nothing is left running -- no leg
        /// re-laid at a dead guid, no behaviour stuck waiting for an arrival that cannot come.
        class ChargeTargetLost : public Scenario
        {
        public:
            ChargeTargetLost() : Scenario("charge-target-lost", 40) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 25.0f, SE.y, Ground(SE.x + 25.0f, SE.y, SE.z), 3.1f);
                if (!w || !k) { Verdict("endsWithoutStall=INVALID(spawn failed) | noInform=INVALID(spawn failed)"); return; }
                const ObjectGuid g = w->GetObjectGuid(), gk = k->GetObjectGuid();
                const uint32 low = w->GetGUIDLow();
                const size_t mark = Informs().size();
                auto ended = std::make_shared<uint32>(0);
                auto lastType = std::make_shared<Motion::Kind>(Motion::Kind::Idle);
                auto atTwo = std::make_shared<Pt>();
                auto atThree = std::make_shared<Pt>();
                // The premise: a kobold that turned out not to be a summon leaves nothing
                // despawned, and without this flag the verdict would blame the kernel with
                // BUG for a setup failure instead of reporting the premise as unmet.
                auto despawned = std::make_shared<bool>(false);
                At(500, [this, g, gk]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    w->GetMotionMaster()->MoveCharge(k, CHARGE_SPEED);
                    Log("MoveCharge at the kobold %.1f yd away, mt=%s", Dist2(w->Where().X(), w->Where().Y(), k->Where().X(), k->Where().Y()), TypeName(w));
                });
                At(1000, [this, g, gk, despawned]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w) { return; }
                    if (k && k->IsTemporarySummon())
                    {
                        static_cast<TemporarySummon*>(k)->UnSummon();
                        *despawned = true;
                        Log("the kobold is despawned mid-charge; wolf at %.2f %.2f mt=%s", w->Where().X(), w->Where().Y(), TypeName(w));
                    }
                    else
                    {
                        Log("ERR the kobold is not a summon: nothing despawned");
                    }
                });
                for (uint32 t = 1200; t <= 3000; t += 200)
                {
                    At(t, [this, g, ended, lastType, atTwo, atThree, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        *lastType = Type(w);
                        if (*ended == 0 && *lastType != Motion::Kind::Point) { *ended = t; }
                        if (t == 2000) { Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() }; *atTwo = p; }
                        if (t == 3000) { Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() }; *atThree = p; }
                        Log("+%5ums %.2f %.2f mt=%s spline=%d", t, w->Where().X(), w->Where().Y(), TypeName(w), w->movespline->Finalized() ? 0 : 1);
                    });
                }
                At(3500, [this, low, mark, ended, lastType, atTwo, atThree, despawned]()
                {
                    if (!*despawned)
                    {
                        Verdict("endsWithoutStall=INVALID(the kobold was not despawned) | noInform=INVALID(the kobold was not despawned)");
                        return;
                    }
                    const bool informed = Informed(Informs(), mark, low, Motion::Kind::Point, 0);
                    // The default the arbiter reselects lays a leg of its own at once, so a finalized
                    // spline is not the evidence: what proves nothing stalled is that the charge's own
                    // 24 yd/s leg is no longer driving the wolf a second after it was retired.
                    const float drift = Dist2(atThree->x, atThree->y, atTwo->x, atTwo->y);
                    char ends[220];
                    if (*ended != 0 && *ended <= 2000 && drift < 5.0f)
                    {
                        snprintf(ends, sizeof(ends), "OK(the point was retired at +%ums; %.2f yd over the second after, not the charge's 24 yd/s)", *ended, drift);
                    }
                    else if (*ended != 0 && *ended <= 2000)
                    {
                        snprintf(ends, sizeof(ends), "BUG(retired at +%ums but still travelling %.2f yd/s)", *ended, drift);
                    }
                    else if (*ended != 0) { snprintf(ends, sizeof(ends), "BUG(the point only ended at +%ums, past 2 s)", *ended); }
                    else { snprintf(ends, sizeof(ends), "BUG(still POINT at 3 s: mt=%s)", Motion::KindName(*lastType)); }
                    char text[360];
                    snprintf(text, sizeof(text), "endsWithoutStall=%s | noInform=%s", ends,
                             informed ? "BUG(POINT 0 informed for a charge whose target was lost)" : "OK(no POINT inform)");
                    Verdict(text);
                });
            }
        };

        /// S41: the charge's destination form (EffectCharge2's swoop): a fixed goal at the
        /// charge speed, no target to track, and the silence the raw spline it replaces kept.
        class SwoopToLocation : public Scenario
        {
        public:
            SwoopToLocation() : Scenario("swoop-to-location", 41) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                if (!w) { Verdict("arrives=INVALID(spawn failed) | noInform=INVALID(spawn failed)"); return; }
                Load(SE.x + 20.0f, SE.y);
                const ObjectGuid g = w->GetObjectGuid();
                const uint32 low = w->GetGUIDLow();
                const size_t mark = Informs().size();
                const Pt goal = { SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z) };
                auto closest = std::make_shared<float>(999.0f);
                At(500, [this, g, goal]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    w->GetMotionMaster()->MoveCharge(goal.x, goal.y, goal.z, CHARGE_SPEED);
                    Log("MoveCharge to %.1f %.1f %.1f (20 yd), mt=%s", goal.x, goal.y, goal.z, TypeName(w));
                });
                for (uint32 t = 700; t <= 3000; t += 200)
                {
                    At(t, [this, g, goal, closest, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        const float d = Dist2(w->Where().X(), w->Where().Y(), goal.x, goal.y);
                        if (d < *closest) { *closest = d; }
                        Log("+%5ums %.2f %.2f mt=%s dGoal=%.2f spline=%d", t, w->Where().X(), w->Where().Y(), TypeName(w), d, w->movespline->Finalized() ? 0 : 1);
                    });
                }
                At(3500, [this, low, mark, closest]()
                {
                    const bool informed = Informed(Informs(), mark, low, Motion::Kind::Point, 0);
                    char arrives[140];
                    if (*closest < 2.0f) { snprintf(arrives, sizeof(arrives), "OK(came within %.2f yd of the goal)", *closest); }
                    else { snprintf(arrives, sizeof(arrives), "BUG(closest %.2f yd)", *closest); }
                    char text[300];
                    snprintf(text, sizeof(text), "arrives=%s | noInform=%s", arrives,
                             informed ? "BUG(POINT 0 informed: the swoop is silent)" : "OK(no POINT inform)");
                    Verdict(text);
                });
            }
        };

        /// S42: MovementInform runs before ReengageVictim within the same Outcome
        /// (NativeBehaviour::PerformOutcome processes Effect::Inform first, Effect::
        /// ReengageVictim second, and the latter reads IsChasing()/IsFollowing() live, "after
        /// the inform ran" per its own comment): an AI callback that reenters the facade from
        /// inside the inform -- installing a fresh MoveFollow the instant EFFECT 80 lands -- is
        /// seen by ReengageVictim's live check and it steps aside. The wolf keeps a victim
        /// throughout (Attack + AddThreat, as death-drops-sources sets one up) so ReengageVictim
        /// would have installed a chase had the follow not preempted it; followKept proves the
        /// final selection is FOLLOW, not that CHASE.
        class InformReentersFacade : public Scenario
        {
        public:
            InformReentersFacade() : Scenario("inform-reenters-facade", 42) {}

            void Prepare() override
            {
                m_leader = ObjectGuid();
                m_installed = false;
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* leader = Spawn(WOLF, SE.x + 6.0f, SE.y + 6.0f, Ground(SE.x + 6.0f, SE.y + 6.0f, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!w || !leader || !k) { Verdict("followKept=INVALID(spawn failed)"); return; }
                m_leader = leader->GetObjectGuid();
                const ObjectGuid g = w->GetObjectGuid();
                const ObjectGuid gk = k->GetObjectGuid();
                auto jump = std::make_shared<Pt>();
                auto called = std::make_shared<bool>(false);
                At(500, [this, g, gk]()
                {
                    Creature* w = Get(g); Creature* k = Get(gk); if (!w || !k) { return; }
                    w->Attack(k, true);
                    w->AddThreat(k, 1000.0f);
                    Log("Attack + AddThreat on the kobold: victim=%d mt=%s", w->getVictim() ? 1 : 0, TypeName(w));
                });
                At(1000, [this, g, jump, called]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    const float x = w->Where().X(), y = w->Where().Y(), z = w->Where().Z();
                    jump->x = x; jump->y = y + 12.0f; jump->z = Ground(x, y + 12.0f, z);
                    *called = w->GetMotionMaster()->MoveJump(jump->x, jump->y, jump->z, 7.5f, 5.0f, 80);
                    Log("MoveJump(80) 12 yd sideways with a victim set: accepted=%d mt=%s", *called ? 1 : 0, TypeName(w));
                });
                for (uint32 t = 3000; t <= 5000; t += 500)   // well past the arc: the reentry has already run
                {
                    At(t, [this, g, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        Log("+%5ums mt=%s reentered=%d victim=%d", t, TypeName(w), m_installed ? 1 : 0, w->getVictim() ? 1 : 0);
                    });
                }
                At(5500, [this, g, called]()
                {
                    Creature* w = Get(g);
                    if (!w || !*called) { Verdict("followKept=INVALID(jump refused or lost)"); return; }
                    if (!m_installed) { Verdict("followKept=INVALID(the inform never reentered the facade)"); return; }
                    const bool ok = Type(w) == Motion::Kind::Follow;
                    char text[160];
                    snprintf(text, sizeof(text), "followKept=%s(mt=%s)", ok ? "OK" : "BUG", TypeName(w));
                    Verdict(text);
                });
            }

            /// The recording hook (Scenario.h:105): called synchronously from inside
            /// NativeBehaviour::PerformOutcome's Effect::Inform case, before that same
            /// Outcome reaches Effect::ReengageVictim -- installing the follow here IS
            /// reentering the facade from inside the inform.
            void OnInform(Creature* creature, Motion::Kind kind, uint32 id) override
            {
                if (kind != Motion::Kind::Effect || id != 80 || m_installed) { return; }
                if (Creature* leader = Get(m_leader))
                {
                    creature->GetMotionMaster()->MoveFollow(leader, 2.0f, 0.0f);
                    m_installed = true;
                    Log("OnInform EFFECT 80: MoveFollow installed mid-outcome, mt=%s", TypeName(creature));
                }
            }

        private:
            ObjectGuid m_leader;
            bool       m_installed;
        };

        /// S43: a flying creature's death runs a fall -- Creature::SetDeathState calls
        /// MoveFall only when CanFly() (Creature.cpp:2041-2044), and for this template that
        /// reads true from the InhabitType flag alone, no vehicle and no script needed.
        /// fallRuns proves the fall actually runs (the z drops in the window right after
        /// death and ends near the ground); deadHolds proves the death claim holds
        /// throughout, the same predicate death-drops-sources reads for a grounded kill.
        class FlyerFallsAtDeath : public Scenario
        {
        public:
            FlyerFallsAtDeath() : Scenario("flyer-falls-at-death", 43) {}

            void Prepare() override
            {
                const float startZ = Ground(SE.x, SE.y, SE.z) + 10.0f;
                Creature* w = Spawn(FLYER, SE.x, SE.y, startZ, 0.0f);
                if (!w) { Verdict("fallRuns=INVALID(spawn failed) | deadHolds=INVALID(spawn failed)"); return; }
                if (!w->CanFly())
                {
                    Log("ERR entry %u does not fly on the bare map (CanFly()=0): pick another entry", FLYER);
                    Verdict("fallRuns=INVALID(the template does not fly) | deadHolds=INVALID(the template does not fly)");
                    return;
                }
                const ObjectGuid g = w->GetObjectGuid();
                Log("spawned entry %u at z=%.2f (ground %.2f, CanFly=%d)", FLYER, w->Where().Z(), Ground(SE.x, SE.y, SE.z), w->CanFly() ? 1 : 0);
                auto zAtKill = std::make_shared<float>(startZ);
                auto zAtHalf = std::make_shared<float>(startZ);
                auto zAtTwo = std::make_shared<float>(startZ);
                auto deadOk = std::make_shared<bool>(false);
                At(500, [this, g, zAtKill]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    *zAtKill = w->Where().Z();
                    w->DealDamage(w, w->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
                    Log("killed at z=%.2f: alive=%d state=%d mt=%s", *zAtKill, w->IsAlive() ? 1 : 0, int(w->GetDeathState()), TypeName(w));
                });
                At(1000, [this, g, zAtHalf]()   // +500ms after the kill
                {
                    Creature* w = Get(g); if (!w) { return; }
                    *zAtHalf = w->Where().Z();
                    Log("+500ms z=%.2f mt=%s", *zAtHalf, TypeName(w));
                });
                At(2500, [this, g, zAtTwo, deadOk]()   // +2000ms after the kill
                {
                    Creature* w = Get(g); if (!w) { return; }
                    *zAtTwo = w->Where().Z();
                    *deadOk = w->GetMotionMaster()->Inhibited(Motion::Inhibition::Dead);
                    Log("+2000ms z=%.2f mt=%s dead=%d", *zAtTwo, TypeName(w), *deadOk ? 1 : 0);
                });
                At(3000, [this, g, zAtKill, zAtHalf, zAtTwo, deadOk]()
                {
                    Creature* w = Get(g);
                    if (!w) { Verdict("fallRuns=INVALID(lost) | deadHolds=INVALID(lost)"); return; }
                    const float ground = Ground(w->Where().X(), w->Where().Y(), w->Where().Z());
                    const float droppedByHalf = *zAtKill - *zAtHalf;
                    const float nearGround = fabsf(*zAtTwo - ground);
                    const bool falls = droppedByHalf > 1.0f && nearGround < 1.5f;
                    char fall[200];
                    if (falls) { snprintf(fall, sizeof(fall), "OK(dropped %.1f yd by +0.5s, %.1f yd of the ground at +2s)", droppedByHalf, nearGround); }
                    else { snprintf(fall, sizeof(fall), "BUG(z %.1f -> %.1f -> %.1f, ground %.1f)", *zAtKill, *zAtHalf, *zAtTwo, ground); }
                    char text[320];
                    snprintf(text, sizeof(text), "fallRuns=%s | deadHolds=%s", fall, *deadOk ? "OK(Inhibited(Dead) held)" : "BUG(Inhibited(Dead) false while dead)");
                    Verdict(text);
                });
            }
        };

        /// S44: a Point requested while a root already holds the unit lays no leg -- the
        /// claim is granted (POINT is selected) but the native reads mayMove itself and the
        /// unit stands where it was rooted, not merely a jump landing on an already-moving
        /// leg (S36's case). Once the root lifts, the very same request runs; the inform
        /// fires exactly once, at the real arrival.
        class PointUnderBlockLaysNothing : public Scenario
        {
        public:
            PointUnderBlockLaysNothing() : Scenario("point-under-block-lays-nothing", 44) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                if (!w) { Verdict("noLegUnderRoot=INVALID(spawn failed) | legAfterRoot=INVALID(spawn failed) | informsAtArrival=INVALID(spawn failed)"); return; }
                const ObjectGuid g = w->GetObjectGuid();
                const uint32 low = w->GetGUIDLow();
                const size_t mark = Informs().size();
                auto called = std::make_shared<bool>(false);
                auto rootLanded = std::make_shared<bool>(false);
                auto held = std::make_shared<std::vector<Sample> >();
                auto after = std::make_shared<std::vector<Sample> >();
                At(500, [this, g]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    w->CastSpell(w, ROOT, true);
                    Log("Web on the standing wolf: rooted=%d mt=%s", w->IsRooted() ? 1 : 0, TypeName(w));
                });
                At(1000, [this, g, called, rootLanded]()
                {
                    Creature* w = Get(g); if (!w) { return; }
                    *rootLanded = w->IsRooted();
                    const float x = w->Where().X(), y = w->Where().Y(), z = w->Where().Z();
                    w->GetMotionMaster()->MovePoint(3, x + 25.0f, y, Ground(x + 25.0f, y, z), true);
                    *called = true;
                    Log("MovePoint(3) 25 yd east requested under the root: rooted=%d mt=%s", *rootLanded ? 1 : 0, TypeName(w));
                });
                for (uint32 t = 1500; t <= 4000; t += 250)   // under the root: nothing moves, the point stays selected
                {
                    At(t, [this, g, held, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        Sample s; s.t = t; s.x = w->Where().X(); s.y = w->Where().Y(); s.z = w->Where().Z(); s.mt = Type(w);
                        held->push_back(s);
                        Log("rooted +%5ums %.2f %.2f mt=%s rooted=%d", s.t, s.x, s.y, Motion::KindName(s.mt), w->IsRooted() ? 1 : 0);
                    });
                }
                for (uint32 t = 6000; t <= 8000; t += 250)   // the Web is out (cast at 0.5 s, about 5 s): the point runs
                {
                    At(t, [this, g, after, t]()
                    {
                        Creature* w = Get(g); if (!w) { return; }
                        Sample s; s.t = t; s.x = w->Where().X(); s.y = w->Where().Y(); s.z = w->Where().Z(); s.mt = Type(w);
                        after->push_back(s);
                        Log("free +%5ums %.2f %.2f mt=%s rooted=%d", s.t, s.x, s.y, Motion::KindName(s.mt), w->IsRooted() ? 1 : 0);
                    });
                }
                At(12000, [this, low, mark, called, rootLanded, held, after]()
                {
                    if (!*called || held->size() < 5 || after->size() < 5)
                    {
                        Verdict("noLegUnderRoot=INVALID(no samples) | legAfterRoot=INVALID(no samples) | informsAtArrival=INVALID(no samples)");
                        return;
                    }
                    if (!*rootLanded)
                    {
                        Verdict("noLegUnderRoot=INVALID(the Web never landed) | legAfterRoot=INVALID(the Web never landed) | informsAtArrival=INVALID(the Web never landed)");
                        return;
                    }
                    std::vector<Pt> heldPts;
                    bool point = true;
                    for (size_t k = 0; k < held->size(); ++k)
                    {
                        Sample const& s = (*held)[k];
                        Pt p = { s.x, s.y, s.z };
                        heldPts.push_back(p);
                        if (s.mt != Motion::Kind::Point) { point = false; }
                    }
                    std::vector<Pt> afterPts;
                    for (size_t k = 0; k < after->size(); ++k)
                    {
                        Sample const& s = (*after)[k];
                        Pt p = { s.x, s.y, s.z };
                        afterPts.push_back(p);
                    }
                    const float drift = Spread(heldPts);
                    const float moved = Spread(afterPts);
                    uint32 count = 0;
                    for (size_t k = mark; k < Informs().size(); ++k)
                    {
                        Inform const& r = Informs()[k];
                        if (r.guidLow == low && r.kind == Motion::Kind::Point && r.id == 3) { ++count; }
                    }
                    char noLeg[140];
                    if (drift < 0.5f && point) { snprintf(noLeg, sizeof(noLeg), "OK(stood within %.2f yd, POINT throughout)", drift); }
                    else { snprintf(noLeg, sizeof(noLeg), "BUG(drifted %.2f yd%s)", drift, point ? "" : " and lost the point"); }
                    char legAfter[140];
                    if (moved > 3.0f) { snprintf(legAfter, sizeof(legAfter), "OK(moved %.1f yd once the Web ended)", moved); }
                    else { snprintf(legAfter, sizeof(legAfter), "BUG(moved only %.1f yd after the Web ended)", moved); }
                    char informsText[80];
                    if (count == 1) { snprintf(informsText, sizeof(informsText), "OK((POINT, 3) informed once)"); }
                    else { snprintf(informsText, sizeof(informsText), "BUG(informed %u time(s))", count); }
                    char text[440];
                    snprintf(text, sizeof(text), "noLegUnderRoot=%s | legAfterRoot=%s | informsAtArrival=%s", noLeg, legAfter, informsText);
                    Verdict(text);
                });
            }
        };
    }

    void RegisterSimpleScenarios(Runner& r)
    {
        r.Register(new EffectRefusedUnderRoot());
        r.Register(new EffectLaunchesUnderStun());
        r.Register(new ChargeTracksTarget());
        r.Register(new ChargeStopsOnRoot());
        r.Register(new ChargeTargetLost());
        r.Register(new SwoopToLocation());
        r.Register(new InformReentersFacade());
        r.Register(new FlyerFallsAtDeath());
        r.Register(new PointUnderBlockLaysNothing());
    }
}
