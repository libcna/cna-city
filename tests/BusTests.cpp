// SPDX-License-Identifier: MIT
//
// The surface network. Written the day the buses stopped being a body shape handed to private
// commuters, so most of these assert the things that made that change worth making.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        /// Simulation is deliberately non-copyable -- it owns a thread pool -- so the fixtures
        /// hand one back by reference rather than by value.
        void MakeBusCity(Simulation& sim, std::uint32_t agents = 6000)
        {
            SimConfig config = SmallSimConfig(agents);
            config.startHour = 7.0f;
            config.carOwnership = 0.2f;
            sim.Initialize(config);
        }
    }

    TEST(Buses, RoutesAreLoopsOfStopsInOrderRoundTheLine)
    {
        Simulation sim;
        MakeBusCity(sim, 500);
        const BusNetwork& buses = sim.buses();
        ASSERT_FALSE(buses.routes().empty()) << "no bus route was generated at all";
        ASSERT_GT(buses.stops().size(), 5u);

        for (std::uint32_t r = 0; r < buses.routes().size(); ++r)
        {
            const BusRoute& route = buses.routes()[r];
            ASSERT_GE(route.stops.size(), 4u);
            ASSERT_EQ(route.stops.size(), route.stopDistance.size());
            ASSERT_GE(route.points.size(), 4u);
            ASSERT_EQ(route.points.size(), route.distance.size());
            ASSERT_EQ(route.points.size(), route.offset.size());
            EXPECT_GT(route.length, 500.0f);

            // A bus drives forwards round the loop and takes its stops in order. Out of order, it
            // drives past one and then reverses its target for the rest of the day.
            for (std::size_t i = 1; i < route.stopDistance.size(); ++i)
                EXPECT_GT(route.stopDistance[i], route.stopDistance[i - 1])
                    << "route " << r << " calls at stop " << i << " before stop " << i - 1;
            for (float d : route.stopDistance)
            {
                EXPECT_GE(d, 0.0f);
                EXPECT_LE(d, route.length);
            }
        }
    }

    TEST(Buses, EveryStopIsServedAndKnowsWhatServesIt)
    {
        // A shelter with no service is worse than no shelter: passengers queue at it forever.
        Simulation sim;
        MakeBusCity(sim, 500);
        const BusNetwork& buses = sim.buses();
        for (std::uint32_t s = 0; s < buses.stops().size(); ++s)
        {
            const BusStop& stop = buses.stops()[s];
            EXPECT_FALSE(stop.routes.empty()) << "stop " << s << " is served by nothing";
            EXPECT_LT(stop.node, sim.city().roads().nodes().size());
            for (const auto& [route, index] : stop.routes)
            {
                ASSERT_LT(route, buses.routes().size());
                ASSERT_LT(index, buses.routes()[route].stops.size());
                EXPECT_EQ(buses.routes()[route].stops[index], s)
                    << "stop " << s << " claims a place on a route that calls somewhere else";
            }
        }
    }

    TEST(Buses, AStopIsBesideTheCarriagewayRatherThanInIt)
    {
        // Stops used to be laid out on the junction nodes themselves, and a route turns at a node:
        // sampling the direction there gives the bisector of the turn, so the nearside offset
        // threw the bus outside the corner by the offset times root two -- onto the grass.
        Simulation sim;
        MakeBusCity(sim, 500);
        for (const BusStop& stop : sim.buses().stops())
        {
            const float queueToKerb = Distance(stop.position, stop.kerb);
            EXPECT_GT(queueToKerb, 1.0f) << "passengers queue in the gutter";
            EXPECT_LT(queueToKerb, 8.0f) << "passengers queue eight metres from the kerb";

            const std::uint32_t node = sim.city().roads().FindNearestNode(stop.kerb);
            ASSERT_NE(node, 0xFFFFFFFFu);
            EXPECT_LT(Distance(sim.city().roads().nodes()[node].position, stop.kerb), 60.0f)
                << "a bus stop is 60 m from the nearest junction";
        }
    }

    TEST(Buses, TheFleetRunsTheRoutesAndCallsAtTheStops)
    {
        Simulation sim;
        MakeBusCity(sim, 1000);
        ASSERT_FALSE(sim.buses().buses().empty());
        int dwellsSeen = 0;
        for (int i = 0; i < 1500; ++i)
        {
            sim.Step(2.0f);
            for (const Bus& bus : sim.buses().buses())
            {
                ASSERT_LT(bus.route, sim.buses().routes().size());
                const BusRoute& route = sim.buses().routes()[bus.route];
                EXPECT_GE(bus.position, 0.0f);
                EXPECT_LE(bus.position, route.length);
                EXPECT_LT(bus.nextStop, route.stops.size());
                EXPECT_GE(bus.speed, 0.0f);
                EXPECT_LT(bus.speed, 20.0f);
                if (bus.dwellRemaining > 0.0f) ++dwellsSeen;
            }
        }
        EXPECT_GT(dwellsSeen, 0) << "no bus ever stopped at a stop";
    }

    TEST(Buses, NoBusDrivesThroughAnotherOnItsOwnRoute)
    {
        // Buses are not in the car-following stream -- see plan.md -- so their own look-ahead is
        // the only thing between two of them and the same twelve metres of road, which is what a
        // screenshot caught a red bus and a green one doing.
        //
        // Along a route the look-ahead is exact and this asserts it. Across routes it is not, and
        // that is the honest boundary of an ad-hoc rule: two services converging on a shared
        // arterial from different streets see each other late, and the residual -- a few dozen
        // moments a simulated hour, never closer than four metres -- is bounded here rather than
        // asserted away. Putting the fleet in the IDM arrays is what would close it.
        Simulation sim;
        MakeBusCity(sim, 1000);
        int sameRoute = 0;
        int acrossRoutes = 0;
        float worstDistance = 1e9f;
        // Half-second steps, which is Simulation::kMovementStep -- the step the look-ahead is
        // actually consulted at.
        for (int i = 0; i < 3600; ++i)
        {
            sim.Step(0.5f);
            const std::vector<Bus>& fleet = sim.buses().buses();
            for (std::size_t a = 0; a < fleet.size(); ++a)
            {
                Vec2 pa(0.0f, 0.0f);
                float ha = 0.0f;
                sim.buses().Placement(fleet[a], pa, ha);
                for (std::size_t b = a + 1; b < fleet.size(); ++b)
                {
                    Vec2 pb(0.0f, 0.0f);
                    float hb = 0.0f;
                    sim.buses().Placement(fleet[b], pb, hb);
                    if (DistanceSq(pa, pb) > 144.0f) continue;
                    // Only buses travelling the same way. Two passing on opposite carriageways are
                    // about three metres apart, and that is a correctly modelled street.
                    if (Dot(FromHeading(ha), FromHeading(hb)) < 0.7f) continue;
                    if (fleet[a].speed <= 0.05f || fleet[b].speed <= 0.05f) continue;
                    worstDistance = std::min(worstDistance, Distance(pa, pb));
                    if (fleet[a].route == fleet[b].route) ++sameRoute;
                    else ++acrossRoutes;
                }
            }
        }
        EXPECT_EQ(sameRoute, 0)
            << "a bus drove into the one in front of it on its own route " << sameRoute << " times";
        EXPECT_LT(acrossRoutes, 400)
            << "converging routes are overlapping far more than the known residual";
        if (worstDistance < 1e8f)
            EXPECT_GT(worstDistance, 3.5f) << "two moving buses are more than half inside each other";
    }

    TEST(Buses, NoBusIsStuckForever)
    {
        // The other half of the look-ahead's contract, and the more important one: a rule that
        // stops buses is easy, and a rule that stops them and never lets them go turns the whole
        // service off. It did, twice, while this was being written -- two buses each yielding to
        // the other, nine metres apart, for the rest of the day.
        Simulation sim;
        MakeBusCity(sim, 1000);
        const std::size_t fleetSize = sim.buses().buses().size();
        ASSERT_GT(fleetSize, 0u);
        std::vector<float> stalled(fleetSize, 0.0f);
        std::vector<float> travelled(fleetSize, 0.0f);
        std::vector<float> previous(fleetSize, 0.0f);
        for (std::size_t b = 0; b < fleetSize; ++b) previous[b] = sim.buses().buses()[b].position;

        float worstStall = 0.0f;
        for (int i = 0; i < 1800; ++i)
        {
            sim.Step(2.0f);
            const std::vector<Bus>& fleet = sim.buses().buses();
            for (std::size_t b = 0; b < fleetSize; ++b)
            {
                if (fleet[b].speed < 0.01f && fleet[b].dwellRemaining <= 0.0f) stalled[b] += 2.0f;
                else stalled[b] = 0.0f;
                worstStall = std::max(worstStall, stalled[b]);
                float step = fleet[b].position - previous[b];
                if (step < 0.0f) step += sim.buses().routes()[fleet[b].route].length;
                travelled[b] += step;
                previous[b] = fleet[b].position;
            }
        }
        // The red-light hold has a 45 s ceiling of its own, so anything much past that is a bus
        // that has stopped for good.
        EXPECT_LT(worstStall, 90.0f) << "a bus stood still for " << worstStall
                                     << " s without dwelling or waiting at a signal";
        for (std::size_t b = 0; b < fleetSize; ++b)
            EXPECT_GT(travelled[b], 5000.0f)
                << "bus " << b << " covered only " << travelled[b]
                << " m in a simulated hour; it is not running a service";
    }

    TEST(Buses, PassengersBoardRideAndGetOff)
    {
        Simulation sim;
        MakeBusCity(sim, 8000);
        std::uint32_t peakRiding = 0;
        std::uint32_t peakWaiting = 0;
        for (int i = 0; i < 2400; ++i)
        {
            sim.Step(3.0f);
            peakRiding = std::max(peakRiding, sim.stats().onBus);
            peakWaiting = std::max(peakWaiting, sim.stats().waitingBus);
        }
        EXPECT_GT(peakRiding, 0u) << "nobody ever got on a bus";

        for (int i = 0; i < 2400; ++i) sim.Step(3.0f);
        EXPECT_LT(sim.stats().waitingBus, std::max<std::uint32_t>(peakWaiting / 2, 5))
            << "the bus stops never drain";
    }

    TEST(Buses, APassengerOnABusMovesWithIt)
    {
        Simulation sim;
        MakeBusCity(sim, 8000);
        for (int i = 0; i < 2400; ++i)
        {
            sim.Step(3.0f);
            const std::vector<Bus>& fleet = sim.buses().buses();
            for (std::size_t a = 0; a < sim.agents().size(); ++a)
            {
                if (sim.agents().mode[a] != static_cast<std::uint8_t>(Mode::OnBus)) continue;
                const std::uint32_t index = sim.agents().busVehicle[a];
                ASSERT_LT(index, fleet.size()) << "a passenger is aboard a bus that does not exist";
                Vec2 at(0.0f, 0.0f);
                float heading = 0.0f;
                sim.buses().Placement(fleet[index], at, heading);
                EXPECT_LT(Distance(sim.agents().position[a], at), 1.0f)
                    << "a passenger has been left behind by the bus they are on";
            }
        }
    }

    TEST(Buses, NoPrivateDriverIsGivenABus)
    {
        // The defect this whole subsystem replaced: VehicleKind::Bus was a body shape in the fleet
        // mix, handed to one commuter in twenty-five, so four hundred citizens a day drove a
        // twelve-metre bus to work alone.
        Simulation sim;
        MakeBusCity(sim, 4000);
        for (int i = 0; i < 1200; ++i)
        {
            sim.Step(3.0f);
            for (const Vehicle& vehicle : sim.traffic().vehicles())
                if (vehicle.active)
                    ASSERT_NE(static_cast<VehicleKind>(vehicle.kind), VehicleKind::Bus)
                        << "a private commuter is driving a bus again";
        }
    }

    TEST(Buses, PlanRideNeverProposesARideThatIsWorseThanWalking)
    {
        Simulation sim;
        MakeBusCity(sim, 500);
        const BusNetwork& buses = sim.buses();
        Rng rng(3);
        int proposed = 0;
        for (int trial = 0; trial < 400; ++trial)
        {
            const Vec2 from(rng.NextFloat(-600.0f, 600.0f), rng.NextFloat(-600.0f, 600.0f));
            const Vec2 to(rng.NextFloat(-600.0f, 600.0f), rng.NextFloat(-600.0f, 600.0f));
            BusNetwork::Ride ride;
            if (!buses.PlanRide(from, to, ride)) continue;
            ++proposed;
            ASSERT_LT(ride.boardStop, buses.stops().size());
            ASSERT_LT(ride.alightStop, buses.stops().size());
            ASSERT_LT(ride.route, buses.routes().size());
            EXPECT_NE(ride.boardStop, ride.alightStop);
            EXPECT_GT(ride.rideDistance, 0.0f);

            // Nine metres a second on the bus against one and a third on foot, ignoring the wait:
            // a proposal that is slower than walking even on that generous accounting is a
            // proposal no citizen should be offered.
            const float walk = Distance(from, to) / 1.35f;
            const float ride_ = (Distance(from, buses.stops()[ride.boardStop].position) +
                                 Distance(buses.stops()[ride.alightStop].position, to)) /
                                    1.35f +
                                ride.rideDistance / 9.0f;
            EXPECT_LE(ride_, walk * 1.01f)
                << "a proposed bus trip is slower than simply walking";
        }
        EXPECT_GT(proposed, 20) << "the bus network is unusable for almost every trip";
    }
}
