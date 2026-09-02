// SPDX-License-Identifier: MIT
#include "CityMath.hpp"

#include <algorithm>

namespace CnaCity
{
    bool PointInPolygon(const std::vector<Vec2>& polygon, Vec2 p)
    {
        bool inside = false;
        const std::size_t n = polygon.size();
        for (std::size_t i = 0, j = n - 1; i < n; j = i++)
        {
            const Vec2 a = polygon[i];
            const Vec2 b = polygon[j];
            if ((a.Y > p.Y) != (b.Y > p.Y) &&
                p.X < (b.X - a.X) * (p.Y - a.Y) / (b.Y - a.Y + 1e-12f) + a.X)
                inside = !inside;
        }
        return inside;
    }

    std::vector<Vec2> SimplifyPolygon(const std::vector<Vec2>& polygon, float angleEpsilon)
    {
        const std::size_t n = polygon.size();
        if (n < 4) return polygon;
        std::vector<Vec2> result;
        result.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec2 prev = polygon[(i + n - 1) % n];
            const Vec2 curr = polygon[i];
            const Vec2 next = polygon[(i + 1) % n];
            const Vec2 in = Normalized(curr - prev);
            const Vec2 out = Normalized(next - curr);
            if (LengthSq(in) < 0.5f || LengthSq(out) < 0.5f) continue;   // duplicate vertex
            // |sin(turn)| is the cross product of two unit vectors, which is the cheap test and
            // the numerically stable one near zero -- exactly where the decision is made.
            if (std::fabs(Cross(in, out)) < angleEpsilon && Dot(in, out) > 0.0f) continue;
            result.push_back(curr);
        }
        return result.size() >= 3 ? result : polygon;
    }

    std::vector<Vec2> InsetPolygon(const std::vector<Vec2>& polygon, float distance)
    {
        const std::size_t n = polygon.size();
        if (n < 3 || distance <= 0.0f) return polygon;

        // The winding decides which side "inward" is on, and a block face arrives from the
        // planar-graph walk counter-clockwise. Normalising here rather than trusting the caller
        // is what keeps a clockwise face from being inflated into its neighbours.
        std::vector<Vec2> ring = polygon;
        if (SignedArea2(ring) < 0.0f)
            std::reverse(ring.begin(), ring.end());

        struct Line { Vec2 point; Vec2 dir; };
        std::vector<Line> lines;
        lines.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec2 a = ring[i];
            const Vec2 b = ring[(i + 1) % n];
            const Vec2 dir = Normalized(b - a);
            if (LengthSq(dir) < 0.5f) return {};       // a degenerate edge means a degenerate block
            const Vec2 inward = Perp(dir);             // counter-clockwise winding => left is inside
            lines.push_back(Line{a + inward * distance, dir});
        }

        std::vector<Vec2> result;
        result.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const Line& prev = lines[(i + n - 1) % n];
            const Line& curr = lines[i];
            const float denom = Cross(prev.dir, curr.dir);
            if (std::fabs(denom) < 1e-4f)
            {
                // Nearly collinear neighbours: the intersection races off to infinity, so take the
                // offset endpoint itself, which is the limit of the corner as the angle closes.
                result.push_back(curr.point);
                continue;
            }
            const float t = Cross(curr.point - prev.point, curr.dir) / denom;
            result.push_back(prev.point + prev.dir * t);
        }

        // The inset is only valid while it stays a simple polygon with the original orientation
        // and some area left. Every rejection here is a block too thin to build on.
        if (SignedArea2(result) <= 0.0f) return {};
        if (PolygonArea(result) < 1.0f) return {};
        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec2 edge = result[(i + 1) % n] - result[i];
            if (Dot(edge, lines[i].dir) <= 0.0f) return {};   // that edge folded back on itself
        }
        return result;
    }
}
