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

#ifndef MANGOS_MOTION_ARBITER_H
#define MANGOS_MOTION_ARBITER_H

#include "Platform/Define.h"
#include <array>
#include <memory>
#include <optional>
#include <vector>

/**
 * The movement kernel's arbiter (design v2 §4): which behaviour a unit runs,
 * and why the others stopped. Seven layers, one entry each except Control,
 * which holds a set of claims by identity (§4.1); three policies between
 * them (§4.2); every public operation is a transaction whose generation
 * decides whether a request issued from inside a hook survives (§4.3); a
 * fixed ring of decisions for the GM dump. Nothing here knows a unit, a
 * driver, a map or a clock: the shell above (P3-B) delivers the events this
 * class accumulates and tells it when a leg or a timer ended. The legacy
 * MovementGeneratorType conversions live with the shim, outside this seam.
 */
namespace Motion
{
    enum class Kind : uint8
    {
        Idle, Wander, Patrol, Follow,      ///< Default layer
        Chase,                             ///< Combat
        Point, FlyLand, Home, AssistRun,   ///< Scripted
        Distract, AssistDistract,          ///< Distract
        Fear, Confused,                    ///< Control (claims)
        Effect,                            ///< Forced
        Taxi,                              ///< Taxi
        Count
    };

    enum class Layer : uint8 { Default, Combat, Scripted, Distract, Control, Forced, Taxi, Count };

    enum class Policy : uint8 { Supersede, Suspend, Override };

    enum class FinishReason : uint8
    {
        Arrived, Cut, Blocked, Expired, Superseded, Overridden, Cleared, Cancelled, TargetLost, Died
    };

    /// Kernel-external events the policy table maps (§4.2 event rows).
    enum class ExternalEvent : uint8 { CombatStarted };

    /// What the outermost transaction is, which decides the fate of requests
    /// issued from inside it (§4.3 generations).
    enum class TransactionKind : uint8 { Normal, Clear, ClearAll, Death };

    Layer       LayerOf(Kind kind);                      ///< the §4.1 table
    Policy      PolicyOf(Kind kind, bool resumeCombat);  ///< the §4.2 table; Point flips to Suspend with resumeCombat
    bool        SelfExpiring(Kind kind);                 ///< Home, Distract, Effect: cancelled by any other-layer request
    char const* KindName(Kind kind);
    char const* LayerName(Layer layer);
    char const* ReasonName(FinishReason reason);

    /// One request to run a behaviour.
    struct MoveRequest
    {
        Kind   kind;
        uint32 id = 0;               ///< the MovementInform id the shim projects, 0 when none
        bool   resumeCombat = false; ///< Point only: Suspend instead of Override (D2)
        uint64 claim = 0;            ///< Control only: the identity of the claim (the aura); never 0 there
    };

    /// One held entry, wherever it sits.
    struct Held
    {
        Held() : kind(Kind::Idle), id(0), seq(0), claim(0), generation(0), doomed(false), started(false) {}
        Kind   kind;
        uint32 id;         ///< MovementInform id, 0 when none
        uint32 seq;        ///< arrival order; newest wins ties and tells a resume from a fresh start
        uint64 claim;      ///< Control entries only, else 0
        uint32 generation; ///< the transaction that created it
        bool   doomed;     ///< created inside a discarding transaction: finished at its commit
        bool   started;    ///< it has been the selection at least once
    };

    /// One selection event, drained by the shell after a mutating call.
    struct Event
    {
        enum class Kind : uint8 { Finished, Suspended, Resumed, DefaultSwapped };
        Kind         kind;
        Motion::Kind who;
        uint32       id;
        FinishReason reason;   ///< Finished/DefaultSwapped: why; Suspended: Cut; Resumed: Arrived
        uint64       claim;    ///< Control entries only, else 0
        uint32       seq;      ///< the entry's sequence
    };

    /// One line of the decision ring: what was asked, what was selected before and after.
    struct Decision
    {
        enum class Op : uint8
        {
            InstallDefault, Request, Clear, ClearAll, ExpireSelected, Expire, FinishSelected,
            CancelControl, Release, Notify, Die, Commit
        };
        Op           op;
        Motion::Kind kind;        ///< the operation's kind argument (Idle when none)
        uint32       id;
        uint64       claim;
        bool         hadBefore;
        Held         before;
        bool         hadAfter;
        Held         after;
        uint32       generation;
    };

    char const* OpName(Decision::Op op);

    class Arbiter;

    /**
     * The scope of one mutation (§4.3). The outermost guard sets the kind and
     * advances the generation; nested guards join it. When the outermost one
     * ends, entries created inside a discarding kind (Clear, ClearAll, Death)
     * are finished: this is how a request a finalizer issues during a clear or
     * a death is visible while the hook runs and gone when the operation ends.
     * A nested Death guard escalates the outer one to Death too: nothing a
     * hook requests after it survives. A nested Clear or ClearAll discards for
     * its own extent only — what a hook requests while it is open is doomed,
     * what the enclosing operation requests after it closes survives
     * (MoveTargetedHome clears, then asks for Home). The guard is stack-only
     * and must not outlive the arbiter it references.
     */
    class Transaction
    {
        public:
            Transaction(Arbiter& arbiter, TransactionKind kind);
            ~Transaction();
            Transaction(Transaction const&) = delete;
            Transaction& operator=(Transaction const&) = delete;

        private:
            Arbiter&        m_arbiter;
            bool            m_outermost;
            bool            m_raised;    ///< a nested Clear/ClearAll raised the kind for this guard's extent
            TransactionKind m_restore;   ///< the kind a raised nested guard puts back when it ends
    };

    /**
     * The pure selection core. One Default entry with the factory default
     * retained beneath a pushed one, one Combat entry, one command per layer
     * above them (Scripted, Distract, Forced, Taxi), and the Control claim
     * set. Only decides: no unit, no driver, no clock.
     */
    class Arbiter
    {
        public:
            Arbiter();

            /// Factory default: swap, nothing cancelled. The factory default survives
            /// any guard, so the shell's post-death InstallDefault(Kind::Idle) may run
            /// inside the death's own transaction.
            void InstallDefault(Kind kind);
            /// Generic request entry: derives layer and policy from the kind, applies
            /// self-expiry (Home/Distract/Effect) and the policy's cancellation effects.
            /// A Control kind adds or updates the claim of `request.claim` (never 0).
            void Request(MoveRequest const& request);
            /// The Clear(reset, all) projection: drop every command and the combat entry,
            /// pop a pushed default; `all` takes the default and the Control claims too —
            /// a partial clear leaves the claims, which end only through their identity.
            void Clear(bool all);
            /// MovementExpired / Update()==false on whatever is currently selected; a
            /// selected Control claim is left alone.
            void ExpireSelected();
            /// Finish the highest entry of this kind, as the stack expiring that generator
            /// would: a command Expired, combat TargetLost, a Follow default TargetLost with
            /// its fallback restored; any other default, or no entry of that kind, is a no-op.
            void Expire(Kind kind);
            /// Finish whatever is currently selected, for the given reason.
            void FinishSelected(FinishReason reason);
            /// Release every Control claim of this kind (a take that ends the episode without its aura: the pet possession take).
            void CancelControl(Kind kind);
            /// Release one Control claim by identity.
            /// @return True when a claim of that identity was held and is now finished.
            bool Release(uint64 claim);

            /// Apply an event row (§4.2): CombatStarted cancels the Distract layer.
            void Notify(ExternalEvent event);
            /// Death: finish everything as Died, ascending layer order; the model is empty after.
            void Die();
            /// The current transaction generation (advances with each outermost transaction).
            uint32 Generation() const { return m_generation; }
            /// True while the outermost open transaction is Clear, ClearAll or Death.
            bool InDiscardingTransaction() const;

            /// True when nothing is selected (no default, no combat, no command, no claim).
            bool Empty() const;
            /// The currently selected entry, if any.
            std::optional<Held> Selected() const;
            /// The layer the current selection lives on, if any.
            std::optional<Layer> SelectedLayer() const;
            /// The Default-layer entry, if any.
            std::optional<Held> const& Default() const { return m_default; }
            /// The Combat-layer entry, if any.
            std::optional<Held> const& Combat() const { return m_combat; }
            /// The newest sequence handed out; an entry created by the last request has a greater one than any before it.
            uint32 LastSeq() const { return m_seq; }
            /// The factory default parked beneath a pushed one, if any (not part of Contents()).
            std::optional<Held> const& Fallback() const { return m_fallbackDefault; }
            /// True while events are queued.
            bool HasEvents() const { return !m_events.empty(); }
            /// The entry held on a command layer: for Control, the selected claim.
            /// Empty for Default and Combat, which have their own accessors.
            std::optional<Held> Command(Layer layer) const;
            /// Every Control claim, in precedence order (the selected one first).
            std::vector<Held> Claims() const;
            /// Whether any Control claim of this kind is held.
            bool HasClaim(Kind kind) const;
            /// Every held entry, ascending layer order (Default, Combat, then the commands,
            /// the claims in precedence order on the Control layer).
            std::vector<Held> Contents() const;
            /// True when this sequence is still held: an entry Contents() would list, or the
            /// parked fallback. The allocation-free answer to the question Contents() is
            /// otherwise built to ask, for the shell's per-binding, per-tick sweeps.
            bool Holds(uint32 seq) const;
            /// Take and clear the accumulated events.
            std::vector<Event> DrainEvents();

            /// Ring capacity: at most this many decisions kept for Decisions().
            static constexpr size_t kRingSize = 32;
            /// Allocate the decision ring; off by default (a resident 3 KB per unit buys nothing outside a GM session). Idempotent.
            void EnableRing();
            bool RingEnabled() const { return m_ring != nullptr; }
            /// The decision ring, oldest first: every public mutation with the selection before and after.
            std::vector<Decision> Decisions() const;

        private:
            /// Finish the Default-layer entry and promote the factory default beneath it, if any.
            void PopDefault(FinishReason reason);
            /// Finish the entry in `slot`, if any, logging Finished and clearing it.
            void Finish(std::optional<Held>& slot, FinishReason reason);
            /// Finish and erase the claim at `index`.
            void FinishClaim(size_t index, FinishReason reason);
            /// Repeatedly finish the highest-ranked claim of `kind` (Kind::Count for
            /// every claim), in precedence order, for `reason`.
            void FinishClaimsOfKind(Kind kind, FinishReason reason);
            /// The body of FinishSelected without its own transaction or ring record,
            /// so a caller that wants its own label can wrap it.
            void FinishSelectedNoRecord(FinishReason reason);
            /// Compare the selection before and after a mutation and log Suspended/Resumed.
            void Reselect(std::optional<Held> const& before);
            /// True when some entry Contents() would list (not the fallback) still has this seq.
            bool StillHeld(uint32 seq) const;
            /// The held entry with this `seq` (m_default, m_combat, the command slots other
            /// than the unused Control one, or a claim), or NULL.
            Held* HeldBySeq(uint32 seq);
            /// Apply a Default-layer request (§4.2): swap, Idle-as-command, Follow fallback.
            void RequestDefault(MoveRequest const& request, Held const& held, Policy policy);
            /// Apply a command-layer request: supersede the layer, then Override cancels below it.
            void RequestCommand(MoveRequest const& request, Held const& held, Layer layer, Policy policy);
            /// Add or update a Control claim.
            void RequestClaim(MoveRequest const& request, Held const& held);
            /// The index of the selected claim (Confused before Fear, then newest), if any.
            std::optional<size_t> SelectedClaimIndex() const;
            /// A fresh Held for this request.
            Held Stamp(Kind kind, uint32 id, uint64 claim);
            /// The outermost transaction ended: finish what it doomed.
            void Commit();
            /// Append one line to the decision ring.
            void Record(Decision::Op op, Kind kind, uint32 id, uint64 claim, std::optional<Held> const& before);
            friend class Transaction;

            std::optional<Held> m_default;         ///< the Default-layer entry
            std::optional<Held> m_fallbackDefault; ///< the factory default beneath a pushed one;
                                                   ///< restored when the stack pops it
            std::optional<Held> m_combat;          ///< the Combat-layer entry
            std::array<std::optional<Held>, static_cast<size_t>(Layer::Count)> m_commands; ///< per-layer commands (Control unused)
            std::vector<Held> m_claims;            ///< the Control claim set, arrival order
            uint32 m_seq;                          ///< monotonic arrival counter
            std::vector<Event> m_events;           ///< accumulated since the last DrainEvents
            uint32 m_generation;                   ///< advanced by each outermost transaction
            uint32 m_depth;                        ///< open transactions
            TransactionKind m_outerKind;           ///< the outermost open one's kind
            bool m_doomedInGeneration;             ///< Stamp doomed an entry since the outermost guard opened; Commit sweeps only then
            std::unique_ptr<std::array<Decision, kRingSize>> m_ring; ///< the decision ring, allocated on demand, next write at m_ringNext
            size_t m_ringNext;                     ///< the next slot to overwrite, wraps at kRingSize
            size_t m_ringCount;                    ///< entries recorded so far, capped at kRingSize
    };
}

#endif
