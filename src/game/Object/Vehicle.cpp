/*
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

/*
 * @file Vehicle.cpp
 * This file contains the code needed for CMaNGOS to support vehicles
 * Currently implemented
 * - Board to board a passenger onto a vehicle (includes checks)
 * - Unboard to unboard a passenger from the vehicle
 * - SwitchSeat to switch to another seat of the same vehicle
 * - CanBoard to check if a passenger can board a vehicle
 * - Internal helper to set the controlling and spells for a vehicle's seat
 * - Internal helper to control the available seats of a vehicle
 */

#include "Vehicle.h"
#include "Utilities/Errors.h"
#include "Platform/Define.h"
#include "SharedDefines.h"
#include "ObjectGuid.h"
#include "Log.h"
#include "Unit.h"
#include "MoveSend.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "ObjectMgr.h"
#include "SQLStorages.h"
#include "Util.h"
#include "movement/MoveSpline.h"
#include "MapManager.h"
#include "TemporarySummon.h"

void ObjectMgr::LoadVehicleAccessory()
{
    sVehicleAccessoryStorage.Load();

    sLog.outString(">> Loaded %u vehicle accessories", sVehicleAccessoryStorage.GetRecordCount());
    sLog.outString();

    // Check content
    for (SQLMultiStorage::SQLSIterator<VehicleAccessory> itr = sVehicleAccessoryStorage.getDataBegin<VehicleAccessory>(); itr < sVehicleAccessoryStorage.getDataEnd<VehicleAccessory>(); ++itr)
    {
        if (!sCreatureStorage.LookupEntry<CreatureInfo>(itr->vehicleEntry))
        {
            sLog.outErrorDb("Table `vehicle_accessory` has entry (vehicle entry: %u, seat %u, passenger %u) where vehicle_entry is invalid, skip vehicle.", itr->vehicleEntry, itr->seatId, itr->passengerEntry);
            sVehicleAccessoryStorage.EraseEntry(itr->vehicleEntry);
            continue;
        }
        if (!sCreatureStorage.LookupEntry<CreatureInfo>(itr->passengerEntry))
        {
            sLog.outErrorDb("Table `vehicle_accessory` has entry (vehicle entry: %u, seat %u, passenger %u) where accessory_entry is invalid, skip vehicle.", itr->vehicleEntry, itr->seatId, itr->passengerEntry);
            sVehicleAccessoryStorage.EraseEntry(itr->vehicleEntry);
            continue;
        }
        if (itr->seatId >= MAX_VEHICLE_SEAT)
        {
            sLog.outErrorDb("Table `vehicle_accessory` has entry (vehicle entry: %u, seat %u, passenger %u) where seat is invalid (must be between 0 and %u), skip vehicle.", itr->vehicleEntry, itr->seatId, itr->passengerEntry, MAX_VEHICLE_SEAT - 1);
            sVehicleAccessoryStorage.EraseEntry(itr->vehicleEntry);
            continue;
        }
    }
}

/*
 * Constructor of VehicleInfo
 *
 * @param owner         MUST be provided owner of the vehicle (type Unit)
 * @param vehicleEntry  MUST be provided dbc-entry of the vehicle
 * @param overwriteNpcEntry Use to overwrite the GetEntry() result for selecting associated passengers
 *
 * This function will initialise the VehicleInfo of the vehicle owner
 * Also the seat-map is created here
 */
VehicleInfo::VehicleInfo(Unit* owner, VehicleEntry const* vehicleEntry, uint32 overwriteNpcEntry) :
    m_vehicleEntry(vehicleEntry),
    m_creatureSeats(0),
    m_playerSeats(0),
    m_overwriteNpcEntry(overwriteNpcEntry),
    m_isInitialized(false),
    m_owner(owner),
    m_lastPose(owner->Where()),
    m_updatePositionsTimer(500)
{
    MANGOS_ASSERT(vehicleEntry);
    MANGOS_ASSERT(m_owner);

    // Initial fill of available seats for the vehicle
    for (uint8 i = 0; i < MAX_VEHICLE_SEAT; ++i)
    {
        if (uint32 seatId = vehicleEntry->SeatID[i])
        {
            if (VehicleSeatEntry const* seatEntry = sVehicleSeatStore.LookupEntry(seatId))
            {
                m_vehicleSeats.insert(VehicleSeatMap::value_type(i, seatEntry));

                if (IsUsableSeatForCreature(seatEntry->Flags))
                {
                    m_creatureSeats |= 1 << i;
                }

                if (IsUsableSeatForPlayer(seatEntry->Flags, seatEntry->FlagsB))
                {
                    m_playerSeats |= 1 << i;
                }
            }
        }
    }
}

VehicleInfo::~VehicleInfo()
{
    ((Unit*)m_owner)->RemoveSpellsCausingAura(SPELL_AURA_CONTROL_VEHICLE);

    RemoveAccessoriesFromMap();                             // Remove accessories (for example required with player vehicles)
}

void VehicleInfo::Initialize()
{
    if (!m_overwriteNpcEntry)
    {
        m_overwriteNpcEntry = m_owner->GetEntry();
    }

    // Loading passengers (rough version only!)
    SQLMultiStorage::SQLMSIteratorBounds<VehicleAccessory> bounds = sVehicleAccessoryStorage.getBounds<VehicleAccessory>(m_overwriteNpcEntry);
    for (SQLMultiStorage::SQLMultiSIterator<VehicleAccessory> itr = bounds.first; itr != bounds.second; ++itr)
    {
        if (Creature* summoned = m_owner->SummonCreature(itr->passengerEntry, m_owner->Where().X(), m_owner->Where().Y(), m_owner->Where().Z(), 2 * m_owner->Where().Facing(), TEMPSPAWN_DEAD_DESPAWN, 0))
        {
            DEBUG_LOG("VehicleInfo(of %s)::Initialize: Load vehicle accessory %s onto seat %u", m_owner->GetGuidStr().c_str(), summoned->GetGuidStr().c_str(), itr->seatId);
            m_accessoryGuids.insert(summoned->GetObjectGuid());
            int32 basepoint0 = itr->seatId + 1;
            summoned->CastCustomSpell((Unit*)m_owner, SPELL_RIDE_VEHICLE_HARDCODED, &basepoint0, NULL, NULL, true);
        }
    }

    // Initialize movement limitations
    uint32 vehicleFlags = GetVehicleEntry()->Flags;
    Unit* pVehicle = (Unit*)m_owner;

    if (vehicleFlags & VEHICLE_FLAG_NO_STRAFE)
    {
        pVehicle->m_movementInfo.AddMovementFlags2(MOVEFLAG2_NO_STRAFE);
    }
    if (vehicleFlags & VEHICLE_FLAG_NO_JUMPING)
    {
        pVehicle->m_movementInfo.AddMovementFlags2(MOVEFLAG2_NO_JUMPING);
    }
    if (vehicleFlags & VEHICLE_FLAG_FULLSPEEDTURNING)
    {
        pVehicle->m_movementInfo.AddMovementFlags2(MOVEFLAG2_FULLSPEEDTURNING);
    }
    if (vehicleFlags & VEHICLE_FLAG_ALLOW_PITCHING)
    {
        pVehicle->m_movementInfo.AddMovementFlags2(MOVEFLAG2_ALLOW_PITCHING);
    }
    if (vehicleFlags & VEHICLE_FLAG_FULLSPEEDPITCHING)
    {
        pVehicle->m_movementInfo.AddMovementFlags2(MOVEFLAG2_FULLSPEEDPITCHING);
    }

    if (vehicleFlags & VEHICLE_FLAG_FIXED_POSITION)
    {
        // Never released: the vehicle is fixed for its life.
        pVehicle->GetMotionMaster()->Inhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::FixedVehicle, pVehicle->GetObjectGuid().GetCounter()));
    }

    // Initialize power type based on DBC values (creatures only)
    if (pVehicle->GetTypeId() == TYPEID_UNIT)
    {
        if (PowerDisplayEntry const* powerEntry = sPowerDisplayStore.LookupEntry(GetVehicleEntry()->PowerDisplayID_0))
        {
            pVehicle->SetPowerType(Powers(powerEntry->power));
        }
    }

    m_isInitialized = true;
}

/*
 * This function will board a passenger onto a vehicle
 *
 * @param passenger MUST be provided. This Unit will be boarded onto the vehicles (if it checks out)
 * @param seat      Seat to which the passenger will be boarded (if can, elsewise an alternative will be selected if possible)
 */
/* ******************************* Passengers, and their seats ************************* */

Geometry::Frame VehicleInfo::SeatFrame() const
{
    return Geometry::Frame::Deck(m_owner->GetObjectGuid().GetRawValue());
}

Geometry::Placement VehicleInfo::SeatPoseOf(Geometry::Vector3 const& worldPoint,
                                            float worldFacing) const
{
    // The seat pose is the vehicle's own local frame: the exact inverse of
    // CalculateGlobalPositionOf's composition (world = pos + R(o)*local, Basis().localToWorld),
    // i.e. local = R(o)^T * (world - pos) -- the same rotation Basis().worldToLocal applies.
    // The old core paired this side with its own negated composition
    // (TransportBase::NormalizeRotatedPosition); the core unification (e881112bd, #306,
    // 2026-07-29) replaced the composition with the standard localToWorld above but kept this
    // side's negation, so every rider composed by UpdateGlobalPositions on a moving vehicle has
    // sat at its reflection through the vehicle's centre since. No database row carries a seat
    // offset and VehicleSeatEntry's attachment offsets are still unused, so nothing outside this
    // function's own inverse depended on the old sign (see the caller/consumer audit in the
    // commit body).
    const Geometry::Vector3 delta = worldPoint - m_owner->Where().Pos();
    const Geometry::Vector3 flat =
        m_owner->Where().Basis().rot.transpose().mul(Geometry::Vector3(delta.x, delta.y, 0.0f));

    Geometry::Placement seat;
    seat.EnterFrame(SeatFrame(), Geometry::Vector3(flat.x, flat.y, delta.z),
                    worldFacing - m_owner->Where().Facing());
    return seat;
}

bool VehicleInfo::HasOnBoard(WorldObject const* passenger) const
{
    MANGOS_ASSERT(passenger);

    // Down from the (possible) passenger until we reach our owner, or no passenger at all.
    while (passenger->IsBoarded())
    {
        if (passenger->GetTransportInfo()->GetTransport() == m_owner)
        {
            return true;
        }

        passenger = passenger->GetTransportInfo()->GetTransport();
    }

    return false;
}

void VehicleInfo::BoardPassenger(WorldObject* passenger, Geometry::Placement const& seatPose,
                                 uint8 seat)
{
    TransportInfo* transportInfo = new TransportInfo(passenger, this, seatPose, seat);

    m_passengers.insert(PassengerMap::value_type(passenger, transportInfo));

    // The passenger needs fast access to transportInfo
    passenger->SetTransportInfo(transportInfo);
}

void VehicleInfo::UnBoardPassenger(WorldObject* passenger)
{
    PassengerMap::iterator itr = m_passengers.find(passenger);

    if (itr == m_passengers.end())
    {
        return;
    }

    // The seat pose is local to this vehicle's frame (BoardPassenger's own comment on
    // TransportInfo::SetSeatPose): copied here as a value -- the TransportInfo holding it is
    // deleted below, before anything that could run arbitrary code gets a chance to touch it.
    Geometry::Placement const seatPose = itr->second->Seat();

    // Leave the passenger map -- and delete the TransportInfo -- before anything below can run
    // arbitrary code. UpdateGlobalPositionOf's relocation calls OnRelocated (visibility,
    // notifiers) synchronously, and PassengerMap is an unordered_map: a passenger boarding onto
    // this same vehicle from inside that callback can rehash it and invalidate this iterator --
    // a use-after-invalidation on a delete/erase reached afterward. Erasing first also closes
    // the half-state window where GetTransportInfo() already read NULL but the seat still read
    // taken (GetTakenSeatsMask/GetPassenger) and the movement block still carried the old
    // transport offset to an observer entering view in that window.
    delete itr->second;
    m_passengers.erase(itr);

    // Stepping off must put the passenger back in the WORLD frame with the matching WORLD
    // position -- but SetTransportInfo(NULL) below only re-tags the frame (WorldObject::
    // RefreshFrame: "the pose is untouched: this says where the numbers are measured, not what
    // they are"), so a passenger unboarded straight from its seat pose would keep that pose's
    // LOCAL numbers, now mislabelled as world ones, if nothing composed them. UpdateGlobalPositionOf
    // does exactly that below: the same composition the 500 ms refresh in VehicleInfo::Update
    // uses to drag a seated rider along a moving vehicle, and it relocates through the map
    // (Map::CreatureRelocation / PlayerRelocation), keeping the grid cell and running
    // OnRelocated -- a plain Place().MoveTo would silently skip both.
    passenger->SetTransportInfo(NULL);  // re-tag to the World frame first: relocation notifiers
                                        // measure distances, and Placement fails closed across frames

    if (!passenger->IsInWorld())
    {
        // No live map to relocate through -- a teardown (e.g. ~VehicleInfo's
        // RemoveSpellsCausingAura, reached from Unit::~Unit, where GetMap() would assert). Keep
        // the frame re-tag and the seat-pose conversion, but write the composed pose directly,
        // the shape this code had before it started relocating through the map.
        float gx, gy, gz, go;
        CalculateGlobalPositionOf(seatPose.X(), seatPose.Y(), seatPose.Z(), seatPose.Facing(), gx, gy, gz, go);
        passenger->Place().MoveTo(gx, gy, gz, go);
        return;
    }

    // The old cell Map::PlayerRelocation unlinks from is read from the placement; a rider's may
    // still be the seat pose (local numbers, a cell at the map's centre) if nothing has composed
    // it yet. Seed it with the vehicle's own world pose first -- but ONLY while the placement
    // still equals the copied seat pose: a placement still holding the seat pose names a cell at
    // the map's centre, but one already composed by the 500 ms refresh names the rider's OWN
    // cell and must be left as it is, or the seed would introduce the very mismatch it exists to
    // avoid. (The cross-cell case, a seat offset straddling a cell edge, is P4-C's, the rider
    // placement model's own limit.) This is the placement's OLD position for the cell
    // derivation only -- the relocation just below writes the composed (and correct) pose over
    // it.
    if (passenger->Where().X() == seatPose.X() && passenger->Where().Y() == seatPose.Y() && passenger->Where().Z() == seatPose.Z())
    {
        passenger->Place().MoveTo(m_owner->Where().X(), m_owner->Where().Y(), m_owner->Where().Z(), m_owner->Where().Facing());
    }

    UpdateGlobalPositionOf(passenger, seatPose.X(), seatPose.Y(), seatPose.Z(), seatPose.Facing());
}

TransportInfo::TransportInfo(WorldObject* owner, VehicleInfo* transport,
                             Geometry::Placement const& seatPose, uint8 seat) :
    m_owner(owner),
    m_transport(transport),
    m_seatPose(seatPose),
    m_seat(seat)
{
    MANGOS_ASSERT(owner && m_transport);
    SetSeatPose(seatPose);
}

WorldObject* TransportInfo::GetTransport() const
{
    return m_transport->GetOwner();
}

ObjectGuid TransportInfo::GetTransportGuid() const
{
    return m_transport->GetOwner()->GetObjectGuid();
}

bool TransportInfo::IsOnVehicle() const
{
    return m_transport->GetOwner()->GetTypeId() == TYPEID_PLAYER ||
           m_transport->GetOwner()->GetTypeId() == TYPEID_UNIT;
}

bool TransportInfo::HasOnBoard(WorldObject const* passenger) const
{
    return m_transport->HasOnBoard(passenger);
}

void TransportInfo::SetSeatPose(Geometry::Placement const& seatPose)
{
    m_seatPose = seatPose;

    // The extent is the passenger's, not the pose's: a placement that carries none makes
    // every reach test act as though the riders were points.
    m_seatPose.Resize(m_owner->Where().Extent());

    m_owner->Place() = m_seatPose;

    // The create block a LATER observer receives carries this offset, so a rider that moved
    // and then had someone arrive would otherwise be drawn at the spot it boarded at.
    if (m_owner->GetTypeId() == TYPEID_UNIT || m_owner->GetTypeId() == TYPEID_PLAYER)
    {
        Unit* unit = static_cast<Unit*>(m_owner);
        if (!unit->m_movementInfo.GetTransportGuid().IsEmpty())
        {
            unit->m_movementInfo.SetTransportData(GetTransportGuid(), m_seatPose.X(),
                                                  m_seatPose.Y(), m_seatPose.Z(),
                                                  m_seatPose.Facing(), 0, m_seat);
        }
    }
}

void VehicleInfo::Board(Unit* passenger, uint8 seat)
{
    MANGOS_ASSERT(passenger);

    DEBUG_LOG("VehicleInfo(of %s)::Board: Try to board passenger %s to seat %u", m_owner->GetGuidStr().c_str(), passenger->GetGuidStr().c_str(), seat);

    // This check is also called in Spell::CheckCast()
    if (!CanBoard(passenger))
    {
        return;
    }

    // Use the planned seat only if the seat is valid, possible to choose and empty
    if (!IsSeatAvailableFor(passenger, seat))
        if (!GetUsableSeatFor(passenger, seat))
        {
            return;
        }

    VehicleSeatEntry const* seatEntry = GetSeatEntry(seat);
    MANGOS_ASSERT(seatEntry);

    // ToDo: Unboard passenger from a MOTransport when they are properly implemented
    /*if (TransportInfo* transportInfo = passenger->GetTransportInfo())
    {
        WorldObject* transporter = transportInfo->GetTransport();

        // Must be a MO transporter
        MANGOS_ASSERT(transporter->GetObjectGuid().IsMOTransport());

        ((Transport*)transporter)->UnBoardPassenger(passenger);
    }*/

    DEBUG_LOG("VehicleInfo::Board: Board passenger: %s to seat %u", passenger->GetGuidStr().c_str(), seat);

    // Stop the passenger's own spline HERE, while it is still a world object, not after it
    // becomes a rider: BoardPassenger (below) makes IsBoarded() true, and from that moment
    // CommitSplinePosition (Unit.cpp:5848-5866) treats a running spline's position as
    // SEAT-LOCAL and writes it straight into the seat pose. A root claimed on an already-
    // boarded passenger used to be exactly that trigger -- Inhibit -> Suspend -> the
    // generator's Interrupt -> InterruptMoving -- committing a wandering wolf's WORLD
    // position into the seat pose and the client's transport offset with it, which is why a
    // stationary vehicle still dragged its rider miles away the first time it moved: the
    // "seat pose" was never local to begin with.
    passenger->InterruptMoving();

    // A stop only PENDS the placement write for a world unit (applied on the passenger's
    // next Update, and dropped there if something else relocated it meanwhile --
    // Unit.cpp:530-549): the boarding position is the stop's pending commit when a spline ran,
    // the placement otherwise. Exception (pre-existing, not widened here): a passenger already
    // a rider on ANOTHER vehicle -- the cross-vehicle click path, no UnBoard first -- commits
    // straight into that other vehicle's seat pose instead (Unit.cpp:5857-5863, no pending path
    // for a rider), so Where() there is the OLD seat pose, composed below as if a world point.
    Geometry::Position const* pending = passenger->PendingSplineCommit();
    const float wx = pending ? pending->X() : passenger->Where().X();
    const float wy = pending ? pending->Y() : passenger->Where().Y();
    const float wz = pending ? pending->Z() : passenger->Where().Z();
    const float wo = pending ? pending->Facing() : passenger->Where().Facing();

    // Calculate passengers local position
    float lx, ly, lz, lo;
    CalculateBoardingPositionOf(wx, wy, wz, wo, lx, ly, lz, lo);

    // The rider's grid link moves to the vehicle's cell now, while the placement still holds
    // world numbers (Map::PlayerRelocation reads the old cell from it). BoardPassenger below
    // replaces the placement with the seat pose and never touches the link, so a boarding from
    // across a cell edge would otherwise leave the rider registered in the cell it came from:
    // every later relocation of it (the 500 ms composition, the unboard's seed) assumes the
    // vehicle's cell.
    if (passenger->IsInWorld() && m_owner->IsInWorld())
    {
        if (passenger->GetTypeId() == TYPEID_PLAYER)
        {
            m_owner->GetMap()->PlayerRelocation((Player*)passenger, m_owner->Where().X(), m_owner->Where().Y(), m_owner->Where().Z(), m_owner->Where().Facing());
        }
        else if (passenger->GetTypeId() == TYPEID_UNIT)
        {
            m_owner->GetMap()->CreatureRelocation((Creature*)passenger, m_owner->Where().X(), m_owner->Where().Y(), m_owner->Where().Z(), m_owner->Where().Facing());
        }
    }

    Geometry::Placement seatPose;
    seatPose.EnterFrame(SeatFrame(), Geometry::Vector3(lx, ly, lz), lo);
    BoardPassenger(passenger, seatPose, seat);

    // Set data for createobject packets
    // No ONTRANSPORT bit in 4.3.4: the transport guid in the movement block IS the flag.
    passenger->m_movementInfo.SetTransportData(m_owner->GetObjectGuid(), lx, ly, lz, lo, 0, seat);

    if (passenger->GetTypeId() == TYPEID_PLAYER)
    {
        Player* pPlayer = (Player*)passenger;
        pPlayer->RemovePet(PET_SAVE_AS_CURRENT);

        WorldPacket data(SMSG_ON_CANCEL_EXPECTED_RIDE_VEHICLE_AURA);
        pPlayer->GetSession()->SendPacket(&data);

        // SMSG_BREAK_TARGET (?)
    }

    passenger->GetMotionMaster()->Inhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::Seat, m_owner->GetObjectGuid().GetCounter(), seat));

    // A rider's path is seat-local: MoveSplineInit::Launch starts it from
    // GetTransportInfo()->Seat() -- the boarding offset BoardPassenger just set, wherever the
    // passenger happened to be standing -- and sends it as SMSG_MONSTER_MOVE_TRANSPORT with the
    // transport guid and seat index (MoveSplineInit.cpp:110-127, 180-188). The destination is
    // the seat's own attachment point (VehicleSeatEntry::AttachmentOffset_0..2, in the vehicle's
    // frame; many seats carry (0,0,0), the frame's own origin) facing the vehicle's own heading
    // (0.0 in the seat frame): this IS the boarding animation, the walk from the boarding spot to
    // the seat's modelled point, the same walk the client plays. As the spline ticks, the seat
    // pose itself walks with it -- CommitSplinePosition/UpdateSplineMovement write a rider's
    // spline position back through SetSeatPose -- settling at the attachment once the spline
    // finishes. A seat pose corrupted with a world position (the defect the earlier commits on
    // this branch fixed) was what once made this look like a drift toward the map origin; it
    // never was one -- (0,0,0) was always the seat frame's own origin, not the world's.
    MoveSend::SeatMove(*passenger,
                       Geometry::Vector3(seatEntry->AttachmentOffset_0,
                                         seatEntry->AttachmentOffset_1,
                                         seatEntry->AttachmentOffset_2),
                       true, true, 0.0f);

    // Apply passenger modifications
    ApplySeatMods(passenger, seatEntry->Flags);
}

/*
 * This function will switch the seat of a passenger on the same vehicle
 *
 * @param passenger MUST be provided. This Unit will change its seat on the vehicle
 * @param seat      Seat to which the passenger will be switched
 */
void VehicleInfo::SwitchSeat(Unit* passenger, uint8 seat)
{
    MANGOS_ASSERT(passenger);

    DEBUG_LOG("VehicleInfo::SwitchSeat: passenger: %s try to switch to seat %u", passenger->GetGuidStr().c_str(), seat);

    // Switching seats is not possible
    if (m_vehicleEntry->Flags & VEHICLE_FLAG_DISABLE_SWITCH)
    {
        return;
    }

    PassengerMap::const_iterator itr = m_passengers.find(passenger);
    MANGOS_ASSERT(itr != m_passengers.end());

    // We are already boarded to this seat
    if (itr->second->GetTransportSeat() == seat)
    {
        return;
    }

    // Check if it's a valid seat
    if (!IsSeatAvailableFor(passenger, seat))
    {
        return;
    }

    const uint8 oldSeat = itr->second->GetTransportSeat();
    VehicleSeatEntry const* seatEntry = GetSeatEntry(oldSeat);
    MANGOS_ASSERT(seatEntry);

    // Switching seats is only allowed if this flag is set
    if (~seatEntry->Flags & SEAT_FLAG_CAN_SWITCH)
    {
        return;
    }

    // Remove passenger modifications of the old seat
    RemoveSeatMods(passenger, seatEntry->Flags);

    // Claim the new seat's source before releasing the old one: the two identities differ (the
    // seat index is part of each), so the aggregate never falls to zero between them and the
    // client never sees an unroot immediately followed by a root. UnBoard(changeVehicle = true)
    // followed by the next vehicle's Board has the same shape across two different vehicles and
    // is left as it is.
    passenger->GetMotionMaster()->Inhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::Seat, m_owner->GetObjectGuid().GetCounter(), seat));
    passenger->GetMotionMaster()->Uninhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::Seat, m_owner->GetObjectGuid().GetCounter(), oldSeat));

    // Set to new seat
    itr->second->SetTransportSeat(seat);

    // Get seatEntry of new seat
    seatEntry = GetSeatEntry(seat);
    MANGOS_ASSERT(seatEntry);

    // Same reasoning as Board's own spline: the destination is the NEW seat's own attachment
    // point (VehicleSeatEntry::AttachmentOffset_0..2, the vehicle's frame; many seats carry
    // (0,0,0)), not (0,0,0) unconditionally. SetTransportSeat above only changed the seat
    // INDEX, so the walk is from wherever the pose settled on the old seat to the new seat's
    // modelled point -- the same boarding-style animation Board's own spline plays; the pose
    // itself follows as the spline ticks (see Board).
    // A seat switch goes out without the boarding flag, so the client walks the passenger
    // from where the pose settled to the new seat's modelled point rather than snapping.
    MoveSend::SeatMove(*passenger,
                       Geometry::Vector3(seatEntry->AttachmentOffset_0,
                                         seatEntry->AttachmentOffset_1,
                                         seatEntry->AttachmentOffset_2),
                       true);

    // Apply passenger modifications of the new seat
    ApplySeatMods(passenger, seatEntry->Flags);
}

/*
 * This function will Unboard a passenger
 *
 * @param passenger         MUST be provided. This Unit will be unboarded from the vehicle
 * @param changeVehicle     If set, the passenger is expected to be directly boarded to another vehicle,
 *                          and hence he will not be unboarded but only removed from this vehicle.
 */
void VehicleInfo::UnBoard(Unit* passenger, bool changeVehicle)
{
    MANGOS_ASSERT(passenger);

    DEBUG_LOG("VehicleInfo::Unboard: passenger: %s", passenger->GetGuidStr().c_str());

    PassengerMap::const_iterator itr = m_passengers.find(passenger);
    MANGOS_ASSERT(itr != m_passengers.end());

    const uint8 seat = itr->second->GetTransportSeat();
    VehicleSeatEntry const* seatEntry = GetSeatEntry(seat);
    MANGOS_ASSERT(seatEntry);

    // The symmetric of Board's own stop (see there): a rider spline still in flight here (the
    // board spline itself, or a seat switch's) writes seat-local coordinates through
    // CommitSplinePosition while IsBoarded() is still true (Unit.cpp:5857-5866), so stop it
    // before UnBoardPassenger reads the seat pose below -- otherwise the exit spline this
    // function launches further down would read that spline's still-in-flight, still-seat-local
    // ComputePosition() as if it were already a world position (MoveSplineInit.cpp:130-134,
    // the non-rider branch, which is what Launch takes once IsBoarded() has flipped to false).
    passenger->InterruptMoving();

    // The movement block stops naming this vehicle before the unboard relocates: the
    // relocation's OnRelocated can run a visibility update that builds a create block from
    // m_movementInfo for an observer entering view, which would otherwise still carry the old
    // transport guid, offset and seat for a unit no longer aboard. On a change of vehicle the
    // next Board writes the new vehicle's data.
    passenger->m_movementInfo.ClearTransportData();

    UnBoardPassenger(passenger);

    // Remove passenger modifications
    RemoveSeatMods(passenger, seatEntry->Flags);

    // Every unboard releases its own seat's root (C1b), changing vehicle or not: a passenger
    // moving straight to another vehicle would otherwise leave this seat's source held forever
    // (nothing ever unboards it a second time to release it); the new vehicle's Board claims
    // its own seat regardless.
    passenger->GetMotionMaster()->Uninhibit(Motion::Inhibition::Rooted, Motion::InhibitSource(Motion::SourceDomain::Seat, m_owner->GetObjectGuid().GetCounter(), seat));

    if (!changeVehicle)                                     // Send expected unboarding packages
    {
        if (passenger->GetTypeId() == TYPEID_PLAYER)
        {
            Player* pPlayer = (Player*)passenger;
            pPlayer->ResummonPetTemporaryUnSummonedIfAny();
            pPlayer->SetFallInformation(0, pPlayer->Where().Z());

            // SMSG_PET_DISMISS_SOUND (?)
        }

        // ToDo: Set proper unboard coordinates
        MoveSend::SeatMove(*passenger, m_owner->Where().Pos(), false);

        // Despawn if passenger was accessory
        if (passenger->GetTypeId() == TYPEID_UNIT && m_accessoryGuids.find(passenger->GetObjectGuid()) != m_accessoryGuids.end())
        {
            Creature* cPassenger = static_cast<Creature*>(passenger);
            // TODO Same TODO as in VehicleInfo::RemoveAccessoriesFromMap
            cPassenger->ForcedDespawn(5000);
            m_accessoryGuids.erase(passenger->GetObjectGuid());
        }
    }

    // Some creature vehicles get despawned after passenger unboarding
    if (m_owner->GetTypeId() == TYPEID_UNIT)
    {
        // TODO: Guesswork, but seems to be fairly near correct
        // Only if the passenger was on control seat? Also depending on some flags
        if ((seatEntry->Flags & SEAT_FLAG_CAN_CONTROL) &&
                !(m_vehicleEntry->Flags & (VEHICLE_FLAG_UNK4 | VEHICLE_FLAG_UNK20)))
        {
            if (((Creature*)m_owner)->IsTemporarySummon())
            {
                ((Creature*)m_owner)->ForcedDespawn(1000);
            }
        }
    }
}

/*
 * This function will check if a passenger can be boarded
 *
 * @param passenger         Unit that attempts to board onto a vehicle
 */
bool VehicleInfo::CanBoard(Unit* passenger) const
{
    if (!passenger)
    {
        return false;
    }

    // Passenger is this vehicle
    if (passenger == m_owner)
    {
        return false;
    }

    // Passenger is already on this vehicle (in this case switching seats is required)
    if (passenger->IsBoarded() && passenger->GetTransportInfo()->GetTransport() == m_owner)
    {
        return false;
    }

    // Prevent circular boarding: passenger (could only be vehicle) must not have m_owner on board
    if (passenger->IsVehicle() && passenger->GetVehicleInfo()->HasOnBoard(m_owner))
    {
        return false;
    }

    // Check if we have at least one empty seat
    if (!GetEmptySeats())
    {
        return false;
    }

    // Passenger is already boarded
    if (m_passengers.find(passenger) != m_passengers.end())
    {
        return false;
    }

    // Check for empty player seats
    if (passenger->GetTypeId() == TYPEID_PLAYER)
    {
        return GetEmptySeatsMask() & m_playerSeats;
    }

    // Check for empty creature seats
    return GetEmptySeatsMask() & m_creatureSeats;
}

Unit* VehicleInfo::GetPassenger(uint8 seat) const
{
    for (PassengerMap::const_iterator itr = m_passengers.begin(); itr != m_passengers.end(); ++itr)
        if (itr->second->GetTransportSeat() == seat)
        {
            return (Unit*)itr->first;
        }

    return NULL;
}

// Helper function to undo the turning of the vehicle to calculate a relative position of the passenger when boarding
void VehicleInfo::CalculateBoardingPositionOf(float gx, float gy, float gz, float go, float& lx, float& ly, float& lz, float& lo) const
{
    const Geometry::Placement seat = SeatPoseOf(Geometry::Vector3(gx, gy, gz), go);
    lx = seat.X();
    ly = seat.Y();
    lz = seat.Z();
    lo = seat.Facing();
}

void VehicleInfo::Update(uint32 diff)
{
    if (m_updatePositionsTimer < diff)
    {
        if ((m_owner->Where().Pos() - m_lastPose.Pos()).magnitude() > 1.0f ||
                Geometry::Placement::NormalizeOrientation(
                    m_owner->Where().Facing() - m_lastPose.Facing()) > 0.01f)
        {
            UpdateGlobalPositions();
        }

        m_updatePositionsTimer = 500;
    }
    else
    {
        m_updatePositionsTimer -= diff;
    }
}

void VehicleInfo::UpdateGlobalPositions()
{
    for (PassengerMap::const_iterator itr = m_passengers.begin(); itr != m_passengers.end(); ++itr)
    {
        Geometry::Placement const& seat = itr->second->Seat();
        UpdateGlobalPositionOf(itr->first, seat.X(), seat.Y(), seat.Z(), seat.Facing());
    }

    m_lastPose = m_owner->Where();
}

void VehicleInfo::UpdateGlobalPositionOf(WorldObject* passenger, float lx, float ly, float lz, float lo) const
{
    float gx, gy, gz, go;
    CalculateGlobalPositionOf(lx, ly, lz, lo, gx, gy, gz, go);

    if (passenger->GetTypeId() == TYPEID_PLAYER)
    {
        m_owner->GetMap()->PlayerRelocation((Player*)passenger, gx, gy, gz, go);
    }
    else if (passenger->GetTypeId() == TYPEID_UNIT)
    {
        m_owner->GetMap()->CreatureRelocation((Creature*)passenger, gx, gy, gz, go);
    }
    else
    {
        return;
    }

    if (((Unit*)passenger)->IsVehicle())
    {
        ((Unit*)passenger)->GetVehicleInfo()->UpdateGlobalPositions();
    }
}

void VehicleInfo::CalculateGlobalPositionOf(float lx, float ly, float lz, float lo, float& gx, float& gy, float& gz, float& go) const
{
    // A seat offset carried into the world by the vehicle's own basis -- the one place a
    // local-to-world composition IS legitimate, because a vehicle is a grid unit whose
    // pose the server actually owns (unlike a global transport's hull).
    const Geometry::Vector3 world =
        m_owner->Where().Basis().localToWorld(Geometry::Vector3(lx, ly, lz));
    gx = world.x;
    gy = world.y;
    gz = world.z;
    go = Geometry::Placement::NormalizeOrientation(lo + m_owner->Where().Facing());
}

void VehicleInfo::RemoveAccessoriesFromMap()
{
    // Remove all accessories
    for (GuidSet::const_iterator itr = m_accessoryGuids.begin(); itr != m_accessoryGuids.end(); ++itr)
    {
        if (Creature* pAccessory = m_owner->GetMap()->GetCreature(*itr))
        {
            // TODO - unclear how long to despawn, also maybe some flag etc depending
            pAccessory->ForcedDespawn(5000);
        }
    }
    m_accessoryGuids.clear();
    m_isInitialized = false;
}

/* ************************************************************************************************
 *          Helper function for seat control
 * ***********************************************************************************************/

/// Get the Vehicle SeatEntry of a seat by position
VehicleSeatEntry const* VehicleInfo::GetSeatEntry(uint8 seat) const
{
    VehicleSeatMap::const_iterator itr = m_vehicleSeats.find(seat);
    return itr != m_vehicleSeats.end() ? itr->second : NULL;
}

/*
 * This function will get a usable seat for a passenger
 *
 * @param passenger         MUST be provided. Unit for which to try to get a free seat
 * @param seat              will contain an available seat if returned true
 * @return                  return TRUE if and only if an available seat was found. In this case @seat will contain the id
 */
bool VehicleInfo::GetUsableSeatFor(Unit* passenger, uint8& seat) const
{
    MANGOS_ASSERT(passenger);

    uint8 possibleSeats = (passenger->GetTypeId() == TYPEID_PLAYER) ? (GetEmptySeatsMask() & m_playerSeats) : (GetEmptySeatsMask() & m_creatureSeats);

    // No usable seats available
    if (!possibleSeats)
    {
        return false;
    }

    // Start with 0
    seat = 0;

    for (uint8 i = 1; seat < MAX_VEHICLE_SEAT; i <<= 1, ++seat)
        if (possibleSeats & i)
        {
            return true;
        }

    return false;
}

/// Returns if a @passenger could board onto @seat - @passenger MUST be provided
bool VehicleInfo::IsSeatAvailableFor(Unit* passenger, uint8 seat) const
{
    MANGOS_ASSERT(passenger);

    return seat < MAX_VEHICLE_SEAT &&
           (GetEmptySeatsMask() & (passenger->GetTypeId() == TYPEID_PLAYER ? m_playerSeats : m_creatureSeats) & (1 << seat));
}

/// Wrapper to collect all taken seats
uint8 VehicleInfo::GetTakenSeatsMask() const
{
    uint8 takenSeatsMask = 0;

    for (PassengerMap::const_iterator itr = m_passengers.begin(); itr != m_passengers.end(); ++itr)
    {
        takenSeatsMask |= 1 << itr->second->GetTransportSeat();
    }

    return takenSeatsMask;
}

bool VehicleInfo:: IsUsableSeatForPlayer(uint32 seatFlags, uint32 seatFlagsB) const
{
    return seatFlags & SEAT_FLAG_CAN_EXIT ||
           seatFlags & SEAT_FLAG_UNCONTROLLED ||
           seatFlagsB &
           (SEAT_FLAG_B_USABLE_FORCED   | SEAT_FLAG_B_USABLE_FORCED_2 |
            SEAT_FLAG_B_USABLE_FORCED_3 | SEAT_FLAG_B_USABLE_FORCED_4);
}

/// Add control and such modifiers to a passenger if required
void VehicleInfo::ApplySeatMods(Unit* passenger, uint32 seatFlags)
{
    Unit* pVehicle = (Unit*)m_owner;                        // Vehicles are alawys Unit

    if (seatFlags & SEAT_FLAG_NOT_SELECTABLE)
    {
        passenger->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
    }

    if (passenger->GetTypeId() == TYPEID_PLAYER)
    {
        Player* pPlayer = (Player*)passenger;

        // group update
        if (pPlayer->GetGroup())
        {
            pPlayer->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_VEHICLE_SEAT);
        }

        if (seatFlags & SEAT_FLAG_CAN_CONTROL)
        {
            pPlayer->GetCamera().SetView(pVehicle);

            pPlayer->SetCharm(pVehicle);
            pVehicle->SetCharmerGuid(pPlayer->GetObjectGuid());

            pVehicle->GetMotionMaster()->Inhibit(Motion::Inhibition::Possessed, Motion::InhibitSource(Motion::SourceDomain::Possession, pPlayer->GetObjectGuid().GetCounter()));
            pVehicle->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);

            // Unconfirmed - default speed handling. Before the grant: a gait change is
            // server-only and the kernel refuses it on a client-driven unit.
            if (pVehicle->GetTypeId() == TYPEID_UNIT)
            {
                if (!pPlayer->IsWalking() && pVehicle->IsWalking())
                {
                    ((Creature*)pVehicle)->SetWalk(false, true);
                }
                else if (pPlayer->IsWalking() && !pVehicle->IsWalking())
                {
                    ((Creature*)pVehicle)->SetWalk(true, true);
                }
            }

            pPlayer->SetClientControl(pVehicle, 1);

            if (pVehicle->GetTypeId() == TYPEID_UNIT)
            {
                // set vehicle faction as per the controller faction
                ((Creature*)pVehicle)->SetFactionTemporary(pPlayer->getFaction(), TEMPFACTION_NONE);
            }
        }

        if (seatFlags & SEAT_FLAG_CAN_CAST)
        {
            CharmInfo* charmInfo = pVehicle->InitCharmInfo(pVehicle);
            charmInfo->InitVehicleCreateSpells();

            pPlayer->PossessSpellInitialize();
        }
    }
    else if (passenger->GetTypeId() == TYPEID_UNIT)
    {
        if (seatFlags & SEAT_FLAG_CAN_CONTROL)
        {
            passenger->SetCharm(pVehicle);
            pVehicle->SetCharmerGuid(passenger->GetObjectGuid());
        }

        ((Creature*)passenger)->AI()->SetCombatMovement(false);
        // Not entirely sure how this must be handled in relation to CONTROL
        // But in any way this at least would require some changes in the movement system most likely
        passenger->GetMotionMaster()->Clear(false, true);
        passenger->GetMotionMaster()->MoveIdle();
    }
}

/// Remove control and such modifiers to a passenger if they were added
void VehicleInfo::RemoveSeatMods(Unit* passenger, uint32 seatFlags)
{
    Unit* pVehicle = (Unit*)m_owner;

    if (seatFlags & SEAT_FLAG_NOT_SELECTABLE)
    {
        passenger->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
    }

    if (passenger->GetTypeId() == TYPEID_PLAYER)
    {
        Player* pPlayer = (Player*)passenger;

        // group update
        if (pPlayer->GetGroup())
        {
            pPlayer->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_VEHICLE_SEAT);
        }

        if (seatFlags & SEAT_FLAG_CAN_CONTROL)
        {
            pPlayer->SetCharm(NULL);
            pVehicle->SetCharmerGuid(ObjectGuid());

            pPlayer->SetClientControl(pVehicle, 0);

            pVehicle->GetMotionMaster()->Uninhibit(Motion::Inhibition::Possessed, Motion::InhibitSource(Motion::SourceDomain::Possession, pPlayer->GetObjectGuid().GetCounter()));
            pVehicle->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);

            // must be called after movement control unapplying
            pPlayer->GetCamera().ResetView();

            // The player's own grant after the vehicle's revoke (spec §5).
            pPlayer->SetClientControl(pPlayer, 1);

            // reset vehicle faction
            if (pVehicle->GetTypeId() == TYPEID_UNIT)
            {
                ((Creature*)pVehicle)->ClearTemporaryFaction();
            }
        }

        if (seatFlags & SEAT_FLAG_CAN_CAST)
        {
            pPlayer->RemovePetActionBar();
        }
    }
    else if (passenger->GetTypeId() == TYPEID_UNIT)
    {
        if (seatFlags & SEAT_FLAG_CAN_CONTROL)
        {
            passenger->SetCharm(NULL);
            pVehicle->SetCharmerGuid(ObjectGuid());
        }

        // Reinitialize movement
        ((Creature*)passenger)->AI()->SetCombatMovement(true, true);
        if (!passenger->getVictim())
        {
            passenger->GetMotionMaster()->Initialize();
        }
    }
}
