// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CnaCity
{
    /**
     * @brief Writing a benchmark run out as something a person and a script can both read.
     *
     * A number printed to a terminal is a number nobody has next week. The point of this is that
     * every run of the benchmark leaves a directory behind: the machine it was measured on, the
     * tables as CSV so a script can diff two of them, and one HTML page so a person can look at
     * the shape of the curve rather than at four significant figures.
     *
     * The page has no external dependencies -- the charts are inline SVG generated here. A report
     * that needs a CDN is a report that stops working the day it is opened on a machine without a
     * network, which for a benchmark artefact is most of them.
     */

    /** @brief What the run was measured on. Written to `system.json`. */
    struct SystemInfo
    {
        std::string renderer;      ///< The CNA renderer this build was configured with.
        std::string graphicsCard;  ///< Filled only when a device was created.
        std::string buildType;
        std::string compiler;
        std::string os;
        int hardwareThreads = 0;
        int workerThreads = 0;
        std::uint64_t seed = 0;
        std::string cityDigest;    ///< So two reports can be told apart at a glance.
        std::string takenAt;       ///< Local time, ISO-ish. Provenance, not a measurement.
        /// One-minute load average where the platform has one, negative where it does not. A
        /// number measured on a busy machine is not wrong, it is just not about this program.
        double loadAverage = -1.0;
    };

    /** @brief One row of `simulation.csv`: the tick at one population. */
    struct SimulationRow
    {
        std::uint32_t agents = 0;
        double setupMs = 0.0;
        double meanMs = 0.0;
        double p99Ms = 0.0;
        double worstMs = 0.0;
        double decisionMs = 0.0;
        double walkMs = 0.0;
        double crowdMs = 0.0;
        double trafficMs = 0.0;
        double metroMs = 0.0;
        double busMs = 0.0;
        double memoryMb = 0.0;
        std::uint64_t routeQueries = 0;
        double cacheHitRate = 0.0;
        std::uint32_t peakTravelling = 0;
        std::uint64_t gridlocked = 0;
        /// How many times this scale was measured, and how far apart the fastest and slowest of
        /// those runs were. A benchmark number without a spread is a benchmark number that cannot
        /// be argued with: on a machine that was not idle the same build measures 0.43 ms and
        /// 1.25 ms for the same work, and a report that prints one of them as fact is worse than
        /// no report.
        int runs = 1;
        double spreadMs = 0.0;
    };

    /** @brief One row of `rendering.csv`: a frame from one viewpoint. */
    struct RenderingRow
    {
        std::string view;
        int width = 0;
        int height = 0;
        std::string quality;
        double frameMs = 0.0;
        double simulationMs = 0.0;
        double drawMs = 0.0;
        double shadowMs = 0.0;
        double prepassMs = 0.0;
        double sceneMs = 0.0;
        double instanceMs = 0.0;
        std::uint32_t drawCalls = 0;
        std::uint32_t triangles = 0;
    };

    /** @brief One row of `passes.csv`: a GPU pass, from CNA's own timer queries. */
    struct PassRow
    {
        std::string view;
        std::string pass;
        double milliseconds = 0.0;
    };

    struct Report
    {
        SystemInfo system;
        std::vector<SimulationRow> simulation;
        std::vector<RenderingRow> rendering;
        std::vector<PassRow> passes;
    };

    /** @brief Reads a report directory back in. Used by the comparison. */
    [[nodiscard]] bool ReadReport(const std::string& directory, Report& out, std::string& error);

    /**
     * @brief Writes one page comparing several reports.
     *
     * Two uses, and they want the same page. One is the renderer comparison this project exists
     * to make possible -- the same city, seed, hour and camera through OPENGLES3, OPENGL33 and
     * Vulkan, with the differences in one table. The other is the one that comes up daily: did
     * this commit change anything, and by how much against the run-to-run spread?
     *
     * A difference smaller than the spread of either run is not reported as a change, because a
     * benchmark that calls noise a regression is a benchmark people stop reading.
     */
    [[nodiscard]] bool WriteComparison(const std::string& path,
                                       const std::vector<std::string>& labels,
                                       const std::vector<Report>& reports, std::string& error);

    /** @brief Fills in everything about the machine that does not need a graphics device. */
    [[nodiscard]] SystemInfo DescribeSystem();

    /**
     * @brief Writes @p report into @p directory, creating it if it is not there.
     *
     * @return False on failure, with @p error filled. A benchmark that silently fails to write its
     *         results has wasted the whole run.
     */
    [[nodiscard]] bool WriteReport(const std::string& directory, const Report& report,
                                   std::string& error);
}
