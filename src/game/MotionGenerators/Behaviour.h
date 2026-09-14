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

class Unit;
class MovementGenerator;

/// How an Effect launches its spline at first selection (design §4): the parameters
/// MoveJump/MoveFall used to hand to MoveSplineInit right after the push.
struct EffectLaunch
{
    enum Kind : uint8 { None, Jump, Fall };
    Kind  kind;
    float x;
    float y;
    float z;
    float speed;   ///< horizontal speed (jump)
    float height;  ///< parabola height (jump)
    EffectLaunch() : kind(None), x(0.0f), y(0.0f), z(0.0f), speed(0.0f), height(0.0f) {}
};

/**
 * One held behaviour of the movement kernel's shell (design §3-§4). The arbiter
 * decides which one is selected; the shell calls these hooks in the order the
 * arbiter's events dictate and ticks the selected one. In P3-B every behaviour
 * adapts a legacy MovementGenerator; P5 replaces them with natives over the
 * per-unit driver.
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
};

#endif
