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

/**
 * @file MovementHandler.cpp
 * @brief Movement opcode handlers
 *
 * This file handles movement-related opcodes including:
 * - MSG_MOVE_WORLDPORT_ACK: Acknowledge map teleport
 * - MSG_MOVE_TELEPORT_ACK: Acknowledge teleport
 * - MSG_MOVE_HEARTBEAT: Movement heartbeat
 * - MSG_MOVE_SET_FACING: Set facing direction
 * - MSG_MOVE_JUMP: Jump
 * - MSG_MOVE_START_FORWARD: Start moving forward
 * - MSG_MOVE_START_BACKWARD: Start moving backward
 * - MSG_MOVE_STOP: Stop movement
 * - MSG_MOVE_START_STRAFE_LEFT: Start strafing left
 * - MSG_MOVE_START_STRAFE_RIGHT: Start strafing right
 * - MSG_MOVE_START_PITCH_UP: Start pitching up
 * - MSG_MOVE_START_PITCH_DOWN: Start pitching down
 * - MSG_MOVE_SET_RUN_MODE: Set run mode
 * - MSG_MOVE_SET_WALK_MODE: Set walk mode
 * - MSG_MOVE_FALL_LAND: Land after fall
 * - MSG_MOVE_START_SWIM: Start swimming
 * - MSG_MOVE_STOP_SWIM: Stop swimming
 * - MSG_MOVE_SPLASH: Water splash
 * - MSG_MOVE_ASCEND: Ascend (flying)
 * - MSG_MOVE_DESCEND: Descend (flying)
 *
 * Movement packets are validated and synchronized with the server's
 * authoritative position to prevent cheating.
 */

#include "Platform/Define.h"
#include "Common/TimeConstants.h"
#include <ctime>
#include "WorldPacket.h"
#include "WorldSession.h"
#include "OpcodeTable.h"
#include "Log.h"
#include "Corpse.h"
#include "Group.h"
#include "Player.h"
#include "Vehicle.h"
#include "SpellAuras.h"
#include "MapManager.h"
#include "Transports.h"
#include "TransportMap.h"
#include <cmath>
#include "BattleGround/BattleGround.h"
#include "MapPersistentStateMgr.h"
#include "ObjectMgr.h"
#include "ObjectLookup.h"
#include "movement/WireParity.h"
#include "wire/MovementCapture.h"
#include "wire/MovementFamilies.h"
#include "wire/MovementSequences.h"
#include "wire/TeleportCodec.h"
#include "wire/MoverCodec.h"
#include "Change.h"
#include "PacketMatrix.h"

/**
 * @brief Handles the packet-based worldport acknowledgement.
 *
 * @param recv_data The received opcode packet.
 */
void WorldSession::HandleMoveWorldportAckOpcode(WorldPacket& /*recv_data*/)
{
    DEBUG_LOG("WORLD: got MSG_MOVE_WORLDPORT_ACK.");
    HandleMoveWorldportAckOpcode();
}

/**
 * @brief Finalizes a far teleport after the client acknowledges worldport.
 */
void WorldSession::HandleMoveWorldportAckOpcode()
{
    // ignore unexpected far teleports
    if (!GetPlayer()->IsBeingTeleportedFar())
    {
        return;
    }

    // get start teleport coordinates (will used later in fail case)
    Geometry::Location old_loc(GetPlayer()->GetMapId(),
                          GetPlayer()->Where().X(), GetPlayer()->Where().Y(),
                          GetPlayer()->Where().Z(), GetPlayer()->Where().Facing());

    // get the teleport destination
    Geometry::Location& loc = GetPlayer()->GetTeleportDest();

    // possible errors in the coordinate validity check (only cheating case possible)
    if (!MapManager::IsValidMapCoord(loc.MapId(), loc.X(), loc.Y(), loc.Z(), loc.Facing()))
    {
        sLog.outError("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to a not valid location "
                      "(map:%u, x:%f, y:%f, z:%f) We port him to his homebind instead..",
                      GetPlayer()->GetGuidStr().c_str(), loc.MapId(), loc.X(), loc.Y(), loc.Z());
        // stop teleportation else we would try this again and again in LogoutPlayer...
        GetPlayer()->SetSemaphoreTeleportFar(false);
        // and teleport the player to a valid place
        GetPlayer()->TeleportToHomebind();
        return;
    }

    // get the destination map entry, not the current one, this will fix homebind and reset greeting
    MapEntry const* mEntry = sMapStore.LookupEntry(loc.MapId());

    Map* map = NULL;

    // prevent crash at attempt landing to not existed battleground instance
    if (mEntry->IsBattleGroundOrArena())
    {
        if (GetPlayer()->GetBattleGroundId())
        {
            map = sMapMgr.FindMap(loc.MapId(), GetPlayer()->GetBattleGroundId());
        }

        if (!map)
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far to nonexisten battleground instance "
                       " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
                       GetPlayer()->GetGuidStr().c_str(), loc.MapId(), loc.X(), loc.Y(), loc.Z());

            GetPlayer()->SetSemaphoreTeleportFar(false);

            // Teleport to previous place, if can not be ported back TP to homebind place
            if (!GetPlayer()->TeleportTo(old_loc))
            {
                DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s can not be ported to his previous place, teleporting him to his homebind place...",
                           GetPlayer()->GetGuidStr().c_str());
                GetPlayer()->TeleportToHomebind();
            }
            return;
        }
    }

    InstanceTemplate const* mInstance = ObjectMgr::GetInstanceTemplate(loc.MapId());

    // reset instance validity, except if going to an instance inside an instance
    if (GetPlayer()->m_InstanceValid == false && !mInstance)
    {
        GetPlayer()->m_InstanceValid = true;
    }

    GetPlayer()->SetSemaphoreTeleportFar(false);

    // relocate the player to the teleport destination
    if (!map)
    {
        map = sMapMgr.CreateMap(loc.MapId(), GetPlayer());
    }

    GetPlayer()->SetMap(map);
    GetPlayer()->Place().MoveTo(loc.X(), loc.Y(), loc.Z(), loc.Facing());

    // The client threw away every object it had when it left the old map, so the set of
    // "things he already has" is now a lie in the one direction that hurts: anything still
    // listed here will be skipped by UpdateVisibilityOf and never sent again. That
    // includes the vessel he is standing on, which exists on both sides of the seam and so
    // keeps its guid across it.
    GetPlayer()->m_clientGUIDs.clear();

    GetPlayer()->SendInitialPacketsBeforeAddToMap();
    // the CanEnter checks are done in TeleporTo but conditions may change
    // while the player is in transit, for example the map may get full
    // Aboard, this is HER map, not the one just named in SMSG_NEW_WORLD -- see
    // Player::BoardingMap. The client is loading the world map she sails and never learns
    // the other one exists.
    if (!GetPlayer()->BoardingMap()->Add(GetPlayer()))
    {
        // if player wasn't added to map, reset his map pointer!
        GetPlayer()->ResetMap();

        DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s was teleported far but couldn't be added to map "
                   " (map:%u, x:%f, y:%f, z:%f) Trying to port him to his previous place..",
                   GetPlayer()->GetGuidStr().c_str(), loc.MapId(), loc.X(), loc.Y(), loc.Z());

        // Teleport to previous place, if can not be ported back TP to homebind place
        if (!GetPlayer()->TeleportTo(old_loc))
        {
            DETAIL_LOG("WorldSession::HandleMoveWorldportAckOpcode: %s can not be ported to his previous place, teleporting him to his homebind place...",
                       GetPlayer()->GetGuidStr().c_str());
            GetPlayer()->TeleportToHomebind();
        }
        return;
    }

    // battleground state prepare (in case join to BG), at relogin/tele player not invited
    // only add to bg group and object, if the player was invited (else he entered through command)
    if (_player->InBattleGround())
    {
        // cleanup setting if outdated
        if (!mEntry->IsBattleGroundOrArena())
        {
            // We're not in BG
            _player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
            // reset destination bg team
            _player->SetBGTeam(TEAM_NONE);
        }
        // join to bg case
        else if (BattleGround* bg = _player->GetBattleGround())
        {
            if (_player->IsInvitedForBattleGroundInstance(_player->GetBattleGroundId()))
            {
                bg->AddPlayer(_player);
            }
        }
    }

    GetPlayer()->SendInitialPacketsAfterAddToMap();

    // flight fast teleport case
    if (GetPlayer()->Movement()->IsOnTaxi())
    {
        if (!_player->InBattleGround())
        {
            // The crossing's ack: the next map's leg, or the flight's end when this is not the
            // map it aimed at (P5-B family 5). The control stays revoked across the crossing:
            // SendInitialPacketsBeforeAddToMap skipped its grant.
            return;
        }

        // battleground state prepare, stop flight (the abort clears the route and returns the control)
        GetPlayer()->Movement()->Finish();
    }

    if (mInstance)
    {
        Difficulty diff = GetPlayer()->GetDifficulty(mEntry->IsRaid());
        if (MapDifficultyEntry const* mapDiff = GetMapDifficultyData(mEntry->ID, diff))
        {
            if (mapDiff->RaidDuration)
            {
                if (time_t timeReset = sMapPersistentStateMgr.GetScheduler().GetResetTimeFor(mEntry->ID, diff))
                {
                    uint32 timeleft = uint32(timeReset - time(NULL));
                    GetPlayer()->SendInstanceResetWarning(mEntry->ID, diff, timeleft);
                }
            }
        }
    }

    // mount allow check
    if (!mEntry->IsMountAllowed())
    {
        _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        _player->RemoveSpellsCausingAura(SPELL_AURA_FLY);
    }
    else
    {
        // recheck mount capabilities at far teleport
        Unit::AuraList const& mMountAuras = _player->GetAurasByType(SPELL_AURA_MOUNTED);
        for (Unit::AuraList::const_iterator itr = mMountAuras.begin(); itr != mMountAuras.end(); )
        {
            Aura const* aura = *itr;

            // mount is no longer suitable
            MountCapabilityEntry const* entry = _player->GetMountCapability(aura->GetSpellEffect()->EffectMiscValue_1);
            if (!entry)
            {
                _player->RemoveAurasDueToSpell(aura->GetId());
                itr = mMountAuras.begin();
                continue;
            }

            // mount capability changed
            if (int32(entry->ID) != aura->GetModifier()->m_amount)
            {
                if (MountCapabilityEntry const* oldEntry = sMountCapabilityStore.LookupEntry(aura->GetModifier()->m_amount))
                {
                    _player->RemoveAurasDueToSpell(oldEntry->SpeedModSpell);
                }

                _player->CastSpell(_player, entry->SpeedModSpell, true);

                const_cast<Aura*>(aura)->ChangeAmount(entry->ID);
            }

            ++itr;
        }

        uint32 zone, area;
        _player->GetTerrain()->GetZoneAndAreaId(zone, area, _player->Where().X(), _player->Where().Y(), _player->Where().Z());
        // recheck fly auras
        Unit::AuraList const& mFlyAuras = _player->GetAurasByType(SPELL_AURA_FLY);
        for (Unit::AuraList::const_iterator itr = mFlyAuras.begin(); itr != mFlyAuras.end(); )
        {
            Aura const* aura = *itr;
            if (!_player->CanStartFlyInArea(_player->GetMapId(), zone, area))
            {
                _player->RemoveAurasDueToSpell(aura->GetId());
                itr = mFlyAuras.begin();
                continue;
            }

            ++itr;
        }
    }

    // honorless target
    if (GetPlayer()->pvpInfo.inHostileArea)
    {
        GetPlayer()->CastSpell(GetPlayer(), 2479, true);
    }

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    // lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();

    // notify group after successful teleport
    if (Group* group = _player->GetGroup())
    {
        _player->SetGroupUpdateFlag(GROUP_UPDATE_FULL);

        // GROUP_UPDATE_FULL only refreshes member stats. An LFG group also
        // carries roster flags the client reads once, out of SMSG_GROUP_LIST,
        // and it drops that packet if it arrives during a loading screen --
        // so re-send the roster now that this member has actually landed.
        if (group->isLFGGroup())
        {
            group->SendUpdate();
        }
    }
}

/**
 * @brief Finalizes a near teleport after the client acknowledges it.
 *
 * @param recv_data The received opcode packet.
 */
void WorldSession::HandleMoveTeleportAckOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_MOVE_TELEPORT_ACK");

    ObjectGuid guid;
    uint32 counter, time;

    Wire::TeleportAck ack;
    Wire::DecodeResult const r = Wire::DecodeTeleportAck(recv_data, ack);
    if (!r.ok())
    {
        WireParity::Rejected(CMSG_MOVE_TELEPORT_ACK, r.error);
        throw ByteBufferException(false, recv_data.rpos(), 0, recv_data.size());
    }
    counter = ack.counter;
    time = ack.time;
    guid = ObjectGuid(ack.guid);

    DEBUG_LOG("Guid: %s", guid.GetString().c_str());
    DEBUG_LOG("Counter %u, time %u", counter, time / IN_MILLISECONDS);

    // The near teleport is a player's (a creature is never issued one); the ack must
    // name a member, and the landing runs for a player being teleported near.
    const bool isMember = Movers().MayAck(guid.GetRawValue());
    Unit* member = isMember ? MemberUnit(guid) : NULL;
    if (!member)
    {
        CountAck(&AckCounters::seen, &AckTotalsCounters::seen);
        // A member the map lost (out of the world, gone) is not a wrong guid.
        if (isMember)
        {
            Movers().Unresolved();
        }
        CountAck(&AckCounters::wrongGuid, &AckTotalsCounters::wrongGuid);
        return;
    }
    Player* plMover = member->GetTypeId() == TYPEID_PLAYER ? (Player*)member : NULL;
    if (!plMover || !plMover->IsBeingTeleportedNear())
    {
        return;
    }

    CountAck(&AckCounters::seen, &AckTotalsCounters::seen);

    // The kernel's pending teleport closes on this counter; the landing below runs
    // whatever it says -- a teleport the server issued must land, or the player stays
    // behind its semaphore.
    const uint32 now = GameTime::GetGameTimeMS();
    std::vector<Motion::Emission> emissions = plMover->MotionState().Ack(Motion::ChangeType::Teleport, counter, Motion::AckPayload(), now);
    switch (plMover->MotionState().LastAck())
    {
        case Motion::AckResult::Matched:   CountAck(&AckCounters::matched, &AckTotalsCounters::matched); break;
        case Motion::AckResult::Tombstone: CountAck(&AckCounters::tombstone, &AckTotalsCounters::tombstone); break;
        case Motion::AckResult::Future:    CountAck(&AckCounters::future, &AckTotalsCounters::future); break;
        default:                           CountAck(&AckCounters::stale, &AckTotalsCounters::stale); break;
    }

    plMover->SetSemaphoreTeleportNear(false);

    uint32 old_zone = plMover->GetTerrain()->GetZoneId(plMover->Where().X(), plMover->Where().Y(), plMover->Where().Z());

    Geometry::Location const& dest = plMover->GetTeleportDest();

    plMover->SetPosition(dest.X(), dest.Y(), dest.Z(), dest.Facing(), true);

    // Observers learn the landing from the kernel's teleport update, built from the
    // stored status at the destination.
    plMover->m_movementInfo.ChangePosition(dest.X(), dest.Y(), dest.Z(), dest.Facing());
    plMover->SendEmissions(emissions);

    uint32 newzone, newarea;
    plMover->GetTerrain()->GetZoneAndAreaId(newzone, newarea, plMover->Where().X(), plMover->Where().Y(), plMover->Where().Z());
    plMover->UpdateZone(newzone, newarea);

    // new zone
    if (old_zone != newzone)
    {
        // honorless target
        if (plMover->pvpInfo.inHostileArea)
        {
            plMover->CastSpell(plMover, 2479, true);
        }
    }

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    // lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

/**
 * @brief Processes standard client movement updates.
 *
 * @param recv_data The received opcode packet.
 */
void WorldSession::HandleMovementOpcodes(WorldPacket& recv_data)
{
    uint16 opcode = recv_data.GetOpcode();
    if (!sLog.HasLogFilter(LOG_FILTER_PLAYER_MOVES))
    {
        DEBUG_LOG("WORLD: Received opcode %s (%u, 0x%X)", LookupOpcodeName(opcode), opcode, opcode);
        recv_data.hexlike();
    }

    // ExecuteOpcode already recorded this packet if the wire layer knows it -- a
    // whole-packet layout or a family -- the exact complement of the condition
    // there; what neither describes is what the next worklist needs a capture of.
    if (Wire::MovementCapture::IsOpen() && !Wire::IsKnown(opcode))
    {
        Wire::MovementCapture::Record('C', opcode, recv_data.contents(), recv_data.size());
    }

    /* extract packet */
    MovementInfo movementInfo;
    recv_data >> movementInfo;
    /*----------------*/

    // Design v2 §7 (F1), the authority rung: the client moves the unit it selected and
    // only that one; anything else is dropped and counted before it is validated.
    if (!Movers().MovesAs(movementInfo.GetGuid().GetRawValue()))
    {
        if (movementInfo.GetStatusInfo().hasTransportData)
        {
            static bool toldMover = false;
            if (!toldMover)
            {
                toldMover = true;
                sLog.outError("Transport: deck packet for a guid this session does not move, "
                              "dropped (said once)");
            }
        }
        return;
    }
    Unit* mover = SelectedMover();
    if (!mover)
    {
        Movers().Unresolved();
        return;
    }
    Player* plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if (plMover && plMover->IsBeingTeleported())
    {
        return;
    }

    if (!VerifyMovementInfo(movementInfo))
    {
        return;
    }

    // fall damage generation (ignore in flight case that can be triggered also at lags in moment teleportation to another map).
    if (opcode == CMSG_MOVE_FALL_LAND && plMover && !plMover->IsTaxiFlying())
    {
        plMover->HandleFall(movementInfo);
    }

    /* process position-change */
    HandleMoverRelocation(mover, movementInfo);

    if (plMover)
    {
        plMover->UpdateFallInformationIfNeed(movementInfo, opcode);
    }

    // stop some emotes at player move
    if (mover && (mover->GetUInt32Value(UNIT_NPC_EMOTESTATE) != 0))
    {
        mover->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
    }

    WorldPacket data(SMSG_PLAYER_MOVE, recv_data.size());
    data << movementInfo;
    mover->SendMessageToSetExcept(&data, _player);
}

/**
 * @brief One handler for every movement ack the registry has a layout for.
 *
 * The ack is a movement status with a counter (and, for a speed or a height, the
 * value): decoded through the registry, checked for membership in the session's
 * allowed movers (a possessed creature's acks land in the creature's state), matched
 * by the kernel against the pending change of its type. A match relocates the mover
 * with the ack's status -- the client's position and flags at the moment it applied
 * the change -- and sends the observer packet the matrix names, built from that
 * status; a payload mismatch is counted and the change resent once (the kernel's
 * emission); everything else is counted and left alone. Design v2 §6.2, §7.
 *
 * @param recv_data The received ack.
 */
void WorldSession::HandleMovementAck(WorldPacket& recv_data)
{
    const uint16 opcode = recv_data.GetOpcode();

    MovementInfo movementInfo;
    recv_data >> movementInfo;
    CountAck(&AckCounters::seen, &AckTotalsCounters::seen);

    // Membership, not selection (design v2 §7): a change sent to the player is acked
    // as the player while the vehicle is the selected unit. The ack lands in the
    // kernel of the unit it names.
    const ObjectGuid guid = movementInfo.GetGuid();
    const bool isMember = Movers().MayAck(guid.GetRawValue());
    Unit* mover = isMember ? MemberUnit(guid) : NULL;
    if (!mover)
    {
        // A member the map lost (out of the world, gone) is not a wrong guid.
        if (isMember)
        {
            Movers().Unresolved();
        }
        CountAck(&AckCounters::wrongGuid, &AckTotalsCounters::wrongGuid);
        DEBUG_LOG("WorldSession::HandleMovementAck: %s acked %s for %s, not a unit it moves",
                  _player->GetGuidStr().c_str(), LookupOpcodeName(opcode), guid.GetString().c_str());
        return;
    }
    Player* plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    Motion::MatrixRow const* row = Motion::RowForAck(opcode);
    if (!row)
    {
        // The opcode table routes only ack opcodes here; a row-less one is a table edit
        // this handler has not seen.
        sLog.outError("WorldSession::HandleMovementAck: no matrix row for %s (0x%X)", LookupOpcodeName(opcode), opcode);
        return;
    }

    Motion::AckPayload payload;
    if (Motion::IsSpeed(row->type) || row->type == Motion::ChangeType::CollisionHeight)
    {
        payload.hasValue = true;
        payload.value = movementInfo.GetExtraFloat();
    }

    // Design v2 §10.1: the semantic rung comes before the kernel. A status that fails
    // it consumes nothing -- the entry stays pending for the timeout policy or the next
    // change of its type -- and is counted as unverified.
    if (!VerifyMovementInfo(movementInfo))
    {
        CountAck(&AckCounters::unverified, &AckTotalsCounters::unverified);
        return;
    }

    const uint32 now = GameTime::GetGameTimeMS();
    std::vector<Motion::Emission> emissions = mover->MotionState().Ack(row->type, movementInfo.GetCounter(), payload, now);
    switch (mover->MotionState().LastAck())
    {
        case Motion::AckResult::Matched:
            CountAck(&AckCounters::matched, &AckTotalsCounters::matched);
            break;
        case Motion::AckResult::PayloadMismatch:
        {
            CountAck(&AckCounters::mismatched, &AckTotalsCounters::mismatched);
            if (!emissions.empty())
            {
                CountAck(&AckCounters::resent, &AckTotalsCounters::resent);
            }
            float desired = row->type == Motion::ChangeType::CollisionHeight ? mover->MotionState().Desired().collisionHeight :
                             Motion::IsSpeed(row->type) ? mover->MotionState().Desired().speed[Motion::SpeedIndex(row->type)] : 0.0f;
            sLog.outError("WorldSession::HandleMovementAck: %s acked %s with %f, the server sent %f (counter %u): %s",
                          _player->GetName(), LookupOpcodeName(opcode), payload.value, desired,
                          movementInfo.GetCounter(), emissions.empty() ? "left to the timeout policy" : "resent once");
            break;
        }
        case Motion::AckResult::Tombstone:
            CountAck(&AckCounters::tombstone, &AckTotalsCounters::tombstone);
            break;
        case Motion::AckResult::NoPending:
        case Motion::AckResult::Stale:
            CountAck(&AckCounters::stale, &AckTotalsCounters::stale);
            break;
        case Motion::AckResult::Future:
            CountAck(&AckCounters::future, &AckTotalsCounters::future);
            break;
    }

    if (plMover && plMover->IsBeingTeleported())
    {
        // The client is answering while a near or far teleport is in flight (the resync's
        // reissue, or a change acked inside a teleport's window). The kernel took the ack
        // like any other and the emissions go out -- an observer form's position is one
        // the teleport update corrects a moment later, its speed or flag is not lost --
        // but the status is not stored: the teleport lands the player, not this packet.
        CountAck(&AckCounters::teleporting, &AckTotalsCounters::teleporting);
    }
    else if (mover->MotionState().LastAck() == Motion::AckResult::Matched)
    {
        // The ack's status is the mover's status now; the observer form is built from
        // it once it is stored.
        HandleMoverRelocation(mover, movementInfo);
    }
    mover->SendEmissions(emissions);
}

/**
 * @brief The client selects which of its allowed movers it moves (design v2 §7).
 *
 * Read through the codec (the previous body called the guid write templates on the
 * received packet and never read it); a stranger is refused and counted.
 *
 * @param recv_data The received opcode packet.
 */
void WorldSession::HandleSetActiveMoverOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_SET_ACTIVE_MOVER");

    Wire::ActiveMover mover;
    Wire::DecodeResult const r = Wire::DecodeActiveMover(recv_data, CMSG_SET_ACTIVE_MOVER, mover);
    if (!r.ok())
    {
        WireParity::Rejected(CMSG_SET_ACTIVE_MOVER, r.error);
        throw ByteBufferException(false, recv_data.rpos(), 0, recv_data.size());
    }

    if (!Movers().Select(mover.guid))
    {
        DEBUG_LOG("HandleSetActiveMoverOpcode: %s selected %s, which it may not move",
                  _player->GetGuidStr().c_str(), ObjectGuid(mover.guid).GetString().c_str());
    }
}

/**
 * @brief The client stops moving the unit it had selected (design v2 §7).
 *
 * The status the packet carries is not stored: the unit's last accepted movement
 * stands, and the next packet for the next selected unit is what moves anything.
 *
 * @param recv_data The received opcode packet.
 */
void WorldSession::HandleMoveNotActiveMoverOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("WORLD: Received opcode CMSG_MOVE_NOT_ACTIVE_MOVER");

    MovementInfo mi;
    recv_data >> mi;

    if (!Movers().Deselect(mi.GetGuid().GetRawValue()))
    {
        DEBUG_LOG("HandleMoveNotActiveMoverOpcode: %s deselected %s, not the selected unit",
                  _player->GetGuidStr().c_str(), mi.GetGuid().GetString().c_str());
    }
}

/**
 * @brief Broadcasts the player's mount special animation.
 *
 * @param recvdata The received opcode packet.
 */
void WorldSession::HandleMountSpecialAnimOpcode(WorldPacket& /*recvdata*/)
{
    // DEBUG_LOG("WORLD: Received opcode CMSG_MOUNTSPECIAL_ANIM");

    WorldPacket data(SMSG_MOUNTSPECIAL_ANIM, 8);
    data << GetPlayer()->GetObjectGuid();

    GetPlayer()->SendMessageToSet(&data, false);
}

/**
 * @brief Sends a knockback packet to the client.
 *
 * @param angle The horizontal knockback angle.
 * @param horizontalSpeed The horizontal speed component.
 * @param verticalSpeed The vertical speed component.
 */
void WorldSession::SendKnockBack(float angle, float horizontalSpeed, float verticalSpeed)
{
    Motion::KnockBackParams params;
    params.directionX = cos(angle);
    params.directionY = sin(angle);
    params.horizontal = horizontalSpeed;
    params.vertical = -verticalSpeed;   // as the wire carries it
    Player* player = GetPlayer();
    player->SendEmissions(player->MotionState().Apply(Motion::KnockBackChange(params), GameTime::GetGameTimeMS()));
}

/**
 * @brief Handles the client's response to a summon request.
 *
 * @param recv_data The received opcode packet.
 */
void WorldSession::HandleSummonResponseOpcode(WorldPacket& recv_data)
{
    if (!_player->IsAlive() || _player->IsInCombat())
    {
        return;
    }

    ObjectGuid summonerGuid;
    bool agree;
    recv_data >> summonerGuid;
    recv_data >> agree;

    _player->SummonIfPossible(agree);
}

/**
 * @brief Verifies movement coordinates and transport offsets.
 *
 * @param movementInfo The movement state to validate.
 * @return true if the movement data is valid; otherwise false.
 */
bool WorldSession::VerifyMovementInfo(MovementInfo const& movementInfo) const
{
    if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->X(), movementInfo.GetPos()->Y(), movementInfo.GetPos()->Z(), movementInfo.GetPos()->Facing()))
    {
        return false;
    }

    MovementInfo::StatusInfo const& si = movementInfo.GetStatusInfo();
    if (si.hasTransportData)
    {
        // WHY A DECK PACKET WAS THROWN AWAY. Refusing one silently is indistinguishable
        // from the client never sending it, and the two have opposite causes. Each reason
        // is said once and then stays quiet, so a client that argues every tick cannot
        // bury the log.
        static bool toldEmpty = false, toldExtent = false, toldCoord = false;
        // The wire's gate, not the guid, decides whether a transport block is
        // present -- and the writer forwards it on that gate. A block announced
        // with an empty guid names no transport the server knows; it is dropped
        // here rather than relayed as one.
        if (movementInfo.GetTransportGuid().IsEmpty())
        {
            if (!toldEmpty)
            {
                toldEmpty = true;
                sLog.outError("Transport: deck packet with no guid, dropped (said once)");
            }
            return false;
        }

        // WHERE HE STANDS ON THE DECK MAP. The wire spells this field t_x/t_y/t_z and the
        // protocol calls it an offset, but the moment it is ours it is a position on the
        // vessel's own map -- nothing is composed with it, ever.
        //
        // So the test is whether it names a place on that map, and a map's bounds are the
        // hull's. The old one compared 50 yards against the POSITIVE side alone and threw
        // the whole packet away otherwise, which froze the stored position at the last one
        // accepted: even a common transport ship reaches x[-59, +45], so anyone forward of
        // the mast stopped moving at 50.
        //
        // The guard it replaces is still needed: a leaving zeppelin sometimes reports these
        // as absolute continent coordinates, and those are thousands.
        const Geometry::Position* onDeck = movementInfo.GetTransportPos();

        float extent = MAX_DECK_EXTENT;
        if (Transport* vessel = _player
                                    ? Transport::GetTransport(_player->GetMap(),
                                                              movementInfo.GetTransportGuid())
                                    : NULL)
        {
            extent = vessel->AsMap() ? vessel->AsMap()->HullRadius() + DECK_EDGE_MARGIN
                                     : MAX_DECK_EXTENT;
        }

        if (std::fabs(onDeck->X()) > extent || std::fabs(onDeck->Y()) > extent ||
            std::fabs(onDeck->Z()) > extent)
        {
            if (!toldExtent)
            {
                toldExtent = true;
                sLog.outError("Transport: deck position (%.2f %.2f %.2f) outside hull extent "
                              "%.2f for guid %s, dropped (said once)",
                              onDeck->X(), onDeck->Y(), onDeck->Z(), extent,
                              movementInfo.GetTransportGuid().GetString().c_str());
            }
            return false;
        }

        if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->X() + movementInfo.GetTransportPos()->X(), movementInfo.GetPos()->Y() + movementInfo.GetTransportPos()->Y(),
                                     movementInfo.GetPos()->Z() + movementInfo.GetTransportPos()->Z(), movementInfo.GetPos()->Facing() + movementInfo.GetTransportPos()->Facing()))
        {
            if (!toldCoord)
            {
                toldCoord = true;
                sLog.outError("Transport: deck packet sums to an invalid map coord, dropped (said once)");
            }
            return false;
        }
    }

    return true;
}

/**
 * @brief Applies validated movement info to the current mover.
 *
 * @param mover The unit the movement info applies to.
 * @param movementInfo The movement state to apply.
 */
void WorldSession::HandleMoverRelocation(Unit* mover, MovementInfo& movementInfo)
{
    // Design v2 6.3: the client's timestamp is rebased to server time through the
    // session clock before it is stored or relayed. Until the first time-sync
    // pair lands the clock falls back to server-now and counts it.
    movementInfo.UpdateTime(m_timeBase.Rebase(movementInfo.GetTime(), GameTime::GetGameTimeMS()));

    if (Player* plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL)
    {
        // BEFORE the transport branch, and the ordering is the whole of it.
        // TransportMap::Add reads the passenger's OWN m_movementInfo for his deck offset; the
        // assignment used to sit below this branch, so on the first ONTRANSPORT packet Add read
        // the PREVIOUS one -- which carries no transport data -- and stood him on the hull
        // origin. His minions were then drawn beside (0, 0, 0): a yard off the keel and six
        // metres under the deck, which is what "the pet comes up through the walls" was.
        // WHETHER THE SERVER IS STILL HOLDING HIM TO THE DECK. In 4.3.4 there is no
        // ONTRANSPORT bit: the transport guid in the movement block IS the flag, so the
        // only thing that can go wrong is that guid arriving, or being kept, or not. Logged
        // on every CHANGE and never otherwise, so it is one line per boarding rather than
        // one per packet.
        {
            const bool wasAboard = !plMover->m_movementInfo.GetTransportGuid().IsEmpty();
            const bool nowAboard = !movementInfo.GetTransportGuid().IsEmpty();
            if (wasAboard != nowAboard)
            {
                sLog.outString("Transport: %s %s deck guid %s (server holds %s, hull map %s)",
                               plMover->GetName(),
                               nowAboard ? "reports" : "drops",
                               movementInfo.GetTransportGuid().GetString().c_str(),
                               plMover->m_transport ? "yes" : "no",
                               (plMover->FindMap() && plMover->FindMap()->AsTransport()) ? "yes" : "no");
            }
        }

        plMover->m_movementInfo = movementInfo;

        if (movementInfo.GetTransportGuid())
        {
            if (!plMover->m_transport)
            {
                // THE GUID ARRIVED BUT MAY NAME NOTHING WE HOLD. A global transport that was
                // never registered in this set cannot be found here, and the failure is
                // silent: the passenger simply never boards, stays on the world map, and
                // watches the hull sail out from under him. Said once per guid.
                bool found = false;

                // elevators also cause the client to send transport guid - just unmount if the guid can be found in the transport list
                for (MapManager::TransportSet::const_iterator iter = sMapMgr.m_Transports.begin(); iter != sMapMgr.m_Transports.end(); ++iter)
                {
                    if ((*iter)->GetObjectGuid() == movementInfo.GetTransportGuid())
                    {
                        found = true;
                        plMover->m_transport = (*iter);

                        // He walked aboard, so his client already has the vessel and is
                        // rendering the map she sails; moving him onto her own map is safe
                        // at once. Nothing tells the client -- it never learns that id.
                        if (TransportMap* hull = (*iter)->AsMap())
                        {
                            hull->Embark(plMover);
                        }
                        break;
                    }
                }

                if (!found)
                {
                    static ObjectGuid toldAbout;
                    if (toldAbout != movementInfo.GetTransportGuid())
                    {
                        toldAbout = movementInfo.GetTransportGuid();
                        sLog.outError("Transport: %s reports deck guid %s, which is in NO "
                                      "registered transport (%u held). He will not board.",
                                      plMover->GetName(),
                                      movementInfo.GetTransportGuid().GetString().c_str(),
                                      uint32(sMapMgr.m_Transports.size()));
                    }
                }
            }
        }
        else if (plMover->m_transport)               // if we were on a transport, leave
        {
            // He walked ashore, and his own client just told us where: that world point is
            // better than anything we could derive from a hull whose pose we only estimate.
            //
            // BUT ONLY IF IT IS NEXT TO THE SHIP. Nobody steps off a vessel onto another
            // continent, and the number in this field is not always his: while he is aboard
            // we tell him his world position is (0, 0, 0), and he echoes it straight back.
            // Drop the flag for one packet -- the client does, on arrival, before it has
            // resolved the hull -- and that zero is read as a destination. (0, 0, 0) on map
            // 0 is the middle of Lordamere Lake, which is exactly where people landed.
            if (TransportMap* hull = plMover->m_transport->AsMap())
            {
                Transport* vessel = plMover->m_transport;

                const float reach = hull->HullRadius() + vessel->NodeSlack() +
                                    DECK_EDGE_MARGIN;

                const bool ashore = vessel->Where().WithinDist(
                    Geometry::Vector3(movementInfo.GetPos()->X(),
                                      movementInfo.GetPos()->Y(),
                                      movementInfo.GetPos()->Z()), reach);

                if (ashore)
                {
                    hull->Disembark(plMover, movementInfo.GetPos()->X(),
                                    movementInfo.GetPos()->Y(), movementInfo.GetPos()->Z(),
                                    movementInfo.GetPos()->Facing());
                }
                else
                {
                    // Not a step ashore at all. Put him down on the ship's own coarse pose:
                    // wrong by a hull's length at worst, instead of by a continent.
                    hull->Disembark(plMover, vessel->Where().X(), vessel->Where().Y(),
                                    vessel->Where().Z(), vessel->Where().Facing());
                }
            }
            plMover->m_transport = NULL;
            movementInfo.ClearTransportData();
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) != plMover->IsInWater())
        {
            // now client not include swimming flag in case jumping under water
            plMover->SetInWater(!plMover->IsInWater() || plMover->GetTerrain()->IsUnderWater(movementInfo.GetPos()->X(), movementInfo.GetPos()->Y(), movementInfo.GetPos()->Z()));
        }

        // Aboard, the deck offset IS his position: it is what the client computed against
        // the hull it is drawing, and the world pair in the same packet describes a place
        // on a map he is no longer filed under. Ashore, the two are the same packet field.
        if (plMover->m_transport && plMover->GetMap()->AsTransport())
        {
            const Geometry::Position* offset = movementInfo.GetTransportPos();
            plMover->SetPosition(offset->X(), offset->Y(), offset->Z(), offset->Facing());
        }
        else if (plMover->IsBoarded())
        {
            // A VEHICLE rider, which is neither of the cases above: he has no m_transport
            // (that is a vessel's, and a vehicle is a creature) and his map is the ordinary
            // world one. His placement, though, is his SEAT POSE -- a seat-local point, which
            // the vehicle's own movement composes into the world.
            //
            // Relocating him from this packet would hand Map::PlayerRelocation a seat-local
            // pair as if it were a world one. It reads the OLD cell from the placement, so it
            // looks up a cell near the origin, gets no grid back for it, and dereferences the
            // null: Map.cpp's RemoveFromGrid, which is the crash a player boarding a Krazz
            // Cannon hit on 2026-09-20 (the "Creature X: 4.2 Y: -0.05 in grid[46,35] instead
            // grid[32,31]" complaints at shutdown are the same seat-local-as-world confusion,
            // one step short of the fault).
            //
            // Nothing needs to move here in any case. The seat pose is the server's: it comes
            // from VehicleSeat.dbc when he boards and from the vehicle's movement thereafter,
            // and the client is not authoritative over where a seat is. So keep his movement
            // info -- the flags, the transport guid, the timestamp all still matter -- and
            // leave the placement to the vehicle.
        }
        else
        {
            plMover->SetPosition(movementInfo.GetPos()->X(), movementInfo.GetPos()->Y(), movementInfo.GetPos()->Z(), movementInfo.GetPos()->Facing());
        }
        plMover->m_movementInfo = movementInfo;

        /* Movement should cancel looting */
        if (ObjectGuid lootGUID = plMover->GetLootGuid())
        {
            plMover->SendLootRelease(lootGUID);
        }

        // The old -500 cutoff assumed nothing legitimate sat that low; that held
        // for old-world maps but not Cata's deep-ocean zones. Vashj'ir's Abyssal
        // Depths has live spirit-healer/graveyard content down to ~ -1800
        // (confirmed against creature/gameobject/areatrigger_teleport data), and a
        // swimmer sits around -950, so a -500 cutoff void-kills legitimate players
        // there. This is a plain Z threshold independent of the map/liquid data,
        // so it must sit below all real content regardless of the terrain-height
        // fix: -3000 keeps a large margin below the deepest legitimate depth while
        // still catching a genuine fall through the map (which keeps accelerating).
        if (movementInfo.GetPos()->Z() < -3000.0f)
        {
            if (plMover->GetBattleGround()
                && plMover->GetBattleGround()->HandlePlayerUnderMap(_player))
            {
                // do nothing, the handle already did if returned true
            }
            else
            {
                // NOTE: this is actually called many times while falling
                // even after the player has been teleported away
                // TODO: discard movement packets after the player is rooted
                if (plMover->IsAlive())
                {
                    plMover->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, plMover->GetMaxHealth());
                    // pl can be alive if GM/etc
                    if (!plMover->IsAlive())
                    {
                        // change the death state to CORPSE to prevent the death timer from
                        // starting in the next player update
                        plMover->KillPlayer();
                        plMover->BuildPlayerRepop();
                    }
                }

                // cancel the death timer here if started
                plMover->RepopAtGraveyard();
            }
        }
    }
    else                                                    // creature charmed
    {
        // The stored status is what the observer form and the create block read;
        // a possessed creature's observer packets carried its pre-possession
        // flags, time and position before.
        mover->m_movementInfo = movementInfo;

        if (mover->IsInWorld() && mover->GetTypeId() == TYPEID_UNIT)
        {
            mover->GetMap()->CreatureRelocation((Creature*)mover, movementInfo.GetPos()->X(), movementInfo.GetPos()->Y(), movementInfo.GetPos()->Z(), movementInfo.GetPos()->Facing());
        }
    }
}
