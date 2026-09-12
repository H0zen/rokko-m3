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

#ifndef MANGOS_HARNESS_AI_H
#define MANGOS_HARNESS_AI_H

#include "CreatureAI.h"

namespace Harness
{
    class Scenario;

    /**
     * A decorating AI: forwards every hook to the AI the factory selected for the
     * creature (so it behaves as it would in the world) and records the movement
     * informs, the home arrival and the death for the scenario, as the old Lua
     * harness's creature events did above the normal AI. Owns the wrapped AI.
     *
     * While a scenario owns a creature, Creature::AI() publicly returns this
     * decorator, so a dynamic_cast to the script's own concrete AI type (as SD3's
     * quest hooks do for the chicken, npcs_special.cpp:397/409) fails for the
     * scenario's duration; the wrapped AI still receives every callback beneath it.
     * The harness's actors are its own summons, and, for one scenario, the world's
     * Mouse.
     */
    class HarnessAI : public CreatureAI
    {
    public:
        HarnessAI(Creature* creature, CreatureAI* wrapped, Scenario* scenario);
        ~HarnessAI() override;

        /// Hands the wrapped AI back to the caller and forgets it: the destructor
        /// deletes it only while still held. For the runner's end-of-scenario sweep,
        /// which installs it back on the creature in this decorator's place.
        CreatureAI* Release();

        void MovementInform(uint32 type, uint32 id) override;
        void JustReachedHome() override;
        void JustDied(Unit* killer) override;

        // Forwarded unchanged.
        void GetAIInformation(ChatHandler& reader) override;
        void MoveInLineOfSight(Unit* who) override;
        bool CanIgnoreForRelocationNotify(Unit* who) const override;
        void EnterCombat(Unit* enemy) override;
        void EnterEvadeMode() override;
        void HealedBy(Unit* healer, uint32& amount) override;
        void DamageDeal(Unit* doneTo, uint32& damage) override;
        void DamageTaken(Unit* dealer, uint32& damage) override;
        void CorpseRemoved(uint32& respawnDelay) override;
        void SummonedCreatureJustDied(Creature* summoned) override;
        void KilledUnit(Unit* victim) override;
        void OwnerKilledUnit(Unit* victim) override;
        void JustSummoned(Creature* summoned) override;
        void JustSummoned(GameObject* go) override;
        void SummonedCreatureDespawn(Creature* summoned) override;
        void SpellHit(Unit* caster, SpellEntry const* spell) override;
        void SpellHitTarget(Unit* target, SpellEntry const* spell) override;
        void AttackedBy(Unit* attacker) override;
        void JustRespawned() override;
        void SummonedMovementInform(Creature* summoned, uint32 type, uint32 data) override;
        void ReceiveEmote(Player* player, uint32 emote) override;
        void AttackStart(Unit* who) override;
        void UpdateAI(uint32 const diff) override;
        bool IsVisible(Unit* who) const override;
        bool canReachByRangeAttack(Unit* who) override;
        void ReceiveAIEvent(AIEventType eventType, Creature* sender, Unit* invoker, uint32 miscValue) override;
        void Reset() override;

    private:
        CreatureAI* m_wrapped;
        Scenario*   m_scenario;
    };
}

#endif
