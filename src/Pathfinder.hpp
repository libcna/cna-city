// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "CityMath.hpp"
#include "RoadNetwork.hpp"

namespace CnaCity
{
    class City;

    inline constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;

    /** @brief Which network a route is being planned on; they cost different things. */
    enum class TravelMode : std::uint8_t
    {
        Foot = 0,    ///< Any road may be walked except the ring highway; cost is distance.
        Car          ///< Cost is time, so a driver prefers an arterial detour to a local short cut.
    };

    /**
     * @brief Two-level route planning over the road graph, with a shared result cache.
     *
     * The city's road graph is small -- a couple of thousand junctions -- and a flat A\* over it
     * would already be fast enough for one query. It is not fast enough for a hundred thousand
     * citizens each re-planning several times a day, and that is the actual problem: the cost is
     * the *number* of queries, not the size of any one of them.
     *
     * So there are two levels and a cache, and each attacks a different part of that:
     *
     * - **Level 2, the district graph.** Forty-nine cells, four-connected. A route across the city
     *   is first planned here, which costs almost nothing and produces a corridor.
     * - **Level 1, the road graph, restricted to that corridor** (plus one ring of districts, so a
     *   corridor that clips a park does not dead-end). This is what turns an A\* that could touch
     *   every junction in the city into one that touches a few hundred.
     * - **The cache**, keyed on the junction pair. Citizens do not have uniformly random
     *   destinations: they go to the same few thousand doorways, and the second person to walk
     *   from a given junction to a given office pays nothing.
     *
     * Level 0 -- getting from a doorway to the first junction, and avoiding the other pedestrians
     * on the way -- is not here. That is steering, and it lives in Crowd and Traffic.
     */
    class Pathfinder
    {
    public:
        void Build(const City& city);

        /**
         * @brief Plans from @p startNode to @p goalNode, appending the node sequence to @p out.
         *
         * @return The number of nodes appended, or 0 when no route exists. The start node is
         *         included; the goal node is the last entry.
         */
        std::uint32_t FindPath(std::uint32_t startNode, std::uint32_t goalNode, TravelMode mode,
                               std::vector<std::uint32_t>& out);

        /** @brief Cache statistics, for the HUD and the benchmark. */
        struct Stats
        {
            std::uint64_t queries = 0;
            std::uint64_t hits = 0;
            std::uint64_t nodesExpanded = 0;
            std::uint64_t corridorFallbacks = 0;   ///< Restricted searches that had to be retried city-wide.
        };
        [[nodiscard]] const Stats& stats() const { return stats_; }

        [[nodiscard]] std::size_t cacheBytes() const;
        /** @brief The largest district heat right now, so an overlay can scale its colours. */
        [[nodiscard]] float peakHeat() const { return peakHeat_; }

        /** @brief Empties the cache; used when the benchmark changes scale. */
        void ClearCache();

        /**
         * @brief Per district, how hard the planner has been working there lately.
         *
         * Incremented on a cache *miss* -- a hit costs nothing and says nothing about where the
         * work is -- and decayed with @ref DecayHeat, so what it shows is recent rather than
         * cumulative. A cumulative count over a simulated day is uniform by lunchtime and answers
         * no question anybody has; the interesting one is "which part of the city is the planner
         * struggling with *now*", and that is what an overlay can be pointed at.
         */
        [[nodiscard]] const std::vector<float>& heatByDistrict() const { return heat_; }

        /** @brief Fades @ref heatByDistrict towards zero. Called once per simulated tick. */
        void DecayHeat(float dt);

    private:
        /// Per district; see heatByDistrict.
        std::vector<float> heat_;
        float peakHeat_ = 0.0f;

        struct CacheEntry
        {
            std::uint64_t key = 0;        ///< (start << 32) | goal; 0 means empty.
            std::uint32_t offset = 0;     ///< Into cacheNodes_.
            std::uint32_t length = 0;
        };

        [[nodiscard]] float EdgeCost(const RoadSegment& segment, TravelMode mode) const;
        [[nodiscard]] bool Traversable(const RoadSegment& segment, TravelMode mode) const;
        bool SearchRoads(std::uint32_t startNode, std::uint32_t goalNode, TravelMode mode,
                         const std::vector<std::uint8_t>* allowedDistricts,
                         std::vector<std::uint32_t>& out);
        void BuildDistrictCorridor(std::uint16_t from, std::uint16_t to);

        const City* city_ = nullptr;
        const RoadNetwork* roads_ = nullptr;

        // --- Level 2 ---------------------------------------------------------------------------
        int districtSide_ = 0;
        std::vector<std::uint8_t> corridor_;      ///< Per district: 1 when the restricted search may enter.
        std::vector<float> districtG_;
        std::vector<std::uint16_t> districtFrom_;
        std::vector<std::uint8_t> districtClosed_;
        std::vector<std::uint8_t> corridorScratch_;

        // --- Level 1 ---------------------------------------------------------------------------
        // Reused across queries and stamped rather than cleared: memset-ing two arrays the size of
        // the road graph on every one of a hundred thousand queries costs more than the searches.
        std::vector<float> gScore_;
        std::vector<std::uint32_t> cameFrom_;
        std::vector<std::uint32_t> visitStamp_;
        std::uint32_t stamp_ = 0;
        std::vector<std::uint32_t> openHeap_;
        std::vector<float> openF_;

        // --- The cache -------------------------------------------------------------------------
        std::vector<CacheEntry> cache_;
        std::vector<std::uint32_t> cacheNodes_;
        std::size_t cacheEntries_ = 0;
        std::uint32_t queryCounter_ = 0;
        Stats stats_;
    };
}
