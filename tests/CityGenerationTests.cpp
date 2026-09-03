// SPDX-License-Identifier: MIT
//
// The city is generated from one 64-bit seed and is const for the rest of the run. Everything
// downstream -- the benchmark's comparability, the replayability of a defect, the claim in the
// README that the same seed is the same city -- depends on that being literally true.

#include <map>

#include "TestSupport.hpp"

namespace CnaCityTests
{
    TEST(CityGeneration, TheSameSeedProducesTheSameCity)
    {
        City first;
        City second;
        first.Generate(SmallCityConfig(31337));
        second.Generate(SmallCityConfig(31337));

        ASSERT_EQ(first.roads().nodes().size(), second.roads().nodes().size());
        ASSERT_EQ(first.buildings().size(), second.buildings().size());
        ASSERT_EQ(first.props().size(), second.props().size());
        for (std::size_t i = 0; i < first.buildings().size(); ++i)
        {
            const Building& a = first.buildings()[i];
            const Building& b = second.buildings()[i];
            EXPECT_FLOAT_EQ(a.center.X, b.center.X);
            EXPECT_FLOAT_EQ(a.center.Y, b.center.Y);
            EXPECT_FLOAT_EQ(a.height, b.height);
            EXPECT_EQ(a.residents, b.residents);
            EXPECT_EQ(a.jobs, b.jobs);
        }
    }

    TEST(CityGeneration, ADifferentSeedProducesADifferentCity)
    {
        City first;
        City second;
        first.Generate(SmallCityConfig(1));
        second.Generate(SmallCityConfig(2));
        // Not a strict requirement of anything, but a generator that ignores its seed passes every
        // determinism test in this file and is useless.
        bool differs = first.buildings().size() != second.buildings().size();
        for (std::size_t i = 0; !differs && i < first.buildings().size(); ++i)
            differs = first.buildings()[i].center.X != second.buildings()[i].center.X;
        EXPECT_TRUE(differs);
    }

    TEST(CityGeneration, EveryBuildingHasADoorOnTheRoadNetwork)
    {
        // A doorway is the hand-off between a building and the street, and it is the only part of
        // a building the simulation ever touches. One that is nowhere near a road is a citizen who
        // walks into the middle of a block and stops.
        City city;
        city.Generate(SmallCityConfig());
        ASSERT_GT(city.buildings().size(), 100u);
        for (const Building& building : city.buildings())
        {
            const std::uint32_t node = city.roads().FindNearestNode(building.doorway);
            ASSERT_NE(node, 0xFFFFFFFFu);
            EXPECT_LT(Distance(city.roads().nodes()[node].position, building.doorway), 260.0f)
                << "a doorway is 260 m from the nearest junction";
        }
    }

    TEST(CityGeneration, ThereAreMoreHomesAndJobsThanTheHeadlinePopulation)
    {
        // 100 000 citizens need somewhere to live and somewhere to work. When the capacity is
        // short, the shortfall does not announce itself -- the agents simply share a building.
        City city;
        city.Generate(CityConfig{});
        EXPECT_GT(city.totalResidentCapacity(), 100000u);
        EXPECT_GT(city.totalJobCapacity(), 100000u);
        EXPECT_FALSE(city.homes().empty());
        EXPECT_FALSE(city.workplaces().empty());
        EXPECT_FALSE(city.leisureVenues().empty());
    }

    TEST(CityGeneration, DistrictsCoverTheCityAndCarryEveryZone)
    {
        City city;
        city.Generate(CityConfig{});
        ASSERT_GT(city.districts().size(), 4u);

        std::map<int, int> byZone;
        for (const District& district : city.districts()) ++byZone[static_cast<int>(district.zone)];
        // Zoning drives building height, footprint and material. A city that generated one zone
        // would still render; it would just be uniform, which is the failure that looks like a
        // style choice.
        EXPECT_GE(byZone.size(), 3u) << "the districts are all the same zone";

        // Every district must be findable from its own centre, or DistrictAt disagrees with the
        // table it indexes and the pathfinder's corridors are drawn in the wrong place.
        for (const District& district : city.districts())
            EXPECT_EQ(city.DistrictAt(district.center), district.id);
    }

    TEST(CityGeneration, BuildingHeightAtAgreesWithTheBuildings)
    {
        // The occupancy grid the chase camera uses to climb out of walls. When it under-reports,
        // the camera sits inside a building; when it over-reports, the camera climbs for no reason.
        City city;
        city.Generate(SmallCityConfig());
        int checked = 0;
        for (const Building& building : city.buildings())
        {
            if (building.podiumHeight > 0.0f) continue;   // stepped towers are not one height
            const float sampled = city.BuildingHeightAt(building.center);
            EXPECT_GE(sampled, building.height * 0.5f)
                << "the height grid does not know about the building standing on this point";
            if (++checked > 400) break;
        }
        EXPECT_GT(checked, 0);
        // And open ground is open.
        EXPECT_FLOAT_EQ(city.BuildingHeightAt(Vec2(1.0e5f, 1.0e5f)), 0.0f);
    }
}
