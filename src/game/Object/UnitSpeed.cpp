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

#include "Unit.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "World.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "SpellMgr.h"
#include "QuestDef.h"
#include "Player.h"
#include "Creature.h"
#include "Spell.h"
#include "Group.h"
#include "SpellAuras.h"
#include "MapManager.h"
#include "CreatureAI.h"
#include "TemporarySummon.h"
#include "Formulas.h"
#include "Pet.h"
#include "Util.h"
#include "Totem.h"
#include "Vehicle.h"
#include "BattleGround/BattleGround.h"
#include "InstanceData.h"
#include "OutdoorPvP/OutdoorPvP.h"
#include "MapPersistentStateMgr.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "movement/MoveSplineInit.h"
#include "movement/MoveSpline.h"
#include "CreatureLinkingMgr.h"
#include "GameTime.h"

#include <math.h>
#include <stdarg.h>

// Base movement-speed table; defined in Unit.cpp.
extern float baseMoveSpeed[MAX_MOVE_TYPE];

namespace
{
    /// A feared unit runs a quarter faster for as long as the fear holds.
    ///
    /// Retail, in the Cataclysm 4.0.6a dumps analysed in peer/retail-fear-movement-2026-09-20.md,
    /// sends `SMSG_SPLINE_SET_RUN_SPEED 6.9444 -> 8.6805` inside the fear aura's OWN batch and
    /// `8.6805 -> 6.9444` when the aura is removed: 8.6805 / 6.9444 = 1.25 exactly, on the FINAL
    /// speed. warcraft.wiki.gg says the same in words: "All Fear effects also increase the
    /// afflicted target's run speed by 25%, making it harder to catch feared targets."
    const float kFearRunSpeedFactor = 1.25f;
}

/**
 * @brief Recalculates movement speed for a move type from active modifiers.
 *
 * @param mtype The movement type to update.
 * @param forced True to send a forced speed change packet to players.
 * @param ratio Additional multiplier applied after recalculation.
 */
void Unit::UpdateSpeed(UnitMoveType mtype, bool forced, float ratio, bool ignoreChange)
{
    // not in combat pet have same speed as owner
    switch (mtype)
    {
        case MOVE_RUN:
        case MOVE_WALK:
        case MOVE_SWIM:
            if (GetTypeId() == TYPEID_UNIT && ((Creature*)this)->IsPet() && FollowLatched())
            {
                if (Unit* owner = GetOwner())
                {
                    SetSpeedRate(mtype, owner->GetSpeedRate(mtype), forced, ignoreChange);
                    return;
                }
            }
            break;
        default:
            break;
    }

    int32 main_speed_mod  = 0;
    float stack_bonus     = 1.0f;
    float non_stack_bonus = 1.0f;

    switch (mtype)
    {
        case MOVE_WALK:
            break;
        case MOVE_RUN:
        {
            if (IsMounted()) // Use on mount auras
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS);
                non_stack_bonus = (100.0f + GetMaxPositiveAuraModifier(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK)) / 100.0f;
            }
            else
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_SPEED_ALWAYS);
                non_stack_bonus = (100.0f + GetMaxPositiveAuraModifier(SPELL_AURA_MOD_SPEED_NOT_STACK)) / 100.0f;
            }
            // The fear's own quarter (kFearRunSpeedFactor). It lives HERE, inside the
            // recalculation, and not as a SetSpeedRate poke from Unit::SetFeared: this function
            // rebuilds the rate from scratch on every aura change, so a flat poke would be
            // silently clobbered by the next thing that recalculates and its restore would fight
            // whatever else had moved meanwhile. Computed here instead, it survives every
            // recalculation and composes exactly as an aura-driven modifier would.
            //
            // BOTH candidates are scaled, because `bonus` below is max(non_stack_bonus,
            // stack_bonus): folding the quarter into one of them alone would let a larger
            // modifier of the other kind swallow the fear's bonus through that max. Scaling both
            // by the same positive factor leaves the max's choice alone and lifts its value, so
            // this is exactly `speed *= 1.25` applied at the top of the run-speed arithmetic --
            // above the normalisation cap, the slow, the assistance cut and the creature
            // template's SpeedRun, every one of which is a multiplication that commutes with it
            // (the cap and the slow's minimum-speed floor are the two that do not, and both
            // should bound the boosted speed, not be bounded by it).
            //
            // An AURA fear only, not Blocked(Motion::ReasonFeared). Retail's evidence is
            // entirely about fear EFFECTS -- the dumps' aura batches, and the wiki's "All Fear
            // effects" -- and a mob running away at low health is not one: it is the AI's own
            // Creature::DoFleeToGetAssistance, which reaches Unit::SetFeared with no spell and
            // already cuts its own speed to 0.66. Gating on the reason would give it
            // 0.66 x 1.25 = 0.825 on evidence we do not have, and, because that timed flee
            // expires on its own and never calls SetFeared(false), would leave the quarter
            // hanging on it until something unrelated recalculated. The claim knows which it
            // is, and the claim is what goes away when the flee ends.
            //
            // Read through the shell's published block, as Unit::IsTaxiFlying is: the live
            // arbiter is not readable from every caller of UpdateSpeed, and the answer only has
            // to be right at the end of the commit that lands or lifts the fear -- which is when
            // Unit::SetFeared calls back here.
            if (IsFearedByAura())
            {
                stack_bonus     *= kFearRunSpeedFactor;
                non_stack_bonus *= kFearRunSpeedFactor;
            }
            break;
        }
        case MOVE_RUN_BACK:
            return;
        case MOVE_SWIM:
        {
            main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_SWIM_SPEED);
            break;
        }
        case MOVE_SWIM_BACK:
            return;
        case MOVE_FLIGHT:
        {
            if (IsMounted()) // Use on mount auras
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_FLIGHT_SPEED_MOUNTED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_FLIGHT_SPEED_MOUNTED_STACKING);
                non_stack_bonus = (100.0f + GetMaxPositiveAuraModifier(SPELL_AURA_MOD_FLIGHT_SPEED_MOUNTED_NOT_STACKING)) / 100.0f;
            }
            else             // Use not mount (shapeshift for example) auras (should stack)
            {
                main_speed_mod  = GetTotalAuraModifier(SPELL_AURA_MOD_FLIGHT_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_FLIGHT_SPEED_STACKING);
                non_stack_bonus = (100.0f + GetMaxPositiveAuraModifier(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACKING)) / 100.0f;
            }
            break;
        }
        case MOVE_FLIGHT_BACK:
            return;
        default:
            sLog.outError("Unit::UpdateSpeed: Unsupported move type (%d)", mtype);
            return;
    }

    float bonus = non_stack_bonus > stack_bonus ? non_stack_bonus : stack_bonus;
    // now we ready for speed calculation
    float speed  = main_speed_mod ? bonus * (100.0f + main_speed_mod) / 100.0f : bonus;

    switch (mtype)
    {
        case MOVE_RUN:
        case MOVE_SWIM:
        case MOVE_FLIGHT:
        {
            // Normalize speed by 191 aura SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED if need
            // TODO: possible affect only on MOVE_RUN
            if (int32 normalization = GetMaxPositiveAuraModifier(SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED))
            {
                // Use speed from aura
                float max_speed = normalization / baseMoveSpeed[mtype];
                if (speed > max_speed)
                {
                    speed = max_speed;
                }
            }
            break;
        }
        default:
            break;
    }

    // for creature case, we check explicit if mob searched for assistance
    if (GetTypeId() == TYPEID_UNIT)
    {
        if (((Creature*)this)->HasSearchedAssistance())
        {
            speed *= 0.66f;                                  // best guessed value, so this will be 33% reduction. Based off initial speed, mob can then "run", "walk fast" or "walk".
        }
    }
    // for player case, we look for some custom rates
    else
    {
        if (GetDeathState() == CORPSE)
        {
            speed *= sWorld.getConfig(((Player*)this)->InBattleGround() ? CONFIG_FLOAT_GHOST_RUN_SPEED_BG : CONFIG_FLOAT_GHOST_RUN_SPEED_WORLD);
        }
    }

    // Apply strongest slow aura mod to speed
    int32 slow = GetMaxNegativeAuraModifier(SPELL_AURA_MOD_DECREASE_SPEED);
    if (slow)
    {
        speed *= (100.0f + slow) / 100.0f;
        float min_speed = (float)GetMaxPositiveAuraModifier(SPELL_AURA_MOD_MINIMUM_SPEED) / 100.0f;
        if (speed < min_speed)
        {
            speed = min_speed;
        }
    }

    if (GetTypeId() == TYPEID_UNIT)
    {
        switch (mtype)
        {
            case MOVE_RUN:
                speed *= ((Creature*)this)->GetCreatureInfo()->SpeedRun;
                break;
            case MOVE_WALK:
                speed *= ((Creature*)this)->GetCreatureInfo()->SpeedWalk;
                break;
            default:
                break;
        }
    }

    SetSpeedRate(mtype, speed * ratio, forced, ignoreChange);
}

/**
 * @brief Gets the current movement speed for a move type.
 *
 * @param mtype The movement type.
 * @return The resulting speed value.
 */
float Unit::GetSpeed(UnitMoveType mtype) const
{
    return m_speed_rate[mtype] * baseMoveSpeed[mtype];
}

struct SetSpeedRateHelper
{
    explicit SetSpeedRateHelper(UnitMoveType _mtype, bool _forced, bool _ignoreChange) : mtype(_mtype), forced(_forced), ignoreChange(_ignoreChange) {}
    void operator()(Unit* unit) const { unit->UpdateSpeed(mtype, forced, 1.0f, ignoreChange); }
    UnitMoveType mtype;
    bool forced, ignoreChange;
};

/**
 * @brief Sets the speed rate for a move type and broadcasts the change.
 *
 * @param mtype The movement type.
 * @param rate The new speed rate.
 * @param forced True to use forced speed change handling for players.
 */
void Unit::SetSpeedRate(UnitMoveType mtype, float rate, bool forced, bool ignoreChange)
{
    if (rate < 0)
    {
        rate = 0.0f;
    }

    // Update speed only on change
    if (m_speed_rate[mtype] != rate || ignoreChange)
    {
        m_speed_rate[mtype] = rate;

        PropagateSpeedChange();

        // Design v2 §6.1: the kernel decides the packet. A client-driven unit gets the
        // mover form with a counter and a pending entry the ack closes; a server-driven
        // one the spline form, at once, to everyone. `forced` no longer picks a packet
        // (every player change is negotiated now); it still reaches the controlled
        // units below as it did.
        SendEmissions(m_motion.Apply(Motion::SpeedChange(uint8(mtype), GetSpeed(mtype)), GameTime::GetGameTimeMS()));
    }

    CallForAllControlledUnits(SetSpeedRateHelper(mtype, forced, ignoreChange), CONTROLLED_PET | CONTROLLED_GUARDIANS | CONTROLLED_CHARM | CONTROLLED_MINIPET);
}

/**
 * @brief Applies or removes the feared state.
 *
 * @param apply True to apply fear; false to remove it.
 * @param casterGuid The caster responsible for the effect.
 * @param spellID The spell that caused the effect.
 * @param time The remaining flee duration.
 * @param effIndex The aura's effect index (its claim's identity with the spell and the caster).
 */
void Unit::SetFeared(bool apply, ObjectGuid casterGuid, uint32 spellID, uint32 time, uint8 effIndex)
{
    const uint64 claim = Motion::ControlClaim(spellID, effIndex, casterGuid.GetCounter());
    if (apply)
    {
        if (HasAuraType(SPELL_AURA_PREVENTS_FLEEING))
        {
            return;
        }

        // Nothing lands on a passenger (reference §8.4): the arbiter refuses a control request
        // under a flight, so neither the flag nor the client's revoke is taken for a claim that
        // will never be held; the aura's removal then finds nothing to give back.
        if (GetMotionMaster()->Mobility().reasons & Motion::ReasonOnTaxi)
        {
            return;
        }

        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING);
        CastStop(GetObjectGuid() == casterGuid ? spellID : 0);

        // Control is taken once per episode (design v2 §8), before the flee spline is laid:
        // a second fear or confuse on an already controlled player sends no second revoke.
        if (GetTypeId() == TYPEID_PLAYER &&
            !GetMotionMaster()->HoldsControl(Motion::Kind::Fear) && !GetMotionMaster()->HoldsControl(Motion::Kind::Confused))
        {
            ((Player*)this)->SetClientControl(this, 0);
        }

        Unit* caster = IsInWorld() ? GetMap()->GetUnit(casterGuid) : NULL;

        // Retail's quarter (kFearRunSpeedFactor) needs no call from here. MoveFleeing's own
        // commit publishes the claim, and MotionMaster::Publish recalculates MOVE_RUN whenever
        // the published auraFear changes -- which is the only mechanism, deliberately: a second
        // one here would be right for this route and silently absent for every other, which is
        // the mistake that cost two rounds. It also lands correctly when this function runs
        // inside an already-open facade scope (an inform's callback applying a fear): the
        // published block is not refreshed until the OUTERMOST commit, so a call placed here
        // would read the stale block and compute the unboosted rate, where the publish hook
        // fires at that outermost commit and gets it right.
        GetMotionMaster()->MoveFleeing(caster, time, claim);   // caster==NULL processed in MoveFleeing
    }
    else
    {
        // The quarter goes back with the aura, as the dumps' 8.6805 -> 6.9444 does -- inside
        // this call, but not from a line in it: ReleaseControl's own commit publishes the block
        // without this claim, and MotionMaster::Publish recalculates MOVE_RUN there. That is
        // before everything below, so the end of control lays its chase or its run home at the
        // restored speed; it is correct when a surviving claim is the AI's low-health flee,
        // which is not entitled to the quarter and which the early return below would have
        // skipped; and it needs no line here for the routes that never reach this function at
        // all (the possession take's CancelControl, a full Clear, the death).
        const bool released = GetMotionMaster()->ReleaseControl(claim);

        if (GetMotionMaster()->HoldsControl(Motion::Kind::Fear))
        {
            return;   // another fear drives (reference §3.6): the flag stays, control stays taken
        }

        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING);

        // The end of control runs for the claim that just went: a removal that released
        // nothing (a fear refused at apply, a second prevent-fleeing aura's loop) changes no
        // target and moves nothing.
        if (released && GetTypeId() != TYPEID_PLAYER && IsAlive())
        {
            Creature* c = ((Creature*)this);

            // attack caster if can: the caster becomes the victim before the end rule reads one
            if (Unit* caster = IsInWorld() ? GetMap()->GetUnit(casterGuid) : NULL)
            {
                c->AttackedBy(caster);
            }

            // The end of control (reference §3.1.4, §3.1.6): a victim means the chase resumes
            // (the masked chase through the arbiter while it still aims at the victim, a fresh
            // one otherwise); none means the run home.
            if (Unit* victim = getVictim())
            {
                if (!GetMotionMaster()->IsChasing() || GetMotionMaster()->ChaseTarget() != victim)
                {
                    GetMotionMaster()->MoveChase(victim);
                }
            }
            else
            {
                GetMotionMaster()->MoveTargetedHome();
            }
        }

        // Control returns with the last control aura (P2-D's rule): ReleaseControl's commit
        // published the last claim's end, so the grant passes; a remaining confuse keeps
        // control until its own removal. Not under a taxi: a claim refused under a
        // flight has nothing to give back, and the flight keeps the control until its landing or
        // abort, which grant (P5-B family 5).
        if (GetTypeId() == TYPEID_PLAYER && !GetMotionMaster()->HoldsControl(Motion::Kind::Confused) && !IsTaxiFlying())
        {
            ((Player*)this)->SetClientControl(this, 1);
        }
    }
}

/**
 * @brief Applies or removes the confused state.
 *
 * @param apply True to apply confusion; false to remove it.
 * @param casterGuid The caster responsible for the effect.
 * @param spellID The spell that caused the effect.
 * @param effIndex The aura's effect index (its claim's identity with the spell and the caster).
 */
void Unit::SetConfused(bool apply, ObjectGuid casterGuid, uint32 spellID, uint8 effIndex)
{
    const uint64 claim = Motion::ControlClaim(spellID, effIndex, casterGuid.GetCounter());
    if (apply)
    {
        // As for a fear: refused under a flight, so no flag and no revoke for a claim never held.
        if (GetMotionMaster()->Mobility().reasons & Motion::ReasonOnTaxi)
        {
            return;
        }

        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED);

        CastStop(GetObjectGuid() == casterGuid ? spellID : 0);

        // Control is taken once per episode (design v2 §8), before the wander is laid.
        if (GetTypeId() == TYPEID_PLAYER &&
            !GetMotionMaster()->HoldsControl(Motion::Kind::Fear) && !GetMotionMaster()->HoldsControl(Motion::Kind::Confused))
        {
            ((Player*)this)->SetClientControl(this, 0);
        }

        if (GetTypeId() == TYPEID_UNIT)
        {
            SetTargetGuid(ObjectGuid());
        }
        GetMotionMaster()->MoveConfused(claim);   // players too (reference §3.3.1): server-driven wandering around the spot
    }
    else
    {
        const bool released = GetMotionMaster()->ReleaseControl(claim);
        if (GetMotionMaster()->HoldsControl(Motion::Kind::Confused))
        {
            return;   // another confuse drives: the flag stays, control stays taken
        }

        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED);

        // The end of control runs for the claim that just went: a removal that released
        // nothing changes no target and moves nothing.
        if (released && GetTypeId() != TYPEID_PLAYER && IsAlive())
        {
            // The end of control (reference §3.3.4): a victim means the chase resumes (the masked
            // one while it still aims at the victim, a fresh one otherwise), none the run home.
            if (Unit* victim = getVictim())
            {
                if (!GetMotionMaster()->IsChasing() || GetMotionMaster()->ChaseTarget() != victim)
                {
                    GetMotionMaster()->MoveChase(victim);
                }
            }
            else
            {
                GetMotionMaster()->MoveTargetedHome();
            }
        }

        // As for a fear: not under a taxi, whose landing or abort grants.
        if (GetTypeId() == TYPEID_PLAYER && !GetMotionMaster()->HoldsControl(Motion::Kind::Fear) && !IsTaxiFlying())
        {
            ((Player*)this)->SetClientControl(this, 1);
        }
    }
}

/**
 * @brief Applies or removes feign death state handling.
 *
 * @param apply True to enable feign death; false to clear it.
 * @param casterGuid The caster responsible for the effect.
 * @param spellID The feigning spell, 0 for a caller with no spell of its own (the kernel's
 * block-state identity then falls back to a shared one for that family, spell 5384).
 */
void Unit::SetFeignDeath(bool apply, ObjectGuid casterGuid, uint32 spellID)
{
    // Every aura caller names its own spell and caster (two feign auras on one unit are two
    // sources); a caller with no spell of its own shares the one fallback identity.
    const uint64 source = Motion::ControlClaim(spellID != 0 ? spellID : 5384, 0, casterGuid.GetCounter());

    if (apply)
    {
        /*
        WorldPacket data(SMSG_FEIGN_DEATH_RESISTED, 9);
        data<<GetGUID();
        data<<uint8(0);
        SendMessageToSet(&data,true);
        */

        if (GetTypeId() != TYPEID_PLAYER)
        {
            StopMoving();
        }
        else
        {
            ((Player*)this)->m_movementInfo.SetMovementFlags(MOVEFLAG_NONE);
        }

        // blizz like 2.0.x
        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_UNK_29);
        // blizz like 2.0.x
        SetFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_FEIGN_DEATH);
        // blizz like 2.0.x
        SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);

        GetMotionMaster()->Inhibit(Motion::Inhibition::Dead, source);
        CombatStop();
        GetMotionMaster()->ExpireCombat();   // CombatStop() clears the victim, not the Combat entry; end the chase as combat itself ends
        RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_IMMUNE_OR_LOST_SELECTION);

        // prevent interrupt message
        if (casterGuid == GetObjectGuid())
        {
            FinishSpell(CURRENT_GENERIC_SPELL, false);
        }
        InterruptNonMeleeSpells(true);
        GetHostileRefManager().deleteReferences();
    }
    else
    {
        /*
        WorldPacket data(SMSG_FEIGN_DEATH_RESISTED, 9);
        data<<GetGUID();
        data<<uint8(1);
        SendMessageToSet(&data,true);
        */
        // The block's own lift resumes whatever the feign paused -- a chase in the Combat
        // layer, a follow, a patrol -- from where it stood; nothing here needs to guess it
        // back from combat state.
        GetMotionMaster()->Uninhibit(Motion::Inhibition::Dead, source);

        // The flags follow the last feign: a second feign aura on the same unit keeps them
        // (a real death removes its auras before it inhibits, so this reads feign sources only).
        if (GetMotionMaster()->Inhibited(Motion::Inhibition::Dead))
        {
            return;
        }

        // blizz like 2.0.x
        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_UNK_29);
        // blizz like 2.0.x
        RemoveFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_FEIGN_DEATH);
        // blizz like 2.0.x
        RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
    }
}
