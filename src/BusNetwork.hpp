// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Archive.hpp"
#include "CityMath.hpp"
#include "Traffic.hpp"

namespace CnaCity
{
    class City;
    class Pathfinder;

    inline constexpr std::uint32_t kNoStop = 0xFFFFFFFFu;

    /** @brief Where a bus stops and a passenger stands. */
    struct BusStop
    {
        Vec2 position{0.0f, 0.0f};      ///< On the pavement, where the queue forms.
        Vec2 kerb{0.0f, 0.0f};          ///< Where the bus itself pulls up.
        std::uint32_t node = 0;         ///< The road node a walking passenger aims for.
        std::string name;
        /// Every (route, index-on-route) that calls here. Two routes sharing a stop is what makes
        /// the network a network, and the router treats a change as a second trip -- see
        /// @ref BusNetwork::PlanRoute.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> routes;
    };

    /**
     * @brief One bus route: a closed loop of stops and the driving line between them.
     *
     * A loop rather than an out-and-back because a loop has no terminus to turn round at, and a
     * turning circle on a road network generated from a planar graph is a special case with no
     * payoff. Every route runs one way round; the reverse direction is a different route.
     */
    struct BusRoute
    {
        std::vector<std::uint32_t> stops;        ///< In order round the loop.
        std::vector<float> stopDistance;         ///< Distance along @ref points of each stop.
        std::vector<Vec2> points;                ///< The driving line, through road nodes.
        /// Per point, how far right of the centreline the nearside lane is. A bus keeps to the
        /// kerb, and the kerb is a different distance away on an arterial and on a collector, so
        /// carrying one constant put buses in the overtaking lane of every four-lane road and on
        /// the pavement of every two-lane one.
        std::vector<float> offset;
        /// Per point, the road node it came from. The legs between them are what puts a bus on
        /// the same road the cars are on rather than on a line of its own.
        std::vector<std::uint32_t> node;
        /// Per leg (point i to i+1): the road segment, and whether the leg runs A-to-B along it.
        /// 0xFFFFFFFF where the two points are not joined by a segment, which the router should
        /// never produce and which is treated as "off the road" rather than trusted.
        std::vector<std::uint32_t> legSegment;
        std::vector<std::uint8_t> legForward;
        /// Per point, the road node it came from when that node is signalised, or 0xFFFFFFFF.
        ///
        /// Carried rather than looked up, because the alternative is a spatial query per bus per
        /// tick to answer "is there a signal in front of me", and the answer is no for almost all
        /// of them. It was a third of the whole simulation tick at low populations.
        std::vector<std::uint32_t> signalNode;
        std::vector<float> distance;             ///< Cumulative distance to each point.
        float length = 0.0f;
        std::uint8_t number = 0;                 ///< What it says on the front of the bus.
    };

    /** @brief One bus, somewhere round its route. */
    struct Bus
    {
        std::uint32_t route = 0;
        float position = 0.0f;       ///< Metres round the loop.
        float speed = 0.0f;
        std::uint32_t nextStop = 0;  ///< Index into BusRoute::stops.
        float dwellRemaining = 0.0f;
        float redLightSeconds = 0.0f;///< How long it has been held at a signal.
        std::uint32_t onboard = 0;
        std::uint32_t capacity = 68;
        std::uint8_t appearance = 0;
    };

    /**
     * @brief The surface network: stops, routes, and the buses running them.
     *
     * Buses exist here rather than in @ref Traffic for the same reason trains exist in
     * @ref MetroNetwork: a bus is not a car with a different body, it is a vehicle following a
     * timetable, and the thing that makes it interesting -- that it stops where the shelters are,
     * waits, and takes people with it -- has nothing to do with car-following. Before this, a bus
     * was a body shape handed to one private commuter in twenty-five, which produced a city where
     * four hundred people drove a twelve-metre bus to work alone and every shelter on every
     * arterial was decoration.
     *
     * They are not in the IDM stream, so they do not queue behind cars; they do obey the signals,
     * which is the half of the interaction that is visible from the pavement.
     */
    class BusNetwork
    {
    public:
        /**
         * @brief Lays out stops on the arterials and threads @p routeCount loops through them.
         * @param pathfinder Used to connect consecutive stops along the actual road network, so a
         *                   bus drives round the streets rather than across the blocks.
         */
        void Generate(const City& city, Pathfinder& pathfinder, int routeCount,
                      std::uint64_t seed);

        /** @brief Advances every bus by @p dt seconds. @p green answers "may I cross this node". */
        /**
         * @brief Advances every bus by @p dt seconds.
         *
         * @param mayProceed Answers "may a vehicle here, heading there, cross the junction".
         * @param gapAhead   The distance to the nearest thing in front of a bus in its own lane,
         *                   and how fast that thing is going -- read out of the road's own
         *                   occupancy structure, so it counts cars and buses alike. This is the
         *                   half of the unification that makes a bus queue; the other half is
         *                   @ref Occupancy, which is what makes the cars behind it queue.
         */
        void Step(const City& city, float dt,
                  const std::function<bool(Vec2, Vec2)>& mayProceed,
                  const std::function<float(std::uint32_t, std::uint8_t, std::uint8_t, float,
                                            std::uint32_t, float*)>& gapAhead);

        /** @brief Where every bus is on the road, for the traffic model to queue behind. */
        [[nodiscard]] std::vector<RoadObstacle> RoadOccupancy() const;

        [[nodiscard]] const std::vector<BusStop>& stops() const { return stops_; }
        [[nodiscard]] const std::vector<BusRoute>& routes() const { return routes_; }
        [[nodiscard]] const std::vector<Bus>& buses() const { return buses_; }
        [[nodiscard]] std::vector<Bus>& mutableBuses() { return buses_; }

        /** @brief Reads or writes the fleet. The routes and stops come from the seed. */
        void Serialize(Archive& archive) { archive.Vector(buses_); }

        /** @brief The world position of a point @p metres round @p route. */
        [[nodiscard]] Vec2 PointOnRoute(std::uint32_t route, float distance) const;
        /** @brief The direction of travel at a point @p metres round @p route. */
        [[nodiscard]] Vec2 DirectionOnRoute(std::uint32_t route, float distance) const;

        /** @brief Where a bus sits on the road and which way it faces, offset into the nearside. */
        void Placement(const Bus& bus, Vec2& outPosition, float& outHeading) const;
        /** @brief The nearside lane's offset from the centreline at a point round @p route. */
        [[nodiscard]] float OffsetOnRoute(std::uint32_t route, float distance) const;

        /**
         * @brief Where a bus is on the *road*, rather than on its own line.
         *
         * The bridge between the timetable and the carriageway. Returns false when the route has
         * wandered off the segment table, which the router should never produce.
         */
        [[nodiscard]] bool RoadPositionOf(const Bus& bus, std::uint32_t& outSegment,
                                          std::uint8_t& outForward, float& outS) const;

        /** @brief The kerbside lane index for the leg @p bus is on. */
        [[nodiscard]] std::uint8_t LaneOf(const Bus& bus) const;

        /**
         * @brief Which leg of @p route a bus at @p position is on.
         *
         * One answer, used by everything that needs it. The lane and the road segment used to be
         * worked out separately -- the segment from the leg, the lane from the lateral offset
         * interpolated *along* the leg -- and the two disagreed wherever a route moved from a
         * narrow street to a wide one: the offset crossed a lane boundary in the middle of a leg,
         * so two buses on the same piece of kerb were filed under different lanes and became
         * invisible to each other. They then drove through each other into a single point, which
         * a gap-based follower model has no way out of.
         */
        [[nodiscard]] std::size_t LegAt(const BusRoute& route, float position) const;

        /** @brief The stop nearest @p point, or @ref kNoStop. */
        [[nodiscard]] std::uint32_t NearestStop(Vec2 point) const;

        /**
         * @brief The best boarding and alighting pair for a trip, or false when a bus will not help.
         *
         * Direct routes only, for the reason the metro takes direct lines only: a passenger whose
         * plan needs a change waits at a stop for a bus that by construction is never going to
         * serve their destination, and the queue never drains.
         */
        struct Ride
        {
            std::uint32_t boardStop = kNoStop;
            std::uint32_t alightStop = kNoStop;
            std::uint32_t route = 0;
            float rideDistance = 0.0f;
        };
        [[nodiscard]] bool PlanRide(Vec2 from, Vec2 to, Ride& ride) const;

        /** @brief The route a passenger at @p from takes toward @p to, or 0xFFFFFFFF. */
        [[nodiscard]] std::uint32_t RouteBetween(std::uint32_t fromStop,
                                                 std::uint32_t toStop) const;

        [[nodiscard]] std::size_t MemoryBytes() const;

    private:
        std::vector<BusStop> stops_;
        std::vector<BusRoute> routes_;
        std::vector<Bus> buses_;
        /// Buses that are not moving, and where they are. Rebuilt each pass and appended to as
        /// the pass stops more of them, so an arriving bus can be told the bay is occupied.
        /// "Standing" rather than "dwelling": a bus whose dwell has expired but which cannot pull
        /// away yet is still filling the bay, and treating only dwelling buses as present is what
        /// let seven of them end up on the same metre of road.
        std::vector<std::uint32_t> standing_;
        std::vector<Vec2> standingAt_;
        /// The buses on each route, rebuilt each pass. Lets the same-route following check below
        /// look at the seven buses of one route rather than at the whole fleet.
        std::vector<std::vector<std::uint32_t>> routeBuses_;
    };
}
