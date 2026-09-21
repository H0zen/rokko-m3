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

#include "MoveSplineInit.h"
#include "Move/ClientRules.h"
#include "Move/Route.h"
#include "MoveSplineSpeed.h"
#include "MoveSpline.h"
#include "packet_builder.h"
#include "Unit.h"
#include "Transports.h"
#include "Vehicle.h"
#include "TransportMap.h"
#include "Map.h"
#include <atomic>

namespace
{
    /// The vessel whose deck this unit is standing on, or an empty guid. Derived from the
    /// map, so a spline goes out as SMSG_MONSTER_MOVE_TRANSPORT for anything on a deck --
    /// crew, pet or totem alike -- without anyone having registered it as anything.
    ObjectGuid DeckVesselGuidOf(Unit const& unit)
    {
        if (Map* on = unit.FindMap())
        {
            if (TransportMap* hull = on->AsTransport())
            {
                if (Transport* vessel = hull->Vessel())
                {
                    return vessel->GetObjectGuid();
                }
            }
        }
        return ObjectGuid();
    }
}

namespace Movement
{
    /**
     * @brief Selects the appropriate speed type based on movement flags.
     * @param moveFlags The movement flags.
     * @return The selected UnitMoveType.
     */
    UnitMoveType SelectSpeedType(uint32 moveFlags)
    {
        if (moveFlags & MOVEFLAG_FLYING)
        {
            if (moveFlags & MOVEFLAG_BACKWARD /*&& speed_obj.flight >= speed_obj.flight_back*/)
            {
                return MOVE_FLIGHT_BACK;
            }
            else
            {
                return MOVE_FLIGHT;
            }
        }
        else if (moveFlags & MOVEFLAG_SWIMMING)
        {
            if (moveFlags & MOVEFLAG_BACKWARD /*&& speed_obj.swim >= speed_obj.swim_back*/)
            {
                return MOVE_SWIM_BACK;
            }
            else
            {
                return MOVE_SWIM;
            }
        }
        else if (moveFlags & MOVEFLAG_WALK_MODE)
        {
            // if ( speed_obj.run > speed_obj.walk )
            return MOVE_WALK;
        }
        else if (moveFlags & MOVEFLAG_BACKWARD /*&& speed_obj.run >= speed_obj.run_back*/)
        {
            return MOVE_RUN_BACK;
        }

        return MOVE_RUN;
    }

    /**
     * @brief Final pass of initialization that launches spline movement.
     * @return int32 duration - estimated travel time
     */
    int32 MoveSplineInit::Launch()
    {
        MoveSpline& move_spline = *unit.movespline;
        // A VEHICLE seat is a real transform the server owns, so a rider's pose has to be
        // fetched from it. A DECK is not: the unit's map is the vessel and its position is
        // already deck-local, so Where() is the answer and nothing is composed.
        TransportInfo* transportInfo = unit.GetTransportInfo();
        if (transportInfo && !transportInfo->IsOnVehicle())
        {
            transportInfo = NULL;
        }

        const ObjectGuid vesselGuid = DeckVesselGuidOf(unit);

        Geometry::Position real_position(unit.Where().Pos(), unit.Where().Facing());

        if (transportInfo)
        {
            Geometry::Placement const& deck = transportInfo->Seat();
            real_position.MoveTo(deck.Pos(), deck.Facing());
        }

        // there is a big chance that current position is unknown if current state is not finalized, need compute it
        // this also allows calculate spline position and update map position in much greater intervals
        if (!move_spline.Finalized() && !transportInfo)
        {
            real_position = move_spline.ComputePosition();
        }
        else if (!transportInfo)
        {
            // A stop just took the spline's position and the placement has not caught up
            // (it is written on the unit's next Update): start from where the stop was sent.
            if (Geometry::Position const* pending = unit.PendingSplineCommit())
            {
                real_position = *pending;
            }
        }

        if (args.path.empty())
        {
            // should i do the things that user should do?
            MoveTo(real_position.Pos());
        }

        // correct first vertex
        args.path[0] = real_position.Pos();
        args.initialOrientation = real_position.Facing();

        // AND THEN STRIP WHAT THAT JUST CREATED. Forcing point zero to the mover's own
        // position is right -- it is what keeps the client from splicing its position in
        // front and walking a longer path than we timed -- but a patrol that reaches a
        // node, waits, and sets off again from that node has just had point zero set to
        // the very waypoint that is already point one.
        //
        // The client does not deduplicate a normal path (only its cyclic one does, at
        // 2.4e-7), so it builds a segment of no length and divides by it. That division is
        // an integer atan2 in fixed point, sub_AC3C00 in Wow.exe 15595, and it takes the
        // whole client down with ERROR #132.
        //
        // NEVER BELOW TWO POINTS, though, and never a refusal. Returning 0 from here does
        // not mean "already there" to the caller -- MotionDriver reads it as the spline
        // REFUSING the leg and marks the mover blocked, so a patrol gives up on the node
        // and jumps to another. Whole paths were collapsing that way and the patrols
        // wandered between a few scattered points. A leg whose every point folds into one
        // is a leg that goes nowhere, which is what the original path already said, so it
        // goes out unchanged and behaves exactly as it did before any of this.
        if (args.path.size() > 1)
        {
            const uint16 before = uint16(args.path.size());
            const uint16 kept = Move::Client::StripDeadSegments(&args.path[0], before);
            if (kept >= 2)
            {
                args.path.resize(kept);
            }
        }

        uint32 moveFlags = unit.m_movementInfo.GetMovementFlags();
        if (args.walk)
        {
            moveFlags |= MOVEFLAG_WALK_MODE;
        }
        else
        {
            moveFlags &= ~MOVEFLAG_WALK_MODE;
        }

        moveFlags |= MOVEFLAG_FORWARD;

        if (args.velocity == 0.f)
        {
            args.velocity = unit.GetSpeed(SelectSpeedType(moveFlags));
        }

        if (!args.Validate(&unit))
        {
            return 0;
        }

        unit.m_movementInfo.SetMovementFlags((MovementFlags)moveFlags);
        move_spline.Initialize(args);

        // THE NEW ENGINE RUNNING BESIDE THE OLD ONE, answering only into the log.
        //
        // Move::Route keeps the polyline instead of re-simulating it, and the point of
        // keeping it is that a position can then be read at any instant rather than
        // written every 400 ms. Before anything is switched over, the two have to be shown
        // to describe the same motion on real traffic -- and the cheapest place they can
        // disagree is the duration, which is the whole path divided by one speed.
        //
        // A LINEAR path must agree exactly: both measure the same chords. A SMOOTH one is
        // expected to differ, because MoveSpline measures the Catmull-Rom curve and a Leg
        // measures the chords between its points, so only the linear case is reported.
        //
        // A FALLING one is not a comparison at all. isSmooth() only tests the Catmullrom
        // bit, so a fall passes for linear -- but its duration comes from FallInitializer,
        // which solves the drop against gravity and never looks at args.velocity. Asking a
        // Leg to match that is asking the wrong question, and the answer was three log
        // lines of up to 196 ms that meant nothing.
        //
        // The 1 ms is CommonInitializer seeding its accumulator at minimal_duration, so
        // every spline duration is exactly one millisecond longer than the distance it
        // covers. That constant was the whole content of sixty-nine reports: on a leg of
        // ten milliseconds it is ten percent, which cleared a threshold meant to catch
        // real disagreement. Take it off before comparing.
        //
        // Silence here is the evidence; a line is a discrepancy worth reading.
        if (!args.flags.isSmooth() && !args.flags.falling && args.path.size() > 1)
        {
            Move::Route shadow;
            if (shadow.Launch(&args.path[0], uint16(args.path.size()), args.velocity, 0))
            {
                const int32 theirs = move_spline.Duration() - 1;
                const int32 ours = int32(shadow.Duration());
                const int32 drift = ours > theirs ? ours - theirs : theirs - ours;
                if (theirs > 0 && drift * 20 > theirs)
                {
                    sLog.outError("Move::Route shadow: %s over %u point(s) at %.2f yd/s -- "
                                  "spline says %d ms, leg says %d ms (%d ms apart, "
                                  "flags 0x%08X)",
                                  unit.GetGuidStr().c_str(), uint32(args.path.size()),
                                  args.velocity, theirs, ours, drift, args.flags.raw());
                }
            }
        }

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        data << unit.GetPackGUID();

        if (transportInfo)
        {
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << transportInfo->GetTransportGuid().WriteAsPacked();
            data << int8(transportInfo->GetTransportSeat());
        }
        else if (!vesselGuid.IsEmpty())
        {
            // NO SEAT. A seat is a vehicle's, and a vehicle is a unit: it has a seat map,
            // a transform per seat and a passenger bound to one. A ship has none of that --
            // she is a map, and what is on her is simply on her. -1 is how the client is
            // told there is no seat, and it is what both reference cores send for a
            // MO_TRANSPORT.
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << vesselGuid.WriteAsPacked();
            data << int8(-1);
        }

        // WHAT THE CLIENT WILL MAKE OF THIS, asked before it goes out rather than guessed
        // at afterwards. The client does not obey our duration: it measures the polyline
        // it ended up with, divides by a speed of its own, and uses that (Wow.exe 15595,
        // sub_5CB460 and sub_A27900). So a leg that asks to travel faster than four times
        // the mover's speed is not refused, it is silently stretched -- the mover arrives
        // late and no existing check says a word. This reports it.
        const int32 sentDuration = move_spline.Duration();
        if (sentDuration > 0)
        {
            const float sentLength = float(sentDuration) * args.velocity * 0.001f;
            const Move::Client::Verdict verdict =
                Move::Client::Inspect(sentLength, uint32(sentDuration),
                                      unit.GetSpeed(SelectSpeedType(moveFlags)),
                                      args.flags.raw());
            if (verdict.tooFast)
            {
                sLog.outError("MoveSplineInit::Launch: %s asks for %.2f yd/s over %.2f yd, "
                              "above the client's ceiling of %.2f -- the client will stretch "
                              "this leg by %d ms (flags 0x%08X)",
                              unit.GetGuidStr().c_str(), verdict.askedSpeed, sentLength,
                              verdict.ceiling, verdict.stretchMs, args.flags.raw());
            }
        }

        PacketBuilder::WriteMonsterMove(move_spline, data);
        unit.SendMessageToSet(&data, true);

        return sentDuration;
    }

    /**
     * @brief Stops any creature movement.
     */
    void MoveSplineInit::Stop()
    {
        MoveSpline& move_spline = *unit.movespline;

        // No need to stop if we are not moving
        if (move_spline.Finalized())
        {
            return;
        }

        // A VEHICLE seat is a real transform the server owns, so a rider's pose has to be
        // fetched from it. A DECK is not: the unit's map is the vessel and its position is
        // already deck-local, so Where() is the answer and nothing is composed.
        TransportInfo* transportInfo = unit.GetTransportInfo();
        if (transportInfo && !transportInfo->IsOnVehicle())
        {
            transportInfo = NULL;
        }

        const ObjectGuid vesselGuid = DeckVesselGuidOf(unit);

        Geometry::Position real_position(unit.Where().Pos(), unit.Where().Facing());

        if (transportInfo)
        {
            Geometry::Placement const& deck = transportInfo->Seat();
            real_position.MoveTo(deck.Pos(), deck.Facing());
        }

        // there is a big chance that current position is unknown if current state is not finalized, need compute it
        // this also allows calculate spline position and update map position in much greater intervals
        if (!move_spline.Finalized() && !transportInfo)
        {
            real_position = move_spline.ComputePosition();
        }

        if (args.path.empty())
        {
            // should i do the things that user should do?
            MoveTo(real_position.Pos());
        }

        // current first vertex
        args.path[0] = real_position.Pos();

        args.flags = MoveSplineFlag::Done;
        unit.m_movementInfo.RemoveMovementFlag(MOVEFLAG_FORWARD);
        move_spline.Initialize(args);

        WorldPacket data(SMSG_MONSTER_MOVE, 64);
        data << unit.GetPackGUID();

        if (transportInfo)
        {
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << transportInfo->GetTransportGuid().WriteAsPacked();
            data << int8(transportInfo->GetTransportSeat());
        }
        else if (!vesselGuid.IsEmpty())
        {
            // NO SEAT. A seat is a vehicle's, and a vehicle is a unit: it has a seat map,
            // a transform per seat and a passenger bound to one. A ship has none of that --
            // she is a map, and what is on her is simply on her. -1 is how the client is
            // told there is no seat, and it is what both reference cores send for a
            // MO_TRANSPORT.
            data.SetOpcode(SMSG_MONSTER_MOVE_TRANSPORT);
            data << vesselGuid.WriteAsPacked();
            data << int8(-1);
        }

        data << uint8(0);
        data << real_position.X() << real_position.Y() << real_position.Z();
        data << move_spline.GetId();
        data << uint8(MonsterMoveStop);
        unit.SendMessageToSet(&data, true);
    }

    /**
     * @brief Constructor that initializes the MoveSplineInit with a reference to a Unit.
     * @param m Reference to the Unit to be moved.
     */
    /// generates per-spline ids; the client uses them to tell successive splines apart
    static std::atomic<uint32> splineIdGen(0);

    MoveSplineInit::MoveSplineInit(Unit& m) : unit(m)
    {
        args.splineId = ++splineIdGen;
        // mix existing state into new
        args.walk = unit.m_movementInfo.HasMovementFlag(MOVEFLAG_WALK_MODE);
        args.flags.canSwim = unit.CanSwim(); ///< client refuses to swim a spline mover without this bit (TC 4.3.4)
        args.flags.flying = unit.m_movementInfo.HasMovementFlag((MovementFlags)(MOVEFLAG_CAN_FLY | MOVEFLAG_FLYING | MOVEFLAG_LEVITATING));
    }

    /**
     * @brief Enables falling mode, carrying the unit's slow-fall state.
     */
    void MoveSplineInit::SetFall()
    {
        args.flags.EnableFalling();
        args.flags.fallingSlow = unit.m_movementInfo.HasMovementFlag(MOVEFLAG_SAFE_FALL);
    }

    /**
     * @brief Sets unit's facing to a specified target after all path done.
     * @param target The target to face.
     */
    void MoveSplineInit::SetFacing(const Unit* target)
    {
        args.flags.EnableFacingTarget();
        args.facing.target = target->GetObjectGuid().GetRawValue();
    }

    /**
     * @brief Adds final facing animation.
     * Sets unit's facing to specified point/angle after all path done.
     * You can have only one final facing: previous will be overridden.
     * @param angle The angle to face.
     */
    void MoveSplineInit::SetFacing(float angle)
    {
        args.facing.angle = Geometry::wrap(angle, 0.f, (float)Geometry::twoPi());
        args.flags.EnableFacingAngle();
    }
}
