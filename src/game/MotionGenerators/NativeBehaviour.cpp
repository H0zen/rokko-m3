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

#include "NativeBehaviour.h"
#include "MotionMaster.h"
#include "MotionFrame.h"
#include "Unit.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "TemporarySummon.h"
#include "Map.h"
#include "Object.h"
#include "ObjectLookup.h"
#include "World.h"
#include "movement/MoveSpline.h"
#include "movement/MoveSplineInit.h"

/**
 * @brief Constructor: takes the native over, with a driver of its own.
 * @param native The kernel behaviour this adapter drives.
 */
NativeBehaviour::NativeBehaviour(std::unique_ptr<Motion::Behaviour> native)
    : m_native(std::move(native)), m_suspended(false)
{
}

/**
 * @brief Destructor: the native and its driver go with the binding.
 */
NativeBehaviour::~NativeBehaviour()
{
}

/**
 * @brief The legacy movement generator type a kind projects onto.
 * @param kind The kernel kind.
 * @return The type the facade, the scripts and the informs still speak.
 */
MovementGeneratorType NativeBehaviour::Project(Motion::Kind kind)
{
    switch (kind)
    {
        case Motion::Kind::Idle:           return IDLE_MOTION_TYPE;
        case Motion::Kind::Point:          return POINT_MOTION_TYPE;
        case Motion::Kind::FlyLand:        return POINT_MOTION_TYPE;   // the fly/land generator never had a type of its own
        case Motion::Kind::AssistRun:      return ASSISTANCE_MOTION_TYPE;
        case Motion::Kind::Distract:       return DISTRACT_MOTION_TYPE;
        case Motion::Kind::AssistDistract: return ASSISTANCE_DISTRACT_MOTION_TYPE;
        case Motion::Kind::Effect:         return EFFECT_MOTION_TYPE;
        // The eight kinds families 2-4 still own: a native is never one of them, and naming
        // them here makes a kind added later a compile warning instead of a silent Idle.
        case Motion::Kind::Wander:
        case Motion::Kind::Patrol:
        case Motion::Kind::Follow:
        case Motion::Kind::Chase:
        case Motion::Kind::Home:
        case Motion::Kind::Fear:
        case Motion::Kind::Confused:
        case Motion::Kind::Taxi:
        case Motion::Kind::Count:
            break;
    }
    return IDLE_MOTION_TYPE;
}

/**
 * @brief The projection the facade reports for this behaviour.
 * @return The legacy type of the native's kind.
 */
MovementGeneratorType NativeBehaviour::LegacyType() const
{
    return Project(m_native->Kind());
}

/**
 * @brief Everything the native may know about its unit right now.
 * @param owner The moving unit.
 * @param tick True on the behaviour's own tick: the driver's edges are consumed there, once.
 * @return The Sight the native computes from.
 */
Motion::Sight NativeBehaviour::See(Unit& owner, bool tick)
{
    Motion::Sight s;
    if (tick)
    {
        m_last = m_driver.BeginTick(owner);   // consumes the edges: once per tick
    }
    s.status = m_last;
    s.status.traveling = !owner.movespline->Finalized();   // live, for the hooks between ticks
    s.position = Motion::Vector3(owner.Where().X(), owner.Where().Y(), owner.Where().Z());
    s.facing = owner.Where().Facing();
    s.canReact = !owner.hasUnitState(UNIT_STAT_CAN_NOT_REACT | UNIT_STAT_NOT_MOVE);
    s.canMove = !owner.hasUnitState(UNIT_STAT_CAN_NOT_MOVE);
    s.landed = owner.movespline->Finalized() && !owner.movespline->Cut();
    s.alive = owner.IsAlive();
    if (m_native->TracksTarget())
    {
        if (Unit* target = ObjectLookup::GetUnit(owner, ObjectGuid(m_native->Target())))
        {
            float x, y, z;
            // The charge's contact point, the same call EffectCharge makes, so the native's goal
            // is the spell's own answer (SpellEffectObjectCombat.cpp: the target anchors it and
            // the mover is the object placed next to it).
            ContactPointNear(*target, &owner, x, y, z, 3.666666f);
            s.hasTarget = true;
            s.targetPoint = Motion::Vector3(x, y, z);
        }
    }
    return s;
}

/**
 * @brief The roaming pair the point family still mirrors, until a later family retires it.
 * @param owner The moving unit.
 * @param what The write the native asked for.
 */
void NativeBehaviour::Roam(Unit& owner, Motion::Roaming what)
{
    switch (what)
    {
        case Motion::Roaming::SetBoth:   owner.addUnitState(UNIT_STAT_ROAMING | UNIT_STAT_ROAMING_MOVE); break;
        case Motion::Roaming::ClearMove: owner.clearUnitState(UNIT_STAT_ROAMING_MOVE); break;
        case Motion::Roaming::ClearBoth: owner.clearUnitState(UNIT_STAT_ROAMING | UNIT_STAT_ROAMING_MOVE); break;
        case Motion::Roaming::Keep:      break;
    }
}

/**
 * @brief The Effect's spline: the driver lays legs, so an arc is launched here.
 * @param owner The moving unit.
 * @param launch The jump or the fall the native asked for.
 */
void NativeBehaviour::Launch(Unit& owner, Motion::EffectLaunch const& launch)
{
    Movement::MoveSplineInit init(owner);
    init.MoveTo(launch.point.x, launch.point.y, launch.point.z);
    if (launch.kind == Motion::EffectLaunch::Jump)
    {
        init.SetParabolic(launch.height, 0);
        init.SetVelocity(launch.speed);
    }
    else
    {
        init.SetFall();
    }
    switch (launch.facing.mode)
    {
        case Motion::Facing::Mode::Angle:
            init.SetFacing(launch.facing.angle);
            break;
        case Motion::Facing::Mode::Target:
            if (Unit* target = ObjectLookup::GetUnit(owner, ObjectGuid(launch.facing.target)))
            {
                init.SetFacing(target);
            }
            break;
        case Motion::Facing::Mode::Spot:
            init.SetFacing(launch.facing.spot);   // the kernel's Vector3 is Geometry's, the one Movement takes
            break;
        case Motion::Facing::Mode::None:
            break;   // the travel direction, as every launch does today
    }
    init.Launch();
}

/**
 * @brief Performs one Step: the shell operations of the moment, then the intent.
 * @param owner The moving unit.
 * @param step What the native returned.
 */
void NativeBehaviour::Perform(Unit& owner, Motion::Step const& step)
{
    if (step.stop)
    {
        owner.StopMoving();
    }
    if (step.interrupt)
    {
        owner.InterruptMoving();
    }
    if (step.resetLeg)
    {
        m_driver.ResetLeg();
    }
    Roam(owner, step.roaming);
    if (!step.apply)
    {
        return;
    }
    if (step.intent.act == Motion::MoveIntent::Act::Launch)
    {
        Launch(owner, step.intent.launch);   // never handed to the driver: it has no Launch case
        return;
    }
    Motion::MoveIntent intent = step.intent;
    if (intent.act == Motion::MoveIntent::Act::Move)
    {
        // The goal came in world coordinates, as the generator converted it each tick.
        intent.goal = Motion::FrameFor(owner).FromWorld(owner, intent.goal);
    }
    m_driver.Apply(owner, intent);
}

/**
 * @brief First selection.
 * @param owner The moving unit.
 */
void NativeBehaviour::Activate(Unit& owner)
{
    m_suspended = false;
    Perform(owner, m_native->Activate(See(owner, false)));
}

/**
 * @brief Masked by a higher layer, or paused by the block.
 * @param owner The moving unit.
 */
void NativeBehaviour::Suspend(Unit& owner)
{
    if (m_suspended)
    {
        return;   // a block's and a mask's Suspended may both arrive: one interrupt
    }
    m_suspended = true;
    Perform(owner, m_native->Suspend());
}

/**
 * @brief Selected again.
 * @param owner The moving unit.
 * @param reset True for the stack's Reset: re-lay from where the unit stands.
 */
void NativeBehaviour::Resume(Unit& owner, bool reset)
{
    m_suspended = false;
    Perform(owner, m_native->Resume(See(owner, false), reset));
}

/**
 * @brief One tick of the selected behaviour.
 * @param owner The moving unit.
 * @param diff The elapsed update time in milliseconds.
 * @return False when the native asked to be retired.
 */
bool NativeBehaviour::Tick(Unit& owner, uint32 diff)
{
    // No IsSelected re-entrancy guard here, unlike the legacy adapter: a native's tick performs
    // no hook and issues no facade call -- every effect it asks for runs from Finish.
    const Motion::Sight sight = See(owner, true);
    const Motion::Step step = m_native->Tick(sight, diff);
    if (step.apply && step.intent.act == Motion::MoveIntent::Act::Done)
    {
        Roam(owner, step.roaming);
        return false;
    }
    Perform(owner, step);
    return true;
}

/**
 * @brief Why the behaviour ended itself.
 * @param owner The moving unit.
 * @return The native's reason, from the last tick's status and the live spline.
 */
Motion::FinishReason NativeBehaviour::EndReason(Unit& owner) const
{
    Motion::Sight s;
    s.status = m_last;
    s.landed = owner.movespline->Finalized() && !owner.movespline->Cut();
    return m_native->EndReason(s);
}

/**
 * @brief The behaviour is retired: the native's recipe runs.
 * @param owner The moving unit.
 * @param why Why it ended.
 */
void NativeBehaviour::Finish(Unit& owner, Motion::FinishReason why)
{
    PerformOutcome(owner, m_native->Finish(why, See(owner, false)));
}

/**
 * @brief Performs an Outcome in order, its predicates read live.
 * @param owner The moving unit.
 * @param outcome The recipe the native returned.
 */
void NativeBehaviour::PerformOutcome(Unit& owner, Motion::Outcome const& outcome)
{
    Roam(owner, outcome.roaming);
    if (outcome.interrupt && !m_suspended)
    {
        owner.InterruptMoving();   // a suspended behaviour was interrupted at its Suspend
    }
    if (owner.GetTypeId() != TYPEID_UNIT)
    {
        return;   // every effect below is a creature's (the generators returned here too)
    }
    Creature& creature = static_cast<Creature&>(owner);
    for (size_t i = 0; i < outcome.effects.size(); ++i)
    {
        Motion::Effect const& e = outcome.effects[i];
        switch (e.kind)
        {
            case Motion::Effect::Inform:
                if (creature.AI())
                {
                    creature.AI()->MovementInform(Project(e.who), e.id);
                }
                break;
            case Motion::Effect::SummonedInform:
            {
                if (!creature.IsTemporarySummon())
                {
                    break;
                }
                const ObjectGuid summonerGuid = static_cast<TemporarySummon&>(creature).GetSummonerGuid();
                if (!summonerGuid.IsCreature())
                {
                    break;
                }
                if (Creature* summoner = creature.GetMap()->GetCreature(summonerGuid))
                {
                    if (summoner->AI())
                    {
                        summoner->AI()->SummonedMovementInform(&creature, Project(e.who), e.id);
                    }
                }
                break;
            }
            case Motion::Effect::ReengageVictim:
            {
                // Read live, after the inform ran: its AI callback may have installed a chase or a follow.
                if (!owner.IsAlive() || owner.hasUnitState(UNIT_STAT_CONFUSED | UNIT_STAT_FLEEING | UNIT_STAT_NO_COMBAT_MOVEMENT))
                {
                    break;
                }
                if (owner.GetMotionMaster()->IsChasing() || owner.GetMotionMaster()->IsFollowing())
                {
                    break;
                }
                if (Unit* victim = owner.getVictim())
                {
                    owner.GetMotionMaster()->MoveChase(victim);
                }
                break;
            }
            case Motion::Effect::CallAssistance:
                creature.SetNoCallAssistance(false);
                creature.CallAssistance();
                break;
            case Motion::Effect::SeekAssistDistract:
                if (creature.IsAlive())
                {
                    creature.GetMotionMaster()->MoveSeekAssistanceDistract(sWorld.getConfig(CONFIG_UINT32_CREATURE_FAMILY_ASSISTANCE_DELAY));
                }
                break;
            case Motion::Effect::AttackVictim:
                if (Unit* victim = owner.getVictim())
                {
                    if (owner.IsAlive())
                    {
                        owner.AttackStop(true);
                        if (creature.AI())
                        {
                            creature.AI()->AttackStart(victim);
                        }
                    }
                }
                break;
        }
    }
}
