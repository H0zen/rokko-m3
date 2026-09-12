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

#include "HarnessAI.h"
#include "Scenario.h"
#include "Creature.h"
#include "MotionMaster.h"

namespace Harness
{
    namespace
    {
        /// Not a MovementGeneratorType: the marker the old harness used for a death.
        const uint32 kDiedType = 0xFFFF;

        /// One recorded event, at the creature's position as the hook saw it.
        void Record(Scenario* scenario, Creature* creature, uint32 type, uint32 id)
        {
            if (!scenario || !creature)
            {
                return;
            }
            Inform r;
            r.type = type;
            r.id = id;
            r.x = creature->Where().X();
            r.y = creature->Where().Y();
            r.guidLow = creature->GetGUIDLow();
            scenario->Informs().push_back(r);
        }
    }

    HarnessAI::HarnessAI(Creature* creature, CreatureAI* wrapped, Scenario* scenario)
        : CreatureAI(creature), m_wrapped(wrapped), m_scenario(scenario)
    {
    }

    HarnessAI::~HarnessAI()
    {
        delete m_wrapped;
    }

    CreatureAI* HarnessAI::Release()
    {
        CreatureAI* wrapped = m_wrapped;
        m_wrapped = NULL;
        return wrapped;
    }

    void HarnessAI::MovementInform(uint32 type, uint32 id)
    {
        Record(m_scenario, m_creature, type, id);
        if (m_wrapped)
        {
            m_wrapped->MovementInform(type, id);
        }
    }

    void HarnessAI::JustReachedHome()
    {
        Record(m_scenario, m_creature, HOME_MOTION_TYPE, 0);
        if (m_wrapped)
        {
            m_wrapped->JustReachedHome();
        }
    }

    void HarnessAI::JustDied(Unit* killer)
    {
        Record(m_scenario, m_creature, kDiedType, 0);
        if (m_wrapped)
        {
            m_wrapped->JustDied(killer);
        }
    }

    void HarnessAI::GetAIInformation(ChatHandler& reader)
    {
        if (m_wrapped)
        {
            m_wrapped->GetAIInformation(reader);
        }
    }

    void HarnessAI::MoveInLineOfSight(Unit* who)
    {
        if (m_wrapped)
        {
            m_wrapped->MoveInLineOfSight(who);
        }
    }

    bool HarnessAI::CanIgnoreForRelocationNotify(Unit* who) const
    {
        return m_wrapped ? m_wrapped->CanIgnoreForRelocationNotify(who) : false;
    }

    void HarnessAI::EnterCombat(Unit* enemy)
    {
        if (m_wrapped)
        {
            m_wrapped->EnterCombat(enemy);
        }
    }

    void HarnessAI::EnterEvadeMode()
    {
        if (m_wrapped)
        {
            m_wrapped->EnterEvadeMode();
        }
    }

    void HarnessAI::HealedBy(Unit* healer, uint32& amount)
    {
        if (m_wrapped)
        {
            m_wrapped->HealedBy(healer, amount);
        }
    }

    void HarnessAI::DamageDeal(Unit* doneTo, uint32& damage)
    {
        if (m_wrapped)
        {
            m_wrapped->DamageDeal(doneTo, damage);
        }
    }

    void HarnessAI::DamageTaken(Unit* dealer, uint32& damage)
    {
        if (m_wrapped)
        {
            m_wrapped->DamageTaken(dealer, damage);
        }
    }

    void HarnessAI::CorpseRemoved(uint32& respawnDelay)
    {
        if (m_wrapped)
        {
            m_wrapped->CorpseRemoved(respawnDelay);
        }
    }

    void HarnessAI::SummonedCreatureJustDied(Creature* summoned)
    {
        if (m_wrapped)
        {
            m_wrapped->SummonedCreatureJustDied(summoned);
        }
    }

    void HarnessAI::KilledUnit(Unit* victim)
    {
        if (m_wrapped)
        {
            m_wrapped->KilledUnit(victim);
        }
    }

    void HarnessAI::OwnerKilledUnit(Unit* victim)
    {
        if (m_wrapped)
        {
            m_wrapped->OwnerKilledUnit(victim);
        }
    }

    void HarnessAI::JustSummoned(Creature* summoned)
    {
        if (m_wrapped)
        {
            m_wrapped->JustSummoned(summoned);
        }
    }

    void HarnessAI::JustSummoned(GameObject* go)
    {
        if (m_wrapped)
        {
            m_wrapped->JustSummoned(go);
        }
    }

    void HarnessAI::SummonedCreatureDespawn(Creature* summoned)
    {
        if (m_wrapped)
        {
            m_wrapped->SummonedCreatureDespawn(summoned);
        }
    }

    void HarnessAI::SpellHit(Unit* caster, SpellEntry const* spell)
    {
        if (m_wrapped)
        {
            m_wrapped->SpellHit(caster, spell);
        }
    }

    void HarnessAI::SpellHitTarget(Unit* target, SpellEntry const* spell)
    {
        if (m_wrapped)
        {
            m_wrapped->SpellHitTarget(target, spell);
        }
    }

    void HarnessAI::AttackedBy(Unit* attacker)
    {
        if (m_wrapped)
        {
            m_wrapped->AttackedBy(attacker);
        }
    }

    void HarnessAI::JustRespawned()
    {
        if (m_wrapped)
        {
            m_wrapped->JustRespawned();
        }
    }

    void HarnessAI::SummonedMovementInform(Creature* summoned, uint32 type, uint32 data)
    {
        if (m_wrapped)
        {
            m_wrapped->SummonedMovementInform(summoned, type, data);
        }
    }

    void HarnessAI::ReceiveEmote(Player* player, uint32 emote)
    {
        if (m_wrapped)
        {
            m_wrapped->ReceiveEmote(player, emote);
        }
    }

    void HarnessAI::AttackStart(Unit* who)
    {
        if (m_wrapped)
        {
            m_wrapped->AttackStart(who);
        }
    }

    void HarnessAI::UpdateAI(uint32 const diff)
    {
        if (m_wrapped)
        {
            m_wrapped->UpdateAI(diff);
        }
    }

    bool HarnessAI::IsVisible(Unit* who) const
    {
        return m_wrapped ? m_wrapped->IsVisible(who) : false;
    }

    bool HarnessAI::canReachByRangeAttack(Unit* who)
    {
        return m_wrapped ? m_wrapped->canReachByRangeAttack(who) : false;
    }

    void HarnessAI::ReceiveAIEvent(AIEventType eventType, Creature* sender, Unit* invoker, uint32 miscValue)
    {
        if (m_wrapped)
        {
            m_wrapped->ReceiveAIEvent(eventType, sender, invoker, miscValue);
        }
    }

    void HarnessAI::Reset()
    {
        if (m_wrapped)
        {
            m_wrapped->Reset();
        }
    }
}
