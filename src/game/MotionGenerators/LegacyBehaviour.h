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

#ifndef MANGOS_LEGACYBEHAVIOUR_H
#define MANGOS_LEGACYBEHAVIOUR_H

#include "Behaviour.h"

/**
 * A legacy MovementGenerator as a behaviour (design §4, the hook matrix). Owns the
 * generator unless it is the shared idle singleton. Only the selected behaviour
 * ticks; the hooks map onto Initialize/Interrupt/Reset/Finalize per class, and a
 * replaced or cancelled behaviour gets Interrupt plus the cleanup Interrupt leaves
 * undone, never the full Finalize the stack ran only on completion or Clear.
 */
class LegacyBehaviour : public MotionBehaviour
{
    public:
        LegacyBehaviour(Motion::Kind kind, MovementGenerator* generator, bool owned, EffectLaunch const& launch = EffectLaunch());
        ~LegacyBehaviour() override;
        LegacyBehaviour(LegacyBehaviour const&) = delete;
        LegacyBehaviour& operator=(LegacyBehaviour const&) = delete;

        Motion::Kind Kind() const override { return m_kind; }
        MovementGeneratorType LegacyType() const override;
        void Activate(Unit& owner) override;
        void Suspend(Unit& owner) override;
        void Resume(Unit& owner, bool reset) override;
        void Finish(Unit& owner, Motion::FinishReason why) override;
        bool Tick(Unit& owner, uint32 diff) override;
        Motion::FinishReason EndReason(Unit& owner) const override;
        MovementGenerator* Legacy() override { return m_generator; }
        MovementGenerator const* Legacy() const override { return m_generator; }
        void SpeedChanged() override;
        bool GetResetPosition(Unit& owner, float& x, float& y, float& z, float& o) const override;

    private:
        void Launch(Unit& owner);                 ///< the Effect's spline, once, at first selection
        void CleanupAfterInterrupt(Unit& owner);  ///< the state bits Interrupt leaves set
        bool Landed(Unit const& owner) const;     ///< the Effect's spline ran out, uncut

        Motion::Kind       m_kind;
        MovementGenerator* m_generator;
        bool               m_owned;
        EffectLaunch       m_launch;
        bool               m_suspended;   ///< Suspend ran since the last Activate/Resume: the mover belongs to another behaviour
};

#endif
