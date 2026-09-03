// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "CityMath.hpp"

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
        void Step(const City& city, float dt,
                  const std::function<bool(Vec2, Vec2)>& mayProceed);

        [[nodiscard]] const std::vector<BusStop>& stops() const { return stops_; }
        [[nodiscard]] const std::vector<BusRoute>& routes() const { return routes_; }
        [[nodiscard]] const std::vector<Bus>& buses() const { return buses_; }
        [[nodiscard]] std::vector<Bus>& mutableBuses() { return buses_; }

        /** @brief The world position of a point @p metres round @p route. */
        [[nodiscard]] Vec2 PointOnRoute(std::uint32_t route, float distance) const;
        /** @brief The direction of travel at a point @p metres round @p route. */
        [[nodiscard]] Vec2 DirectionOnRoute(std::uint32_t route, float distance) const;

        /** @brief Where a bus sits on the road and which way it faces, offset into the nearside. */
        void Placement(const Bus& bus, Vec2& outPosition, float& outHeading) const;
        /** @brief The nearside lane's offset from the centreline at a point round @p route. */
        [[nodiscard]] float OffsetOnRoute(std::uint32_t route, float distance) const;

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
        /// One bus's road position for the tick, so the "do not drive into the bus in front" check
        /// compares everybody against the same instant rather than against a half-moved fleet.
        struct Occupancy
        {
            Vec2 position{0.0f, 0.0f};
            Vec2 direction{1.0f, 0.0f};
            float heading = 0.0f;
        };

        std::vector<BusStop> stops_;
        std::vector<BusRoute> routes_;
        std::vector<Bus> buses_;
        std::vector<Occupancy> occupancy_;
        /// The buses standing at a stop this tick, so the two checks that only care about those
        /// scan a handful rather than the whole fleet.
        std::vector<std::uint32_t> dwelling_;
    };
}
