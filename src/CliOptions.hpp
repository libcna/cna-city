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
        Headless       ///< Simulation only: no window, no graphics device at all.
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
        bool showHelp = false;
        bool listWeather = false;
        std::string error;
    };

    /** @brief Parses argv. On failure, fills @ref CliOptions::error and returns false. */
    bool ParseCommandLine(int argc, char** argv, CliOptions& options);

    void PrintUsage();
}
