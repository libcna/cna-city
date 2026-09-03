// SPDX-License-Identifier: MIT
//
// The program claims determinism, and the claim is load-bearing: the benchmark's scaling curve is
// only a curve if each scale runs the same city, and a defect is only reproducible if the run can
// be repeated. It has been broken once already, silently -- per-agent decisions hashed on the tick
// counter, so a machine drawing at 120 fps stepped twice as often, hashed twice as many times and
// produced a different set of trips from the same seed. Decisions hash on the simulated clock now.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        /**
         * @brief A cheap order-sensitive digest of everything that moves.
         *
         * Positions are quantised to a centimetre before hashing. Bit-exact float comparison would
         * make this test fail on a compiler flag rather than on a defect, and a centimetre is two
         * orders of magnitude below anything the simulation decides on.
         */
        std::uint64_t StateChecksum(const Simulation& sim)
        {
            std::uint64_t h = 1469598103934665603ULL;
            const auto mix = [&h](std::uint64_t value) {
                h ^= value;
                h *= 1099511628211ULL;
            };
            const auto mixFloat = [&mix](float value) {
                mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(value * 100.0f))));
            };

            const Agents& agents = sim.agents();
            for (std::size_t i = 0; i < agents.size(); ++i)
            {
                mixFloat(agents.position[i].X);
                mixFloat(agents.position[i].Y);
                mix(agents.mode[i]);
                mix(agents.activity[i]);
            }
            for (const Vehicle& vehicle : sim.traffic().vehicles())
            {
                mix(vehicle.active);
                if (!vehicle.active) continue;
                mix(vehicle.segment);
                mixFloat(vehicle.s);
            }
            for (const MetroTrain& train : sim.metro().trains()) mixFloat(train.position);
            for (const Bus& bus : sim.buses().buses()) mixFloat(bus.position);
            return h;
        }

        std::uint64_t RunAndDigest(const SimConfig& config, float totalSeconds, float sliceSeconds)
        {
            Simulation sim;
            sim.Initialize(config);
            const int slices = static_cast<int>(totalSeconds / sliceSeconds);
            for (int i = 0; i < slices; ++i) sim.Step(sliceSeconds);
            return StateChecksum(sim);
        }
    }

    TEST(Determinism, TheSameSeedAndTheSameStepsGiveTheSameCity)
    {
        const SimConfig config = SmallSimConfig(3000, 777);
        EXPECT_EQ(RunAndDigest(config, 1800.0f, 1.0f), RunAndDigest(config, 1800.0f, 1.0f));
    }

    TEST(Determinism, TheFrameRateDoesNotChangeTheCity)
    {
        // The defect: hashing per-agent decisions on the tick counter made the whole day depend on
        // how fast the machine drew. Half-second slices and one-second slices are 120 fps and
        // 60 fps at the same time scale, and must produce the same day.
        const SimConfig config = SmallSimConfig(2500, 5150);
        const std::uint64_t atSixty = RunAndDigest(config, 1200.0f, 1.0f);
        const std::uint64_t atOneTwenty = RunAndDigest(config, 1200.0f, 0.5f);
        EXPECT_EQ(atSixty, atOneTwenty)
            << "the same seed produced a different city at a different frame rate";
    }

    TEST(Determinism, TheWorkerThreadCountDoesNotChangeTheCity)
    {
        // The parallel half moves the people who are outdoors, and it is only safe to parallelise
        // because each agent writes only its own slot. A shared write would show up here and
        // nowhere else.
        SimConfig one = SmallSimConfig(3000, 8080);
        one.threads = 1;
        SimConfig many = one;
        many.threads = 8;
        EXPECT_EQ(RunAndDigest(one, 900.0f, 1.0f), RunAndDigest(many, 900.0f, 1.0f))
            << "the city depends on how many worker threads it was given";
    }

    // There is deliberately no unit test here for the arrival-queue ordering defect, and the
    // reason is worth writing down rather than leaving as an omission.
    //
    // The list of citizens who arrived somewhere this tick is filled in parallel with an atomic
    // increment, so its order was whichever worker finished first -- and an arrival joins the back
    // of a platform queue that a train drains from the front. Comparing two runs only catches that
    // when the scheduling happens to differ *at a moment that changes an outcome*, which needs a
    // full train or a full bus. Measured: at fifty thousand citizens over ninety simulated minutes
    // the comparison caught it three times in six; at twenty and thirty thousand, funnelled
    // through a single metro line and a single bus route, not once in twelve.
    //
    // A test that passes half the time when the bug is present is worse than no test, because it
    // teaches people that a failure is noise. The guard for this class lives at full scale
    // instead, where the race is reliable:
    //
    //     cna-city --seed 42 --agents 100000 --simulate 8h --checksum
    //
    // which reproduces at half the step size and on a different number of worker threads, and
    // said NO to both while the defect was in. That is minutes rather than seconds, so it belongs
    // in CI rather than in this file -- see the Tests section of README.md.

    TEST(Determinism, ADifferentSeedGivesADifferentCity)
    {
        EXPECT_NE(RunAndDigest(SmallSimConfig(2000, 11), 600.0f, 1.0f),
                  RunAndDigest(SmallSimConfig(2000, 12), 600.0f, 1.0f));
    }

    TEST(Determinism, TheClockAdvancesWithTheSimulatedTimeRegardlessOfSliceSize)
    {
        Simulation coarse;
        coarse.Initialize(SmallSimConfig(200));
        Simulation fine;
        fine.Initialize(SmallSimConfig(200));
        for (int i = 0; i < 300; ++i) coarse.Step(2.0f);
        for (int i = 0; i < 1200; ++i) fine.Step(0.5f);
        EXPECT_NEAR(coarse.clock().hour(), fine.clock().hour(), 1e-4f);
        EXPECT_EQ(coarse.clock().day(), fine.clock().day());
    }

    TEST(Determinism, AStepBeyondTheSubStepCeilingDropsTheSurplusRatherThanBankingIt)
    {
        // The ceiling is kMovementStep * kMaxSubSteps -- five simulated seconds a call, which at
        // 60 fps is a time scale of 300, well past the 60 the demo runs at. Beyond it the surplus
        // is dropped rather than paid off over the following frames, because banking it turns one
        // slow frame into a slow minute. That means a caller asking for more than the ceiling gets
        // less world than it asked for, and this is where that is written down.
        Simulation sim;
        sim.Initialize(SmallSimConfig(200));
        const float ceiling = Simulation::kMovementStep * Simulation::kMaxSubSteps;
        sim.Step(60.0f);
        EXPECT_NEAR(sim.clock().hour(), SmallSimConfig(200).startHour + ceiling / 3600.0f, 1e-4f);
    }
}
