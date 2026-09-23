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
#include "Player.h"
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
#include "movement/MoveSplineSpeed.h" // the one speed-mode selector, shared with the spline launch
#include "PatrolWelding.h"    // the kernel's weld bound (src/motion is on the include path, as BehaviourModel.h is)
#include "TargetKinematics.h" // the kernel's pure velocity classifier

#include <algorithm>

namespace
{
    /// A native's continuation may loop within one tick (again = true) without elapsed time
    /// advancing; this bounds it against a policy bug that never converges. The bound covers
    /// the longest run any native can legitimately ask for: every node of a full weld (32, the
    /// kernel's WAYPOINT_SMOOTHING_MAX_LOOKAHEAD), plus the patrol's trailing arrival and the
    /// prepare that drains the phase, with room to spare -- one tick drains any weld.
    constexpr uint32 kMaxContinuation = uint32(Motion::WAYPOINT_SMOOTHING_MAX_LOOKAHEAD) + 8;

    /// The shell's running state (the old UNIT_STAT_RUNNING_STATE): a chase's or a fear's leg
    /// latched (MotionMaster::LatchBank::RunningLeg), or the creature's RUNNING gait.
    bool RunningState(Unit const& unit)
    {
        return unit.GetMotionMaster()->Latches().RunningLeg() || unit.hasUnitState(UNIT_STAT_RUNNING);
    }

    /// A unit's LIVE placement -- position AND facing -- in its own coordinate space: the
    /// running spline's interpolated point and the heading it carries there, else the placement.
    /// Boarded, a spline's coordinates are seat-local (Unit::CommitSplinePosition) -- which is
    /// exactly what that unit's placement speaks too -- so one expression covers both and never
    /// mixes a deck coordinate with a world one.
    Movement::Location LiveLocation(Unit const& u)
    {
        if (u.movespline->Finalized())
        {
            const Geometry::Vector3 p = u.Where().Pos();
            return Movement::Location(p.x, p.y, p.z, u.Where().Facing());
        }
        return u.movespline->ComputePosition();
    }

    /// Just the point of that placement, for the callers a heading says nothing to.
    Geometry::Vector3 LivePosition(Unit const& u)
    {
        const Movement::Location loc = LiveLocation(u);
        return Geometry::Vector3(loc.x, loc.y, loc.z);
    }

    /// The speed a unit travels at right now: the mode its own movement flags select.
    float SpeedNow(Unit const& u)
    {
        return u.GetSpeed(Movement::SelectSpeedType(u.m_movementInfo.GetMovementFlags()));
    }
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
 * @brief Everything the native may know about its unit right now.
 * @param owner The moving unit.
 * @param tick True on the behaviour's own tick: the driver's edges are consumed there, once.
 * @return The Sight the native computes from.
 */
Motion::Sight NativeBehaviour::See(Unit& owner, bool tick)
{
    Motion::Sight s;
    s.suspended = m_suspended;
    if (tick)
    {
        m_last = m_driver.BeginTick(owner);   // consumes the edges: once per tick
    }
    s.status = m_last;
    s.status.traveling = !owner.movespline->Finalized();   // live, for the hooks between ticks
    s.position = Motion::Vector3(owner.Where().X(), owner.Where().Y(), owner.Where().Z());
    s.facing = owner.Where().Facing();
    s.canReact = !owner.Blocked(Motion::kCannotReactReasons | Motion::kNotMoveReasons) && !owner.IsFeigningDeath();   // the old CAN_NOT_REACT | NOT_MOVE
    s.canMove = !owner.CannotMove();
    s.notMove = owner.Blocked(Motion::kNotMoveReasons) || owner.IsFeigningDeath();   // the old NOT_MOVE
    s.landed = owner.movespline->Finalized() && !owner.movespline->Cut();
    s.alive = owner.IsAlive();
    s.runningState = RunningState(owner);
    s.levitating = owner.IsLevitating();
    s.extent = owner.Where().Extent();
    s.isCreature = owner.GetTypeId() == TYPEID_UNIT;
    s.isPet = s.isCreature && static_cast<Creature&>(owner).IsPet();
    s.combatMovementHeld = owner.hasUnitState(UNIT_STAT_NO_COMBAT_MOVEMENT);
    s.swimming = owner.m_movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING);
    s.canFlyHint = s.isCreature && static_cast<Creature&>(owner).CanFly();
    if (m_native->TracksTarget())
    {
        if (Unit* target = ObjectLookup::GetUnit(owner, ObjectGuid(m_native->Target())))
        {
            SeeTarget(owner, *target, s.target);
            if (m_native->NeedsContactPoint())
            {
                float x, y, z;
                // The charge's contact point, the same call EffectCharge makes, so the native's goal
                // is the spell's own answer (SpellEffectObjectCombat.cpp: the target anchors it and
                // the mover is the object placed next to it) -- now measured from the target's LIVE
                // position rather than from the placement its last relocation recorded, so a charge
                // at a walking target aims where it is (design §6.2). The centre is in the target's
                // own coordinate space, which is what the placement overload reads too. Only the
                // charge reads it, so only the charge pays the free-spot search per tick.
                ContactPointNear(*target, LivePosition(*target), &owner, x, y, z, 3.666666f);
                s.hasTarget = true;
                s.targetPoint = Motion::Vector3(x, y, z);
            }
        }
    }
    m_targetView = s.target;   // the effects decide against the observation the native saw
    return s;
}

/**
 * @brief One coherent observation of a tracked target, in the MOVER's frame.
 * @param owner The moving unit.
 * @param target The resolved target.
 * @param view Filled only when the target is alive, in the world and in the mover's frame;
 *        left invalid otherwise, since a target on another frame is no destination at all.
 */
void NativeBehaviour::SeeTarget(Unit& owner, Unit& target, Motion::TargetView& view) const
{
    if (!target.IsAlive() || !target.IsInWorld() || !owner.Where().ShareFrame(target.Where()))
    {
        return;
    }
    Motion::IMotionFrame const& frame = Motion::FrameFor(owner);
    // A boarded target's spline AND placement are both seat-local, which is already the frame
    // the mover reads -- the same-frame test above excludes every mixed case. An unboarded
    // target's are world, so they come through FromWorld like any other anchor (the identity
    // under the world frame).
    const bool local = target.IsBoarded();
    const bool splineRunning = !target.movespline->Finalized();
    const Movement::Location live = LiveLocation(target);
    const Geometry::Vector3 livePoint(live.x, live.y, live.z);
    view.valid = true;
    view.position = local ? livePoint : frame.FromWorld(owner, livePoint);
    // The facing comes from the same live placement the position did, so the two never
    // disagree: while a spline runs, the heading it carries at that point (its direction of
    // travel, or the facing it was launched with). The PLACEMENT's orientation is only
    // relocated once per POSITION_UPDATE_DELAY, so an angled chase or a follow -- both take
    // their bearing from the leader's facing (TrackingBehaviour::Bearing), which is how a pet
    // holds its follow angle -- aimed at a heading up to 400 ms old, and a leader mid-turn was
    // followed around a corner it had already left. A world facing comes into the frame the
    // way ObjectOrientation brings a placement's; a boarded target's spline facing is already
    // seat-local, exactly as its coordinates are.
    view.facing = splineRunning
                      ? (local ? live.orientation : frame.FacingToFrame(owner, live.orientation))
                      : frame.ObjectOrientation(owner, target);
    view.extent = target.Where().Extent();
    view.reachSum = owner.GetFloatValue(UNIT_FIELD_COMBATREACH) + target.GetFloatValue(UNIT_FIELD_COMBATREACH);
    view.meleeRange = std::max(view.reachSum + 4.0f / 3.0f, 5.0f);
    view.walking = target.IsWalking();
    view.isVictim = owner.getVictim() == &target;

    Motion::TargetMotionInput in;
    if (splineRunning)
    {
        in.splineRunning = true;
        in.splineLinear = !target.movespline->isSmooth();
        in.splineCyclic = target.movespline->isCyclic();
        in.splineAirborne = target.movespline->Airborne();
        in.splineFrom = view.position;
        const Geometry::Vector3 dest = target.movespline->CurrentDestination();
        in.splineTo = local ? dest : frame.FromWorld(owner, dest);
        // A Catmull-Rom bends away from the chord that runs to its current destination, so
        // that chord is not a heading and the curve's own derivative is. Computed only for
        // the curved form: the linear one's chord IS its direction, exactly, and asking the
        // spline again for every tracked target every tick would buy nothing. Not for a
        // cycle or a parabola either, which ClassifyTargetMotion refuses whatever we hand it.
        if (!in.splineLinear && !in.splineCyclic && !in.splineAirborne)
        {
            const Geometry::Vector3 heading = target.movespline->ComputeDirection();
            if (heading.x != 0.0f || heading.y != 0.0f || heading.z != 0.0f)
            {
                // A DIRECTION is not a point, and FromWorld moves points. It is carried in as
                // the difference of two points a yard apart along it, both through the same
                // conversion the destination above uses -- which is what a rotating frame (a
                // deck) would need and what the world frame answers as the identity.
                const Geometry::Vector3 ahead = livePoint + heading;
                in.splineHeading = (local ? ahead : frame.FromWorld(owner, ahead)) - view.position;
                in.splineHeadingValid = true;
            }
        }
        // The SPLINE's own speed, not the unit's for its mode: a charge runs at 24 yd/s and a
        // taxi at the path's, both overrides the movement flags know nothing about.
        in.speed = target.movespline->Velocity();
    }
    else if (target.MoverSession() != NULL)
    {
        // Client-driven: the flags of the last movement packet say where it is heading.
        in.playerMoved = true;
        in.forward = target.m_movementInfo.HasMovementFlag(MOVEFLAG_FORWARD);
        in.backward = target.m_movementInfo.HasMovementFlag(MOVEFLAG_BACKWARD);
        in.strafeLeft = target.m_movementInfo.HasMovementFlag(MOVEFLAG_STRAFE_LEFT);
        in.strafeRight = target.m_movementInfo.HasMovementFlag(MOVEFLAG_STRAFE_RIGHT);
        in.falling = target.m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING) ||
                     target.m_movementInfo.HasMovementFlag(MOVEFLAG_FALLINGFAR);
        in.facing = view.facing;
        in.speed = SpeedNow(target);
    }
    const Motion::TargetMotion motion = Motion::ClassifyTargetMotion(in);
    view.velocity = motion.velocity;
    view.velocityTrusted = motion.trusted;
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
 *        SetWalk, and a waypoint arrival's hook saw the roaming leg latch already cleared -- the
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
    owner.GetMotionMaster()->WriteRoaming(step.roaming);
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
            owner.GetMotionMaster()->WriteRoaming(step.roaming);
            PerformEffects(owner, step.effects);
            return false;
        }
        PerformOps(owner, step);
        // The deleted IntentMovementGenerator::Update re-checked IsSelected(this) after EVERY Intent before
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
 * @brief Performs an Outcome in order, its predicates read live: the roaming write, the
 *        interrupt (unless this behaviour was already suspended), the stop, the effects.
 * @param owner The moving unit.
 * @param outcome The recipe the native returned.
 */
void NativeBehaviour::PerformOutcome(Unit& owner, Motion::Outcome const& outcome)
{
    owner.GetMotionMaster()->WriteRoaming(outcome.roaming);
    if (outcome.interrupt && !m_suspended)
    {
        owner.InterruptMoving();   // a suspended behaviour was interrupted at its Suspend
    }
    if (outcome.stop || outcome.stopForced)
    {
        owner.StopMoving(outcome.stopForced);   // every owner's: the two players' finishes ask for it
    }
    PerformEffects(owner, outcome.effects);
}

/**
 * @brief The effects loop, in the order given: an Outcome's finishing recipe or a Step's
 *        mid-tick set (the shell performs these before the intent). A creature's effect is
 *        skipped for a player owner, per kind (Effect::Owners): a feared or confused player
 *        carries its latches exactly as a creature does, and nothing else of the recipe.
 * @param owner The moving unit.
 * @param effects The effects to perform, in order.
 */
void NativeBehaviour::PerformEffects(Unit& owner, std::vector<Motion::Effect> const& effects)
{
    if (effects.empty())
    {
        return;
    }
    Creature* creaturePtr = owner.GetTypeId() == TYPEID_UNIT ? static_cast<Creature*>(&owner) : NULL;
    const uint8 mine = creaturePtr ? Motion::Effect::OwnerCreature : Motion::Effect::OwnerPlayer;
    for (size_t i = 0; i < effects.size(); ++i)
    {
        Motion::Effect const& e = effects[i];
        if (!(Motion::Effect::Owners(e.kind) & mine))
        {
            continue;   // a creature's effect on a player owner, or a player's on a creature: skipped, as the generators returned before their informs and re-engages
        }
        if (e.kind == Motion::Effect::Latches)
        {
            // The native names its own presence and leg; the facade maps its kind to the
            // channel. Set first, then clear, so a write that does both to one latch ends
            // cleared, as the generators' order did.
            owner.GetMotionMaster()->WriteLatches(m_native->Kind(), e.latchSet, e.latchClear);
            continue;
        }
        if (e.kind == Motion::Effect::WipeLatches)
        {
            // The Home native's first-tick wipe: the latches, the dynamic unit-state bits and
            // the published state (P5-C2; the commit's end publishes again).
            owner.GetMotionMaster()->WipeLatches();
            continue;
        }
        if (!creaturePtr)
        {
            // A player's kinds (Effect::Owners passed it): the taxi's six.
            if (owner.GetTypeId() == TYPEID_PLAYER)
            {
                PerformTaxi(static_cast<Player&>(owner), e);
            }
            continue;
        }
        Creature& creature = *creaturePtr;
        switch (e.kind)
        {
            case Motion::Effect::Inform:
                if (creature.AI())
                {
                    creature.AI()->MovementInform(e.who, e.id);
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
                        summoner->AI()->SummonedMovementInform(&creature, e.who, e.id);
                    }
                }
                break;
            }
            case Motion::Effect::ReengageVictim:
            {
                // The installs are read live, after the inform ran (its AI callback may have installed
                // a chase or a follow); the control state is read as of the last commit, as the old
                // mirrored bits were: a fear the inform applied is not yet seen (inform-fears-mid-outcome).
                if (!owner.IsAlive() || owner.Blocked(Motion::ReasonConfused | Motion::ReasonFeared) ||
                    owner.hasUnitState(UNIT_STAT_NO_COMBAT_MOVEMENT))
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
            case Motion::Effect::PathInform:
                if (creature.AI())
                {
                    creature.AI()->WaypointPathInform(e.raw, e.event, e.id);
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
            case Motion::Effect::Latches:
            case Motion::Effect::WipeLatches:
                break;   // performed above, for every owner
            case Motion::Effect::SyncSpeed:
                // The deleted SyncSpeedWithMaster: only a pet following its OWNER copies its
                // pace, so the guard is the generator's (a pet chasing something else keeps its own).
                if (creature.IsPet() && creature.GetOwnerGuid() == ObjectGuid(m_native->Target()))
                {
                    creature.UpdateSpeed(MOVE_RUN, true);
                    creature.UpdateSpeed(MOVE_WALK, true);
                    creature.UpdateSpeed(MOVE_SWIM, true);
                }
                break;
            case Motion::Effect::EngageInReach:
            {
                // The chase's ReachTarget, decided LIVE: the mover's own interpolated position
                // against the observation the native decided from, in 3D. The native re-emits
                // this on every idle tick, so a false here is never remembered.
                if (!m_targetView.valid)
                {
                    break;
                }
                Unit* target = ObjectLookup::GetUnit(creature, ObjectGuid(m_native->Target()));
                if (!target || (creature.getVictim() == target && creature.hasUnitState(UNIT_STAT_MELEE_ATTACKING)))
                {
                    // Only "already meleeing THIS target" is nothing left to do. Skipping every
                    // victim, as this guard used to, left the legacy ReachTarget's ranged-to-melee
                    // upgrade with no live path at all: ChaseBehaviour holds the whole tick while
                    // the target is NOT the victim (TrackingBehaviour::Tick step 3, LostTarget), so
                    // the only target this effect can ever see is the victim, and Unit::Attack's
                    // `m_attacking == victim && !UNIT_STAT_MELEE_ATTACKING` branch (Unit.cpp) --
                    // which adds the melee bit and sends the attack start -- was unreachable. A
                    // ranged attacker now closes to reach and upgrades here, once: Unit::Attack
                    // returns early with no packet on every later idle tick, since the bit is set.
                    break;
                }
                const Geometry::Vector3 mine = LivePosition(creature);
                const float dx = mine.x - m_targetView.position.x;
                const float dy = mine.y - m_targetView.position.y;
                const float dz = mine.z - m_targetView.position.z;
                if ((dx * dx) + (dy * dy) + (dz * dz) <= m_targetView.meleeRange * m_targetView.meleeRange)
                {
                    creature.Attack(target, true);
                }
                break;
            }
            case Motion::Effect::RestoreTemporaryFaction:
                if (creature.GetTemporaryFactionFlags() & TEMPFACTION_RESTORE_REACH_HOME)
                {
                    creature.ClearTemporaryFaction();
                }
                break;
            case Motion::Effect::LoadAddon:
                creature.LoadCreatureAddon(true);
                break;
            case Motion::Effect::JustReachedHome:
                if (creature.AI())
                {
                    creature.AI()->JustReachedHome();
                }
                break;
            case Motion::Effect::ClearTarget:
                creature.SetTargetGuid(ObjectGuid());   // the flee's creature initialisation: a panicking creature faces no one
                break;
            case Motion::Effect::ClearFleeingFlag:
                creature.RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING);   // the timed flee's: it has no aura to clear the client-visible flag
                break;
            case Motion::Effect::RestoreGait:
                // Read LIVE, at this place in the recipe: after the interrupt's clear on a
                // displacing finish, before the native's own clear on the untimed Finalize.
                creature.SetWalk(!RunningState(creature), false);
                break;
            case Motion::Effect::TaxiTakeoff:
            case Motion::Effect::TaxiEvent:
            case Motion::Effect::TaxiSeam:
            case Motion::Effect::TaxiCross:
            case Motion::Effect::TaxiLand:
            case Motion::Effect::TaxiAbort:
                break;   // a player's kinds: never reach a creature (Effect::Owners)
        }
    }
}

/**
 * @brief The taxi's six operations, a player's alone: the native says when, Player::Taxi* say how.
 * @param player The passenger.
 * @param e The effect; a kind that is not the taxi's is nothing here (the owner rule passed it,
 *        so only the six reach this; the default names that subset on purpose).
 */
void NativeBehaviour::PerformTaxi(Player& player, Motion::Effect const& e)
{
    switch (e.kind)
    {
        case Motion::Effect::TaxiTakeoff:
            player.TaxiTakeoff(e.id);
            break;
        case Motion::Effect::TaxiEvent:
            DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "Taxi %s event %u for player %s",
                             e.flag ? "departure" : "arrival", e.id, player.GetName());
            StartEvents_Event(player.GetMap(), e.id, &player, &player, e.flag);
            break;
        case Motion::Effect::TaxiSeam:
            player.TaxiSeamPassed();
            break;
        case Motion::Effect::TaxiCross:
            if (!player.TaxiCross(e.raw, e.point.x, e.point.y, e.point.z, e.angle))
            {
                // The map cannot be entered: the flight ends where the mover is (TaxiAbort).
                player.GetMotionMaster()->MovementExpired(false);
            }
            break;
        case Motion::Effect::TaxiLand:
            player.ScheduleTaxiLanding(e.flag, e.point.x, e.point.y, e.point.z, e.angle);
            break;
        case Motion::Effect::TaxiAbort:
            player.TaxiAbort();
            break;
        default:
            break;   // a dispatcher over the six: every other kind is a creature's and never reaches it
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
    Motion::Vector3 p;
    if (!GroundPoint(at, p))
    {
        return false;
    }
    z = p.z;
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

bool NativeBehaviour::CanMove() const { return !U().CannotMove(); }
bool NativeBehaviour::Casting() const { return U().IsNonMeleeSpellCasted(false, false, true); }
bool NativeBehaviour::WaypointPaused() const { return U().hasUnitState(UNIT_STAT_WAYPOINT_PAUSED); }
bool NativeBehaviour::CanFly() const { return U().GetTypeId() == TYPEID_UNIT && static_cast<Creature&>(U()).CanFly(); }

/**
 * @brief The frame's free-spot search around an explicit centre, with the collision selector.
 *
 * The centre is the NATIVE's -- the target's live position, or one it has led -- rather than the
 * target object's placement, which is what makes a derived spot actually move with its target.
 * The target still anchors the search (its map, its phase, its frame, the grid area the
 * neighbours come from), so it is resolved again here and the spot is refused when it is gone.
 */
bool NativeBehaviour::StandingSpot(Motion::Vector3 const& center, float distance2d, float absAngle, Motion::Vector3& out)
{
    Unit* target = ObjectLookup::GetUnit(U(), ObjectGuid(m_native->Target()));
    // The same three conditions SeeTarget demands before it fills the view: a dead, unloaded or
    // differently framed anchor would send the search through another frame's grid and hand back
    // a point in coordinates the mover does not speak. No spot at all is the honest answer.
    if (!target || !target->IsAlive() || !target->IsInWorld() || !U().Where().ShareFrame(target->Where()))
    {
        return false;
    }
    out = Motion::FrameFor(U()).NearPointAt(U(), *target, center, U().Where().Extent(), distance2d, absAngle);
    return true;
}

/**
 * @brief The fear source, resolved on demand at the pick: the generator's two reads.
 * @param rawGuid The fright's raw guid.
 * @param position Its placement in the mover's frame (the frame's ObjectPosition).
 * @param distance The distance between the two placements.
 * @return False when it cannot be found. A corpse resolves (the generator's GetUnit had no
 *         alive test); a fright on another frame is read exactly as the generator read it.
 */
bool NativeBehaviour::Fright(uint64 rawGuid, Motion::Vector3& position, float& distance)
{
    Unit const* fright = ObjectLookup::GetUnit(U(), ObjectGuid(rawGuid));
    if (!fright)
    {
        return false;
    }
    distance = fright->Where().DistanceTo(U().Where());
    position = Motion::FrameFor(U()).ObjectPosition(U(), *fright);
    return true;
}

/**
 * @brief The frame's floor under a guess reached from the mover's own position, the whole point.
 */
bool NativeBehaviour::GroundPoint(Motion::Vector3 const& guess, Motion::Vector3& out)
{
    Motion::IMotionFrame const& frame = Motion::FrameFor(U());
    const std::optional<Motion::Vector3> point = frame.GroundPoint(U(), frame.MoverPosition(U()), guess);
    if (!point)
    {
        return false;
    }
    out = *point;
    return true;
}

bool NativeBehaviour::ClaimHeld(Motion::Kind kind) const { return U().GetMotionMaster()->HoldsControl(kind); }

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
