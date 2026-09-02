// SPDX-License-Identifier: MIT
#include "Pathfinder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "City.hpp"

namespace CnaCity
{
    namespace
    {
        /// 16 384 routes. Sized so the whole table stays inside a few megabytes and, more to the
        /// point, so that the hot set -- the trips between the doorways people actually use --
        /// fits without thrashing.
        constexpr std::size_t kCacheSlots = 1u << 14;
        /// Each cached route may hold this many junctions, in a region of the arena that belongs
        /// to its slot and to nothing else. Longer routes are still returned, they are simply not
        /// cached, which is the right trade: they are also the rarest.
        ///
        /// A ring arena shared by every slot was the first design and it was quietly wrong: when
        /// the write cursor wrapped it overwrote nodes that live entries still pointed at, and
        /// those entries then returned a route stitched out of somebody else's. Six megabytes of
        /// fixed regions buys the whole eviction problem away.
        constexpr std::uint32_t kMaxCachedNodes = 96;
        constexpr std::uint32_t kCacheArena = kCacheSlots * kMaxCachedNodes;

        /// How much slower than the speed limit a driver is assumed to be, per class. It is what
        /// makes a car route prefer an arterial that is twice as long, which is the behaviour that
        /// puts traffic where a city actually has it.
        constexpr float kClassSpeedFactor[kRoadClassCount] = {1.00f, 0.82f, 0.70f, 0.55f, 0.40f};
    }

    void Pathfinder::Build(const City& city)
    {
        city_ = &city;
        roads_ = &city.roads();
        districtSide_ = city.districtGridSide();

        const std::size_t nodeCount = roads_->nodes().size();
        gScore_.assign(nodeCount, 0.0f);
        cameFrom_.assign(nodeCount, kNoNode);
        visitStamp_.assign(nodeCount, 0);
        stamp_ = 0;
        openHeap_.reserve(1024);
        openF_.assign(nodeCount, 0.0f);

        const std::size_t districtCount = city.districts().size();
        corridor_.assign(districtCount, 0);
        districtG_.assign(districtCount, 0.0f);
        districtFrom_.assign(districtCount, 0);

        ClearCache();
    }

    void Pathfinder::ClearCache()
    {
        cache_.assign(kCacheSlots, CacheEntry{});
        cacheNodes_.assign(kCacheArena, 0);
        cacheEntries_ = 0;
        queryCounter_ = 0;
    }

    std::size_t Pathfinder::cacheBytes() const
    {
        return cache_.size() * sizeof(CacheEntry) + cacheNodes_.size() * sizeof(std::uint32_t);
    }

    bool Pathfinder::Traversable(const RoadSegment& segment, TravelMode mode) const
    {
        // A pedestrian may not walk along the ring highway: it has no pavement, and a route that
        // sent one down it would be both wrong and, on the overlay, extremely obvious.
        if (mode == TravelMode::Foot) return segment.roadClass != RoadClass::Highway;
        return segment.roadClass != RoadClass::Alley;
    }

    float Pathfinder::EdgeCost(const RoadSegment& segment, TravelMode mode) const
    {
        if (mode == TravelMode::Foot) return segment.length;
        const RoadProfile& profile = ProfileOf(segment.roadClass);
        const float speed = profile.speedLimit * kClassSpeedFactor[static_cast<int>(segment.roadClass)];
        return segment.length / std::max(1.0f, speed);
    }

    void Pathfinder::BuildDistrictCorridor(std::uint16_t from, std::uint16_t to)
    {
        const std::size_t count = corridor_.size();
        std::fill(corridor_.begin(), corridor_.end(), static_cast<std::uint8_t>(0));
        if (count == 0 || districtSide_ <= 0) return;

        // Dijkstra over a four-connected grid of forty-nine cells. Small enough that the array
        // scan below beats a heap, and clear enough to read.
        constexpr float kInfinity = std::numeric_limits<float>::infinity();
        std::fill(districtG_.begin(), districtG_.end(), kInfinity);
        std::vector<std::uint8_t> closed(count, 0);
        districtG_[from] = 0.0f;
        districtFrom_[from] = from;

        const std::vector<District>& districts = city_->districts();
        for (;;)
        {
            std::uint16_t best = 0;
            float bestG = kInfinity;
            for (std::size_t i = 0; i < count; ++i)
                if (!closed[i] && districtG_[i] < bestG) { bestG = districtG_[i]; best = static_cast<std::uint16_t>(i); }
            if (bestG == kInfinity) break;
            if (best == to) break;
            closed[best] = 1;

            const int bx = best % districtSide_;
            const int by = best / districtSide_;
            const int offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& offset : offsets)
            {
                const int nx = bx + offset[0];
                const int ny = by + offset[1];
                if (nx < 0 || ny < 0 || nx >= districtSide_ || ny >= districtSide_) continue;
                const auto neighbour = static_cast<std::uint16_t>(ny * districtSide_ + nx);
                // Parkland has no through streets, so a corridor that crosses one is a corridor
                // the road-level search cannot follow. Costing it high rather than forbidding it
                // keeps a route that genuinely has to skirt a park from failing outright.
                const float penalty = districts[neighbour].zone == ZoneType::Park ? 3.0f : 1.0f;
                const float step = Distance(districts[best].center, districts[neighbour].center) * penalty;
                if (districtG_[best] + step < districtG_[neighbour])
                {
                    districtG_[neighbour] = districtG_[best] + step;
                    districtFrom_[neighbour] = best;
                }
            }
        }

        if (districtG_[to] == kInfinity)
        {
            // No corridor: open everything and let the road-level search decide.
            std::fill(corridor_.begin(), corridor_.end(), static_cast<std::uint8_t>(1));
            return;
        }

        std::uint16_t walk = to;
        for (std::size_t guard = 0; guard <= count; ++guard)
        {
            corridor_[walk] = 1;
            if (walk == from) break;
            walk = districtFrom_[walk];
        }

        // Widen by one ring. A corridor exactly one cell wide is a corridor the road graph can
        // fail to follow -- a street that leaves the cell and comes straight back is normal, and a
        // search forbidden from taking it reports "unreachable" for a trip anyone could make.
        std::vector<std::uint8_t> widened = corridor_;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!corridor_[i]) continue;
            const int bx = static_cast<int>(i) % districtSide_;
            const int by = static_cast<int>(i) / districtSide_;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = bx + dx;
                    const int ny = by + dy;
                    if (nx < 0 || ny < 0 || nx >= districtSide_ || ny >= districtSide_) continue;
                    widened[static_cast<std::size_t>(ny) * districtSide_ + nx] = 1;
                }
        }
        corridor_.swap(widened);
    }

    bool Pathfinder::SearchRoads(std::uint32_t startNode, std::uint32_t goalNode, TravelMode mode,
                                 const std::vector<std::uint8_t>* allowedDistricts,
                                 std::vector<std::uint32_t>& out)
    {
        const std::vector<RoadNode>& nodes = roads_->nodes();
        const std::vector<RoadSegment>& segments = roads_->segments();
        const Vec2 goalPosition = nodes[goalNode].position;

        // The heuristic has to be admissible in the same units the edges are costed in, or A*
        // stops being A*. For a driver that is distance over the *fastest* road in the city.
        const float heuristicScale = mode == TravelMode::Foot
                                         ? 1.0f
                                         : 1.0f / (ProfileOf(RoadClass::Highway).speedLimit);

        ++stamp_;
        openHeap_.clear();
        gScore_[startNode] = 0.0f;
        cameFrom_[startNode] = kNoNode;
        visitStamp_[startNode] = stamp_;
        openF_[startNode] = Distance(nodes[startNode].position, goalPosition) * heuristicScale;
        openHeap_.push_back(startNode);

        const auto compare = [this](std::uint32_t a, std::uint32_t b) { return openF_[a] > openF_[b]; };

        bool found = false;
        while (!openHeap_.empty())
        {
            std::pop_heap(openHeap_.begin(), openHeap_.end(), compare);
            const std::uint32_t current = openHeap_.back();
            openHeap_.pop_back();
            if (current == goalNode) { found = true; break; }
            ++stats_.nodesExpanded;

            const RoadNode& node = nodes[current];
            for (std::uint32_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = roads_->incidenceBegin(current)[k];
                const RoadSegment& segment = segments[inc.segment];
                if (!Traversable(segment, mode)) continue;
                const std::uint32_t neighbour = inc.other;
                if (allowedDistricts != nullptr && !(*allowedDistricts)[nodes[neighbour].district] &&
                    neighbour != goalNode)
                    continue;

                const float tentative = gScore_[current] + EdgeCost(segment, mode);
                if (visitStamp_[neighbour] == stamp_ && tentative >= gScore_[neighbour]) continue;
                visitStamp_[neighbour] = stamp_;
                gScore_[neighbour] = tentative;
                cameFrom_[neighbour] = current;
                openF_[neighbour] = tentative +
                                    Distance(nodes[neighbour].position, goalPosition) * heuristicScale;
                openHeap_.push_back(neighbour);
                std::push_heap(openHeap_.begin(), openHeap_.end(), compare);
            }
        }

        if (!found) return false;

        // Walk the predecessors back, then reverse in place. Appending to the caller's buffer
        // rather than returning a vector is what keeps a hundred thousand queries allocation-free.
        const std::size_t base = out.size();
        std::uint32_t walk = goalNode;
        for (std::size_t guard = 0; guard <= nodes.size(); ++guard)
        {
            out.push_back(walk);
            if (walk == startNode) break;
            walk = cameFrom_[walk];
            if (walk == kNoNode) { out.resize(base); return false; }
        }
        std::reverse(out.begin() + static_cast<std::ptrdiff_t>(base), out.end());
        return true;
    }

    std::uint32_t Pathfinder::FindPath(std::uint32_t startNode, std::uint32_t goalNode,
                                       TravelMode mode, std::vector<std::uint32_t>& out)
    {
        if (roads_ == nullptr || startNode == kNoNode || goalNode == kNoNode) return 0;
        ++stats_.queries;
        ++queryCounter_;
        if (startNode == goalNode)
        {
            out.push_back(startNode);
            return 1;
        }

        // The mode is part of the key: a driver's route between the same two junctions is a
        // different route, and mixing them was a bug waiting to happen rather than a saving.
        const std::uint64_t key = (static_cast<std::uint64_t>(startNode) << 33) |
                                  (static_cast<std::uint64_t>(goalNode) << 1) |
                                  static_cast<std::uint64_t>(mode == TravelMode::Car ? 1 : 0);
        const std::uint64_t hashed = key * 0x9e3779b97f4a7c15ULL;
        const std::size_t slot = static_cast<std::size_t>(hashed >> 50) & (kCacheSlots - 1);

        CacheEntry& entry = cache_[slot];
        if (entry.key == key && entry.length > 0)
        {
            ++stats_.hits;
            out.insert(out.end(), cacheNodes_.begin() + entry.offset,
                       cacheNodes_.begin() + entry.offset + entry.length);
            return entry.length;
        }

        const std::size_t base = out.size();
        const std::uint16_t fromDistrict = roads_->nodes()[startNode].district;
        const std::uint16_t toDistrict = roads_->nodes()[goalNode].district;

        bool ok = false;
        if (fromDistrict != toDistrict)
        {
            BuildDistrictCorridor(fromDistrict, toDistrict);
            ok = SearchRoads(startNode, goalNode, mode, &corridor_, out);
            if (!ok)
            {
                // The corridor was wrong, not the trip. Retry city-wide -- and count it, because a
                // fallback rate that climbs is how a broken level-2 graph announces itself.
                ++stats_.corridorFallbacks;
                out.resize(base);
            }
        }
        if (!ok) ok = SearchRoads(startNode, goalNode, mode, nullptr, out);
        if (!ok) return 0;

        const auto length = static_cast<std::uint32_t>(out.size() - base);
        if (length <= kMaxCachedNodes)
        {
            // Direct-mapped, one region per slot: a colliding key simply takes the slot over.
            // There is no chain and no LRU, because the access pattern makes them pointless --
            // popular doorways are hit constantly and everything else is hit once.
            if (entry.key == 0) ++cacheEntries_;
            entry.key = key;
            entry.offset = static_cast<std::uint32_t>(slot) * kMaxCachedNodes;
            entry.length = length;
            std::copy(out.begin() + static_cast<std::ptrdiff_t>(base), out.end(),
                      cacheNodes_.begin() + entry.offset);
        }
        return length;
    }
}
