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

#include "Utilities/MathDefines.h"
#include "MotionFrame.h"
#include "Map.h"
#include "MapManager.h"
#include "PathFinder.h"
#include "Player.h"
#include "Transports.h"
#include "TransportMap.h"
#include "Unit.h"
#include "Utilities/Util.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace Motion
{
    namespace
    {
        /// The default ceiling on a routed path, in yards. Re-applied on every query
        /// because the limit is sticky on a reused PathFinder, so an unlimited request
        /// after a capped one (a flee leg) would otherwise inherit the cap.
        constexpr float DEFAULT_PATH_LENGTH =
            float(MAX_POINT_PATH_LENGTH) * SMOOTH_PATH_STEP_SIZE;

        /// The length of a routed path, summed over its segments: what a caller's cap names.
        float RouteLength(PointsArray const& points)
        {
            float total = 0.0f;
            for (size_t i = 1; i < points.size(); ++i)
            {
                const Vector3 step = points[i] - points[i - 1];
                total += sqrtf(step.x * step.x + step.y * step.y + step.z * step.z);
            }
            return total;
        }

        /// A unit's router on a map, behind IPathQuery: the world map the mover is filed under,
        /// or a vessel's deck (its own map, while the world still holds the mover's guid).
        class UnitPathQuery final : public IPathQuery
        {
            public:
                explicit UnitPathQuery(Unit const& mover) : m_path(&mover) {}
                /// No caller today: TransportFrame inherits WorldFrame::CreatePathQuery. The deck's
                /// documented entry for the vehicle phase (P4-C).
                UnitPathQuery(Unit const& mover, uint32 deckMapId) : m_path(&mover, deckMapId) {}

                bool Calculate(Vector3 const& start, Vector3 const& goal,
                               bool forceDestination, float lengthLimit) override
                {
                    // The smoother always gets its own budget. PathFinder::setPathLengthLimit
                    // reads yards as a COUNT OF PATH POINTS (yards over the 4-yard smoothing
                    // step) and findSmoothPath FAILS when it runs out of them before the goal,
                    // so a caller's 30-yard cap allowed seven points and refused every route
                    // that had to bend: a feared unit stood still in any city, its animation
                    // running, while the same leg smoothed fine over open ground (the live
                    // test of 2026-09-20). The cap is applied below, to the route's length.
                    m_path.setPathLengthLimit(DEFAULT_PATH_LENGTH);

                    if (!m_path.calculate(start.x, start.y, start.z,
                                          goal.x, goal.y, goal.z, forceDestination))
                    {
                        return false;
                    }

                    // A failed route still leaves a straight-line shortcut in the points,
                    // which some movement kinds want and others refuse -- so report it
                    // through Failed() rather than deciding here.
                    if (m_path.getPath().size() < 2)
                    {
                        return false;
                    }

                    // The caller's cap, applied to what it names: the route's length. A bolt
                    // that would walk half a zone to reach its point is not a bolt, and the
                    // behaviour hears the refusal and draws another point next tick.
                    return lengthLimit <= 0.0f || RouteLength(m_path.getPath()) <= lengthLimit;
                }

                PointsArray const& Points() const override { return m_path.getPath(); }

                bool Failed() const override
                {
                    return (m_path.getPathType() & PATHFIND_NOPATH) != 0;
                }

                bool Routed() const override
                {
                    return (m_path.getPathType() &
                            (PATHFIND_NOPATH | PATHFIND_NOT_USING_PATH)) == 0;
                }

                bool Partial() const override
                {
                    return (m_path.getPathType() & PATHFIND_INCOMPLETE) != 0;
                }

                bool Progresses() const override
                {
                    // Under a yard of travel is a route that ends where it starts.
                    const float advance =
                        (m_path.getActualEndPosition() - m_path.getStartPosition()).magnitude();
                    return advance >= 1.0f;
                }

                bool Reachable() const override
                {
                    return (m_path.getPathType() & PATHFIND_NORMAL) != 0;
                }

            private:
                /// getPath() is non-const on PathFinder, though reading the routed points
                /// does not mutate the query as far as callers are concerned.
                mutable PathFinder m_path;
        };

        /**
         * @brief The world's own coordinate system: navmesh routing, terrain heights.
         *        What the movement code always did, now behind the frame interface so a
         *        transport frame can replace it wholesale.
         */
        class WorldFrame : public IMotionFrame
        {
            public:
                FrameKind Kind() const override { return FrameKind::World; }

                std::unique_ptr<IPathQuery> CreatePathQuery(Unit const& mover) const override
                {
                    return std::make_unique<UnitPathQuery>(mover);
                }

                Vector3 MoverPosition(Unit const& mover) const override
                {
                    Vector3 p;
                    p.x = mover.Where().X(), p.y = mover.Where().Y(), p.z = mover.Where().Z();
                    return p;
                }

                /// The world frame IS world space, so both conversions are the identity.
                /// This is what lets every behaviour convert its anchors unconditionally
                /// and cost nothing for the units that are not on a vessel.
                Vector3 FromWorld(Unit const& /*mover*/, Vector3 const& world) const override
                {
                    return world;
                }

                Vector3 ObjectPosition(Unit const& /*mover*/, WorldObject const& obj) const override
                {
                    Vector3 p;
                    p.x = obj.Where().X(), p.y = obj.Where().Y(), p.z = obj.Where().Z();
                    return p;
                }

                float ObjectOrientation(Unit const& /*mover*/, WorldObject const& obj) const override
                {
                    return obj.Where().Facing();
                }

                /// The world frame IS world space, so a world bearing is already a frame one.
                float FacingToFrame(Unit const& /*mover*/, float worldFacing) const override
                {
                    return worldFacing;
                }

                Vector3 NearPoint(Unit const& mover, WorldObject const& target,
                                  float searcherBounding, float distance2d,
                                  float absAngle) const override
                {
                    Vector3 p;
                    FindFreeSpotNear(target, &mover, p.x, p.y, p.z, searcherBounding, distance2d, absAngle);
                    return p;
                }

                Vector3 NearPointAt(Unit const& mover, WorldObject const& target,
                                    Vector3 const& center, float searcherBounding,
                                    float distance2d, float absAngle) const override
                {
                    Vector3 p;
                    FindFreeSpotNear(target, center, &mover, p.x, p.y, p.z, searcherBounding, distance2d, absAngle);
                    return p;
                }

                std::optional<Vector3> RandomPoint(Unit& mover, Vector3 const& centre,
                                                   float radius) const override
                {
                    Vector3 p = centre;
                    if (!mover.GetMap()->GetReachableRandomPosition(&mover, p.x, p.y, p.z, radius))
                    {
                        return std::nullopt;
                    }

                    return p;
                }

                std::optional<Vector3> GroundPoint(Unit& mover, Vector3 const& from,
                                                   Vector3 const& guess) const override
                {
                    Map* map = mover.GetMap();
                    const uint32 phase = mover.GetPhaseMask();

                    Vector3 p = guess;
                    if (auto floor = map->FloorNear(phase, p.x, p.y, p.z))
                    {
                        p.z = *floor;
                    }
                    else
                    {
                        return std::nullopt;
                    }

                    // A player is pulled back to the first obstruction on the way, so a
                    // feared player cannot be shoved through a wall. The half-yard lift
                    // avoids false hits against the ground itself and small clutter.
                    if (mover.GetTypeId() == TYPEID_PLAYER)
                    {
                        float testZ = p.z + 0.5f;
                        if (map->GetHitPosition(from.x, from.y, from.z + 0.5f,
                                                p.x, p.y, testZ, phase, -0.1f))
                        {
                            p.z = testZ;
                            if (auto floor = map->FloorNear(phase, p.x, p.y, p.z))
                            {
                                p.z = *floor;
                            }
                            else
                            {
                                return std::nullopt;
                            }
                        }
                    }

                    return p;
                }
        };

        /* ***************************** The transport frame **************************
         *
         * A boarded unit is not standing on the map it is advertised over. It is standing
         * on the vessel's OWN map, and deck-local coordinates are that map's coordinates,
         * so everything below speaks them in and out with no transform anywhere.
         *
         * That is not an optimisation, it is the only correct thing to do. The server does
         * not know where the ship is: the client interpolates it along a Catmull-Rom curve
         * the server never reproduces. Posing the hull into world coordinates and firing
         * rays at it computes collisions against a ship that is not there.
         */

        /// Each sample is sought from this far above the last one -- so a low step or a
        /// ramp is found underfoot -- and no further than this below it, so a hatch does
        /// not silently drop the leg onto the deck two levels down.
        constexpr float DECK_SEARCH_UP = 2.0f;
        constexpr float DECK_SEARCH_DOWN = 6.0f;

        /// Chest height for the obstruction probe, so the deck a unit is standing ON is
        /// not itself read as the thing blocking it.
        constexpr float DECK_PROBE_HEIGHT = 1.0f;

        /// Tries before a random deck point is given up on. A deck is small and mostly
        /// clutter; rejecting a bad point is cheaper than being clever about picking one.
        constexpr int DECK_RANDOM_TRIES = 8;

        /// Drop a deck-local point onto the deck. Local in, local out.
        std::optional<Vector3> DeckDrop(TransportMap const& hull, Vector3 const& local)
        {
            const auto z = hull.SurfaceAt(local.x, local.y, local.z,
                                          DECK_SEARCH_UP, DECK_SEARCH_DOWN);
            if (!z)
            {
                return std::nullopt;
            }

            return Vector3(local.x, local.y, *z);
        }

        /// Does the vessel's own geometry stand between these two deck points?
        bool DeckBlocked(TransportMap const& hull, Vector3 const& from, Vector3 const& to)
        {
            return hull.IsBlocked(Vector3(from.x, from.y, from.z + DECK_PROBE_HEIGHT),
                                  Vector3(to.x, to.y, to.z + DECK_PROBE_HEIGHT));
        }

        /**
         * @brief A deck is a map WITH EDGES. That is the whole of the difference.
         *
         * Every question about where something IS was answered here by asking the vessel --
         * and once boarding stopped being recorded, those answers became (0,0,0) and every
         * deckhand set off from the centre of the ship. They were the wrong questions to
         * begin with: a unit on a deck is on a MAP, its position is that map's position,
         * and WorldFrame already reads it straight from the placement, routes on that map's
         * navmesh and drops onto that map's terrain -- which is the hull.
         *
         * What is left below is what a continent does not have: a rail. Off the edge or
         * through a bulkhead is REFUSED rather than clamped, because a unit inches from a
         * gunwale must be told no, not quietly pulled back.
         */
        class TransportFrame final : public WorldFrame
        {
            public:
                FrameKind Kind() const override { return FrameKind::Transport; }

                Vector3 NearPoint(Unit const& mover, WorldObject const& target,
                                  float searcherBounding, float distance2d,
                                  float absAngle) const override
                {
                    // The centred form, anchored on the target's own placement: the two
                    // bodies were identical but for where the offset was measured from.
                    return NearPointAt(mover, target, ObjectPosition(mover, target),
                                       searcherBounding, distance2d, absAngle);
                }

                Vector3 NearPointAt(Unit const& mover, WorldObject const& /*target*/,
                                    Vector3 const& center, float /*searcherBounding*/,
                                    float distance2d, float absAngle) const override
                {
                    // absAngle is a FRAME angle -- the callers derive it from
                    // ObjectOrientation or from frame positions -- so the offset is applied
                    // in the deck's own 2D system and no yaw correction belongs here. The
                    // centre is deck-local, like every other coordinate down here.
                    const Vector3 guess(center.x + distance2d * std::cos(absAngle),
                                        center.y + distance2d * std::sin(absAngle),
                                        center.z);

                    TransportMap* hull = mover.GetMap()->AsTransport();
                    if (!hull)
                    {
                        return guess;
                    }

                    if (const auto onDeck = DeckDrop(*hull, guess))
                    {
                        return *onDeck;
                    }

                    // Off the edge. Handed back unresolved on purpose: the router will
                    // refuse the leg and the behaviour hears `blocked` and picks somewhere
                    // else. Quietly pulling it back onto the deck here would leave a chase
                    // standing still, convinced it had arrived.
                    return guess;
                }

                std::optional<Vector3> RandomPoint(Unit& mover, Vector3 const& centre,
                                                   float radius) const override
                {
                    TransportMap* hull = mover.GetMap()->AsTransport();
                    if (!hull)
                    {
                        return std::nullopt;
                    }

                    for (int attempt = 0; attempt < DECK_RANDOM_TRIES; ++attempt)
                    {
                        const float angle = frand(0.0f, 2 * M_PI_F);
                        const float dist = radius * std::sqrt(frand(0.0f, 1.0f));

                        const Vector3 guess(centre.x + dist * std::cos(angle),
                                            centre.y + dist * std::sin(angle),
                                            centre.z);

                        const auto onDeck = DeckDrop(*hull, guess);
                        if (onDeck && !DeckBlocked(*hull, centre, *onDeck))
                        {
                            return onDeck;
                        }
                    }

                    return std::nullopt;
                }

                std::optional<Vector3> GroundPoint(Unit& mover, Vector3 const& from,
                                                   Vector3 const& guess) const override
                {
                    TransportMap* hull = mover.GetMap()->AsTransport();
                    if (!hull)
                    {
                        return std::nullopt;
                    }

                    const auto onDeck = DeckDrop(*hull, guess);
                    if (!onDeck)
                    {
                        return std::nullopt;   // no deck there: a panicking unit may not
                                               // bolt over the rail
                    }

                    // Unlike the world frame, the obstruction test applies to EVERYONE and
                    // rejects rather than pulls back. A deck is a warren of bulkheads and
                    // the units on it are inches from them.
                    if (DeckBlocked(*hull, from, *onDeck))
                    {
                        return std::nullopt;
                    }

                    return onDeck;
                }
        };

        /// Both are stateless, so one instance of each serves every mover on every vessel.
        const WorldFrame s_worldFrame;
        const TransportFrame s_transportFrame;
    }

    IMotionFrame const& FrameFor(Unit const& mover)
    {
        // The one place in the server where "which world am I in?" is decided. A boarded
        // unit moves in its vessel's frame; everything else in the map's. Nothing above
        // this call knows the difference, and nothing above it needs to.
        if (Map const* on = mover.FindMap())
        {
            if (on->AsTransport())
            {
                return s_transportFrame;
            }
        }

        return s_worldFrame;
    }
}
