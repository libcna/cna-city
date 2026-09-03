// SPDX-License-Identifier: MIT
//
// The invariants a long run asserts, and the machinery that reads a trend out of one.
//
// Half of these deliberately break the world first. A checker that has only ever been shown
// passing is a checker nobody should believe: the failure this project most wants to catch is a
// route slot quietly leaking one per morning, and if the check for that were inverted, or
// comparing a count against itself, every clean run would still say "ok". So each invariant is
// tested twice -- once on a healthy city, and once on a city with exactly one thing wrong with it.
//
// Breaking it means writing through a const_cast. That is legal here, because nothing involved was
// declared const -- the Simulation is a plain local and its members are non-const -- and it is the
// honest way to test a checker whose whole job is to notice states no correct code can produce.

#include <vector>

#include "Checksum.hpp"
#include "Soak.hpp"
#include "TestSupport.hpp"

namespace CnaCityTests
{
    using CnaCity::CheckInvariants;
    using CnaCity::DriftPerDay;
    using CnaCity::LeastSquaresSlope;
    using CnaCity::PipelinedStepper;
    using CnaCity::Violation;

    namespace
    {
        /** @brief A city with a morning's worth of people in flight, so the checks have something
         *  to check. An empty city passes every invariant trivially. */
        void Warm(Simulation& sim, float seconds = 900.0f)
        {
            sim.Initialize(SmallSimConfig());
            RunFor(sim, seconds, 2.0f);
        }

        /** @brief The one agent index that is doing @p mode, or kNoIndex. */
        std::uint32_t FindByMode(const Simulation& sim, Mode mode)
        {
            for (std::uint32_t i = 0; i < sim.agents().size(); ++i)
                if (sim.agents().mode[i] == static_cast<std::uint8_t>(mode)) return i;
            return kNoIndex;
        }

        Agents& Mutable(const Simulation& sim)
        {
            return const_cast<Agents&>(sim.agents());
        }

        std::size_t Violations(const Simulation& sim)
        {
            std::vector<Violation> found;
            return CheckInvariants(sim, found);
        }
    }

    // --- The healthy city ------------------------------------------------------------------------

    TEST(SoakInvariantTest, AHealthyCityViolatesNothingThroughTheDay)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig());
        std::vector<Violation> found;

        // Every hour of a whole simulated day, including the two that break things: the morning
        // peak, when everything is being allocated at once, and the small hours, when it all has
        // to have been given back.
        for (int hour = 0; hour < 24; ++hour)
        {
            RunFor(sim, 3600.0f, 2.0f);
            CheckInvariants(sim, found);
            ASSERT_TRUE(found.empty())
                << "at " << sim.clock().hour() << " h on day " << sim.clock().day() << ": "
                << found.front().what;
        }
    }

    TEST(SoakInvariantTest, AFreshCityViolatesNothingBeforeAnybodyMoves)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig());
        EXPECT_EQ(Violations(sim), 0u);
    }

    // --- One thing wrong at a time ---------------------------------------------------------------

    TEST(SoakInvariantTest, ANonFinitePositionIsCaught)
    {
        Simulation sim;
        Warm(sim);
        const std::uint32_t walker = FindByMode(sim, Mode::Walking);
        ASSERT_NE(walker, kNoIndex) << "nobody was walking, so there was nothing to corrupt";

        Mutable(sim).position[walker].X = std::numeric_limits<float>::quiet_NaN();
        std::vector<Violation> found;
        ASSERT_EQ(CheckInvariants(sim, found), 1u);
        EXPECT_NE(found.front().what.find("non-finite"), std::string::npos) << found.front().what;
    }

    TEST(SoakInvariantTest, ACitizenWhoLeavesTheCityIsCaught)
    {
        Simulation sim;
        Warm(sim);
        const std::uint32_t walker = FindByMode(sim, Mode::Walking);
        ASSERT_NE(walker, kNoIndex);

        Mutable(sim).position[walker].X = 1.0e6f;
        std::vector<Violation> found;
        ASSERT_EQ(CheckInvariants(sim, found), 1u);
        EXPECT_NE(found.front().what.find("outside the city"), std::string::npos)
            << found.front().what;
    }

    TEST(SoakInvariantTest, SomebodyIndoorsStillHoldingACarIsCaught)
    {
        Simulation sim;
        Warm(sim);
        const std::uint32_t indoors = FindByMode(sim, Mode::Indoors);
        ASSERT_NE(indoors, kNoIndex);

        // The shape of a real leak: the trip ended, the mode went back to Indoors, and the vehicle
        // was never despawned. Nothing else in the city looks wrong -- the car simply drives on
        // with a driver who is at home.
        Mutable(sim).vehicle[indoors] = 0;
        std::vector<Violation> found;
        ASSERT_GE(CheckInvariants(sim, found), 1u);
        EXPECT_NE(found.front().what.find("owns vehicle"), std::string::npos) << found.front().what;
    }

    TEST(SoakInvariantTest, TwoCitizensOnOneRouteSlotIsCaught)
    {
        Simulation sim;
        Warm(sim);
        std::uint32_t first = kNoIndex, second = kNoIndex;
        for (std::uint32_t i = 0; i < sim.agents().size(); ++i)
        {
            if (sim.agents().pathSlot[i] == kNoIndex) continue;
            if (first == kNoIndex) first = i;
            else { second = i; break; }
        }
        ASSERT_NE(second, kNoIndex) << "fewer than two routes were in flight";

        Mutable(sim).pathSlot[second] = sim.agents().pathSlot[first];
        std::vector<Violation> found;
        ASSERT_GE(CheckInvariants(sim, found), 1u);
        EXPECT_NE(found.front().what.find("held by two agents"), std::string::npos)
            << found.front().what;
    }

    TEST(SoakInvariantTest, ARouteSlotNobodyHoldsIsCaught)
    {
        Simulation sim;
        Warm(sim);

        // Exactly the zombie the soak exists to find: acquired, never released, and invisible in
        // every other number the simulation reports.
        auto& pool = const_cast<RoutePool&>(sim.routes());
        const std::uint32_t leaked = pool.Acquire();
        ASSERT_NE(leaked, kNoIndex);

        std::vector<Violation> found;
        ASSERT_EQ(CheckInvariants(sim, found), 1u);
        EXPECT_NE(found.front().what.find("are lost"), std::string::npos) << found.front().what;

        pool.Release(leaked);
        EXPECT_EQ(Violations(sim), 0u) << "releasing the slot should make the city clean again";
    }

    TEST(SoakInvariantTest, AnOverloadedBusIsCaught)
    {
        Simulation sim;
        Warm(sim);
        auto& buses = const_cast<BusNetwork&>(sim.buses()).mutableBuses();
        ASSERT_FALSE(buses.empty());
        buses[0].onboard = buses[0].capacity + 1;

        std::vector<Violation> found;
        ASSERT_GE(CheckInvariants(sim, found), 1u);
        EXPECT_NE(found.front().what.find("seats"), std::string::npos) << found.front().what;
    }

    TEST(SoakInvariantTest, PassengersCountedFromBothEndsMustAgree)
    {
        Simulation sim;
        Warm(sim, 2400.0f);
        auto& buses = const_cast<BusNetwork&>(sim.buses()).mutableBuses();
        ASSERT_FALSE(buses.empty());

        // One passenger appears on a bus without being one. Both counts stay individually
        // plausible; only comparing them finds it.
        buses[0].onboard += 1;
        std::vector<Violation> found;
        ASSERT_EQ(CheckInvariants(sim, found), 1u);
        EXPECT_NE(found.front().what.find("citizens are aboard one"), std::string::npos)
            << found.front().what;
    }

    TEST(SoakInvariantTest, TheViolationLimitStopsTheFlood)
    {
        Simulation sim;
        Warm(sim);

        // One broken invariant tends to break a hundred thousand times, and a report that is a
        // hundred thousand identical lines is a report nobody reads to the end.
        Agents& agents = Mutable(sim);
        for (std::uint32_t i = 0; i < agents.size(); ++i)
            agents.position[i].Y = std::numeric_limits<float>::infinity();

        std::vector<Violation> found;
        EXPECT_EQ(CheckInvariants(sim, found, 5), 5u);
        EXPECT_EQ(found.size(), 5u);
    }

    TEST(SoakInvariantTest, ViolationsAreAppendedRatherThanReplaced)
    {
        Simulation sim;
        Warm(sim);
        std::vector<Violation> found;
        found.push_back(Violation{"something from earlier", 0, 0, 0.0f});

        Mutable(sim).position[FindByMode(sim, Mode::Walking)].X =
            std::numeric_limits<float>::quiet_NaN();
        EXPECT_EQ(CheckInvariants(sim, found), 1u);
        ASSERT_EQ(found.size(), 2u);
        EXPECT_EQ(found.front().what, "something from earlier");
    }

    TEST(SoakInvariantTest, AViolationSaysWhenItHappened)
    {
        Simulation sim;
        Warm(sim, 3600.0f);
        Mutable(sim).position[FindByMode(sim, Mode::Walking)].X =
            std::numeric_limits<float>::quiet_NaN();

        std::vector<Violation> found;
        ASSERT_EQ(CheckInvariants(sim, found), 1u);
        EXPECT_EQ(found.front().tick, sim.tick());
        EXPECT_FLOAT_EQ(found.front().hour, sim.clock().hour());
        EXPECT_EQ(found.front().day, sim.clock().day());
    }

    // --- The frame model is not part of the simulation ---------------------------------------------

    TEST(SoakPipelineTest, PipeliningDoesNotChangeTheSimulation)
    {
        // The claim `--frame-model pipelined` rests on. It has to be checked rather than reasoned
        // about: the step runs on another thread while the frame works from its captured copies,
        // and "the draw only reads" was true of the code right up until it was not.
        Simulation serial;
        Simulation pipelined;
        serial.Initialize(SmallSimConfig());
        pipelined.Initialize(SmallSimConfig());
        PipelinedStepper stepper;

        for (int i = 0; i < 900; ++i)
        {
            serial.Step(2.0f);
            stepper.Step(pipelined, 2.0f);
            if ((i + 1) % 150 != 0) continue;
            const WorldChecksum mine = ComputeChecksum(serial);
            const WorldChecksum theirs = ComputeChecksum(pipelined);
            ASSERT_EQ(mine.total, theirs.total)
                << "the two frame models diverged after " << (i + 1) << " ticks -- agents "
                << ToHex(mine.agents) << " vs " << ToHex(theirs.agents) << ", traffic "
                << ToHex(mine.traffic) << " vs " << ToHex(theirs.traffic);
        }
    }

    TEST(SoakPipelineTest, APipelinedWorldPassesTheSameInvariants)
    {
        Simulation sim;
        sim.Initialize(SmallSimConfig());
        PipelinedStepper stepper;
        std::vector<Violation> found;
        for (int hour = 0; hour < 6; ++hour)
        {
            for (int i = 0; i < 1800; ++i) stepper.Step(sim, 2.0f);
            CheckInvariants(sim, found);
            ASSERT_TRUE(found.empty()) << found.front().what;
        }
    }

    // --- Reading a trend out of a noisy signal -----------------------------------------------------

    TEST(SoakTrendTest, AFlatLineHasNoGradient)
    {
        EXPECT_NEAR(LeastSquaresSlope({5.0, 5.0, 5.0, 5.0, 5.0, 5.0}), 0.0, 1e-12);
    }

    TEST(SoakTrendTest, AStraightLineGivesItsOwnGradient)
    {
        EXPECT_NEAR(LeastSquaresSlope({1.0, 3.0, 5.0, 7.0, 9.0}), 2.0, 1e-12);
        EXPECT_NEAR(LeastSquaresSlope({9.0, 7.0, 5.0, 3.0, 1.0}), -2.0, 1e-12);
    }

    namespace
    {
        /** @brief Three days of a quantity that swings daily and drifts by @p perDay. */
        std::vector<double> DailyCycle(double perDay, double phaseHours = 0.0)
        {
            std::vector<double> series;
            for (int i = 0; i < 72; ++i)
            {
                const double t = (static_cast<double>(i) + phaseHours) * 3.14159265358979 / 12.0;
                series.push_back(1000.0 + 400.0 * std::sin(t) +
                                 perDay * static_cast<double>(i) / 24.0);
            }
            return series;
        }
    }

    TEST(SoakTrendTest, ARawGradientIsFooledByTheDailyCycle)
    {
        // Kept as a test because it is the reason DriftPerDay exists. A pure daily swing with no
        // drift at all has a large least-squares gradient: whole periods cancel in the mean and
        // not in the covariance with time. A leak detector built on the raw gradient would call
        // this city a leak, and would have missed a real one of the opposite sign.
        EXPECT_GT(std::abs(LeastSquaresSlope(DailyCycle(0.0))), 3.0);
    }

    TEST(SoakTrendTest, ADailySwingHidesNoLeakAndInventsNone)
    {
        // The swing must cancel and the drift must survive, whatever hour the run started at --
        // otherwise the test either misses every leak or reports one every night.
        for (double phase : {0.0, 6.0, 6.5, 13.0, 19.25})
        {
            EXPECT_NEAR(DriftPerDay(DailyCycle(0.0, phase)), 0.0, 1e-9) << "phase " << phase;
            EXPECT_NEAR(DriftPerDay(DailyCycle(12.0, phase)), 12.0, 1e-9) << "phase " << phase;
            EXPECT_NEAR(DriftPerDay(DailyCycle(-12.0, phase)), -12.0, 1e-9) << "phase " << phase;
        }
    }

    TEST(SoakTrendTest, ADriftWantsTwoWholeDays)
    {
        // A refusal to answer, not an answer of "no drift". Anything shorter would have the
        // cycle's own gradient in it, which is what the previous test measures at three per hour.
        std::vector<double> series = DailyCycle(12.0);
        EXPECT_EQ(DriftPerDay(std::vector<double>(series.begin(), series.begin() + 47)), 0.0);
        EXPECT_NEAR(DriftPerDay(std::vector<double>(series.begin(), series.begin() + 48)), 12.0,
                    1e-9);
        // A trailing part-day is dropped rather than allowed to weight the hours it contains.
        EXPECT_NEAR(DriftPerDay(std::vector<double>(series.begin(), series.begin() + 60)), 12.0,
                    1e-9);
    }

    TEST(SoakTrendTest, TooFewPointsIsNotAGradient)
    {
        EXPECT_EQ(LeastSquaresSlope({}), 0.0);
        EXPECT_EQ(LeastSquaresSlope({1.0}), 0.0);
        EXPECT_EQ(LeastSquaresSlope({1.0, 100.0}), 0.0);
    }
    // --- What the first soak run found -------------------------------------------------------

    TEST(SoakRegressionTest, BusesNeverEndUpInTheSamePlace)
    {
        // A gap-based follower model cannot resolve two vehicles at one point: each reads the
        // other as zero metres ahead, both hold at a standstill, and nothing in the model is
        // asymmetric enough to break the tie. The first soak run found the consequence rather
        // than the cause -- a small city with the command line's default fourteen bus routes
        // finished the day with whole routes stacked on a single metre of road and fifteen
        // passengers aboard who never arrived anywhere, while every other check still passed.
        //
        // This is the default configuration with --size 620, which is deliberately oversubscribed:
        // ninety-three buses over twenty stops. That is the point. A benchmark is run at
        // configurations nobody designed for, and "the fleet deadlocks" is not an acceptable
        // answer at any of them.
        SimConfig config;
        config.city.halfSize = 620.0f;
        config.agentCount = 3000;
        config.metroLines = 3;
        config.threads = 4;

        Simulation sim;
        sim.Initialize(config);
        std::vector<Violation> found;
        for (int hour = 0; hour < 6; ++hour)
        {
            RunFor(sim, 3600.0f, 2.0f);
            CheckInvariants(sim, found);
            ASSERT_TRUE(found.empty()) << "after " << (hour + 1) << " simulated hours: "
                                       << found.front().what;
        }
    }

    TEST(SoakRegressionTest, NobodyIsStillOnABusInTheSmallHours)
    {
        // The outcome the deadlock produced, asserted directly. Everybody who boarded during the
        // day has to have got off by the middle of the night -- not most of them, all of them.
        //
        // The city here is the ordinary one, not the oversubscribed one above. Ninety-three buses
        // over the twenty stops of a 1.2 km city is a fine place to insist that no two of them
        // end up in the same spot, and not a fair place to insist the network still delivers
        // everybody: it is saturated by construction. See plan.md P21 for what that configuration
        // still does and what is known about it.
        SimConfig config;
        config.agentCount = 2000;
        config.metroLines = 3;
        config.threads = 4;

        Simulation sim;
        sim.Initialize(config);
        RunFor(sim, 20.5f * 3600.0f, 2.0f);   // 06:30 to 03:00 the following night.

        EXPECT_NEAR(sim.clock().hour(), 3.0f, 0.1f);
        EXPECT_EQ(sim.stats().onBus, 0u) << "citizens are still riding a bus at three in the "
                                            "morning";
        EXPECT_EQ(sim.stats().riding, 0u) << "citizens are still on a train at three in the "
                                             "morning";
        std::uint64_t seatsTaken = 0;
        for (const Bus& bus : sim.buses().buses()) seatsTaken += bus.onboard;
        EXPECT_EQ(seatsTaken, 0u) << "the buses still believe they are carrying somebody";
        EXPECT_EQ(sim.routes().inUse(), 0u) << "route slots are still out overnight";
    }

}
