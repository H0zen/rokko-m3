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
#include "ObjectMgr.h"
#include "ScriptMgr.h"
#include "World.h"
#include "Log/Log.h"
#include "Utilities/Errors.h"
#include "Utilities/Util.h"
#include "movement/MoveSpline.h"
#include "movement/MoveSplineInit.h"
#include "PatrolWelding.h"    // the kernel's weld bound (src/motion is on the include path, as BehaviourModel.h is)

namespace
{
    /// A native's continuation may loop within one tick (again = true) without elapsed time
    /// advancing; this bounds it against a policy bug that never converges. The bound covers
    /// the longest run any native can legitimately ask for: every node of a full weld (32, the
    /// kernel's WAYPOINT_SMOOTHING_MAX_LOOKAHEAD), plus the patrol's trailing arrival and the
    /// prepare that drains the phase, with room to spare -- one tick drains any weld.
    constexpr uint32 kMaxContinuation = uint32(Motion::WAYPOINT_SMOOTHING_MAX_LOOKAHEAD) + 8;
}

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
        case Motion::Kind::Wander:         return RANDOM_MOTION_TYPE;
        case Motion::Kind::Patrol:         return WAYPOINT_MOTION_TYPE;
        // The six kinds families 2-4 still own: a native is never one of them, and naming
        // them here makes a kind added later a compile warning instead of a silent Idle.
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
    s.runningState = owner.hasUnitState(UNIT_STAT_RUNNING_STATE);
    s.levitating = owner.IsLevitating();
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
        case Motion::Roaming::SetRoam:   owner.addUnitState(UNIT_STAT_ROAMING); break;
        case Motion::Roaming::SetMove:   owner.addUnitState(UNIT_STAT_ROAMING_MOVE); break;
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
 * @brief Performs a Step's shell operations: stop/interrupt/resetLeg, the roaming write, then
 *        the effects. The generators stopped and cleared their unit-state bits before their
 *        SetWalk, and a waypoint arrival's hook saw ROAMING_MOVE already cleared -- the
 *        effects run after the roaming write and before the intent.
 * @param owner The moving unit.
 * @param step What the native returned.
 */
void NativeBehaviour::PerformOps(Unit& owner, Motion::Step const& step)
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
    PerformEffects(owner, step.effects);
}

/**
 * @brief Applies a Step's intent: the launcher's own arc, or the driver's Apply with the goal
 *        converted out of world coordinates. Only called for a Step that carries `apply`.
 * @param owner The moving unit.
 * @param step What the native returned.
 */
void NativeBehaviour::ApplyIntent(Unit& owner, Motion::Step const& step)
{
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
 * @brief Both halves of a Step, for the hooks (Activate, Suspend, Resume, PerformStep): their
 *        steps carry no intent to re-check, and each acts on the behaviour the facade just chose.
 * @param owner The moving unit.
 * @param step What the native returned.
 */
void NativeBehaviour::Perform(Unit& owner, Motion::Step const& step)
{
    PerformOps(owner, step);
    if (step.apply)
    {
        ApplyIntent(owner, step);
    }
}

/**
 * @brief First selection.
 * @param owner The moving unit.
 */
void NativeBehaviour::Activate(Unit& owner)
{
    m_unit = &owner;
    m_suspended = false;
    m_query.reset();   // a fresh welding pass starts from a fresh router, as the generator's did
    Perform(owner, m_native->Activate(See(owner, false), *this));
}

/**
 * @brief Masked by a higher layer, or paused by the block.
 * @param owner The moving unit.
 */
void NativeBehaviour::Suspend(Unit& owner)
{
    m_unit = &owner;
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
    m_unit = &owner;
    m_suspended = false;
    if (reset)
    {
        m_query.reset();   // re-laying from the spot starts from a fresh router, as Activate does
    }
    Perform(owner, m_native->Resume(See(owner, false), *this, reset));
}

/**
 * @brief One tick of the selected behaviour: a continuation of up to kMaxContinuation rounds.
 *        Each round performs its Step's shell operations exactly once (a Done intent is handled
 *        before they run, since it never applies through the driver), then the selection is
 *        re-checked; `again` loops the continuation at once, otherwise the round's intent is
 *        applied and the tick ends.
 * @param owner The moving unit.
 * @param diff The elapsed update time in milliseconds.
 * @return False when the native asked to be retired.
 */
bool NativeBehaviour::Tick(Unit& owner, uint32 diff)
{
    // No IsSelected re-entrancy guard around the whole tick, unlike the legacy adapter: the
    // continuation below performs its own effects mid-tick and re-checks the selection by
    // sequence after every round, which is the same guard at a finer grain.
    m_unit = &owner;
    const Motion::Sight sight = See(owner, true);
    uint32 elapsed = diff;
    for (uint32 round = 0; round < kMaxContinuation; ++round)
    {
        const Motion::Step step = m_native->Tick(sight, *this, elapsed);
        if (step.apply && step.intent.act == Motion::MoveIntent::Act::Done)
        {
            Roam(owner, step.roaming);
            PerformEffects(owner, step.effects);
            return false;
        }
        PerformOps(owner, step);
        // IntentMovementGenerator::Update re-checked IsSelected(this) after EVERY Intent before
        // applying it, because a hook fired from inside (a waypoint inform) may have replaced the
        // generator. The same check, after every round's effects: a leg laid now would belong to
        // a behaviour that is no longer selected.
        if (!(owner.IsAlive() && owner.IsInWorld() && owner.GetMotionMaster()->IsSelectedSequence(m_seq)))
        {
            return true;   // the effects replaced, suspended or removed us: the generator returned Hold here
        }
        if (step.again)
        {
            elapsed = 0;
            continue;
        }
        if (step.apply)
        {
            ApplyIntent(owner, step);
        }
        return true;
    }
    // The bound covers every round a full weld can ask for, so reaching it is a policy that
    // never converges rather than an ordinary long tick.
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS,
                     "NativeBehaviour: %s kind %u used all %u continuation rounds without applying an intent; its policy did not converge",
                     owner.GetGuidStr().c_str(), uint32(m_native->Kind()), kMaxContinuation);
    return true;
}

/**
 * @brief Why the behaviour ended itself.
 * @param owner The moving unit.
 * @return The native's reason, from the last tick's status and the live spline.
 */
Motion::FinishReason NativeBehaviour::EndReason(Unit& owner) const
{
    // No m_unit write here: this hook takes no Services&, so nothing can read it.
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
    m_unit = &owner;
    PerformOutcome(owner, m_native->Finish(why, See(owner, false), *this));
}

/**
 * @brief The home/reset position a default behaviour answers (the patrol); asks the native.
 * @param owner The moving unit.
 * @param x, y, z, o Filled with the position and its facing when one exists.
 * @return False when the native has none.
 */
bool NativeBehaviour::GetResetPosition(Unit& owner, float& x, float& y, float& z, float& o) const
{
    m_unit = &owner;   // mutable: the port's owner of the moment, not observable state
    // The port itself is still handed over non-const: Services' draws and routes are non-const
    // by contract (a route mutates the router), though a ResetPosition only reads through it.
    NativeBehaviour& self = const_cast<NativeBehaviour&>(*this);
    Motion::Vector3 pos;
    if (!m_native->ResetPosition(self.See(owner, false), self, pos, o))
    {
        return false;
    }
    x = pos.x;
    y = pos.y;
    z = pos.z;
    return true;
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
    PerformEffects(owner, outcome.effects);
}

/**
 * @brief The effects loop, creature-only, in the order given: an Outcome's finishing recipe
 *        or a Step's mid-tick set (the shell performs these before the intent).
 * @param owner The moving unit.
 * @param effects The effects to perform, in order.
 */
void NativeBehaviour::PerformEffects(Unit& owner, std::vector<Motion::Effect> const& effects)
{
    if (effects.empty())
    {
        return;
    }
    if (owner.GetTypeId() != TYPEID_UNIT)
    {
        return;   // every effect below is a creature's (the generators returned here too)
    }
    Creature& creature = static_cast<Creature&>(owner);
    for (size_t i = 0; i < effects.size(); ++i)
    {
        Motion::Effect const& e = effects[i];
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
            case Motion::Effect::InformRaw:
                if (creature.AI())
                {
                    creature.AI()->MovementInform(e.raw, e.id);
                }
                break;
            case Motion::Effect::RunScript:
                creature.GetMap()->ScriptsStart(DBS_ON_CREATURE_MOVEMENT, e.id, &creature, &creature);
                break;
            case Motion::Effect::Emote:
                creature.HandleEmote(e.id);
                break;
            case Motion::Effect::CastSpell:
                creature.CastSpell(&creature, e.id, false);
                break;
            case Motion::Effect::SetDisplay:
                creature.SetDisplayId(e.id);
                break;
            case Motion::Effect::Say:
                if (MangosStringLocale const* textData = sObjectMgr.GetMangosStringLocale(int32(e.id)))
                {
                    creature.MonsterText(textData, NULL);
                }
                else
                {
                    sLog.outErrorDb("%s attempted to do text %i, but required text-data could not be found",
                                     creature.GetGuidStr().c_str(), int32(e.id));
                }
                break;
            case Motion::Effect::ClearEmoteState:
                creature.SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
                break;
            case Motion::Effect::SetWalk:
                creature.SetWalk(e.flag, false);
                break;
            case Motion::Effect::ClearWaypointPaused:
                creature.clearUnitState(UNIT_STAT_WAYPOINT_PAUSED);
                break;
        }
    }
}

// ---- Motion::Services -----------------------------------------------------------------

/**
 * @brief The frame's mover-aware reachable random point; every draw the native makes goes here.
 */
bool NativeBehaviour::RandomPoint(Motion::Vector3 const& centre, float radius, Motion::Vector3& out)
{
    const std::optional<Motion::Vector3> p = Motion::FrameFor(U()).RandomPoint(U(), centre, radius);
    if (!p)
    {
        return false;
    }
    out = *p;
    return true;
}

/**
 * @brief The floor under a point in the mover's frame.
 */
bool NativeBehaviour::Ground(Motion::Vector3 const& at, float& z)
{
    Motion::IMotionFrame const& frame = Motion::FrameFor(U());
    const std::optional<Motion::Vector3> floor = frame.GroundPoint(U(), frame.MoverPosition(U()), at);
    if (!floor)
    {
        return false;
    }
    z = floor->z;
    return true;
}

float NativeBehaviour::Frand(float a, float b) { return frand(a, b); }
uint32 NativeBehaviour::Urand(uint32 a, uint32 b) { return urand(a, b); }
int32 NativeBehaviour::Irand(int32 a, int32 b) { return irand(a, b); }

/**
 * @brief A route in the mover's frame, over the adapter's own router.
 *
 * One router per welding pass, as the generator built one per BuildSmoothPath pass
 * (WaypointMovementGenerator.cpp:366): the native drops it through ResetRoute at the head of
 * every pass, and the legs welded within that pass then share it, exactly as the generator's
 * legs shared the query it had just built. It is rebuilt here as well whenever the frame, the
 * map or the instance changed (the driver's own Query() test, MotionDriver.cpp:56-73), and
 * dropped at every Activate and every Resume(reset).
 */
Motion::RouteResult NativeBehaviour::Route(Motion::Vector3 const& from, Motion::Vector3 const& to, Motion::PointsArray& points)
{
    Motion::RouteResult r;
    Motion::IMotionFrame const& frame = Motion::FrameFor(U());
    if (!m_query || m_queryFrame != frame.Kind() ||
        m_queryMapId != U().GetMapId() || m_queryInstanceId != U().GetInstanceId())
    {
        m_query = frame.CreatePathQuery(U());
        m_queryFrame = frame.Kind();
        m_queryMapId = U().GetMapId();
        m_queryInstanceId = U().GetInstanceId();
    }
    r.usable = m_query->Calculate(from, to, false, 0.0f);
    r.routed = r.usable && m_query->Routed();
    r.partial = m_query->Partial();
    r.progresses = m_query->Progresses();
    if (r.usable)
    {
        points = m_query->Points();
    }
    return r;
}

/**
 * @brief Drops the router: the next route is the first leg of a fresh welding pass.
 */
void NativeBehaviour::ResetRoute() { m_query.reset(); }

bool NativeBehaviour::CanMove() const { return !U().hasUnitState(UNIT_STAT_CAN_NOT_MOVE); }
bool NativeBehaviour::Casting() const { return U().IsNonMeleeSpellCasted(false, false, true); }
bool NativeBehaviour::WaypointPaused() const { return U().hasUnitState(UNIT_STAT_WAYPOINT_PAUSED); }
bool NativeBehaviour::CanFly() const { return U().GetTypeId() == TYPEID_UNIT && static_cast<Creature&>(U()).CanFly(); }

bool NativeBehaviour::Anchor(Motion::Vector3& out) const
{
    if (U().GetTypeId() != TYPEID_UNIT)
    {
        return false;
    }
    Geometry::Vector3 const& a = static_cast<Creature&>(U()).CombatAnchor();
    if (a.x == 0.0f && a.y == 0.0f && a.z == 0.0f)
    {
        return false;
    }
    out = Motion::Vector3(a.x, a.y, a.z);
    return true;
}
