// SPDX-License-Identifier: MIT
#include "RoadNetwork.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace CnaCity
{
    namespace
    {
        /// Metres and m/s. The numbers are ordinary European street standards rather than
        /// invented ones, because the whole demo's sense of scale rests on them: a 3.25 m lane is
        /// what makes a bus look like a bus next to a building that is four of them wide.
        constexpr RoadProfile kProfiles[kRoadClassCount] = {
            /* Highway   */ {11.0f, 3.60f, 0.0f, 27.8f, 3, true,  false},
            /* Arterial  */ { 7.5f, 3.40f, 3.5f, 13.9f, 2, true,  true},
            /* Collector */ { 5.2f, 3.20f, 2.8f, 11.1f, 1, true,  true},
            /* Local     */ { 3.4f, 3.20f, 2.2f,  8.3f, 1, true,  true},
            /* Alley     */ { 2.2f, 2.20f, 0.0f,  5.6f, 1, false, false},
        };

    }

    const RoadProfile& ProfileOf(RoadClass roadClass)
    {
        return kProfiles[static_cast<int>(roadClass)];
    }

    void RoadNetwork::AddSegment(Vec2 from, Vec2 to, RoadClass roadClass)
    {
        if (DistanceSq(from, to) < 0.25f) return;
        raw_.push_back(RawSegment{from, to, roadClass});
    }

    void RoadNetwork::AddPolyline(const std::vector<Vec2>& points, RoadClass roadClass)
    {
        for (std::size_t i = 1; i < points.size(); ++i)
            AddSegment(points[i - 1], points[i], roadClass);
    }

    void RoadNetwork::Build(float weldRadius)
    {
        nodes_.clear();
        segments_.clear();
        incident_.clear();
        bounds_ = Bounds2{};

        // ---- 1. Cut every raw segment against every other -------------------------------------
        //
        // A uniform grid over the raw segments keeps this near-linear. Without it a city with
        // twenty thousand raw segments is four hundred million pair tests, which is the difference
        // between a start-up that takes a moment and one that takes a minute.
        Bounds2 rawBounds;
        for (const RawSegment& s : raw_) { rawBounds.Add(s.a); rawBounds.Add(s.b); }
        if (rawBounds.IsEmpty()) return;

        const float broadCell = 120.0f;
        const int bw = std::max(1, static_cast<int>((rawBounds.max.X - rawBounds.min.X) / broadCell) + 1);
        const int bh = std::max(1, static_cast<int>((rawBounds.max.Y - rawBounds.min.Y) / broadCell) + 1);
        std::vector<std::vector<std::uint32_t>> buckets(static_cast<std::size_t>(bw) * bh);
        auto bucketRange = [&](const RawSegment& s, int& x0, int& y0, int& x1, int& y1) {
            const float minX = std::min(s.a.X, s.b.X), maxX = std::max(s.a.X, s.b.X);
            const float minY = std::min(s.a.Y, s.b.Y), maxY = std::max(s.a.Y, s.b.Y);
            x0 = Clamp(static_cast<int>((minX - rawBounds.min.X) / broadCell), 0, bw - 1);
            x1 = Clamp(static_cast<int>((maxX - rawBounds.min.X) / broadCell), 0, bw - 1);
            y0 = Clamp(static_cast<int>((minY - rawBounds.min.Y) / broadCell), 0, bh - 1);
            y1 = Clamp(static_cast<int>((maxY - rawBounds.min.Y) / broadCell), 0, bh - 1);
        };
        for (std::uint32_t i = 0; i < raw_.size(); ++i)
        {
            int x0, y0, x1, y1;
            bucketRange(raw_[i], x0, y0, x1, y1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    buckets[static_cast<std::size_t>(y) * bw + x].push_back(i);
        }

        std::vector<std::vector<float>> cuts(raw_.size());
        for (std::size_t i = 0; i < raw_.size(); ++i)
        {
            cuts[i].push_back(0.0f);
            cuts[i].push_back(1.0f);
        }
        for (const std::vector<std::uint32_t>& bucket : buckets)
        {
            for (std::size_t bi = 0; bi < bucket.size(); ++bi)
                for (std::size_t bj = bi + 1; bj < bucket.size(); ++bj)
                {
                    const std::uint32_t i = bucket[bi];
                    const std::uint32_t j = bucket[bj];
                    float t = 0.0f, u = 0.0f;
                    if (!IntersectSegments(raw_[i].a, raw_[i].b, raw_[j].a, raw_[j].b, &t, &u))
                        continue;
                    cuts[i].push_back(t);
                    cuts[j].push_back(u);
                }
        }

        // ---- 2. Weld the cut points into nodes -------------------------------------------------
        //
        // The key quantises a position to a weld grid; the four-neighbour probe is what stops two
        // points either side of a cell boundary becoming two junctions.
        const float invWeld = 1.0f / std::max(weldRadius, 1e-3f);
        std::unordered_map<std::uint64_t, std::uint32_t> weld;
        weld.reserve(raw_.size() * 4);
        auto keyOf = [&](int gx, int gy) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 32) |
                   static_cast<std::uint32_t>(gy);
        };
        auto nodeAt = [&](Vec2 p) -> std::uint32_t {
            const int gx = static_cast<int>(std::floor(p.X * invWeld));
            const int gy = static_cast<int>(std::floor(p.Y * invWeld));
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    auto it = weld.find(keyOf(gx + dx, gy + dy));
                    if (it != weld.end() &&
                        DistanceSq(nodes_[it->second].position, p) <= weldRadius * weldRadius)
                        return it->second;
                }
            const auto index = static_cast<std::uint32_t>(nodes_.size());
            RoadNode node;
            node.position = p;
            nodes_.push_back(node);
            weld.emplace(keyOf(gx, gy), index);
            bounds_.Add(p);
            return index;
        };

        // ---- 3. Emit the sub-segments ----------------------------------------------------------
        //
        // Duplicate suppression is not optional. The arterial grid and a district's own street
        // lines can lie on top of each other for a stretch, and a graph with two parallel edges
        // between the same pair of nodes breaks the face walk, which assumes each half-edge has
        // exactly one twin.
        std::unordered_map<std::uint64_t, std::uint32_t> seen;
        for (std::size_t i = 0; i < raw_.size(); ++i)
        {
            std::vector<float>& ts = cuts[i];
            std::sort(ts.begin(), ts.end());
            ts.erase(std::unique(ts.begin(), ts.end(),
                                 [](float a, float b) { return std::fabs(a - b) < 1e-5f; }),
                     ts.end());
            const RawSegment& s = raw_[i];
            for (std::size_t k = 1; k < ts.size(); ++k)
            {
                const Vec2 pa = Lerp(s.a, s.b, ts[k - 1]);
                const Vec2 pb = Lerp(s.a, s.b, ts[k]);
                if (DistanceSq(pa, pb) < 0.16f) continue;
                const std::uint32_t na = nodeAt(pa);
                const std::uint32_t nb = nodeAt(pb);
                if (na == nb) continue;

                const std::uint64_t key = (static_cast<std::uint64_t>(std::min(na, nb)) << 32) |
                                          std::max(na, nb);
                auto it = seen.find(key);
                if (it != seen.end())
                {
                    // Keep the more important of the two: an arterial that shares its centreline
                    // with a local street is an arterial.
                    RoadSegment& existing = segments_[it->second];
                    if (static_cast<int>(s.roadClass) < static_cast<int>(existing.roadClass))
                        existing.roadClass = s.roadClass;
                    continue;
                }

                RoadSegment segment;
                segment.nodeA = na;
                segment.nodeB = nb;
                segment.roadClass = s.roadClass;
                segment.direction = Normalized(nodes_[nb].position - nodes_[na].position);
                segment.length = Distance(nodes_[na].position, nodes_[nb].position);
                seen.emplace(key, static_cast<std::uint32_t>(segments_.size()));
                segments_.push_back(segment);
            }
        }

        // ---- 4. Prune dead ends -----------------------------------------------------------------
        //
        // Every dangling edge in this network is an artefact -- a street clipped a hair short of
        // the arterial it should meet, or a stub left where a diagonal grazes the boundary. None
        // of them is a cul-de-sac anybody meant, and each one is poison to the face walk below: a
        // face containing a slit is not a simple polygon, and the block extractor answers with one
        // enormous self-touching outline instead of the dozen real blocks around it.
        for (;;)
        {
            std::vector<std::uint32_t> degree(nodes_.size(), 0);
            for (const RoadSegment& s : segments_) { ++degree[s.nodeA]; ++degree[s.nodeB]; }
            std::vector<RoadSegment> kept;
            kept.reserve(segments_.size());
            for (const RoadSegment& s : segments_)
                if (degree[s.nodeA] >= 2 && degree[s.nodeB] >= 2)
                    kept.push_back(s);
            if (kept.size() == segments_.size()) break;
            segments_.swap(kept);
        }

        // Compact the nodes the pruning orphaned, so that a nearest-node query can never answer
        // with a node no road reaches.
        {
            std::vector<std::uint32_t> remap(nodes_.size(), 0xFFFFFFFFu);
            std::vector<RoadNode> kept;
            kept.reserve(nodes_.size());
            for (const RoadSegment& s : segments_)
                for (std::uint32_t n : {s.nodeA, s.nodeB})
                    if (remap[n] == 0xFFFFFFFFu)
                    {
                        remap[n] = static_cast<std::uint32_t>(kept.size());
                        kept.push_back(nodes_[n]);
                    }
            for (RoadSegment& s : segments_)
            {
                s.nodeA = remap[s.nodeA];
                s.nodeB = remap[s.nodeB];
            }
            nodes_.swap(kept);
            bounds_ = Bounds2{};
            for (const RoadNode& n : nodes_) bounds_.Add(n.position);
        }

        // ---- 5. Adjacency, sorted by outgoing heading ------------------------------------------
        std::vector<std::uint32_t> counts(nodes_.size(), 0);
        for (const RoadSegment& s : segments_) { ++counts[s.nodeA]; ++counts[s.nodeB]; }
        std::uint32_t running = 0;
        for (std::size_t n = 0; n < nodes_.size(); ++n)
        {
            nodes_[n].firstIncident = running;
            nodes_[n].incidentCount = 0;
            running += counts[n];
        }
        incident_.resize(running);
        for (std::uint32_t si = 0; si < segments_.size(); ++si)
        {
            const RoadSegment& s = segments_[si];
            for (int side = 0; side < 2; ++side)
            {
                const std::uint32_t from = side == 0 ? s.nodeA : s.nodeB;
                const std::uint32_t to   = side == 0 ? s.nodeB : s.nodeA;
                RoadNode& node = nodes_[from];
                Incidence& slot = incident_[node.firstIncident + node.incidentCount];
                slot.segment = si;
                slot.other = to;
                slot.heading = Heading(nodes_[to].position - node.position);
                ++node.incidentCount;
                if (static_cast<int>(s.roadClass) < static_cast<int>(node.highestClass))
                    node.highestClass = s.roadClass;
            }
        }
        for (RoadNode& node : nodes_)
        {
            Incidence* begin = incident_.data() + node.firstIncident;
            std::sort(begin, begin + node.incidentCount,
                      [](const Incidence& a, const Incidence& b) { return a.heading < b.heading; });
        }

        // A junction is signalised when at least three roads meet and one of them carries enough
        // traffic to need it. Everything else is a give-way, which the traffic model treats as a
        // priority rule rather than a light.
        for (RoadNode& node : nodes_)
            node.signalised = node.incidentCount >= 3 &&
                              static_cast<int>(node.highestClass) <= static_cast<int>(RoadClass::Collector);

        BuildSpatialIndex();
        raw_.clear();
        raw_.shrink_to_fit();
    }

    void RoadNetwork::ExtractBlocks(float sidewalkExtra)
    {
        blocks_.clear();
        if (segments_.empty()) return;

        // Half-edge 2*s is A->B, 2*s+1 is B->A. `next` is the standard planar walk: at the head of
        // a half-edge, take the outgoing edge immediately *clockwise* of this edge's twin. That
        // traces every bounded face counter-clockwise and the unbounded one clockwise, which is
        // what tells them apart at the end without any point-in-polygon test.
        const std::size_t halfCount = segments_.size() * 2;
        std::vector<std::uint32_t> next(halfCount, 0xFFFFFFFFu);

        // Where each half-edge sits in its origin node's angle-sorted incidence list.
        std::vector<std::uint32_t> slotOf(halfCount, 0);
        for (std::uint32_t n = 0; n < nodes_.size(); ++n)
        {
            const RoadNode& node = nodes_[n];
            for (std::uint32_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = incident_[node.firstIncident + k];
                const RoadSegment& seg = segments_[inc.segment];
                const std::uint32_t half = seg.nodeA == n ? inc.segment * 2 : inc.segment * 2 + 1;
                slotOf[half] = k;
            }
        }
        for (std::uint32_t half = 0; half < halfCount; ++half)
        {
            const std::uint32_t twin = half ^ 1u;
            const RoadSegment& seg = segments_[half / 2];
            const std::uint32_t head = (half & 1u) == 0 ? seg.nodeB : seg.nodeA;
            const RoadNode& node = nodes_[head];
            if (node.incidentCount == 0) continue;
            const std::uint32_t slot = slotOf[twin];
            const std::uint32_t prev = (slot + node.incidentCount - 1) % node.incidentCount;
            const Incidence& inc = incident_[node.firstIncident + prev];
            const RoadSegment& nextSeg = segments_[inc.segment];
            next[half] = nextSeg.nodeA == head ? inc.segment * 2 : inc.segment * 2 + 1;
        }

        std::vector<bool> visited(halfCount, false);
        std::vector<Vec2> outline;
        for (std::uint32_t start = 0; start < halfCount; ++start)
        {
            if (visited[start]) continue;
            outline.clear();
            std::uint32_t half = start;
            bool broken = false;
            RoadClass frontage = RoadClass::Alley;
            for (std::size_t guard = 0; guard <= halfCount; ++guard)
            {
                if (half == 0xFFFFFFFFu) { broken = true; break; }
                visited[half] = true;
                const RoadSegment& seg = segments_[half / 2];
                const std::uint32_t tail = (half & 1u) == 0 ? seg.nodeA : seg.nodeB;
                outline.push_back(nodes_[tail].position);
                if (static_cast<int>(seg.roadClass) < static_cast<int>(frontage))
                    frontage = seg.roadClass;
                half = next[half];
                if (half == start) break;
                if (visited[half]) { broken = true; break; }
            }
            if (broken || outline.size() < 3) continue;
            // The outer face walks clockwise, so its signed area is negative: that is the whole
            // test, and it is why the winding convention above had to be pinned down.
            const float area2 = SignedArea2(outline);
            if (area2 <= 0.0f) continue;

            CityBlock block;
            block.outline = SimplifyPolygon(outline);
            block.centroid = PolygonCentroid(block.outline);
            // The inset is the half-carriageway plus the pavement of the *most important* road on
            // the face, plus the caller's setback. One distance for the whole block rather than
            // one per edge is the simplification made here; its visible consequence is a slightly
            // deeper setback on the minor side of a corner block, which is what real corner blocks
            // look like anyway.
            const RoadProfile& profile = ProfileOf(frontage);
            const float inset = profile.carriagewayHalfWidth + profile.sidewalkWidth + sidewalkExtra;
            // A sliver block -- the triangles a diagonal avenue cuts out of a grid -- collapses
            // under the full setback, and rejecting it outright leaves a visible hole in the
            // street wall. Retry with a narrower pavement before giving up: a real city builds
            // those plots too, right up to the kerb.
            for (const float scale : {1.0f, 0.65f, 0.4f, 0.25f})
            {
                block.buildable = InsetPolygon(block.outline, inset * scale);
                if (!block.buildable.empty()) break;
            }
            block.area = block.buildable.empty() ? 0.0f : PolygonArea(block.buildable);
            blocks_.push_back(std::move(block));
        }
    }

    std::uint32_t RoadNetwork::CellOf(Vec2 p) const
    {
        const int x = Clamp(static_cast<int>((p.X - bounds_.min.X) / cellSize_), 0, gridWidth_ - 1);
        const int y = Clamp(static_cast<int>((p.Y - bounds_.min.Y) / cellSize_), 0, gridHeight_ - 1);
        return static_cast<std::uint32_t>(y) * gridWidth_ + x;
    }

    void RoadNetwork::BuildSpatialIndex()
    {
        if (nodes_.empty()) return;
        gridWidth_  = std::max(1, static_cast<int>((bounds_.max.X - bounds_.min.X) / cellSize_) + 1);
        gridHeight_ = std::max(1, static_cast<int>((bounds_.max.Y - bounds_.min.Y) / cellSize_) + 1);
        const std::size_t cells = static_cast<std::size_t>(gridWidth_) * gridHeight_;

        // Counting sort into CSR, twice: once over nodes, once over segments. A segment lands in
        // every cell its bounding box touches, so a nearest-segment query only has to look at the
        // query cell and its ring of neighbours.
        std::vector<std::uint32_t> counts(cells + 1, 0);
        for (const RoadNode& n : nodes_) ++counts[CellOf(n.position) + 1];
        for (std::size_t i = 1; i <= cells; ++i) counts[i] += counts[i - 1];
        nodeCellStart_ = counts;
        nodeCellItems_.resize(nodes_.size());
        std::vector<std::uint32_t> cursor(counts.begin(), counts.end() - 1);
        for (std::uint32_t i = 0; i < nodes_.size(); ++i)
            nodeCellItems_[cursor[CellOf(nodes_[i].position)]++] = i;

        std::vector<std::uint32_t> segCounts(cells + 1, 0);
        auto forEachSegmentCell = [&](const RoadSegment& s, auto&& fn) {
            const Vec2 a = nodes_[s.nodeA].position;
            const Vec2 b = nodes_[s.nodeB].position;
            const int x0 = Clamp(static_cast<int>((std::min(a.X, b.X) - bounds_.min.X) / cellSize_), 0, gridWidth_ - 1);
            const int x1 = Clamp(static_cast<int>((std::max(a.X, b.X) - bounds_.min.X) / cellSize_), 0, gridWidth_ - 1);
            const int y0 = Clamp(static_cast<int>((std::min(a.Y, b.Y) - bounds_.min.Y) / cellSize_), 0, gridHeight_ - 1);
            const int y1 = Clamp(static_cast<int>((std::max(a.Y, b.Y) - bounds_.min.Y) / cellSize_), 0, gridHeight_ - 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    fn(static_cast<std::uint32_t>(y) * gridWidth_ + x);
        };
        for (const RoadSegment& s : segments_)
            forEachSegmentCell(s, [&](std::uint32_t cell) { ++segCounts[cell + 1]; });
        for (std::size_t i = 1; i <= cells; ++i) segCounts[i] += segCounts[i - 1];
        segCellStart_ = segCounts;
        segCellItems_.resize(segCounts[cells]);
        std::vector<std::uint32_t> segCursor(segCounts.begin(), segCounts.end() - 1);
        for (std::uint32_t i = 0; i < segments_.size(); ++i)
            forEachSegmentCell(segments_[i], [&](std::uint32_t cell) { segCellItems_[segCursor[cell]++] = i; });
    }

    std::uint32_t RoadNetwork::FindNearestNode(Vec2 point) const
    {
        if (nodes_.empty()) return 0xFFFFFFFFu;
        const int cx = Clamp(static_cast<int>((point.X - bounds_.min.X) / cellSize_), 0, gridWidth_ - 1);
        const int cy = Clamp(static_cast<int>((point.Y - bounds_.min.Y) / cellSize_), 0, gridHeight_ - 1);
        std::uint32_t best = 0xFFFFFFFFu;
        float bestDist = 1e30f;
        // Widening rings: stop as soon as a ring that could not contain anything closer than the
        // current best comes up empty-handed.
        for (int ring = 0; ring < std::max(gridWidth_, gridHeight_); ++ring)
        {
            if (best != 0xFFFFFFFFu && bestDist < static_cast<float>(ring - 1) * cellSize_) break;
            for (int y = cy - ring; y <= cy + ring; ++y)
            {
                if (y < 0 || y >= gridHeight_) continue;
                for (int x = cx - ring; x <= cx + ring; ++x)
                {
                    if (x < 0 || x >= gridWidth_) continue;
                    if (ring > 0 && std::abs(x - cx) != ring && std::abs(y - cy) != ring) continue;
                    const std::uint32_t cell = static_cast<std::uint32_t>(y) * gridWidth_ + x;
                    for (std::uint32_t k = nodeCellStart_[cell]; k < nodeCellStart_[cell + 1]; ++k)
                    {
                        const std::uint32_t n = nodeCellItems_[k];
                        const float d = Distance(nodes_[n].position, point);
                        if (d < bestDist) { bestDist = d; best = n; }
                    }
                }
            }
        }
        return best;
    }

    std::uint32_t RoadNetwork::FindNearestNodeOfClass(Vec2 point, RoadClass maxClass) const
    {
        if (nodes_.empty()) return 0xFFFFFFFFu;
        std::uint32_t best = 0xFFFFFFFFu;
        float bestDist = 1e30f;
        const int cx = Clamp(static_cast<int>((point.X - bounds_.min.X) / cellSize_), 0, gridWidth_ - 1);
        const int cy = Clamp(static_cast<int>((point.Y - bounds_.min.Y) / cellSize_), 0, gridHeight_ - 1);
        for (int ring = 0; ring < std::max(gridWidth_, gridHeight_); ++ring)
        {
            if (best != 0xFFFFFFFFu && bestDist < static_cast<float>(ring - 1) * cellSize_) break;
            for (int y = cy - ring; y <= cy + ring; ++y)
            {
                if (y < 0 || y >= gridHeight_) continue;
                for (int x = cx - ring; x <= cx + ring; ++x)
                {
                    if (x < 0 || x >= gridWidth_) continue;
                    if (ring > 0 && std::abs(x - cx) != ring && std::abs(y - cy) != ring) continue;
                    const std::uint32_t cell = static_cast<std::uint32_t>(y) * gridWidth_ + x;
                    for (std::uint32_t k = nodeCellStart_[cell]; k < nodeCellStart_[cell + 1]; ++k)
                    {
                        const std::uint32_t n = nodeCellItems_[k];
                        if (static_cast<int>(nodes_[n].highestClass) > static_cast<int>(maxClass))
                            continue;
                        const float d = Distance(nodes_[n].position, point);
                        if (d < bestDist) { bestDist = d; best = n; }
                    }
                }
            }
        }
        return best;
    }

    std::uint32_t RoadNetwork::FindNearestSegment(Vec2 point, Vec2* outOnRoad, float* outT) const
    {
        if (segments_.empty()) return 0xFFFFFFFFu;
        const int cx = Clamp(static_cast<int>((point.X - bounds_.min.X) / cellSize_), 0, gridWidth_ - 1);
        const int cy = Clamp(static_cast<int>((point.Y - bounds_.min.Y) / cellSize_), 0, gridHeight_ - 1);
        std::uint32_t best = 0xFFFFFFFFu;
        float bestDist = 1e30f;
        Vec2 bestPoint(0.0f, 0.0f);
        float bestT = 0.0f;
        for (int ring = 0; ring < std::max(gridWidth_, gridHeight_); ++ring)
        {
            if (best != 0xFFFFFFFFu && bestDist < static_cast<float>(ring - 1) * cellSize_) break;
            for (int y = cy - ring; y <= cy + ring; ++y)
            {
                if (y < 0 || y >= gridHeight_) continue;
                for (int x = cx - ring; x <= cx + ring; ++x)
                {
                    if (x < 0 || x >= gridWidth_) continue;
                    if (ring > 0 && std::abs(x - cx) != ring && std::abs(y - cy) != ring) continue;
                    const std::uint32_t cell = static_cast<std::uint32_t>(y) * gridWidth_ + x;
                    for (std::uint32_t k = segCellStart_[cell]; k < segCellStart_[cell + 1]; ++k)
                    {
                        const std::uint32_t si = segCellItems_[k];
                        const RoadSegment& s = segments_[si];
                        float t = 0.0f;
                        const Vec2 on = ClosestPointOnSegment(nodes_[s.nodeA].position,
                                                              nodes_[s.nodeB].position, point, &t);
                        const float d = Distance(on, point);
                        if (d < bestDist) { bestDist = d; best = si; bestPoint = on; bestT = t; }
                    }
                }
            }
        }
        if (outOnRoad != nullptr) *outOnRoad = bestPoint;
        if (outT != nullptr) *outT = bestT;
        return best;
    }

    float RoadNetwork::TotalLength() const
    {
        float total = 0.0f;
        for (const RoadSegment& s : segments_) total += s.length;
        return total;
    }
}
