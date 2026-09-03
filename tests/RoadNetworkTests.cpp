// SPDX-License-Identifier: MIT
//
// The road graph is the thing every other subsystem is a client of: the pathfinder searches it,
// the traffic drives it, the pedestrians walk beside it, the buses are routed along it and the
// blocks the buildings stand in are its faces. A defect here is a defect everywhere, and it does
// not look like one -- the district-street bug that made every block an annulus produced a city
// with 10.8 million square metres of "block" in a 9.6 million square metre city, and rendered.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        /**
         * @brief A square with a cross through it: twelve segments and no dangling ends.
         *
         * Every test graph in this file has to be a closed figure, and that is a property of the
         * builder rather than of the tests. `Build` prunes dead ends to convergence -- every
         * dangling edge in a generated city is an artefact, and one is poison to the block
         * extractor's face walk -- so a bare plus sign is pruned to nothing, correctly.
         */
        RoadNetwork MakeClosedTestGraph()
        {
            RoadNetwork roads;
            roads.AddSegment(Vec2(-100.0f, -100.0f), Vec2(100.0f, -100.0f), RoadClass::Arterial);
            roads.AddSegment(Vec2(100.0f, -100.0f), Vec2(100.0f, 100.0f), RoadClass::Arterial);
            roads.AddSegment(Vec2(100.0f, 100.0f), Vec2(-100.0f, 100.0f), RoadClass::Arterial);
            roads.AddSegment(Vec2(-100.0f, 100.0f), Vec2(-100.0f, -100.0f), RoadClass::Arterial);
            roads.AddSegment(Vec2(-100.0f, 0.0f), Vec2(100.0f, 0.0f), RoadClass::Collector);
            roads.AddSegment(Vec2(0.0f, -100.0f), Vec2(0.0f, 100.0f), RoadClass::Collector);
            roads.Build();
            return roads;
        }
    }

    TEST(RoadNetwork, CrossingPolylinesAreSplitAtTheirIntersection)
    {
        const RoadNetwork roads = MakeClosedTestGraph();

        // The two crossing streets meet in the middle, and a graph that does not cut them there is
        // one a driver cannot turn in.
        const std::uint32_t centre = roads.FindNearestNode(Vec2(0.0f, 0.0f));
        ASSERT_NE(centre, 0xFFFFFFFFu);
        EXPECT_NEAR(roads.nodes()[centre].position.X, 0.0f, 0.01f);
        EXPECT_NEAR(roads.nodes()[centre].position.Y, 0.0f, 0.01f);
        EXPECT_EQ(roads.incidenceCount(centre), 4u);

        // Nine nodes -- four corners, four edge midpoints, one centre -- and twelve segments.
        EXPECT_EQ(roads.nodes().size(), 9u);
        EXPECT_EQ(roads.segments().size(), 12u);

        // And the arterial is still an arterial where the collector crosses it: the class of a
        // shared centreline is the more important of the two.
        const std::uint32_t edge = roads.FindNearestNode(Vec2(100.0f, 0.0f));
        const std::uint32_t toCorner = roads.FindSegmentBetween(edge,
                                                                roads.FindNearestNode(Vec2(100.0f, 100.0f)));
        ASSERT_NE(toCorner, 0xFFFFFFFFu);
        EXPECT_EQ(roads.segments()[toCorner].roadClass, RoadClass::Arterial);
    }

    TEST(RoadNetwork, DanglingStubsArePrunedAway)
    {
        // Not a convenience: a face containing a slit is not a simple polygon, and the block
        // extractor answers with one enormous self-touching outline instead of the dozen real
        // blocks around it. That is exactly what produced 393 giant blocks covering more ground
        // than the city has.
        RoadNetwork roads;
        roads.AddSegment(Vec2(-100.0f, -100.0f), Vec2(100.0f, -100.0f), RoadClass::Arterial);
        roads.AddSegment(Vec2(100.0f, -100.0f), Vec2(100.0f, 100.0f), RoadClass::Arterial);
        roads.AddSegment(Vec2(100.0f, 100.0f), Vec2(-100.0f, 100.0f), RoadClass::Arterial);
        roads.AddSegment(Vec2(-100.0f, 100.0f), Vec2(-100.0f, -100.0f), RoadClass::Arterial);
        roads.AddSegment(Vec2(0.0f, 100.0f), Vec2(0.0f, 260.0f), RoadClass::Local);   // a stub
        roads.Build();

        EXPECT_EQ(roads.nodes().size(), 5u) << "the square plus the point the stub met it at";
        EXPECT_EQ(roads.segments().size(), 5u);
        for (const RoadNode& node : roads.nodes())
            EXPECT_LT(node.position.Y, 101.0f) << "the stub survived pruning";
    }

    TEST(RoadNetwork, EndpointsWithinTheWeldRadiusBecomeOneNode)
    {
        // Two streets that were meant to meet and miss by a centimetre leave a hole a driver falls
        // into, and the hole is invisible: the roads still touch on screen. A triangle is the
        // smallest figure that survives dead-end pruning, so the gap is put at one of its corners
        // -- if it does not weld, the whole triangle is dangling and is pruned to nothing.
        RoadNetwork roads;
        roads.AddSegment(Vec2(-50.0f, 0.0f), Vec2(0.0f, 60.0f), RoadClass::Local);
        roads.AddSegment(Vec2(0.01f, 60.0f), Vec2(50.0f, 0.0f), RoadClass::Local);
        roads.AddSegment(Vec2(50.0f, 0.0f), Vec2(-50.0f, 0.0f), RoadClass::Local);
        roads.Build(0.35f);

        ASSERT_EQ(roads.nodes().size(), 3u) << "the two ends did not weld and the triangle was pruned";
        EXPECT_EQ(roads.segments().size(), 3u);
        const std::uint32_t joint = roads.FindNearestNode(Vec2(0.0f, 60.0f));
        ASSERT_NE(joint, 0xFFFFFFFFu);
        EXPECT_EQ(roads.incidenceCount(joint), 2u);
    }

    TEST(RoadNetwork, EveryIncidenceIsMirroredByItsNeighbour)
    {
        City city;
        city.Generate(SmallCityConfig());
        const RoadNetwork& roads = city.roads();
        ASSERT_GT(roads.nodes().size(), 50u);

        // An undirected graph stored as per-node incidence lists is only undirected if both ends
        // agree. A one-way edge here is a route the planner finds and the driver cannot take.
        for (std::uint32_t n = 0; n < roads.nodes().size(); ++n)
            for (std::size_t k = 0; k < roads.incidenceCount(n); ++k)
            {
                const Incidence& inc = roads.incidenceBegin(n)[k];
                ASSERT_LT(inc.other, roads.nodes().size());
                EXPECT_EQ(roads.FindSegmentBetween(inc.other, n), inc.segment)
                    << "node " << n << " lists " << inc.other << " but not the reverse";
            }
    }

    TEST(RoadNetwork, EverySegmentsDirectionMatchesItsEndpoints)
    {
        City city;
        city.Generate(SmallCityConfig());
        const RoadNetwork& roads = city.roads();
        for (const RoadSegment& segment : roads.segments())
        {
            const Vec2 a = roads.nodes()[segment.nodeA].position;
            const Vec2 b = roads.nodes()[segment.nodeB].position;
            ASSERT_GT(segment.length, 0.0f);
            EXPECT_NEAR(segment.length, Distance(a, b), 0.05f);
            // The cached unit direction is what places every vehicle on the road. When it
            // disagrees with the endpoints, the cars drive beside the carriageway.
            const Vec2 expected = Normalized(b - a);
            EXPECT_NEAR(segment.direction.X, expected.X, 0.01f);
            EXPECT_NEAR(segment.direction.Y, expected.Y, 0.01f);
        }
    }

    TEST(RoadNetwork, NoNodeIsADeadEndOfNothing)
    {
        City city;
        city.Generate(SmallCityConfig());
        const RoadNetwork& roads = city.roads();
        for (std::uint32_t n = 0; n < roads.nodes().size(); ++n)
            EXPECT_GT(roads.incidenceCount(n), 0u)
                << "node " << n << " is in the graph and joined to nothing";
    }

    TEST(RoadNetwork, FindNearestNodeReallyReturnsTheNearest)
    {
        // It is a widening-ring grid search rather than a scan, and a ring search that stops one
        // ring early is right almost always -- which is the worst kind of wrong.
        City city;
        city.Generate(SmallCityConfig());
        const RoadNetwork& roads = city.roads();
        Rng rng(99);
        for (int trial = 0; trial < 200; ++trial)
        {
            const Vec2 point(rng.NextFloat(-700.0f, 700.0f), rng.NextFloat(-700.0f, 700.0f));
            const std::uint32_t found = roads.FindNearestNode(point);
            ASSERT_NE(found, 0xFFFFFFFFu);
            const float best = Distance(roads.nodes()[found].position, point);
            float trueBest = 1e30f;
            for (const RoadNode& node : roads.nodes())
                trueBest = std::min(trueBest, Distance(node.position, point));
            EXPECT_NEAR(best, trueBest, 0.01f) << "the ring search stopped early at " << point.X
                                               << "," << point.Y;
        }
    }

    TEST(RoadNetwork, BlocksAreBoundedAndPlausiblySized)
    {
        // The district-street defect: streets that stopped 0.6 m short of the arterials left each
        // grid an island, so the half-edge walk returned annuli instead of blocks -- 393 of them,
        // each enormous and self-touching, totalling more area than the city has.
        City city;
        city.Generate(SmallCityConfig());
        const RoadNetwork& roads = city.roads();
        ASSERT_GT(roads.blocks().size(), 10u);

        const CityConfig& config = city.config();
        const float cityArea = config.halfSize * config.halfSize * 4.0f;
        float total = 0.0f;
        for (const CityBlock& block : roads.blocks())
        {
            ASSERT_GE(block.outline.size(), 3u);
            float area = 0.0f;
            for (std::size_t i = 0; i < block.outline.size(); ++i)
            {
                const Vec2& p = block.outline[i];
                const Vec2& q = block.outline[(i + 1) % block.outline.size()];
                area += p.X * q.Y - q.X * p.Y;
            }
            area = std::abs(area) * 0.5f;
            EXPECT_LT(area, cityArea * 0.25f) << "a single block covers a quarter of the city";
            total += area;
        }
        EXPECT_LT(total, cityArea)
            << "the blocks cover more ground than the city has; they are overlapping annuli";
    }
}
