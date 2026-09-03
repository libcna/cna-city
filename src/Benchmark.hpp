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

    /**
     * @brief Simulates for `--simulate` and prints a digest of the world.
     *
     * The point of it is CI: the same seed has to give the same city on every build, and a
     * five-line digest is something a script can compare where a screenshot is not. It also
     * re-runs the last stretch at a different step size and on a different number of threads and
     * says whether those agreed, because a determinism claim that is only ever checked one way is
     * a determinism claim that has already been broken twice here.
     */
    int RunChecksum(const CliOptions& options);

    /** @brief Re-runs a `--record` file and reports whether it still reproduces. */
    int RunReplayFile(const CliOptions& options);

    /**
     * @brief Runs the sweep and writes a directory of results.
     *
     * A number printed to a terminal is a number nobody has next week. This leaves `system.json`,
     * `simulation.csv`, `memory.csv`, `rendering.csv`, `passes.csv` and one self-contained
     * `report.html` behind, so a run can be kept, diffed against another machine, and looked at.
     */
    int RunReport(const CliOptions& options);

    /** @brief Reads several report directories and writes one page comparing them. */
    int RunCompare(const CliOptions& options);
}
