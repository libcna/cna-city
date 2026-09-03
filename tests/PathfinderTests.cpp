// SPDX-License-Identifier: MIT
//
// Two-level A* over a corridor from a district graph, plus a direct-mapped cache. The cache is the
// reason a hundred times the agents costs eight times the tick, so it is worth testing that it
// returns the same answer as the search that filled it -- a cache that returns a *different* route
// is a defect that only shows up as agents walking through walls at high population.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        struct Fixture
        {
            City city;
            Pathfinder pathfinder;
            Fixture()
            {
                city.Generate(SmallCityConfig());
                pathfinder.Build(city);
            }
        };

        /** @brief Every consecutive pair in a route must actually be joined by a road. */
        void ExpectWalkable(const City& city, const std::vector<std::uint32_t>& path)
        {
            for (std::size_t i = 1; i < path.size(); ++i)
                EXPECT_NE(city.roads().FindSegmentBetween(path[i - 1], path[i]), 0xFFFFFFFFu)
                    << "the route jumps from node " << path[i - 1] << " to " << path[i]
                    << " with no road between them";
        }
    }

    TEST(Pathfinder, ARouteStartsAtTheStartEndsAtTheGoalAndUsesRealRoads)
    {
        Fixture fixture;
        Rng rng(7);
        int planned = 0;
        for (int trial = 0; trial < 60; ++trial)
        {
            const auto count = static_cast<std::uint32_t>(fixture.city.roads().nodes().size());
            const std::uint32_t start = rng.NextUInt(count);
            const std::uint32_t goal = rng.NextUInt(count);
            if (start == goal) continue;

            std::vector<std::uint32_t> path;
            const std::uint32_t length = fixture.pathfinder.FindPath(start, goal, TravelMode::Foot,
                                                                     path);
            if (length == 0) continue;   // unreachable is a legitimate answer
            ++planned;
            ASSERT_EQ(path.size(), length);
            EXPECT_EQ(path.front(), start);
            EXPECT_EQ(path.back(), goal);
            ExpectWalkable(fixture.city, path);
        }
        EXPECT_GT(planned, 40) << "most junction pairs in a connected city should be reachable";
    }

    TEST(Pathfinder, TheCacheReturnsTheRouteTheSearchFound)
    {
        Fixture fixture;
        const auto count = static_cast<std::uint32_t>(fixture.city.roads().nodes().size());
        Rng rng(11);
        for (int trial = 0; trial < 40; ++trial)
        {
            const std::uint32_t start = rng.NextUInt(count);
            const std::uint32_t goal = rng.NextUInt(count);
            std::vector<std::uint32_t> first;
            if (fixture.pathfinder.FindPath(start, goal, TravelMode::Foot, first) == 0) continue;
            std::vector<std::uint32_t> second;
            fixture.pathfinder.FindPath(start, goal, TravelMode::Foot, second);
            EXPECT_EQ(first, second) << "the second query for the same pair answered differently";
        }
        EXPECT_GT(fixture.pathfinder.stats().hits, 0u) << "nothing was cached at all";
    }

    TEST(Pathfinder, RepeatedQueriesAreCounted)
    {
        Fixture fixture;
        const std::uint32_t a = fixture.city.roads().FindNearestNode(Vec2(-400.0f, -400.0f));
        const std::uint32_t b = fixture.city.roads().FindNearestNode(Vec2(400.0f, 400.0f));
        std::vector<std::uint32_t> path;
        ASSERT_GT(fixture.pathfinder.FindPath(a, b, TravelMode::Foot, path), 0u);
        const std::uint64_t hitsBefore = fixture.pathfinder.stats().hits;
        for (int i = 0; i < 10; ++i)
        {
            path.clear();
            fixture.pathfinder.FindPath(a, b, TravelMode::Foot, path);
        }
        EXPECT_EQ(fixture.pathfinder.stats().hits, hitsBefore + 10)
            << "the direct-mapped cache is not holding a route queried ten times running";
    }

    TEST(Pathfinder, ClearingTheCacheDoesNotChangeTheAnswers)
    {
        // --bench clears the cache between scales. If that changed the routes, the scaling curve
        // would be comparing different work at each population.
        Fixture fixture;
        const std::uint32_t a = fixture.city.roads().FindNearestNode(Vec2(-300.0f, 0.0f));
        const std::uint32_t b = fixture.city.roads().FindNearestNode(Vec2(300.0f, 100.0f));
        std::vector<std::uint32_t> before;
        ASSERT_GT(fixture.pathfinder.FindPath(a, b, TravelMode::Car, before), 0u);
        fixture.pathfinder.ClearCache();
        std::vector<std::uint32_t> after;
        fixture.pathfinder.FindPath(a, b, TravelMode::Car, after);
        EXPECT_EQ(before, after);
    }

    TEST(Pathfinder, ACarRouteNeverUsesAnAlley)
    {
        // Alleys are service lanes: pedestrians and deliveries. A car route through one is a car
        // driving between two buildings, which is both wrong and very visible.
        Fixture fixture;
        const auto count = static_cast<std::uint32_t>(fixture.city.roads().nodes().size());
        Rng rng(23);
        int checked = 0;
        for (int trial = 0; trial < 60 && checked < 25; ++trial)
        {
            std::vector<std::uint32_t> path;
            if (fixture.pathfinder.FindPath(rng.NextUInt(count), rng.NextUInt(count),
                                            TravelMode::Car, path) < 2)
                continue;
            ++checked;
            for (std::size_t i = 1; i < path.size(); ++i)
            {
                const std::uint32_t segment = fixture.city.roads().FindSegmentBetween(path[i - 1],
                                                                                       path[i]);
                ASSERT_NE(segment, 0xFFFFFFFFu);
                EXPECT_NE(fixture.city.roads().segments()[segment].roadClass, RoadClass::Alley);
            }
        }
        EXPECT_GT(checked, 0);
    }
}
