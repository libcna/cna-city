// SPDX-License-Identifier: MIT
//
// Snapshots. The reason they exist is the benchmark: measuring the morning peak means simulating
// up to the morning peak first, and at a hundred thousand citizens that is minutes of warm-up
// before the first number -- paid again for every scale and every run.
//
// The property that makes a snapshot worth having is not that it loads. It is that the world
// *continues the same way*: a run restored from a snapshot and the run it was taken from must
// still agree an hour later. Anything left out of the file -- a countdown, a generator's state,
// the order of a queue -- shows up as a slow drift rather than as a failure to load, which is why
// the tests here compare futures rather than contents.

#include <cstdio>
#include <filesystem>

#include "Checksum.hpp"
#include "Snapshot.hpp"
#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        std::string TempFile(const char* name)
        {
            const std::filesystem::path directory =
                std::filesystem::temp_directory_path() / "cna-city-tests";
            std::filesystem::create_directories(directory);
            return (directory / name).string();
        }
    }

    TEST(Snapshot, ARestoredWorldIsTheWorldThatWasSaved)
    {
        const SimConfig config = SmallSimConfig(4000, 5555);
        const std::string path = TempFile("moment.snapshot");

        Simulation original;
        original.Initialize(config);
        RunFor(original, 2400.0f, 2.0f);
        const WorldChecksum atSave = ComputeChecksum(original);

        std::string error;
        ASSERT_TRUE(SaveSnapshot(path, original, "a test moment", error)) << error;

        Simulation restored;
        ASSERT_TRUE(LoadSnapshot(path, restored, error)) << error;
        EXPECT_EQ(ComputeChecksum(restored), atSave) << "the snapshot did not restore the moment";
        EXPECT_EQ(restored.tick(), original.tick());
        EXPECT_FLOAT_EQ(restored.clock().hour(), original.clock().hour());
        std::remove(path.c_str());
    }

    TEST(Snapshot, ARestoredWorldHasTheSameFuture)
    {
        // The test that actually matters. Restoring the visible state is easy; restoring it so
        // that the next hour plays out identically means every countdown, every generator and
        // every queue came back too.
        const SimConfig config = SmallSimConfig(5000, 6767);
        const std::string path = TempFile("future.snapshot");

        Simulation original;
        original.Initialize(config);
        RunFor(original, 1800.0f, 2.0f);

        std::string error;
        ASSERT_TRUE(SaveSnapshot(path, original, "", error)) << error;
        Simulation restored;
        ASSERT_TRUE(LoadSnapshot(path, restored, error)) << error;

        RunFor(original, 3600.0f, 2.0f);
        RunFor(restored, 3600.0f, 2.0f);
        EXPECT_EQ(ComputeChecksum(restored), ComputeChecksum(original))
            << "a restored world drifted from the one it was taken from within an hour";
        std::remove(path.c_str());
    }

    TEST(Snapshot, TheWeatherKeepsHeadingWhereItWasHeading)
    {
        // Named for what a snapshot most easily gets wrong. The weather is a five-minute lag
        // towards a target with a countdown behind it, so a file that stored where it *is* without
        // where it is *going* would load correctly and then drift somewhere else.
        SimConfig config = SmallSimConfig(600, 8989);
        config.randomWeather = true;
        const std::string path = TempFile("weather.snapshot");

        Simulation original;
        original.Initialize(config);
        RunFor(original, 3000.0f, 5.0f);

        std::string error;
        ASSERT_TRUE(SaveSnapshot(path, original, "", error)) << error;
        Simulation restored;
        ASSERT_TRUE(LoadSnapshot(path, restored, error)) << error;

        RunFor(original, 7200.0f, 5.0f);
        RunFor(restored, 7200.0f, 5.0f);
        EXPECT_EQ(restored.weather().kind(), original.weather().kind());
        EXPECT_FLOAT_EQ(restored.weather().cloudiness(), original.weather().cloudiness());
        EXPECT_FLOAT_EQ(restored.weather().wetness(), original.weather().wetness());
        std::remove(path.c_str());
    }

    TEST(Snapshot, TheHeaderDescribesItselfWithoutGeneratingACity)
    {
        SimConfig config = SmallSimConfig(3000, 1212);
        config.startHour = 8.0f;
        const std::string path = TempFile("header.snapshot");

        Simulation sim;
        sim.Initialize(config);
        RunFor(sim, 1200.0f, 2.0f);
        std::string error;
        ASSERT_TRUE(SaveSnapshot(path, sim, "morning rush", error)) << error;

        SnapshotInfo info;
        ASSERT_TRUE(ReadSnapshotInfo(path, info, error)) << error;
        EXPECT_EQ(info.note, "morning rush");
        EXPECT_EQ(info.config.city.seed, config.city.seed);
        EXPECT_EQ(info.config.agentCount, config.agentCount);
        EXPECT_EQ(info.tick, sim.tick());
        EXPECT_NEAR(info.hour, sim.clock().hour(), 1e-4f);
        EXPECT_EQ(info.cityChecksum, ComputeCityChecksum(sim));
        std::remove(path.c_str());
    }

    TEST(Snapshot, ASnapshotFromADifferentCityIsRefused)
    {
        // The failure this prevents is a slow one: the same seed with a changed generator gives
        // roads that the saved traffic is no longer on, so the vehicles would load onto segments
        // that mean something else now. Faking it by editing the recorded digest is the closest a
        // test can get to a generator that has moved.
        const std::string path = TempFile("wrongcity.snapshot");
        Simulation sim;
        sim.Initialize(SmallSimConfig(1000, 4646));
        RunFor(sim, 600.0f, 2.0f);
        std::string error;
        ASSERT_TRUE(SaveSnapshot(path, sim, "", error)) << error;

        // The city digest sits at a known offset in the header: magic, version, four layout
        // words. Flipping a bit in it is what a changed generator looks like from here.
        std::FILE* handle = std::fopen(path.c_str(), "r+b");
        ASSERT_NE(handle, nullptr);
        const long offset = static_cast<long>(sizeof(std::uint64_t) + sizeof(std::uint32_t) +
                                              4 * sizeof(std::uint32_t));
        ASSERT_EQ(std::fseek(handle, offset, SEEK_SET), 0);
        std::uint64_t digest = 0;
        ASSERT_EQ(std::fread(&digest, sizeof(digest), 1, handle), 1u);
        digest ^= 1ULL;
        ASSERT_EQ(std::fseek(handle, offset, SEEK_SET), 0);
        ASSERT_EQ(std::fwrite(&digest, sizeof(digest), 1, handle), 1u);
        std::fclose(handle);

        Simulation loaded;
        EXPECT_FALSE(LoadSnapshot(path, loaded, error));
        EXPECT_NE(error.find("different city"), std::string::npos) << error;
        std::remove(path.c_str());
    }

    TEST(Snapshot, ATruncatedFileIsRefusedRatherThanHalfLoaded)
    {
        const std::string path = TempFile("truncated.snapshot");
        Simulation sim;
        sim.Initialize(SmallSimConfig(2000, 3434));
        RunFor(sim, 600.0f, 2.0f);
        std::string error;
        ASSERT_TRUE(SaveSnapshot(path, sim, "", error)) << error;

        const auto full = std::filesystem::file_size(path);
        std::filesystem::resize_file(path, full / 2);

        Simulation loaded;
        EXPECT_FALSE(LoadSnapshot(path, loaded, error));
        EXPECT_FALSE(error.empty());
        std::remove(path.c_str());

        // And something that is not a snapshot at all.
        const std::string rubbish = TempFile("rubbish.snapshot");
        std::FILE* handle = std::fopen(rubbish.c_str(), "wb");
        ASSERT_NE(handle, nullptr);
        std::fputs("not a snapshot", handle);
        std::fclose(handle);
        EXPECT_FALSE(LoadSnapshot(rubbish, loaded, error));
        std::remove(rubbish.c_str());
    }
}
