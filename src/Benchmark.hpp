// SPDX-License-Identifier: MIT
#pragma once

#include "CliOptions.hpp"

namespace CnaCity
{
    /**
     * @brief Runs the simulation at each `--scales` size and reports where the tick goes.
     *
     * No graphics device is created. That is the point: this measures the city, not the driver,
     * and it is what makes the numbers comparable between machines that do not share a GPU.
     */
    int RunBenchmark(const CliOptions& options);

    /** @brief One simulated day with no window and no device, for profiling and for CI. */
    int RunHeadless(const CliOptions& options);
}
