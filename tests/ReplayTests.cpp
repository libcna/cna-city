// SPDX-License-Identifier: MIT
//
// Checksums and replay. These are the machinery that turns "the same seed is the same city" from a
// sentence in the README into something a script can check, and the reason they exist is that the
// sentence was false in four separate ways until somebody looked.
//
// A replay holds almost nothing -- the seed, the configuration, how many ticks ran, and the few
// moments somebody pressed a key -- because the simulation is a pure function of those. A day of a
// hundred thousand citizens is about a kilobyte.

#include <cstdio>
#include <filesystem>

#include "Checksum.hpp"
#include "Replay.hpp"
#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        std::string TempPath(const char* name)
        {
            const std::filesystem::path directory =
                std::filesystem::temp_directory_path() / "cna-city-tests";
            std::filesystem::create_directories(directory);
            return (directory / name).string();
        }

        void RunSlices(Simulation& sim, const SimConfig& config, float seconds, float slice)
        {
            sim.Initialize(config);
            const int steps = static_cast<int>(seconds / slice);
            for (int i = 0; i < steps; ++i) sim.Step(slice);
        }
    }

    // --- The digest itself -------------------------------------------------------------------

    TEST(Checksum, HexRoundTrips)
    {
        for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1},
                                          std::uint64_t{0xDEADBEEFCAFEBABEULL},
                                          ~std::uint64_t{0}})
        {
            const std::string text = ToHex(value);
            EXPECT_EQ(text.size(), 16u);
            std::uint64_t parsed = 0;
            ASSERT_TRUE(FromHex(text, parsed));
            EXPECT_EQ(parsed, value);
        }
        std::uint64_t ignored = 0;
        EXPECT_FALSE(FromHex("", ignored));
        EXPECT_FALSE(FromHex("nothexadecimal!!", ignored));
        EXPECT_FALSE(FromHex("abc", ignored));
    }

    TEST(Checksum, TheCityHalfDependsOnTheSeedAndNothingElse)
    {
        // The static city is generated before a single tick runs, so its digest must not move with
        // the population, the clock or the thread count. When it does, the generator has changed
        // and every other comparison downstream of it is meaningless -- which is why the replay
        // player checks this one before it steps at all.
        SimConfig a = SmallSimConfig(1000, 4242);
        SimConfig b = SmallSimConfig(9000, 4242);
        b.startHour = 19.0f;
        b.threads = 4;
        Simulation first;
        Simulation second;
        first.Initialize(a);
        second.Initialize(b);
        EXPECT_EQ(ComputeCityChecksum(first), ComputeCityChecksum(second));

        Simulation other;
        other.Initialize(SmallSimConfig(1000, 4243));
        EXPECT_NE(ComputeCityChecksum(first), ComputeCityChecksum(other));
    }

    TEST(Checksum, EveryComponentMovesWhenTheWorldDoes)
    {
        // A digest that never changes passes every reproduction test ever written.
        Simulation sim;
        sim.Initialize(SmallSimConfig(4000));
        const WorldChecksum start = ComputeChecksum(sim);
        RunFor(sim, 1800.0f, 2.0f);
        const WorldChecksum later = ComputeChecksum(sim);

        EXPECT_EQ(start.city, later.city) << "the static city moved during the simulation";
        EXPECT_NE(start.agents, later.agents);
        EXPECT_NE(start.transit, later.transit);
        EXPECT_NE(start.world, later.world) << "the clock did not move";
        EXPECT_NE(start.total, later.total);
    }

    TEST(Checksum, TheSameRunGivesTheSameDigest)
    {
        const SimConfig config = SmallSimConfig(3000, 6161);
        Simulation a;
        Simulation b;
        RunSlices(a, config, 900.0f, 1.0f);
        RunSlices(b, config, 900.0f, 1.0f);
        EXPECT_EQ(ComputeChecksum(a), ComputeChecksum(b));
    }

    // --- The file ----------------------------------------------------------------------------

    TEST(Replay, AFileRoundTripsThroughDisk)
    {
        ReplayFile written;
        written.config = SmallSimConfig(1234, 99);
        written.ticks = 4321;
        written.events.push_back(ReplayEvent{100, ReplayEvent::Kind::Weather, 3.0f});
        written.events.push_back(ReplayEvent{250, ReplayEvent::Kind::Hour, 21.25f});
        written.checkpoints.push_back(
            ReplayCheckpoint{4321, WorldChecksum{1, 2, 3, 4, 5, 6}});

        const std::string path = TempPath("roundtrip.cna-replay");
        std::string error;
        ASSERT_TRUE(SaveReplay(path, written, error)) << error;

        ReplayFile read;
        ASSERT_TRUE(LoadReplay(path, read, error)) << error;
        EXPECT_EQ(read.config.city.seed, written.config.city.seed);
        EXPECT_EQ(read.config.agentCount, written.config.agentCount);
        EXPECT_EQ(read.config.metroLines, written.config.metroLines);
        EXPECT_EQ(read.config.busRoutes, written.config.busRoutes);
        EXPECT_FLOAT_EQ(read.config.startHour, written.config.startHour);
        EXPECT_EQ(read.ticks, written.ticks);
        ASSERT_EQ(read.events.size(), 2u);
        EXPECT_EQ(read.events[1].tick, 250u);
        EXPECT_FLOAT_EQ(read.events[1].value, 21.25f);
        ASSERT_EQ(read.checkpoints.size(), 1u);
        EXPECT_EQ(read.checkpoints[0].checksum, written.checkpoints[0].checksum);
        std::remove(path.c_str());
    }

    TEST(Replay, RubbishIsRejectedRatherThanMisread)
    {
        const std::string path = TempPath("rubbish.cna-replay");
        std::FILE* handle = std::fopen(path.c_str(), "wb");
        ASSERT_NE(handle, nullptr);
        std::fputs("this is not a replay\n", handle);
        std::fclose(handle);

        ReplayFile file;
        std::string error;
        EXPECT_FALSE(LoadReplay(path, file, error));
        EXPECT_FALSE(error.empty());
        std::remove(path.c_str());

        EXPECT_FALSE(LoadReplay(TempPath("no-such-file.cna-replay"), file, error));
    }

    // --- Recording and replaying -------------------------------------------------------------

    TEST(Replay, ARecordedRunReproduces)
    {
        const SimConfig config = SmallSimConfig(3000, 8181);
        const std::string path = TempPath("recorded.cna-replay");

        {
            Simulation sim;
            sim.Initialize(config);
            ReplayRecorder recorder;
            ASSERT_TRUE(recorder.Open(path, config)) << recorder.error();
            for (int i = 0; i < 400; ++i)
            {
                sim.Step(2.0f);
                recorder.MaybeCheckpoint(sim, 200);
            }
            recorder.Close(sim);
            ASSERT_TRUE(recorder.error().empty()) << recorder.error();
        }

        ReplayFile file;
        std::string error;
        ASSERT_TRUE(LoadReplay(path, file, error)) << error;
        EXPECT_GT(file.checkpoints.size(), 2u);
        EXPECT_EQ(file.ticks, 1600u) << "400 steps of two seconds is 1600 half-second ticks";

        const ReplayResult result = RunReplay(file);
        EXPECT_TRUE(result.reproduced) << "diverged at tick " << result.divergedAtTick << " in the "
                                       << result.divergedIn;
        EXPECT_EQ(result.checkpointsChecked, file.checkpoints.size());
        std::remove(path.c_str());
    }

    TEST(Replay, ARecordingIsSmallBecauseTheStateIsRecomputed)
    {
        // The property that makes this worth having: a replay is the *input*, not the world. If
        // this ever starts scaling with the population, something has begun storing state.
        const SimConfig config = SmallSimConfig(20000, 3131);
        const std::string path = TempPath("small.cna-replay");
        {
            Simulation sim;
            sim.Initialize(config);
            ReplayRecorder recorder;
            ASSERT_TRUE(recorder.Open(path, config));
            for (int i = 0; i < 600; ++i)
            {
                sim.Step(2.0f);
                recorder.MaybeCheckpoint(sim, 400);
            }
            recorder.Close(sim);
        }
        EXPECT_LT(std::filesystem::file_size(path), 4096u)
            << "a replay of twenty thousand citizens should be a page of text";
        std::remove(path.c_str());
    }

    TEST(Replay, ATamperedCheckpointIsCaughtAndLocated)
    {
        const SimConfig config = SmallSimConfig(2000, 5252);
        ReplayFile file;
        {
            const std::string path = TempPath("tamper.cna-replay");
            Simulation sim;
            sim.Initialize(config);
            ReplayRecorder recorder;
            ASSERT_TRUE(recorder.Open(path, config));
            for (int i = 0; i < 300; ++i)
            {
                sim.Step(2.0f);
                recorder.MaybeCheckpoint(sim, 200);
            }
            recorder.Close(sim);
            std::string error;
            ASSERT_TRUE(LoadReplay(path, file, error)) << error;
            std::remove(path.c_str());
        }
        ASSERT_GE(file.checkpoints.size(), 2u);

        // Break one component of one checkpoint. The player must stop at that tick and name that
        // component -- "it does not reproduce" on its own is not a lead.
        const std::size_t victim = file.checkpoints.size() - 1;
        file.checkpoints[victim].checksum.traffic ^= 0x1ULL;
        const ReplayResult result = RunReplay(file);
        EXPECT_FALSE(result.reproduced);
        EXPECT_EQ(result.divergedAtTick, file.checkpoints[victim].tick);
        EXPECT_EQ(result.divergedIn, "traffic");
    }

    TEST(Replay, AChangedSeedIsCaughtBeforeASingleTickRuns)
    {
        ReplayFile file;
        {
            const std::string path = TempPath("seed.cna-replay");
            const SimConfig config = SmallSimConfig(1500, 7171);
            Simulation sim;
            sim.Initialize(config);
            ReplayRecorder recorder;
            ASSERT_TRUE(recorder.Open(path, config));
            for (int i = 0; i < 100; ++i) sim.Step(2.0f);
            recorder.Close(sim);
            std::string error;
            ASSERT_TRUE(LoadReplay(path, file, error)) << error;
            std::remove(path.c_str());
        }

        file.config.city.seed = 7172;
        const ReplayResult result = RunReplay(file);
        EXPECT_FALSE(result.reproduced);
        EXPECT_EQ(result.divergedAtTick, 0u);
        EXPECT_EQ(result.divergedIn, "city")
            << "a different city has to be caught before the simulation muddies the evidence";
    }

    TEST(Replay, EventsAreReplayedAtTheirTick)
    {
        // A replay that dropped the events would still reproduce a run in which nobody touched
        // anything, which is most of them -- so this forces one.
        SimConfig config = SmallSimConfig(1200, 2727);
        config.randomWeather = false;
        ReplayFile file;
        {
            const std::string path = TempPath("events.cna-replay");
            Simulation sim;
            sim.Initialize(config);
            ReplayRecorder recorder;
            ASSERT_TRUE(recorder.Open(path, config));
            for (int i = 0; i < 200; ++i)
            {
                if (i == 60)
                {
                    sim.mutableWeather().Force(WeatherKind::Snow);
                    recorder.RecordWeather(sim.tick(), WeatherKind::Snow);
                }
                sim.Step(2.0f);
                recorder.MaybeCheckpoint(sim, 100);
            }
            recorder.Close(sim);
            std::string error;
            ASSERT_TRUE(LoadReplay(path, file, error)) << error;
            std::remove(path.c_str());
        }
        ASSERT_EQ(file.events.size(), 1u);
        EXPECT_TRUE(RunReplay(file).reproduced);

        // And without the event it must *not* reproduce, or the event was never load-bearing and
        // the test above proves nothing.
        ReplayFile without = file;
        without.events.clear();
        EXPECT_FALSE(RunReplay(without).reproduced)
            << "dropping a weather change did not change the run; the event is not being applied";
    }
}
