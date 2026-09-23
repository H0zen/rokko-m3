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
#include "HarnessAI.h"
#include "Creature.h"
#include "Pet.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "MotionMaster.h"
#include "Player.h"
#include "PlayerRegistry.h"
#include "WorldSession.h"
#include "World.h"
#include "DBCStores.h"
#include "Log.h"
#include "TaxiRoute.h"
#include "movement/MoveSpline.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// The taxi-contract family (design/2026-09-22-taxi-contract-and-pets-design.md), orders 908-911,
// all four of them player scenarios and therefore at the tail of the 900 block where nothing they
// do can shift a scenario ahead of them.
namespace Harness
{
    namespace
    {
        const Pt P0 = { -3122.6f, -261.3f, 46.0f };   // Mulgore, the plain every family starts on

        // ---- the route ------------------------------------------------------------------
        //
        // TaxiPath 1957, node 402 (Bloodhoof Village, Mulgore) -> node 22 (Thunder Bluff,
        // Mulgore). Read out of the DBC rather than picked for convenience: 21 TaxiPathNode rows,
        // EVERY ONE of them ContinentID 1, 1 488 yd end to end, which is 50 s at the 30 yd/s
        // Movement.TaxiSpeed. All on the harness's own map, so nothing here can make the runner
        // create a second one; and node 402 carries MountCreatureID[0] = 2224, so the mount
        // lookup answers for the harness's human warrior through its alt-team fallback and the
        // takeoff has a real display id to write.
        const uint32 SOURCE_NODE = 402;
        const uint32 DEST_NODE   = 22;

        /// The two-node route as the taxi takes it.
        std::vector<uint32> Route()
        {
            std::vector<uint32> route;
            route.push_back(SOURCE_NODE);
            route.push_back(DEST_NODE);
            return route;
        }

        /// The mount display the takeoff writes, 0 when the DBC or the world database cannot
        /// answer -- which every verdict below reports rather than passing over.
        uint32 TaxiMount(Player* p)
        {
            return sObjectMgr.GetTaxiMountDisplayId(SOURCE_NODE, p->GetTeam(), true);
        }

        /// Both halves of the client's mover membership, as ScenariosControl.cpp reads them.
        bool OwnMover(Player* p)
        {
            WorldSession* s = p->GetSession();
            return s && s->Movers().IsMember(p->GetObjectGuid().GetRawValue()) && p->MoverSession() == s;
        }

        /// The flight put on him through the session's own entry point, which is what
        /// Player::ActivateTaxiPathTo ends with: the route into m_taxi, then SendDoFlight.
        /// The kernel activates on its next update, so the takeoff lands a tick later.
        void Board(Player* p, uint32 startNode = 0)
        {
            p->m_taxi.ClearTaxiDestinations();
            p->m_taxi.AddTaxiDestination(SOURCE_NODE);
            p->m_taxi.AddTaxiDestination(DEST_NODE);
            p->GetSession()->SendDoFlight(TaxiMount(p), p->m_taxi.GetTaxiDestinations(), startNode);
        }

        /// The flight put down the way the flight master's reboarding and the battleground's
        /// port put one down; leaves no taxi state behind.
        void Unboard(Player* p)
        {
            while (p->GetMotionMaster()->IsOnTaxi())
            {
                p->GetMotionMaster()->MovementExpired(false);
            }
            p->m_taxi.ClearTaxiDestinations();
        }

        // ---- the mounts -----------------------------------------------------------------
        //
        // Picked by reading MountCapability.dbc and MountType.dbc against
        // Unit::GetMountCapability, which walks Capability[23] down to Capability[0] and returns
        // the first row whose riding skill, movement state, map, area, aura and spell all pass.
        //
        //   30174 Riding Turtle      mount type 231 = [231, 265]. Capability 265 wants area 5146
        //                            and is skipped on Mulgore, so this resolves to capability
        //                            231: Flags 0x1d -- no 0x2 -- SpeedModSpell 86496 (auras 32
        //                            and 58). A GROUND mount, and every row of its type is
        //                            RequiredRidingSkill 0, so no skill can move it.
        //   32235                    mount type 248 = [226, 227, 241, 242, 243, 238, 239, 240,
        //                            244..252]: a FLYING mount for Azeroth, and the one fixture
        //                            that resolves BOTH ways on this map.
        //                              riding 150, no licence -> capability 227, Flags 0x1d, no
        //                                0x2. Every flying row is gated past it: 250..252 want
        //                                map 646, 247..249 want spell 90267, 244..246 want map
        //                                0, 238..240 want map 571 and aura 54197, 241..243 want
        //                                map 530. THE SPEC'S CASE -- a flying mount where flight
        //                                is forbidden -- and the pet must stay.
        //                              riding 225 and the licence KNOWN -> capability 247,
        //                                Flags 0x7, SpeedModSpell 86459. The pet must go.
        //   90267                    Flight Master's License. It is rows 244..252's
        //                            RequiredSpell, NOT their RequiredAura, so the gate is
        //                            Player::HasSpell and the way to open it is to learn it --
        //                            which is just as well, since 90267's only SpellEffect row
        //                            is Effect 3 with EffectAura 0 and it raises no aura at all.
        //
        // Both mount spells carry 1 500 ms of cast time (SpellCastTimes row 16) and a triggered
        // cast does NOT skip it, so every reading of the applied state is taken two seconds
        // later rather than in the cast's own step.
        const uint32 GROUND_MOUNT = 30174;
        const uint32 GROUNDED_FLYER = 32235;
        const uint32 GROUNDED_FLYER_SKILL = 150;   // capability 227's RequiredRidingSkill
        const uint32 LICENSED_SKILL = 225;         // capability 247's RequiredRidingSkill
        const uint32 LICENSED_CAPABILITY = 247;    // the flying row the zone holds him back from
        const uint32 FLIGHT_LICENCE = 90267;       // its RequiredSpell
        const uint32 IMP = 416;                    // the warlock's imp, the pet S67 builds

        /// The aura's own question, asked the way Aura::HandleAuraMounted asks it: the mount
        /// type from the spell's EffectMiscValueB, then the capability, then `Flags & 0x2`.
        MountCapabilityEntry const* CapabilityOf(Player* p, uint32 spellId)
        {
            SpellEntry const* spell = sSpellStore.LookupEntry(spellId);
            SpellEffectEntry const* eff = spell ? spell->GetSpellEffect(EFFECT_INDEX_0) : NULL;
            return eff ? p->GetMountCapability(uint32(eff->EffectMiscValue_1)) : NULL;
        }

        /// ONE MOVEMENT WORD, DOWN THE HANDLER THE CLIENT'S OWN PACKETS GO DOWN:
        /// WorldSession::HandleMoverRelocation with the flags under test and the place he
        /// already stands, so nothing moves and only the word changes. The mount's lift-off
        /// edge is decided in there and nowhere else, so a scenario that re-implemented the
        /// edge would be proving its own copy of it.
        void Word(Player* p, uint32 flags)
        {
            MovementInfo word = p->m_movementInfo;
            word.SetMovementFlags(MovementFlags(flags));
            word.ChangePosition(p->Where().X(), p->Where().Y(), p->Where().Z(), p->Where().Facing());
            p->GetSession()->HandleMoverRelocation(p, word);
        }
    }

    /**
     * S68 (order 908): a dead passenger must not read as taxi-flying.
     *
     * WHAT THIS PINS, AND WHAT IT CANNOT. The flight IS unwound on the death path today, at the
     * very end of it: Unit::SetDeathState's i_motionMaster.Die() finishes the kernel's Taxi entry
     * and its Abort effect reaches Player::TaxiAbort. What the change moves is the ORDER -- the
     * unwind now runs at the TOP of Player::SetDeathState's JUST_DIED branch, in front of its own
     * RemovePet and in front of everything Unit::SetDeathState does -- and the harness cannot see
     * between two statements of one call. So the end state below reads the same with the change
     * reverted, and this scenario is a CONTRACT PIN, not a regression catcher: it says that a dead
     * passenger carries no taxi state and keeps his own mover, and it would fail loudly if either
     * ever stopped being true. The consequence the ordering actually fixes -- TaxiAbort's
     * ResummonPetTemporaryUnSummonedIfAny landing behind RemovePet, on a body whose m_deathState
     * is still ALIVE, and loading the passenger's pet back out of the database onto his corpse --
     * needs a database-backed pet and is named for the live checklist instead.
     *
     * The death is driven as SetDeathState(JUST_DIED) directly, which is the function under test.
     * Unit::Kill's loot, durability and PvP machinery is deliberately NOT on the path: it would
     * write against a character row that does not exist, and none of it is what this is about.
     */
    class TaxiDeathClearsTheFlight : public Scenario
    {
    public:
        TaxiDeathClearsTheFlight() : Scenario("taxi-death-clears-the-flight", 908) {}
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   boarded = false;      ///< the route went in and SendDoFlight was called
                uint32 mount = 0;            ///< the display id the takeoff had to write
                uint32 upSamples = 0;        ///< samples before the death with the flight whole
                uint32 notUp = 0;            ///< ...of which any that were not
                bool   killRan = false;
                bool   flyingBefore = false; ///< IsTaxiFlying() the instant before the death
                bool   onTaxiBefore = false; ///< the kernel's Taxi entry, likewise
                bool   moverBefore = false;
                uint32 flagsBefore = 0;
                uint32 mountBefore = 0;
                bool   flyingAfter = false;  ///< every reading below taken the instant it returned
                bool   onTaxiAfter = false;
                bool   routeAfter = true;
                uint32 flagsAfter = 0;
                uint32 mountAfter = 0;
                bool   aliveAfter = true;
                bool   moverAfter = false;
                uint32 afterSamples = 0;
                uint32 taxiBack = 0;         ///< samples after the death on which any taxi state stood again
                uint32 moverLost = 0;        ///< ...on which the ghost did not have his own mover
                uint32 aliveAgain = 0;
            };

            Player* p = SpawnPlayer(P0.x, P0.y, Ground(P0.x, P0.y, P0.z), 0.0f);
            if (!p)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            const ObjectGuid g = p->GetObjectGuid();
            auto st = std::make_shared<St>();

            At(500, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->mount = TaxiMount(p);
                Board(p);
                st->boarded = true;
                Log(" 500ms boarded %u -> %u, mount display %u: onTaxi=%d",
                    SOURCE_NODE, DEST_NODE, st->mount, p->GetMotionMaster()->IsOnTaxi() ? 1 : 0);
            });
            for (uint32 t = 700; t < 2500; t += 100)
            {
                At(t, [this, g, st, t]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    ++st->upSamples;
                    const bool whole = p->IsTaxiFlying() &&
                                       p->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT) &&
                                       p->GetMotionMaster()->IsOnTaxi();
                    if (!whole) { ++st->notUp; }
                    if (t % 500 == 0)
                    {
                        Log("%4ums in the air: flying=%d onTaxi=%d flags 0x%08x mount %u own mover=%d at (%.0f, %.0f, %.0f)",
                            t, p->IsTaxiFlying() ? 1 : 0, p->GetMotionMaster()->IsOnTaxi() ? 1 : 0,
                            p->GetUInt32Value(UNIT_FIELD_FLAGS), p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID),
                            OwnMover(p) ? 1 : 0, p->Where().X(), p->Where().Y(), p->Where().Z());
                    }
                });
            }
            At(2500, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->flyingBefore = p->IsTaxiFlying();
                st->onTaxiBefore = p->GetMotionMaster()->IsOnTaxi();
                st->moverBefore = OwnMover(p);
                st->flagsBefore = p->GetUInt32Value(UNIT_FIELD_FLAGS);
                st->mountBefore = p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID);
                // The health first so the body the death leaves behind is a real corpse, then the
                // one call this item changes. EVERY READING BELOW IS TAKEN IN THIS STEP: the
                // whole unwind happens inside SetDeathState and there is no later moment at which
                // the two orderings could still be told apart.
                st->killRan = true;
                p->SetHealth(0);
                p->SetDeathState(JUST_DIED);
                st->flyingAfter = p->IsTaxiFlying();
                st->onTaxiAfter = p->GetMotionMaster()->IsOnTaxi();
                st->routeAfter = !p->m_taxi.empty();
                st->flagsAfter = p->GetUInt32Value(UNIT_FIELD_FLAGS);
                st->mountAfter = p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID);
                st->aliveAfter = p->IsAlive();
                st->moverAfter = OwnMover(p);
                Log("2500ms THE DEATH: flying %d -> %d, onTaxi %d -> %d, flags 0x%08x -> 0x%08x, mount %u -> %u, route left=%d, alive=%d, own mover %d -> %d",
                    st->flyingBefore ? 1 : 0, st->flyingAfter ? 1 : 0,
                    st->onTaxiBefore ? 1 : 0, st->onTaxiAfter ? 1 : 0,
                    st->flagsBefore, st->flagsAfter, st->mountBefore, st->mountAfter,
                    st->routeAfter ? 1 : 0, st->aliveAfter ? 1 : 0,
                    st->moverBefore ? 1 : 0, st->moverAfter ? 1 : 0);
            });
            for (uint32 t = 2600; t <= 5500; t += 100)
            {
                At(t, [this, g, st, t]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    ++st->afterSamples;
                    if (p->IsTaxiFlying() || p->GetMotionMaster()->IsOnTaxi() || !p->m_taxi.empty() ||
                        p->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT) ||
                        p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID))
                    {
                        ++st->taxiBack;
                    }
                    if (!OwnMover(p)) { ++st->moverLost; }
                    if (p->IsAlive()) { ++st->aliveAgain; }
                    if (t % 1000 == 0)
                    {
                        Log("%4ums dead: flying=%d onTaxi=%d route=%d flags 0x%08x mount %u own mover=%d alive=%d",
                            t, p->IsTaxiFlying() ? 1 : 0, p->GetMotionMaster()->IsOnTaxi() ? 1 : 0,
                            p->m_taxi.empty() ? 0 : 1, p->GetUInt32Value(UNIT_FIELD_FLAGS),
                            p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID), OwnMover(p) ? 1 : 0,
                            p->IsAlive() ? 1 : 0);
                    }
                });
            }
            At(6000, [this, st]()
            {
                char flight[352], gone[448], mover[288];
                if (!st->boarded || !st->killRan)
                {
                    snprintf(flight, sizeof(flight), "INVALID(the boarding or the death step never ran: boarded=%d killed=%d)", st->boarded ? 1 : 0, st->killRan ? 1 : 0);
                    snprintf(gone, sizeof(gone), "INVALID(the boarding or the death step never ran)");
                    snprintf(mover, sizeof(mover), "INVALID(the boarding or the death step never ran)");
                }
                else
                {
                    // --- flightUpBeforeTheDeath: the precondition, and the only thing that keeps
                    // the two categories below from passing on a player who never flew.
                    if (!st->mount)
                    {
                        snprintf(flight, sizeof(flight), "INVALID(node %u answered no mount display for this team, so the takeoff had none to write and the flight is not the one the design describes)", SOURCE_NODE);
                    }
                    else if (!st->upSamples)
                    {
                        snprintf(flight, sizeof(flight), "INVALID(no sample was taken between the boarding and the death)");
                    }
                    else if (st->notUp)
                    {
                        snprintf(flight, sizeof(flight), "BUG(the flight was not whole on %u of the %u samples before the death: flying, the two flags and the kernel's Taxi entry are all required)", st->notUp, st->upSamples);
                    }
                    else if (!st->flyingBefore || !st->onTaxiBefore || !st->mountBefore)
                    {
                        snprintf(flight, sizeof(flight), "BUG(the instant before the death he was flying=%d onTaxi=%d mount=%u)", st->flyingBefore ? 1 : 0, st->onTaxiBefore ? 1 : 0, st->mountBefore);
                    }
                    else
                    {
                        snprintf(flight, sizeof(flight), "OK(flying with flags 0x%08x and mount display %u on all %u samples up to the death and the instant before it)", st->flagsBefore, st->mountBefore, st->upSamples);
                    }
                    // --- taxiGoneAtDeath: the contract. Read the instant SetDeathState returned
                    // and on every sample after it.
                    if (!st->flyingBefore || !st->onTaxiBefore)
                    {
                        snprintf(gone, sizeof(gone), "INVALID(he was not flying into the death, so there was no flight for it to unwind)");
                    }
                    else if (st->flyingAfter || st->onTaxiAfter || st->routeAfter ||
                             (st->flagsAfter & (UNIT_FLAG_DISABLE_MOVE | UNIT_FLAG_TAXI_FLIGHT)) || st->mountAfter)
                    {
                        snprintf(gone, sizeof(gone), "BUG(the instant the death returned he still read flying=%d onTaxi=%d route=%d flags 0x%08x mount=%u)",
                                 st->flyingAfter ? 1 : 0, st->onTaxiAfter ? 1 : 0, st->routeAfter ? 1 : 0, st->flagsAfter, st->mountAfter);
                    }
                    else if (st->aliveAfter)
                    {
                        snprintf(gone, sizeof(gone), "BUG(the taxi state went but he was still alive the instant the death returned, so this is not a death)");
                    }
                    else if (st->afterSamples < 10)
                    {
                        snprintf(gone, sizeof(gone), "INVALID(only %u samples after the death)", st->afterSamples);
                    }
                    else if (st->taxiBack || st->aliveAgain)
                    {
                        snprintf(gone, sizeof(gone), "BUG(taxi state stood again on %u and he read alive on %u of the %u samples after the death)", st->taxiBack, st->aliveAgain, st->afterSamples);
                    }
                    else
                    {
                        snprintf(gone, sizeof(gone), "OK(the instant the death returned: not flying, no Taxi entry, no route, flags 0x%08x, mount 0, dead -- and no taxi state on any of the %u samples after it)", st->flagsAfter, st->afterSamples);
                    }
                    // --- ghostKeepsItsMover: what TaxiAbort's own SetClientControl(this, 1) is
                    // for. No death or repop path grants the mover, so a ghost whose takeoff
                    // revoked it and whose unwind did not give it back could not move at all.
                    if (st->moverBefore)
                    {
                        // The takeoff revokes the mover; if he still had it going into the death
                        // there was nothing for the unwind's grant to give back, and an OK below
                        // would be true for the wrong reason.
                        snprintf(mover, sizeof(mover), "INVALID(he still had his own mover while flying, so the takeoff's revoke did not hold and the grant proves nothing)");
                    }
                    else if (!st->moverAfter)
                    {
                        snprintf(mover, sizeof(mover), "BUG(the ghost did not get his mover back the instant the death returned: the takeoff revoked it and nothing on the death or repop path grants it)");
                    }
                    else if (st->moverLost)
                    {
                        snprintf(mover, sizeof(mover), "BUG(the ghost was without his own mover on %u of the %u samples after the death)", st->moverLost, st->afterSamples);
                    }
                    else
                    {
                        snprintf(mover, sizeof(mover), "OK(revoked while flying, back the instant the death returned, and held on all %u samples after it)", st->afterSamples);
                    }
                }
                Verdict(std::string("flightUpBeforeTheDeath=") + flight + " | taxiGoneAtDeath=" + gone +
                        " | ghostKeepsItsMover=" + mover);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "flightUpBeforeTheDeath=" + w + " | taxiGoneAtDeath=" + w + " | ghostKeepsItsMover=" + w;
        }
    };

    /**
     * S69 (order 909): the resume decided by the landing time, not by where the passenger stands.
     *
     * The user's model (design 2026-09-22 §2): a flight is a paid contract to the destination. The
     * scenario drives Player::ContinueTaxiFlight -- the ONE check all three callers come through,
     * the login and the battleground and dungeon returns -- twice over the same route, once with a
     * landing time still ahead and once with one already past, and reads what it did.
     *
     * TIME IS SET, NOT WAITED FOR. The stamp is a server-time second on the route, so putting one
     * in the past or the future is the honest way to ask the question; burning fifty seconds of
     * virtual clock to watch a flight expire would test the clock, not the decision. The decision
     * itself is pinned independently by the suite (TaxiResume_* in MotionTaxiTest.cpp), and the
     * login path is not driven here at all -- it loads from the character database, which the
     * harness has no row in. That gap is named, not papered over.
     */
    class TaxiResumeByLandingTime : public Scenario
    {
    public:
        TaxiResumeByLandingTime() : Scenario("taxi-resume-by-landing-time", 909) {}
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   airRan = false;
                bool   airFlying = false;      ///< a flight was standing after the airborne resume
                bool   airOnTaxi = false;
                uint32 airMount = 0;
                float  airX = 0.0f, airY = 0.0f;   ///< where the resume put him
                float  startX = 0.0f, startY = 0.0f;
                uint32 airSamples = 0;
                uint32 airNotFlying = 0;
                bool   landRan = false;
                bool   landFlying = true;      ///< a flight standing after the landed resume would be the bug
                bool   landOnTaxi = true;
                bool   landRoute = true;
                uint32 landMount = 1;
                float  landX = 0.0f, landY = 0.0f, landZ = 0.0f;
                float  destX = 0.0f, destY = 0.0f, destZ = 0.0f;
                uint32 landDestMap = 0;                     ///< where the landed resume ASKED to be put
                float  landDestX = 0.0f, landDestY = 0.0f, landDestZ = 0.0f;
                bool   landTeleporting = false;             ///< IsBeingTeleportedNear() after it
                uint32 landSamples = 0;
                uint32 landTaxiBack = 0;
                uint32 stampAtTakeoff = 0;     ///< what the takeoff itself wrote
                uint32 nowAtTakeoff = 0;
                uint32 total = 0;              ///< the welded route's flyable length, yards
            };

            Player* p = SpawnPlayer(P0.x, P0.y, Ground(P0.x, P0.y, P0.z), 0.0f);
            if (!p)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            const ObjectGuid g = p->GetObjectGuid();
            auto st = std::make_shared<St>();
            if (TaxiNodesEntry const* dest = sTaxiNodesStore.LookupEntry(DEST_NODE))
            {
                st->destX = dest->Pos_0;
                st->destY = dest->Pos_1;
                st->destZ = dest->Pos_2;
            }
            std::vector<TaxiRouteNode> welded;
            if (TaxiRoute::Weld(Route(), welded))
            {
                st->total = uint32(TaxiResume::Length(welded, 0) + 0.5f);
            }

            // ---- the stamp the takeoff writes ----------------------------------------------
            At(400, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->nowAtTakeoff = uint32(sWorld.GetGameTime());
                Board(p);
                st->stampAtTakeoff = p->m_taxi.GetLandingTime();
                Log(" 400ms the takeoff stamped the landing: now=%u landing=%u (%u s of contract over %u yd)",
                    st->nowAtTakeoff, st->stampAtTakeoff,
                    st->stampAtTakeoff > st->nowAtTakeoff ? st->stampAtTakeoff - st->nowAtTakeoff : 0, st->total);
                Unboard(p);
            });

            // ---- (a) a landing still ahead: back on the mount -------------------------------
            At(900, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->startX = p->Where().X();
                st->startY = p->Where().Y();
                p->m_taxi.ClearTaxiDestinations();
                p->m_taxi.AddTaxiDestination(SOURCE_NODE);
                p->m_taxi.AddTaxiDestination(DEST_NODE);
                // Half the route flown: the contract has as many seconds left as half its length
                // buys at the taxi speed. This is exactly what a short dungeon pop leaves behind.
                const float speed = sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_TAXI_SPEED);
                p->m_taxi.SetLandingTime(uint32(sWorld.GetGameTime()) + uint32(float(st->total) * 0.5f / speed));
                p->ContinueTaxiFlight();
                st->airRan = true;
                st->airFlying = p->IsTaxiFlying();
                st->airOnTaxi = p->GetMotionMaster()->IsOnTaxi();
                st->airMount = p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID);
                st->airX = p->Where().X();
                st->airY = p->Where().Y();
                Log(" 900ms RESUME with the landing ahead: onTaxi=%d flying=%d mount=%u, he moved from (%.0f, %.0f) to (%.0f, %.0f)",
                    st->airOnTaxi ? 1 : 0, st->airFlying ? 1 : 0, st->airMount,
                    st->startX, st->startY, st->airX, st->airY);
            });
            for (uint32 t = 1100; t <= 2400; t += 100)
            {
                At(t, [this, g, st, t]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    ++st->airSamples;
                    if (!p->IsTaxiFlying() || !p->GetMotionMaster()->IsOnTaxi()) { ++st->airNotFlying; }
                    if (t % 500 == 0)
                    {
                        Log("%4ums resumed flight: onTaxi=%d flying=%d at (%.0f, %.0f, %.0f)", t,
                            p->GetMotionMaster()->IsOnTaxi() ? 1 : 0, p->IsTaxiFlying() ? 1 : 0,
                            p->Where().X(), p->Where().Y(), p->Where().Z());
                    }
                });
            }

            // ---- (b) a landing already past: down at the destination -------------------------
            At(2600, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Unboard(p);
                p->m_taxi.AddTaxiDestination(SOURCE_NODE);
                p->m_taxi.AddTaxiDestination(DEST_NODE);
                // A stamp in the past: the fifteen-minute battleground on a fifty-second flight.
                p->m_taxi.SetLandingTime(uint32(sWorld.GetGameTime()) - 60);
                p->ContinueTaxiFlight();
                st->landRan = true;
                st->landFlying = p->IsTaxiFlying();
                st->landOnTaxi = p->GetMotionMaster()->IsOnTaxi();
                st->landRoute = !p->m_taxi.empty();
                st->landMount = p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID);
                st->landX = p->Where().X();
                st->landY = p->Where().Y();
                st->landZ = p->Where().Z();
                // WHERE THE TELEPORT WAS AIMED, which is the reading that survives headless. A
                // same-map TeleportTo hands the move to CMSG_MOVE_TELEPORT_ACK: SendTeleportPacket
                // names the destination to the client and puts the server's own position back
                // (Player.cpp:1679), and a harness session has no socket to ack with. Where()
                // below is therefore still the old spot BY DESIGN, and what the flight was ended
                // AT is read here instead.
                st->landDestMap = p->GetTeleportDest().mapid;
                st->landDestX = p->GetTeleportDest().coord_x;
                st->landDestY = p->GetTeleportDest().coord_y;
                st->landDestZ = p->GetTeleportDest().coord_z;
                st->landTeleporting = p->IsBeingTeleportedNear();
                Log("2600ms RESUME with the landing past: onTaxi=%d flying=%d route=%d mount=%u | teleport asked for map %u (%.0f, %.0f, %.0f), pending=%d; node %u is at (%.0f, %.0f, %.0f) and he still stands at (%.0f, %.0f, %.0f) until a client acks",
                    st->landOnTaxi ? 1 : 0, st->landFlying ? 1 : 0, st->landRoute ? 1 : 0, st->landMount,
                    st->landDestMap, st->landDestX, st->landDestY, st->landDestZ, st->landTeleporting ? 1 : 0,
                    DEST_NODE, st->destX, st->destY, st->destZ, st->landX, st->landY, st->landZ);
            });
            for (uint32 t = 2800; t <= 4000; t += 100)
            {
                At(t, [this, g, st, t]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    ++st->landSamples;
                    if (p->IsTaxiFlying() || p->GetMotionMaster()->IsOnTaxi() || !p->m_taxi.empty() ||
                        p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID))
                    {
                        ++st->landTaxiBack;
                    }
                    if (t % 500 == 0)
                    {
                        Log("%4ums after the landed resume: onTaxi=%d flying=%d route=%d at (%.0f, %.0f, %.0f)", t,
                            p->GetMotionMaster()->IsOnTaxi() ? 1 : 0, p->IsTaxiFlying() ? 1 : 0,
                            p->m_taxi.empty() ? 0 : 1, p->Where().X(), p->Where().Y(), p->Where().Z());
                    }
                });
            }
            At(4300, [this, g]()
            {
                Player* p = sPlayerRegistry.Find(g); if (p) { Unboard(p); }
            });
            At(4500, [this, st]()
            {
                char stamp[352], air[416], land[448];
                if (!st->total)
                {
                    snprintf(stamp, sizeof(stamp), "INVALID(the route %u -> %u did not weld: the DBC has no path for it on this install)", SOURCE_NODE, DEST_NODE);
                    snprintf(air, sizeof(air), "INVALID(the route did not weld)");
                    snprintf(land, sizeof(land), "INVALID(the route did not weld)");
                }
                else
                {
                    // --- takeoffStampsTheLanding: the contract's clock, written once, at the
                    // click. Everything else here is about reading it back.
                    const uint32 speed = uint32(sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_TAXI_SPEED));
                    const uint32 expect = speed ? (st->total + speed / 2) / speed : 0;
                    const uint32 got = st->stampAtTakeoff > st->nowAtTakeoff ? st->stampAtTakeoff - st->nowAtTakeoff : 0;
                    if (!st->stampAtTakeoff)
                    {
                        snprintf(stamp, sizeof(stamp), "BUG(the takeoff wrote no landing time at all, so every resume of this flight would read as landed)");
                    }
                    else if (got + 1 < expect || got > expect + 1)
                    {
                        snprintf(stamp, sizeof(stamp), "BUG(the takeoff stamped %u s of flight for %u yd at %u yd/s, which should be about %u s)", got, st->total, speed, expect);
                    }
                    else
                    {
                        snprintf(stamp, sizeof(stamp), "OK(%u yd of welded route at %u yd/s stamped as %u s of contract, landing at %u)", st->total, speed, got, st->stampAtTakeoff);
                    }
                    // --- resumeAheadOfLandingFlies.
                    if (!st->airRan)
                    {
                        snprintf(air, sizeof(air), "INVALID(the airborne resume step never ran)");
                    }
                    else if (!st->airOnTaxi || !st->airFlying)
                    {
                        snprintf(air, sizeof(air), "BUG(the resume with the landing still ahead did not put him back in the air: onTaxi=%d flying=%d)", st->airOnTaxi ? 1 : 0, st->airFlying ? 1 : 0);
                    }
                    else if (!st->airMount)
                    {
                        snprintf(air, sizeof(air), "BUG(he was flying with no mount display, so the takeoff half of the resume did not run)");
                    }
                    else if (st->airSamples < 10)
                    {
                        snprintf(air, sizeof(air), "INVALID(only %u samples after the airborne resume)", st->airSamples);
                    }
                    else if (st->airNotFlying)
                    {
                        snprintf(air, sizeof(air), "BUG(the resumed flight was down again on %u of the %u samples after it)", st->airNotFlying, st->airSamples);
                    }
                    else
                    {
                        // He resumes FROM WHERE HE STANDS, flying to the node the clock chose --
                        // the design's own named fallback, and the reason there is no distance to
                        // assert here. WHICH node the clock chooses is pinned by the suite
                        // (TaxiResume_BeforeTheLandingIsThePointTheRouteHasReached); what this
                        // proves is that a contract still running puts him back in the air at all.
                        snprintf(air, sizeof(air), "OK(back on the mount with display %u and flying on all %u samples, from where he stood at (%.0f, %.0f))",
                                 st->airMount, st->airSamples, st->airX, st->airY);
                    }
                    // --- resumePastLandingLandsAtTheDestination.
                    if (!st->landRan)
                    {
                        snprintf(land, sizeof(land), "INVALID(the landed resume step never ran)");
                    }
                    else if (st->landOnTaxi || st->landFlying || st->landRoute || st->landMount)
                    {
                        snprintf(land, sizeof(land), "BUG(a flight was standing after a landing time already past: onTaxi=%d flying=%d route=%d mount=%u)",
                                 st->landOnTaxi ? 1 : 0, st->landFlying ? 1 : 0, st->landRoute ? 1 : 0, st->landMount);
                    }
                    else if (!st->landTeleporting)
                    {
                        snprintf(land, sizeof(land), "BUG(the flight ended but no teleport was issued: he was left standing at (%.0f, %.0f) instead of being sent to the destination he paid for)",
                                 st->landX, st->landY);
                    }
                    else
                    {
                        const float off = Dist2(st->landDestX, st->landDestY, st->destX, st->destY);
                        if (st->landDestMap != 1 || off > 5.0f)
                        {
                            snprintf(land, sizeof(land), "BUG(the teleport was aimed at map %u (%.0f, %.0f), %.0f yd from node %u's own position (%.0f, %.0f))",
                                     st->landDestMap, st->landDestX, st->landDestY, off, DEST_NODE, st->destX, st->destY);
                        }
                        else if (st->landSamples < 8 || st->landTaxiBack)
                        {
                            snprintf(land, sizeof(land), "%s(%u samples after the landed resume, taxi state standing again on %u of them)",
                                     st->landTaxiBack ? "BUG" : "INVALID", st->landSamples, st->landTaxiBack);
                        }
                        else
                        {
                            snprintf(land, sizeof(land), "OK(no flight, no route, no mount, and a teleport issued to %.2f yd of node %u's own position on map %u; no taxi state on any of the %u samples after it -- the move itself waits on CMSG_MOVE_TELEPORT_ACK, which a sessionless harness cannot send)",
                                     off, DEST_NODE, st->landDestMap, st->landSamples);
                        }
                    }
                }
                Verdict(std::string("takeoffStampsTheLanding=") + stamp +
                        " | resumeAheadOfLandingFlies=" + air +
                        " | resumePastLandingLandsAtTheDestination=" + land);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "takeoffStampsTheLanding=" + w + " | resumeAheadOfLandingFlies=" + w +
                   " | resumePastLandingLandsAtTheDestination=" + w;
        }
    };

    /**
     * S70 (order 910): the stop retail sends when there is nothing to cancel.
     *
     * The harness has no sessions, so it cannot see the wire. What it CAN see is the writer's own
     * footprint: MoveSplineInit::StopHere ends in move_spline.Initialize(args), so a stop that
     * went out leaves the unit's spline carrying a NEW id and finalized, and one that did not
     * leaves the id exactly where it was. That is the same launched-spline proxy S67 counts legs
     * with, read synchronously on either side of the call instead of sampled.
     *
     * Both operations are driven directly -- Player::TaxiTakeoff and Player::PerformTaxiLanding,
     * the two places the design puts a stop in front of a control change -- because the proxy has
     * to be read on the instant and the kernel's own activation happens between ticks. The BYTES
     * are pinned by the suite instead (MotionWriters_the_point_carrying_stop_*), and the fact that
     * these packets reach a client at all is on the live checklist.
     */
    class TaxiStopsAtTheTransitions : public Scenario
    {
    public:
        TaxiStopsAtTheTransitions() : Scenario("taxi-stops-at-the-transitions", 910) {}
        bool UsesPlayer() const override { return true; }

        void Prepare() override
        {
            struct St
            {
                bool   takeoffRan = false;
                bool   splineBefore = true;   ///< !Finalized() going into the takeoff: must be false
                uint32 idBeforeTakeoff = 0;
                uint32 idAfterTakeoff = 0;
                bool   finalizedAfterTakeoff = false;
                bool   controlBeforeTakeoff = false;
                bool   controlAfterTakeoff = true;
                bool   landingRan = false;
                uint32 idBeforeLanding = 0;
                uint32 idAfterLanding = 0;
                bool   finalizedAfterLanding = false;
                bool   controlAfterLanding = false;
                bool   splineBeforeLanding = true;
            };

            Player* p = SpawnPlayer(P0.x, P0.y, Ground(P0.x, P0.y, P0.z), 0.0f);
            if (!p)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            const ObjectGuid g = p->GetObjectGuid();
            auto st = std::make_shared<St>();

            At(500, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // THE RETAIL SETUP, and the reason the old Stop() sent nothing: the passenger has
                // been standing still under his own control and no server spline has ever been
                // laid for him, so MoveSplineInit::Stop's Finalized() guard returns early.
                st->splineBefore = !p->movespline->Finalized();
                st->idBeforeTakeoff = p->movespline->GetId();
                st->controlBeforeTakeoff = OwnMover(p);
                st->takeoffRan = true;
                p->TaxiTakeoff(TaxiMount(p));
                st->idAfterTakeoff = p->movespline->GetId();
                st->finalizedAfterTakeoff = p->movespline->Finalized();
                st->controlAfterTakeoff = OwnMover(p);
                Log(" 500ms THE TAKEOFF: spline id %u -> %u, finalized=%d, own mover %d -> %d, flags 0x%08x",
                    st->idBeforeTakeoff, st->idAfterTakeoff, st->finalizedAfterTakeoff ? 1 : 0,
                    st->controlBeforeTakeoff ? 1 : 0, st->controlAfterTakeoff ? 1 : 0,
                    p->GetUInt32Value(UNIT_FIELD_FLAGS));
            });
            At(1500, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // The landing's own condition: the flight's spline expired ~180 ms ago, so there
                // is again nothing for a stop to cancel. Nothing has been laid here at all, which
                // is the same thing as far as the guard is concerned.
                st->splineBeforeLanding = !p->movespline->Finalized();
                st->idBeforeLanding = p->movespline->GetId();
                p->ScheduleTaxiLanding(false, p->Where().X(), p->Where().Y(), p->Where().Z(), p->Where().Facing());
                st->landingRan = true;
                p->PerformTaxiLanding();
                st->idAfterLanding = p->movespline->GetId();
                st->finalizedAfterLanding = p->movespline->Finalized();
                st->controlAfterLanding = OwnMover(p);
                Log("1500ms THE LANDING: spline id %u -> %u, finalized=%d, own mover=%d, flags 0x%08x",
                    st->idBeforeLanding, st->idAfterLanding, st->finalizedAfterLanding ? 1 : 0,
                    st->controlAfterLanding ? 1 : 0, p->GetUInt32Value(UNIT_FIELD_FLAGS));
            });
            At(2000, [this, st]()
            {
                char take[416], land[416], order[352];
                if (!st->takeoffRan || !st->landingRan)
                {
                    snprintf(take, sizeof(take), "INVALID(the takeoff or the landing step never ran: takeoff=%d landing=%d)", st->takeoffRan ? 1 : 0, st->landingRan ? 1 : 0);
                    snprintf(land, sizeof(land), "INVALID(the takeoff or the landing step never ran)");
                    snprintf(order, sizeof(order), "INVALID(the takeoff or the landing step never ran)");
                }
                else
                {
                    if (st->splineBefore)
                    {
                        snprintf(take, sizeof(take), "INVALID(a spline was already running into the takeoff, so an ordinary Stop() would have sent one too and this proves nothing)");
                    }
                    else if (st->idAfterTakeoff == st->idBeforeTakeoff)
                    {
                        snprintf(take, sizeof(take), "BUG(the takeoff left the spline id at %u: nothing was written, which is what MoveSplineInit::Stop's Finalized() guard does)", st->idBeforeTakeoff);
                    }
                    else if (!st->finalizedAfterTakeoff)
                    {
                        snprintf(take, sizeof(take), "BUG(the takeoff wrote spline %u but left it running; a stop is a finalized spline)", st->idAfterTakeoff);
                    }
                    else
                    {
                        snprintf(take, sizeof(take), "OK(no spline was running and the takeoff still wrote one: id %u -> %u, finalized)", st->idBeforeTakeoff, st->idAfterTakeoff);
                    }
                    if (st->splineBeforeLanding)
                    {
                        snprintf(land, sizeof(land), "INVALID(a spline was running into the landing, so this is not the expired-flight case the design is about)");
                    }
                    else if (st->idAfterLanding == st->idBeforeLanding)
                    {
                        snprintf(land, sizeof(land), "BUG(the landing left the spline id at %u: nothing was written)", st->idBeforeLanding);
                    }
                    else if (!st->finalizedAfterLanding)
                    {
                        snprintf(land, sizeof(land), "BUG(the landing wrote spline %u but left it running)", st->idAfterLanding);
                    }
                    else
                    {
                        snprintf(land, sizeof(land), "OK(the flight's spline had expired and the landing still wrote one: id %u -> %u, finalized)", st->idBeforeLanding, st->idAfterLanding);
                    }
                    // --- stopThenControl: the order, which is the whole point of the pair. The
                    // stop must be written BEFORE the control changes, in both directions.
                    if (!st->controlBeforeTakeoff)
                    {
                        snprintf(order, sizeof(order), "INVALID(he did not have his own mover going into the takeoff, so the revoke had nothing to take)");
                    }
                    else if (st->controlAfterTakeoff)
                    {
                        snprintf(order, sizeof(order), "BUG(the takeoff did not revoke his mover, so the stop it wrote did not precede a control change)");
                    }
                    else if (!st->controlAfterLanding)
                    {
                        snprintf(order, sizeof(order), "BUG(the landing did not grant his mover back, so the stop it wrote did not precede a control change)");
                    }
                    else
                    {
                        snprintf(order, sizeof(order), "OK(a stop written and then the mover revoked at the takeoff, a stop written and then the mover granted at the landing: retail's (stop, control) mirrored)");
                    }
                }
                Verdict(std::string("takeoffStopsWithNoSpline=") + take +
                        " | landingStopsWithNoSpline=" + land + " | stopThenControl=" + order);
            });
        }

    private:
        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "takeoffStopsWithNoSpline=" + w + " | landingStopsWithNoSpline=" + w +
                   " | stopThenControl=" + w;
        }
    };

    /**
     * S71 (order 911): the 4.2.0 pet rule -- a permanent pet stays out and follows on a ground
     * mount, and goes only on lift-off with a flying one.
     *
     * THE HALF THAT WAS BROKEN IS THE GROUND HALF, which is why it is the half proven end to end
     * here. Unit::Mount's "Flying case" tested the MOUNT SPELL for
     * SPELL_AURA_MOD_FLIGHT_SPEED_MOUNTED, an aura no 4.3.4 mount spell carries -- the speed
     * moved to the capability's SpeedModSpell, where aura 207 lives now -- so the test was dead,
     * every mount fell into the arm below it, and that arm unsummoned any controlled
     * non-temporary pet. The decision now comes from the resolved MountCapabilityEntry, which is
     * already gated on the map, the zone, the riding skill and the licence aura.
     *
     * THE FIXTURE IS ONE MOUNT SPELL RESOLVING BOTH WAYS. Spell 32235 is a flying mount; on map 1
     * with riding skill 150 and no Flight Master's License it resolves to capability 227 (Flags
     * 0x1d, no 0x2) because every flying row of mount type 248 is gated on a map or an aura it
     * does not have, and with skill 225 and spell 90267 applied the same spell resolves to
     * capability 247 (Flags 0x7). That is the design's "verify the capability resolves to a
     * non-flying entry there rather than assuming", shown rather than asserted, and it is what
     * makes phase 2 a real case and not a ground mount wearing a flying name.
     *
     * PHASE 4 AND THE LIFT-OFF (live test 2026-09-22, C4/T7). The user flew with a pet out and
     * reported the half #116 got wrong: a flying mount despawned the pet THE MOMENT IT WAS
     * SUMMONED, where retail keeps it until the rider actually leaves the ground. So the despawn
     * is no longer on the aura's apply at all -- Unit::Mount keeps the pet on both kinds of
     * mount now -- and the trigger moved to WorldSession::HandleMoverRelocation, which fires it
     * on the first movement word that carries MOVEFLAG_FLYING while its sender is mounted.
     * Phase 4 casts the same spell 32235 with the gate from phase 3 still open, so this time it
     * really can fly, and reads three things: the pet is STILL OUT once the mount has landed
     * (the half that was broken), a mounted player's ordinary ground-movement word changes
     * nothing, and the first flying word puts the pet away. Phase 0, before any of it, is the
     * guard: the same flying word on an UNMOUNTED player must do nothing. (The taxi guard needs
     * no arm of its own -- Player::TaxiTakeoff deliberately does not set UNIT_FLAG_MOUNT, so
     * IsMounted() is already false for a passenger, and the taxi's own unsummon and resummon are
     * orders 908-910's business.)
     *
     * WHAT IS NOT PROVEN HERE, AND WHY. No despawn is ever DRIVEN to completion: a real
     * Pet::Unsummon ends in SavePetToDB, which would INSERT a character_pet row for a character
     * that does not exist, and the harness must never write to the character database. Phase 3
     * therefore reads the capability the aura WOULD hand down -- the one input #116 added -- and
     * stops there. Phase 4 goes one step further and drives the DECISION: the pet's owner guid
     * is cleared immediately before the flying word, which closes SavePetToDB's third gate and
     * makes Pet::Unsummon return at its owner check, while PetMgr::UnsummonTemporaryIfAny has
     * already recorded the pet number on the way in. GetTemporaryUnsummonedPetNumber() is
     * therefore the falsifier for the whole rule -- non-zero exactly when the unsummon was
     * reached -- and the pet's own disappearance, and its return on dismount, stay on the live
     * checklist. For the same reason NEITHER negative check may be left to run headless: putting
     * the despawn back on the ground arm, or back on the flying arm's apply, reaches
     * UnsummonPetTemporaryIfAny while the pet still has its owner guid, and that writes the row.
     * (Phase 4's negative check was run once on 2026-09-22 and the row it wrote was deleted.)
     */
    class MountKeepsThePetOnTheGround : public Scenario
    {
    public:
        MountKeepsThePetOnTheGround() : Scenario("mount-keeps-the-pet-on-the-ground", 911) {}
        bool UsesPlayer() const override { return true; }

        /// One mount phase: the capability read the way the aura reads it, then the apply, then
        /// the window under the mount.
        struct Phase
        {
            bool   castRan = false;
            bool   readRan = false;
            uint32 spell = 0;
            uint32 capabilityId = 0;    ///< what GetMountCapability answered here, 0 for none
            uint32 capabilityFlags = 0;
            bool   canFly = false;      ///< the predicate the aura passes down: Flags & 0x2
            bool   petBefore = false;   ///< his own live pet the instant before the cast
            bool   aura = false;        ///< the mount aura landed (these spells take 1.5 s to cast)
            bool   mounted = false;
            bool   petAfter = false;
            uint32 tempNumber = 0;      ///< GetTemporaryUnsummonedPetNumber(): non-zero only if the unsummon arm ran
            uint32 samples = 0;
            uint32 petGone = 0;
            uint32 unmounted = 0;       ///< samples on which the mount was not on him after all
            bool   petAtDismount = false;
        };

        void Prepare() override
        {
            struct St
            {
                bool   built = false;
                uint32 petNumber = 0;
                Phase  ground;      ///< a plain ground mount
                Phase  grounded;    ///< a FLYING mount where flight is forbidden
                bool   gateRan = false;
                bool   licence = false;        ///< spell 90267 known once the gate is opened
                uint32 openedCapability = 0;   ///< what spell 32235 resolves to then
                uint32 openedFlags = 0;
                bool   openedCanFly = false;

                // ---- phase 0 and phase 4: the lift-off ------------------------------------
                bool   afootRan = false;       ///< the unmounted flying word went down the handler
                uint32 afootNumber = 0;        ///< GetTemporaryUnsummonedPetNumber() after it: must be 0
                Phase  flying;                 ///< the SAME mount with the gate open: it really can fly
                bool   liftRan = false;        ///< the two mounted words went down the handler
                bool   mountedAtLift = false;  ///< ...with the mount still on him
                uint32 wordBeforeLift = 0;
                uint32 groundWordNumber = 0;   ///< after a mounted, NOT-flying word: must be 0
                uint32 liftNumber = 0;         ///< after the flying word: must be the pet's number
                uint32 wordAfterLift = 0;
            };

            Player* p = SpawnPlayer(P0.x, P0.y, Ground(P0.x, P0.y, P0.z), 0.0f);
            Pet* pet = p ? BuildPet(p) : NULL;
            if (!p || !pet)
            {
                Verdict(Invalid("spawn failed"));
                return;
            }
            const ObjectGuid g = p->GetObjectGuid(), gp = pet->GetObjectGuid();
            auto st = std::make_shared<St>();
            st->built = true;
            st->petNumber = pet->GetCharmInfo()->GetPetNumber();
            st->ground.spell = GROUND_MOUNT;
            st->grounded.spell = GROUNDED_FLYER;
            Log("the player %s stands at (%.1f, %.1f) with his own pet (entry %u, number %u): IsPet=%d controlled=%d his pet guid=%d",
                g.GetString().c_str(), p->Where().X(), p->Where().Y(), IMP, st->petNumber,
                pet->IsPet() ? 1 : 0, pet->isControlled() ? 1 : 0, p->GetPetGuid() == gp ? 1 : 0);

            // ---- phase 0: the guard, before anything is mounted -----------------------------
            //
            // A flying word from a player who is NOT mounted -- a druid in flight form, a
            // levitating priest, a death knight on a gargoyle -- keeps his pet. The word is then
            // taken back off him so phase 4's flying word is a real EDGE and not a repeat.
            At(100, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Word(p, MOVEFLAG_FLYING);
                st->afootRan = true;
                st->afootNumber = p->GetTemporaryUnsummonedPetNumber();
                Log(" 100ms a FLYING word on an UNMOUNTED player: mounted=%d word 0x%08x, temporary pet number %u",
                    p->IsMounted() ? 1 : 0, uint32(p->m_movementInfo.GetMovementFlags()), st->afootNumber);
            });
            At(200, [this, g]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Word(p, MOVEFLAG_NONE);
                Log(" 200ms the flying word taken back off him: word 0x%08x",
                    uint32(p->m_movementInfo.GetMovementFlags()));
            });

            // ---- phase 1: a plain ground mount --------------------------------------------
            // 1 500 ms of cast time (SpellCastTimes row 16) and a TRIGGERED cast does not skip
            // it -- Spell::Prepare only shortcuts a triggered spell whose timer is already zero
            // -- so every reading of the applied state is taken two seconds later, not in the
            // cast's own step the way an instant spell's would be.
            Cast(500, g, gp, st, &St::ground, "the ground mount");
            ReadApply(2500, g, gp, st, &St::ground);
            Sample(2600, 4500, g, gp, st, &St::ground);
            Pull(4500, g, gp, st, &St::ground);

            // ---- phase 2: a FLYING mount where flight is forbidden --------------------------
            At(4800, [this, g]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // Capability 227's RequiredRidingSkill, as the CURRENT value: GetMountCapability
                // compares against GetSkillValue, not the maximum. Without it mount type 248
                // resolves to nothing at all here and Spell::CheckCast refuses the mount outright
                // (SpellChecks.cpp:1700), which is a different finding from the one under test.
                p->SetSkill(SKILL_RIDING, GROUNDED_FLYER_SKILL, GROUNDED_FLYER_SKILL);
                Log("4800ms riding skill set to %u (current=%u)", GROUNDED_FLYER_SKILL, p->GetSkillValue(SKILL_RIDING));
            });
            Cast(5000, g, gp, st, &St::grounded, "the flying mount where flight is forbidden");
            ReadApply(7000, g, gp, st, &St::grounded);
            Sample(7100, 9000, g, gp, st, &St::grounded);
            Pull(9000, g, gp, st, &St::grounded);

            // ---- phase 3: the SAME mount once the zone stops forbidding flight ---------------
            //
            // Phase 2 only means something if spell 32235 really can fly and was held to the
            // ground by this zone; if its mount type had no flying row reachable here at all, a
            // pet that stayed would be evidence about a ground mount. So the gate is OPENED and
            // the same spell asked again. Nothing is cast and no pet is touched: learning a spell
            // and raising a skill are memory-only on a player the harness never saves.
            At(9300, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->gateRan = true;
                p->SetSkill(SKILL_RIDING, LICENSED_SKILL, LICENSED_SKILL);
                p->learnSpell(FLIGHT_LICENCE, false);
                st->licence = p->HasSpell(FLIGHT_LICENCE);
                Log("9300ms the gate opened: riding %u, %u known = %d",
                    p->GetSkillValue(SKILL_RIDING), FLIGHT_LICENCE, st->licence ? 1 : 0);
            });
            At(9800, [this, g, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                if (MountCapabilityEntry const* cap = CapabilityOf(p, GROUNDED_FLYER))
                {
                    st->openedCapability = cap->ID;
                    st->openedFlags = cap->Flags;
                    st->openedCanFly = (cap->Flags & 0x2) != 0;
                }
                Log("9800ms the same spell asked again: %u now resolves to capability %u flags 0x%02x canFly=%d (it was %u flags 0x%02x)",
                    GROUNDED_FLYER, st->openedCapability, st->openedFlags, st->openedCanFly ? 1 : 0,
                    st->grounded.capabilityId, st->grounded.capabilityFlags);
            });

            // ---- phase 4: the same mount, now airworthy, and the LIFT-OFF -------------------
            //
            // The gate opened at 9300 stays open, so spell 32235 resolves to capability 247
            // (Flags 0x7, the 0x2 set) this time: a mount that really can fly, which is what
            // #116 despawned the pet for at the apply and what retail keeps it through.
            st->flying.spell = GROUNDED_FLYER;
            Cast(10000, g, gp, st, &St::flying, "the SAME flying mount with the gate open");
            ReadApply(12000, g, gp, st, &St::flying);
            Sample(12100, 13000, g, gp, st, &St::flying);
            At(13100, [this, g, gp, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                st->liftRan = true;
                st->mountedAtLift = p->IsMounted();
                st->wordBeforeLift = uint32(p->m_movementInfo.GetMovementFlags());
                // A MOUNTED player walking forward: a movement word with no MOVEFLAG_FLYING in
                // it must not put the pet away, or the rule is "mounting" again by another name.
                Word(p, MOVEFLAG_FORWARD);
                st->groundWordNumber = p->GetTemporaryUnsummonedPetNumber();
                Log("13100ms a mounted GROUND word: mounted=%d word 0x%08x -> 0x%08x, temporary pet number %u",
                    st->mountedAtLift ? 1 : 0, st->wordBeforeLift,
                    uint32(p->m_movementInfo.GetMovementFlags()), st->groundWordNumber);
            });
            At(13400, [this, g, gp, st]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                // SavePetToDB's third gate closed BEFORE the word, not after: the unsummon this
                // word reaches is a real one, and a real one ends in a character_pet write for a
                // character that does not exist. With the owner guid gone the write is refused
                // and Pet::Unsummon returns at its own owner check -- while the pet number has
                // already been recorded on the way in, which is the whole reading.
                if (Pet* pet = FindPet(gp)) { pet->SetOwnerGuid(ObjectGuid()); }
                Word(p, MOVEFLAG_FORWARD | MOVEFLAG_FLYING);
                st->liftNumber = p->GetTemporaryUnsummonedPetNumber();
                st->wordAfterLift = uint32(p->m_movementInfo.GetMovementFlags());
                Log("13400ms THE LIFT-OFF word: word 0x%08x, mounted=%d, temporary pet number %u",
                    st->wordAfterLift, p->IsMounted() ? 1 : 0, st->liftNumber);
            });
            Pull(13700, g, gp, st, &St::flying);

            // ---- the pet released, S67's own recipe: the owner guid FIRST -------------------
            At(14000, [this, g, gp]()
            {
                Player* p = sPlayerRegistry.Find(g);
                Pet* pet = FindPet(gp);
                if (!pet)
                {
                    Log("ERR cleanup: the pet was already gone");
                    return;
                }
                // Closes SavePetToDB's third gate for every path out of here at once, including
                // the ones that are not obvious (Pet::Update unsummoning a pet whose owner it
                // cannot resolve). Nothing this scenario does may reach the character database.
                pet->SetOwnerGuid(ObjectGuid());
                if (p && p->GetPetGuid() == pet->GetObjectGuid()) { p->SetPet(NULL); }
                pet->Unsummon(PET_SAVE_NOT_IN_SLOT);
                Log("14000ms the pet released and unsummoned");
            });
            At(14400, [this, st]()
            {
                char ground[512], forbidden[512], licensed[448];
                if (!st->built)
                {
                    Verdict(Invalid("the pet was not built"));
                    return;
                }
                Read(st->ground, "a plain ground mount", 231, ground, sizeof(ground));
                Read(st->grounded, "a flying mount in a zone that forbids flight", 227, forbidden, sizeof(forbidden));
                // --- theSameMountFliesOnceTheGateOpens: what makes phase 2 a case at all, and
                // the design's "verify the capability resolves to a non-flying entry there
                // rather than assuming" shown rather than asserted. ONE spell, both answers.
                if (!st->gateRan)
                {
                    snprintf(licensed, sizeof(licensed), "INVALID(the gate step never ran)");
                }
                else if (!st->licence)
                {
                    snprintf(licensed, sizeof(licensed), "INVALID(spell %u could not be learned, so the gate was never opened and the comparison cannot be made)", FLIGHT_LICENCE);
                }
                else if (!st->openedCapability)
                {
                    snprintf(licensed, sizeof(licensed), "BUG(with riding %u and %u known, spell %u resolved no capability at all)", LICENSED_SKILL, FLIGHT_LICENCE, GROUNDED_FLYER);
                }
                else if (st->openedCapability != LICENSED_CAPABILITY || !st->openedCanFly)
                {
                    snprintf(licensed, sizeof(licensed), "BUG(with the gate open, spell %u resolved capability %u flags 0x%02x canFly=%d; the DBC rows say %u with 0x2 set)",
                             GROUNDED_FLYER, st->openedCapability, st->openedFlags, st->openedCanFly ? 1 : 0, LICENSED_CAPABILITY);
                }
                else if (st->openedCapability == st->grounded.capabilityId)
                {
                    snprintf(licensed, sizeof(licensed), "BUG(the gate changed nothing: spell %u resolved capability %u both with and without it)", GROUNDED_FLYER, st->openedCapability);
                }
                else
                {
                    snprintf(licensed, sizeof(licensed), "OK(ONE spell, %u, answering both ways on this map: capability %u flags 0x%02x without %u known -- a flying mount held to the ground by the zone, whose pet stayed -- and capability %u flags 0x%02x with it, which can fly. The gate is inside GetMountCapability exactly as the design assumed, so the pet rule never has to know about zones)",
                             GROUNDED_FLYER, st->grounded.capabilityId, st->grounded.capabilityFlags,
                             FLIGHT_LICENCE, st->openedCapability, st->openedFlags);
                }
                // --- phase 4: the half the live test of 2026-09-22 sent back. Three readings,
                // each its own category, because they fail for different reasons.
                char stayed[512], afoot[352], lift[512];
                ReadFlyingApply(*st, stayed, sizeof(stayed));
                if (!st->afootRan)
                {
                    snprintf(afoot, sizeof(afoot), "INVALID(the unmounted-word step never ran)");
                }
                else if (st->afootNumber)
                {
                    snprintf(afoot, sizeof(afoot), "BUG(a FLYING word from an UNMOUNTED player recorded temporary pet number %u: the lift-off rule is firing on anything that flies, not on a mount)", st->afootNumber);
                }
                else
                {
                    snprintf(afoot, sizeof(afoot), "OK(a FLYING word from an unmounted player left the pet alone -- flight form, levitation and a gargoyle are not mounts)");
                }
                ReadLiftOff(*st, lift, sizeof(lift));

                Verdict(std::string("groundMountKeepsThePet=") + ground +
                        " | forbiddenFlightKeepsThePet=" + forbidden +
                        " | theSameMountFliesOnceTheGateOpens=" + licensed +
                        " | flyingMountKeepsThePetUntilLiftOff=" + stayed +
                        " | unmountedFlyingWordKeepsThePet=" + afoot +
                        " | liftOffPutsThePetAway=" + lift);
            });
        }

    private:
        template <class St>
        void Cast(uint32 at, ObjectGuid g, ObjectGuid gp, std::shared_ptr<St> st, Phase St::* which, char const* what)
        {
            At(at, [this, g, gp, st, which, what, at]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Phase& ph = st.get()->*which;
                Pet* pet = FindPet(gp);
                ph.petBefore = pet && pet->IsAlive() && p->GetPetGuid() == gp;
                // Read BEFORE the cast, because this is the answer Aura::HandleAuraMounted
                // resolves and hands to Unit::Mount, and because Spell::CheckCast refuses a
                // mount whose capability is null before any of it happens.
                if (MountCapabilityEntry const* cap = CapabilityOf(p, ph.spell))
                {
                    ph.capabilityId = cap->ID;
                    ph.capabilityFlags = cap->Flags;
                    ph.canFly = (cap->Flags & 0x2) != 0;
                }
                ph.castRan = true;
                SelfCast(p, ph.spell);
                Log("%4ums %s cast (spell %u): capability %u flags 0x%02x canFly=%d, pet his=%d -- 1.5 s of cast time to run",
                    at, what, ph.spell, ph.capabilityId, ph.capabilityFlags, ph.canFly ? 1 : 0,
                    ph.petBefore ? 1 : 0);
            });
        }

        template <class St>
        void ReadApply(uint32 at, ObjectGuid g, ObjectGuid gp, std::shared_ptr<St> st, Phase St::* which)
        {
            At(at, [this, g, gp, st, which, at]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Phase& ph = st.get()->*which;
                Pet* pet = FindPet(gp);
                ph.readRan = true;
                ph.aura = p->HasAura(ph.spell);
                ph.mounted = p->IsMounted();
                ph.tempNumber = p->GetTemporaryUnsummonedPetNumber();
                ph.petAfter = pet && pet->IsAlive() && p->GetPetGuid() == gp;
                Log("%4ums the mount landed: aura=%d mounted=%d display %u | pet his=%d, temporary pet number %u",
                    at, ph.aura ? 1 : 0, ph.mounted ? 1 : 0, p->GetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID),
                    ph.petAfter ? 1 : 0, ph.tempNumber);
            });
        }

        template <class St>
        void Sample(uint32 from, uint32 to, ObjectGuid g, ObjectGuid gp, std::shared_ptr<St> st, Phase St::* which)
        {
            for (uint32 t = from; t < to; t += 100)
            {
                At(t, [this, g, gp, st, which, t]()
                {
                    Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                    Phase& ph = st.get()->*which;
                    Pet* pet = FindPet(gp);
                    ++ph.samples;
                    if (!pet || !pet->IsAlive() || p->GetPetGuid() != gp) { ++ph.petGone; }
                    if (!p->IsMounted()) { ++ph.unmounted; }
                    if (t % 500 == 0)
                    {
                        Log("%4ums mounted=%d pet present=%d his=%d temporary number %u at (%.1f, %.1f)", t,
                            p->IsMounted() ? 1 : 0, pet ? 1 : 0, p->GetPetGuid() == gp ? 1 : 0,
                            p->GetTemporaryUnsummonedPetNumber(),
                            pet ? pet->Where().X() : 0.0f, pet ? pet->Where().Y() : 0.0f);
                    }
                });
            }
        }

        template <class St>
        void Pull(uint32 at, ObjectGuid g, ObjectGuid gp, std::shared_ptr<St> st, Phase St::* which)
        {
            At(at, [this, g, gp, st, which, at]()
            {
                Player* p = sPlayerRegistry.Find(g); if (!p) { return; }
                Phase& ph = st.get()->*which;
                p->RemoveAurasDueToSpell(ph.spell);
                Pet* pet = FindPet(gp);
                ph.petAtDismount = pet && pet->IsAlive() && p->GetPetGuid() == gp;
                Log("%4ums the mount aura %u pulled: mounted=%d pet still his=%d temporary number %u",
                    at, ph.spell, p->IsMounted() ? 1 : 0, ph.petAtDismount ? 1 : 0,
                    p->GetTemporaryUnsummonedPetNumber());
            });
        }

        /// One ground phase's verdict. `expect` is the capability row the DBC says this fixture
        /// must resolve to here; anything else is a broken FIXTURE, reported as INVALID, not a
        /// broken rule.
        void Read(Phase const& ph, char const* what, uint32 expect, char* out, size_t size) const
        {
            if (!ph.castRan || !ph.readRan)
            {
                snprintf(out, size, "INVALID(the %s steps never ran: cast=%d read=%d)", what, ph.castRan ? 1 : 0, ph.readRan ? 1 : 0);
                return;
            }
            if (ph.capabilityId != expect)
            {
                snprintf(out, size, "INVALID(spell %u resolved capability %u here, not the %u the MountCapability rows say for this map, skill and licence: the fixture is not what it claims)",
                         ph.spell, ph.capabilityId, expect);
                return;
            }
            if (ph.canFly)
            {
                snprintf(out, size, "INVALID(capability %u flags 0x%02x reads as flying here, so this is not the ground case it was built to be)",
                         ph.capabilityId, ph.capabilityFlags);
                return;
            }
            if (!ph.petBefore)
            {
                snprintf(out, size, "INVALID(he had no live pet of his own going into %s, so there was nothing for the mount to keep)", what);
                return;
            }
            if (!ph.aura || !ph.mounted)
            {
                snprintf(out, size, "INVALID(the cast of %u left him aura=%d mounted=%d, so Unit::Mount's pet branch was never entered)",
                         ph.spell, ph.aura ? 1 : 0, ph.mounted ? 1 : 0);
                return;
            }
            if (ph.tempNumber)
            {
                snprintf(out, size, "BUG(%s recorded temporary pet number %u: UnsummonPetTemporaryIfAny ran on a GROUND capability (%u, flags 0x%02x) and the pet was despawned)",
                         what, ph.tempNumber, ph.capabilityId, ph.capabilityFlags);
                return;
            }
            if (!ph.petAfter)
            {
                snprintf(out, size, "BUG(the pet was gone once %s had landed, on ground capability %u flags 0x%02x)",
                         what, ph.capabilityId, ph.capabilityFlags);
                return;
            }
            if (ph.samples < 10)
            {
                snprintf(out, size, "INVALID(only %u samples while %s was on him)", ph.samples, what);
                return;
            }
            if (ph.unmounted)
            {
                snprintf(out, size, "INVALID(the mount was off him again on %u of the %u samples, so the window does not measure a mounted player)", ph.unmounted, ph.samples);
                return;
            }
            if (ph.petGone)
            {
                snprintf(out, size, "BUG(the pet was not his live pet on %u of the %u samples while %s was on him)", ph.petGone, ph.samples, what);
                return;
            }
            if (!ph.petAtDismount)
            {
                snprintf(out, size, "BUG(the pet was gone by the time the mount aura was pulled)");
                return;
            }
            snprintf(out, size, "OK(capability %u flags 0x%02x has no 0x2 here, no temporary pet number was recorded, and the pet was his live pet once the mount had landed, on all %u samples under it, and still after the dismount)",
                     ph.capabilityId, ph.capabilityFlags, ph.samples);
        }

        /// Phase 4's first half: a mount that really CAN fly, and a pet that is still out under
        /// it. This is the reading the live test of 2026-09-22 sent back -- #116 despawned here.
        template <class St>
        void ReadFlyingApply(St const& st, char* out, size_t size) const
        {
            Phase const& ph = st.flying;
            if (!ph.castRan || !ph.readRan)
            {
                snprintf(out, size, "INVALID(the flying-mount steps never ran: cast=%d read=%d)", ph.castRan ? 1 : 0, ph.readRan ? 1 : 0);
                return;
            }
            if (ph.capabilityId != LICENSED_CAPABILITY || !ph.canFly)
            {
                snprintf(out, size, "INVALID(with the gate open spell %u resolved capability %u flags 0x%02x canFly=%d here, not the flying %u the MountCapability rows say: this is not the flying case it was built to be)",
                         ph.spell, ph.capabilityId, ph.capabilityFlags, ph.canFly ? 1 : 0, LICENSED_CAPABILITY);
                return;
            }
            if (!ph.petBefore)
            {
                snprintf(out, size, "INVALID(he had no live pet of his own going into the flying mount, so there was nothing for it to keep)");
                return;
            }
            if (!ph.aura || !ph.mounted)
            {
                snprintf(out, size, "INVALID(the cast of %u left him aura=%d mounted=%d, so Unit::Mount's pet branch was never entered)", ph.spell, ph.aura ? 1 : 0, ph.mounted ? 1 : 0);
                return;
            }
            if (ph.tempNumber)
            {
                snprintf(out, size, "BUG(the flying mount recorded temporary pet number %u at its APPLY: the pet was put away for summoning the mount, not for leaving the ground -- capability %u flags 0x%02x)",
                         ph.tempNumber, ph.capabilityId, ph.capabilityFlags);
                return;
            }
            if (!ph.petAfter || ph.petGone)
            {
                snprintf(out, size, "BUG(the pet was not his live pet once the flying mount had landed: at the apply=%d, and gone on %u of the %u samples under it)",
                         ph.petAfter ? 1 : 0, ph.petGone, ph.samples);
                return;
            }
            if (ph.samples < 8 || ph.unmounted)
            {
                snprintf(out, size, "INVALID(%u samples under the flying mount, %u of them with no mount on him)", ph.samples, ph.unmounted);
                return;
            }
            snprintf(out, size, "OK(capability %u flags 0x%02x CAN fly here, and the pet was still his live pet once the mount had landed and on all %u samples under it, with no temporary pet number recorded -- the mount's apply no longer despawns anything)",
                     ph.capabilityId, ph.capabilityFlags, ph.samples);
        }

        /// Phase 4's second half: the edge itself, one word at a time.
        template <class St>
        void ReadLiftOff(St const& st, char* out, size_t size) const
        {
            if (!st.liftRan)
            {
                snprintf(out, size, "INVALID(the lift-off steps never ran)");
                return;
            }
            if (!st.flying.mounted || !st.mountedAtLift)
            {
                snprintf(out, size, "INVALID(he was not mounted when the words went down: at the apply=%d, at the lift-off=%d)", st.flying.mounted ? 1 : 0, st.mountedAtLift ? 1 : 0);
                return;
            }
            if (st.wordBeforeLift & MOVEFLAG_FLYING)
            {
                snprintf(out, size, "INVALID(his word already carried MOVEFLAG_FLYING (0x%08x) before the lift-off, so there was no edge to see)", st.wordBeforeLift);
                return;
            }
            if (st.groundWordNumber)
            {
                snprintf(out, size, "BUG(a mounted GROUND word -- 0x%08x, no MOVEFLAG_FLYING -- recorded temporary pet number %u: the pet goes for being mounted, not for leaving the ground)",
                         st.wordBeforeLift, st.groundWordNumber);
                return;
            }
            if (!(st.wordAfterLift & MOVEFLAG_FLYING))
            {
                snprintf(out, size, "INVALID(the lift-off word did not survive the handler: his word reads 0x%08x, with no MOVEFLAG_FLYING in it)", st.wordAfterLift);
                return;
            }
            if (!st.liftNumber)
            {
                snprintf(out, size, "BUG(the first mounted word carrying MOVEFLAG_FLYING (0x%08x) recorded NO temporary pet number: the pet rides on through the lift-off)", st.wordAfterLift);
                return;
            }
            if (st.liftNumber != st.petNumber)
            {
                snprintf(out, size, "BUG(the lift-off recorded temporary pet number %u, not his pet's %u)", st.liftNumber, st.petNumber);
                return;
            }
            snprintf(out, size, "OK(mounted on a flying mount, a ground word (0x%08x) left the pet alone and the first word carrying MOVEFLAG_FLYING (0x%08x) put pet number %u away -- the despawn is on the lift-off, where 4.2.0 puts it, and no longer on the mount's apply)",
                     st.wordBeforeLift, st.wordAfterLift, st.liftNumber);
        }

        /// HIGHGUID_PET lives in its own store; Scenario::Get answers only HIGHGUID_UNIT.
        Pet* FindPet(ObjectGuid guid) const
        {
            Map* map = GetMap();
            return map ? map->GetPet(guid) : NULL;
        }

        /**
         * A player-owned pet in memory with no `character_pet` row and no write to the character
         * database: Spell::DoSummonPet's own recipe minus its closing SavePetToDB, exactly as
         * S67's player-owned-pet-possession builds one. The order is load-bearing in the same
         * three places: SetOwnerGuid before AIM_Initialize (a player's pet takes the Idle factory
         * default), SetOwnerGuid before InitStatsForLevel (which resolves GetOwner()), and
         * SetActiveObjectState before Map::Add.
         */
        Pet* BuildPet(Player* owner)
        {
            Map* map = GetMap();
            CreatureInfo const* cinfo = ObjectMgr::GetCreatureTemplate(IMP);
            if (!map || !owner || !cinfo)
            {
                Log("ERR pet: no map, no owner, or no creature template %u", IMP);
                return NULL;
            }
            const float x = owner->Where().X() + 4.0f;
            const float y = owner->Where().Y();
            Load(x, y);
            Pet* pet = new Pet(SUMMON_PET);
            CreatureCreatePos pos(map, x, y, Ground(x, y, owner->Where().Z()), 0.0f, 1);
            const uint32 petNumber = sObjectMgr.GeneratePetNumber();
            if (!pet->Create(map->GenerateLocalLowGuid(HIGHGUID_PET), pos, cinfo, petNumber))
            {
                delete pet;
                Log("ERR pet: Pet::Create failed for entry %u", IMP);
                return NULL;
            }
            pet->SetSpawn(pos);
            pet->SetOwnerGuid(owner->GetObjectGuid());
            pet->SetCreatorGuid(owner->GetObjectGuid());
            pet->setFaction(owner->getFaction());
            pet->SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, 0);
            pet->InitStatsForLevel(owner->getLevel());
            pet->GetCharmInfo()->SetPetNumber(petNumber, pet->isControlled());
            pet->GetCharmInfo()->SetReactState(REACT_DEFENSIVE);
            pet->InitPetCreateSpells();
            pet->SetActiveObjectState(true);
            map->Add((Creature*)pet);
            pet->AIM_Initialize();
            // The factory AI dropped from under the recording decorator, as Scenario::Silence
            // does it for a spawned actor: PetAI::UpdateAI draws from urand for its autocast pick
            // and would perturb the seeded stream every other scenario shares.
            pet->SetAI(new HarnessAI(pet, pet->AI(), this));
            if (HarnessAI* recording = dynamic_cast<HarnessAI*>(pet->AI()))
            {
                delete recording->Release();
            }
            owner->SetPet(pet);
            return pet;
        }

        static std::string Invalid(char const* why)
        {
            std::string w = std::string("INVALID(") + why + ")";
            return "groundMountKeepsThePet=" + w + " | forbiddenFlightKeepsThePet=" + w +
                   " | theSameMountFliesOnceTheGateOpens=" + w +
                   " | flyingMountKeepsThePetUntilLiftOff=" + w +
                   " | unmountedFlyingWordKeepsThePet=" + w +
                   " | liftOffPutsThePetAway=" + w;
        }
    };

    void RegisterTaxiScenarios(Runner& r)
    {
        r.Register(new TaxiDeathClearsTheFlight());
        r.Register(new TaxiResumeByLandingTime());
        r.Register(new TaxiStopsAtTheTransitions());
        r.Register(new MountKeepsThePetOnTheGround());
    }
}
