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

#ifndef MANGOS_MOTION_CONTROLMOVES_H
#define MANGOS_MOTION_CONTROLMOVES_H

#include "BehaviourModel.h"
#include "Utilities/MathDefines.h"

// The two control behaviours a Control claim drives (P5-B family 4): FearBehaviour replaces
// FleeingMovementGenerator and its timed subclass, ConfusedBehaviour replaces
// ConfusedMovementGenerator -- both deleted on this branch. Every draw is the generator's, in
// its order, through the Services port; the shell's claims machinery (the aura-keyed identity,
// the shared unit state, the end-of-control rule, a player's client control) lives above these
// and is untouched. Neither sets a facing, informs anything or answers a reset position.

namespace Motion
{
    /// The flee's bands and rests as data (design §4.1): today's constants, which are TC's.
    /// A retail-backed change (the notes' open question A(a), a live capture of a feared
    /// player) is a value here, not a rewrite.
    struct FearGeometry
    {
        float  minQuiet = 28.0f;          ///< closer than this to the fright: bolt away from it
        float  maxQuiet = 43.0f;          ///< farther than this: drift back toward it
        float  legLimit = 30.0f;          ///< the routed leg's length cap: a bolt, not a journey
        uint32 restMin = 800;             ///< the rest standing after a bolt, ms
        uint32 restMax = 1500;
        uint32 retryMs = 50;              ///< after a refused point or a blocked leg
        float  closeFactorMin = 0.4f;     ///< inside minQuiet: dist = f * (minQuiet - d)
        float  closeFactorMax = 1.3f;
        float  closeJitter = M_PI_F / 8;  ///< inside minQuiet: the bearing away, +- this
        float  farFactorMin = 0.4f;       ///< beyond maxQuiet: dist = f * (maxQuiet - minQuiet)
        float  farFactorMax = 1.0f;
        float  farJitter = M_PI_F / 4;    ///< beyond maxQuiet: the bearing back, +- this
        float  bandFactorMin = 0.6f;      ///< inside the band: dist = f * (maxQuiet - minQuiet), any bearing
        float  bandFactorMax = 1.2f;
    };

    /// Panic (design §4.1): bolt away from the fright in short routed bursts, a beat standing
    /// between them; the timed variant (the low-health runner) ends itself and re-engages.
    class FearBehaviour : public Behaviour
    {
        public:
            struct Params
            {
                uint64       fright = 0;              ///< the fear source's raw guid, resolved at each pick through Services::Fright
                uint32       timeLimitMs = 0;         ///< the timed variant; 0 = until the claim is released
                uint32       stateFleeingMove = 0;    ///< the shell's UNIT_STAT_FLEEING_MOVE, opaque here
                FearGeometry geometry;
            };
            explicit FearBehaviour(Params const& p) : m_p(p), m_totalLeft(int32(p.timeLimitMs)) {}
            Motion::Kind Kind() const override { return Motion::Kind::Fear; }
            Step Activate(Sight const& sight, Services& svc) override;
            Step Suspend() override;
            Step Resume(Sight const& sight, Services& svc, bool reset) override;
            Step Tick(Sight const& sight, Services& svc, uint32 diff) override;
            FinishReason EndReason(Sight const&) const override { return FinishReason::Expired; }   ///< the timed clock, or a dead mover
            /// The timed Finalize restores a creature's gait unconditionally (design §6.5: the
            /// generator left the run, and the chase it starts sets it again at once).
            Outcome Finish(FinishReason why, Sight const& sight, Services& svc) override;
            /// For the listing (HeldView::target): the fright is not tracked per tick, so TracksTarget() stays false.
            uint64 Target() const override { return m_p.fright; }
            uint32 Variant() const override { return m_p.timeLimitMs ? 1 : 0; }   ///< TIMED_FLEEING's projection
        private:
            /// The generator's PickFleePoint: the bearing draw, the fright, the band's two draws,
            /// the ground point. False when the bearing it picked has no ground under it.
            bool PickFleePoint(Sight const& sight, Services& svc, Vector3& out);
            Params  m_p;
            int32   m_totalLeft;         ///< the timed clock: counts only on the ticks it receives, so a block pauses it; untouched by a reset
            int32   m_rest = 0;          ///< ms left standing before the next bolt; <= 0 = passed; counts only while standing
            bool    m_havePoint = false;
            Vector3 m_point;
    };

    /// Disoriented staggering (design §4.2): a lurch toward a random point near where the unit
    /// lost its wits, roughly once a second, at a walk. The stagger keeps running while a leg is
    /// walked, so a fresh point supersedes the last one part-way: that never-quite-arriving
    /// motion is what reads as confusion, not the wander's rest-then-hop.
    class ConfusedBehaviour : public Behaviour
    {
        public:
            struct Params
            {
                uint32 stateConfusedMove = 0;   ///< the shell's UNIT_STAT_CONFUSED_MOVE, opaque here
                float  radius = 10.0f;          ///< the stagger envelope around the anchor (Movement.ConfuseRadius)
                uint32 staggerMin = 800;        ///< the stagger after a lurch, ms, counted from the lurch
                uint32 staggerMax = 1500;
                uint32 retryMs = 50;            ///< the first retry after a refused point; doubles per failure up to staggerMin
            };
            explicit ConfusedBehaviour(Params const& p) : m_p(p) {}
            Motion::Kind Kind() const override { return Motion::Kind::Confused; }
            Step Activate(Sight const& sight, Services& svc) override;
            Step Suspend() override;
            Step Resume(Sight const& sight, Services& svc, bool reset) override;
            Step Tick(Sight const& sight, Services& svc, uint32 diff) override;
            FinishReason EndReason(Sight const&) const override { return FinishReason::Expired; }
            Outcome Finish(FinishReason why, Sight const& sight, Services& svc) override;
            Vector3 const& Anchor() const { return m_anchor; }   ///< where the unit stood when it was confused (the tests)
        private:
            /// The generator's Initialize without the anchor capture (its Reset): the stagger
            /// cleared, no lurch, the leg forgotten, then the stop and the bit unless dead or held.
            Step Restart(Sight const& sight);
            uint32 RetryDelay();
            Params  m_p;
            Vector3 m_anchor;           ///< captured once at Activate, kept through a reset (the generator's, and TC's)
            int32   m_stagger = 0;      ///< ms before the next lurch; runs even mid-leg
            bool    m_haveLurch = false;
            Vector3 m_lurch;
            uint32  m_retries = 0;      ///< failed picks in a row; never reset by a restart (the generator's Initialize left it too)
    };
}

#endif
