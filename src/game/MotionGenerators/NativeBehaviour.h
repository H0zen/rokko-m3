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

#ifndef MANGOS_NATIVEBEHAVIOUR_H
#define MANGOS_NATIVEBEHAVIOUR_H

#include "Behaviour.h"        // the shell's MotionBehaviour
#include "MotionDriver.h"
#include "BehaviourModel.h"   // the kernel's (src/motion is on the include path, as Arbiter.h is)
#include "Utilities/Errors.h" // MANGOS_ASSERT, for U()

#include <memory>

/**
 * A native kernel behaviour as a shell behaviour (P5-B family 1 design section 3): owns the
 * native and a MotionDriver, fills the Sight, applies the Step's intent through the driver
 * or the launcher, writes the roaming pair, and performs an Outcome's recipe in order with
 * its predicates read live. The projection (the legacy type) is mapped here from the kind.
 * Since P5-B family 2 it is also the native's Motion::Services port: every draw and every
 * route the native asks for runs here, over the unit and the adapter's own path query.
 */
class NativeBehaviour : public MotionBehaviour, private Motion::Services
{
    public:
        explicit NativeBehaviour(std::unique_ptr<Motion::Behaviour> native);
        ~NativeBehaviour() override;
        NativeBehaviour(NativeBehaviour const&) = delete;
        NativeBehaviour& operator=(NativeBehaviour const&) = delete;

        Motion::Kind Kind() const override { return m_native->Kind(); }
        MovementGeneratorType LegacyType() const override;
        void Activate(Unit& owner) override;
        void Suspend(Unit& owner) override;
        void Resume(Unit& owner, bool reset) override;
        void Finish(Unit& owner, Motion::FinishReason why) override;
        bool Tick(Unit& owner, uint32 diff) override;
        Motion::FinishReason EndReason(Unit& owner) const override;
        MovementGenerator* Legacy() override { return NULL; }
        MovementGenerator const* Legacy() const override { return NULL; }
        void SpeedChanged() override { m_driver.OnSpeedChanged(); }
        bool GetResetPosition(Unit& owner, float& x, float& y, float& z, float& o) const override;
        bool Reachable() const override { return m_driver.Reachable(); }
        uint64 TrackedTarget() const override { return m_native->TracksTarget() ? m_native->Target() : 0; }

        /// The projection of a kind (the facade's legacy type answer).
        static MovementGeneratorType Project(Motion::Kind kind);

        /// The arbiter sequence this binding was given, set once by MotionMaster::BindNative
        /// right after construction: what the tick's per-round IsSelectedSequence re-check reads.
        void SetSequence(uint32 seq) { m_seq = seq; }

        /// The native this adapter drives (MotionMaster::HeldPatrol and friends read it).
        Motion::Behaviour* Native() { return m_native.get(); }
        Motion::Behaviour const* Native() const { return m_native.get(); }

        /// The facing mode of the intent the driver last acted on (MotionMaster::SelectedLegFacingMode).
        Motion::Facing::Mode LegFacingMode() const { return m_driver.LegFacingMode(); }

        /// Forgets the driver's leg (MotionMaster::SetNextWaypoint, after the native accepts
        /// the jump: the leg the driver was tracking no longer applies).
        void ResetLeg() { m_driver.ResetLeg(); }

        /// Runs one Step through the shell (MotionMaster::PauseWaypoints, for a step the
        /// native handed back directly rather than through Activate/Resume/Tick).
        void PerformStep(Unit& owner, Motion::Step const& step) { Perform(owner, step); }

    private:
        // ---- Motion::Services (private: only the native calls these, through the Behaviour hooks) ----
        bool RandomPoint(Motion::Vector3 const& centre, float radius, Motion::Vector3& out) override;
        bool Ground(Motion::Vector3 const& at, float& z) override;
        float Frand(float min, float max) override;
        uint32 Urand(uint32 min, uint32 max) override;
        int32 Irand(int32 min, int32 max) override;
        Motion::RouteResult Route(Motion::Vector3 const& from, Motion::Vector3 const& to, Motion::PointsArray& points) override;
        void ResetRoute() override;
        bool CanMove() const override;
        bool Casting() const override;
        bool WaypointPaused() const override;
        bool Anchor(Motion::Vector3& out) const override;
        bool CanFly() const override;
        bool StandingSpot(Motion::Vector3 const& center, float distance2d, float absAngle, Motion::Vector3& out) override;

        /// The owner of the moment, for the Services implementations below: asserts m_unit was
        /// set (every hook sets it before the native can call back through the port).
        Unit& U() const { MANGOS_ASSERT(m_unit); return *m_unit; }

        Motion::Sight See(Unit& owner, bool tick);   ///< tick: consume the driver's edges; else read the live spline only
        /// Fills the Sight's TargetView from a resolved target: the frame rule, the live
        /// position, the reaches and the classified velocity (design §3).
        void SeeTarget(Unit& owner, Unit& target, Motion::TargetView& view) const;
        /// A Step's shell operations, in order: stop, interrupt, resetLeg, the roaming write, the effects.
        void PerformOps(Unit& owner, Motion::Step const& step);
        /// A Step's intent tail: the launcher's arc, or the driver's Apply with the goal converted from world.
        void ApplyIntent(Unit& owner, Motion::Step const& step);
        /// Both halves, for the hooks that have no selection to re-check between them.
        void Perform(Unit& owner, Motion::Step const& step);
        void PerformOutcome(Unit& owner, Motion::Outcome const& outcome);
        /// The effects loop, creature-only, in order: an Outcome's recipe or a Step's mid-tick set.
        void PerformEffects(Unit& owner, std::vector<Motion::Effect> const& effects);
        void Launch(Unit& owner, Motion::EffectLaunch const& launch);
        void Roam(Unit& owner, Motion::Roaming what);

        std::unique_ptr<Motion::Behaviour> m_native;
        MotionDriver       m_driver;
        Motion::MoveStatus m_last;      ///< the last tick's status (EndReason and the hooks read it)
        bool               m_suspended; ///< Suspend ran since the last Activate/Resume: the mover belongs to another behaviour
        uint32             m_seq = 0;   ///< this binding's arbiter sequence (IsSelectedSequence)

        // ---- the Services port's own state: the owner of the moment, and the adapter's router ----
        /// Set at the start of every hook, before the native is called; mutable because the
        /// const GetResetPosition hook is a hook like any other and the port's owner of the
        /// moment is not observable state.
        mutable Unit* m_unit = NULL;
        std::unique_ptr<Motion::IPathQuery> m_query; ///< the router: dropped at every welding pass (ResetRoute), shared by the legs within one
        Motion::FrameKind m_queryFrame = Motion::FrameKind::World;    ///< the frame m_query was built for; a leg never spans two frames, so a change rebuilds it
        uint32             m_queryMapId = 0;      ///< the map m_query was built for
        uint32             m_queryInstanceId = 0; ///< and the instance: the mesh query is per instance
        /// The last Sight's TargetView, kept for the effects: EngageInReach tests the mover's
        /// live position against the observation the native decided from, not a fresh one.
        Motion::TargetView m_targetView;
};

#endif // MANGOS_NATIVEBEHAVIOUR_H
