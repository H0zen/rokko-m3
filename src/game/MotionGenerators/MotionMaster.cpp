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
#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include "MotionMaster.h"
#include "NativeBehaviour.h"
#include "SimpleMoves.h"
#include "DefaultMoves.h"
#include "TrackingMoves.h"
#include "MovementIntent.h"
#include "ControlMoves.h"
#include "TaxiMove.h"
#include "WaypointManager.h"
#include "movement/MoveSpline.h"
#include "movement/MoveSplineInit.h"
#include "Map.h"
#include "ObjectLookup.h"
#include "CreatureAISelector.h"
#include "Creature.h"
#include "CreatureLinkingMgr.h"
#include "Pet.h"
#include "Player.h"
#include "TaxiRoute.h"
#include "World.h"
#include "DBCStores.h"
#include "ObjectMgr.h"

namespace
{
    const uint64 kScriptConfuse = Motion::ControlClaim(0, 2, 1);   ///< MoveConfused() with no identity (no script calls it today)
    const uint32 kMaxCommitRounds = 8; ///< finalizers re-entering the facade during a commit
    /// Every reason but Dead: a real death never set a unit-state bit, and IsAlive() answers it.
    const uint8 kPublishedReasons = Motion::ReasonRooted | Motion::ReasonStunned | Motion::ReasonPossessed |
                                    Motion::ReasonFeared | Motion::ReasonConfused | Motion::ReasonDistracted |
                                    Motion::ReasonOnTaxi;

    /// A leash radius below this is meaningless and would make every hop degenerate (the generator's own floor).
    const float MIN_WANDER_RADIUS = 0.1f;

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

    /**
     * @brief The follow native's parameters, as both request sites build them.
     * @param target The unit to trail.
     * @param dist The requested distance behind it.
     * @param angle The requested bearing relative to its facing.
     * @return The Params, with the cadence, the horizon and the config tolerance.
     */
    Motion::FollowBehaviour::FollowParams MakeFollowParams(Unit const& target, float dist, float angle)
    {
        Motion::FollowBehaviour::FollowParams p;
        p.target = target.GetObjectGuid().GetRawValue();   // resolved per tick; never a stored pointer (design v2 §3.2)
        p.offset = dist;
        p.angle = angle;
        p.routineMs = 400;    // retail's measured re-lay cluster, in place of the generator's 50 ms poll
        // One cadence of lead on a trusted velocity by default: the heel point (design §6.3).
        // Movement.FollowHorizonMs = 0 turns it off, which is retail's own aim and the baseline
        // the harness's follow-keeps-pace scenario is measured against.
        p.horizonMs = sWorld.getConfig(CONFIG_UINT32_MOVEMENT_FOLLOW_HORIZON_MS);
        p.recalcRange = sWorld.getConfig(CONFIG_FLOAT_RATE_TARGET_POS_RECALCULATION_RANGE);
        return p;
    }

    /**
     * @brief The shell's LoadPath: resolves the path and copies each node into the native's Params.
     * @param creature The creature the path is loaded for.
     * @param pathId The requested path id (0 for "the default path").
     * @param source The requested path source; PATH_NO_PATH for "figure it out".
     * @param initialDelay How long the patrol waits before its first leg.
     * @param overwriteEntry An entry to load the path for instead of the creature's own; 0 for the creature's own.
     * @param out Filled with the resolved path and its nodes.
     * @return True when a non-empty path was resolved; false leaves `out` with no nodes (the
     *         native then holds, exactly as the generator's unresolved LoadPath did).
     */
    bool BuildPatrolParams(Creature& creature, int32 pathId, WaypointPathOrigin source,
                           uint32 initialDelay, uint32 overwriteEntry,
                           Motion::PatrolBehaviour::Params& out)
    {
        DETAIL_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "BuildPatrolParams: loading waypoint path for %s", creature.GetGuidStr().c_str());
        if (!overwriteEntry)
        {
            overwriteEntry = creature.GetEntry();
        }

        WaypointPathOrigin resolvedOrigin = source;
        WaypointPath const* path = NULL;
        if (source == PATH_NO_PATH && pathId == 0)
        {
            path = sWaypointMgr.GetDefaultPath(overwriteEntry, creature.GetGUIDLow(), &resolvedOrigin);
        }
        else
        {
            resolvedOrigin = (source == PATH_NO_PATH) ? PATH_FROM_ENTRY : source;
            path = sWaypointMgr.GetPathFromOrigin(overwriteEntry, creature.GetGUIDLow(), pathId, resolvedOrigin);
        }

        out.pathId = pathId;
        out.origin = uint32(resolvedOrigin);
        // The entry this path was actually resolved with (the creature's own unless the caller
        // overwrote it), so a later refresh reaches the same map key, and the manager revision
        // the nodes below are read at: UpdateMotion compares it before every patrol tick.
        out.entry = overwriteEntry;
        out.revision = sWaypointMgr.Revision();
        out.external = resolvedOrigin == PATH_FROM_EXTERNAL && pathId > 0;
        out.externalOrigin = resolvedOrigin == PATH_FROM_EXTERNAL;
        out.initialDelay = initialDelay;

        if (!path)
        {
            if (resolvedOrigin == PATH_FROM_EXTERNAL)
            {
                sLog.outErrorScriptLib("BuildPatrolParams: %s doesn't have waypoint path %i", creature.GetGuidStr().c_str(), pathId);
            }
            else
            {
                sLog.outErrorDb("BuildPatrolParams: %s doesn't have waypoint path %i", creature.GetGuidStr().c_str(), pathId);
            }
            return false;
        }

        if (path->empty())
        {
            return false;
        }

        for (WaypointPath::const_iterator itr = path->begin(); itr != path->end(); ++itr)
        {
            WaypointNode const& src = itr->second;
            Motion::PatrolBehaviour::Node node;
            node.id = itr->first;
            node.pos = Motion::Vector3(src.x, src.y, src.z);
            node.orientation = src.orientation;
            node.delay = src.delay;
            node.scriptId = src.script_id;
            if (WaypointBehavior* behavior = src.behavior)
            {
                node.emote = behavior->emote;
                node.spell = behavior->spell;
                node.model1 = behavior->model1;
                node.model2 = behavior->model2;
                for (int i = 0; i < MAX_WAYPOINT_TEXT && behavior->textid[i]; ++i)
                {
                    node.textIds.push_back(behavior->textid[i]);
                }
                for (int i = 0; i < MAX_WAYPOINT_TEXT; ++i)
                {
                    if (behavior->textid[i])
                    {
                        node.textAnywhere = true;
                        break;
                    }
                }
            }
            out.nodes.push_back(node);
        }

        return true;
    }

    /**
     * @brief Re-reads a running patrol's nodes when a waypoint edit has changed them.
     *
     * The deleted generator held a live WaypointPath pointer at the manager's own node map, so a
     * GM's `.wp modify` reached a patrol already walking that path. The native owns a copy
     * instead, and this is what keeps the copy honest: one integer compare per patrol tick, and
     * a re-read only when the manager's revision has actually moved. Only the selected binding
     * is checked -- a masked patrol refreshes when it is selected again, which is the first tick
     * it could act on the new nodes anyway.
     * @param owner The moving unit.
     * @param behaviour The selected binding's behaviour.
     */
    void RefreshPatrolPath(Unit& owner, NativeBehaviour& behaviour)
    {
        if (behaviour.Kind() != Motion::Kind::Patrol || owner.GetTypeId() != TYPEID_UNIT)
        {
            return;
        }
        Motion::PatrolBehaviour* native =
            static_cast<Motion::PatrolBehaviour*>(behaviour.Native());
        if (native->Revision() == sWaypointMgr.Revision())
        {
            return;
        }
        // The same entry the path was resolved with the first time (BuildPatrolParams normalized
        // 0 to the creature's own), so the refresh reads the very same map key; the initial delay
        // is not re-applied, since ReplaceNodes takes the nodes alone and leaves the patrol's own
        // progress -- its current node, its wait and the leg in flight -- untouched.
        Motion::PatrolBehaviour::Params np;
        BuildPatrolParams(static_cast<Creature&>(owner), native->PathId(), WaypointPathOrigin(native->Origin()),
                          0, native->Entry(), np);
        native->ReplaceNodes(np.nodes, np.revision);
    }
}

// ---- Bound --------------------------------------------------------------------

MotionMaster::Bound::Bound(uint32 s, std::unique_ptr<NativeBehaviour> b) : seq(s), behaviour(std::move(b)), activated(false)
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
    : m_owner(unit), m_depth(0), m_scopeKind(Motion::TransactionKind::Normal), m_pendingReset(PendingReset::None), m_exposedSeq(0), m_clientRooted(false)
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
    m_bound.clear();   // held entries deleted, no hooks: the stack deleted without Finalize too
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
            Publish();                // the published block, after every settled commit (P5-C2; the P5-A mirror's place)
            m_retired.clear();
            return;
        }
        transaction.emplace(m_arbiter, m_scopeKind);   // a finalizer re-entered: the strongest kind this scope saw, until nothing is left
    }
    sLog.outError("MotionMaster: %s commit did not settle in %u rounds", m_owner->GetGuidStr().c_str(), kMaxCommitRounds);
    transaction.reset();
    DeliverEvents();
    Reconcile();   // whatever this queues stays in the arbiter's queue; the next facade call's commit delivers it
    Publish();
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
            if (event.reason == Motion::FinishReason::Blocked)
            {
                if (bound->activated)
                {
                    bound->behaviour->Suspend(*m_owner);   // the block: paused where it stands, never finished
                }
                return;
            }
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
            if (event.reason == Motion::FinishReason::Blocked)
            {
                m_pendingReset = PendingReset::Always;   // the block lifted: re-lay from where the unit stands (spec §5)
            }
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
        else if (m_arbiter.Evaluate().ticks)
        {
            // The selection hears Resume at every commit, reset or not, unless the block holds it:
            // a paused behaviour stays suspended until the lift's own Resumed(Blocked) arrives.
            const bool reset = m_pendingReset == PendingReset::Always ||
                               (m_pendingReset == PendingReset::WhenExposed && selected->seq == m_exposedSeq);
            bound->behaviour->Resume(*m_owner, reset);
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
    std::unique_ptr<NativeBehaviour> gone = std::move(m_bound[index].behaviour);
    const bool activated = m_bound[index].activated;
    m_bound.erase(m_bound.begin() + static_cast<std::vector<Bound>::difference_type>(index));
    if (activated)
    {
        gone->Finish(*m_owner, reason);
    }
    m_retired.push_back(std::move(gone));   // a behaviour whose own Tick fired this hook must outlive it
}

/**
 * @brief Binds a native kernel behaviour to the entry the request just produced.
 * @param seqBefore The arbiter's newest sequence before the request.
 * @param native The kernel behaviour to adapt.
 * @return True when the model kept the entry and it now has a behaviour.
 */
bool MotionMaster::BindNative(uint32 seqBefore, std::unique_ptr<Motion::Behaviour> native)
{
    if (!IsHeld(seqBefore + 1))
    {
        return false;   // refused by the model: the native dies with this call
    }
    std::unique_ptr<NativeBehaviour> adapter(new NativeBehaviour(std::move(native)));
    adapter->SetSequence(seqBefore + 1);   // what the tick's per-round IsSelectedSequence re-check reads
    m_bound.push_back(Bound(seqBefore + 1, std::move(adapter)));
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
 * @brief One facade request whose behaviour is a native of the kernel.
 * @param request The move request.
 * @param native The kernel behaviour the adapter drives.
 */
void MotionMaster::Request(Motion::MoveRequest const& request, std::unique_ptr<Motion::Behaviour> native)
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    const uint32 before = m_arbiter.LastSeq();
    m_arbiter.Request(request);
    // The entry this request stamped is bound before any hook runs: a hook may issue a
    // request of its own, and that nested entry must not see the facade empty over a model
    // that already holds the new selection.
    const bool bound = BindNative(before, std::move(native));
    DeliverEvents();
    if (bound)
    {
        SweepStale(request.kind);
    }
}

/**
 * @brief One Effect request, through the shell's own gate.
 * @param id The MovementInform id, 0 when none.
 * @param launch The arc or the fall the Effect native guards.
 * @return False when the request was refused.
 */
bool MotionMaster::RequestEffect(uint32 id, Motion::EffectLaunch const& launch)
{
    // A knockback arc never displaces a rooted unit (reference: the family's retail notes, A):
    // refused before the model, recorded in the ring, nothing bound, nothing informed. Only
    // Rooted refuses: a stun mid-flight is undocumented and the jump completes under one; the
    // dying flyer's fall is requested after the death inhibit and must run.
    if (launch.kind == Motion::EffectLaunch::Jump && (m_arbiter.Evaluate(Motion::Kind::Effect).reasons & Motion::ReasonRooted))
    {
        m_arbiter.Refuse(R(Motion::Kind::Effect, id));
        return false;
    }
    Request(R(Motion::Kind::Effect, id), std::unique_ptr<Motion::Behaviour>(new Motion::EffectBehaviour(id, launch)));
    return true;
}

/**
 * @brief Installs a native factory default; the caller owns the transaction.
 * @param kind The kind the default runs under.
 * @param native The kernel behaviour the adapter drives.
 */
void MotionMaster::InstallFactoryNative(Motion::Kind kind, std::unique_ptr<Motion::Behaviour> native)
{
    const uint32 before = m_arbiter.LastSeq();
    m_arbiter.InstallDefault(kind);
    const bool bound = BindNative(before, std::move(native));   // before the hooks, as Request does it
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
    if (m_owner->GetTypeId() == TYPEID_UNIT && !m_owner->Blocked(Motion::ReasonPossessed))
    {
        Creature* creature = (Creature*)m_owner;
        MANGOS_ASSERT(creature->GetCreatureInfo() != NULL);   // every creature reaching here has one: the default-type reads below assume it
        // A player's pet has no factory default: the old follow default fell through to the
        // idle below too, and the pet code installs its follow itself.
        const CreatureMovementType wanted = creature->GetOwnerGuid().IsPlayer() ? CREATURE_MOVEMENT_IDLE : creature->GetDefaultMovementType();
        if (wanted == CREATURE_MOVEMENT_RANDOM)
        {
            // No factory is registered for a random default: the wander native is installed
            // directly, as the factory constructor built it (no vertical band).
            Geometry::Placement const& spawn = creature->Spawn();
            Motion::WanderBehaviour::Params p;
            p.centre = Motion::Vector3(spawn.X(), spawn.Y(), spawn.Z());
            p.radius = std::max(creature->GetRespawnRadius(), MIN_WANDER_RADIUS);
            p.verticalZ = 0.0f;   // no vertical band: this wander never flies, whatever the creature can do
            InstallFactoryNative(Motion::Kind::Wander, std::unique_ptr<Motion::Behaviour>(new Motion::WanderBehaviour(p)));
            return;
        }
        if (wanted == CREATURE_MOVEMENT_WAYPOINT)
        {
            // Likewise for a waypoint default: the patrol native, loading the default path
            // exactly as InitializeWaypointPath(pathId 0, PATH_NO_PATH) did; an unresolved path
            // is a patrol with no nodes, which holds, as the generator's did.
            Motion::PatrolBehaviour::Params p;
            BuildPatrolParams(*creature, 0, PATH_NO_PATH, 0, 0, p);
            InstallFactoryNative(Motion::Kind::Patrol, std::unique_ptr<Motion::Behaviour>(new Motion::PatrolBehaviour(p)));
            return;
        }
    }
    // Nothing registered for this creature's default type, and every player: the idle native
    // is the default, as the shared idle singleton used to be.
    InstallFactoryNative(Motion::Kind::Idle, std::unique_ptr<Motion::Behaviour>(new Motion::IdleBehaviour()));
}

/**
 * @brief Updates the motion of the unit.
 * @param diff Time difference.
 */
void MotionMaster::UpdateMotion(uint32 diff)
{
    if (!m_arbiter.Evaluate().ticks)
    {
        return;   // the block (P5-A): the selected behaviour is paused; nothing moves
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
    RefreshPatrolPath(*m_owner, *bound->behaviour);
    // Identity is the binding's sequence, not a generator pointer: a native has no generator.
    const uint32 tickingSeq = bound->seq;
    const bool alive = bound->behaviour->Tick(*m_owner, diff);
    const std::optional<Motion::Held> now = m_arbiter.Selected();
    if (!alive && now && now->seq == tickingSeq)
    {
        bound = Find(tickingSeq);   // the tick may have re-entered the facade; re-find
        if (!bound)
        {
            return;
        }
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
 * @brief Clears the held behaviours.
 * @param reset Whether the survivor resets.
 * @param all Whether the default goes too.
 */
void MotionMaster::Clear(bool reset, bool all)
{
    Scope scope(*this, all ? Motion::TransactionKind::ClearAll : Motion::TransactionKind::Clear);
    m_pendingReset = (reset && !all) ? PendingReset::Always : PendingReset::None;   // before the hooks: a later call in the same scope wins, as the stack's flag did
    m_arbiter.Clear(all);
    // A partial clear leaves a selected Control claim alone, its reset included: what lies
    // beneath resumes through the arbiter when the claim ends.
    if (!all)
    {
        std::optional<Motion::Held> selected = m_arbiter.Selected();
        if (selected && selected->claim != 0)   // Held::claim is non-zero for Control entries only
        {
            m_pendingReset = PendingReset::None;
        }
    }
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
    Request(R(Motion::Kind::Idle), std::unique_ptr<Motion::Behaviour>(new Motion::IdleBehaviour()));
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
    Motion::WanderBehaviour::Params p;
    p.centre = Motion::Vector3(x, y, z);
    if (radius < MIN_WANDER_RADIUS)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "MotionMaster: wander radius too small, clamped to %f", MIN_WANDER_RADIUS);
    }
    p.radius = std::max(radius, MIN_WANDER_RADIUS);
    p.verticalZ = verticalZ;   // whether it actually flies is the native's live Services::CanFly() read, tick by tick
    Request(R(Motion::Kind::Wander), std::unique_ptr<Motion::Behaviour>(new Motion::WanderBehaviour(p)));
}

/**
 * @brief Moves the unit to its home position.
 */
void MotionMaster::MoveTargetedHome()
{
    if (m_arbiter.Reasons() & (Motion::ReasonFeared | Motion::ReasonPossessed))
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
        Bound const* current = SelectedBound();
        if (!current || !current->behaviour->GetResetPosition(*m_owner, x, y, z, o))
        {
            Geometry::Placement const& home = static_cast<Creature*>(m_owner)->Spawn();
            x = home.X();
            y = home.Y();
            z = home.Z();
            o = home.Facing();
        }
        Motion::HomeBehaviour::Params p;
        p.home = Motion::Vector3(x, y, z);
        p.facing = o;
        Request(R(Motion::Kind::Home), std::unique_ptr<Motion::Behaviour>(new Motion::HomeBehaviour(p)));
    }
    else if (m_owner->GetTypeId() == TYPEID_UNIT && ((Creature*)m_owner)->GetCharmerOrOwnerGuid())
    {
        if (Unit* target = ((Creature*)m_owner)->GetCharmerOrOwner())
        {
            DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s follow to %s", m_owner->GetGuidStr().c_str(), target->GetGuidStr().c_str());
            Request(R(Motion::Kind::Follow), std::unique_ptr<Motion::Behaviour>(
                        new Motion::FollowBehaviour(MakeFollowParams(*target, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE))));
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
 * @param claim The claim's identity (Motion::ControlClaim); 0 derives the script identity.
 */
void MotionMaster::MoveConfused(uint64 claim)
{
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s move confused", m_owner->GetGuidStr().c_str());
    Motion::ConfusedBehaviour::Params p;
    p.radius = sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_CONFUSE_RADIUS);
    Request(R(Motion::Kind::Confused, 0, false, claim ? claim : kScriptConfuse), std::unique_ptr<Motion::Behaviour>(new Motion::ConfusedBehaviour(p)));
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
    Motion::ChaseBehaviour::ChaseParams p;
    p.target = target->GetObjectGuid().GetRawValue();   // resolved per tick; never a stored pointer (design v2 §3.2)
    p.offset = dist;
    p.angle = angle;
    p.routineMs = 1000;   // retail's observed ~1 Hz drift re-check, in place of the generator's 100 ms poll
    p.lead = sWorld.getConfig(CONFIG_BOOL_MOVEMENT_CHASE_LEAD);   // the experiment (Movement.ChaseLead), off by default
    p.leadMs = 500;
    Request(R(Motion::Kind::Chase), std::unique_ptr<Motion::Behaviour>(new Motion::ChaseBehaviour(p)));
}

/**
 * @brief Makes the unit follow a target.
 * @param target Pointer to the target unit.
 * @param dist Distance to maintain from the target.
 * @param angle Angle to maintain from the target.
 */
void MotionMaster::MoveFollow(Unit* target, float dist, float angle)
{
    if (m_arbiter.Reasons() & (Motion::ReasonFeared | Motion::ReasonPossessed))
    {
        return;
    }
    Clear();
    if (!target)
    {
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s follow to %s", m_owner->GetGuidStr().c_str(), target->GetGuidStr().c_str());
    Request(R(Motion::Kind::Follow), std::unique_ptr<Motion::Behaviour>(
                new Motion::FollowBehaviour(MakeFollowParams(*target, dist, angle))));
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
    Motion::PointBehaviour::Params p;
    p.kind = Motion::Kind::Point;
    p.id = id;
    p.goal = Motion::Vector3(x, y, z);
    p.flags = generatePath ? Motion::MOVE_NONE : Motion::MOVE_STRAIGHT;
    Request(R(Motion::Kind::Point, id), std::unique_ptr<Motion::Behaviour>(new Motion::PointBehaviour(p)));
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
    Motion::PointBehaviour::Params p;
    p.kind = Motion::Kind::AssistRun;
    p.goal = Motion::Vector3(x, y, z);
    p.flags = Motion::MOVE_WALK;   // it walks, so the players it is fetching have a chance to catch it
    p.informs = false;             // the assistance finisher replaces the point's: it never informed
    Request(R(Motion::Kind::AssistRun), std::unique_ptr<Motion::Behaviour>(new Motion::PointBehaviour(p)));
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
    Request(R(Motion::Kind::AssistDistract), std::unique_ptr<Motion::Behaviour>(new Motion::DistractBehaviour(Motion::Kind::AssistDistract, time)));
}

/**
 * @brief Makes the unit flee from an enemy.
 * @param enemy Pointer to the enemy unit.
 * @param time Time limit for the fleeing movement (a creature's low-health runner); a player's is always unbounded.
 * @param claim The claim's identity (Motion::ControlClaim); 0 derives a script identity from the enemy.
 */
void MotionMaster::MoveFleeing(Unit* enemy, uint32 time, uint64 claim)
{
    if (!enemy)
    {
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s flee from %s", m_owner->GetGuidStr().c_str(), enemy->GetGuidStr().c_str());
    Motion::FearBehaviour::Params p;
    p.fright = enemy->GetObjectGuid().GetRawValue();   // resolved at each pick through the port; never a stored pointer
    p.timeLimitMs = m_owner->GetTypeId() != TYPEID_PLAYER ? time : 0;   // the generator chose its timed class by the same test
    const uint64 identity = claim ? claim : Motion::ControlClaim(0, 1, enemy->GetObjectGuid().GetCounter());
    // A refreshed aura of the same identity binds this fresh native and retires the running
    // one (the arbiter's in-place update, design fact 5): a fresh pick, a fresh rest, a fresh clock.
    Request(R(Motion::Kind::Fear, 0, false, identity), std::unique_ptr<Motion::Behaviour>(new Motion::FearBehaviour(p)));
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
    if (ActiveKind() == Motion::Kind::Patrol)
    {
        sLog.outError("Creature %s (Entry %u) attempt to MoveWaypoint() but creature is already using waypoint", m_owner->GetGuidStr().c_str(), m_owner->GetEntry());
        return;
    }
    Creature* creature = (Creature*)m_owner;
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s start MoveWaypoint()", m_owner->GetGuidStr().c_str());
    Motion::PatrolBehaviour::Params p;
    BuildPatrolParams(*creature, id, (WaypointPathOrigin)source, initialDelay, overwriteEntry, p);
    Request(R(Motion::Kind::Patrol), std::unique_ptr<Motion::Behaviour>(new Motion::PatrolBehaviour(p)));
}

/**
 * @brief Holds a waypoint patrol where it stands.
 * @param ms How long to hold before the patrol goes on.
 * @return True when the selected behaviour was a patrol and took the pause.
 */
bool MotionMaster::PauseWaypoints(int32 ms)
{
    Bound* bound = SelectedBound();
    if (!bound || bound->behaviour->Kind() != Motion::Kind::Patrol)
    {
        return false;
    }
    NativeBehaviour* adapter = bound->behaviour.get();
    Motion::PatrolBehaviour* patrol = static_cast<Motion::PatrolBehaviour*>(adapter->Native());
    adapter->PerformStep(*m_owner, patrol->Pause(ms));
    return true;
}

/**
 * @brief A player's taxi flight over the whole route, as one native.
 * @param route The node ids in order, the source first (PlayerTaxi::GetTaxiDestinations).
 * @param startNode The first hop's path node the flight starts toward.
 * @param mountDisplayId The taxi mount's display id.
 */
void MotionMaster::MoveTaxiFlight(std::vector<uint32> const& route, uint32 startNode, uint32 mountDisplayId)
{
    if (m_owner->GetTypeId() != TYPEID_PLAYER)
    {
        sLog.outError("%s attempt taxi over %u nodes", m_owner->GetGuidStr().c_str(), uint32(route.size()));
        return;
    }
    Motion::TaxiBehaviour::Params p;
    p.speed = sWorld.getConfig(CONFIG_FLOAT_MOVEMENT_TAXI_SPEED);
    p.mountDisplayId = mountDisplayId;
    p.startNode = startNode;
    // The hops welded into one node array (design §5): the seam node once -- the incoming hop's
    // last row, kept and marked -- and the outgoing hop's node 0 dropped, as the hop chaining's
    // pathNode = 1 skipped it. The weld itself is TaxiRoute::Weld's, shared with the resume so
    // that the landing time is measured over exactly the polyline this spline is built from.
    std::vector<TaxiRouteNode> welded;
    if (!TaxiRoute::Weld(route, welded))
    {
        sLog.outError("%s attempt taxi over a missing or degenerate path along a route of %u nodes", m_owner->GetGuidStr().c_str(), uint32(route.size()));
        static_cast<Player*>(m_owner)->m_taxi.ClearTaxiDestinations();
        return;
    }
    p.nodes.reserve(welded.size());
    for (size_t i = 0; i < welded.size(); ++i)
    {
        Motion::TaxiBehaviour::Node node;
        node.mapId = welded[i].mapId;
        node.pos = Motion::Vector3(welded[i].x, welded[i].y, welded[i].z);
        node.arrivalEvent = welded[i].arrivalEvent;
        node.departureEvent = welded[i].departureEvent;
        node.seam = welded[i].seam;
        p.nodes.push_back(node);
    }
    if (p.nodes.empty() || startNode >= p.nodes.size())
    {
        sLog.outError("%s attempt taxi from node %u of a route of %u path nodes", m_owner->GetGuidStr().c_str(), startNode, uint32(p.nodes.size()));
        static_cast<Player*>(m_owner)->m_taxi.ClearTaxiDestinations();
        return;
    }
    // The landing snaps onto the destination's TaxiNodes position when it has one (a spell
    // taxi's node may not): the notes' SMSG_MOVE_TELEPORT 2.19 yd below the last path node.
    if (TaxiNodesEntry const* destination = sTaxiNodesStore.LookupEntry(route.back()))
    {
        if (destination->Pos_0 != 0.0f || destination->Pos_1 != 0.0f || destination->Pos_2 != 0.0f)
        {
            p.hasLanding = true;
            p.landing = Motion::Vector3(destination->Pos_0, destination->Pos_1, destination->Pos_2);
        }
    }
    // THE LANDING TIME, stamped once and only once (design 2026-09-22 §2): the whole remaining
    // route's flyable length over the very speed this spline is laid at. It is the takeoff that
    // stamps it -- a resume finds one already set and keeps it, because the contract was bought
    // at the click and a passenger who spent ten minutes in a battleground does not get those
    // ten minutes added to his flight. PlayerTaxi::ClearTaxiDestinations drops the stamp with
    // the route, so the next takeoff always finds zero.
    Player* passenger = static_cast<Player*>(m_owner);
    if (!passenger->m_taxi.GetLandingTime())
    {
        const float remaining = TaxiResume::Length(welded, startNode);
        passenger->m_taxi.SetLandingTime(uint32(sWorld.GetGameTime()) + uint32(remaining / p.speed + 0.5f));
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s taxi from node %u to node %u (%u path nodes, from %u, landing at %u)",
                     m_owner->GetGuidStr().c_str(), route.front(), route.back(), uint32(p.nodes.size()), startNode,
                     passenger->m_taxi.GetLandingTime());
    Request(R(Motion::Kind::Taxi), std::unique_ptr<Motion::Behaviour>(new Motion::TaxiBehaviour(p)));
}

/**
 * @brief The worldport ack of a flight's map crossing: the next leg, or the end of the flight.
 */
void MotionMaster::TaxiContinue()
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    std::optional<Motion::Held> taxi = m_arbiter.Command(Motion::Layer::Taxi);
    Bound* bound = taxi ? Find(taxi->seq) : NULL;
    if (!bound || !bound->activated)
    {
        return;
    }
    NativeBehaviour* adapter = bound->behaviour.get();
    Motion::TaxiBehaviour* flight = static_cast<Motion::TaxiBehaviour*>(adapter->Native());
    if (!flight->CrossingLandedOn(m_owner->GetMapId()))
    {
        // Not the map the crossing aimed at (the transfer bounced back), or no crossing was
        // pending: the flight ends where the mover stands (TaxiAbort).
        // reset = false as SendDoFlight's expiry: nothing lies under a taxi to re-lay; the
        // battleground pop keeps the handler's MovementExpired() as the tree had it.
        MovementExpired(false);
        return;
    }
    // No Evaluate().ticks gate as the other direct resumes carry: a taxi is never blocked by a
    // stun, a root or a possession (Mobility::Decide returns before them for Selected::Taxi);
    // only death precedes it, and a dead passenger's flight is finished, never resumed.
    bound->behaviour->Resume(*m_owner, true);   // the next map's leg; the flags and the revoke are still in place
}

/**
 * @brief Makes the unit distracted for a specified time.
 * @param timer Time limit for the distraction.
 */
void MotionMaster::MoveDistract(uint32 timer)
{
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s distracted (timer: %u)", m_owner->GetGuidStr().c_str(), timer);
    Request(R(Motion::Kind::Distract), std::unique_ptr<Motion::Behaviour>(new Motion::DistractBehaviour(Motion::Kind::Distract, timer)));
}

/**
 * @brief Makes the unit jump to a point.
 * @param x X-coordinate of the destination.
 * @param y Y-coordinate of the destination.
 * @param z Z-coordinate of the destination.
 * @param horizontalSpeed The horizontal speed of the jump.
 * @param max_height The height of the parabola.
 * @param id ID of the movement.
 * @return False when the jump was refused: a rooted unit is never displaced by an arc.
 */
bool MotionMaster::MoveJump(float x, float y, float z, float horizontalSpeed, float max_height, uint32 id)
{
    Motion::EffectLaunch launch;
    launch.kind = Motion::EffectLaunch::Jump;
    launch.point = Motion::Vector3(x, y, z);
    launch.speed = horizontalSpeed;
    launch.height = max_height;
    return RequestEffect(id, launch);
}

/**
 * @brief Makes the unit jump to a position.
 * @param pos The destination.
 * @param horizontalSpeed The horizontal speed of the jump.
 * @param max_height The height of the parabola.
 * @param id ID of the movement.
 * @return False when the jump was refused.
 */
bool MotionMaster::MoveJump(Position& pos, float horizontalSpeed, float max_height, uint32 id)
{
    return MoveJump(pos.x, pos.y, pos.z, horizontalSpeed, max_height, id);
}

/**
 * @brief A jump that ends FACING something -- a target, or a given orientation.
 *
 * Kept where mangos_two teleports instead (its EffectJump ends in NearTeleportTo
 * with a TODO). A spline the client can see is the better answer, so this is the
 * implementation that wins. Since P5-B family 1 it is an Effect like every other
 * jump -- it was the one arc that laid no behaviour, so nothing guarded it, nothing
 * cancelled what it displaced, and a rooted caster was displaced by it.
 * @param x X-coordinate of the destination.
 * @param y Y-coordinate of the destination.
 * @param z Z-coordinate of the destination.
 * @param o The orientation to end in when there is no target.
 * @param horizontalSpeed The horizontal speed of the jump.
 * @param max_height The height of the parabola.
 * @param target The unit to end facing, NULL for `o`.
 * @return False when the jump was refused.
 */
bool MotionMaster::MoveJump(float x, float y, float z, float o, float horizontalSpeed, float max_height, Unit* target)
{
    Motion::EffectLaunch launch;
    launch.kind = Motion::EffectLaunch::Jump;
    launch.point = Motion::Vector3(x, y, z);
    launch.speed = horizontalSpeed;
    launch.height = max_height;
    launch.facing = target ? Motion::FacingTarget(target->GetObjectGuid()) : Motion::Facing::ToAngle(o);
    return RequestEffect(0, launch);
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
    Motion::EffectLaunch launch;
    launch.kind = Motion::EffectLaunch::Fall;
    launch.point = Motion::Vector3(m_owner->Where().X(), m_owner->Where().Y(), tz);
    RequestEffect(0, launch);   // never refused: a fall is not a Jump, and the dying flyer's is requested after the death inhibit
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
    // liftOff is not read: the leg is a straight line through the air either way, and which
    // it is is already implied by the height of the destination.
    Motion::PointBehaviour::Params p;
    p.kind = Motion::Kind::FlyLand;
    p.id = id;
    p.goal = Motion::Vector3(x, y, z);
    p.flags = Motion::MOVE_FLY | Motion::MOVE_STRAIGHT;
    Request(R(Motion::Kind::FlyLand, id), std::unique_ptr<Motion::Behaviour>(new Motion::PointBehaviour(p)));
}

/**
 * @brief The charge: a point that follows its target's contact point (P5-B family 1 section 6).
 *
 * The old charge was a raw MonsterMoveWithSpeed to one fixed contact point, so the spell
 * stopped a creature target first to keep that point true. The kernel's point re-lays its
 * leg as the target moves, so the target is no longer stopped: it keeps running and the
 * charge re-targets it. The point is requested with `resumeCombat`, so the chase the spell's
 * Attack installs is held beneath it and resumes the moment the charge ends.
 * @param target The unit charged; nothing happens without one.
 * @param speed The charge's speed in yards per second.
 */
void MotionMaster::MoveCharge(Unit* target, float speed)
{
    if (!target)
    {
        return;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s charges %s at %f", m_owner->GetGuidStr().c_str(), target->GetGuidStr().c_str(), speed);
    // The first leg's goal, the same call the adapter makes for every re-lay:
    // 3.666666 instead of ATTACK_DISTANCE(5.0f) gives the more accurate result.
    float x, y, z;
    ContactPointNear(*target, m_owner, x, y, z, 3.666666f);
    Motion::PointBehaviour::Params p;
    p.kind = Motion::Kind::Point;
    p.id = 0;
    p.goal = Motion::Vector3(x, y, z);
    p.flags = Motion::MOVE_FORCE_DEST;   // routed, and it arrives at the exact contact point
    p.speed = speed;
    // A self-targeted charge (a positive charge effect on the caster) takes the fixed goal: its
    // own contact point would move with it and a tracked leg would never end.
    p.target = target == m_owner ? 0 : target->GetObjectGuid().GetRawValue();
    p.informs = false;                   // the raw spline it replaces informed nothing
    Request(R(Motion::Kind::Point, 0, true), std::unique_ptr<Motion::Behaviour>(new Motion::PointBehaviour(p)));
}

/**
 * @brief The swoop: the charge's destination form, a fixed goal with no target to track.
 * @param x X-coordinate of the goal.
 * @param y Y-coordinate of the goal.
 * @param z Z-coordinate of the goal.
 * @param speed The charge's speed in yards per second.
 */
void MotionMaster::MoveCharge(float x, float y, float z, float speed)
{
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "%s charges to (X: %f Y: %f Z: %f) at %f", m_owner->GetGuidStr().c_str(), x, y, z, speed);
    Motion::PointBehaviour::Params p;
    p.kind = Motion::Kind::Point;
    p.id = 0;
    p.goal = Motion::Vector3(x, y, z);
    p.flags = Motion::MOVE_FORCE_DEST;
    p.speed = speed;
    p.informs = false;
    Request(R(Motion::Kind::Point, 0, true), std::unique_ptr<Motion::Behaviour>(new Motion::PointBehaviour(p)));
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
 * @brief The held patrol native wherever it sits; the newest when two are held.
 * @return The patrol behaviour, or NULL.
 */
Motion::PatrolBehaviour* MotionMaster::HeldPatrol()
{
    for (size_t i = m_bound.size(); i-- > 0;)   // newest first: a patrol pushed over a parked factory patrol is the one the readers mean, as the stack's top-down search found it
    {
        if (m_bound[i].behaviour->Kind() == Motion::Kind::Patrol)
        {
            return static_cast<Motion::PatrolBehaviour*>(m_bound[i].behaviour->Native());
        }
    }
    return NULL;
}

/**
 * @brief The held patrol native wherever it sits; the newest when two are held.
 * @return The patrol behaviour, or NULL.
 */
Motion::PatrolBehaviour const* MotionMaster::HeldPatrol() const
{
    for (size_t i = m_bound.size(); i-- > 0;)   // newest first: a patrol pushed over a parked factory patrol is the one the readers mean, as the stack's top-down search found it
    {
        if (m_bound[i].behaviour->Kind() == Motion::Kind::Patrol)
        {
            return static_cast<Motion::PatrolBehaviour const*>(m_bound[i].behaviour->Native());
        }
    }
    return NULL;
}

/**
 * @brief The selected native's re-lay counters, by cause.
 * @return The counts, or NULL when the selection counts nothing (only the tracking
 *         natives keep them; design v2 §5: GM-dumpable).
 */
Motion::RelayCounts const* MotionMaster::SelectedRelays() const
{
    Bound const* bound = SelectedBound();
    if (!bound)
    {
        return NULL;
    }
    return bound->behaviour->Native()->Relays();
}

/**
 * @brief The facing the selected native's driver last asked for.
 * @return The running leg's facing mode, or the hold's once the leg has finished;
 *         Motion::Facing::Mode::None when nothing is selected.
 */
Motion::Facing::Mode MotionMaster::SelectedLegFacingMode() const
{
    Bound const* bound = SelectedBound();
    if (!bound)
    {
        return Motion::Facing::Mode::None;
    }
    return bound->behaviour->LegFacingMode();
}

/**
 * @brief Sets the next waypoint for the unit.
 * @param pointId ID of the next waypoint.
 * @return True if the next waypoint was successfully set, false otherwise.
 */
bool MotionMaster::SetNextWaypoint(uint32 pointId)
{
    for (size_t i = m_bound.size(); i-- > 0;)   // newest first, as HeldPatrol scans
    {
        if (m_bound[i].behaviour->Kind() != Motion::Kind::Patrol)
        {
            continue;
        }
        NativeBehaviour* adapter = m_bound[i].behaviour.get();
        if (!static_cast<Motion::PatrolBehaviour*>(adapter->Native())->SetNextWaypoint(pointId))
        {
            return false;
        }
        adapter->ResetLeg();   // the driver's leg, not the native's own state: the caller's job
        return true;
    }
    return false;
}

/**
 * @brief Gets the last reached waypoint.
 * @return The ID of the last reached waypoint.
 */
uint32 MotionMaster::getLastReachedWaypoint() const
{
    Motion::PatrolBehaviour const* patrol = HeldPatrol();
    return patrol ? patrol->LastReached() : 0;
}

/**
 * @brief Gets the waypoint path information.
 * @param oss Output stream to store the waypoint path information.
 */
void MotionMaster::GetWaypointPathInformation(std::ostringstream& oss) const
{
    Motion::PatrolBehaviour const* patrol = HeldPatrol();
    if (!patrol)
    {
        return;
    }
    oss << "WaypointMovement: Last Reached WP: " << patrol->LastReached() << " ";
    oss << "(Loaded path " << patrol->PathId() << " from " << WaypointManager::GetOriginString(WaypointPathOrigin(patrol->Origin())) << ")\n";
}

/**
 * @brief The held patrol's loaded path id and origin.
 * @param pathId Filled with the path id.
 * @param origin Filled with the path's origin.
 * @return True when a patrol is held.
 */
bool MotionMaster::GetWaypointPathInformation(int32& pathId, WaypointPathOrigin& origin) const
{
    Motion::PatrolBehaviour const* patrol = HeldPatrol();
    if (!patrol)
    {
        return false;
    }
    pathId = patrol->PathId();
    origin = WaypointPathOrigin(patrol->Origin());
    return true;
}

/**
 * @brief Extends (or cuts short) the selected patrol's pause at its current node.
 * @param ms The time delta (a positive value shortens the wait, a negative one extends it).
 * @return True when the selected behaviour is a patrol.
 */
bool MotionMaster::AddToSelectedPatrolPause(int32 ms)
{
    Bound* bound = SelectedBound();
    if (!bound || bound->behaviour->Kind() != Motion::Kind::Patrol)
    {
        return false;
    }
    static_cast<Motion::PatrolBehaviour*>(bound->behaviour->Native())->AddToPauseTime(ms);
    return true;
}

/**
 * @brief The selected patrol's current node.
 * @return The node id, or 0 when the selection is not a patrol.
 */
uint32 MotionMaster::SelectedPatrolNode() const
{
    Bound const* bound = SelectedBound();
    if (!bound || bound->behaviour->Kind() != Motion::Kind::Patrol)
    {
        return 0;
    }
    return static_cast<Motion::PatrolBehaviour const*>(bound->behaviour->Native())->CurrentNode();
}

/**
 * @brief The selected patrol's welded leg's point count (the harness's welding measurement).
 * @return The point count, or 0 when the selection is not a patrol.
 */
size_t MotionMaster::SelectedPatrolLegPoints() const
{
    Bound const* bound = SelectedBound();
    if (!bound || bound->behaviour->Kind() != Motion::Kind::Patrol)
    {
        return 0;
    }
    return static_cast<Motion::PatrolBehaviour const*>(bound->behaviour->Native())->LegPointCount();
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
    InstallFactoryNative(Motion::Kind::Idle, std::unique_ptr<Motion::Behaviour>(new Motion::IdleBehaviour()));   // never doomed: survives the death's own guard
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
 * @brief Combat ended without a death or an evade: the Combat entry finishes as TargetLost;
 *        the feign's apply uses it.
 */
void MotionMaster::ExpireCombat()
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    m_arbiter.Expire(Motion::Kind::Chase);
}

/**
 * @brief Ends one Control claim by identity; the newest remaining claim of the layer drives.
 * @param claim The claim's identity (Motion::ControlClaim).
 */
bool MotionMaster::ReleaseControl(uint64 claim)
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    return m_arbiter.Release(claim);
}

/**
 * @brief Whether any Control claim of this kind is held.
 * @param kind Fear or Confused.
 * @return True when at least one claim of the kind is in the model.
 */
bool MotionMaster::HoldsControl(Motion::Kind kind) const
{
    return m_arbiter.HasClaim(kind);
}

/**
 * @brief Feeds the kernel's block: the reason's source begins.
 * @param what The inhibition.
 * @param source The source's identity (Motion::InhibitSource or a ControlClaim-shaped aura identity).
 */
void MotionMaster::Inhibit(Motion::Inhibition what, uint64 source)
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    m_arbiter.Inhibit(what, source);
    ProjectClientRoot();
}

/**
 * @brief Feeds the kernel's block: the reason's source ends.
 */
void MotionMaster::Uninhibit(Motion::Inhibition what, uint64 source)
{
    Scope scope(*this, Motion::TransactionKind::Normal);
    m_arbiter.Uninhibit(what, source);
    ProjectClientRoot();
}

/**
 * @brief The mover authority changed (a grant or a revoke): recompute the client root, which
 * depends on it. Safe to call when nothing changes -- the projection is edge-triggered on
 * m_clientRooted.
 */
void MotionMaster::RefreshClientRoot()
{
    ProjectClientRoot();
}

/**
 * @brief The client's root flag follows rooted-or-stunned, on the aggregate's edges only, so two
 * roots and a stun releasing in any order leave the mover rooted exactly until the last one goes.
 * A stunned server-driven unit is stopped, not rooted, as before (the stun handler's StopMoving);
 * a stunned CLIENT MOVER gets the root (reference 2.3) -- a player, or a unit a session is
 * actually moving. The authority can change while a block is held, which is why the grant and the
 * revoke call RefreshClientRoot above: Unit::TakePossessOf raises the Possessed inhibition before
 * SetClientControl hands the body over, so a creature stunned before the take is first projected
 * as the server-driven unit it still is and only the grant's recompute roots it.
 */
void MotionMaster::ProjectClientRoot()
{
    const bool want = m_arbiter.Inhibited(Motion::Inhibition::Rooted) ||
                      (m_owner->IsClientMover() && m_arbiter.Inhibited(Motion::Inhibition::Stunned));
    if (want == m_clientRooted)
    {
        return;
    }
    m_clientRooted = want;
    m_owner->SetRoot(want);
}

/**
 * @brief Publishes the shell's view of the block (P5-C2): the arbiter's reasons at the end of a
 * commit (settled, or the unsettled fallback's), where the P5-A unit-state mirror wrote its bits,
 * and the feign apart; assigned whole.
 */
void MotionMaster::Publish()
{
    PublishedState next;
    next.reasons = static_cast<uint8>(m_arbiter.Reasons() & kPublishedReasons);
    // An aura's fear apart from the AI's own low-health flee, which raises the same reason.
    // Read from the claims, so it ends exactly when the claim does, however the claim ends.
    next.auraFear = m_arbiter.HasAuraClaim(Motion::Kind::Fear);
    std::vector<uint64> const& dead = m_arbiter.Sources(Motion::Inhibition::Dead);
    for (size_t i = 0; i < dead.size(); ++i)
    {
        if (dead[i] != Motion::kDeathSource)
        {
            next.feign = true;
        }
    }
    const bool wasAuraFear = m_published.auraFear;
    m_published = next;   // assigned FIRST: UpdateSpeed below reads Unit::IsFearedByAura() out of it

    // Retail's x1.25 for a fear aura (Unit::UpdateSpeed) follows the published flag, so that
    // every route by which the claim can begin or end is covered by construction -- the aura's
    // own Release, the possession take's CancelControl (Unit.cpp:7149), a full Clear, the death,
    // and any route nobody has enumerated yet. Enumerating them is what went wrong twice before:
    // the shell can only patch the callers it has thought of, and the flag is the one place they
    // all pass through.
    //
    // Safe at this point, checked rather than assumed:
    //  - No recursion. Commit() runs from Scope::~Scope BEFORE m_depth is decremented, so m_depth
    //    is still 1 here; any facade call made from below would open a NESTED scope, which never
    //    commits and so never publishes. A publish cannot re-enter a publish.
    //  - Nothing below opens one anyway. UpdateSpeed reaches back into the kernel exactly twice:
    //    Unit::PropagateSpeedChange -> MotionMaster::PropagateSpeedChange, which only walks
    //    m_bound calling SpeedChanged() (no scope, no arbiter mutation, no queued event) -- and
    //    it is the right moment for it, the selection having just been reconciled; and
    //    SetSpeedRate's CallForAllControlledUnits, which touches pets and charms, each with its
    //    own MotionMaster and its own published state.
    //  - The map phase. SetSpeedRate emits, and Unit::SendEmissions calls AssertMotionOwner.
    //    Publish() runs inside a facade call already bound by that same rule, and emitting from
    //    inside a commit is what the outcome effects already do (Effect::RestoreGait -> SetWalk
    //    -> SendEmissions). No new ownership class is introduced.
    //  - Cost. Only on a change, which is twice per fear episode.
    if (next.auraFear != wasAuraFear)
    {
        m_owner->UpdateSpeed(MOVE_RUN, true);
    }
}

/**
 * @brief A native's Latch effect (P5-C3) on the channel its kind names: set first, then clear, so
 *        a write that does both to one latch ends cleared, as the generators' order did.
 * @param kind The emitting native's kind.
 * @param set Motion::LatchBit bits to set.
 * @param clear Motion::LatchBit bits to clear.
 */
void MotionMaster::WriteLatches(Motion::Kind kind, uint8 set, uint8 clear)
{
    bool* presence = NULL;
    bool* leg = NULL;
    switch (kind)
    {
        case Motion::Kind::Chase:
            presence = &m_latches.chase;
            leg = &m_latches.chaseLeg;
            break;
        case Motion::Kind::Follow:
            presence = &m_latches.follow;
            leg = &m_latches.followLeg;
            break;
        case Motion::Kind::Fear:
            leg = &m_latches.fearLeg;
            break;
        case Motion::Kind::Confused:
            leg = &m_latches.confusedLeg;
            break;
        case Motion::Kind::Idle:
        case Motion::Kind::Wander:
        case Motion::Kind::Patrol:
        case Motion::Kind::Point:
        case Motion::Kind::FlyLand:
        case Motion::Kind::Home:
        case Motion::Kind::AssistRun:
        case Motion::Kind::Distract:
        case Motion::Kind::AssistDistract:
        case Motion::Kind::Effect:
        case Motion::Kind::Taxi:
        case Motion::Kind::Count:
            break;   // no channel: the roaming pair is WriteRoaming's, the Home's wipe WipeLatches'
    }
    if (presence && (set & Motion::LatchPresence))
    {
        *presence = true;
    }
    if (leg && (set & Motion::LatchLeg))
    {
        *leg = true;
    }
    if (presence && (clear & Motion::LatchPresence))
    {
        *presence = false;
    }
    if (leg && (clear & Motion::LatchLeg))
    {
        *leg = false;
    }
}

/**
 * @brief A Step's or an Outcome's roaming write (P5-C3): the roaming pair, whoever emits it.
 * @param what The write the native asked for.
 */
void MotionMaster::WriteRoaming(Motion::Roaming what)
{
    switch (what)
    {
        case Motion::Roaming::SetBoth:
            m_latches.roaming = true;
            m_latches.roamingLeg = true;
            break;
        case Motion::Roaming::ClearMove:
            m_latches.roamingLeg = false;
            break;
        case Motion::Roaming::ClearBoth:
            m_latches.roaming = false;
            m_latches.roamingLeg = false;
            break;
        case Motion::Roaming::SetRoam:
            m_latches.roaming = true;
            break;
        case Motion::Roaming::SetMove:
            m_latches.roamingLeg = true;
            break;
        case Motion::Roaming::Keep:
            break;
    }
}

/**
 * @brief The Home native's first-tick wipe (P5-C3): every latch, the published block (P5-C2) and
 *        the dynamic unit-state bits that remain (UNIT_STAT_ALL_DYN_STATES: melee attacking,
 *        attacking a player, isolated), as the natives' old opaque clear of that mask did.
 */
void MotionMaster::WipeLatches()
{
    m_owner->clearUnitState(UNIT_STAT_ALL_DYN_STATES);
    ClearPublished();
    ClearAllLatches();
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
    // A blocked behaviour is already suspended and stays that way across the relocation; the
    // lift's own Resumed(Blocked) relays it from the new spot, so this pair runs only when the
    // selection ticks.
    if (bound && bound->activated && m_arbiter.Evaluate().ticks)
    {
        bound->behaviour->Suspend(*m_owner);
    }
    m_owner->GetMap()->CreatureRelocation((Creature*)m_owner, x, y, z, o);
    m_owner->SendHeartBeat();
    // The relocation and the heartbeat may have changed what is selected; resume whatever
    // is selected now, as the stack applied its Reset to whatever ended up on top.
    bound = SelectedBound();
    if (bound && bound->activated && m_arbiter.Evaluate().ticks)
    {
        bound->behaviour->Resume(*m_owner, true);
    }
}

/**
 * @brief Whether this arbiter sequence is the one selected right now.
 * @param seq The sequence to test (a native binding's own, from BindNative).
 * @return True when it is the current selection: a native's shell reads this after every
 *         round's effects, to notice they replaced or removed it before applying its intent.
 */
bool MotionMaster::IsSelectedSequence(uint32 seq) const
{
    std::optional<Motion::Held> selected = m_arbiter.Selected();
    return selected && selected->seq == seq;
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
 * @brief The held chase's target.
 * @return The Combat entry's chase target, or NULL when no chase is held.
 */
Unit* MotionMaster::ChaseTarget() const
{
    std::optional<Motion::Held> const& combat = m_arbiter.Combat();
    Bound const* bound = combat ? Find(combat->seq) : NULL;
    if (!bound || bound->behaviour->Kind() != Motion::Kind::Chase)
    {
        return NULL;
    }
    // The held entry's guid, resolved now: the pointer the generator stored through its
    // FollowerReference is gone, and a target that has left the world answers NULL here.
    const uint64 guid = bound->behaviour->Native()->Target();
    return ObjectLookup::GetUnit(*m_owner, ObjectGuid(guid));
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
    if (!bound)
    {
        return NULL;
    }
    const uint64 guid = bound->behaviour->Native()->Target();
    return ObjectLookup::GetUnit(*m_owner, ObjectGuid(guid));
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
 * @brief The selected native's variant.
 * @return Behaviour::Variant() of the selected native, 0 when nothing is selected.
 */
uint32 MotionMaster::SelectedVariant() const
{
    Bound const* bound = SelectedBound();
    return bound ? bound->behaviour->Native()->Variant() : 0;
}

/**
 * @brief Whether the selected behaviour can reach its goal.
 * @return The selected behaviour's answer; true when nothing is selected.
 */
bool MotionMaster::IsReachable() const
{
    Bound const* bound = SelectedBound();
    return !bound || bound->behaviour->Reachable();
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
        view.kind = m_bound[i].behaviour->Kind();
        view.selected = &m_bound[i] == selected;
        view.reachable = m_bound[i].behaviour->Reachable();
        view.target = m_bound[i].behaviour->TrackedTarget();
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
