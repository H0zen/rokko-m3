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

#ifndef MANGOS_MOTION_STATE_H
#define MANGOS_MOTION_STATE_H

#include "Change.h"
#include "PacketMatrix.h"
#include "PendingChanges.h"

#include <vector>

/**
 * The kinematic state of one unit and its change pipeline (design v2 §6):
 * desired state, committed the moment a change is decided and what all
 * gameplay reads; confirmed state, what the owning client has acked, which
 * gates what observers are told. A server-driven unit confirms at once and
 * broadcasts the spline form. A client-driven unit opens a pending entry,
 * sends the mover form with a counter, and confirms on the ack, which is
 * when the observer form goes out. This class decides; it does not send:
 * every call returns the emissions the caller should build (Writers.h) and
 * send. Time is an argument; nothing here reads a clock. The same phase
 * invariant TimeBase.h names is what keeps a unit's state single-threaded:
 * Unit::Update and the movement and ack handlers run in the map phase,
 * login and worldport in the session phase.
 */
namespace Motion
{
    struct Kinematics
    {
        float speed[9];            ///< flat yd/s, UnitMoveType order
        bool  root;
        bool  canFly;
        bool  waterWalk;
        bool  featherFall;
        bool  hover;
        bool  gravityDisabled;
        bool  canTransitionSwimFly;
        bool  walk;                ///< Gait: server-driven only
        bool  swim;                ///< server-driven only
        float collisionHeight;

        Kinematics();
        bool operator==(Kinematics const& r) const;
        void Apply(Change const& change);   ///< KnockBack and Teleport change nothing here
    };

    enum class EmissionKind : uint8 { Mover, Observer, Spline };

    struct Emission
    {
        EmissionKind kind;
        uint16       opcode;
        uint32       counter;   ///< 0 for a spline form
        Change       change;
    };

    struct StateCounters
    {
        uint32 applied, refused, emitted, acked, confirmed, epochs, modeChanges, kicks, mismatched, resent, resyncs;
        StateCounters() : applied(0), refused(0), emitted(0), acked(0), confirmed(0), epochs(0), modeChanges(0), kicks(0),
                           mismatched(0), resent(0), resyncs(0) {}
    };

    class State
    {
    public:
        State(Mode mode, TimeoutPolicy const& policy, Kinematics const& initial);

        Mode GetMode() const { return m_mode; }
        void SetMode(Mode mode, uint32 now);

        Kinematics const& Desired() const { return m_desired; }
        Kinematics const& Confirmed() const { return m_confirmed; }

        std::vector<Emission> Apply(Change const& change, uint32 now);
        std::vector<Emission> Ack(ChangeType type, uint32 counter, AckPayload const& payload, uint32 now);
        AckResult LastAck() const { return m_lastAck; }
        std::vector<Emission> Tick(uint32 now);
        void NewEpoch(uint32 now);
        bool KickRequested() const { return m_kick; }
        void ClearKick() { m_kick = false; }     ///< Call once the kick has been acted on, so the next tick's still-pending entries do not kick and log again.
        /// True after Tick() reported a resync (design v2 §6.2): every pending entry was
        /// just reissued; the caller snaps the client to where the server has it (a near
        /// teleport) and calls ClearResync() once it has.
        bool ResyncRequested() const { return m_resync; }
        void ClearResync() { m_resync = false; }
        /// The desired state as fresh changes (design v2 §6.2's snapshot after a new epoch):
        /// the seven speeds whose row has an ack, in UnitMoveType order; each set flag; the
        /// collision height when above 0, with the "force" reason. Apply each in order.
        std::vector<Change> Snapshot() const;

        PendingChanges const& Pending() const { return m_pending; }
        StateCounters const& Counters() const { return m_counters; }

    private:
        Emission Emit(EmissionKind kind, uint16 opcode, uint32 counter, Change const& change);

        Mode           m_mode;
        Kinematics     m_desired;
        Kinematics     m_confirmed;
        PendingChanges m_pending;
        AckResult      m_lastAck;
        bool           m_kick;
        bool           m_resync;
        StateCounters  m_counters;
    };
}

#endif
