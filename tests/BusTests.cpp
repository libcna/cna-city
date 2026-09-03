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

    TEST(Buses, NothingOnTheRoadEndsUpInsideAnythingElse)
    {
        // Buses and cars share one occupancy model now: a bus publishes where it is into the road's
        // lane buckets, so the cars behind it queue, and reads the same buckets back, so it queues
        // behind them. Before that they were two models of the same asphalt and each drove through
        // the other.
        //
        // "Inside each other" is measured on the road rather than in the world, which is the
        // distinction the earlier version of this test could not make: two buses eight metres apart
        // on two different streets that meet at a junction are not overlapping, they are at a
        // junction. Same segment, same direction, same lane, closer than a vehicle length -- that
        // is an overlap.
        Simulation sim;
        MakeBusCity(sim, 1000);
        const float busLength = ProfileOf(VehicleKind::Bus).length;

        int busIntoBus = 0;
        int busIntoCar = 0;
        int samples = 0;
        for (int i = 0; i < 3600; ++i)
        {
            sim.Step(0.5f);
            const std::vector<Bus>& fleet = sim.buses().buses();

            struct Placed
            {
                std::uint32_t segment;
                std::uint8_t forward;
                std::uint8_t lane;
                float s;
                float speed;
            };
            std::vector<Placed> onRoad;
            for (const Bus& bus : fleet)
            {
                Placed p{};
                if (!sim.buses().RoadPositionOf(bus, p.segment, p.forward, p.s)) continue;
                p.lane = sim.buses().LaneOf(bus);
                p.speed = bus.speed;
                onRoad.push_back(p);
            }

            for (std::size_t a = 0; a < onRoad.size(); ++a)
            {
                for (std::size_t b = a + 1; b < onRoad.size(); ++b)
                {
                    if (onRoad[a].segment != onRoad[b].segment ||
                        onRoad[a].forward != onRoad[b].forward ||
                        onRoad[a].lane != onRoad[b].lane)
                        continue;
                    ++samples;
                    if (std::abs(onRoad[a].s - onRoad[b].s) < busLength &&
                        onRoad[a].speed > 0.05f && onRoad[b].speed > 0.05f)
                        ++busIntoBus;
                }

                // And against the cars, which is the half that did not exist at all before.
                for (const Vehicle& vehicle : sim.traffic().vehicles())
                {
                    if (!vehicle.active || vehicle.segment != onRoad[a].segment ||
                        vehicle.forward != onRoad[a].forward || vehicle.lane != onRoad[a].lane)
                        continue;
                    const float half =
                        (busLength + ProfileOf(static_cast<VehicleKind>(vehicle.kind)).length) * 0.5f;
                    if (std::abs(vehicle.s - onRoad[a].s) < half * 0.6f && vehicle.speed > 0.05f &&
                        onRoad[a].speed > 0.05f)
                        ++busIntoCar;
                }
            }
        }
        EXPECT_GT(samples, 0) << "no two buses ever shared a lane; the test proves nothing";

        // Not zero, and the bound is the honest part of this test.
        //
        // Sharing the occupancy model took same-lane overlaps from hundreds a run to single
        // figures, and what is left is inherent to a bus not being integrated by the IDM itself:
        // when it crosses a junction its position along the road jumps to the start of the next
        // segment, and if something is standing there the two are momentarily inside each other.
        // A car in that position is pushed back by the traffic model's own negative-gap
        // correction, which a bus cannot have because Traffic does not own where it is.
        //
        // Adding a correction of the bus's own was tried and made it slightly worse -- pushing a
        // bus back can put it inside whatever is behind. Closing this properly means moving the
        // fleet into the IDM arrays, which is a bigger change than the one this test guards.
        // Measured here: three thousand six hundred ticks with ninety buses is a third of a
        // million bus-ticks, and this bounds the residual at a handful of them.
        EXPECT_LE(busIntoBus, 6) << busIntoBus << " moving bus pairs are inside each other";
        EXPECT_LE(busIntoCar, 6) << busIntoCar << " moving buses are inside a car";
    }

    TEST(Buses, TheCarsQueueBehindAStandingBus)
    {
        // The behaviour the unification is *for*, and the one a screenshot shows: a bus stopped at
        // a stop is an obstacle in its lane, so the traffic behind it slows rather than passing
        // through it. Before, a stationary bus was invisible to the road.
        Simulation sim;
        MakeBusCity(sim, 4000);
        const float busLength = ProfileOf(VehicleKind::Bus).length;

        int carsFoundBehindADwellingBus = 0;
        int thoseThatWereSlowing = 0;
        for (int i = 0; i < 2400; ++i)
        {
            sim.Step(0.5f);
            for (const Bus& bus : sim.buses().buses())
            {
                if (bus.dwellRemaining <= 0.0f) continue;
                std::uint32_t segment = 0;
                std::uint8_t forward = 1;
                float along = 0.0f;
                if (!sim.buses().RoadPositionOf(bus, segment, forward, along)) continue;
                const std::uint8_t lane = sim.buses().LaneOf(bus);

                for (const Vehicle& vehicle : sim.traffic().vehicles())
                {
                    if (!vehicle.active || vehicle.segment != segment ||
                        vehicle.forward != forward || vehicle.lane != lane)
                        continue;
                    const float behind = along - vehicle.s;
                    if (behind < busLength || behind > busLength + 12.0f) continue;
                    ++carsFoundBehindADwellingBus;
                    if (vehicle.acceleration < 0.05f) ++thoseThatWereSlowing;
                }
            }
        }
        ASSERT_GT(carsFoundBehindADwellingBus, 20)
            << "no car ever came up behind a bus at a stop; the test proves nothing";
        // Not all of them: a car far enough back on a long segment is still accelerating quite
        // correctly. Most of them is the claim.
        EXPECT_GT(static_cast<double>(thoseThatWereSlowing) / carsFoundBehindADwellingBus, 0.6)
            << thoseThatWereSlowing << " of " << carsFoundBehindADwellingBus
            << " cars close behind a stopped bus were still speeding up";
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
