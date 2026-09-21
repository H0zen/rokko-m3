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

#ifndef MANGOS_MOTIONMASTER_H
#define MANGOS_MOTIONMASTER_H

#include "Platform/Define.h"
#include "Arbiter.h"
#include "MoveIntent.h"   // Motion::Facing::Mode, returned by value from SelectedLegFacingMode
#include "WaypointManager.h"
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

namespace Geometry { struct Position; }

class Unit;
class NativeBehaviour;

// Creature Entry ID used for waypoints show, visible only for GMs
#define VISUAL_WAYPOINT 1

namespace Motion
{
    class Behaviour;      ///< a native kernel behaviour (src/motion/BehaviourModel.h); the .cpp has the definition
    class PatrolBehaviour; ///< the waypoint patrol native (src/motion/DefaultMoves.h)
    struct EffectLaunch;   ///< a jump, a knockback arc or a fall (src/motion/MoveIntent.h); the .cpp has the definition
    struct RelayCounts;    ///< a tracking native's re-lays by cause (src/motion/BehaviourModel.h)
    enum class Roaming : uint8; ///< a Step's or an Outcome's roaming write (src/motion/BehaviourModel.h)

    /**
     * @brief The identity of a Control claim: the aura that holds it.
     * @param spellId The aura's spell (0 for the low-health flee and for a script's fear).
     * @param effIndex The aura's effect index (0 or 1 for the two spell-less cases).
     * @param casterCounter The caster's guid counter (the victim's for the low-health flee).
     * @return A non-zero identity; two applications of one aura share it and update the claim in place.
     */
    inline uint64 ControlClaim(uint32 spellId, uint8 effIndex, uint32 casterCounter)
    {
        return (uint64(spellId) << 40) | (uint64(effIndex) << 32) | uint64(casterCounter);
    }
}

/**
 * The movement facade and, since P3-B, the kernel's Controller shell (design
 * 2026-09-13-movement-p3b-controller-design.md): one Motion::Arbiter decides which
 * held behaviour runs, one adapted native (NativeBehaviour) behaves per held entry, and
 * every public call is an arbiter transaction whose events reach the behaviours
 * through the hook matrix when the outermost call ends. Only the selected
 * behaviour ticks. The entry points the vendored scripts call are the ones the
 * shim gate pins; the rest is the core's.
 */
class MotionMaster
{
    public:
        explicit MotionMaster(Unit* unit);
        ~MotionMaster();

        /// The factory default: clear everything, install the creature's default movement (idle for players).
        void Initialize();
        /// One tick of the selected behaviour; nothing while the block's decision withholds it (Evaluate().ticks).
        void UpdateMotion(uint32 diff);
        /// Every command and combat finish; the pushed default too when `all`; the survivor resets when `reset && !all`. A Control claim is left alone: it ends with its aura.
        void Clear(bool reset = true, bool all = false);
        /// The selected behaviour finishes; the exposed one resets when `reset` and nothing was pushed over it. A Control claim is left alone: it ends with its aura.
        void MovementExpired(bool reset = true);

        void MoveIdle();
        void MoveRandomAroundPoint(float x, float y, float z, float radius, float verticalZ = 0.0f);
        void MoveTargetedHome();
        void MoveFollow(Unit* target, float dist, float angle);
        void MoveChase(Unit* target, float dist = 0.0f, float angle = 0.0f);
        void MoveConfused(uint64 claim = 0);                                     ///< a Confused claim; 0 derives the script identity
        void MoveFleeing(Unit* enemy, uint32 timeLimit = 0, uint64 claim = 0);   ///< a Fear claim; 0 derives a script identity from the enemy
        void MovePoint(uint32 id, float x, float y, float z, bool generatePath = true);
        void MoveSeekAssistance(float x, float y, float z);
        void MoveSeekAssistanceDistract(uint32 timer);
        void MoveWaypoint(int32 id = 0, uint32 source = 0, uint32 initialDelay = 0, uint32 overwriteEntry = 0);
        /// Holds a waypoint patrol where it stands; true when the selected behaviour was a patrol and took the pause.
        bool PauseWaypoints(int32 ms);
        /// A player's taxi flight over the whole resolved route (the node ids, the source first):
        /// the hops welded into one native flight. startNode indexes the first hop's path nodes
        /// (the closest segment on a resume); the mount display is written at the takeoff.
        void MoveTaxiFlight(std::vector<uint32> const& route, uint32 startNode, uint32 mountDisplayId);
        /// The worldport ack of a flight's map crossing: the next map's leg when this is the map the
        /// crossing aimed at, else the flight expires where the mover stands.
        void TaxiContinue();
        void MoveDistract(uint32 timeLimit);
        /// A jump or a knockback arc. @return False when it was refused: a rooted unit is never displaced by an arc.
        bool MoveJump(float x, float y, float z, float horizontalSpeed, float max_height, uint32 id = 0);
        bool MoveJump(Geometry::Position& pos, float horizontalSpeed, float max_height, uint32 id = 0);
        /// A jump that ends facing a target, or a given orientation (was MoveDestination: a raw spline; now an Effect like every jump).
        bool MoveJump(float x, float y, float z, float o, float horizontalSpeed, float max_height, Unit* target);
        void MoveFall();
        void MoveFlyOrLand(uint32 id, float x, float y, float z, bool liftOff);
        /// The charge (P5-B family 1 section 6): a point that follows its target's contact point at `speed`, routed with a forced destination, informing nothing.
        void MoveCharge(Unit* target, float speed);
        /// The swoop's destination form: a fixed goal at `speed`, informing nothing.
        void MoveCharge(float x, float y, float z, float speed);

        /// An outside reason a behaviour may not move the unit (P5-A, spec §6): the one game-side
        /// path to the kernel's block. Inside one scope it feeds the arbiter, projects the client
        /// root (rooted or stunned: SetRoot on the aggregate's edge only) and publishes the block
        /// at the commit's end (Published).
        void Inhibit(Motion::Inhibition what, uint64 source);
        void Uninhibit(Motion::Inhibition what, uint64 source);
        /// Whether any source holds this reason: the one answer to "is this unit rooted".
        bool Inhibited(Motion::Inhibition what) const { return m_arbiter.Inhibited(what); }
        /// What the selected behaviour may do right now, and why not.
        Motion::MobilityDecision Mobility() const { return m_arbiter.Evaluate(); }

        /// The shell's view of the kernel's block (P5-C2): what the arbiter held at the end of
        /// every commit (settled, or the unsettled fallback's), where the P5-A unit-state mirror
        /// wrote its bits. Never refreshed inside a transaction, so a reader in a nested facade
        /// call (an inform's AI callback, a finisher's effect) sees the previous commit's answer,
        /// as it saw the mirrored bits; the arbiter's live state is the kernel's own (Inhibited,
        /// HoldsControl, Mobility).
        struct PublishedState
        {
            uint8 reasons = 0;     ///< Motion::Reason bits: Rooted, Stunned, Possessed, Feared, Confused, Distracted, OnTaxi; never Dead (a real death is IsAlive()'s)
            bool  feign   = false; ///< a Dead source other than the death's own (Sources(Dead) minus kDeathSource): a feign
        };
        /// The block as of the last commit: the published state (see PublishedState).
        PublishedState const& Published() const { return m_published; }
        /// An outside wipe of the unit's state (a respawn's or a revive's clearUnitState(UNIT_STAT_ALL_STATE),
        /// the Home native's first-tick UNIT_STAT_ALL_DYN_STATES clear): the published state goes
        /// with it at once, as the old mirrored bits went with it, and the next commit
        /// publishes again what the sources still hold (the death keeps its Seat, FixedVehicle and
        /// Possession sources).
        void ClearPublished() { m_published = PublishedState(); }
        /// Player::TaxiAbort's early publication: the flight's end is published before the commit
        /// that finishes it, so the pet's resummon (Player::IsPetNeedBeTemporaryUnsummoned) and the
        /// hostile-state change after it see no flight; the commit's own publication agrees.
        void PublishTaxiEnded() { m_published.reasons = static_cast<uint8>(m_published.reasons & ~Motion::ReasonOnTaxi); }

        /// A native's Latch effect (P5-C3): the LatchBit bits set, then cleared, on the channel the
        /// emitting native's kind names -- a chase's or a follow's presence and leg, a fear's or a
        /// confuse's leg; nothing for any other kind. Shared and destructive, as the unit-state bits
        /// they replace were: a clear clears the channel whoever set it, and a native finishing after
        /// Retire erased its binding still writes it.
        void WriteLatches(Motion::Kind kind, uint8 set, uint8 clear);
        /// A Step's or an Outcome's roaming write (Motion::Roaming): the roaming pair, whoever emits it
        /// (the wander, the patrol, the point family).
        void WriteRoaming(Motion::Roaming what);
        /// The Home native's first-tick wipe (Effect::WipeLatches): every latch (ClearAllLatches), the
        /// published block (P5-C2) and the dynamic unit-state bits that remain (UNIT_STAT_ALL_DYN_STATES:
        /// the melee, attack-player and isolated bits), as the mask the native used to carry cleared.
        void WipeLatches();

        /// The natives' latches (P5-C3): what a chase, a follow, the roaming family, a fear and a
        /// confuse hold and run, written in each recipe's order by WriteLatches, WriteRoaming and
        /// WipeLatches, and cleared by the stops (ClearMovingLatches) and the whole-state wipes
        /// (ClearAllLatches). One bank per unit, shared and destructive as the unit-state bits it
        /// replaces were: a Point's finish clears the roaming pair whoever set it, and a native
        /// finishing after Retire erased its binding still writes its own leg.
        struct LatchBank
        {
            bool chase = false;        ///< a chase native is active (the old UNIT_STAT_CHASE)
            bool chaseLeg = false;     ///< a chase leg runs (the old UNIT_STAT_CHASE_MOVE)
            bool follow = false;       ///< a follow native is active (the old UNIT_STAT_FOLLOW)
            bool followLeg = false;    ///< a follow leg runs (the old UNIT_STAT_FOLLOW_MOVE)
            bool roaming = false;      ///< a wander, a patrol or a point-family native is active (the old UNIT_STAT_ROAMING)
            bool roamingLeg = false;   ///< its leg runs (the old UNIT_STAT_ROAMING_MOVE)
            bool fearLeg = false;      ///< a fear's leg runs (the old UNIT_STAT_FLEEING_MOVE)
            bool confusedLeg = false;  ///< a confuse's lurch runs (the old UNIT_STAT_CONFUSED_MOVE)
            /// A leg in flight (the old UNIT_STAT_MOVING): the confuse's lurch is not counted, as it never was.
            bool Moving() const { return roamingLeg || chaseLeg || followLeg || fearLeg; }
            /// A chase's or a fear's leg (the old UNIT_STAT_RUNNING_STATE, less the creature's RUNNING gait).
            bool RunningLeg() const { return chaseLeg || fearLeg; }
        };
        /// The natives' latches as of the last write (P5-C3; see LatchBank).
        LatchBank const& Latches() const { return m_latches; }
        /// A stop from outside (Unit::StopMoving, the pet AI's opener, the two pet cast handlers, the
        /// distract effect): the legs the old UNIT_STAT_MOVING held, the confuse's lurch excepted.
        void ClearMovingLatches()
        {
            m_latches.roamingLeg = false;
            m_latches.chaseLeg = false;
            m_latches.followLeg = false;
            m_latches.fearLeg = false;
        }
        /// A whole-state wipe (a respawn's, a revive's or a pet revive's clearUnitState(UNIT_STAT_ALL_STATE),
        /// and the Home's WipeLatches): every latch.
        void ClearAllLatches() { m_latches = LatchBank(); }

        void PropagateSpeedChange();
        /// Jumps the held patrol to a given node; it moves there on the next tick. @return False when the node does not exist.
        bool SetNextWaypoint(uint32 pointId);
        /// The last waypoint node the held patrol reached; 0 before the first one.
        uint32 getLastReachedWaypoint() const;
        void GetWaypointPathInformation(std::ostringstream& oss) const;
        /// The held patrol's loaded path id and origin. @return False when no patrol is held.
        bool GetWaypointPathInformation(int32& pathId, WaypointPathOrigin& origin) const;
        /// Extends (or cuts short) the selected patrol's pause at its current node. @return False when the selection is not a patrol.
        bool AddToSelectedPatrolPause(int32 ms);
        /// The selected patrol's current node; 0 when the selection is not a patrol.
        uint32 SelectedPatrolNode() const;
        /// The selected patrol's welded leg's point count (the harness's welding measurement); 0 when the selection is not a patrol.
        size_t SelectedPatrolLegPoints() const;
        bool GetDestination(float& x, float& y, float& z);

        /// Death: every behaviour finishes Died while the unit still reads alive, then the idle default.
        void Die();
        /// Release the control claims of this kind (a take that ends the episode without its aura: the pet possession take).
        void CancelControl(Motion::Kind kind);
        /// Combat ended without a death or an evade: the Combat layer's one kind, Chase (today
        /// the only one), finishes as TargetLost; the feign's apply uses it. Must follow the
        /// layer if another Combat kind is ever added.
        void ExpireCombat();
        /// End one Control claim by identity; the newest remaining claim of the layer drives.
        /// @return True when the claim was held.
        bool ReleaseControl(uint64 claim);
        /// Whether any Control claim of this kind is held (the aura handlers' "last claim" test).
        bool HoldsControl(Motion::Kind kind) const;
        /// A near teleport: suspend the selection, relocate, resume it with a reset.
        void RelocateSelected(float x, float y, float z, float o);
        /// True iff this arbiter sequence is the one selected right now (the native shell's per-round re-check).
        bool IsSelectedSequence(uint32 seq) const;
        // ---- typed queries (P3-C) -------------------------------------------------------
        /// The selected entry's kind: what runs now (the stack's "current type"); Idle when nothing is held.
        Motion::Kind ActiveKind() const;
        /// A chase is held (the Combat entry), selected or masked.
        bool IsChasing() const;
        /// The held chase's target, or NULL: no chase is held, or the target has left the
        /// world (the guid is resolved now, not a stored pointer).
        Unit* ChaseTarget() const;
        /// The current default is a follow (the parked fallback does not count), selected or masked.
        bool IsFollowing() const;
        /// The held follow's target, or NULL: no follow is the default, or the target has
        /// left the world (the guid is resolved now).
        Unit* FollowTarget() const;
        /// The current default is a patrol, selected or masked.
        bool IsPatrolling() const;
        /// A taxi flight is held.
        bool IsOnTaxi() const;
        /// The selected native's Behaviour::Variant(): 0 for every kind but the timed flee's 1
        /// (the harness's timed-flee sample; the old timed-fleeing projection).
        uint32 SelectedVariant() const;
        /// The selected behaviour can reach its goal; true when nothing is selected
        /// (nothing could have reported a failed path: taunts stay where they are).
        bool IsReachable() const;
        /// The combat-started event row (design v2 §4.2): a new combat cancels the Distract layer; ignored from inside a movement operation.
        void CombatStarted();
        /// True when this sequence has a binding and that binding has been activated.
        bool IsActivated(uint32 seq) const;
        /// The selected native's re-lay counters by cause (design v2 §5), else NULL: a native
        /// that counts nothing answers NULL.
        Motion::RelayCounts const* SelectedRelays() const;
        /// The facing the selected native's driver last asked for: the running leg's, or the
        /// hold's once it has finished. None when nothing is selected.
        /// A read for the GM harness, not a script entry point.
        Motion::Facing::Mode SelectedLegFacingMode() const;

        /// One held entry for a listing: what it is, without asking a behaviour for it.
        struct HeldView
        {
            Motion::Kind kind;                   ///< the kernel kind the entry runs under
            bool selected;                       ///< this is the one that ticks
            bool reachable;                      ///< it can still reach its goal
            uint64 target;                       ///< the raw guid it tracks, 0 for a non-tracking native
        };
        /// Every held behaviour in arrival order, the selected one marked.
        std::vector<HeldView> Held() const;
        /// The model, for the GM dump.
        Motion::Arbiter const& Arbiter() const { return m_arbiter; }

    private:
        /// One held entry's behaviour, keyed by the arbiter's sequence.
        struct Bound
        {
            uint32 seq;
            std::unique_ptr<NativeBehaviour> behaviour;
            bool activated;
            Bound(uint32 s, std::unique_ptr<NativeBehaviour> b);
            Bound(Bound&& other) noexcept;
            Bound& operator=(Bound&& other) noexcept;
            ~Bound();
        };
        /// The stack's reset latch: consumed once at the outermost commit.
        enum class PendingReset : uint8 { None, WhenExposed, Always };

        class Scope;   ///< the transaction guard (MotionMaster.cpp)

        /// One facade request whose behaviour is a native of the kernel.
        void Request(Motion::MoveRequest const& request, std::unique_ptr<Motion::Behaviour> native);
        /// One Effect request, through the shell's own gate: a Jump on a rooted unit is refused.
        /// @return False when it was refused; nothing was bound and nothing will inform.
        bool RequestEffect(uint32 id, Motion::EffectLaunch const& launch);
        void InstallFactoryNative(Motion::Kind kind, std::unique_ptr<Motion::Behaviour> native);
        bool BindNative(uint32 seqBefore, std::unique_ptr<Motion::Behaviour> native);
        void SweepStale(Motion::Kind kind);
        void Commit(std::optional<Motion::Transaction>& transaction);
        void DeliverEvents();
        void Deliver(Motion::Event const& event);
        void Reconcile();
        bool IsHeld(uint32 seq) const;
        size_t IndexOf(uint32 seq) const;
        Bound* Find(uint32 seq);
        Bound const* Find(uint32 seq) const;
        Bound* SelectedBound();
        Bound const* SelectedBound() const;
        void Retire(size_t index, Motion::FinishReason reason);
        /// The client root follows the aggregate of Rooted and Stunned: SetRoot on its edges only.
        void ProjectClientRoot();
        /// Publishes the shell's view of the block at the end of every commit, settled or not (P5-C2).
        void Publish();
        /// The held patrol native wherever it sits (default slot, masked or not), else NULL.
        Motion::PatrolBehaviour* HeldPatrol();
        Motion::PatrolBehaviour const* HeldPatrol() const;
        /// Allocate the arbiter's decision ring (Movement.DecisionRing): the constructor's.
        void EnableDecisionRing();

        Unit*              m_owner;
        Motion::Arbiter    m_arbiter;
        std::vector<Bound> m_bound;
        std::vector<std::unique_ptr<NativeBehaviour> > m_retired; ///< finished behaviours, destroyed at the end of the outermost commit (a behaviour ticking when its hook finished it must outlive its own Tick)
        uint32             m_depth;          ///< open scopes
        Motion::TransactionKind m_scopeKind; ///< the kind the outermost commit runs under; a nested death raises it to Death
        PendingReset       m_pendingReset;
        uint32             m_exposedSeq;     ///< WhenExposed: the entry an expiry exposed
        bool               m_clientRooted;   ///< what ProjectClientRoot last told the owner
        PublishedState     m_published;      ///< the block as of the last commit (P5-C2)
        LatchBank          m_latches;        ///< the natives' latches (P5-C3)
};

#endif // MANGOS_MOTIONMASTER_H
