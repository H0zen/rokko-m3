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

#include "Log/Log.h"
#include <cmath>
#include <optional>
#include <sstream>
#include "MotionMaster.h"
#include "Behaviour.h"
#include "LegacyBehaviour.h"
#include "ConfusedMovementGenerator.h"
#include "FleeingMovementGenerator.h"
#include "HomeMovementGenerator.h"
#include "IdleMovementGenerator.h"
#include "PointMovementGenerator.h"
#include "TargetedMovementGenerator.h"
#include "WaypointMovementGenerator.h"
#include "RandomMovementGenerator.h"
#include "movement/MoveSpline.h"
#include "movement/MoveSplineInit.h"
#include "Map.h"
#include "CreatureAISelector.h"
#include "Creature.h"
#include "CreatureLinkingMgr.h"
#include "Pet.h"
#include "World.h"
#include "DBCStores.h"

namespace
{
    /**
     * @brief The arbiter kind a legacy generator type projects onto.
     * @param type The legacy movement generator type.
     * @return The kind the arbiter holds it under.
     */
    Motion::Kind KindOf(MovementGeneratorType type)
    {
        switch (type)
        {
            case IDLE_MOTION_TYPE:                return Motion::Kind::Idle;
            case RANDOM_MOTION_TYPE:              return Motion::Kind::Wander;
            case WAYPOINT_MOTION_TYPE:            return Motion::Kind::Patrol;
            case CONFUSED_MOTION_TYPE:            return Motion::Kind::Confused;
            case CHASE_MOTION_TYPE:               return Motion::Kind::Chase;
            case HOME_MOTION_TYPE:                return Motion::Kind::Home;
            case FLIGHT_MOTION_TYPE:              return Motion::Kind::Taxi;
            case POINT_MOTION_TYPE:               return Motion::Kind::Point;
            case FLEEING_MOTION_TYPE:
            case TIMED_FLEEING_MOTION_TYPE:       return Motion::Kind::Fear;
            case DISTRACT_MOTION_TYPE:            return Motion::Kind::Distract;
            case ASSISTANCE_MOTION_TYPE:          return Motion::Kind::AssistRun;
            case ASSISTANCE_DISTRACT_MOTION_TYPE: return Motion::Kind::AssistDistract;
            case FOLLOW_MOTION_TYPE:              return Motion::Kind::Follow;
            case EFFECT_MOTION_TYPE:              return Motion::Kind::Effect;
            default:                              return Motion::Kind::Idle;
        }
    }

    const uint64 kFearClaim = 1;       ///< one live fear per unit until P4 hands the aura handlers identities
    const uint64 kConfusedClaim = 2;
    const uint32 kMaxCommitRounds = 8; ///< finalizers re-entering the facade during a commit

    /**
     * @brief One move request, spelled out.
     * @param kind The behaviour asked for.
     * @param id The MovementInform id, 0 when none.
     * @param resumeCombat Point only: suspend combat instead of overriding it.
     * @param claim Control only: the identity of the claim.
     * @return The request the arbiter takes.
     */
    Motion::MoveRequest R(Motion::Kind kind, uint32 id = 0, bool resumeCombat = false, uint64 claim = 0)
    {
        Motion::MoveRequest r;
        r.kind = kind;
        r.id = id;
        r.resumeCombat = resumeCombat;
        r.claim = claim;
        return r;
    }
}

// ---- Bound --------------------------------------------------------------------

MotionMaster::Bound::Bound(uint32 s, std::unique_ptr<MotionBehaviour> b) : seq(s), behaviour(std::move(b)), activated(false)
{
}

MotionMaster::Bound::Bound(Bound&& other) noexcept : seq(other.seq), behaviour(std::move(other.behaviour)), activated(other.activated)
{
}

MotionMaster::Bound& MotionMaster::Bound::operator=(Bound&& other) noexcept
{
    if (this != &other)
    {
        seq = other.seq;
        behaviour = std::move(other.behaviour);
        activated = other.activated;
    }
    return *this;
}

MotionMaster::Bound::~Bound()
{
}

// ---- Scope: one facade call = one arbiter transaction --------------------------

/**
 * The outermost scope owns the commit: it delivers the arbiter's events while the
 * transaction is still open (so a finalizer's requests fall under the same
 * generation), closes it (the doomed sweep), delivers what that sweep finished, and
 * only then reconciles the selection -- a round at a time, under a fresh transaction
 * of the same kind, until nothing is left. The behaviours the rounds finished are
 * destroyed when the last one ends, never inside a tick. A nested scope only joins.
 */
class MotionMaster::Scope
{
    public:
        Scope(MotionMaster& master, Motion::TransactionKind kind)
            : m_master(master), m_outermost(master.m_depth == 0)
        {
            if (m_outermost)
            {
                m_master.m_scopeKind = kind;
            }
            else if (kind == Motion::TransactionKind::Death)
            {
                m_master.m_scopeKind = Motion::TransactionKind::Death;   // a nested death escalates, as the arbiter's own transaction does
            }
            ++m_master.m_depth;
            m_transaction.emplace(m_master.m_arbiter, kind);
        }
        ~Scope()
        {
            if (m_outermost)
            {
                m_master.Commit(m_transaction);
            }
            else
            {
                m_transaction.reset();
            }
            --m_master.m_depth;
            if (m_outermost)
            {
                m_master.m_scopeKind = Motion::TransactionKind::Normal;
            }
        }
        Scope(Scope const&) = delete;
        Scope& operator=(Scope const&) = delete;
    private:
        MotionMaster&                      m_master;
        bool                               m_outermost;
        std::optional<Motion::Transaction> m_transaction;
};

// ---- construction --------------------------------------------------------------

/**
 * @brief Constructor for MotionMaster.
 * @param unit Pointer to the unit.
 */
MotionMaster::MotionMaster(Unit* unit)
    : m_owner(unit), m_depth(0), m_scopeKind(Motion::TransactionKind::Normal), m_pendingReset(PendingReset::None), m_exposedSeq(0)
{
    if (sWorld.getConfig(CONFIG_BOOL_MOVEMENT_DECISION_RING))
    {
        EnableDecisionRing();
    }
}

/**
 * @brief Destructor for MotionMaster.
 */
MotionMaster::~MotionMaster()
{
    m_retired.clear();
    m_bound.clear();   // generators deleted, no hooks: the stack deleted without Finalize too
}

// ---- the commit ----------------------------------------------------------------

/**
 * @brief Settles one outermost facade call: hooks, the doomed sweep, the selection.
 * @param transaction The open transaction, closed and reopened here.
 */
void MotionMaster::Commit(std::optional<Motion::Transaction>& transaction)
{
    for (uint32 round = 0; round < kMaxCommitRounds; ++round)
    {
        DeliverEvents();              // what the operation decided, inside its transaction: a finalizer's requests fall under the same generation
        transaction.reset();          // the arbiter's commit: doomed entries finish, events queue
        DeliverEvents();              // what the sweep finished (never activated, so no hook runs; the bindings go)
        Reconcile();                  // activate or resume the selection; may queue more
        if (!m_arbiter.HasEvents())
        {
            m_retired.clear();
            return;
        }
        transaction.emplace(m_arbiter, m_scopeKind);   // a finalizer re-entered: the strongest kind this scope saw, until nothing is left
    }
    sLog.outError("MotionMaster: %s commit did not settle in %u rounds", m_owner->GetGuidStr().c_str(), kMaxCommitRounds);
    transaction.reset();
    DeliverEvents();
    Reconcile();   // whatever this queues stays in the arbiter's queue; the next facade call's commit delivers it
    m_retired.clear();
}

/**
 * @brief Drains the arbiter's events, including those the hooks queue in turn.
 */
void MotionMaster::DeliverEvents()
{
    std::vector<Motion::Event> events = m_arbiter.DrainEvents();
    while (!events.empty())
    {
        for (size_t i = 0; i < events.size(); ++i)
        {
            Deliver(events[i]);
        }
        events = m_arbiter.DrainEvents();
    }
}

/**
 * @brief Runs one arbiter event through the hook matrix.
 * @param event The event the arbiter queued.
 */
void MotionMaster::Deliver(Motion::Event const& event)
{
    const size_t index = IndexOf(event.seq);
    if (index == m_bound.size())
    {
        return;   // a refused or already-unbound entry
    }
    Bound* bound = &m_bound[index];
    switch (event.kind)
    {
        case Motion::Event::Kind::Finished:
        {
            // No delivery of its own afterwards: the caller's loop drains what the hook queued.
            Retire(index, event.reason);
            return;
        }
        case Motion::Event::Kind::DefaultSwapped:
        {
            // The displaced default either parks beneath the new one (the factory default: masked,
            // as the stack interrupted it) or is gone (a second pushed default: superseded).
            std::optional<Motion::Held> const& fallback = m_arbiter.Fallback();
            if (fallback && fallback->seq == event.seq)
            {
                if (bound->activated)
                {
                    bound->behaviour->Suspend(*m_owner);
                }
                return;
            }
            Retire(index, Motion::FinishReason::Superseded);
            return;
        }
        case Motion::Event::Kind::Suspended:
        {
            // The idle command masks without stopping what it covers (scripts rely on the
            // movement continuing physically under MoveIdle).
            std::optional<Motion::Held> selected = m_arbiter.Selected();
            if (selected && selected->kind == Motion::Kind::Idle)
            {
                return;
            }
            if (bound->activated)
            {
                bound->behaviour->Suspend(*m_owner);
            }
            return;
        }
        case Motion::Event::Kind::Resumed:
            return;   // the reset latch decides whether the exposed behaviour resets (Reconcile)
        default:
            return;
    }
}

/**
 * @brief Drops the bindings the model no longer holds and starts or resumes the selection.
 */
void MotionMaster::Reconcile()
{
    // Entries the model refused or replaced in place have a binding and no entry: drop them
    // silently (never activated) or as superseded (the old chase of a D6 update).
    for (size_t i = 0; i < m_bound.size();)
    {
        if (IsHeld(m_bound[i].seq))
        {
            ++i;
            continue;
        }
        const bool activated = m_bound[i].activated;
        Retire(i, Motion::FinishReason::Superseded);
        if (activated)
        {
            // The hook may have finished other entries with reasons of their own (a Clear
            // from AttackStart, a nested request): deliver those before the rescan, or this
            // blanket supersede reaches them first and they lose their Finalize.
            DeliverEvents();
            i = 0;   // the hook may have bound or unbound entries: rescan from the start
        }
    }

    std::optional<Motion::Held> selected = m_arbiter.Selected();
    Bound* bound = selected ? Find(selected->seq) : NULL;
    if (selected && !bound)
    {
        sLog.outError("MotionMaster: %s selected %s has no behaviour", m_owner->GetGuidStr().c_str(), Motion::KindName(selected->kind));
    }
    if (bound)
    {
        if (!bound->activated)
        {
            bound->activated = true;
            bound->behaviour->Activate(*m_owner);   // never a reset: the stack never Reset a freshly pushed generator
        }
        else if (m_pendingReset == PendingReset::Always ||
                 (m_pendingReset == PendingReset::WhenExposed && selected->seq == m_exposedSeq))
        {
            bound->behaviour->Resume(*m_owner, true);
        }
    }
    m_pendingReset = PendingReset::None;

#ifdef MANGOS_DEBUG
    // The shell's invariant, once the selection has settled: every entry the model holds
    // (the parked factory default with them) has exactly one binding, and every binding
    // answers to an entry. Allocating to ask never reaches a release build.
    std::vector<Motion::Held> model = m_arbiter.Contents();
    if (std::optional<Motion::Held> const& parked = m_arbiter.Fallback())
    {
        model.push_back(*parked);
    }
    bool agree = model.size() == m_bound.size();
    for (size_t i = 0; agree && i < model.size(); ++i)
    {
        agree = Find(model[i].seq) != NULL;
    }
    for (size_t i = 0; agree && i < m_bound.size(); ++i)
    {
        agree = IsHeld(m_bound[i].seq);
    }
    if (!agree)
    {
        sLog.outError("MotionMaster: %s bindings disagree with the model", m_owner->GetGuidStr().c_str());
    }
#endif
}

// ---- binding -------------------------------------------------------------------

/**
 * @brief Whether the model still holds this sequence, the parked factory default included.
 * @param seq The arbiter sequence to look for.
 * @return True when an entry with this sequence is held.
 */
bool MotionMaster::IsHeld(uint32 seq) const
{
    return m_arbiter.Holds(seq);   // allocation-free: this runs per binding, per unit, per tick
}

/**
 * @brief The index of this sequence's binding.
 * @param seq The arbiter sequence.
 * @return Its index in m_bound, or m_bound.size() when it has none.
 */
size_t MotionMaster::IndexOf(uint32 seq) const
{
    for (size_t i = 0; i < m_bound.size(); ++i)
    {
        if (m_bound[i].seq == seq)
        {
            return i;
        }
    }
    return m_bound.size();
}

/**
 * @brief The binding of this sequence, or NULL.
 * @param seq The arbiter sequence.
 * @return The bound entry, or NULL.
 */
MotionMaster::Bound* MotionMaster::Find(uint32 seq)
{
    const size_t index = IndexOf(seq);
    return index < m_bound.size() ? &m_bound[index] : NULL;
}

/**
 * @brief The binding of this sequence, or NULL.
 * @param seq The arbiter sequence.
 * @return The bound entry, or NULL.
 */
MotionMaster::Bound const* MotionMaster::Find(uint32 seq) const
{
    const size_t index = IndexOf(seq);
    return index < m_bound.size() ? &m_bound[index] : NULL;
}

/**
 * @brief The binding of the selected entry, or NULL.
 * @return The selected bound entry, or NULL.
 */
MotionMaster::Bound* MotionMaster::SelectedBound()
{
    std::optional<Motion::Held> selected = m_arbiter.Selected();
    return selected ? Find(selected->seq) : NULL;
}

/**
 * @brief The binding of the selected entry, or NULL.
 * @return The selected bound entry, or NULL.
 */
MotionMaster::Bound const* MotionMaster::SelectedBound() const
{
    std::optional<Motion::Held> selected = m_arbiter.Selected();
    return selected ? Find(selected->seq) : NULL;
}

/**
 * @brief Moves a binding's behaviour out, drops the binding and retires the behaviour.
 * @param index The binding's index in m_bound.
 * @param reason Why it ended; delivered to Finish only when the behaviour ever ran.
 */
void MotionMaster::Retire(size_t index, Motion::FinishReason reason)
{
    std::unique_ptr<MotionBehaviour> gone = std::move(m_bound[index].behaviour);
    const bool activated = m_bound[index].activated;
    m_bound.erase(m_bound.begin() + static_cast<std::vector<Bound>::difference_type>(index));
    if (activated)
    {
        gone->Finish(*m_owner, reason);
    }
    m_retired.push_back(std::move(gone));   // a generator whose own Update fired this hook must outlive it
}

/**
 * @brief Binds the generator to the entry the request just produced.
 * @param kind The kind the request asked for.
 * @param seqBefore The arbiter's newest sequence before the request.
 * @param generator The legacy generator to adapt.
 * @param owned True when the behaviour owns (and deletes) the generator.
 * @param launch The Effect's spline parameters, if any.
 * @return True when the model kept the entry and it now has a behaviour.
 */
bool MotionMaster::Bind(Motion::Kind kind, uint32 seqBefore, MovementGenerator* generator, bool owned, EffectLaunch const& launch)
{
    // The arbiter stamps exactly once per Request/InstallDefault, so the entry this call
    // produced -- when the model kept it -- is exactly seqBefore + 1. A refusal (a control
    // with no identity: never stamped; an Idle default over an Idle command: stamped and
    // dropped) leaves nothing holding that sequence.
    if (!IsHeld(seqBefore + 1))
    {
        if (owned)
        {
            delete generator;
        }
        return false;
    }
    m_bound.push_back(Bound(seqBefore + 1, std::unique_ptr<MotionBehaviour>(new LegacyBehaviour(kind, generator, owned, launch))));
    return true;
}

/**
 * @brief Retires the binding a request left behind when the model updated an entry in place.
 * @param kind The kind the request asked for.
 *
 * Run after the request's events are delivered: by then the only binding of this kind the
 * model no longer holds is one it updated in place (a chase on a chasing unit, a claim of
 * the same identity), which finishes no entry and so queues no event of its own.
 */
void MotionMaster::SweepStale(Motion::Kind kind)
{
    for (size_t i = 0; i < m_bound.size();)
    {
        if (IsHeld(m_bound[i].seq) || m_bound[i].behaviour->Kind() != kind)
        {
            ++i;
            continue;
        }
        const bool activated = m_bound[i].activated;
        Retire(i, Motion::FinishReason::Superseded);
        if (activated)
        {
            DeliverEvents();   // a reason the hook queued must reach its entry before the rescan
            i = 0;
        }
    }
}

/**
 * @brief One facade request: a transaction, the model, the hooks, the binding.
 * @param request The move request.
 * @param generator The legacy generator to adapt.
 * @param owned True when the behaviour owns the generator.
 * @param launch The Effect's spline parameters, if any.
 */
void MotionMaster::Request(Motion::MoveRequest const& request, MovementGenerator* generator, bool owned, EffectLaunch const& launch)
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    const uint32 before = m_arbiter.LastSeq();
    m_arbiter.Request(request);
    // The entry this request stamped is bound before any hook runs: a hook may issue a
    // request of its own (a finalizer re-engaging combat), and that nested entry must not
    // be able to claim this generator -- nor may a hook see the facade empty over a model
    // that already holds the new selection.
    const bool bound = Bind(request.kind, before, generator, owned, launch);
    // What the request finished (superseded, overridden, cancelled, a swapped default) is
    // delivered now, with their own reasons and inside this transaction, as Mutate ran the
    // displaced generator's hooks synchronously.
    DeliverEvents();
    if (bound)
    {
        SweepStale(request.kind);
    }
}

/**
 * @brief One facade request with no Effect launch.
 * @param request The move request.
 * @param generator The legacy generator to adapt.
 * @param owned True when the behaviour owns the generator.
 */
void MotionMaster::Request(Motion::MoveRequest const& request, MovementGenerator* generator, bool owned)
{
    Request(request, generator, owned, EffectLaunch());
}

/**
 * @brief Installs the factory default; the caller owns the transaction.
 * @param kind The kind the default runs under.
 * @param generator The legacy generator to adapt.
 * @param owned True when the behaviour owns the generator.
 */
void MotionMaster::InstallFactory(Motion::Kind kind, MovementGenerator* generator, bool owned)
{
    const uint32 before = m_arbiter.LastSeq();
    m_arbiter.InstallDefault(kind);
    const bool bound = Bind(kind, before, generator, owned, EffectLaunch());   // before the hooks, as Request does it
    DeliverEvents();   // the clear's or the death's Finished events, and the swap's, with their own reasons
    if (bound)
    {
        SweepStale(kind);
    }
}

// ---- the facade ----------------------------------------------------------------

/**
 * @brief Initializes the MotionMaster.
 */
void MotionMaster::Initialize()
{
    m_owner->StopMoving();
    Scope scope(*this, Motion::TransactionKind::ClearAll);
    m_arbiter.Clear(true);
    MovementGenerator* movement = NULL;
    bool owned = false;
    if (m_owner->GetTypeId() == TYPEID_UNIT && !m_owner->hasUnitState(UNIT_STAT_CONTROLLED))
    {
        movement = FactorySelector::selectMovementGenerator((Creature*)m_owner);
        owned = movement != NULL;
    }
    if (!movement)
    {
        movement = &si_idleMovement;
    }
    InstallFactory(KindOf(movement->GetMovementGeneratorType()), movement, owned);
    if (movement->GetMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
    {
        (static_cast<WaypointMovementGenerator*>(movement))->InitializeWaypointPath(*((Creature*)(m_owner)), 0, PATH_NO_PATH, 0, 0);
    }
}

/**
 * @brief Gets the current movement generator.
 * @return Pointer to the selected behaviour's generator, or NULL.
 */
MovementGenerator const* MotionMaster::GetCurrent() const
{
    Bound const* bound = SelectedBound();
    return bound ? bound->behaviour->Legacy() : NULL;
}

/**
 * @brief Updates the motion of the unit.
 * @param diff Time difference.
 */
void MotionMaster::UpdateMotion(uint32 diff)
{
    if (m_owner->hasUnitState(UNIT_STAT_CAN_NOT_MOVE))
    {
        return;
    }
    if (m_arbiter.Empty())
    {
        Initialize();   // the stack reinstalled the factory default when it ran empty
    }
    Scope scope(*this, Motion::TransactionKind::Normal);
    Bound* bound = SelectedBound();
    if (!bound || !bound->activated)
    {
        return;   // activated at this scope's commit; ticks from the next update
    }
    MovementGenerator const* ticking = bound->behaviour->Legacy();
    const bool alive = bound->behaviour->Tick(*m_owner, diff);
    if (!alive && IsSelected(ticking))
    {
        bound = SelectedBound();   // the tick may have re-entered the facade; re-find
        const Motion::FinishReason reason = bound->behaviour->EndReason(*m_owner);
        const std::optional<Motion::Held> before = m_arbiter.Selected();
        m_arbiter.FinishSelected(reason);
        const std::optional<Motion::Held> after = m_arbiter.Selected();
        if (after && (!before || before->seq != after->seq))
        {
            m_pendingReset = PendingReset::WhenExposed;   // MovementExpired(reset = true), as UpdateMotion did
            m_exposedSeq = after->seq;
        }
    }
}

/**
 * @brief Clears the movement generators.
 * @param reset Whether the survivor resets.
 * @param all Whether the default goes too.
 */
void MotionMaster::Clear(bool reset, bool all)
{
    Scope scope(*this, all ? Motion::TransactionKind::ClearAll : Motion::TransactionKind::Clear);
    m_pendingReset = (reset && !all) ? PendingReset::Always : PendingReset::None;   // before the hooks: a later call in the same scope wins, as the stack's flag did
    m_arbiter.Clear(all);
    // The cleared entries' hooks run here, inside this scope's transaction: nested in another
    // operation, the clear discards for its own extent (a finalizer's request during it is
    // doomed, as the stack's clean loop popped what a finalizer pushed); outermost, the scope's
    // commit finishes the doomed entries at its end.
    DeliverEvents();
}

/**
 * @brief Expires the selected behaviour.
 * @param reset Whether the exposed behaviour resets.
 */
void MotionMaster::MovementExpired(bool reset)
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    const std::optional<Motion::Held> before = m_arbiter.Selected();
    m_arbiter.ExpireSelected();
    const std::optional<Motion::Held> after = m_arbiter.Selected();
    if (reset && after && (!before || before->seq != after->seq))
    {
        m_pendingReset = PendingReset::WhenExposed;
        m_exposedSeq = after->seq;
    }
    else
    {
        m_pendingReset = PendingReset::None;
    }
}

/**
 * @brief Moves the unit to idle state.
 */
void MotionMaster::MoveIdle()
{
    Request(R(Motion::Kind::Idle), &si_idleMovement, false);
}

/**
 * @brief Moves the unit randomly around a point.
 * @param x X-coordinate of the center point.
 * @param y Y-coordinate of the center point.
 * @param z Z-coordinate of the center point.
 * @param radius Radius of the random movement.
 * @param verticalZ Vertical offset for the movement.
 */
void MotionMaster::MoveRandomAroundPoint(float x, float y, float z, float radius, float verticalZ)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
    {
        sLog.outError("%s attempt to move random.", m_owner->GetGuidStr().c_str());
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s move random.", m_owner->GetGuidStr().c_str());
    Request(R(Motion::Kind::Wander), new RandomMovementGenerator(x, y, z, radius, verticalZ), true);
}

/**
 * @brief Moves the unit to its home position.
 */
void MotionMaster::MoveTargetedHome()
{
    if (m_owner->hasUnitState(UNIT_STAT_LOST_CONTROL))
    {
        return;
    }
    Clear(false);
    if (m_owner->GetTypeId() == TYPEID_UNIT && !((Creature*)m_owner)->GetCharmerOrOwnerGuid())
    {
        if (m_owner->IsLinkingEventTrigger() && m_owner->GetMap()->GetCreatureLinkingHolder()->TryFollowMaster((Creature*)m_owner))
        {
            DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s refollowed linked master", m_owner->GetGuidStr().c_str());
            return;
        }
        DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s targeted home", m_owner->GetGuidStr().c_str());
        // The stack asked the generator beneath for the reset position from inside Home's
        // Initialize; here the default is the selection after the clear, so ask it now.
        float x, y, z, o;
        MovementGenerator const* current = GetCurrent();
        if (!current || !current->GetResetPosition(*m_owner, x, y, z, o))
        {
            Geometry::Placement const& home = static_cast<Creature*>(m_owner)->Spawn();
            x = home.X();
            y = home.Y();
            z = home.Z();
            o = home.Facing();
        }
        Request(R(Motion::Kind::Home), new HomeMovementGenerator(Motion::Vector3(x, y, z), o), true);
    }
    else if (m_owner->GetTypeId() == TYPEID_UNIT && ((Creature*)m_owner)->GetCharmerOrOwnerGuid())
    {
        if (Unit* target = ((Creature*)m_owner)->GetCharmerOrOwner())
        {
            DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s follow to %s", m_owner->GetGuidStr().c_str(), target->GetGuidStr().c_str());
            Request(R(Motion::Kind::Follow), new FollowMovementGenerator(*target, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE), true);
        }
        else
        {
            DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s attempt but fail to follow owner", m_owner->GetGuidStr().c_str());
        }
    }
    else
    {
        sLog.outError("%s attempt targeted home", m_owner->GetGuidStr().c_str());
    }
}

/**
 * @brief Makes the unit move in a confused manner.
 */
void MotionMaster::MoveConfused()
{
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s move confused", m_owner->GetGuidStr().c_str());
    Request(R(Motion::Kind::Confused, 0, false, kConfusedClaim), new ConfusedMovementGenerator(), true);
}

/**
 * @brief Makes the unit chase a target.
 * @param target Pointer to the target unit.
 * @param dist Distance to maintain from the target.
 * @param angle Angle to maintain from the target.
 */
void MotionMaster::MoveChase(Unit* target, float dist, float angle)
{
    if (!target)
    {
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s chase to %s", m_owner->GetGuidStr().c_str(), target->GetGuidStr().c_str());
    Request(R(Motion::Kind::Chase), new ChaseMovementGenerator(*target, dist, angle), true);
}

/**
 * @brief Makes the unit follow a target.
 * @param target Pointer to the target unit.
 * @param dist Distance to maintain from the target.
 * @param angle Angle to maintain from the target.
 */
void MotionMaster::MoveFollow(Unit* target, float dist, float angle)
{
    if (m_owner->hasUnitState(UNIT_STAT_LOST_CONTROL))
    {
        return;
    }
    Clear();
    if (!target)
    {
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s follow to %s", m_owner->GetGuidStr().c_str(), target->GetGuidStr().c_str());
    Request(R(Motion::Kind::Follow), new FollowMovementGenerator(*target, dist, angle), true);
}

/**
 * @brief Moves the unit to a specific point.
 * @param id ID of the movement.
 * @param x X-coordinate of the destination.
 * @param y Y-coordinate of the destination.
 * @param z Z-coordinate of the destination.
 * @param generatePath Whether to generate a path to the destination.
 */
void MotionMaster::MovePoint(uint32 id, float x, float y, float z, bool generatePath)
{
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s targeted point (Id: %u X: %f Y: %f Z: %f)", m_owner->GetGuidStr().c_str(), id, x, y, z);
    Request(R(Motion::Kind::Point, id), new PointMovementGenerator(id, x, y, z, generatePath), true);
}

/**
 * @brief Makes the unit seek assistance at a specific point.
 * @param x X-coordinate of the assistance point.
 * @param y Y-coordinate of the assistance point.
 * @param z Z-coordinate of the assistance point.
 */
void MotionMaster::MoveSeekAssistance(float x, float y, float z)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
    {
        sLog.outError("%s attempt to seek assistance", m_owner->GetGuidStr().c_str());
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s seek assistance (X: %f Y: %f Z: %f)", m_owner->GetGuidStr().c_str(), x, y, z);
    Request(R(Motion::Kind::AssistRun), new AssistanceMovementGenerator(x, y, z), true);
}

/**
 * @brief Makes the unit seek assistance and then distract.
 * @param time Time for the distraction.
 */
void MotionMaster::MoveSeekAssistanceDistract(uint32 time)
{
    if (m_owner->GetTypeId() == TYPEID_PLAYER)
    {
        sLog.outError("%s attempt to call distract after assistance", m_owner->GetGuidStr().c_str());
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s is distracted after assistance call (Time: %u)", m_owner->GetGuidStr().c_str(), time);
    Request(R(Motion::Kind::AssistDistract), new AssistanceDistractMovementGenerator(time), true);
}

/**
 * @brief Makes the unit flee from an enemy.
 * @param enemy Pointer to the enemy unit.
 * @param time Time limit for the fleeing movement.
 */
void MotionMaster::MoveFleeing(Unit* enemy, uint32 time)
{
    if (!enemy)
    {
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s flee from %s", m_owner->GetGuidStr().c_str(), enemy->GetGuidStr().c_str());
    MovementGenerator* generator = (m_owner->GetTypeId() != TYPEID_PLAYER && time)
        ? static_cast<MovementGenerator*>(new TimedFleeingMovementGenerator(enemy->GetObjectGuid(), time))
        : static_cast<MovementGenerator*>(new FleeingMovementGenerator(enemy->GetObjectGuid()));
    Request(R(Motion::Kind::Fear, 0, false, kFearClaim), generator, true);
}

/**
 * @brief Moves the unit along a waypoint path.
 * @param id ID of the waypoint path.
 * @param source Source of the waypoint path.
 * @param initialDelay Initial delay before starting the movement.
 * @param overwriteEntry Entry to overwrite.
 */
void MotionMaster::MoveWaypoint(int32 id, uint32 source, uint32 initialDelay, uint32 overwriteEntry)
{
    if (m_owner->GetTypeId() != TYPEID_UNIT)
    {
        sLog.outError("Non-creature %s attempt to MoveWaypoint()", m_owner->GetGuidStr().c_str());
        return;
    }
    if (GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
    {
        sLog.outError("Creature %s (Entry %u) attempt to MoveWaypoint() but creature is already using waypoint", m_owner->GetGuidStr().c_str(), m_owner->GetEntry());
        return;
    }
    Creature* creature = (Creature*)m_owner;
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s start MoveWaypoint()", m_owner->GetGuidStr().c_str());
    WaypointMovementGenerator* generator = new WaypointMovementGenerator(*creature);
    generator->InitializeWaypointPath(*creature, id, (WaypointPathOrigin)source, initialDelay, overwriteEntry);
    Request(R(Motion::Kind::Patrol), generator, true);   // a default request is never refused
}

/**
 * @brief Holds a waypoint patrol where it stands.
 * @param ms How long to hold before the patrol goes on.
 * @return True when the selected behaviour was a patrol and took the pause.
 */
bool MotionMaster::PauseWaypoints(int32 ms)
{
    Bound* bound = SelectedBound();
    if (!bound || bound->behaviour->LegacyType() != WAYPOINT_MOTION_TYPE)
    {
        return false;
    }
    static_cast<WaypointMovementGenerator*>(bound->behaviour->Legacy())->Pause(*m_owner, ms);
    return true;
}

/**
 * @brief Moves the unit along a taxi flight path.
 * @param path ID of the flight path.
 * @param pathnode Node of the flight path.
 */
void MotionMaster::MoveTaxiFlight(uint32 path, uint32 pathnode)
{
    if (m_owner->GetTypeId() != TYPEID_PLAYER)
    {
        sLog.outError("%s attempt taxi to (Path %u node %u)", m_owner->GetGuidStr().c_str(), path, pathnode);
        return;
    }
    if (path >= sTaxiPathNodesByPath.size())
    {
        sLog.outError("%s attempt taxi to (nonexistent Path %u node %u)", m_owner->GetGuidStr().c_str(), path, pathnode);
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s taxi to (Path %u node %u)", m_owner->GetGuidStr().c_str(), path, pathnode);
    Request(R(Motion::Kind::Taxi), new FlightPathMovementGenerator(sTaxiPathNodesByPath[path], pathnode), true);
}

/**
 * @brief Makes the unit distracted for a specified time.
 * @param timer Time limit for the distraction.
 */
void MotionMaster::MoveDistract(uint32 timer)
{
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s distracted (timer: %u)", m_owner->GetGuidStr().c_str(), timer);
    Request(R(Motion::Kind::Distract), new DistractMovementGenerator(timer), true);
}

/**
 * @brief Makes the unit jump to a point.
 * @param x X-coordinate of the destination.
 * @param y Y-coordinate of the destination.
 * @param z Z-coordinate of the destination.
 * @param horizontalSpeed The horizontal speed of the jump.
 * @param max_height The height of the parabola.
 * @param id ID of the movement.
 */
void MotionMaster::MoveJump(float x, float y, float z, float horizontalSpeed, float max_height, uint32 id)
{
    EffectLaunch launch;
    launch.kind = EffectLaunch::Jump;
    launch.x = x;
    launch.y = y;
    launch.z = z;
    launch.speed = horizontalSpeed;
    launch.height = max_height;
    Request(R(Motion::Kind::Effect, id), new EffectMovementGenerator(id), true, launch);
}

/**
 * @brief Makes the unit jump to a position.
 * @param pos The destination.
 * @param horizontalSpeed The horizontal speed of the jump.
 * @param max_height The height of the parabola.
 * @param id ID of the movement.
 */
void MotionMaster::MoveJump(Position& pos, float horizontalSpeed, float max_height, uint32 id)
{
    MoveJump(pos.x, pos.y, pos.z, horizontalSpeed, max_height, id);
}

/**
 * @brief A jump that ends FACING something -- a target, or a given orientation.
 *
 * Kept where mangos_two teleports instead (its EffectJump ends in NearTeleportTo
 * with a TODO). A spline the client can see is the better answer, so this is the
 * implementation that wins; it lays no behaviour because the jump IS the whole
 * movement and there is nothing left to drive afterwards.
 */
void MotionMaster::MoveDestination(float x, float y, float z, float o, float horizontalSpeed, float max_height, Unit* target)
{
    // unchanged: a raw spline (P5 routes it)
    Movement::MoveSplineInit init(*m_owner);
    init.MoveTo(x, y, z);
    init.SetParabolic(max_height, 0);
    init.SetVelocity(horizontalSpeed);
    target ? init.SetFacing(target) : init.SetFacing(o);
    init.Launch();
}

/**
 * @brief Makes the unit fall to the ground.
 */
void MotionMaster::MoveFall()
{
    // Use larger distance for vmap height search than in most other cases
    const auto floor = m_owner->GetMap()->Floor(m_owner->GetPhaseMask(), m_owner->Where().X(), m_owner->Where().Y(), m_owner->Where().Z());
    if (!floor)
    {
        DEBUG_LOG("MotionMaster::MoveFall: unable retrive a proper height at map %u (x: %f, y: %f, z: %f).",
                  m_owner->GetMap()->GetId(), m_owner->Where().X(), m_owner->Where().Y(), m_owner->Where().Z());
        return;
    }
    // Abort too if the ground is very near
    const float tz = *floor;
    if (fabs(m_owner->Where().Z() - tz) < 0.1f)
    {
        return;
    }
    EffectLaunch launch;
    launch.kind = EffectLaunch::Fall;
    launch.x = m_owner->Where().X();
    launch.y = m_owner->Where().Y();
    launch.z = tz;
    Request(R(Motion::Kind::Effect, 0), new EffectMovementGenerator(0), true, launch);
}

/**
 * @brief Makes the unit fly or land.
 * @param id ID of the movement.
 * @param x X-coordinate of the destination.
 * @param y Y-coordinate of the destination.
 * @param z Z-coordinate of the destination.
 * @param liftOff Whether the unit should lift off or land.
 */
void MotionMaster::MoveFlyOrLand(uint32 id, float x, float y, float z, bool liftOff)
{
    if (m_owner->GetTypeId() != TYPEID_UNIT)
    {
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s targeted point for %s (Id: %u X: %f Y: %f Z: %f)", m_owner->GetGuidStr().c_str(), liftOff ? "liftoff" : "landing", id, x, y, z);
    Request(R(Motion::Kind::FlyLand, id), new FlyOrLandMovementGenerator(id, x, y, z, liftOff), true);
}

/**
 * @brief Gets the type of the current movement generator.
 * @return The type of the selected behaviour's generator.
 */
MovementGeneratorType MotionMaster::GetCurrentMovementGeneratorType() const
{
    Bound const* bound = SelectedBound();
    return bound ? bound->behaviour->LegacyType() : IDLE_MOTION_TYPE;
}

/**
 * @brief Propagates the speed change to every held behaviour.
 */
void MotionMaster::PropagateSpeedChange()
{
    for (size_t i = 0; i < m_bound.size(); ++i)
    {
        m_bound[i].behaviour->SpeedChanged();
    }
}

/**
 * @brief The held patrol generator wherever it sits; the newest when two are held.
 * @return The waypoint generator, or NULL.
 */
WaypointMovementGenerator* MotionMaster::HeldWaypoint()
{
    for (size_t i = m_bound.size(); i-- > 0;)   // newest first: a patrol pushed over a parked factory patrol is the one the readers mean, as the stack's top-down search found it
    {
        if (m_bound[i].behaviour->LegacyType() == WAYPOINT_MOTION_TYPE)
        {
            return static_cast<WaypointMovementGenerator*>(m_bound[i].behaviour->Legacy());
        }
    }
    return NULL;
}

/**
 * @brief The held patrol generator wherever it sits; the newest when two are held.
 * @return The waypoint generator, or NULL.
 */
WaypointMovementGenerator const* MotionMaster::HeldWaypoint() const
{
    for (size_t i = m_bound.size(); i-- > 0;)   // newest first: a patrol pushed over a parked factory patrol is the one the readers mean, as the stack's top-down search found it
    {
        if (m_bound[i].behaviour->LegacyType() == WAYPOINT_MOTION_TYPE)
        {
            return static_cast<WaypointMovementGenerator const*>(m_bound[i].behaviour->Legacy());
        }
    }
    return NULL;
}

/**
 * @brief The held taxi flight.
 * @return The flight generator, or NULL.
 */
FlightPathMovementGenerator* MotionMaster::HeldFlight()
{
    for (size_t i = 0; i < m_bound.size(); ++i)
    {
        if (m_bound[i].behaviour->LegacyType() == FLIGHT_MOTION_TYPE)
        {
            return static_cast<FlightPathMovementGenerator*>(m_bound[i].behaviour->Legacy());
        }
    }
    return NULL;
}

/**
 * @brief Sets the next waypoint for the unit.
 * @param pointId ID of the next waypoint.
 * @return True if the next waypoint was successfully set, false otherwise.
 */
bool MotionMaster::SetNextWaypoint(uint32 pointId)
{
    WaypointMovementGenerator* waypoint = HeldWaypoint();
    return waypoint ? waypoint->SetNextWaypoint(pointId) : false;
}

/**
 * @brief Gets the last reached waypoint.
 * @return The ID of the last reached waypoint.
 */
uint32 MotionMaster::getLastReachedWaypoint() const
{
    WaypointMovementGenerator const* waypoint = HeldWaypoint();
    return waypoint ? waypoint->getLastReachedWaypoint() : 0;
}

/**
 * @brief Gets the waypoint path information.
 * @param oss Output stream to store the waypoint path information.
 */
void MotionMaster::GetWaypointPathInformation(std::ostringstream& oss) const
{
    if (WaypointMovementGenerator const* waypoint = HeldWaypoint())
    {
        waypoint->GetPathInformation(oss);
    }
}

/**
 * @brief Gets the destination coordinates.
 * @param x Reference to the X-coordinate.
 * @param y Reference to the Y-coordinate.
 * @param z Reference to the Z-coordinate.
 * @return True if the destination coordinates were successfully obtained, false otherwise.
 */
bool MotionMaster::GetDestination(float& x, float& y, float& z)
{
    if (m_owner->movespline->Finalized())
    {
        return false;
    }
    const Geometry::Vector3& dest = m_owner->movespline->FinalDestination();
    x = dest.x;
    y = dest.y;
    z = dest.z;
    return true;
}

// ---- the new operations --------------------------------------------------------

/**
 * @brief Death: every behaviour finishes Died, then the idle default.
 */
void MotionMaster::Die()
{
    Scope scope(*this, Motion::TransactionKind::Death);
    m_arbiter.Die();
    InstallFactory(Motion::Kind::Idle, &si_idleMovement, false);   // never doomed: survives the death's own guard
}

/**
 * @brief Releases the control claims of this kind.
 * @param kind The control kind (Fear, Confused).
 */
void MotionMaster::CancelControl(Motion::Kind kind)
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    m_arbiter.CancelControl(kind);
}

/**
 * @brief A near teleport: suspend the selection, relocate, resume it with a reset.
 * @param x The destination X coordinate.
 * @param y The destination Y coordinate.
 * @param z The destination Z coordinate.
 * @param o The destination facing.
 */
void MotionMaster::RelocateSelected(float x, float y, float z, float o)
{
    Bound* bound = SelectedBound();
    if (bound && bound->activated)
    {
        bound->behaviour->Suspend(*m_owner);
    }
    m_owner->GetMap()->CreatureRelocation((Creature*)m_owner, x, y, z, o);
    m_owner->SendHeartBeat();
    // The relocation and the heartbeat may have changed what is selected; resume whatever
    // is selected now, as the stack applied its Reset to whatever ended up on top.
    bound = SelectedBound();
    if (bound && bound->activated)
    {
        bound->behaviour->Resume(*m_owner, true);
    }
}

/**
 * @brief Whether this generator belongs to the selected behaviour.
 * @param generator The generator to test.
 * @return True when it is the selected behaviour's generator.
 */
bool MotionMaster::IsSelected(MovementGenerator const* generator) const
{
    Bound const* bound = SelectedBound();
    return bound && bound->behaviour->Legacy() == generator;
}

/**
 * @brief The selected entry's kind: what runs now.
 * @return The kind, Idle when nothing is held.
 */
Motion::Kind MotionMaster::ActiveKind() const
{
    std::optional<Motion::Held> selected = m_arbiter.Selected();
    return selected ? selected->kind : Motion::Kind::Idle;
}

/**
 * @brief Whether a chase is held, selected or masked.
 * @return True when the Combat entry exists.
 */
bool MotionMaster::IsChasing() const
{
    return m_arbiter.Combat().has_value();
}

/**
 * @brief Whether the current default is a follow, selected or masked.
 * @return True for a Follow default; the parked fallback does not count.
 */
bool MotionMaster::IsFollowing() const
{
    std::optional<Motion::Held> const& current = m_arbiter.Default();
    return current && current->kind == Motion::Kind::Follow;
}

/**
 * @brief The held follow's target.
 * @return The target unit, or NULL without a follow.
 */
Unit* MotionMaster::FollowTarget() const
{
    std::optional<Motion::Held> const& current = m_arbiter.Default();
    Bound const* bound = (current && current->kind == Motion::Kind::Follow) ? Find(current->seq) : NULL;
    if (!bound || bound->behaviour->LegacyType() != FOLLOW_MOTION_TYPE)
    {
        return NULL;
    }
    return static_cast<FollowMovementGenerator const*>(bound->behaviour->Legacy())->GetTarget();
}

/**
 * @brief Whether the current default is a patrol, selected or masked.
 * @return True for a Patrol default.
 */
bool MotionMaster::IsPatrolling() const
{
    std::optional<Motion::Held> const& current = m_arbiter.Default();
    return current && current->kind == Motion::Kind::Patrol;
}

/**
 * @brief Whether a taxi flight is held.
 * @return True when the Taxi entry exists.
 */
bool MotionMaster::IsOnTaxi() const
{
    return m_arbiter.Command(Motion::Layer::Taxi).has_value();
}

/**
 * @brief Whether the selected behaviour can reach its goal.
 * @return The selected generator's answer; true when nothing is selected.
 */
bool MotionMaster::IsReachable() const
{
    MovementGenerator const* current = GetCurrent();
    return !current || current->IsReachable();
}

/**
 * @brief The combat-started event row: a new combat cancels the Distract layer.
 */
void MotionMaster::CombatStarted()
{
    // A combat start from inside a movement operation (a finalizer's AttackStop/AttackStart re-engaging) is that operation's own doing, not a new combat: the row fires for combat that begins outside a commit.
    if (m_depth > 0)
    {
        return;
    }

    Scope scope(*this, Motion::TransactionKind::Normal);
    m_arbiter.Notify(Motion::ExternalEvent::CombatStarted);
}

/**
 * @brief Whether this sequence's behaviour has been activated.
 * @param seq The arbiter sequence.
 * @return True when a binding exists for it and has run Activate.
 */
bool MotionMaster::IsActivated(uint32 seq) const
{
    Bound const* bound = Find(seq);
    return bound && bound->activated;
}

/**
 * @brief Every held behaviour in arrival order, the selected one marked.
 * @return The listing.
 */
std::vector<MotionMaster::HeldView> MotionMaster::Held() const
{
    std::vector<HeldView> out;
    Bound const* selected = SelectedBound();
    for (size_t i = 0; i < m_bound.size(); ++i)
    {
        HeldView view;
        view.generator = m_bound[i].behaviour->Legacy();
        view.selected = &m_bound[i] == selected;
        out.push_back(view);
    }
    return out;
}

/**
 * @brief Allocates the arbiter's decision ring.
 */
void MotionMaster::EnableDecisionRing()
{
    m_arbiter.EnableRing();
}
