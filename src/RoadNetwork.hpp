// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "CityMath.hpp"

namespace CnaCity
{
    /**
     * @brief What a road is for, which decides everything else about it.
     *
     * Width, lane count, speed limit, whether its junctions are signalised, whether buildings may
     * front onto it and how the renderer draws it all follow from this one enum -- the same way a
     * real street's classification decides them.
     */
    enum class RoadClass : std::uint8_t
    {
        Highway   = 0,  ///< The ring road: grade-separated in spirit, no frontage, no signals.
        Arterial  = 1,  ///< The 500 m grid and the diagonals; signalised, four lanes, buses.
        Collector = 2,  ///< District spine streets, two lanes each way.
        Local     = 3,  ///< Ordinary streets: one lane each way, kerbside parking, buildings.
        Alley     = 4   ///< Service lanes inside a block; pedestrians and deliveries only.
    };

    inline constexpr int kRoadClassCount = 5;

    /** @brief The fixed physical profile of each road class, in metres and metres per second. */
    struct RoadProfile
    {
        float carriagewayHalfWidth;  ///< Kerb to centreline.
        float laneWidth;
        float sidewalkWidth;         ///< 0 where the class has no pavement.
        float speedLimit;            ///< m/s.
        std::uint8_t lanesPerSide;
        bool  hasStreetLights;
        bool  allowsFrontage;        ///< Whether buildings may address this road.
    };

    [[nodiscard]] const RoadProfile& ProfileOf(RoadClass roadClass);

    /** @brief A junction, or a point where a road changes class or direction. */
    struct RoadNode
    {
        Vec2 position;
        std::uint32_t firstIncident = 0;   ///< Index into RoadNetwork::incident().
        std::uint16_t incidentCount = 0;
        std::uint16_t district = 0;
        RoadClass highestClass = RoadClass::Alley;  ///< The most important road meeting here.
        bool signalised = false;
    };

    /** @brief One straight stretch of road between two nodes. Undirected; travel is per lane. */
    struct RoadSegment
    {
        std::uint32_t nodeA = 0;
        std::uint32_t nodeB = 0;
        RoadClass roadClass = RoadClass::Local;
        std::uint16_t district = 0;
        float length = 0.0f;
        Vec2 direction{1.0f, 0.0f};   ///< Unit vector from A to B.
    };

    /**
     * @brief One incidence of a segment at a node, pre-sorted by outgoing heading.
     *
     * The sort is what makes both of the two things that need it cheap: the planar face walk that
     * finds city blocks, and a driver picking the next road at a junction.
     */
    struct Incidence
    {
        std::uint32_t segment = 0;
        std::uint32_t other = 0;    ///< The node at the far end.
        float heading = 0.0f;       ///< Outgoing heading from this node, in (-pi, pi].
    };

    /**
     * @brief A face of the road graph: the land enclosed by streets, i.e. a city block.
     *
     * Blocks come out of the network rather than being placed on it, which is why a diagonal
     * avenue produces triangular blocks with sharp corners exactly as it does in a real city.
     */
    struct CityBlock
    {
        std::vector<Vec2> outline;   ///< Counter-clockwise, on the road centrelines.
        std::vector<Vec2> buildable; ///< The outline inset past pavements and setback; may be empty.
        Vec2 centroid{0.0f, 0.0f};
        float area = 0.0f;           ///< Of @ref buildable, in m^2.
        std::uint16_t district = 0;
    };

    /**
     * @brief The city's street plan as a planar graph, built by cutting polylines against each
     * other.
     *
     * Nothing here knows about traffic, agents or rendering: this is the geometry every other
     * subsystem is a client of. It is built once, at start-up, and is `const` afterwards.
     */
    class RoadNetwork
    {
    public:
        /** @brief Adds a polyline to be cut into the graph. Only valid before @ref Build. */
        void AddPolyline(const std::vector<Vec2>& points, RoadClass roadClass);

        /** @brief Adds a single straight road. Only valid before @ref Build. */
        void AddSegment(Vec2 from, Vec2 to, RoadClass roadClass);

        /**
         * @brief Cuts every added polyline against every other, welds the results and indexes them.
         *
         * @param weldRadius Points closer than this become one node. Two streets that were meant
         *        to meet and miss by a centimetre would otherwise leave a hole a driver falls into.
         */
        void Build(float weldRadius = 0.35f);

        /** @brief Extracts the graph's bounded faces as city blocks. Call after @ref Build. */
        void ExtractBlocks(float sidewalkExtra);

        [[nodiscard]] const std::vector<RoadNode>& nodes() const { return nodes_; }
        [[nodiscard]] const std::vector<RoadSegment>& segments() const { return segments_; }
        [[nodiscard]] const std::vector<Incidence>& incident() const { return incident_; }
        [[nodiscard]] const std::vector<CityBlock>& blocks() const { return blocks_; }
        [[nodiscard]] const Bounds2& bounds() const { return bounds_; }

        [[nodiscard]] std::vector<RoadNode>& mutableNodes() { return nodes_; }
        [[nodiscard]] std::vector<RoadSegment>& mutableSegments() { return segments_; }

        /** @brief The incidences of @p node, as a contiguous view into @ref incident. */
        [[nodiscard]] const Incidence* incidenceBegin(std::uint32_t node) const
        {
            return incident_.data() + nodes_[node].firstIncident;
        }
        [[nodiscard]] std::size_t incidenceCount(std::uint32_t node) const
        {
            return nodes_[node].incidentCount;
        }

        /** @brief The node nearest to @p point, or 0xFFFFFFFF when the network is empty. */
        [[nodiscard]] std::uint32_t FindNearestNode(Vec2 point) const;

        /**
         * @brief The segment nearest to @p point, with the closest point on it.
         *
         * This is how an agent standing in a building doorway finds the pavement it should join.
         */
        [[nodiscard]] std::uint32_t FindNearestSegment(Vec2 point, Vec2* outOnRoad = nullptr,
                                                       float* outT = nullptr) const;

        /** @brief The nearest node reachable only over roads of class @p maxClass or better. */
        [[nodiscard]] std::uint32_t FindNearestNodeOfClass(Vec2 point, RoadClass maxClass) const;

        /** @brief The segment joining @p a and @p b, or 0xFFFFFFFF when they are not neighbours. */
        [[nodiscard]] std::uint32_t FindSegmentBetween(std::uint32_t a, std::uint32_t b) const
        {
            const RoadNode& node = nodes_[a];
            for (std::uint16_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = incident_[node.firstIncident + k];
                if (inc.other == b) return inc.segment;
            }
            return 0xFFFFFFFFu;
        }

        /** @brief Where @p segment sits in @p node's incidence list; used for signal groups. */
        [[nodiscard]] std::uint32_t IncidenceSlot(std::uint32_t node, std::uint32_t segment) const
        {
            const RoadNode& n = nodes_[node];
            for (std::uint16_t k = 0; k < n.incidentCount; ++k)
                if (incident_[n.firstIncident + k].segment == segment) return n.firstIncident + k;
            return 0xFFFFFFFFu;
        }

        [[nodiscard]] float TotalLength() const;

    private:
        struct RawSegment
        {
            Vec2 a;
            Vec2 b;
            RoadClass roadClass;
        };

        void BuildSpatialIndex();
        [[nodiscard]] std::uint32_t CellOf(Vec2 p) const;

        std::vector<RawSegment> raw_;
        std::vector<RoadNode> nodes_;
        std::vector<RoadSegment> segments_;
        std::vector<Incidence> incident_;
        std::vector<CityBlock> blocks_;
        Bounds2 bounds_;

        /// A uniform grid over nodes and over segments, for the two nearest-X queries above. The
        /// network is static after Build(), so a grid beats a tree on both build time and lookup.
        float cellSize_ = 50.0f;
        int gridWidth_ = 0;
        int gridHeight_ = 0;
        std::vector<std::uint32_t> nodeCellStart_;
        std::vector<std::uint32_t> nodeCellItems_;
        std::vector<std::uint32_t> segCellStart_;
        std::vector<std::uint32_t> segCellItems_;
    };
}
