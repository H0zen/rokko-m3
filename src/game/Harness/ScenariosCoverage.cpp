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
#include "Vehicle.h"
#include "movement/MoveSpline.h"
#include "Log.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

// The coverage scenarios (after P5-A): the feeders the block scenarios never exercised --
// a vehicle seat, a real death and respawn, a possession by a creature charmer, two feign
// auras on one unit (design 2026-09-16-movement-p5-coverage-design.md; reference 9, 11,
// 15.4, 15.5). Every scenario spawns its own actors at SE and ends them.
namespace Harness
{
    namespace
    {
        const uint32 WOLF = 69;
        const uint32 KOBOLD = 6;
        const uint32 DEMOLISHER = 28094;   // Wintergrasp Demolisher, vehicle 106: a driver seat and passenger seats
        const Pt SE = { -3200.0f, -300.0f, 47.0f };
        const uint32 ROOT = 745;           // Web: a plain root aura, about 5 s
        const uint32 FEAR = 5782;          // Fear: the claim's spell id for SetFeared
        const uint32 FEIGN_A = 29266;      // Permanent Feign Death (the dummy family)
        const uint32 FEIGN_B = 31261;      // Permanent Feign Death (Root), a second member of the family

        /// The kernel's reads a category asserts, gathered once per sample.
        struct BlockRead
        {
            bool rooted;       ///< MotionMaster::Inhibited(Rooted)
            bool stunned;      ///< MotionMaster::Inhibited(Stunned)
            bool dead;         ///< MotionMaster::Inhibited(Dead)
            bool possessed;    ///< MotionMaster::Inhibited(Possessed)
            bool mayMove;      ///< Mobility().mayMove
            uint8 reasons;     ///< Mobility().reasons
            size_t rootSources;
            size_t deadSources;
        };

        BlockRead ReadBlock(Creature* c)
        {
            MotionMaster* mm = c->GetMotionMaster();
            const Motion::MobilityDecision d = mm->Mobility();
            BlockRead r = { mm->Inhibited(Motion::Inhibition::Rooted), mm->Inhibited(Motion::Inhibition::Stunned),
                            mm->Inhibited(Motion::Inhibition::Dead), mm->Inhibited(Motion::Inhibition::Possessed),
                            d.mayMove, d.reasons,
                            mm->Arbiter().Sources(Motion::Inhibition::Rooted).size(),
                            mm->Arbiter().Sources(Motion::Inhibition::Dead).size() };
            return r;
        }

        /// S32: a vehicle seat roots its passenger (reference 9.4-9.5; design P5-A 9.10): the
        /// seat is one Rooted source; a point requested on the seat is held while the boarding
        /// walk settles the seat pose at the seat's own attachment point (VehicleSeatEntry::
        /// AttachmentOffset_0..2 -- many seats carry (0,0,0)) and it stays there; a seat switch
        /// keeps the root with the new seat's source and walks the pose to the NEW seat's
        /// attachment; the unboard releases the root and returns the rider to where it
        /// boarded; a fresh point plays after. A creature takes the first seat whose flags
        /// lack SEAT_FLAG_CAN_CONTROL (IsUsableSeatForCreature) -- a non-control seat keeps
        /// the vehicle's control seat empty, so no charm forms -- seatRoots asserts that too.
        /// A rider's Where() IS its seat pose (WorldObject::Where(), TransportInfo::
        /// SetSeatPose): a Spread on a seated unit's samples measures the seat pose's drift in
        /// the VEHICLE's frame, not a world displacement. With a stationary vehicle the two
        /// happen to coincide, which is why the round-0 run's world-corrupted pose (fixed in
        /// VehicleInfo::Board -- a rider's world spline, still running the instant it became a
        /// rider, used to land in the seat pose once the seat root's own claim suspended it)
        /// read heldOnSeat=BUG(spread 4.5) -- a large spread, not a silent OK; the reviews
        /// traced the rest. seatPoseIsLocal pins the pose itself so that mistake cannot pass
        /// silently again. The board and switch splines walk the rider from its boarding
        /// offset to the seat's own attachment point -- the boarding animation, not a drift --
        /// so seatSettles/switchSettles assert the pose actually arrives and stops there, not
        /// merely that it holds still wherever it happened to board.
        class SeatHoldsPassenger : public Scenario
        {
        public:
            SeatHoldsPassenger() : Scenario("seat-holds-passenger", 32) {}

            void Prepare() override
            {
                Creature* v = Spawn(DEMOLISHER, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* w = Spawn(WOLF, SE.x + 5.0f, SE.y, Ground(SE.x + 5.0f, SE.y, SE.z), 3.1f);
                if (!v || !w) { Verdict(Invalid("spawn failed")); return; }
                const ObjectGuid gv = v->GetObjectGuid(), gw = w->GetObjectGuid();
                auto seat = std::make_shared<int>(-1);
                auto seat2 = std::make_shared<int>(-1);
                auto seatFailReason = std::make_shared<std::string>("not reached");
                auto preBoard = std::make_shared<Pt>();
                auto seat1Attach = std::make_shared<Pt>();
                auto seat2Attach = std::make_shared<Pt>();
                auto held = std::make_shared<std::vector<Pt> >();
                auto heldMayMove = std::make_shared<bool>(false);
                auto switchHeld = std::make_shared<std::vector<Pt> >();
                auto after = std::make_shared<std::vector<Pt> >();
                auto seatRoots = std::make_shared<std::string>("INVALID(not reached)");
                auto seatPoseIsLocal = std::make_shared<std::string>("INVALID(not reached)");
                auto switchKeeps = std::make_shared<std::string>("INVALID(not reached)");
                auto unboardReleases = std::make_shared<std::string>("INVALID(not reached)");
                auto unboardPlacesBack = std::make_shared<std::string>("INVALID(not reached)");

                At(500, [this, gw]()
                {
                    // A world spline in flight when it boards at +1000ms: the wolf is still on
                    // its own errand, not yet a rider, so this is the regression guard for the
                    // seat-pose corruption the class comment describes.
                    if (Creature* w = Get(gw)) { w->GetMotionMaster()->MovePoint(9, SE.x + 30.0f, SE.y, Ground(SE.x + 30.0f, SE.y, SE.z)); Log("pre-board point requested (a world spline in flight at boarding), mt=%s", TypeName(w)); }
                });
                At(1000, [this, gv, gw, seat, seatFailReason, preBoard, seat1Attach]()
                {
                    Creature* v = Get(gv); Creature* w = Get(gw); if (!v || !w) { return; }
                    VehicleInfo* vi = v->GetVehicleInfo();
                    if (!vi) { Log("no vehicle info on %u", v->GetEntry()); *seatFailReason = "no vehicle info"; return; }
                    Log("vehicle info initialised=%d", vi->IsInitialized() ? 1 : 0);
                    // The first seat that both accepts the wolf AND is not a control seat
                    // (SEAT_FLAG_CAN_CONTROL): a passenger seat is a matter of the flag, not the
                    // index -- this template happens to make seat 0 the control seat, but that
                    // is this template's fact, not a rule this loop may assume.
                    for (uint8 s = 0; s < 8; ++s)
                    {
                        if (!vi->IsSeatAvailableFor(w, s)) { continue; }
                        VehicleSeatEntry const* candidate = vi->GetSeatEntry(s);
                        if (candidate && !(candidate->Flags & SEAT_FLAG_CAN_CONTROL)) { *seat = s; break; }
                    }
                    if (*seat < 0) { Log("no passenger seat accepts the wolf"); *seatFailReason = "no passenger seat"; return; }
                    if (VehicleSeatEntry const* chosen = vi->GetSeatEntry(uint8(*seat)))
                    {
                        *seat1Attach = Pt { chosen->AttachmentOffset_0, chosen->AttachmentOffset_1, chosen->AttachmentOffset_2 };
                        Log("chosen seat %d flags=0x%x attach=%.1f %.1f %.1f", *seat, chosen->Flags, seat1Attach->x, seat1Attach->y, seat1Attach->z);
                    }
                    // The wolf's own world spot right before it stops being a world object: the
                    // spline's live position (up to a POSITION_UPDATE_DELAY ahead of the placement),
                    // read WITHOUT stopping it -- the spline must still be in flight when Board runs,
                    // since Board's own stop of it is what seatPoseIsLocal proves. Board reads the
                    // same spot from the stop's pending commit. seatPoseIsLocal also checks the seat
                    // pose's sign against this spot (the wolf boards from the vehicle's +x side).
                    if (!w->movespline->Finalized())
                    {
                        Movement::Location const loc = w->movespline->ComputePosition();
                        *preBoard = Pt { loc.x, loc.y, loc.z };
                    }
                    else
                    {
                        *preBoard = Pt { w->Where().X(), w->Where().Y(), w->Where().Z() };
                    }
                    vi->Board(w, uint8(*seat));
                    Log("wolf boards seat %d from %.1f %.1f, mt=%s", *seat, preBoard->x, preBoard->y, TypeName(w));
                });
                At(1500, [this, gv, gw, seat, seatRoots]()
                {
                    Creature* v = Get(gv); Creature* w = Get(gw); if (!v || !w || *seat < 0) { return; }
                    const BlockRead r = ReadBlock(w);
                    std::vector<uint64> const& src = w->GetMotionMaster()->Arbiter().Sources(Motion::Inhibition::Rooted);
                    const uint64 want = Motion::InhibitSource(Motion::SourceDomain::Seat, v->GetObjectGuid().GetCounter(), uint32(*seat));
                    const bool noCharm = !r.possessed && v->GetCharmerGuid().IsEmpty();
                    const bool ok = w->IsRooted() && r.rooted && w->hasUnitState(UNIT_STAT_ROOT) && src.size() == 1 && src[0] == want && noCharm;
                    char text[200];
                    snprintf(text, sizeof(text), "%s(rooted=%d state=%d sources=%u possessed=%d charmer=%d)", ok ? "OK" : "BUG",
                             w->IsRooted() ? 1 : 0, w->hasUnitState(UNIT_STAT_ROOT) ? 1 : 0, uint32(src.size()), r.possessed ? 1 : 0, v->GetCharmerGuid().IsEmpty() ? 0 : 1);
                    *seatRoots = text;
                    Log("seated: %s", text);
                });
                At(1500, [this, gv, gw, seat, seatPoseIsLocal, preBoard]()
                {
                    Creature* v = Get(gv); Creature* w = Get(gw); if (!v || !w || *seat < 0) { return; }
                    TransportInfo* ti = w->GetTransportInfo();
                    if (!ti) { *seatPoseIsLocal = "INVALID(no transport info)"; Log("seatPoseIsLocal: %s", seatPoseIsLocal->c_str()); return; }
                    // OK below ~15 yd: the wolf boards from about 5 yd out and may have walked a
                    // few yards more on its pre-board errand by now; a seat pose corrupted with a
                    // world position reads in the thousands (the bug this pins: see the class
                    // comment). The sign pins the convention: the pose's x is the boarding spot's
                    // offset along the vehicle's heading (R(o)^T · (spot - vehicle), standard),
                    // so it carries the boarding side's sign and never exceeds that offset (the
                    // walk to the attachment point only shortens it); a reflected pose (the old
                    // -R(o) · delta) reads the opposite sign.
                    Geometry::Placement const& seatPose = ti->Seat();
                    const float mag = seatPose.Pos().magnitude();
                    const float dx = preBoard->x - v->Where().X(), dy = preBoard->y - v->Where().Y();
                    const float along = cosf(v->Where().Facing()) * dx + sinf(v->Where().Facing()) * dy;
                    const bool sameSide = seatPose.X() * along > 0.0f && fabsf(seatPose.X()) <= fabsf(along) + 0.5f;
                    const bool ok = mag < 15.0f && sameSide;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(magnitude %.1f, x %.1f along %.1f)", ok ? "OK" : "BUG", mag, seatPose.X(), along);
                    *seatPoseIsLocal = text;
                    Log("seatPoseIsLocal: %s", text);
                });
                At(2000, [this, gw]()
                {
                    if (Creature* w = Get(gw)) { w->GetMotionMaster()->MovePoint(0, SE.x + 25.0f, SE.y, Ground(SE.x + 25.0f, SE.y, SE.z)); Log("point requested on the seat, mt=%s", TypeName(w)); }
                });
                for (uint32 t = 2500; t <= 5000; t += 500)
                {
                    At(t, [this, gw, held, heldMayMove, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        TransportInfo* ti = w->GetTransportInfo(); if (!ti) { return; }
                        // The seat pose itself (TransportInfo::Seat(), always the seat pose; a
                        // rider's Where() is only its last writer): this samples the rider's walk
                        // to the seat's attachment point in the VEHICLE's frame, not the world's --
                        // seatSettles below checks it actually arrives there.
                        Pt p = { ti->Seat().X(), ti->Seat().Y(), ti->Seat().Z() };
                        held->push_back(p);
                        if (ReadBlock(w).mayMove) { *heldMayMove = true; }
                        Log("on seat +%4ums mt=%s rider(seat) %.1f %.1f mayMove=%d", t, TypeName(w), p.x, p.y, ReadBlock(w).mayMove ? 1 : 0);
                    });
                }
                At(5000, [this, gv, gw, seat, seat2, seat2Attach]()
                {
                    Creature* v = Get(gv); Creature* w = Get(gw); if (!v || !w || *seat < 0) { return; }
                    VehicleInfo* vi = v->GetVehicleInfo(); if (!vi) { return; }
                    // Same rule as the initial pick (M5): the flag, not the index.
                    for (uint8 s = uint8(*seat + 1); s < 8; ++s)
                    {
                        if (!vi->IsSeatAvailableFor(w, s)) { continue; }
                        VehicleSeatEntry const* candidate = vi->GetSeatEntry(s);
                        if (candidate && !(candidate->Flags & SEAT_FLAG_CAN_CONTROL)) { *seat2 = s; break; }
                    }
                    if (*seat2 < 0) { Log("single passenger seat, the switch is skipped"); return; }
                    if (VehicleSeatEntry const* chosen = vi->GetSeatEntry(uint8(*seat2)))
                    {
                        *seat2Attach = Pt { chosen->AttachmentOffset_0, chosen->AttachmentOffset_1, chosen->AttachmentOffset_2 };
                        Log("switch target seat %d flags=0x%x attach=%.1f %.1f %.1f", *seat2, chosen->Flags, seat2Attach->x, seat2Attach->y, seat2Attach->z);
                    }
                    vi->SwitchSeat(w, uint8(*seat2));
                    Log("wolf switches to seat %d", *seat2);
                });
                At(5500, [this, gv, gw, seat, seat2, switchKeeps]()
                {
                    Creature* v = Get(gv); Creature* w = Get(gw); if (!v || !w || *seat < 0) { return; }
                    if (*seat2 < 0) { *switchKeeps = "OK(single seat, switch skipped)"; return; }
                    std::vector<uint64> const& src = w->GetMotionMaster()->Arbiter().Sources(Motion::Inhibition::Rooted);
                    const uint64 want = Motion::InhibitSource(Motion::SourceDomain::Seat, v->GetObjectGuid().GetCounter(), uint32(*seat2));
                    const bool ok = w->IsRooted() && src.size() == 1 && src[0] == want;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(rooted=%d sources=%u seat=%d)", ok ? "OK" : "BUG", w->IsRooted() ? 1 : 0, uint32(src.size()), *seat2);
                    *switchKeeps = text;
                    Log("after the switch: %s", text);
                });
                for (uint32 t = 5500; t <= 6500; t += 500)
                {
                    At(t, [this, gw, switchHeld, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        TransportInfo* ti = w->GetTransportInfo(); if (!ti) { return; }
                        // The seat pose again: a window between the switch and the unboard, so
                        // switchSettles can check the pose actually walks to the NEW seat's
                        // attachment point instead of staying at the old one's.
                        Pt p = { ti->Seat().X(), ti->Seat().Y(), ti->Seat().Z() };
                        switchHeld->push_back(p);
                        Log("after switch +%4ums mt=%s rider(seat) %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(7000, [this, gv, gw, seat]()
                {
                    Creature* v = Get(gv); Creature* w = Get(gw); if (!v || !w || *seat < 0) { return; }
                    if (VehicleInfo* vi = v->GetVehicleInfo()) { vi->UnBoard(w, false); Log("wolf unboards, mt=%s", TypeName(w)); }
                });
                At(7500, [this, gw, seat, unboardReleases]()
                {
                    Creature* w = Get(gw); if (!w || *seat < 0) { return; }
                    const BlockRead r = ReadBlock(w);
                    const bool ok = !r.rooted && !w->IsRooted() && !w->hasUnitState(UNIT_STAT_ROOT) && r.rootSources == 0;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(rooted=%d state=%d sources=%u mt=%s)", ok ? "OK" : "BUG",
                             w->IsRooted() ? 1 : 0, w->hasUnitState(UNIT_STAT_ROOT) ? 1 : 0, uint32(r.rootSources), TypeName(w));
                    *unboardReleases = text;
                    Log("after the unboard: %s", text);
                });
                At(7500, [this, gv, gw, seat, seat2, seat1Attach, seat2Attach, unboardPlacesBack]()
                {
                    Creature* v = Get(gv); Creature* w = Get(gw); if (!v || !w || *seat < 0) { return; }
                    // The board/switch splines now walk the seat pose to the seat's own
                    // attachment point (class comment), so a stationary vehicle's unboard must
                    // return the rider to THAT -- the vehicle's own position composed with the
                    // currently-seated seat's attachment offset (Basis().localToWorld, the same
                    // composition UpdateGlobalPositionOf uses) -- not to where it originally
                    // boarded: the walk moves the rider on purpose, so preBoard (logged above)
                    // is no longer where the unboard is expected to return it. Reference: the
                    // re-review's reflection, 15.6 yd on the wrong side, before the seat-pose
                    // convention itself was fixed.
                    Pt const& currentAttach = (*seat2 >= 0) ? *seat2Attach : *seat1Attach;
                    Geometry::Vector3 const expected = v->Where().Basis().localToWorld(Geometry::Vector3(currentAttach.x, currentAttach.y, currentAttach.z));
                    const float d = Dist2(w->Where().X(), w->Where().Y(), expected.x, expected.y);
                    const bool ok = d < 3.0f;
                    char text[100];
                    snprintf(text, sizeof(text), "%s(dist %.1f, expected %.1f %.1f)", ok ? "OK" : "BUG", d, expected.x, expected.y);
                    *unboardPlacesBack = text;
                    Log("unboardPlacesBack: %s", text);
                });
                At(8000, [this, gw]()
                {
                    if (Creature* w = Get(gw)) { w->GetMotionMaster()->MovePoint(1, SE.x + 25.0f, SE.y + 10.0f, Ground(SE.x + 25.0f, SE.y + 10.0f, SE.z)); Log("fresh point after the unboard, mt=%s", TypeName(w)); }
                });
                for (uint32 t = 8500; t <= 11000; t += 500)
                {
                    At(t, [this, gw, after, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        after->push_back(p);
                        Log("after +%4ums mt=%s at %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(11500, [this, gw, seat, seatFailReason, held, heldMayMove, seat1Attach, switchHeld, seat2, seat2Attach, after, seatRoots, seatPoseIsLocal, switchKeeps, unboardReleases, unboardPlacesBack]()
                {
                    if (*seat < 0) { Verdict(Invalid(seatFailReason->c_str())); return; }
                    if (!Get(gw)) { Verdict(Invalid("lost")); return; }
                    if (held->size() < 3 || after->size() < 3) { Verdict(Invalid("no samples")); return; }
                    const float afterSpread = Spread(*after);
                    // seatSettles: the pose must have arrived at the seat's own attachment point
                    // by the end of the window (within 0.5 yd) and stopped moving there (the last
                    // two samples within 0.1 yd of each other) -- not merely held wherever it
                    // happened to board (that was heldOnSeat's old, weaker claim).
                    std::string seatSettles;
                    {
                        Pt const& last = (*held)[held->size() - 1];
                        Pt const& prev = (*held)[held->size() - 2];
                        const float toAttach = Dist2(last.x, last.y, seat1Attach->x, seat1Attach->y);
                        const float lastTwo = Dist2(last.x, last.y, prev.x, prev.y);
                        const bool ok = toAttach < 0.5f && lastTwo < 0.1f && !*heldMayMove;
                        char t1[110];
                        snprintf(t1, sizeof(t1), "%s(toAttach %.2f, lastTwo %.2f, mayMove seen %d)", ok ? "OK" : "BUG", toAttach, lastTwo, *heldMayMove ? 1 : 0);
                        seatSettles = t1;
                    }
                    std::string switchSettles;
                    if (*seat2 < 0) { switchSettles = "OK(single seat, switch skipped)"; }
                    else if (switchHeld->size() < 3) { switchSettles = "INVALID(no samples)"; }
                    else
                    {
                        Pt const& last = (*switchHeld)[switchHeld->size() - 1];
                        Pt const& prev = (*switchHeld)[switchHeld->size() - 2];
                        const float toAttach = Dist2(last.x, last.y, seat2Attach->x, seat2Attach->y);
                        const float lastTwo = Dist2(last.x, last.y, prev.x, prev.y);
                        const bool ok = toAttach < 0.5f && lastTwo < 0.1f;
                        char t2[90];
                        snprintf(t2, sizeof(t2), "%s(toAttach %.2f, lastTwo %.2f)", ok ? "OK" : "BUG", toAttach, lastTwo);
                        switchSettles = t2;
                    }
                    char text[640];
                    snprintf(text, sizeof(text), "seatRoots=%s | seatPoseIsLocal=%s | seatSettles=%s | switchKeepsRoot=%s | switchSettles=%s | unboardReleases=%s | unboardPlacesBack=%s | movesAfterUnboard=%s(spread %.1f)",
                             seatRoots->c_str(), seatPoseIsLocal->c_str(), seatSettles.c_str(),
                             switchKeeps->c_str(), switchSettles.c_str(), unboardReleases->c_str(), unboardPlacesBack->c_str(),
                             afterSpread > 3.0f ? "OK" : "BUG", afterSpread);
                    Verdict(text);
                });
            }

        private:
            static std::string Invalid(char const* why)
            {
                std::string w = std::string("INVALID(") + why + ")";
                return "seatRoots=" + w + " | seatPoseIsLocal=" + w + " | seatSettles=" + w + " | switchKeepsRoot=" + w + " | switchSettles=" + w + " | unboardReleases=" + w + " | unboardPlacesBack=" + w + " | movesAfterUnboard=" + w;
            }
        };

        /// S33: a death drops the aura and script sources and holds the body; the respawn is
        /// clean and moves (reference 11.1, 11.5; design P5-A 9.14). Three sources go on a
        /// chasing wolf (a Web, a script root, a script stun); the wolf is killed; Rooted and
        /// Stunned are gone at once and Dead holds with the death source alone; Respawn()
        /// restamps the wall-clock respawn time so the next tick respawns it, after which no
        /// reason and no source remain and a point plays.
        class DeathDropsSources : public Scenario
        {
        public:
            DeathDropsSources() : Scenario("death-drops-sources", 33) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 20.0f, SE.y, Ground(SE.x + 20.0f, SE.y, SE.z), 3.1f);
                if (!w || !k) { Verdict(Invalid("spawn failed")); return; }
                const ObjectGuid gw = w->GetObjectGuid(), gk = k->GetObjectGuid();
                auto sourcesHeld = std::make_shared<std::string>("INVALID(not reached)");
                auto deathDrops = std::make_shared<std::string>("INVALID(not reached)");
                auto respawnClean = std::make_shared<std::string>("INVALID(not reached)");
                auto heldPos = std::make_shared<std::vector<Pt> >();
                auto dead = std::make_shared<std::vector<Pt> >();
                auto after = std::make_shared<std::vector<Pt> >();
                At(500, [this, gw, gk]()
                {
                    Creature* w = Get(gw); Creature* k = Get(gk); if (!w || !k) { return; }
                    w->Attack(k, true);
                    w->AddThreat(k, 1000.0f);
                    w->GetMotionMaster()->MoveChase(k, 0.0f, 0.0f);
                    Log("Attack + MoveChase from 20 yd, mt=%s", TypeName(w));
                });
                At(1000, [this, gw, heldPos]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    w->CastSpell(w, ROOT, true);
                    w->GetMotionMaster()->Inhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::Script, w->GetObjectGuid().GetCounter(), 33));
                    w->GetMotionMaster()->Inhibit(Motion::Inhibition::Stunned, Motion::InhibitSource(Motion::SourceDomain::Script, w->GetObjectGuid().GetCounter(), 33));
                    Log("a Web, a script root and a script stun, mt=%s", TypeName(w));
                    // Design 3.2's third clause: the three sources actually hold the chasing wolf
                    // still, not merely counted -- sampled here and at +1500 ms.
                    heldPos->push_back(Pt { w->Where().X(), w->Where().Y(), w->Where().Z() });
                });
                At(1500, [this, gw, sourcesHeld, heldPos]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    const BlockRead r = ReadBlock(w);
                    heldPos->push_back(Pt { w->Where().X(), w->Where().Y(), w->Where().Z() });
                    const float spread = heldPos->size() >= 2 ? Spread(*heldPos) : 999.0f;
                    const bool ok = r.rootSources == 2 && r.stunned && r.rooted && spread < 0.5f;
                    char text[160];
                    snprintf(text, sizeof(text), "%s(rootSources=%u stunned=%d spread=%.1f)", ok ? "OK" : "BUG", uint32(r.rootSources), r.stunned ? 1 : 0, spread);
                    *sourcesHeld = text;
                    Log("held: %s", text);
                });
                At(2000, [this, gw]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    w->DealDamage(w, w->GetHealth(), NULL, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, NULL, false);
                    Log("killed: alive=%d state=%d mt=%s", w->IsAlive() ? 1 : 0, int(w->GetDeathState()), TypeName(w));
                });
                At(2500, [this, gw, deathDrops]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    const BlockRead r = ReadBlock(w);
                    std::vector<uint64> const& src = w->GetMotionMaster()->Arbiter().Sources(Motion::Inhibition::Dead);
                    const bool ok = !w->IsAlive() && r.dead && !r.rooted && !r.stunned && src.size() == 1 && src[0] == Motion::kDeathSource && r.reasons == Motion::ReasonDead;
                    char text[160];
                    snprintf(text, sizeof(text), "%s(alive=%d dead=%d rooted=%d stunned=%d deadSources=%u reasons=%u)", ok ? "OK" : "BUG",
                             w->IsAlive() ? 1 : 0, r.dead ? 1 : 0, r.rooted ? 1 : 0, r.stunned ? 1 : 0, uint32(src.size()), uint32(r.reasons));
                    *deathDrops = text;
                    Log("after death: %s", text);
                });
                for (uint32 t = 2500; t <= 4000; t += 500)
                {
                    At(t, [this, gw, dead, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        dead->push_back(p);
                        Log("dead +%4ums state=%d at %.1f %.1f", t, int(w->GetDeathState()), p.x, p.y);
                    });
                }
                At(4000, [this, gw]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    w->Respawn();
                    Log("Respawn() called: state=%d", int(w->GetDeathState()));
                });
                At(4500, [this, gw, respawnClean]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    if (!w->IsAlive())
                    {
                        char text[80];
                        snprintf(text, sizeof(text), "INVALID(no respawn: state %d)", int(w->GetDeathState()));
                        *respawnClean = text;
                        Log("%s", text);
                        return;
                    }
                    const BlockRead r = ReadBlock(w);
                    MotionMaster* mm = w->GetMotionMaster();
                    const bool noSources = mm->Arbiter().Sources(Motion::Inhibition::Rooted).empty() && mm->Arbiter().Sources(Motion::Inhibition::Stunned).empty() &&
                                           mm->Arbiter().Sources(Motion::Inhibition::Dead).empty() && mm->Arbiter().Sources(Motion::Inhibition::Possessed).empty();
                    const bool noState = (w->GetUnitState() & (UNIT_STAT_ROOT | UNIT_STAT_STUNNED | UNIT_STAT_DIED)) == 0;
                    const bool ok = !r.dead && r.reasons == 0 && noSources && noState;
                    char text[160];
                    snprintf(text, sizeof(text), "%s(alive dead=%d reasons=%u sources=%d state=%d mt=%s)", ok ? "OK" : "BUG",
                             r.dead ? 1 : 0, uint32(r.reasons), noSources ? 0 : 1, noState ? 0 : 1, TypeName(w));
                    *respawnClean = text;
                    Log("after respawn: %s", text);
                });
                At(5000, [this, gw]()
                {
                    Creature* w = Get(gw); if (!w || !w->IsAlive()) { return; }
                    w->GetMotionMaster()->MovePoint(2, SE.x + 25.0f, SE.y + 10.0f, Ground(SE.x + 25.0f, SE.y + 10.0f, SE.z));
                    Log("point after the respawn, mt=%s", TypeName(w));
                });
                for (uint32 t = 5500; t <= 8000; t += 500)
                {
                    At(t, [this, gw, after, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        after->push_back(p);
                        Log("after +%4ums mt=%s at %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(8500, [this, gw, sourcesHeld, deathDrops, respawnClean, dead, after]()
                {
                    if (!Get(gw)) { Verdict(Invalid("lost")); return; }
                    if (dead->size() < 3 || after->size() < 3) { Verdict(Invalid("no samples")); return; }
                    const float deadSpread = Spread(*dead);
                    const float afterSpread = Spread(*after);
                    const bool respawned = respawnClean->compare(0, 7, "INVALID") != 0;
                    char text[420];
                    snprintf(text, sizeof(text), "sourcesHeld=%s | deathDropsAuraAndScript=%s | deadStands=%s(spread %.1f) | respawnClean=%s | movesAfterRespawn=%s(spread %.1f)",
                             sourcesHeld->c_str(), deathDrops->c_str(),
                             deadSpread < 0.5f ? "OK" : "BUG", deadSpread,
                             respawnClean->c_str(),
                             !respawned ? "INVALID" : (afterSpread > 3.0f ? "OK" : "BUG"), afterSpread);
                    Verdict(text);
                });
            }

        private:
            static std::string Invalid(char const* why)
            {
                std::string w = std::string("INVALID(") + why + ")";
                return "sourcesHeld=" + w + " | deathDropsAuraAndScript=" + w + " | deadStands=" + w + " | respawnClean=" + w + " | movesAfterRespawn=" + w;
            }
        };

        /// S34: a possession by a creature charmer holds the body's own behaviours and lets a
        /// fear play on it (reference 15.4.1-15.4.2; design P5-A 9.2): a point requested before
        /// the possession is held, a fear on the possessed body flees, the release leaves no
        /// reason and a fresh point plays. A creature charmer receives no client root, so the
        /// projection's ordering (the debate's F3) is the live gate's, not this scenario's.
        class PossessedBodyPlaysFear : public Scenario
        {
        public:
            PossessedBodyPlaysFear() : Scenario("possessed-body-plays-fear", 34) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* k = Spawn(KOBOLD, SE.x + 10.0f, SE.y, Ground(SE.x + 10.0f, SE.y, SE.z), 3.1f);
                if (!w || !k) { Verdict(Invalid("spawn failed")); return; }
                const ObjectGuid gw = w->GetObjectGuid(), gk = k->GetObjectGuid();
                auto possessionHolds = std::make_shared<std::string>("INVALID(not reached)");
                auto fearHeld = std::make_shared<std::string>("INVALID(not reached)");
                auto releaseRestores = std::make_shared<std::string>("INVALID(not reached)");
                auto heldPts = std::make_shared<std::vector<Pt> >();
                auto fearPts = std::make_shared<std::vector<Pt> >();
                auto afterPts = std::make_shared<std::vector<Pt> >();
                auto took = std::make_shared<bool>(false);
                At(1000, [this, gw]()
                {
                    if (Creature* w = Get(gw)) { w->GetMotionMaster()->MovePoint(0, SE.x + 25.0f, SE.y, Ground(SE.x + 25.0f, SE.y, SE.z)); Log("point, mt=%s", TypeName(w)); }
                });
                At(2000, [this, gw, gk, took]()
                {
                    Creature* w = Get(gw); Creature* k = Get(gk); if (!w || !k) { return; }
                    *took = k->TakePossessOf(w);
                    Log("kobold possesses the wolf: %d, charmer=%s mt=%s", *took ? 1 : 0, w->GetCharmerGuid().GetString().c_str(), TypeName(w));
                });
                At(2500, [this, gw, gk, took, possessionHolds]()
                {
                    Creature* w = Get(gw); Creature* k = Get(gk); if (!w || !k) { return; }
                    if (!*took) { *possessionHolds = "INVALID(TakePossessOf refused)"; return; }
                    const BlockRead r = ReadBlock(w);
                    const bool ok = w->GetCharmerGuid() == k->GetObjectGuid() && r.possessed && !r.mayMove;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(charmer=%d possessed=%d mayMove=%d)", ok ? "OK" : "BUG",
                             w->GetCharmerGuid() == k->GetObjectGuid() ? 1 : 0, r.possessed ? 1 : 0, r.mayMove ? 1 : 0);
                    *possessionHolds = text;
                    Log("possessed: %s", text);
                });
                for (uint32 t = 2500; t <= 4000; t += 500)
                {
                    At(t, [this, gw, heldPts, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        heldPts->push_back(p);
                        Log("possessed +%4ums mt=%s at %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(4000, [this, gw, gk]()
                {
                    Creature* w = Get(gw); Creature* k = Get(gk); if (!w || !k) { return; }
                    w->SetFeared(true, k->GetObjectGuid(), FEAR, 0, 0);
                    Log("fear on the possessed body, mt=%s", TypeName(w));
                });
                At(4500, [this, gw, fearHeld]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    const bool ok = w->GetMotionMaster()->HoldsControl(Motion::Kind::Fear);
                    *fearHeld = ok ? "OK(the fear claim is held under the possession)" : "BUG(no fear claim on the possessed body)";
                    Log("fear held=%d mt=%s", ok ? 1 : 0, TypeName(w));
                });
                for (uint32 t = 4500; t <= 7000; t += 500)
                {
                    At(t, [this, gw, fearPts, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        fearPts->push_back(p);
                        Log("feared +%4ums mt=%s at %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(7000, [this, gw, gk]()
                {
                    Creature* w = Get(gw); Creature* k = Get(gk); if (!w || !k) { return; }
                    w->SetFeared(false, k->GetObjectGuid(), FEAR, 0, 0);
                    Log("fear ends, mt=%s", TypeName(w));
                });
                At(8000, [this, gk]()
                {
                    if (Creature* k = Get(gk)) { k->ResetControlState(false); Log("kobold releases the wolf"); }
                });
                At(8500, [this, gw, releaseRestores]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    const BlockRead r = ReadBlock(w);
                    const bool ok = !r.possessed && w->GetCharmerGuid().IsEmpty() && r.reasons == 0;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(possessed=%d charmer=%d reasons=%u mt=%s)", ok ? "OK" : "BUG",
                             r.possessed ? 1 : 0, w->GetCharmerGuid().IsEmpty() ? 0 : 1, uint32(r.reasons), TypeName(w));
                    *releaseRestores = text;
                    Log("released: %s", text);
                });
                At(9000, [this, gw]()
                {
                    if (Creature* w = Get(gw)) { w->GetMotionMaster()->MovePoint(1, SE.x - 25.0f, SE.y, Ground(SE.x - 25.0f, SE.y, SE.z)); Log("fresh point after the release, mt=%s", TypeName(w)); }
                });
                for (uint32 t = 9500; t <= 12000; t += 500)
                {
                    At(t, [this, gw, afterPts, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        afterPts->push_back(p);
                        Log("after +%4ums mt=%s at %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(12500, [this, gw, took, possessionHolds, fearHeld, releaseRestores, heldPts, fearPts, afterPts]()
                {
                    if (!Get(gw)) { Verdict(Invalid("lost")); return; }
                    if (!*took) { Verdict(Invalid("TakePossessOf refused")); return; }
                    if (heldPts->size() < 3 || fearPts->size() < 3 || afterPts->size() < 3) { Verdict(Invalid("no samples")); return; }
                    const float held = Spread(*heldPts), fled = Spread(*fearPts), after = Spread(*afterPts);
                    char text[420];
                    snprintf(text, sizeof(text), "possessionHolds=%s | heldUnderPossession=%s(spread %.1f) | fearHeldOnPossessed=%s | fearPlaysOnPossessed=%s(spread %.1f) | releaseRestores=%s | movesAfterRelease=%s(spread %.1f)",
                             possessionHolds->c_str(),
                             held < 0.5f ? "OK" : "BUG", held,
                             fearHeld->c_str(),
                             fled > 3.0f ? "OK" : "BUG", fled,
                             releaseRestores->c_str(),
                             after > 3.0f ? "OK" : "BUG", after);
                    Verdict(text);
                });
            }

        private:
            static std::string Invalid(char const* why)
            {
                std::string w = std::string("INVALID(") + why + ")";
                return "possessionHolds=" + w + " | heldUnderPossession=" + w + " | fearHeldOnPossessed=" + w + " | fearPlaysOnPossessed=" + w + " | releaseRestores=" + w + " | movesAfterRelease=" + w;
            }
        };

        /// S35: two feign auras on one unit are two Dead sources (reference 15.5; the debate's
        /// F5 on PR #84): the first removal keeps the block and the feign flags, the last
        /// removal lifts them and the paused follow resumes. The shape of feign-keeps-follow:
        /// a running leader, a wolf following at 2 yd. Proven for one pair, 29266 + 31261: not
        /// every member of the dummy family coexists with another -- 37493's apply removed 29266
        /// through SetFeignDeath's RemoveAurasWithInterruptFlags(IMMUNE_OR_LOST_SELECTION), the
        /// aura layer's own rule, not the kernel's -- so this is the two-source case, not a
        /// statement about the family.
        class TwoFeignsOneLift : public Scenario
        {
        public:
            TwoFeignsOneLift() : Scenario("two-feigns-one-lift", 35) {}

            void Prepare() override
            {
                Creature* w = Spawn(WOLF, SE.x, SE.y, Ground(SE.x, SE.y, SE.z), 0.0f);
                Creature* leader = Spawn(WOLF, SE.x + 5.0f, SE.y, Ground(SE.x + 5.0f, SE.y, SE.z), 0.0f);
                if (!w || !leader) { Verdict(Invalid("spawn failed")); return; }
                leader->SetWalk(false);
                const ObjectGuid gw = w->GetObjectGuid(), gl = leader->GetObjectGuid();
                auto twoSources = std::make_shared<std::string>("INVALID(not reached)");
                auto firstKeeps = std::make_shared<std::string>("INVALID(not reached)");
                auto lastLifts = std::make_shared<std::string>("INVALID(not reached)");
                auto both = std::make_shared<std::vector<Pt> >();
                auto one = std::make_shared<std::vector<Pt> >();
                auto closed = std::make_shared<float>(999.0f);
                At(500,  [this, gw, gl]() { Creature* w = Get(gw); Creature* l = Get(gl); if (w && l) { w->GetMotionMaster()->MoveFollow(l, 2.0f, 0.0f); Log("follows, mt=%s", TypeName(w)); } });
                At(1000, [this, gl]() { if (Creature* l = Get(gl)) { l->GetMotionMaster()->MovePoint(1, SE.x + 30.0f, SE.y, Ground(SE.x + 30.0f, SE.y, SE.z), true); Log("the leader runs 25 yd"); } });
                At(2000, [this, gw]() { if (Creature* w = Get(gw)) { w->CastSpell(w, FEIGN_A, true); Log("feign A %u: dead sources=%u mt=%s", FEIGN_A, uint32(ReadBlock(w).deadSources), TypeName(w)); } });
                At(2500, [this, gw]() { if (Creature* w = Get(gw)) { w->CastSpell(w, FEIGN_B, true); Log("feign B %u: dead sources=%u mt=%s", FEIGN_B, uint32(ReadBlock(w).deadSources), TypeName(w)); } });
                At(3000, [this, gw, twoSources]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    const BlockRead r = ReadBlock(w);
                    const bool flags = w->HasFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_FEIGN_DEATH) && w->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    if (r.deadSources < 2)
                    {
                        char text[120];
                        snprintf(text, sizeof(text), "INVALID(only %u feign source(s) applied; hasA=%d hasB=%d)", uint32(r.deadSources), w->HasAura(FEIGN_A) ? 1 : 0, w->HasAura(FEIGN_B) ? 1 : 0);
                        *twoSources = text;
                        Log("%s", text);
                        return;
                    }
                    const bool ok = r.deadSources == 2 && r.dead && flags && Type(w) == Motion::Kind::Follow;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(deadSources=%u flags=%d mt=%s)", ok ? "OK" : "BUG", uint32(r.deadSources), flags ? 1 : 0, TypeName(w));
                    *twoSources = text;
                    Log("two feigns: %s", text);
                });
                for (uint32 t = 3000; t <= 4000; t += 500)
                {
                    At(t, [this, gw, both, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        both->push_back(p);
                        Log("two feigns +%4ums mt=%s at %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(4000, [this, gw]() { if (Creature* w = Get(gw)) { w->RemoveAurasDueToSpell(FEIGN_A); Log("feign A removed: dead sources=%u mt=%s", uint32(ReadBlock(w).deadSources), TypeName(w)); } });
                At(4500, [this, gw, firstKeeps]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    const BlockRead r = ReadBlock(w);
                    const bool flags = w->HasFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_FEIGN_DEATH) && w->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    const bool ok = r.dead && r.deadSources == 1 && flags;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(dead=%d deadSources=%u flags=%d)", ok ? "OK" : "BUG", r.dead ? 1 : 0, uint32(r.deadSources), flags ? 1 : 0);
                    *firstKeeps = text;
                    Log("after the first removal: %s", text);
                });
                for (uint32 t = 4500; t <= 6000; t += 500)
                {
                    At(t, [this, gw, one, t]()
                    {
                        Creature* w = Get(gw); if (!w) { return; }
                        Pt p = { w->Where().X(), w->Where().Y(), w->Where().Z() };
                        one->push_back(p);
                        Log("one feign +%4ums mt=%s at %.1f %.1f", t, TypeName(w), p.x, p.y);
                    });
                }
                At(6000, [this, gw]() { if (Creature* w = Get(gw)) { w->RemoveAurasDueToSpell(FEIGN_B); Log("feign B removed: dead sources=%u mt=%s", uint32(ReadBlock(w).deadSources), TypeName(w)); } });
                At(6500, [this, gw, lastLifts]()
                {
                    Creature* w = Get(gw); if (!w) { return; }
                    const BlockRead r = ReadBlock(w);
                    const bool flags = w->HasFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_FEIGN_DEATH) || w->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    const bool ok = !r.dead && r.deadSources == 0 && !flags && Type(w) == Motion::Kind::Follow;
                    char text[120];
                    snprintf(text, sizeof(text), "%s(dead=%d flags=%d mt=%s)", ok ? "OK" : "BUG", r.dead ? 1 : 0, flags ? 1 : 0, TypeName(w));
                    *lastLifts = text;
                    Log("after the last removal: %s", text);
                });
                // The follower matches its leader's own gait (the follow native's walk mirror),
                // and the leader's point leg here runs at its WALK pace, same as the reference
                // feign-keeps-follow (ScenariosBlock.cpp) shape this scenario borrows: closing a
                // ~25 yd gap at that pace takes about 5.5 s once the follow resumes, so the window
                // is the same 8 s that reference uses, not the tighter one first drafted here.
                for (uint32 t = 6500; t <= 14500; t += 500)
                {
                    At(t, [this, gw, gl, closed, t]()
                    {
                        Creature* w = Get(gw); Creature* l = Get(gl); if (!w || !l) { return; }
                        const float d = Dist2(w->Where().X(), w->Where().Y(), l->Where().X(), l->Where().Y());
                        if (d < *closed) { *closed = d; }
                        Log("after +%4ums mt=%s dist=%.1f", t, TypeName(w), d);
                    });
                }
                At(15000, [this, gw, twoSources, firstKeeps, lastLifts, both, one, closed]()
                {
                    if (!Get(gw)) { Verdict(Invalid("lost")); return; }
                    if (twoSources->compare(0, 7, "INVALID") == 0)
                    {
                        std::string why = twoSources->substr(8, twoSources->size() - 9);
                        Verdict(Invalid(why.c_str()));
                        return;
                    }
                    if (both->size() < 2 || one->size() < 3) { Verdict(Invalid("no samples")); return; }
                    const float heldBoth = Spread(*both), heldOne = Spread(*one);
                    char text[420];
                    snprintf(text, sizeof(text), "twoSources=%s | standsUnderTwo=%s(spread %.1f) | firstRemovalKeeps=%s | standsUnderOne=%s(spread %.1f) | lastRemovalLifts=%s | followResumes=%s(closed to %.1f)",
                             twoSources->c_str(),
                             heldBoth < 0.5f ? "OK" : "BUG", heldBoth,
                             firstKeeps->c_str(),
                             heldOne < 0.5f ? "OK" : "BUG", heldOne,
                             lastLifts->c_str(),
                             *closed < 6.0f ? "OK" : "BUG", *closed);
                    Verdict(text);
                });
            }

        private:
            static std::string Invalid(char const* why)
            {
                std::string w = std::string("INVALID(") + why + ")";
                return "twoSources=" + w + " | standsUnderTwo=" + w + " | firstRemovalKeeps=" + w + " | standsUnderOne=" + w + " | lastRemovalLifts=" + w + " | followResumes=" + w;
            }
        };
    }

    void RegisterCoverageScenarios(Runner& r)
    {
        r.Register(new SeatHoldsPassenger());
        r.Register(new DeathDropsSources());
        r.Register(new PossessedBodyPlaysFear());
        r.Register(new TwoFeignsOneLift());
    }
}
