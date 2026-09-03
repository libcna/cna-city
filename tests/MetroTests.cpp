// SPDX-License-Identifier: MIT
//
// The underground. The two defects worth regression-testing here are of opposite kinds: one made
// passengers wait forever on a platform (a routing mistake), and one made the tunnel geometry
// leak daylight (a winding mistake, covered in WindingTests). What is left is the contract the
// two halves meet on: the vertical layout, which six things in four files have to agree about.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    TEST(Metro, TheVerticalLayoutIsSelfConsistent)
    {
        // The first version had the tunnel floor 3.2 m below the carriage and the platform level
        // with neither, so a train ran through the air over a slab nobody was standing on. These
        // are the constants that stop that, and they are asserted rather than commented because
        // they live in one header and are used in four files.
        EXPECT_LT(kMetroTrackBed, kMetroRailTop) << "the rails are under the ballast";
        EXPECT_LT(kMetroRailTop, kMetroCarFloor) << "the carriage floor is below the rail";
        EXPECT_LT(kMetroCarFloor, kMetroCarRoof);
        EXPECT_FLOAT_EQ(kMetroCarFloor, kMetroPlatform)
            << "the platform is not level with the carriage floor, which is what a platform is";
        EXPECT_LT(kMetroCarRoof, kMetroTunnelRoof) << "the train does not fit in the tunnel";
        EXPECT_LT(kMetroWallNear, 0.0f);
        EXPECT_GT(kMetroPlatformEdge, 0.0f);
        EXPECT_LT(kMetroPlatformEdge, kMetroWallFar)
            << "the platform starts outside the tunnel it is in";
        // Cut-and-cover depth: the roof has to be under the street with room for services above
        // it, and the floor has to be somewhere a staircase can reach.
        EXPECT_LT(kMetroDepth + kMetroTunnelRoof, -2.0f) << "the tunnel roof is at street level";
        EXPECT_GT(kMetroDepth + kMetroTrackBed, -25.0f) << "the tunnel is absurdly deep";
    }

    TEST(Metro, LinesHaveStationsAndStationsKnowTheirLines)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(500));
        const MetroNetwork& metro = sim.metro();
        ASSERT_FALSE(metro.lines().empty());
        ASSERT_GT(metro.stations().size(), 3u);

        for (std::uint32_t l = 0; l < metro.lines().size(); ++l)
        {
            const MetroLine& line = metro.lines()[l];
            ASSERT_GE(line.stations.size(), 2u);
            ASSERT_EQ(line.points.size(), line.stations.size());
            ASSERT_EQ(line.distance.size(), line.stations.size());
            EXPECT_GT(line.length, 0.0f);
            for (std::size_t i = 1; i < line.distance.size(); ++i)
                EXPECT_GT(line.distance[i], line.distance[i - 1])
                    << "line " << l << " doubles back on itself at station " << i;

            for (std::uint32_t index = 0; index < line.stations.size(); ++index)
            {
                const MetroStation& station = metro.stations()[line.stations[index]];
                const bool listed = std::find(station.lines.begin(), station.lines.end(),
                                              std::make_pair(l, index)) != station.lines.end();
                EXPECT_TRUE(listed) << "a station on line " << l << " does not list that line";
            }
        }
    }

    TEST(Metro, EveryStationHasAnAxisAndAnEntranceOnTheStreet)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(500));
        for (const MetroStation& station : sim.metro().stations())
        {
            EXPECT_NEAR(Length(station.axis), 1.0f, 0.01f)
                << "the platform axis is not a unit vector; the whole station is built from it";
            EXPECT_LT(station.doorNode, sim.city().roads().nodes().size());
            EXPECT_LT(Distance(station.entrance, station.position), 200.0f)
                << "the staircase comes up 200 m from the platform below it";
        }
    }

    TEST(Metro, PointOnLineWalksTheLineAndClampsAtBothEnds)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(500));
        const MetroNetwork& metro = sim.metro();
        const MetroLine& line = metro.lines().front();

        EXPECT_LT(Distance(metro.PointOnLine(0, 0.0f), line.points.front()), 0.01f);
        EXPECT_LT(Distance(metro.PointOnLine(0, line.length), line.points.back()), 0.01f);
        EXPECT_LT(Distance(metro.PointOnLine(0, -500.0f), line.points.front()), 0.01f);
        EXPECT_LT(Distance(metro.PointOnLine(0, line.length + 500.0f), line.points.back()), 0.01f);

        // Arc length must advance with the parameter, or a train's speed is not its speed.
        float travelled = 0.0f;
        Vec2 previous = metro.PointOnLine(0, 0.0f);
        for (float d = 5.0f; d <= line.length; d += 5.0f)
        {
            const Vec2 here = metro.PointOnLine(0, d);
            travelled += Distance(previous, here);
            previous = here;
        }
        EXPECT_NEAR(travelled, line.length, line.length * 0.02f);
    }

    TEST(Metro, TrainsStayOnTheirLineAndCallAtStations)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(2000));
        ASSERT_FALSE(sim.metro().trains().empty());

        int dwellsSeen = 0;
        for (int i = 0; i < 1200; ++i)
        {
            sim.Step(2.0f);
            for (const MetroTrain& train : sim.metro().trains())
            {
                ASSERT_LT(train.line, sim.metro().lines().size());
                const MetroLine& line = sim.metro().lines()[train.line];
                EXPECT_GE(train.position, -1.0f);
                EXPECT_LE(train.position, line.length + 1.0f)
                    << "a train has run off the end of its line";
                EXPECT_GE(train.speed, 0.0f);
                EXPECT_LT(train.speed, 40.0f);
                if (train.dwellRemaining > 0.0f) ++dwellsSeen;
            }
        }
        EXPECT_GT(dwellsSeen, 0) << "no train ever stopped at a station";
    }

    TEST(Metro, PassengersBoardRideAndAlightRatherThanWaitingForever)
    {
        // Routes were once planned with an interchange, but a passenger can only board a train
        // that serves their destination -- so four hundred and forty-three people were still
        // standing on platforms at three in the morning. Direct lines only, plus a give-up timer.
        SimConfig config = SmallSimConfig(6000);
        config.startHour = 6.5f;
        config.carOwnership = 0.15f;   // push people onto public transport
        Simulation sim;
        sim.Initialize(config);

        std::uint32_t everRode = 0;
        std::uint32_t peakWaiting = 0;
        for (int i = 0; i < 2400; ++i)
        {
            sim.Step(3.0f);
            everRode = std::max(everRode, sim.stats().riding);
            peakWaiting = std::max(peakWaiting, sim.stats().waitingTrain);
        }
        EXPECT_GT(everRode, 0u) << "nobody ever got on a train";

        // Two more simulated hours past the peak: the platforms must drain.
        for (int i = 0; i < 2400; ++i) sim.Step(3.0f);
        EXPECT_LT(sim.stats().waitingTrain, std::max<std::uint32_t>(peakWaiting / 2, 5))
            << sim.stats().waitingTrain << " people are still on platforms hours after the peak";
    }

    TEST(Metro, AnUndergroundAgentIsAtTunnelDepthAndAnOutdoorOneIsNot)
    {
        SimConfig config = SmallSimConfig(6000);
        config.carOwnership = 0.15f;
        Simulation sim;
        sim.Initialize(config);
        bool sawUnderground = false;
        for (int i = 0; i < 1800 && !sawUnderground; ++i)
        {
            sim.Step(3.0f);
            for (std::uint32_t a : sim.waitingAgents())
            {
                EXPECT_NEAR(sim.AgentWorldPosition(a).Y, kMetroDepth + kMetroPlatform, 0.01f);
                sawUnderground = true;
            }
            for (std::uint32_t a : sim.walkingAgents())
                ASSERT_FLOAT_EQ(sim.AgentWorldPosition(a).Y, 0.0f)
                    << "a pedestrian is not at street level";
        }
        EXPECT_TRUE(sawUnderground) << "nobody ever stood on a platform";
    }
}
