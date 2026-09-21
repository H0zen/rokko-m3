#pragma once

// WHERE SOMETHING IS, as a value: a Location -- a frame and a pose in it -- plus the room
// it takes up. The server's whole spatial vocabulary, over Vector3 and GeometryMath.
//
// The three types are one chain, and each rung adds exactly one thing:
//   Position   a pose, measured against nothing. A sample; no comparisons.
//   Location   a pose AND its frame. Enough to say where; storable.
//   Placement  a Location AND an extent. Enough to say whether two things touch.
//
// An object HAS one; it is not a bag of coordinates with geometry bolted on. An Item has
// none, which is why it can be carried but never seen, ranged or faced.
//
// TERRAIN-AGNOSTIC BY CONSTRUCTION: no map, tile, navmesh or collision is reachable, so
// ground height and line of sight belong to the terrain engine. What is left holds in ANY
// frame, so a deck placement needs no special case.
//
// CROSS-FRAME FAILS CLOSED -- infinitely far, never in reach, never in arc. Forgetting the
// map check is the oldest bug in this tree; here it cannot be written.

#include "Geometry/Frame.h"
#include "Geometry/GeometryMath.h"
#include "Geometry/Location.h"
#include "Geometry/Position.h"
#include "Geometry/Shapes.h"
#include "Geometry/Vector2.h"
#include "Geometry/Vector3.h"

#include <cmath>
#include <limits>

namespace Geometry
{
    class Placement
    {
        public:
            Placement() : m_extent(0.0f) {}
            explicit Placement(float extent) : m_extent(extent) {}

            static float Pi() { return pif(); }
            static float TwoPi() { return 2.0f * pif(); }
            static float Unreachable() { return std::numeric_limits<float>::infinity(); }

            static float Gap(float separation, float reach)
            {
                const float gap = separation - reach;
                return gap > 0.0f ? gap : 0.0f;
            }

            static float NormalizeOrientation(float o) { return wrap(o, 0.0f, TwoPi()); }

            static float SignedOrientation(float o)
            {
                const float a = NormalizeOrientation(o);
                return a > pif() ? a - TwoPi() : a;
            }

            const Frame& CurrentFrame() const { return m_where.frame; }
            bool IsPlaced() const { return m_where.frame.IsPlaced(); }

            /// The frame and the pose without the extent -- what a destination that has to be
            /// stored and read back later is. The extent belongs to the object, not the place.
            const Location& AsLocation() const { return m_where; }

            const Vector3& Pos() const { return m_where.at.pos; }
            float X() const { return m_where.at.pos.x; }
            float Y() const { return m_where.at.pos.y; }
            float Z() const { return m_where.at.pos.z; }
            float Facing() const { return m_where.at.facing; }
            float Extent() const { return m_extent; }

            bool IsFinite() const { return m_where.at.pos.isFinite() && Geometry::isFinite(m_where.at.facing); }

            Transform Basis(float scale = 1.0f) const
            {
                return Transform(m_where.at.pos, Mat3::fromEuler(0.0f, 0.0f, m_where.at.facing), scale);
            }

            void EnterFrame(const Frame& frame, const Vector3& pos, float facing)
            {
                m_where.frame = frame;
                m_where.at.pos = pos;
                Face(facing);
            }

            /// Take a stored location whole: its frame and its pose in one step.
            void EnterFrame(const Location& where)
            {
                EnterFrame(where.frame, where.at.pos, where.at.facing);
            }

            void Rebase(const Frame& frame) { m_where.frame = frame; }

            void LeaveFrame() { m_where.frame = Frame(); }

            /// A POSITION THAT IS NOT A NUMBER IS NOT A POSITION, so it is not taken. The
            /// object keeps where it last was instead of becoming NaN.
            ///
            /// This is the only door every writer goes through -- Place() hands out a
            /// mutable reference and the callers mutate it directly -- so it is the only
            /// place a single test can cover all of them. And it has to be covered: a NaN
            /// here reaches the update block and the wire, and the 4.3.4 client turns a
            /// position into an integer to take a bearing from it. (int)NaN is 0x80000000,
            /// which its fixed-point atan2 (sub_AC3C00) then divides by zero, taking the
            /// whole client down with ERROR #132.
            ///
            /// Silent because this is a value type with nothing to log to. A caller that
            /// wants to know can ask IsFinite(); the boundaries that matter -- the spline
            /// packet in particular -- check and report on their own.
            void MoveTo(const Vector3& pos)
            {
                if (pos.isFinite())
                {
                    m_where.at.pos = pos;
                }
            }
            void MoveTo(float x, float y, float z) { MoveTo(Vector3(x, y, z)); }
            void MoveTo(const Vector3& pos, float facing) { MoveTo(pos); Face(facing); }
            void MoveTo(float x, float y, float z, float facing) { MoveTo(Vector3(x, y, z), facing); }

            void Face(float facing)
            {
                if (Geometry::isFinite(facing))
                {
                    m_where.at.facing = NormalizeOrientation(facing);
                }
            }
            void FaceToward(const Vector3& target) { Face(BearingTo(target)); }
            void Resize(float extent) { m_extent = extent; }

            bool ShareFrame(const Placement& other) const
            {
                return m_where.frame.IsPlaced() && m_where.frame == other.m_where.frame;
            }

            float DistanceTo(const Placement& other, bool is3D = true) const
            {
                return ShareFrame(other)
                           ? Gap(std::sqrt(SeparationSq(other.m_where.at.pos, is3D)), m_extent + other.m_extent)
                           : Unreachable();
            }

            float DistanceTo(const Vector3& point, bool is3D = true) const
            {
                return Gap(std::sqrt(SeparationSq(point, is3D)), m_extent);
            }

            float DistanceTo(const Vector2& point) const
            {
                return Gap(std::sqrt(SeparationSq2D(point)), m_extent);
            }

            float HeightGapTo(const Placement& other) const
            {
                return ShareFrame(other)
                           ? Gap(std::fabs(m_where.at.pos.z - other.m_where.at.pos.z), m_extent + other.m_extent)
                           : Unreachable();
            }

            bool WithinDist(const Placement& other, float dist, bool is3D = true) const
            {
                return ShareFrame(other) &&
                       Closer(SeparationSq(other.m_where.at.pos, is3D), dist, m_extent + other.m_extent);
            }

            bool WithinDist(const Vector3& point, float dist, bool is3D = true) const
            {
                return Closer(SeparationSq(point, is3D), dist, m_extent);
            }

            bool WithinDist(const Vector2& point, float dist) const
            {
                return Closer(SeparationSq2D(point), dist, m_extent);
            }

            bool WithinRange(const Placement& other, float minRange, float maxRange, bool is3D = true) const
            {
                return ShareFrame(other) &&
                       InBand(SeparationSq(other.m_where.at.pos, is3D), minRange, maxRange, m_extent + other.m_extent);
            }

            bool WithinRange(const Vector3& point, float minRange, float maxRange, bool is3D = true) const
            {
                return InBand(SeparationSq(point, is3D), minRange, maxRange, m_extent);
            }

            bool WithinRange(const Vector2& point, float minRange, float maxRange) const
            {
                return InBand(SeparationSq2D(point), minRange, maxRange, m_extent);
            }

            /// A zero tolerance skips that axis; all-zero matches nothing, as the database's
            /// integer waypoint tolerances have always meant.
            bool WithinBox(const Vector3& point, const Vector3& tolerance) const
            {
                if (tolerance.x <= 0.0f && tolerance.y <= 0.0f && tolerance.z <= 0.0f)
                {
                    return false;
                }
                if (tolerance.x > 0.0f && std::fabs(m_where.at.pos.x - point.x) >= tolerance.x)
                {
                    return false;
                }
                if (tolerance.y > 0.0f && std::fabs(m_where.at.pos.y - point.y) >= tolerance.y)
                {
                    return false;
                }
                if (tolerance.z > 0.0f && std::fabs(m_where.at.pos.z - point.z) >= tolerance.z)
                {
                    return false;
                }
                return true;
            }

            bool IsNearer(const Placement& a, const Placement& b, bool is3D = true) const
            {
                if (!ShareFrame(a))
                {
                    return false;
                }
                if (!ShareFrame(b))
                {
                    return true;
                }
                return SeparationSq(a.m_where.at.pos, is3D) < SeparationSq(b.m_where.at.pos, is3D);
            }

            float BearingTo(const Vector3& point) const
            {
                const float ang = std::atan2(point.y - m_where.at.pos.y, point.x - m_where.at.pos.x);
                return (ang >= 0.0f) ? ang : TwoPi() + ang;
            }

            float BearingTo(const Vector2& point) const
            {
                const float ang = std::atan2(point.y - m_where.at.pos.y, point.x - m_where.at.pos.x);
                return (ang >= 0.0f) ? ang : TwoPi() + ang;
            }

            float BearingTo(const Placement& other) const
            {
                return ShareFrame(other) ? BearingTo(other.m_where.at.pos) : 0.0f;
            }

            float RelativeBearingTo(const Placement& other) const
            {
                return SignedOrientation(BearingTo(other) - m_where.at.facing);
            }

            float RelativeBearingTo(const Vector3& point) const
            {
                return SignedOrientation(BearingTo(point) - m_where.at.facing);
            }

            bool HasInArc(const Placement& other, float arc) const
            {
                if (!ShareFrame(other))
                {
                    return false;
                }
                const float half = NormalizeOrientation(arc) / 2.0f;
                const float bearing = RelativeBearingTo(other);
                return bearing >= -half && bearing <= half;
            }

            bool IsInFront(const Placement& other, float dist, float arc) const
            {
                return WithinDist(other, dist) && HasInArc(other, arc);
            }

            bool IsInBack(const Placement& other, float dist, float arc) const
            {
                return WithinDist(other, dist) && !HasInArc(other, TwoPi() - arc);
            }

            Vector3 PointAt(float distance2d, float absAngle) const
            {
                return Vector3(m_where.at.pos.x + distance2d * std::cos(absAngle),
                               m_where.at.pos.y + distance2d * std::sin(absAngle),
                               m_where.at.pos.z);
            }

            Vector3 PointAhead(float distance2d) const { return PointAt(distance2d, m_where.at.facing); }

            static float ContactSpread(float gap, float extentA, float extentB)
            {
                return gap + extentA + extentB;
            }

            Vector3 ContactPointToward(const Placement& other, float gap) const
            {
                return PointAt(gap + m_extent + other.m_extent, BearingTo(other));
            }

            /// Rolls INJECTED (`distanceRoll` in [0, 1)), so the pick stays pinnable.
            Vector3 RandomPointAround(float minDist, float maxDist, float angle, float distanceRoll) const
            {
                return PointAt(minDist + distanceRoll * (maxDist - minDist), angle);
            }

        private:
            float SeparationSq(const Vector3& point, bool is3D) const
            {
                const float dx = m_where.at.pos.x - point.x;
                const float dy = m_where.at.pos.y - point.y;
                const float flat = dx * dx + dy * dy;
                if (!is3D)
                {
                    return flat;
                }
                const float dz = m_where.at.pos.z - point.z;
                return flat + dz * dz;
            }

            float SeparationSq2D(const Vector2& point) const
            {
                const float dx = m_where.at.pos.x - point.x;
                const float dy = m_where.at.pos.y - point.y;
                return dx * dx + dy * dy;
            }

            static bool Closer(float separationSq, float dist, float extentSum)
            {
                const float reach = dist + extentSum;
                return separationSq < reach * reach;
            }

            static bool InBand(float separationSq, float minRange, float maxRange, float extentSum)
            {
                if (minRange > 0.0f)
                {
                    const float near_ = minRange + extentSum;
                    if (separationSq < near_ * near_)
                    {
                        return false;
                    }
                }
                const float far_ = maxRange + extentSum;
                return separationSq < far_ * far_;
            }

            Location m_where;
            float m_extent;
    };
}
