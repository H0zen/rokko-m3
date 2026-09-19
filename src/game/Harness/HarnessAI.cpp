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

namespace Harness
{
    namespace
    {
        /// One recorded event, at the creature's position as the hook saw it.
        Inform At(Creature* creature, Inform::Event event)
        {
            Inform r;
            r.event = event;
            r.x = creature->Where().X();
            r.y = creature->Where().Y();
            r.guidLow = creature->GetGUIDLow();
            return r;
        }

        /// A MovementInform record: the kind and the id, at the creature's position.
        void Record(Scenario* scenario, Creature* creature, Motion::Kind kind, uint32 id)
        {
            if (!scenario || !creature)
            {
                return;
            }
            Inform r = At(creature, Inform::Event::Inform);
            r.kind = kind;
            r.id = id;
            scenario->Informs().push_back(r);
        }

        /// A WaypointPathInform record: the path id, the event and the node.
        void RecordPath(Scenario* scenario, Creature* creature, uint32 pathId, Motion::PathEvent event, uint32 node)
        {
            if (!scenario || !creature)
            {
                return;
            }
            Inform r = At(creature, Inform::Event::PathInform);
            r.pathId = pathId;
            r.pathEvent = event;
            r.id = node;
            scenario->Informs().push_back(r);
        }

        /// A ReachedHome or Died record: the event alone.
        void RecordEvent(Scenario* scenario, Creature* creature, Inform::Event event)
        {
            if (!scenario || !creature)
            {
                return;
            }
            scenario->Informs().push_back(At(creature, event));
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

    void HarnessAI::MovementInform(Motion::Kind kind, uint32 id)
    {
        Record(m_scenario, m_creature, kind, id);
        if (m_scenario)
        {
            m_scenario->OnInform(m_creature, kind, id);
        }
        if (m_wrapped)
        {
            m_wrapped->MovementInform(kind, id);
        }
    }

    void HarnessAI::WaypointPathInform(uint32 pathId, Motion::PathEvent event, uint32 node)
    {
        RecordPath(m_scenario, m_creature, pathId, event, node);
        if (m_scenario)
        {
            m_scenario->OnPathInform(m_creature, pathId, event, node);
        }
        if (m_wrapped)
        {
            m_wrapped->WaypointPathInform(pathId, event, node);
        }
    }

    void HarnessAI::JustReachedHome()
    {
        RecordEvent(m_scenario, m_creature, Inform::Event::ReachedHome);
        if (m_wrapped)
        {
            m_wrapped->JustReachedHome();
        }
    }

    void HarnessAI::JustDied(Unit* killer)
    {
        RecordEvent(m_scenario, m_creature, Inform::Event::Died);
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

    void HarnessAI::SummonedMovementInform(Creature* summoned, Motion::Kind kind, uint32 data)
    {
        if (m_wrapped)
        {
            m_wrapped->SummonedMovementInform(summoned, kind, data);
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
