// SPDX-License-Identifier: MIT
//
// The daily routine, and the three defects that made it stop working without making it look
// broken. All three made the program *faster*, which is the specific reason a benchmark needs
// these tests more than an ordinary program does.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        std::uint32_t CountActivity(const Simulation& sim, Activity activity)
        {
            return sim.stats().activityCount[static_cast<int>(activity)];
        }
    }

    TEST(Population, EverybodyGetsAHomeAJobOrASchoolAndASchedule)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(3000));
        const Agents& agents = sim.agents();
        ASSERT_EQ(agents.size(), 3000u);
        for (std::size_t i = 0; i < agents.size(); ++i)
        {
            EXPECT_NE(agents.home[i], kNoIndex) << "citizen " << i << " has nowhere to live";
            EXPECT_LT(agents.leaveHomeMinute[i], 24u * 60u);
            EXPECT_LT(agents.bedMinute[i], 24u * 60u);
            EXPECT_GT(agents.desiredSpeed[i], 0.3f);
            EXPECT_LT(agents.desiredSpeed[i], 3.0f);
        }
    }

    TEST(DailyRoutine, MostOfTheCityLeavesHomeDuringTheMorningPeak)
    {
        // The defect this is named for: a decision pass offered an eighth of the population and
        // planned a few hundred, but set the "already commuted today" bit on every candidate it
        // offered. 99 000 of 100 000 citizens were still at home at half past eight, and stayed
        // there all day. Nothing about the frame looked wrong -- there were simply few people out.
        SimConfig config = SmallSimConfig(3000);
        config.startHour = 6.0f;
        Simulation sim;
        sim.Initialize(config);
        RunFor(sim, 4.0f * 3600.0f, 2.0f);   // 06:00 to 10:00

        const std::uint32_t atWork = CountActivity(sim, Activity::AtWork);
        const std::uint32_t atHome = CountActivity(sim, Activity::AtHome);
        EXPECT_GT(atWork, sim.agents().size() / 5)
            << "only " << atWork << " of " << sim.agents().size() << " citizens reached work";
        EXPECT_LT(atHome, sim.agents().size() * 4 / 5);
    }

    TEST(DailyRoutine, PedestriansActuallyArriveAtALargeTimeScale)
    {
        // At a time scale of 180 a frame is six simulated seconds, a walker covers eight metres in
        // one step, and a waypoint with a three-metre arrival radius is never reached. Thirteen
        // thousand citizens oscillated across their last junction forever at a perfectly healthy
        // 1.3 m/s, and the walking population never went down. Movement is sub-stepped now, and
        // this test is the sub-stepping: it takes the same simulated time in six-second bites.
        SimConfig config = SmallSimConfig(2500);
        config.startHour = 6.5f;
        Simulation sim;
        sim.Initialize(config);

        for (int i = 0; i < 600; ++i) sim.Step(6.0f);   // one simulated hour, six seconds a call
        const std::uint32_t walkingAtPeak = sim.stats().walking;
        EXPECT_GT(sim.stats().subSteps, 1) << "a six-second step was not sub-stepped at all";

        for (int i = 0; i < 1200; ++i) sim.Step(6.0f);  // two more hours
        EXPECT_GT(CountActivity(sim, Activity::AtWork), 0u)
            << "nobody arrived anywhere in three simulated hours of six-second steps";
        EXPECT_LT(sim.stats().walking, walkingAtPeak * 3u)
            << "the walking population only grows; walkers are not arriving";
    }

    TEST(DailyRoutine, TheDecisionStrideReachesEveryAgent)
    {
        // The stride used to cycle on the tick counter, which was correct only while decisions ran
        // on every tick. Once they moved onto simulated time the sequence became 0, 2, 4, 6 and
        // the four odd strides were never selected again: half the city stopped deciding anything,
        // and the population on foot at the peak fell from six thousand to two. That reads as
        // tuning.
        SimConfig config = SmallSimConfig(2000);
        config.startHour = 6.0f;
        Simulation sim;
        sim.Initialize(config);

        std::vector<std::uint8_t> everMoved(sim.agents().size(), 0);
        for (int i = 0; i < 3000; ++i)
        {
            sim.Step(4.0f);
            for (std::size_t a = 0; a < sim.agents().size(); ++a)
                if (sim.agents().mode[a] != static_cast<std::uint8_t>(Mode::Indoors))
                    everMoved[a] = 1;
        }
        const std::size_t moved = static_cast<std::size_t>(
            std::count(everMoved.begin(), everMoved.end(), std::uint8_t{1}));
        EXPECT_GT(moved, sim.agents().size() / 2)
            << "only " << moved << " of " << sim.agents().size()
            << " citizens ever left a building; a decision stride is skipping agents";
    }

    TEST(DailyRoutine, TheActivityHistogramAlwaysAccountsForEverybody)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(1500));
        for (int i = 0; i < 400; ++i)
        {
            sim.Step(5.0f);
            std::uint32_t total = 0;
            for (int a = 0; a < kActivityCount; ++a) total += sim.stats().activityCount[a];
            ASSERT_EQ(total, sim.agents().size()) << "an agent is in no activity at all";
        }
    }

    TEST(DailyRoutine, TheModeCountsAlwaysAddUpToThePopulation)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig(1500));
        for (int i = 0; i < 400; ++i)
        {
            sim.Step(5.0f);
            const SimStats& s = sim.stats();
            ASSERT_EQ(s.indoors + s.walking + s.driving + s.waitingTrain + s.riding + s.waitingBus +
                          s.onBus,
                      sim.agents().size())
                << "an agent is in no mode; a transition dropped them";
        }
    }

    TEST(DailyRoutine, NobodyLeavesTheMapOrStopsBeingFinite)
    {
        // Cheap, and it catches a whole class of steering and integration bug at once: a NaN in a
        // position propagates silently through the crowd grid and only appears as agents missing
        // from the screen.
        SimConfig config = SmallSimConfig(2000);
        Simulation sim;
        sim.Initialize(config);
        const float limit = config.city.halfSize * 4.0f;
        for (int i = 0; i < 900; ++i)
        {
            sim.Step(4.0f);
            for (std::uint32_t a : sim.walkingAgents())
            {
                const Vec2 p = sim.agents().position[a];
                ASSERT_TRUE(std::isfinite(p.X) && std::isfinite(p.Y)) << "agent " << a << " is NaN";
                ASSERT_LT(std::abs(p.X), limit);
                ASSERT_LT(std::abs(p.Y), limit);
            }
        }
    }
}
