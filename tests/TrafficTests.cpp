// SPDX-License-Identifier: MIT
//
// The carriageway: signals, IDM car-following and junctions. Two of the historical defects here
// produced total gridlock, which is the one failure mode that *does* look wrong -- but both were
// misdiagnosed as tuning first, because 4 000 vehicles at 0.2 m/s reads as congestion.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        /// Simulation is deliberately non-copyable -- it owns a thread pool -- so the fixtures
        /// hand one back by reference rather than by value.
        void MakeRunningCity(Simulation& sim, std::uint32_t agents = 3000, float hour = 7.5f)
        {
            SimConfig config = SmallSimConfig(agents);
            config.startHour = hour;
            config.carOwnership = 0.8f;   // put traffic on the road quickly
            sim.Initialize(config);
        }
    }

    TEST(Traffic, SignalsCycleThroughGreenAmberAndRed)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(500));
        const RoadNetwork& roads = sim.city().roads();

        std::uint32_t signalised = 0;
        for (const RoadNode& node : roads.nodes()) signalised += node.signalised ? 1u : 0u;
        ASSERT_GT(signalised, 0u) << "no junction in the city is signalised";
        // But not all of them: signalising every junction whose highest class was a collector put
        // lights on nine junctions in fourteen and the network locked solid.
        EXPECT_LT(signalised, roads.nodes().size() / 3)
            << signalised << " of " << roads.nodes().size()
            << " junctions are signalised; queues on short segments will grow past their entrances";

        std::uint32_t junction = 0;
        for (std::uint32_t n = 0; n < roads.nodes().size(); ++n)
            if (roads.nodes()[n].signalised) { junction = n; break; }

        bool sawGreen = false;
        bool sawRed = false;
        for (int i = 0; i < 400; ++i)
        {
            sim.Step(1.0f);
            for (std::size_t k = 0; k < roads.incidenceCount(junction); ++k)
            {
                if (sim.traffic().IsGreen(junction, static_cast<std::uint32_t>(k))) sawGreen = true;
                else sawRed = true;
            }
        }
        EXPECT_TRUE(sawGreen) << "a signalised junction never showed a green";
        EXPECT_TRUE(sawRed) << "a signalised junction never showed anything but green";
    }

    TEST(Traffic, NoTwoVehiclesOccupyTheSamePlaceInTheSameLane)
    {
        // Every car used to be spawned at the head of its first segment, so two drivers leaving
        // the same street seconds apart spawned *inside* each other -- and the IDM answers a
        // negative gap with maximum braking, for ever.
        Simulation sim;
        MakeRunningCity(sim);
        for (int i = 0; i < 700; ++i) sim.Step(2.0f);

        std::map<std::uint64_t, std::vector<float>> byLane;
        for (const Vehicle& vehicle : sim.traffic().vehicles())
        {
            if (!vehicle.active) continue;
            const std::uint64_t lane = (static_cast<std::uint64_t>(vehicle.segment) << 8) |
                                       (static_cast<std::uint64_t>(vehicle.forward) << 4) |
                                       vehicle.lane;
            byLane[lane].push_back(vehicle.s);
        }
        ASSERT_FALSE(byLane.empty()) << "no vehicle ever reached the road";

        int overlapping = 0;
        int pairs = 0;
        for (auto& [lane, positions] : byLane)
        {
            std::sort(positions.begin(), positions.end());
            for (std::size_t i = 1; i < positions.size(); ++i)
            {
                ++pairs;
                if (positions[i] - positions[i - 1] < 1.5f) ++overlapping;
            }
        }
        if (pairs > 0)
            EXPECT_LT(static_cast<double>(overlapping) / pairs, 0.02)
                << overlapping << " of " << pairs << " neighbouring pairs are inside each other";
    }

    TEST(Traffic, TheNetworkDoesNotLockSolidAtThePeak)
    {
        Simulation sim;
        MakeRunningCity(sim, 4000, 7.0f);
        for (int i = 0; i < 1500; ++i) sim.Step(2.0f);
        ASSERT_GT(sim.traffic().activeCount(), 5u) << "no traffic to measure";
        EXPECT_GT(sim.traffic().meanSpeed(), 0.5f)
            << "mean traffic speed is " << sim.traffic().meanSpeed()
            << " m/s -- that is gridlock, not congestion";
    }

    TEST(Traffic, EveryVehicleStaysOnItsOwnSegmentAndBelowTheSpeedLimit)
    {
        Simulation sim;
        MakeRunningCity(sim);
        for (int i = 0; i < 900; ++i)
        {
            sim.Step(2.0f);
            for (const Vehicle& vehicle : sim.traffic().vehicles())
            {
                if (!vehicle.active) continue;
                ASSERT_LT(vehicle.segment, sim.city().roads().segments().size());
                const RoadSegment& segment = sim.city().roads().segments()[vehicle.segment];
                EXPECT_GE(vehicle.s, -0.5f);
                EXPECT_LE(vehicle.s, segment.length + 1.0f)
                    << "a vehicle has run off the end of its segment";
                EXPECT_GE(vehicle.speed, -0.01f) << "a vehicle is reversing down the carriageway";
                // A car that turns off the ring road onto an arterial carries its speed with it
                // and sheds it over the next few seconds, so being over the local limit is not by
                // itself wrong. Accelerating while over it is.
                // Against the fastest thing the network allows anywhere, not against this road's
                // limit. A car that turns off the ring road onto an arterial carries its speed
                // with it and sheds it over the next few seconds, and the junction pass moves it
                // onto the slower road *after* the car-following pass has run -- so for one tick a
                // vehicle is legitimately both over the local limit and still accelerating.
                // What no vehicle may ever be is faster than the quickest driver on the quickest
                // road, which is the integration blowing up.
                EXPECT_LT(vehicle.speed,
                          ProfileOf(RoadClass::Highway).speedLimit *
                              ProfileOf(VehicleKind::Taxi).speedFactor * 1.1f)
                    << "a vehicle is faster than anything in the city is allowed to go";
            }
        }
    }

    TEST(Traffic, DriversDoNotTeleportToTheirDestination)
    {
        // FinishTrip used to move every arriving agent to the destination doorway from wherever
        // the vehicle happened to be. For a normal arrival that is tens of metres; for one of the
        // vehicles abandoned to gridlock it is a jump across the city, and a demonstration that
        // quietly relocates the citizen it could not deliver is reporting a success it did not
        // have. Those are counted as abandoned walks now instead.
        Simulation sim;
        MakeRunningCity(sim, 4000, 7.0f);
        for (int i = 0; i < 2000; ++i) sim.Step(3.0f);
        EXPECT_GT(sim.stats().carTripsStarted, 0u);
        if (sim.traffic().gridlockedCount() > 0)
            EXPECT_GT(sim.stats().abandonedWalks, 0u)
                << "vehicles were given up on but nobody had to walk; they were teleported";
    }

    TEST(Traffic, PlacementPutsVehiclesOnTheirOwnCarriageway)
    {
        // The lane offset's sign has to agree between the cars and the buses, or one of them
        // drives down the oncoming carriageway. The city drives on the left; what matters here is
        // that opposite directions come out on opposite sides.
        Simulation sim;
        MakeRunningCity(sim);
        for (int i = 0; i < 400; ++i) sim.Step(2.0f);
        const RoadNetwork& roads = sim.city().roads();
        int checked = 0;
        for (const Vehicle& vehicle : sim.traffic().vehicles())
        {
            if (!vehicle.active || checked > 200) continue;
            Vec2 position(0.0f, 0.0f);
            float heading = 0.0f;
            sim.traffic().Placement(sim.city(), vehicle, position, heading);
            const RoadSegment& segment = roads.segments()[vehicle.segment];
            const Vec2 centreline = roads.nodes()[segment.nodeA].position +
                                    segment.direction * vehicle.s;
            const Vec2 offset = position - (vehicle.forward
                                                ? centreline
                                                : roads.nodes()[segment.nodeB].position -
                                                      segment.direction * vehicle.s);
            const float side = Dot(offset, Perp(segment.direction));
            // Whatever the convention, one direction must be on one side and the other on the
            // other -- and the offset must be inside the carriageway.
            EXPECT_LT(std::abs(side), ProfileOf(segment.roadClass).carriagewayHalfWidth + 0.6f)
                << "a vehicle is outside the kerb line";
            if (vehicle.forward) EXPECT_LT(side, 0.1f);
            else EXPECT_GT(side, -0.1f);
            ++checked;
        }
        EXPECT_GT(checked, 0);
    }
}
