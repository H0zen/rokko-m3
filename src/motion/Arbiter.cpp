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

#include "Arbiter.h"

#include <cassert>

/// The seam's own debug assert: the invariants below hold by construction and
/// cost nothing in Release.
#ifdef NDEBUG
#define MOTION_ASSERT(cond) ((void)0)
#else
#define MOTION_ASSERT(cond) assert(cond)
#endif

namespace Motion
{
    Layer LayerOf(Kind kind)
    {
        switch (kind)
        {
            case Kind::Idle:
            case Kind::Wander:
            case Kind::Patrol:
            case Kind::Follow:
                return Layer::Default;
            case Kind::Chase:
                return Layer::Combat;
            case Kind::Point:
            case Kind::FlyLand:
            case Kind::Home:
            case Kind::AssistRun:
                return Layer::Scripted;
            case Kind::Distract:
            case Kind::AssistDistract:
                return Layer::Distract;
            case Kind::Fear:
            case Kind::Confused:
                return Layer::Control;
            case Kind::Effect:
                return Layer::Forced;
            case Kind::Taxi:
                return Layer::Taxi;
            default:
                return Layer::Default;
        }
    }

    Policy PolicyOf(Kind kind, bool resumeCombat)
    {
        switch (kind)
        {
            case Kind::Wander:
            case Kind::Patrol:
            case Kind::FlyLand:
            case Kind::Home:
            case Kind::AssistRun:
            case Kind::Taxi:
                return Policy::Override;
            case Kind::Point:
                return resumeCombat ? Policy::Suspend : Policy::Override;
            case Kind::Follow:
            case Kind::Chase:
                return Policy::Supersede;
            default:
                return Policy::Suspend;   // Idle-as-command, Distract, AssistDistract, Fear, Confused, Effect
        }
    }

    bool SelfExpiring(Kind kind)
    {
        return kind == Kind::Home || kind == Kind::Distract || kind == Kind::Effect;
    }

    char const* KindName(Kind kind)
    {
        static char const* const names[] =
        {
            "Idle", "Wander", "Patrol", "Follow", "Chase", "Point", "FlyLand", "Home",
            "AssistRun", "Distract", "AssistDistract", "Fear", "Confused", "Effect", "Taxi"
        };
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Kind::Count),
                      "KindName out of sync with Kind");
        return kind < Kind::Count ? names[static_cast<uint8>(kind)] : "?";
    }

    char const* LayerName(Layer layer)
    {
        static char const* const names[] =
        {
            "Default", "Combat", "Scripted", "Distract", "Control", "Forced", "Taxi"
        };
        static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Layer::Count),
                      "LayerName out of sync with Layer");
        return layer < Layer::Count ? names[static_cast<uint8>(layer)] : "?";
    }

    char const* ReasonName(FinishReason reason)
    {
        static char const* const names[] =
        {
            "Arrived", "Cut", "Blocked", "Expired", "Superseded", "Overridden", "Cleared",
            "Cancelled", "TargetLost", "Died"
        };
        static_assert(sizeof(names) / sizeof(names[0]) == 10, "ReasonName out of sync with FinishReason");
        const size_t index = static_cast<size_t>(reason);
        return index < sizeof(names) / sizeof(names[0]) ? names[index] : "?";
    }

    namespace
    {
        const uint8 FIRST_COMMAND_LAYER = static_cast<uint8>(Layer::Scripted);
        const uint8 LAYER_COUNT = static_cast<uint8>(Layer::Count);
        const uint8 CONTROL = static_cast<uint8>(Layer::Control);

        /// Intra-Control precedence: Confused outranks Fear.
        int ClaimRank(Kind kind)
        {
            return kind == Kind::Confused ? 2 : 1;
        }

        /// True when `a` is selected over `b`: higher rank, then newer.
        bool Outranks(Held const& a, Held const& b)
        {
            return ClaimRank(a.kind) > ClaimRank(b.kind) ||
                   (ClaimRank(a.kind) == ClaimRank(b.kind) && a.seq > b.seq);
        }
    }

    Arbiter::Arbiter() : m_seq(0), m_generation(0), m_depth(0), m_outerKind(TransactionKind::Normal),
        m_ring(), m_ringNext(0), m_ringCount(0)
    {
    }

    Held Arbiter::Stamp(Kind kind, uint32 id, uint64 claim)
    {
        Held h;
        h.kind = kind;
        h.id = id;
        h.seq = ++m_seq;
        h.claim = claim;
        h.generation = m_generation;
        h.doomed = InDiscardingTransaction();
        h.started = false;
        return h;
    }

    Transaction::Transaction(Arbiter& arbiter, TransactionKind kind) : m_arbiter(arbiter), m_outermost(arbiter.m_depth == 0)
    {
        if (m_outermost)
        {
            m_arbiter.m_outerKind = kind;
            ++m_arbiter.m_generation;
        }
        else if (kind == TransactionKind::Death)
        {
            m_arbiter.m_outerKind = TransactionKind::Death;   // absolute: a nested death escalates the outer one
        }
        ++m_arbiter.m_depth;
    }

    Transaction::~Transaction()
    {
        --m_arbiter.m_depth;
        if (m_outermost)
        {
            m_arbiter.Commit();
            m_arbiter.m_outerKind = TransactionKind::Normal;
        }
    }

    bool Arbiter::InDiscardingTransaction() const
    {
        return m_depth > 0 && m_outerKind != TransactionKind::Normal;
    }

    void Arbiter::Commit()
    {
        if (m_outerKind == TransactionKind::Normal)
        {
            return;
        }
        const std::optional<Held> before = Selected();
        const FinishReason reason = m_outerKind == TransactionKind::Death ? FinishReason::Died : FinishReason::Cleared;
        bool swept = false;
        for (uint8 i = FIRST_COMMAND_LAYER; i < LAYER_COUNT; ++i)
        {
            if (m_commands[i] && m_commands[i]->doomed && m_commands[i]->generation == m_generation)
            {
                Finish(m_commands[i], reason);
                swept = true;
            }
        }
        for (;;)   // highest-ranked doomed claim of this generation first, same precedence order as elsewhere
        {
            std::optional<size_t> best;
            for (size_t i = 0; i < m_claims.size(); ++i)
            {
                if (!m_claims[i].doomed || m_claims[i].generation != m_generation)
                {
                    continue;
                }
                if (!best || Outranks(m_claims[i], m_claims[*best]))
                {
                    best = i;
                }
            }
            if (!best)
            {
                break;
            }
            FinishClaim(*best, reason);
            swept = true;
        }
        if (m_combat && m_combat->doomed && m_combat->generation == m_generation)
        {
            Finish(m_combat, reason);
            swept = true;
        }
        while (m_default && m_default->doomed && m_default->generation == m_generation)
        {
            PopDefault(reason);   // a doomed fallback promoted by the pop is swept by the next turn
            swept = true;
        }
        if (m_fallbackDefault && m_fallbackDefault->doomed && m_fallbackDefault->generation == m_generation)
        {
            Finish(m_fallbackDefault, reason);
            swept = true;
        }
        // The sweep above only ever owns this generation's own doomed entries;
        // anything still held past it is no longer at risk from this guard, so
        // its doomed flag is retired here rather than left to outlive the guard
        // that set it (Important 1: a later discarding guard must not inherit it).
        if (m_default)
        {
            m_default->doomed = false;
        }
        if (m_fallbackDefault)
        {
            m_fallbackDefault->doomed = false;
        }
        if (m_combat)
        {
            m_combat->doomed = false;
        }
        for (uint8 i = FIRST_COMMAND_LAYER; i < LAYER_COUNT; ++i)
        {
            if (m_commands[i])
            {
                m_commands[i]->doomed = false;
            }
        }
        for (Held& claim : m_claims)
        {
            claim.doomed = false;
        }
        Reselect(before);
        if (swept)
        {
            Record(Decision::Op::Commit, Kind::Idle, 0, 0, before);
        }
    }

    void Arbiter::Notify(ExternalEvent event)
    {
        Transaction tx(*this, TransactionKind::Normal);
        const std::optional<Held> before = Selected();
        switch (event)
        {
            case ExternalEvent::CombatStarted:
                Finish(m_commands[static_cast<size_t>(Layer::Distract)], FinishReason::Cancelled);
                break;
            default:
                break;
        }
        Reselect(before);
        Record(Decision::Op::Notify, Kind::Idle, static_cast<uint32>(event), 0, before);
    }

    void Arbiter::Die()
    {
        Transaction tx(*this, TransactionKind::Death);
        const std::optional<Held> before = Selected();
        Finish(m_default, FinishReason::Died);
        Finish(m_fallbackDefault, FinishReason::Died);
        Finish(m_combat, FinishReason::Died);
        for (uint8 i = FIRST_COMMAND_LAYER; i < LAYER_COUNT; ++i)
        {
            if (i == CONTROL)
            {
                FinishClaimsOfKind(Kind::Count, FinishReason::Died);
                continue;
            }
            Finish(m_commands[i], FinishReason::Died);
        }
        Record(Decision::Op::Die, Kind::Idle, 0, 0, before);
    }

    void Arbiter::InstallDefault(Kind kind)
    {
        Transaction tx(*this, TransactionKind::Normal);
        const std::optional<Held> before = Selected();
        if (m_default)
        {
            m_events.push_back({Event::Kind::DefaultSwapped, m_default->kind, m_default->id, FinishReason::Superseded, 0});
        }
        m_default = Stamp(kind, 0, 0);
        m_default->doomed = false;
        m_fallbackDefault.reset();
        Reselect(before);
        Record(Decision::Op::InstallDefault, kind, 0, 0, before);
    }

    void Arbiter::Request(MoveRequest const& request)
    {
        Transaction tx(*this, TransactionKind::Normal);
        const Layer layer = LayerOf(request.kind);
        const std::optional<Held> before = Selected();
        if (layer == Layer::Control && request.claim == 0)
        {
            Record(Decision::Op::Request, request.kind, request.id, request.claim, before);
            return;   // a control without an identity cannot be released: refused
        }
        const Policy policy = PolicyOf(request.kind, request.resumeCombat);
        const Held held = Stamp(request.kind, request.id, layer == Layer::Control ? request.claim : 0);

        if (before && SelfExpiring(before->kind) && LayerOf(before->kind) != layer)
        {
            Finish(m_commands[static_cast<size_t>(LayerOf(before->kind))], FinishReason::Cancelled);
        }

        if (layer == Layer::Default)
        {
            RequestDefault(request, held, policy);
        }
        else if (layer == Layer::Combat)
        {
            if (m_combat)
            {
                m_combat->id = held.id;   // D6: a chase on a chasing unit updates it
                m_combat->seq = held.seq;
            }
            else
            {
                m_combat = held;
            }
        }
        else if (layer == Layer::Control)
        {
            RequestClaim(request, held);
        }
        else
        {
            RequestCommand(request, held, layer, policy);
        }
        Reselect(before);
        Record(Decision::Op::Request, request.kind, request.id, request.claim, before);
    }

    void Arbiter::RequestDefault(MoveRequest const& request, Held const& held, Policy policy)
    {
        if (request.kind == Kind::Idle && !Empty())
        {
            std::optional<Held>& scripted = m_commands[FIRST_COMMAND_LAYER];
            if (scripted && scripted->kind == Kind::Idle)
            {
                return;
            }
            Finish(scripted, FinishReason::Superseded);
            scripted = held;
            return;
        }
        if (m_default && !m_fallbackDefault)
        {
            m_fallbackDefault = m_default;
        }
        if (m_default)
        {
            m_events.push_back({Event::Kind::DefaultSwapped, m_default->kind, m_default->id, FinishReason::Superseded, 0});
        }
        m_default = held;
        if (policy == Policy::Override)
        {
            Finish(m_combat, FinishReason::Overridden);
            Finish(m_commands[static_cast<size_t>(Layer::Scripted)], FinishReason::Overridden);
            Finish(m_commands[static_cast<size_t>(Layer::Distract)], FinishReason::Overridden);
        }
    }

    void Arbiter::RequestCommand(MoveRequest const& /*request*/, Held const& held, Layer layer, Policy policy)
    {
        const size_t index = static_cast<size_t>(layer);
        Finish(m_commands[index], FinishReason::Superseded);
        if (policy == Policy::Override)
        {
            for (uint8 lower = FIRST_COMMAND_LAYER; lower < index; ++lower)
            {
                if (lower == CONTROL)
                {
                    continue;   // no Override from a lower layer reaches Control; a Taxi masks the claims (D8)
                }
                Finish(m_commands[lower], FinishReason::Overridden);
            }
            Finish(m_combat, FinishReason::Overridden);
        }
        m_commands[index] = held;
    }

    void Arbiter::RequestClaim(MoveRequest const& request, Held const& held)
    {
        for (Held& claim : m_claims)
        {
            if (claim.claim == request.claim)
            {
                claim.kind = held.kind;
                claim.id = held.id;
                claim.seq = held.seq;
                return;
            }
        }
        m_claims.push_back(held);
    }

    void Arbiter::Clear(bool all)
    {
        Transaction tx(*this, all ? TransactionKind::ClearAll : TransactionKind::Clear);
        const std::optional<Held> before = Selected();
        for (uint8 i = FIRST_COMMAND_LAYER; i < LAYER_COUNT; ++i)
        {
            Finish(m_commands[i], FinishReason::Cleared);
        }
        FinishClaimsOfKind(Kind::Count, FinishReason::Cleared);
        Finish(m_combat, FinishReason::Cleared);
        if (all)
        {
            Finish(m_default, FinishReason::Cleared);
            Finish(m_fallbackDefault, FinishReason::Cleared);
        }
        else if (m_fallbackDefault)
        {
            PopDefault(FinishReason::Cleared);
        }
        Reselect(before);
        Record(all ? Decision::Op::ClearAll : Decision::Op::Clear, Kind::Idle, 0, 0, before);
    }

    void Arbiter::ExpireSelected()
    {
        Transaction tx(*this, TransactionKind::Normal);
        const std::optional<Held> before = Selected();
        const std::optional<Layer> layer = SelectedLayer();
        if (!layer)
        {
            Record(Decision::Op::ExpireSelected, Kind::Idle, 0, 0, before);
            return;
        }
        switch (*layer)
        {
            case Layer::Default:
            {
                if (!m_fallbackDefault)
                {
                    Record(Decision::Op::ExpireSelected, before->kind, 0, 0, before);
                    return;
                }
                PopDefault(m_default->kind == Kind::Follow ? FinishReason::TargetLost : FinishReason::Expired);
                Reselect(before);
                Record(Decision::Op::ExpireSelected, before->kind, 0, 0, before);
                return;
            }
            case Layer::Combat:
                FinishSelectedNoRecord(FinishReason::TargetLost);
                Record(Decision::Op::ExpireSelected, before->kind, 0, 0, before);
                return;
            default:
                FinishSelectedNoRecord(FinishReason::Expired);
                Record(Decision::Op::ExpireSelected, before->kind, 0, 0, before);
                return;
        }
    }

    void Arbiter::Expire(Kind kind)
    {
        Transaction tx(*this, TransactionKind::Normal);
        const std::optional<Held> before = Selected();
        if (LayerOf(kind) == Layer::Control)
        {
            std::optional<size_t> best;
            for (size_t i = 0; i < m_claims.size(); ++i)
            {
                if (m_claims[i].kind != kind)
                {
                    continue;
                }
                if (!best || m_claims[i].seq > m_claims[*best].seq)
                {
                    best = i;
                }
            }
            if (best)
            {
                FinishClaim(*best, FinishReason::Expired);
                Reselect(before);
            }
            Record(Decision::Op::Expire, kind, 0, 0, before);
            return;
        }
        for (uint8 i = LAYER_COUNT; i-- > FIRST_COMMAND_LAYER;)
        {
            if (m_commands[i] && m_commands[i]->kind == kind)
            {
                Finish(m_commands[i], FinishReason::Expired);
                Reselect(before);
                Record(Decision::Op::Expire, kind, 0, 0, before);
                return;
            }
        }
        if (m_combat && m_combat->kind == kind)
        {
            Finish(m_combat, FinishReason::TargetLost);
            Reselect(before);
            Record(Decision::Op::Expire, kind, 0, 0, before);
            return;
        }
        if (m_default && m_default->kind == kind && m_fallbackDefault)
        {
            PopDefault(kind == Kind::Follow ? FinishReason::TargetLost : FinishReason::Expired);
            Reselect(before);
        }
        Record(Decision::Op::Expire, kind, 0, 0, before);
    }

    void Arbiter::FinishSelectedNoRecord(FinishReason reason)
    {
        const std::optional<Held> before = Selected();
        const std::optional<Layer> layer = SelectedLayer();
        if (!layer)
        {
            return;
        }
        if (*layer == Layer::Default)
        {
            PopDefault(reason);
        }
        else if (*layer == Layer::Combat)
        {
            Finish(m_combat, reason);
        }
        else if (*layer == Layer::Control)
        {
            FinishClaim(*SelectedClaimIndex(), reason);
        }
        else
        {
            Finish(m_commands[static_cast<size_t>(*layer)], reason);
        }
        Reselect(before);
    }

    void Arbiter::FinishSelected(FinishReason reason)
    {
        Transaction tx(*this, TransactionKind::Normal);
        const std::optional<Held> before = Selected();
        FinishSelectedNoRecord(reason);
        Record(Decision::Op::FinishSelected, before ? before->kind : Kind::Idle, 0, 0, before);
    }

    void Arbiter::CancelControl(Kind kind)
    {
        Transaction tx(*this, TransactionKind::Normal);
        const std::optional<Held> before = Selected();
        FinishClaimsOfKind(kind, FinishReason::Cancelled);
        Reselect(before);
        Record(Decision::Op::CancelControl, kind, 0, 0, before);
    }

    void Arbiter::Release(uint64 claim)
    {
        Transaction tx(*this, TransactionKind::Normal);
        const std::optional<Held> before = Selected();
        for (size_t i = 0; i < m_claims.size(); ++i)
        {
            if (m_claims[i].claim == claim)
            {
                FinishClaim(i, FinishReason::Cancelled);
                Reselect(before);
                Record(Decision::Op::Release, Kind::Idle, 0, claim, before);
                return;
            }
        }
        Record(Decision::Op::Release, Kind::Idle, 0, claim, before);
    }

    bool Arbiter::Empty() const
    {
        return !Selected();
    }

    std::optional<size_t> Arbiter::SelectedClaimIndex() const
    {
        std::optional<size_t> best;
        for (size_t i = 0; i < m_claims.size(); ++i)
        {
            if (!best || Outranks(m_claims[i], m_claims[*best]))
            {
                best = i;
            }
        }
        return best;
    }

    std::optional<Layer> Arbiter::SelectedLayer() const
    {
        for (uint8 i = LAYER_COUNT; i-- > FIRST_COMMAND_LAYER;)
        {
            if (i == CONTROL)
            {
                if (!m_claims.empty())
                {
                    return Layer::Control;
                }
                continue;
            }
            if (m_commands[i])
            {
                return static_cast<Layer>(i);
            }
        }
        if (m_combat)
        {
            return Layer::Combat;
        }
        if (m_default)
        {
            return Layer::Default;
        }
        return std::nullopt;
    }

    std::optional<Held> Arbiter::Selected() const
    {
        const std::optional<Layer> layer = SelectedLayer();
        if (!layer)
        {
            return std::nullopt;
        }
        if (*layer == Layer::Default)
        {
            return m_default;
        }
        if (*layer == Layer::Combat)
        {
            return m_combat;
        }
        if (*layer == Layer::Control)
        {
            return m_claims[*SelectedClaimIndex()];
        }
        return m_commands[static_cast<size_t>(*layer)];
    }

    std::optional<Held> Arbiter::Command(Layer layer) const
    {
        if (layer == Layer::Default || layer == Layer::Combat)
        {
            return std::nullopt;
        }
        if (layer == Layer::Control)
        {
            const std::optional<size_t> index = SelectedClaimIndex();
            if (!index)
            {
                return std::nullopt;
            }
            return m_claims[*index];
        }
        return m_commands[static_cast<size_t>(layer)];
    }

    std::vector<Held> Arbiter::Claims() const
    {
        std::vector<Held> out(m_claims);
        for (size_t i = 1; i < out.size(); ++i)   // insertion sort: precedence, then newest
        {
            Held key = out[i];
            size_t j = i;
            while (j > 0 && Outranks(key, out[j - 1]))
            {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = key;
        }
        return out;
    }

    std::vector<Held> Arbiter::Contents() const
    {
        std::vector<Held> out;
        if (m_default)
        {
            out.push_back(*m_default);
        }
        if (m_combat)
        {
            out.push_back(*m_combat);
        }
        for (uint8 i = FIRST_COMMAND_LAYER; i < LAYER_COUNT; ++i)
        {
            if (i == CONTROL)
            {
                const std::vector<Held> claims = Claims();
                out.insert(out.end(), claims.begin(), claims.end());
                continue;
            }
            if (m_commands[i])
            {
                out.push_back(*m_commands[i]);
            }
        }
        return out;
    }

    std::vector<Event> Arbiter::DrainEvents()
    {
        std::vector<Event> out;
        out.swap(m_events);
        return out;
    }

    void Arbiter::PopDefault(FinishReason reason)
    {
        Finish(m_default, reason);
        m_default = m_fallbackDefault;
        m_fallbackDefault.reset();
    }

    void Arbiter::Finish(std::optional<Held>& slot, FinishReason reason)
    {
        if (!slot)
        {
            return;
        }
        m_events.push_back({Event::Kind::Finished, slot->kind, slot->id, reason, slot->claim});
        slot.reset();
    }

    void Arbiter::FinishClaim(size_t index, FinishReason reason)
    {
        Held const& c = m_claims[index];
        m_events.push_back({Event::Kind::Finished, c.kind, c.id, reason, c.claim});
        m_claims.erase(m_claims.begin() + static_cast<std::vector<Held>::difference_type>(index));
    }

    void Arbiter::FinishClaimsOfKind(Kind kind, FinishReason reason)
    {
        for (;;)
        {
            std::optional<size_t> best;
            for (size_t i = 0; i < m_claims.size(); ++i)
            {
                if (kind != Kind::Count && m_claims[i].kind != kind)
                {
                    continue;
                }
                if (!best || Outranks(m_claims[i], m_claims[*best]))
                {
                    best = i;
                }
            }
            if (!best)
            {
                break;
            }
            FinishClaim(*best, reason);
        }
    }

    bool Arbiter::StillHeld(uint32 seq) const
    {
        if (m_default && m_default->seq == seq)
        {
            return true;
        }
        if (m_combat && m_combat->seq == seq)
        {
            return true;
        }
        for (uint8 i = FIRST_COMMAND_LAYER; i < LAYER_COUNT; ++i)
        {
            if (m_commands[i] && m_commands[i]->seq == seq)
            {
                return true;
            }
        }
        for (Held const& claim : m_claims)
        {
            if (claim.seq == seq)
            {
                return true;
            }
        }
        return false;
    }

    Held* Arbiter::HeldBySeq(uint32 seq)
    {
        if (m_default && m_default->seq == seq)
        {
            return &*m_default;
        }
        if (m_combat && m_combat->seq == seq)
        {
            return &*m_combat;
        }
        for (uint8 i = FIRST_COMMAND_LAYER; i < LAYER_COUNT; ++i)
        {
            if (i == CONTROL)
            {
                continue;   // the claim set below, not a command slot
            }
            if (m_commands[i] && m_commands[i]->seq == seq)
            {
                return &*m_commands[i];
            }
        }
        for (Held& claim : m_claims)
        {
            if (claim.seq == seq)
            {
                return &claim;
            }
        }
        return NULL;
    }

    void Arbiter::Reselect(std::optional<Held> const& before)
    {
        const std::optional<Held> after = Selected();
        if (after && (!before || before->seq != after->seq))
        {
            if (before && StillHeld(before->seq))
            {
                m_events.push_back({Event::Kind::Suspended, before->kind, before->id, FinishReason::Cut, before->claim});
            }
            // A resume is for an entry that has run before and is being exposed again, not
            // for a masked entry starting for the first time, and not for the entry just
            // requested (the newest arrival, which includes an in-place update).
            if (after->started && after->seq != m_seq)
            {
                m_events.push_back({Event::Kind::Resumed, after->kind, after->id, FinishReason::Arrived, after->claim});
            }
        }
        if (after)
        {
            if (Held* held = HeldBySeq(after->seq))
            {
                held->started = true;
            }
        }
    }

    void Arbiter::Record(Decision::Op op, Kind kind, uint32 id, uint64 claim, std::optional<Held> const& before)
    {
        Decision d;
        d.op = op;
        d.kind = kind;
        d.id = id;
        d.claim = claim;
        d.hadBefore = before.has_value();
        d.before = before ? *before : Held();
        const std::optional<Held> after = Selected();
        d.hadAfter = after.has_value();
        d.after = after ? *after : Held();
        d.generation = m_generation;
        m_ring[m_ringNext] = d;
        m_ringNext = (m_ringNext + 1) % kRingSize;
        if (m_ringCount < kRingSize)
        {
            ++m_ringCount;
        }
#ifndef NDEBUG
        MOTION_ASSERT(!m_fallbackDefault || m_default);          // a fallback never exists without a default
        for (size_t i = 0; i < m_claims.size(); ++i)
        {
            for (size_t j = i + 1; j < m_claims.size(); ++j)
            {
                MOTION_ASSERT(m_claims[i].claim != m_claims[j].claim);   // one claim per identity
            }
        }
#endif
    }

    std::vector<Decision> Arbiter::Decisions() const
    {
        std::vector<Decision> out;
        out.reserve(m_ringCount);
        const size_t first = m_ringCount < kRingSize ? 0 : m_ringNext;
        for (size_t i = 0; i < m_ringCount; ++i)
        {
            out.push_back(m_ring[(first + i) % kRingSize]);
        }
        return out;
    }
}
