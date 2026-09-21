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

#include <cmath>
#include "Utilities/Errors.h"
#include "Utilities/MathDefines.h"
#include "Object.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "World.h"
#include "Creature.h"
#include "Player.h"
#include "Vehicle.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "UpdateData.h"
#include "UpdateMask.h"
#include "Util.h"
#include "MapManager.h"
#include "Log.h"
#include "Transports.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectPosSelector.h"
#include "TemporarySummon.h"
#include "movement/packet_builder.h"
#include "CreatureLinkingMgr.h"
#include "Chat.h"
#include "GameTime.h"

/**
 * @file WorldObjectSummon.cpp
 * @brief Cohesion split of Object.cpp -- WorldObject map binding, creature/gameobject summoning, near-point selection, visibility refresh and sound/music playback. Same WorldObject class; no behaviour change. CMake file(GLOB) over Object/ picks this file up automatically; Object.h is unchanged.
 */

/**
 * @brief Assigns the current map context to the world object.
 *
 * @param map The map to assign.
 */
void WorldObject::SetMap(Map* map)
{
    MANGOS_ASSERT(map);
    m_currMap = map;
    // lets save current map's Id/instanceId
    m_mapId = map->GetId();
    m_InstanceId = map->GetInstanceId();
    RefreshFrame();
}

/**
 * @brief Reset map
 *
 * Resets the map reference for this object.
 */
/**
 * @brief Resets the world object's map state.
 */
void WorldObject::ResetMap()
{
    m_currMap = NULL;
}

TerrainInfo const* WorldObject::GetTerrain() const
{
    MANGOS_ASSERT(m_currMap);
    return m_currMap->GetTerrain();
}

/**
 * @brief Add object to remove list
 *
 * Adds this object to the map's remove list for cleanup.
 */
/**
 * @brief Schedules the object for removal from the map.
 */
void WorldObject::AddObjectToRemoveList()
{
    GetMap()->AddObjectToRemoveList(this);
}

/**
 * @brief Summon creature
 * @param id Creature entry ID
 * @param x X coordinate
 * @param y Y coordinate
 * @param z Z coordinate
 * @param ang Orientation
 * @param spwtype Temporary spawn type
 * @param despwtime Despawn time
 * @param asActiveObject If true, set as active object
 * @param setRun If true, set run mode
 * @return Summoned creature pointer or NULL
 *
 * Summons a creature at the specified position.
 */
/**
 * @brief Summons a temporary creature near or at the requested position.
 *
 * @param id The creature template id.
 * @param x The summon x coordinate.
 * @param y The summon y coordinate.
 * @param z The summon z coordinate.
 * @param ang The summon orientation.
 * @param spwtype The temporary spawn type.
 * @param despwtime The despawn time in milliseconds.
 * @param asActiveObject true to mark the summon as active.
 * @param setRun true to make the summon run.
 * @return The summoned creature, or null on failure.
 */
Creature* WorldObject::SummonCreature(uint32 id, float x, float y, float z, float ang, TempSpawnType spwtype, uint32 despwtime, bool asActiveObject, bool setRun)
{
    CreatureInfo const* cinfo = ObjectMgr::GetCreatureTemplate(id);
    if (!cinfo)
    {
        sLog.outErrorDb("WorldObject::SummonCreature: Creature (Entry: %u) not existed for summoner: %s. ", id, GetGuidStr().c_str());
        return NULL;
    }

    TemporarySummon* pCreature = new TemporarySummon(GetObjectGuid());

    Team team = TEAM_NONE;
    if (GetTypeId() == TYPEID_PLAYER)
    {
        team = ((Player*)this)->GetTeam();
    }

    CreatureCreatePos pos(GetMap(), x, y, z, ang, GetPhaseMask());

    if (x == 0.0f && y == 0.0f && z == 0.0f)
    {
        pos = CreatureCreatePos(this, Where().Facing(), CONTACT_DISTANCE, ang);
    }

    if (!pCreature->Create(GetMap()->GenerateLocalLowGuid(cinfo->GetHighGuid()), pos, cinfo, team))
    {
        delete pCreature;
        return NULL;
    }

    pCreature->SetSpawn(pos);

    // Set run or walk before any other movement starts
    pCreature->SetWalk(!setRun);

    // Active state set before added to map
    pCreature->SetActiveObjectState(asActiveObject);

    pCreature->Summon(spwtype, despwtime);                  // Also initializes the AI and MMGen

    if (GetTypeId() == TYPEID_UNIT && ((Creature*)this)->AI())
    {
        ((Creature*)this)->AI()->JustSummoned(pCreature);
    }

    // Creature Linking, Initial load is handled like respawn
    if (pCreature->IsLinkingEventTrigger())
    {
        GetMap()->GetCreatureLinkingHolder()->DoCreatureLinkingEvent(LINKING_EVENT_RESPAWN, pCreature);
    }

    // return the creature therewith the summoner has access to it
    return pCreature;
}

/**
 * @brief Summon game object
 * @param id Game object entry ID
 * @param x X coordinate
 * @param y Y coordinate
 * @param z Z coordinate
 * @param angle Orientation
 * @param despwtime Despawn time in milliseconds
 * @return Summoned game object pointer or NULL
 *
 * Summons a game object at the specified position.
 */
/**
 * @brief Summons a temporary game object at the requested position.
 *
 * @param id The gameobject entry id.
 * @param x The summon x coordinate.
 * @param y The summon y coordinate.
 * @param z The summon z coordinate.
 * @param angle The summon orientation.
 * @param despwtime The despawn time in milliseconds.
 * @return The summoned game object, or null on failure.
 */
GameObject* WorldObject::SummonGameObject(uint32 id, float x, float y, float z, float angle, uint32 despwtime)
{
    GameObject* pGameObj = new GameObject;

    Map *map = FindMap();

    if (!map)
    {
        return NULL;
    }

    if (!pGameObj->Create(map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT), id, map, GetPhaseMask(), x, y, z, angle))
    {
        delete pGameObj;
        return NULL;
    }

    pGameObj->SetRespawnTime(despwtime/IN_MILLISECONDS);

    map->Add(pGameObj);
    pGameObj->AIM_Initialize();

    return pGameObj;
}

// how much space should be left in front of/ behind a mob that already uses a space
#define OCCUPY_POS_DEPTH_FACTOR                          1.8f

namespace MaNGOS
{
    /**
     * @brief Near used position functor
     *
     * Checks for used positions near an object for position selection.
     */
    class NearUsedPosDo
    {
        public:
            /**
             * @brief Constructor
             * @param obj Source object
             * @param center The point the used-position accounting is laid out around; the
             *        anchor's own placement when the caller has no live centre of its own.
             * @param searcher Object searching for position
             * @param absAngle Absolute angle
             * @param selector Position selector
             */
            NearUsedPosDo(WorldObject const& obj, Geometry::Vector3 const& center, WorldObject const* searcher, float absAngle, ObjectPosSelector& selector)
                : i_object(obj), i_searcher(searcher), i_absAngle(Geometry::Placement::NormalizeOrientation(absAngle)), i_selector(selector), i_at(obj.Where())
            {
                // The anchor's placement moved onto the centre: the frame (and so every
                // ShareFrame test below) stays the anchor's, only the point the distances and
                // bearings are measured from is the caller's.
                i_at.MoveTo(center);
            }

            void operator()(Corpse*) const {}
            void operator()(DynamicObject*) const {}

            /**
             * @brief Process creature
             * @param c Creature to process
             */
            void operator()(Creature* c) const
            {
                // skip self or target
                if (c == i_searcher || c == &i_object)
                {
                    return;
                }

                float x, y, z;

                if (c->IsStopped() || !c->GetMotionMaster()->GetDestination(x, y, z))
                {
                    x = c->Where().X();
                    y = c->Where().Y();
                }

                add(c, x, y);
            }

            /**
             * @brief Process generic unit
             * @param u Unit to process
             */
            template<class T>
            void operator()(T* u) const
            {
                // skip self or target
                if (u == i_searcher || u == &i_object)
                {
                    return;
                }

                float x, y;

                x = u->Where().X();
                y = u->Where().Y();

                add(u, x, y);
            }

            /**
             * @brief Add used position
             * @param u Object to add
             * @param x X coordinate
             * @param y Y coordinate
             *
             * Adds a used position to the selector.
             */
            void add(WorldObject* u, float x, float y) const
            {
                float dx = i_at.X() - x;
                float dy = i_at.Y() - y;
                float dist2d = sqrt((dx * dx) + (dy * dy));

                // It is ok for the objects to require a bit more space
                float delta = u->Where().Extent();
                if (i_selector.m_searchPosFor && i_selector.m_searchPosFor != u)
                {
                    delta += i_selector.m_searchPosFor->Where().Extent();
                }

                delta *= OCCUPY_POS_DEPTH_FACTOR;           // Increase by factor

                // u is too near/far away from i_object. Do not consider it to occupy space
                if (fabs(i_selector.m_searcherDist - dist2d) > delta)
                {
                    return;
                }

                float angle = i_at.BearingTo(u->Where()) - i_absAngle;

                // move angle to range -pi ... +pi, range before is -2Pi..2Pi
                if (angle > M_PI_F)
                {
                    angle -= 2.0f * M_PI_F;
                }
                else if (angle < -M_PI_F)
                {
                    angle += 2.0f * M_PI_F;
                }

                i_selector.AddUsedArea(u, angle, dist2d);
            }
        private:
            WorldObject const& i_object;
            WorldObject const* i_searcher;
            float              i_absAngle;
            ObjectPosSelector& i_selector;
            Geometry::Placement i_at;   ///< the anchor's placement, moved onto the centre
    };
}

namespace
{
    /// A point the component constructed, pulled back inside the map's coordinate bounds --
    /// which is the map's business, not the geometry's. The placement-taking form the
    /// object-anchored PointNear and the centre-taking free-spot search share.
    Geometry::Vector3 PointNearPlacement(Geometry::Placement const& at, float distance2d, float absAngle)
    {
        Geometry::Vector3 point = at.PointAt(distance2d, absAngle);
        MaNGOS::NormalizeMapCoord(point.x);
        MaNGOS::NormalizeMapCoord(point.y);
        return point;
    }

    /// HasLineOfSight(anchor, point) cast from an explicit centre instead of the anchor's
    /// placement: the same two-yard eye lift, the anchor's map and phase.
    bool HasLineOfSightFrom(WorldObject const& anchor, Geometry::Vector3 const& from, Geometry::Vector3 const& point)
    {
        return anchor.GetMap()->IsInLineOfSight(from.x, from.y, from.z + 2.0f,
                                                point.x, point.y, point.z + 2.0f, anchor.GetPhaseMask());
    }
}

/**
 * @brief Get 2D point near object
 * @param x Output X coordinate
 * @param y Output Y coordinate
 * @param distance2d Distance from object
 * @param absAngle Absolute angle
 *
 * Calculates a 2D point at the specified distance and angle
 * from this object.
 */
Geometry::Vector3 PointNear(WorldObject const& anchor, float distance2d, float absAngle)
{
    return PointNearPlacement(anchor.Where(), distance2d, absAngle);
}

/**
 * @brief Finds a point near an explicit centre while accounting for collisions and line of sight.
 *
 * The anchor still supplies the map, the phase, the frame, its own extent and the grid area the
 * neighbours are gathered from; `center` supplies every position the search itself is laid out
 * around, so a caller with a LIVE position (a target mid-spline) gets a spot that has moved with
 * it. A centre that IS the anchor's position reproduces the object-anchored answer exactly.
 *
 * @param anchor The object the search is anchored to.
 * @param center The point the spot is sought around.
 * @param searcher The object requesting the position.
 * @param x Receives the resulting x coordinate.
 * @param y Receives the resulting y coordinate.
 * @param z Receives the resulting z coordinate.
 * @param searcher_bounding_radius The requester's bounding radius.
 * @param distance2d The desired distance from the centre.
 * @param absAngle The preferred absolute angle.
 */
void FindFreeSpotNear(WorldObject const& anchor, Geometry::Vector3 const& center, WorldObject const* searcher,
                      float& x, float& y, float& z,
                      float searcher_bounding_radius, float distance2d, float absAngle)
{
    // The anchor's placement moved onto the centre: it carries the anchor's frame, so the
    // bearing below shares one with whatever it is measured against, as it always did.
    Geometry::Placement at = anchor.Where();
    at.MoveTo(center);

    const Geometry::Vector3 first = PointNearPlacement(at, distance2d, absAngle);
    x = first.x;
    y = first.y;
    const float init_z = z = center.z;

    // if detection disabled, return first point
    if (!sWorld.getConfig(CONFIG_BOOL_DETECT_POS_COLLISION))
    {
        if (searcher)
        {
            ClampToAllowedZ(*searcher, x, y, z, anchor.GetMap());       // update to LOS height if available
        }
        else
        {
            DropToGround(anchor, x, y, z);
        }
        return;
    }

    // or remember first point
    float first_x = x;
    float first_y = y;
    bool first_los_conflict = false;                        // first point LOS problems

    const float dist = distance2d + searcher_bounding_radius + anchor.Where().Extent();

    // prepare selector for work
    ObjectPosSelector selector(center.x, center.y, distance2d, searcher_bounding_radius, searcher);

    // adding used positions around object. The grid area is gathered around the ANCHOR's
    // placement, while every angle and distance the selector is fed is measured from the
    // CENTRE: the visit radius therefore carries the anchor's own displacement from that
    // centre on top of its reach, so that what it sweeps covers the live centre's whole
    // neighbourhood and not a disc offset from it. For the placement-anchored overload the
    // centre IS the anchor's position, the term is zero, and the visit is the one it always
    // was; for a live centre (a target mid-spline -- a charge runs at 24 yd/s, so ~0.8 yd per
    // 33 ms world tick, and a relocation lands only every POSITION_UPDATE_DELAY) it is what
    // stops a neighbour standing on the far side of the live centre from going unseen and its
    // body from failing to block the spot. Only the visit widens: the z tolerance below stays
    // the plain reach, being a question about the drop rather than about who is standing near.
    {
        const Geometry::Vector3 anchorPos = anchor.Where().Pos();
        const float offX = anchorPos.x - center.x;
        const float offY = anchorPos.y - center.y;
        const float visitDist = dist + std::sqrt(offX * offX + offY * offY);

        MaNGOS::NearUsedPosDo u_do(anchor, center, searcher, absAngle, selector);
        MaNGOS::WorldObjectWorker<MaNGOS::NearUsedPosDo> worker(&anchor, u_do);

        Cell::VisitAllObjects(&anchor, worker, visitDist);
    }

    // maybe can just place in primary position
    if (selector.CheckOriginalAngle())
    {
        if (searcher)
        {
            ClampToAllowedZ(*searcher, x, y, z, anchor.GetMap());       // update to LOS height if available
        }
        else
        {
            DropToGround(anchor, x, y, z);
        }

        if (fabs(init_z - z) < dist && HasLineOfSightFrom(anchor, center, Geometry::Vector3(x, y, z)))
        {
            return;
        }

        first_los_conflict = true;                          // first point have LOS problems
    }

    // set first used pos in lists
    selector.InitializeAngle();

    float angle;                                            // candidate of angle for free pos

    // select in positions after current nodes (selection one by one)
    while (selector.NextAngle(angle))                       // angle for free pos
    {
        const Geometry::Vector3 candidate = PointNearPlacement(at, distance2d, absAngle + angle);
        x = candidate.x;
        y = candidate.y;
        z = center.z;

        if (searcher)
        {
            ClampToAllowedZ(*searcher, x, y, z, anchor.GetMap());       // update to LOS height if available
        }
        else
        {
            DropToGround(anchor, x, y, z);
        }

        if (fabs(init_z - z) < dist && HasLineOfSightFrom(anchor, center, Geometry::Vector3(x, y, z)))
        {
            return;
        }
    }

    // BAD NEWS: not free pos (or used or have LOS problems)
    // Attempt find _used_ pos without LOS problem
    if (!first_los_conflict)
    {
        x = first_x;
        y = first_y;

        if (searcher)
        {
            ClampToAllowedZ(*searcher, x, y, z, anchor.GetMap());       // update to LOS height if available
        }
        else
        {
            DropToGround(anchor, x, y, z);
        }
        return;
    }

    // set first used pos in lists
    selector.InitializeAngle();

    // select in positions after current nodes (selection one by one)
    while (selector.NextUsedAngle(angle))                   // angle for used pos but maybe without LOS problem
    {
        const Geometry::Vector3 candidate = PointNearPlacement(at, distance2d, absAngle + angle);
        x = candidate.x;
        y = candidate.y;
        z = center.z;

        if (searcher)
        {
            ClampToAllowedZ(*searcher, x, y, z, anchor.GetMap());       // update to LOS height if available
        }
        else
        {
            DropToGround(anchor, x, y, z);
        }

        if (fabs(init_z - z) < dist && HasLineOfSightFrom(anchor, center, Geometry::Vector3(x, y, z)))
        {
            return;
        }
    }

    // BAD BAD NEWS: all found pos (free and used) have LOS problem :(
    x = first_x;
    y = first_y;

    if (searcher)
    {
        ClampToAllowedZ(*searcher, x, y, z, anchor.GetMap());           // update to LOS height if available
    }
    else
    {
        DropToGround(anchor, x, y, z);
    }
}

/**
 * @brief Finds a nearby point while accounting for collisions and line of sight.
 *
 * @param anchor The object the point is sought around.
 * @param searcher The object requesting the position.
 * @param x Receives the resulting x coordinate.
 * @param y Receives the resulting y coordinate.
 * @param z Receives the resulting z coordinate.
 * @param searcher_bounding_radius The requester's bounding radius.
 * @param distance2d The desired distance from the anchor.
 * @param absAngle The preferred absolute angle.
 */
void FindFreeSpotNear(WorldObject const& anchor, WorldObject const* searcher, float& x, float& y, float& z,
                      float searcher_bounding_radius, float distance2d, float absAngle)
{
    FindFreeSpotNear(anchor, anchor.Where().Pos(), searcher, x, y, z,
                     searcher_bounding_radius, distance2d, absAngle);
}

void ClosePointNear(WorldObject const& anchor, float& x, float& y, float& z, float bounding_radius,
                    float distance2d, float angle, WorldObject const* searcher)
{
    FindFreeSpotNear(anchor, searcher, x, y, z, bounding_radius,
                     Geometry::Placement::ContactSpread(distance2d, anchor.Where().Extent(), bounding_radius),
                     anchor.Where().Facing() + angle);
}

/**
 * @brief The contact point around an explicit centre.
 *
 * The object-anchored answer measured from a live position: the bearing to the searcher and the
 * free-spot layout both start at `center` rather than at whatever the anchor's placement last
 * recorded. A centre that IS that placement gives the identical point.
 */
void ContactPointNear(WorldObject const& anchor, Geometry::Vector3 const& center, WorldObject const* obj,
                      float& x, float& y, float& z, float distance2d)
{
    Geometry::Placement at = anchor.Where();
    at.MoveTo(center);
    FindFreeSpotNear(anchor, center, obj, x, y, z, obj->Where().Extent(),
                     Geometry::Placement::ContactSpread(distance2d, anchor.Where().Extent(),
                                                        obj->Where().Extent()),
                     at.BearingTo(obj->Where()));
}

void ContactPointNear(WorldObject const& anchor, WorldObject const* obj, float& x, float& y, float& z,
                      float distance2d)
{
    ContactPointNear(anchor, anchor.Where().Pos(), obj, x, y, z, distance2d);
}

void WorldObject::SetPhaseMask(uint32 newPhaseMask, bool update)
{
    m_phaseMask = newPhaseMask;

    if (update && IsInWorld())
    {
        UpdateVisibilityAndView();
    }
}

/**
 * @brief Plays a positional sound for one player or nearby players.
 *
 * @param sound_id The sound entry id.
 * @param target Optional single-player target.
 */
void WorldObject::PlayDistanceSound(uint32 sound_id, Player const* target /*= NULL*/) const
{
    WorldPacket data(SMSG_PLAY_OBJECT_SOUND, 4 + 8);
    data << uint32(sound_id);
    data << GetObjectGuid();
    data << GetObjectGuid();
    if (target)
    {
        target->SendDirectMessage(&data);
    }
    else
    {
        SendMessageToSet(&data, true);
    }
}

/**
 * @brief Plays a direct sound for one player or nearby players.
 *
 * @param sound_id The sound entry id.
 * @param target Optional single-player target.
 */
void WorldObject::PlayDirectSound(uint32 sound_id, Player const* target /*= NULL*/) const
{
    WorldPacket data(SMSG_PLAY_SOUND, 4);
    data << uint32(sound_id);
    data << ObjectGuid();
    if (target)
    {
        target->SendDirectMessage(&data);
    }
    else
    {
        SendMessageToSet(&data, true);
    }
}

/**
 * @brief Plays music for one player or nearby players.
 *
 * @param sound_id The music entry id.
 * @param target Optional single-player target.
 */
void WorldObject::PlayMusic(uint32 sound_id, Player const* target /*= NULL*/) const
{
    WorldPacket data(SMSG_PLAY_MUSIC, 4);
    data << uint32(sound_id);
    if (target)
    {
        target->SendDirectMessage(&data);
    }
    else
    {
        SendMessageToSet(&data, true);
    }
}

/**
 * @brief Refreshes both visibility and viewpoint-dependent visibility state.
 */
void WorldObject::UpdateVisibilityAndView()
{
    GetViewPoint().Call_UpdateVisibilityForOwner();
    UpdateObjectVisibility();
    GetViewPoint().Event_ViewPointVisibilityChanged();
}

/**
 * @brief Recomputes this object's visibility for nearby clients.
 */
void WorldObject::UpdateObjectVisibility()
{
    CellPair p = MaNGOS::ComputeCellPair(Where().X(), Where().Y());
    Cell cell(p);

    GetMap()->UpdateObjectVisibility(this, cell, p);
}

/**
 * @brief Adds the world object to the map's update queue.
 */
void WorldObject::AddToClientUpdateList()
{
    GetMap()->AddUpdateObject(this);
}

/**
 * @brief Remove from client update list
 *
 * Removes this object from the map's update list.
 */
void WorldObject::RemoveFromClientUpdateList()
{
    GetMap()->RemoveUpdateObject(this);
}

/**
 * @brief World object change accumulator
 *
 * Accumulates update data for a world object and nearby players.
 */
struct WorldObjectChangeAccumulator
{
    UpdateDataMapType& i_updateDatas; ///< Update data map
    WorldObject& i_object; ///< World object

    /**
     * @brief Constructor
     * @param obj World object
     * @param d Update data map
     */
    WorldObjectChangeAccumulator(WorldObject& obj, UpdateDataMapType& d) : i_updateDatas(d), i_object(obj)
    {
        // send self fields changes in another way, otherwise
        // with new camera system when player's camera too far from player, camera wouldn't receive packets and changes from player
        if (i_object.isType(TYPEMASK_PLAYER))
        {
            i_object.BuildUpdateDataForPlayer((Player*)&i_object, i_updateDatas);
        }
    }

    /**
     * @brief Visit cameras
     * @param m Camera map
     *
     * Builds update data for all camera owners that can see this object.
     */
    void Visit(CameraMapType& m)
    {
        for (CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        {
            Player* owner = iter->getSource()->GetOwner();
            if (owner != &i_object && owner->HaveAtClient(&i_object))
            {
                i_object.BuildUpdateDataForPlayer(owner, i_updateDatas);
            }
        }
    }

    /**
     * @brief Visit other grid references (no-op)
     */
    template<class SKIP> void Visit(GridRefManager<SKIP>&) {}
};

/**
 * @brief Build update data
 * @param update_players Map of players to their update data
 *
 * Builds update data for all players who can see this object.
 */
void WorldObject::BuildUpdateData(UpdateDataMapType& update_players)
{
    WorldObjectChangeAccumulator notifier(*this, update_players);
    Cell::VisitWorldObjects(this, notifier, GetMap()->GetBroadcastRadius());

    ClearUpdateMask(false);
}

bool WorldObject::IsControlledByPlayer() const
{
    switch (GetTypeId())
    {
        case TYPEID_GAMEOBJECT:
            return ((GameObject*)this)->GetOwnerGuid().IsPlayer();
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
            return ((Unit*)this)->IsCharmerOrOwnerPlayerOrPlayerItself();
        case TYPEID_DYNAMICOBJECT:
            return ((DynamicObject*)this)->GetCasterGuid().IsPlayer();
        case TYPEID_CORPSE:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Print coordinates error
 * @param x X coordinate
 * @param y Y coordinate
 * @param z Z coordinate
 * @param descr Description of the operation
 * @return Always false
 *
 * Logs an error when invalid coordinates are encountered.
 */
bool WorldObject::PrintCoordinatesError(float x, float y, float z, char const* descr) const
{
    sLog.outError("%s with invalid %s coordinates: mapid = %uu, x = %f, y = %f, z = %f", GetGuidStr().c_str(), descr, GetMapId(), x, y, z);
    return false;                                           // always false for continue assert fail
}

/**
 * @brief Set active object state
 * @param active If true, set as active object
 *
 * Sets whether this object is an active object (updated even when no players nearby).
 */
void WorldObject::SetActiveObjectState(bool active)
{
    if (m_isActiveObject == active || (isType(TYPEMASK_PLAYER) && !active))  // player shouldn't became inactive, never
    {
        return;
    }

    if (IsInWorld() && !isType(TYPEMASK_PLAYER))
        // player's update implemented in a different from other active worldobject's way
        // considered using a generic way in future
    {
        if (IsActiveObject() && !active)
        {
            GetMap()->RemoveFromActive(this);
        }
        else if (!IsActiveObject() && active)
        {
            GetMap()->AddToActive(this);
        }
    }
    m_isActiveObject = active;
}

