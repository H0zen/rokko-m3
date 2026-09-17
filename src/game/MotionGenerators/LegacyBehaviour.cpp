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

#include "LegacyBehaviour.h"
#include "MovementGenerator.h"
#include "Creature.h"
#include "Unit.h"

LegacyBehaviour::LegacyBehaviour(Motion::Kind kind, MovementGenerator* generator, bool owned)
    : m_kind(kind), m_generator(generator), m_owned(owned), m_suspended(false)
{
}

LegacyBehaviour::~LegacyBehaviour()
{
    if (m_owned)
    {
        delete m_generator;   // no hook: the stack deleted without Finalize on destruction too
    }
}

MovementGeneratorType LegacyBehaviour::LegacyType() const
{
    return m_generator->GetMovementGeneratorType();
}

void LegacyBehaviour::Activate(Unit& owner)
{
    m_suspended = false;
    m_generator->Initialize(owner);
}

void LegacyBehaviour::Suspend(Unit& owner)
{
    if (m_suspended)
    {
        return;   // a block's Suspended and a mask's Suspended may both arrive; the generator hears one Interrupt
    }
    m_suspended = true;
    m_generator->Interrupt(owner);   // every kind left here travels: none is inert, a timer or a spline
}

void LegacyBehaviour::Resume(Unit& owner, bool reset)
{
    m_suspended = false;
    if (reset)
    {
        m_generator->Reset(owner);
    }
}

void LegacyBehaviour::Finish(Unit& owner, Motion::FinishReason why)
{
    switch (why)
    {
        case Motion::FinishReason::Superseded:
        case Motion::FinishReason::Overridden:
        case Motion::FinishReason::Cancelled:
            if (!m_suspended)
            {
                m_generator->Interrupt(owner);   // a suspended behaviour was interrupted at its Suspend: the mover is another behaviour's now
            }
            CleanupAfterInterrupt(owner);
            return;
        default:
            m_generator->Finalize(owner);
            return;
    }
}

void LegacyBehaviour::CleanupAfterInterrupt(Unit& owner)
{
    switch (m_kind)
    {
        case Motion::Kind::Fear:
            // The arbiter has already erased the finished claim by the time this hook runs, so
            // HoldsControl(Fear) answers for the survivor: a fear that outlives this one keeps its run.
            if (owner.GetTypeId() == TYPEID_UNIT && !owner.GetMotionMaster()->HoldsControl(Motion::Kind::Fear))
            {
                static_cast<Creature&>(owner).SetWalk(!owner.hasUnitState(UNIT_STAT_RUNNING_STATE), false);
            }
            return;
        case Motion::Kind::Confused:
            return;
        default:
            return;
    }
}

bool LegacyBehaviour::Tick(Unit& owner, uint32 diff)
{
    return m_generator->Update(owner, diff);
}

Motion::FinishReason LegacyBehaviour::EndReason(Unit& /*owner*/) const
{
    switch (m_kind)
    {
        case Motion::Kind::Taxi:
            return Motion::FinishReason::Arrived;
        default:
            return Motion::FinishReason::Expired;   // a timed fear, and the endless kinds nothing pops
    }
}

void LegacyBehaviour::SpeedChanged()
{
    m_generator->unitSpeedChanged();
}

bool LegacyBehaviour::GetResetPosition(Unit& owner, float& x, float& y, float& z, float& o) const
{
    return m_generator->GetResetPosition(owner, x, y, z, o);
}
