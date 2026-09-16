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

#ifndef MANGOS_BEHAVIOUR_H
#define MANGOS_BEHAVIOUR_H

#include "Platform/Define.h"
#include "Arbiter.h"
#include "MotionMaster.h"
#include "MoveIntent.h"

class Unit;
class MovementGenerator;

/// The kernel's Effect launch (design §4): the parameters MoveJump/MoveFall hand to the
/// Effect native, which asks the adapter to launch the spline once at its activation.
using Motion::EffectLaunch;

/**
 * One held behaviour of the movement kernel's shell (design §3-§4). The arbiter
 * decides which one is selected; the shell calls these hooks in the order the
 * arbiter's events dictate and ticks the selected one. Since P5-B family 1 the seven
 * simple moves are natives of the kernel over the per-unit driver (NativeBehaviour);
 * the eight kinds families 2-4 own still adapt a legacy MovementGenerator.
 */
class MotionBehaviour
{
    public:
        virtual ~MotionBehaviour() {}
        virtual Motion::Kind Kind() const = 0;
        virtual MovementGeneratorType LegacyType() const = 0;       ///< the projection the facade reports
        virtual void Activate(Unit& owner) = 0;                     ///< first selection
        virtual void Suspend(Unit& owner) = 0;                      ///< masked by a higher layer
        virtual void Resume(Unit& owner, bool reset) = 0;           ///< the selection, at every commit; reset = the stack's Reset (a suspended behaviour clears its flag here)
        virtual void Finish(Unit& owner, Motion::FinishReason why) = 0;
        virtual bool Tick(Unit& owner, uint32 diff) = 0;            ///< false: the behaviour ended itself
        virtual Motion::FinishReason EndReason(Unit& owner) const = 0; ///< why, after a false Tick
        virtual MovementGenerator* Legacy() = 0;                    ///< the adapted generator
        virtual MovementGenerator const* Legacy() const = 0;
        virtual void SpeedChanged() = 0;
        virtual bool GetResetPosition(Unit& owner, float& x, float& y, float& z, float& o) const = 0;
        virtual bool Reachable() const = 0;                         ///< the behaviour can reach its goal (the IsReachable contract)
};

#endif
