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

#ifndef MANGOS_MOTION_MOBILITY_H
#define MANGOS_MOTION_MOBILITY_H

#include "Platform/Define.h"

#include <vector>

/**
 * The kernel's block state (design 2026-09-15-movement-p5a-block-state-design.md):
 * the reasons a unit may not be moved by a behaviour, each counted by source, and
 * the table of what the selected behaviour may do under them. Pure: the arbiter
 * owns one and is the only thing that changes it.
 */
namespace Motion
{
    /// What an outside source can impose on a unit; fear, confuse, distract and taxi are
    /// the arbiter's own entries and are reported as reasons, never stored here.
    enum class Inhibition : uint8 { Rooted, Stunned, Dead, Possessed, Count };

    /// The active reasons as a bit set: the four inhibitions, then the arbiter's own.
    enum Reason : uint8
    {
        ReasonRooted     = 1 << 0,
        ReasonStunned    = 1 << 1,
        ReasonDead       = 1 << 2,
        ReasonPossessed  = 1 << 3,
        ReasonFeared     = 1 << 4,   ///< a Fear claim is held
        ReasonConfused   = 1 << 5,   ///< a Confused claim is held
        ReasonDistracted = 1 << 6,   ///< a Distract-layer entry is held
        ReasonOnTaxi     = 1 << 7    ///< a Taxi entry is held
    };

    /// The reasons that deny a player free movement for the client's own requests (a logout, a
    /// chain target): the old UNIT_STAT_NO_FREE_MOVE less its feign bit, which the shell's
    /// published state carries apart (MotionMaster::PublishedState::feign).
    const uint8 kNoFreeMoveReasons = ReasonRooted | ReasonStunned | ReasonFeared | ReasonConfused | ReasonOnTaxi;

    /// The shell's old unit-state composites as reasons (P5-C2), each less its feign bit, which
    /// the published state carries apart. None names ReasonDead: the published state never
    /// carries it (a real death is IsAlive()'s answer and never set a unit-state bit).
    const uint8 kCannotMoveReasons  = ReasonRooted | ReasonStunned;                   ///< UNIT_STAT_CAN_NOT_MOVE
    const uint8 kNotMoveReasons     = ReasonRooted | ReasonStunned | ReasonDistracted; ///< UNIT_STAT_NOT_MOVE
    const uint8 kCannotReactReasons = ReasonStunned | ReasonFeared | ReasonConfused;   ///< UNIT_STAT_CAN_NOT_REACT
    const uint8 kLostControlReasons = ReasonFeared | ReasonPossessed;                  ///< UNIT_STAT_LOST_CONTROL

    /// The class of the selected entry, for the table.
    enum class Selected : uint8 { None, Ordinary, Distract, Control, Taxi };

    /// What the selected behaviour may do right now, and why not.
    struct MobilityDecision
    {
        bool       ticks;      ///< the behaviour's Tick runs (a distract's clock runs under a stun; a flight goes on under a root)
        bool       mayMove;    ///< it may lay legs and translate the unit
        bool       mayTurn;    ///< the unit may change its facing
        Inhibition dominant;   ///< the reason that decided, Count when none blocked
        uint8      reasons;    ///< every active Reason bit
    };

    /// The bit of an inhibition in Reason.
    uint8 ReasonBit(Inhibition what);
    /// The table (spec §4.1): what `selected` may do under `reasons`.
    MobilityDecision Decide(Selected selected, uint8 reasons);
    /// "Rooted", "Stunned", "Dead", "Possessed", "none".
    char const* InhibitionName(Inhibition what);

    /// The spell a ControlClaim-shaped identity names (see SourceDomain below for the shape,
    /// and Motion::ControlClaim in the shell, which builds it). Zero means the claim was NOT
    /// taken by an aura: the AI's own low-health flee (Creature::DoFleeToGetAssistance) and a
    /// script's fear both pass 0 there, and they are the only takers that do.
    inline uint32 ClaimSpell(uint64 claim) { return uint32(claim >> 40); }

    /// The domain of a source, in the value's top nibble. An aura's source is its
    /// ControlClaim-shaped identity (spell << 40 | effect << 32 | caster counter), whose
    /// top nibble is 0 for every 4.3.4 spell id; the other domains never collide with it.
    enum class SourceDomain : uint8
    {
        Aura = 0,
        Death = 1,
        Possession = 2,
        Seat = 3,
        FixedVehicle = 4,
        Script = 5   ///< reserved for a script without an aura, and the harness; the one script
                     ///< today, grizzly_hills.cpp, has an aura and uses the aura shape instead
    };

    /// A source identity for a non-aura domain: the owner's guid counter and an extra word.
    /// The extra word carries 28 bits; every real one, a seat index or a spell id, is far below.
    inline uint64 InhibitSource(SourceDomain domain, uint32 owner, uint32 extra = 0)
    {
        return (uint64(domain) << 60) | (uint64(extra & 0x0FFFFFFFu) << 32) | uint64(owner);
    }

    /// The one source of a real death (a unit dies once at a time).
    const uint64 kDeathSource = InhibitSource(SourceDomain::Death, 1);

    /// The reasons, each counted by source: a reason holds while any of its sources holds.
    class Mobility
    {
        public:
            /// @return True when the reason became active (its first source).
            bool Inhibit(Inhibition what, uint64 source);
            /// @return True when the reason became inactive (its last source gone); an unknown source is a no-op.
            bool Uninhibit(Inhibition what, uint64 source);
            /// Releases every source of that domain from every inhibition; used at death.
            void DropDomain(SourceDomain domain);
            /// True while any source holds this reason.
            bool Inhibited(Inhibition what) const;
            /// The sources holding a reason, in arrival order (the GM dump).
            std::vector<uint64> const& Sources(Inhibition what) const;
            /// The active inhibitions as Reason bits (never the arbiter's own four).
            uint8 Reasons() const;

        private:
            std::vector<uint64> m_sources[static_cast<size_t>(Inhibition::Count)];
    };
}

#endif
