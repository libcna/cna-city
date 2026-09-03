// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "Camera.hpp"
#include "Simulation.hpp"

namespace CnaCity
{
    /** @brief What the program is being asked to do. */
    enum class RunMode
    {
        Interactive,   ///< A window, a camera and a HUD.
        Benchmark,     ///< A scale sweep with a CSV, with or without a device.
        Headless,      ///< Simulation only: no window, no graphics device at all.
        Checksum,      ///< Simulate, print a digest of the world, exit.
        Replay,        ///< Re-run a recorded file and report whether it still reproduces.
        Report,        ///< Run the sweep and write a directory of results plus an HTML page.
        Compare        ///< Read several report directories and write one page comparing them.
    };

    /** @brief Visual quality, which is a bundle of pipeline settings rather than one dial. */
    enum class Quality
    {
        Low,
        Medium,
        High,
        Ultra
    };

    [[nodiscard]] const char* QualityName(Quality quality);

    struct CliOptions
    {
        SimConfig sim;
        RunMode mode = RunMode::Interactive;
        Quality quality = Quality::High;
        CameraMode camera = CameraMode::Orbit;
        int overlay = 1;   ///< Index into Overlay: 0 none, 1 statistics, 2 road network, 3 routes.

        int windowWidth = 1600;
        int windowHeight = 900;
        bool fullScreen = false;
        bool vsync = true;

        std::string screenshotPath;
        std::string csvPath;
        int frameLimit = 0;                  ///< 0 means run until the window closes.
        std::vector<std::uint32_t> benchScales;
        bool followMetro = false;   ///< Start the follow camera on somebody using the underground.
        /// Start with the post-processing chain bypassed (what `F2` toggles). A screenshot is the
        /// only way to inspect the raw scene on a machine with no keyboard in front of it, and
        /// "is that white wash the geometry or the tonemapper" is the first question every
        /// lighting bug asks.
        bool noPost = false;
        /// Start the follow camera on somebody using the buses. The counterpart of
        /// @ref followMetro, and needed for the same reason: four hundred citizens on a bus out of
        /// a hundred thousand is not something a random pick finds.
        bool followBus = false;
        /// Simulated seconds for --checksum and --record. Accepts "24h", "90m", "600s" or hours.
        float simulateSeconds = 24.0f * 3600.0f;
        std::string reportPath;   ///< --report: directory to write the benchmark artefacts into.
        /// How many times --report measures each scale. The fastest is reported and the spread
        /// beside it; one run on a shared machine is a number nobody can argue with.
        int reportRepeats = 3;
        /// Measure the renderer as well as the simulation. Set when --report runs with a device,
        /// which is the only way to get frame and GPU pass timings at all.
        bool renderReport = false;
        /// Whether --headless was named, kept separately because --report also sets the mode and
        /// the two can arrive in either order.
        bool headlessRequested = false;
        /// Frames measured per viewpoint, after a warm-up of the same length. Long enough that a
        /// shader compile or a cold cache does not become the measurement.
        int reportFrames = 240;
        /// --compare: the report directories to read, in the order they should appear.
        std::vector<std::string> comparePaths;
        std::string comparisonPath = "comparison.html";
        std::string savePath;     ///< --save: write a snapshot when the run ends.
        std::string loadPath;     ///< --load: start from a snapshot instead of from hour zero.
        std::string snapshotNote; ///< --note: free text stored in the snapshot header.
        std::string recordPath;   ///< --record: write a replay of this run here.
        std::string replayPath;   ///< --replay: re-run this file and check it.
        /// Whether --threads was named. A replay takes its thread count from the file unless the
        /// caller overrides it, and "does this still reproduce on one thread" is the question the
        /// override exists to answer.
        bool threadsGiven = false;
        /// Ticks between replay checkpoints. Small enough to bisect a divergence, large enough
        /// that taking one is not most of the run.
        std::uint64_t checkpointInterval = 1200;
        bool showHelp = false;
        bool listWeather = false;
        std::string error;
    };

    /** @brief Parses argv. On failure, fills @ref CliOptions::error and returns false. */
    bool ParseCommandLine(int argc, char** argv, CliOptions& options);

    void PrintUsage();
}
