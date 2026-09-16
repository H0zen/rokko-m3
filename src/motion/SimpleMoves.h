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

#ifndef MANGOS_MOTION_SIMPLEMOVES_H
#define MANGOS_MOTION_SIMPLEMOVES_H

#include "BehaviourModel.h"

/**
 * The simple moves (P5-B family 1, design §4): the seven natives that replace the point
 * and idle generator files. Each reproduces its generator's matrix row exactly.
 */
namespace Motion
{
    /// Nothing: the factory default and the masking command.
    class IdleBehaviour : public Behaviour
    {
        public:
            Motion::Kind Kind() const override { return Motion::Kind::Idle; }
            Step Activate(Sight const&) override { return Step::None(); }
            Step Suspend() override { return Step::None(); }
            Step Resume(Sight const&, bool) override { return Step::None(); }
            Step Tick(Sight const&, uint32) override { return Step::None(); }
            FinishReason EndReason(Sight const&) const override { return FinishReason::Expired; }
            Outcome Finish(FinishReason, Sight const&) override { return Outcome(); }
    };

    /// The point family: a one-shot leg to a goal (Point, FlyLand by flags, AssistRun by flags and a finisher, the charge by a tracked target and a speed).
    class PointBehaviour : public Behaviour
    {
        public:
            struct Params
            {
                Motion::Kind kind = Motion::Kind::Point; ///< Point, FlyLand or AssistRun
                uint32       id = 0;
                Vector3      goal;
                uint32       flags = MOVE_NONE;
                float        speed = 0.0f;               ///< 0 = the unit's pace
                uint64       target = 0;                 ///< the charge: the goal follows this unit's contact point
                bool         informs = true;             ///< false for the charge and the swoop (they never informed)
                float        relayDrift = 2.0f;          ///< the charge: re-lay when the goal drifted more than this
                uint32       relayEveryMs = 500;         ///< the charge: at most one re-lay per this
            };
            explicit PointBehaviour(Params const& p);
            Motion::Kind Kind() const override { return m_p.kind; }
            Step Activate(Sight const& sight) override;
            Step Suspend() override;
            Step Resume(Sight const& sight, bool reset) override;
            Step Tick(Sight const& sight, uint32 diff) override;
            FinishReason EndReason(Sight const&) const override { return m_end; }
            Outcome Finish(FinishReason why, Sight const& sight) override;
            bool TracksTarget() const override { return m_p.target != 0; }
            uint64 Target() const override { return m_p.target; }
            uint32 Relays() const { return m_relays; }   ///< the charge's re-lay count (the harness reports it)
        private:
            Params       m_p;
            bool         m_done = false;
            FinishReason m_end = FinishReason::Arrived;
            Vector3      m_laid;          ///< the goal of the leg last asked for
            bool         m_haveLaid = false;
            uint32       m_sinceRelay = 0;
            uint32       m_relays = 0;
    };

    /// A timer and nothing else (Distract; AssistDistract re-attacks on finish).
    class DistractBehaviour : public Behaviour
    {
        public:
            DistractBehaviour(Motion::Kind kind, uint32 ms) : m_kind(kind), m_timer(ms) {}
            Motion::Kind Kind() const override { return m_kind; }
            Step Activate(Sight const&) override { return Step::None(); }
            Step Suspend() override { return Step::None(); }
            Step Resume(Sight const&, bool) override { return Step::None(); }
            Step Tick(Sight const&, uint32 diff) override;
            FinishReason EndReason(Sight const&) const override { return FinishReason::Expired; }
            Outcome Finish(FinishReason why, Sight const& sight) override;
        private:
            Motion::Kind m_kind;
            uint32       m_timer;
    };

    /// Guards a spline it launched once: a jump, a knockback arc, a fall.
    class EffectBehaviour : public Behaviour
    {
        public:
            EffectBehaviour(uint32 id, EffectLaunch const& launch) : m_id(id), m_launch(launch) {}
            Motion::Kind Kind() const override { return Motion::Kind::Effect; }
            Step Activate(Sight const& sight) override;
            Step Suspend() override { return Step::None(); }
            Step Resume(Sight const&, bool) override { return Step::None(); }
            Step Tick(Sight const& sight, uint32 diff) override;
            FinishReason EndReason(Sight const& sight) const override { return sight.landed ? FinishReason::Arrived : FinishReason::Cut; }
            Outcome Finish(FinishReason why, Sight const& sight) override;
        private:
            uint32       m_id;
            EffectLaunch m_launch;
            bool         m_launched = false;
            bool         m_done = false;
    };
}

#endif
