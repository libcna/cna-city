// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

#include "Microsoft/Xna/Framework/Vector2.hpp"
#include "Microsoft/Xna/Framework/Vector3.hpp"

namespace CnaCity
{
    /**
     * @brief The city's ground-plane vector: `X` is world X, `Y` is world **Z**.
     *
     * The city is flat -- deliberately, because a hundred thousand agents on a heightfield is a
     * different demo -- so every network, block, lane and path is 2D and only becomes 3D at the
     * rendering boundary. Reusing `Microsoft::Xna::Framework::Vector2` rather than declaring a
     * private struct keeps CNA's own math module on the hot path, which is part of what this
     * program is measuring.
     */
    using Vec2 = Microsoft::Xna::Framework::Vector2;

    /** @brief Lifts a ground-plane point to world space at height @p y. */
    [[nodiscard]] inline Microsoft::Xna::Framework::Vector3 ToWorld(Vec2 p, float y = 0.0f)
    {
        return Microsoft::Xna::Framework::Vector3(p.X, y, p.Y);
    }

    /** @brief Drops a world-space point onto the ground plane. */
    [[nodiscard]] inline Vec2 ToGround(const Microsoft::Xna::Framework::Vector3& p)
    {
        return Vec2(p.X, p.Z);
    }

    // `Vector2` brings its own +, -, * and unary - as hidden friends, so this header adds only
    // what XNA's 2D vector never had: the scalar cross product, a perpendicular, and headings.

    [[nodiscard]] inline float Dot(Vec2 a, Vec2 b) { return a.X * b.X + a.Y * b.Y; }
    /** @brief The 2D cross product's scalar magnitude -- positive when @p b is left of @p a. */
    [[nodiscard]] inline float Cross(Vec2 a, Vec2 b) { return a.X * b.Y - a.Y * b.X; }
    [[nodiscard]] inline float LengthSq(Vec2 a) { return a.X * a.X + a.Y * a.Y; }
    [[nodiscard]] inline float Length(Vec2 a) { return std::sqrt(LengthSq(a)); }
    [[nodiscard]] inline float DistanceSq(Vec2 a, Vec2 b) { return LengthSq(b - a); }
    [[nodiscard]] inline float Distance(Vec2 a, Vec2 b) { return Length(b - a); }

    [[nodiscard]] inline Vec2 Normalized(Vec2 a)
    {
        const float len = Length(a);
        return len > 1e-6f ? a * (1.0f / len) : Vec2(0.0f, 0.0f);
    }

    /** @brief @p a rotated a quarter turn to the left. */
    [[nodiscard]] inline Vec2 Perp(Vec2 a) { return Vec2(-a.Y, a.X); }

    [[nodiscard]] inline Vec2 Rotate(Vec2 a, float radians)
    {
        const float c = std::cos(radians), s = std::sin(radians);
        return Vec2(a.X * c - a.Y * s, a.X * s + a.Y * c);
    }

    /** @brief The heading of @p a as a world yaw, in radians, measured from +X toward +Z. */
    [[nodiscard]] inline float Heading(Vec2 a) { return std::atan2(a.Y, a.X); }

    [[nodiscard]] inline Vec2 FromHeading(float radians)
    {
        return Vec2(std::cos(radians), std::sin(radians));
    }

    [[nodiscard]] inline Vec2 Lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

    template <typename T>
    [[nodiscard]] constexpr T Clamp(T value, T low, T high)
    {
        return value < low ? low : (value > high ? high : value);
    }

    [[nodiscard]] inline float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }

    [[nodiscard]] inline float SmoothStep(float edge0, float edge1, float x)
    {
        const float t = Saturate((x - edge0) / (edge1 - edge0 + 1e-9f));
        return t * t * (3.0f - 2.0f * t);
    }

    /** @brief The shortest signed difference between two angles, in (-pi, pi]. */
    [[nodiscard]] inline float AngleDelta(float from, float to)
    {
        constexpr float kTwoPi = 6.28318530718f;
        float d = std::fmod(to - from + 3.14159265359f, kTwoPi);
        if (d < 0.0f) d += kTwoPi;
        return d - 3.14159265359f;
    }

    /** @brief Moves @p from toward @p to by at most @p maxStep radians, the short way round. */
    [[nodiscard]] inline float ApproachAngle(float from, float to, float maxStep)
    {
        const float delta = AngleDelta(from, to);
        return from + Clamp(delta, -maxStep, maxStep);
    }

    /** @brief The point on segment [a,b] closest to @p p, and its parameter along the segment. */
    [[nodiscard]] inline Vec2 ClosestPointOnSegment(Vec2 a, Vec2 b, Vec2 p, float* outT = nullptr)
    {
        const Vec2 ab = b - a;
        const float denom = LengthSq(ab);
        const float t = denom > 1e-9f ? Saturate(Dot(p - a, ab) / denom) : 0.0f;
        if (outT != nullptr) *outT = t;
        return a + ab * t;
    }

    /**
     * @brief Intersects two segments, returning false when they are parallel or do not overlap.
     *
     * The road network is built by dropping polylines onto a plane and letting them cut each
     * other, so this is the primitive the whole layout rests on. Endpoints count as hits (`>= 0`,
     * `<= 1`): a street that ends on an avenue must produce a junction, not a dangling stub.
     */
    [[nodiscard]] inline bool IntersectSegments(Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2,
                                                float* outT, float* outU)
    {
        const Vec2 r = p2 - p1;
        const Vec2 s = q2 - q1;
        const float denom = Cross(r, s);
        if (std::fabs(denom) < 1e-9f) return false;
        const float t = Cross(q1 - p1, s) / denom;
        const float u = Cross(q1 - p1, r) / denom;
        if (t < -1e-6f || t > 1.0f + 1e-6f || u < -1e-6f || u > 1.0f + 1e-6f) return false;
        if (outT != nullptr) *outT = Clamp(t, 0.0f, 1.0f);
        if (outU != nullptr) *outU = Clamp(u, 0.0f, 1.0f);
        return true;
    }

    /** @brief Twice the signed area of a polygon; positive when the winding is counter-clockwise. */
    [[nodiscard]] inline float SignedArea2(const std::vector<Vec2>& polygon)
    {
        float sum = 0.0f;
        const std::size_t n = polygon.size();
        for (std::size_t i = 0; i < n; ++i)
            sum += Cross(polygon[i], polygon[(i + 1) % n]);
        return sum;
    }

    [[nodiscard]] inline float PolygonArea(const std::vector<Vec2>& polygon)
    {
        return std::fabs(SignedArea2(polygon)) * 0.5f;
    }

    [[nodiscard]] inline Vec2 PolygonCentroid(const std::vector<Vec2>& polygon)
    {
        const float area2 = SignedArea2(polygon);
        if (std::fabs(area2) < 1e-6f)
        {
            Vec2 mean(0.0f, 0.0f);
            for (Vec2 p : polygon) mean = mean + p;
            return polygon.empty() ? mean : mean * (1.0f / static_cast<float>(polygon.size()));
        }
        Vec2 acc(0.0f, 0.0f);
        const std::size_t n = polygon.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec2 a = polygon[i];
            const Vec2 b = polygon[(i + 1) % n];
            acc = acc + (a + b) * Cross(a, b);
        }
        return acc * (1.0f / (3.0f * area2));
    }

    [[nodiscard]] bool PointInPolygon(const std::vector<Vec2>& polygon, Vec2 p);

    /**
     * @brief Merges consecutive edges that turn by less than @p angleEpsilon into one.
     *
     * A block's outline comes out of the planar walk with a vertex at every junction on its
     * boundary -- including the junctions on the *far* side of the street, which do not bend the
     * block at all. Left alone, one 90 m street frontage arrives as five 18 m edges, and a
     * building placer that works edge by edge then puts up five stubby buildings with a corner
     * rejection between each pair instead of one proper terrace.
     */
    [[nodiscard]] std::vector<Vec2> SimplifyPolygon(const std::vector<Vec2>& polygon,
                                                    float angleEpsilon = 0.045f);

    /**
     * @brief Offsets a simple polygon inward by @p distance, or returns empty if it collapses.
     *
     * Each edge is moved along its inward normal and consecutive offset lines are intersected --
     * the cheap inset, not a straight skeleton. It is exactly right for a convex block and
     * acceptable for the mildly concave ones a road network produces; a result that self-
     * intersects or loses its orientation is rejected rather than drawn, which is why the return
     * is a vector that can come back empty.
     */
    [[nodiscard]] std::vector<Vec2> InsetPolygon(const std::vector<Vec2>& polygon, float distance);

    /** @brief An axis-aligned bound over a point set, as (min, max). */
    struct Bounds2
    {
        Vec2 min{1e30f, 1e30f};
        Vec2 max{-1e30f, -1e30f};

        void Add(Vec2 p)
        {
            if (p.X < min.X) min.X = p.X;
            if (p.Y < min.Y) min.Y = p.Y;
            if (p.X > max.X) max.X = p.X;
            if (p.Y > max.Y) max.Y = p.Y;
        }

        [[nodiscard]] bool IsEmpty() const { return min.X > max.X; }
        [[nodiscard]] Vec2 Center() const { return (min + max) * 0.5f; }
        [[nodiscard]] Vec2 Extent() const { return (max - min) * 0.5f; }
    };
}
