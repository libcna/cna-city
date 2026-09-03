// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "CityMath.hpp"
#include "FrameWorker.hpp"

#include "CliOptions.hpp"

namespace CnaCity
{
    class Simulation;

    /**
     * @brief The long run, and the checks that make it worth running.
     *
     * "It ran for an hour and did not crash" is not a result. The failures this project is meant
     * to find do not crash: a route slot that is never released, a passenger nobody ever picks up,
     * a queue that grows by four every morning and shrinks by three every evening. Every one of
     * those is invisible in a thirty-second run and fatal in a long one, and none of them raises a
     * signal -- they just make the numbers slowly stop meaning what they used to.
     *
     * So this simulates several days and asserts, rather than watching. Two kinds of assertion:
     * structural ones that must hold at every instant (@ref CheckInvariants), and trends over the
     * whole run that must not drift (accumulation). The first kind says *what* broke; the second
     * says something is leaking without being able to name it, which is still the difference
     * between a benchmark that is measuring the city and one that is measuring its own rot.
     */

    /** @brief One thing that was true and should not have been. */
    struct Violation
    {
        std::string what;          ///< Human-readable, and specific enough to start from.
        std::uint64_t tick = 0;
        int day = 0;
        float hour = 0.0f;
    };

    /**
     * @brief Every structural invariant, checked against @p sim as it stands.
     *
     * O(agents + vehicles), so it is affordable once a simulated hour but not once a tick. The
     * checks are the ones whose failure would be silent: cross-links that must round-trip
     * (an agent's vehicle must name that agent back), sets that must partition (every citizen is
     * in exactly one mode, and the mode lists must agree with the modes), conservation laws
     * (the people on the buses, counted from the buses, must equal the people on buses counted
     * from the citizens), and occupancy that must not exceed capacity.
     *
     * @param out    Violations are appended, never cleared: a caller checking several moments
     *               wants the whole list.
     * @param limit  Stops after this many *new* violations. One broken invariant tends to break
     *               a hundred thousand times, and a report that is a hundred thousand identical
     *               lines is a report nobody reads to the end.
     * @return The number appended.
     */
    std::size_t CheckInvariants(const Simulation& sim, std::vector<Violation>& out,
                                std::size_t limit = 12);

    /** @brief One checkpoint: what the city looked like at one simulated hour. */
    struct SoakSample
    {
        int day = 0;
        float hour = 0.0f;
        std::uint64_t tick = 0;
        std::uint32_t indoors = 0;
        std::uint32_t walking = 0;
        std::uint32_t driving = 0;
        std::uint32_t waitingTrain = 0;
        std::uint32_t riding = 0;
        std::uint32_t waitingBus = 0;
        std::uint32_t onBus = 0;
        std::uint32_t atHome = 0;      ///< Activity, not mode: asleep or at home.
        std::uint32_t atWork = 0;
        std::uint64_t routesInUse = 0;
        std::uint64_t routeCapacity = 0;
        std::uint32_t deferred = 0;
        std::uint64_t poolExhausted = 0;
        std::uint64_t routeFailures = 0;
        std::uint32_t vehiclesActive = 0;
        std::uint32_t vehiclesBlocked = 0;
        std::uint64_t gridlocked = 0;
        std::uint64_t queuedAtStations = 0;
        std::uint64_t queuedAtStops = 0;
        double simMemoryMb = 0.0;      ///< What the simulation believes it holds.
        double residentMb = 0.0;       ///< What the operating system says, where it will say.
        double pathCacheMb = 0.0;
        float daylight = 0.0f;
        std::size_t violations = 0;
    };

    /**
     * @brief Drives a simulation the way the pipelined frame model does, with no graphics device.
     *
     * The point is to reproduce the *shape* of the real frame rather than its pixels: read
     * everything the draw will need before the step is launched, run the step on the worker, and
     * work only from the captured copies until the join. If the simulation's outcome depends on
     * that at all, something is being read across the launch that should not be -- and this runs
     * under ThreadSanitizer, where "should not be" becomes a report with two stacks in it instead
     * of a checksum that differs one run in fifty.
     *
     * What it does not cover is the real renderer's own reads, which live in `CityGame` and need a
     * device. It covers the concurrency structure those reads sit inside, which is where the
     * defect P20 fixed actually lived.
     */
    class PipelinedStepper
    {
    public:
        /** @brief One frame: capture, launch, "draw", join. */
        void Step(Simulation& sim, float slice);

    private:
        void Capture(const Simulation& sim);
        void Consume();

        FrameWorker worker_;
        std::vector<Vec2> points_;
        float daylight_ = 0.0f;
        float night_ = 0.0f;
        float cloudiness_ = 0.0f;
        float wetness_ = 0.0f;
        float snowCover_ = 0.0f;
        double sink_ = 0.0;
    };

    /**
     * @brief Least-squares gradient of @p series against its index, in units per sample.
     *
     * The whole accumulation test rests on this. A leak does not announce itself; it appears as a
     * line with a small positive gradient buried in a signal that swings by a factor of twenty
     * between three in the morning and nine, which is why the test is a regression over whole days
     * rather than a comparison of two instants. Fewer than three points has no gradient, and says
     * zero rather than guessing.
     */
    [[nodiscard]] double LeastSquaresSlope(const std::vector<double>& series);

    /**
     * @brief Upward drift per simulated day of an hourly @p series, with the daily cycle removed.
     *
     * A straight regression through hourly samples does not measure drift, and finding that out
     * was what a test was for: a pure sine of period 24, sampled hourly over exactly three days,
     * has a least-squares gradient of -3.5 per sample. Whole periods cancel in the mean and do
     * *not* cancel in the covariance with time, so a city's daily rhythm -- which swings by a
     * factor of twenty between three in the morning and nine -- produces a gradient of its own,
     * whose sign depends on nothing but the hour the run started at. A leak detector built on that
     * would report a leak on half the runs and hide one on the other half.
     *
     * So the cycle comes out first, by taking whole-day means and fitting the gradient through
     * those. The mean of twenty-four uniform samples of anything with a twenty-four-hour period is
     * exactly zero regardless of the hour the run started at, and the drift passes through
     * untouched. Subtracting per-hour means instead is the obvious alternative and is wrong in a
     * quieter way: it removes part of the drift along with the cycle, and reports a real leak of
     * twelve a day as nine.
     *
     * @param series Hourly samples, consecutive and aligned, oldest first.
     * @return Units per simulated day, or zero when there are fewer than two whole days -- which
     *         is a refusal to answer rather than an answer of "no drift".
     */
    [[nodiscard]] double DriftPerDay(const std::vector<double>& series);

    /**
     * @brief Simulates several days and asserts that nothing accumulates.
     *
     * Runs the same fixed-step workload under both frame models at once and compares the world
     * digest at every checkpoint, because "pipelining does not change the answer" is a claim, and
     * an unchecked claim about concurrency is a bug with a good reputation.
     *
     * @return 0 when every invariant held and nothing drifted, 1 otherwise. Meant for CI.
     */
    int RunSoak(const CliOptions& options);
}
