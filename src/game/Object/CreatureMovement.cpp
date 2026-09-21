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

/**
 * @file CreatureMovement.cpp
 * @brief Cohesion split of Creature.cpp -- creature movement-flag state (walk/swim/fly/hover/levitate/feather-fall/root/water-walk).
 *
 * Same Creature class; no behaviour change. CMake file(GLOB) over Object/
 * picks this file up automatically; Creature.h is unchanged.
 */

#include "Creature.h"
#include "GridMap.h"
#include "GameTime.h"
#include "Common/TimeConstants.h"

namespace
{
    /// How long a patrol holds when a player starts talking to it (HoldForPlayer, below); the
    /// generator's own constant, the sole survivor once WaypointMovementGenerator.h was deleted.
    const uint32 STOP_TIME_FOR_PLAYER = 3 * MINUTE * IN_MILLISECONDS;
}

/**
 * @brief Enables or disables walk mode for the creature.
 *
 * @param enable true to walk; false to run.
 * @param asDefault true to also update the default running state.
 */
void Creature::HoldForPlayer()
{
    StopMoving();
    // The pause is asked for by name: nothing infers it from the stop any more, so a
    // stun or a script's StopMoving() no longer parks a patrol for minutes.
    GetMotionMaster()->PauseWaypoints(STOP_TIME_FOR_PLAYER);
}

void Creature::SetWalk(bool enable, bool asDefault)
{
    if (asDefault)
    {
        if (enable)
        {
            clearUnitState(UNIT_STAT_RUNNING);
        }
        else
        {
            addUnitState(UNIT_STAT_RUNNING);
        }
    }

    // Nothing changed?
    if (enable == m_movementInfo.HasMovementFlag(MOVEFLAG_WALK_MODE))
    {
        return;
    }

    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_WALK_MODE);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_WALK_MODE);
    }

    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::Gait, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables levitation movement flags.
 *
 * @param enable true to levitate; false to clear the flag.
 */
void Creature::SetLevitate(bool enable)
{
    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_LEVITATING);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_LEVITATING);
    }

    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::GravityDisabled, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables swim movement flags and broadcasts the change.
 *
 * @param enable true to swim; false to stop swimming.
 */
void Creature::SetSwim(bool enable)
{
    if (enable == m_movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING))
    {
        return;
    }

    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_SWIMMING);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_SWIMMING);
    }

    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::Swim, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Whether this creature may swim instead of being clamped to the ground.
 */
bool Creature::CanSwim() const
{
    if (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CANT_SWIM))
    {
        return false;
    }

    if ((GetCreatureInfo()->InhabitType & INHABIT_WATER) || HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CAN_SWIM))
    {
        return true;
    }

    if (GetCreatureInfo()->ExtraFlags & CREATURE_FLAG_EXTRA_WALK_IN_WATER)
    {
        return false;
    }

    // a ground creature already in swim mode keeps it until the water is left
    if (IsSwimming())
    {
        return true;
    }

    // not relocated yet during Create(); position is not valid to probe
    if (!IsInWorld())
    {
        return false;
    }

    // the client default: a creature immersed in swim-deep liquid surface-swims
    GridMapLiquidData liquidData;
    return GetTerrain()->IsSwimmable(Where().X(), Where().Y(), Where().Z(),
                                     Where().Extent(), &liquidData) &&
           Where().Z() < liquidData.level + 2.0f; // not on a bridge/ledge above the water body
}

/**
 * @brief Syncs MOVEFLAG_SWIMMING with the liquid at the current position.
 *
 * The spawn-time check in InitEntry only fires once; without this a shore
 * spawned creature pathing through deep water keeps its land movement
 * state (falling animation, run speed). Matches the swim half of TC's
 * Creature::UpdateMovementCapabilities.
 */
void Creature::UpdateSwimmingState()
{
    // client-moved creatures (possess) own their movement flags
    if (Blocked(Motion::ReasonPossessed))
    {
        return;
    }

    // explicit bottom-walkers keep their spawn-time movement flags
    if (GetCreatureInfo()->ExtraFlags & CREATURE_FLAG_EXTRA_WALK_IN_WATER)
    {
        return;
    }

    GridMapLiquidData liquidData;
    bool swimmable = !HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CANT_SWIM) &&
                     GetTerrain()->IsSwimmable(Where().X(), Where().Y(), Where().Z(),
                                               Where().Extent(), &liquidData) &&
                     Where().Z() < liquidData.level + 2.0f; // not on a bridge/ledge above the water body

    SetSwim(swimmable);
}

/**
 * @brief Placeholder for enabling or disabling flight.
 *
 * @param enable Unused flight toggle.
 */
void Creature::SetCanFly(bool enable)
{
    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_CAN_FLY);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_CAN_FLY);
    }

    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::CanFly, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables feather-fall movement behavior.
 *
 * @param enable true to enable feather fall; false to restore normal falling.
 */
void Creature::SetFeatherFall(bool enable)
{
    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_SAFE_FALL);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_SAFE_FALL);
    }

    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::FeatherFall, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables hover movement behavior.
 *
 * @param enable true to hover; false to unset hover.
 */
void Creature::SetHover(bool enable)
{
    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_HOVER);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_HOVER);
    }

    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::Hover, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables root movement behavior.
 *
 * @param enable true to root; false to unroot.
 */
void Creature::SetRoot(bool enable)
{
    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_ROOT);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_ROOT);
    }

    // Server-driven: confirmed at once, the spline form to everyone in range (nothing
    // while out of the world, as before).
    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::Root, enable), GameTime::GetGameTimeMS()));
}

/**
 * @brief Enables or disables water-walking behavior.
 *
 * @param enable true to water walk; false to restore land walking.
 */
void Creature::SetWaterWalk(bool enable)
{
    if (enable)
    {
        m_movementInfo.AddMovementFlag(MOVEFLAG_WATERWALKING);
    }
    else
    {
        m_movementInfo.RemoveMovementFlag(MOVEFLAG_WATERWALKING);
    }

    SendEmissions(m_motion.Apply(Motion::FlagChange(Motion::ChangeType::WaterWalk, enable), GameTime::GetGameTimeMS()));
}
