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

#ifndef MANGOS_H_MOVE_MAP
#define MANGOS_H_MOVE_MAP

#include "Define.h"

#include "../../dep/recastnavigation/Detour/Include/DetourAlloc.h"
#include "../../dep/recastnavigation/Detour/Include/DetourNavMesh.h"
#include "../../dep/recastnavigation/Detour/Include/DetourNavMeshQuery.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

class Unit;

//  memory management
inline void* dtCustomAlloc(size_t size, dtAllocHint /*hint*/)
{
    return (void*)new unsigned char[size];
}

inline void dtCustomFree(void* ptr)
{
    delete[](unsigned char*)ptr;
}

//  move map related classes
namespace MMAP
{
    typedef std::unordered_map<uint32, dtTileRef> MMapTileSet;
    typedef std::unordered_map<uint32, dtNavMeshQuery*> NavMeshQuerySet;

    // dummy struct to hold map's mmap data
    struct MMapData
    {
        MMapData(dtNavMesh* mesh) : navMesh(mesh) {}
        ~MMapData()
        {
            for (NavMeshQuerySet::iterator i = navMeshQueries.begin(); i != navMeshQueries.end(); ++i)
            {
                dtFreeNavMeshQuery(i->second);
            }

            if (navMesh)
            {
                dtFreeNavMesh(navMesh);
            }
        }

        dtNavMesh* navMesh;

        // we have to use single dtNavMeshQuery for every instance, since those are not thread safe
        NavMeshQuerySet navMeshQueries;     // instanceId to query
        MMapTileSet mmapLoadedTiles;        // maps [map grid coords] to [dtTile]
    };


    typedef std::unordered_map<uint32, MMapData*> MMapDataSet;

    // singelton class
    // holds all all access to mmap loading unloading and meshes
    //
    // ONE MANAGER, EVERY MAP THREAD. There is a single instance of this class for the
    // whole process, and its entries are keyed by map id -- so every instance of a
    // dungeon shares one MMapData, one dtNavMesh and one set of queries, while each
    // instance is a separate Map ticked on its own MapUpdater worker. Nothing here was
    // synchronised: two instances of the same map reaching GetNavMeshQuery() for the
    // first time both inserted into the same unordered_map, and a grid load on one
    // instance called dtNavMesh::addTile() on the mesh the other was walking.
    //
    // So m_lock guards two different things at two different grains:
    //
    //   the CONTAINERS -- loadedMMaps, mmapLoadedTiles, navMeshQueries, loadedTiles --
    //   for the length of whichever method touches them, and
    //
    //   the MESH ITSELF, for the length of a whole route. Detour reads the tile array
    //   throughout a query, so a route must hold the lock shared from the moment it
    //   takes the mesh pointer until it has finished with it. That is what Route is
    //   for; addTile/removeTile take the lock exclusive and therefore wait.
    class MMapManager
    {
        public:
            MMapManager() : loadedTiles(0) {}
            ~MMapManager();

            /**
             * @brief Everything one route needs, with the read lock held for its lifetime.
             *
             * Hand it out, keep it on the stack for the duration of the query, let it go.
             * While it lives no tile can be added to or removed from the mesh it names,
             * and the instance's dtNavMeshQuery cannot be freed underneath it -- which is
             * the whole reason the pointers are safe to use at all.
             *
             * Mesh() is NULL when the map has no navmesh, which is the ordinary case for a
             * map with no mmtiles baked, not an error. Query() is NULL additionally when
             * the route was opened for the mesh alone.
             */
            class Route
            {
                public:
                    Route() : m_mesh(nullptr), m_query(nullptr) {}
                    Route(std::shared_lock<std::shared_mutex> hold,
                          dtNavMesh const* mesh, dtNavMeshQuery const* query)
                        : m_hold(std::move(hold)), m_mesh(mesh), m_query(query) {}

                    Route(Route&&) = default;
                    Route& operator=(Route&&) = default;
                    Route(const Route&) = delete;
                    Route& operator=(const Route&) = delete;

                    dtNavMesh const* Mesh() const { return m_mesh; }
                    dtNavMeshQuery const* Query() const { return m_query; }

                private:
                    std::shared_lock<std::shared_mutex> m_hold;
                    dtNavMesh const* m_mesh;
                    dtNavMeshQuery const* m_query;
            };

            /**
             * @brief Take the read lock and resolve this instance's mesh and query.
             *
             * The one entry point a pathfinder should use. Creates the instance's query
             * on first ask (briefly exclusive), then returns holding the lock shared.
             */
            Route OpenRoute(uint32 mapId, uint32 instanceId);

            /**
             * @brief Take the read lock and resolve the map's mesh alone.
             *
             * For readers that walk the tile array without querying -- the console
             * commands that report what is loaded. Creates nothing, so a report cannot
             * allocate an instance's query as a side effect.
             */
            Route OpenMesh(uint32 mapId);

            bool loadMap(uint32 mapId, int32 x, int32 y);
            bool unloadMap(uint32 mapId, int32 x, int32 y);
            bool unloadMap(uint32 mapId);
            bool unloadMapInstance(uint32 mapId, uint32 instanceId);

            // One-shot lookups for callers that only read and report -- the console
            // commands. They drop the lock on return, so the pointer is only good for as
            // long as nothing loads a grid. Anything that runs a query wants OpenRoute().
            dtNavMeshQuery const* GetNavMeshQuery(uint32 mapId, uint32 instanceId);
            dtNavMesh const* GetNavMesh(uint32 mapId);

            uint32 getLoadedTilesCount() const;
            uint32 getLoadedMapsCount() const;
        private:
            // The three below assume the caller already holds m_lock -- exclusive for
            // loadMapData, either mode for the finders.
            bool loadMapData(uint32 mapId);
            MMapData* findMap(uint32 mapId) const;
            dtNavMeshQuery const* findQuery(MMapData* mmap, uint32 instanceId) const;

            static uint32 packTileID(int32 x, int32 y);

            mutable std::shared_mutex m_lock;
            MMapDataSet loadedMMaps;
            uint32 loadedTiles;
    };

    // static class
    // holds all mmap global data
    // access point to MMapManager singelton
    class MMapFactory
    {
        public:
            static MMapManager* createOrGetMMapManager();
            static void clear();
            static void preventPathfindingOnMaps(const char* ignoreMapIds);
            static bool IsPathfindingEnabled(uint32 mapId, const Unit* unit);
            static bool IsPathfindingForceEnabled(const Unit* unit);
            static bool IsPathfindingForceDisabled(const Unit* unit);
    };
}

#endif  // _MOVE_MAP_H
