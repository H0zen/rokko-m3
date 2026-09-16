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

#include <memory>

/**
 * A native kernel behaviour as a shell behaviour (P5-B family 1 design section 3): owns the
 * native and a MotionDriver, fills the Sight, applies the Step's intent through the driver
 * or the launcher, writes the roaming pair, and performs an Outcome's recipe in order with
 * its predicates read live. The projection (the legacy type) is mapped here from the kind.
 */
class NativeBehaviour : public MotionBehaviour
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
        bool GetResetPosition(Unit&, float&, float&, float&, float&) const override { return false; }
        bool Reachable() const override { return m_driver.Reachable(); }

        /// The projection of a kind (the facade's legacy type answer).
        static MovementGeneratorType Project(Motion::Kind kind);

    private:
        Motion::Sight See(Unit& owner, bool tick);   ///< tick: consume the driver's edges; else read the live spline only
        void Perform(Unit& owner, Motion::Step const& step);
        void PerformOutcome(Unit& owner, Motion::Outcome const& outcome);
        void Launch(Unit& owner, Motion::EffectLaunch const& launch);
        void Roam(Unit& owner, Motion::Roaming what);

        std::unique_ptr<Motion::Behaviour> m_native;
        MotionDriver       m_driver;
        Motion::MoveStatus m_last;      ///< the last tick's status (EndReason and the hooks read it)
        bool               m_suspended; ///< Suspend ran since the last Activate/Resume: the mover belongs to another behaviour
};

#endif // MANGOS_NATIVEBEHAVIOUR_H
