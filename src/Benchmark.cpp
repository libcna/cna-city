// SPDX-License-Identifier: MIT
#include "Benchmark.hpp"

#include "Checksum.hpp"
#include "Replay.hpp"
#include "CityGame.hpp"
#include "Report.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Simulation.hpp"
#include "System/Diagnostics/Stopwatch.hpp"

namespace CnaCity
{
    namespace
    {
        constexpr float kTickHz = 30.0f;

        struct ScaleResult
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
        };

        double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }

        /**
         * @brief Measures @p simulatedHours of the city.
         *
         * @param snapshot When set, the measurement starts from that moment instead of from an
         *        empty morning. A benchmark of the peak has to simulate up to the peak first, and
         *        at a hundred thousand citizens that is twenty-five seconds of warm-up before the
         *        first number -- paid again for every scale and every run. Loading it is a quarter
         *        of a second. The snapshot fixes the population, so a sweep and a snapshot are
         *        mutually exclusive and the caller is told so rather than given a surprise.
         */
        ScaleResult RunOneScale(const SimConfig& base, std::uint32_t agents, float simulatedHours,
                                bool verbose, const std::string& snapshot = std::string())
        {
            SimConfig config = base;
            config.agentCount = agents;

            Simulation sim;
            System::Diagnostics::Stopwatch watch;
            watch.Start();
            if (snapshot.empty())
            {
                sim.Initialize(config);
            }
            else
            {
                std::string error;
                if (!LoadSnapshot(snapshot, sim, error))
                {
                    std::fprintf(stderr, "cna-city: %s\n", error.c_str());
                    return ScaleResult{};
                }
                config = sim.config();
                agents = config.agentCount;
            }
            watch.Stop();

            ScaleResult result;
            result.agents = agents;
            result.setupMs = ElapsedMs(watch);
            result.memoryMb = static_cast<double>(sim.MemoryBytes()) / (1024.0 * 1024.0);

            const float step = config.timeScale / kTickHz;
            const int ticks = std::max(1, static_cast<int>(simulatedHours * 3600.0f / step));
            std::vector<double> samples;
            samples.reserve(static_cast<std::size_t>(ticks));

            for (int t = 0; t < ticks; ++t)
            {
                watch.Restart();
                sim.Step(step);
                watch.Stop();
                const double ms = ElapsedMs(watch);
                samples.push_back(ms);

                const SimStats& stats = sim.stats();
                result.decisionMs += stats.decisionMs;
                result.walkMs += stats.walkMs;
                result.crowdMs += stats.crowdMs;
                result.trafficMs += stats.trafficMs;
                result.metroMs += stats.metroMs;
                result.busMs += stats.busMs;
                const std::uint32_t travelling = stats.walking + stats.driving + stats.riding +
                                                 stats.waitingTrain;
                result.peakTravelling = std::max(result.peakTravelling, travelling);

                if (verbose && t % 600 == 0)
                    std::printf("    %s  %-14s %2.0fC  travelling %6u  %.2f ms\n",
                                sim.clock().ClockText(), WeatherName(sim.weather().kind()),
                                static_cast<double>(sim.weather().temperatureC()), travelling, ms);
            }

            const auto count = static_cast<double>(samples.size());
            for (double ms : samples) result.meanMs += ms;
            result.meanMs /= count;
            result.decisionMs /= count;
            result.walkMs /= count;
            result.crowdMs /= count;
            result.trafficMs /= count;
            result.metroMs /= count;
            result.busMs /= count;

            // The 99th percentile matters more than the mean here: the morning peak is one long
            // burst of route planning, and a mean that hides it would report a frame budget the
            // demo does not actually have.
            std::sort(samples.begin(), samples.end());
            result.worstMs = samples.back();
            result.p99Ms = samples[static_cast<std::size_t>(count * 0.99)];

            const Pathfinder::Stats& routes = sim.pathfinder().stats();
            result.routeQueries = routes.queries;
            result.cacheHitRate = routes.queries > 0
                                      ? static_cast<double>(routes.hits) / static_cast<double>(routes.queries)
                                      : 0.0;
            result.gridlocked = sim.traffic().gridlockedCount();
            return result;
        }
        /**
         * @brief Starts @p sim from `--load` if one was given, and from the seed otherwise.
         *
         * A snapshot carries its own configuration, so loading one overrides the command line --
         * a snapshot is a moment in a particular city and there is no such thing as loading it
         * into a different one. Anything on the command line that would change the city is
         * therefore ignored, and saying so is better than quietly producing a third thing.
         */
        bool StartSimulation(Simulation& sim, const CliOptions& options, bool announce = true)
        {
            if (options.loadPath.empty())
            {
                sim.Initialize(options.sim);
                return true;
            }
            std::string error;
            if (!LoadSnapshot(options.loadPath, sim, error))
            {
                std::fprintf(stderr, "cna-city: %s\n", error.c_str());
                return false;
            }
            SnapshotInfo info;
            if (announce && ReadSnapshotInfo(options.loadPath, info, error))
                std::printf("cna-city: loaded %s -- %u citizens at %02d:%02d on day %d%s%s\n",
                            options.loadPath.c_str(), info.config.agentCount,
                            static_cast<int>(info.hour),
                            static_cast<int>(info.hour * 60.0f) % 60, info.day,
                            info.note.empty() ? "" : ", ", info.note.c_str());
            return true;
        }

        /// Writes a snapshot if `--save` was given. Reports rather than fails the run.
        void FinishSimulation(const Simulation& sim, const CliOptions& options)
        {
            if (options.savePath.empty()) return;
            std::string error;
            if (SaveSnapshot(options.savePath, sim, options.snapshotNote, error))
                std::printf("cna-city: wrote %s\n", options.savePath.c_str());
            else
                std::fprintf(stderr, "cna-city: %s\n", error.c_str());
        }

        /// Steps @p sim for @p seconds in slices of @p slice, recording as it goes.
        void SimulateFor(Simulation& sim, float seconds, float slice, ReplayRecorder* recorder,
                         std::uint64_t checkpointInterval)
        {
            const auto steps = static_cast<int>(seconds / slice);
            for (int i = 0; i < steps; ++i)
            {
                sim.Step(slice);
                if (recorder != nullptr) recorder->MaybeCheckpoint(sim, checkpointInterval);
            }
        }
    }

    int RunHeadless(const CliOptions& options)
    {
        Simulation sim;
        System::Diagnostics::Stopwatch watch;
        watch.Start();
        if (!StartSimulation(sim, options)) return 2;
        watch.Stop();

        const City& city = sim.city();
        std::printf("cna-city (headless)\n");
        std::printf("  city      %zu junctions, %zu roads, %zu blocks, %zu buildings, %.1f km\n",
                    city.roads().nodes().size(), city.roads().segments().size(),
                    city.roads().blocks().size(), city.buildings().size(),
                    city.roads().TotalLength() / 1000.0);
        std::printf("  metro     %zu lines, %zu stations, %zu trains\n", sim.metro().lines().size(),
                    sim.metro().stations().size(), sim.metro().trains().size());
        std::printf("  buses     %zu routes, %zu stops, %zu buses\n", sim.buses().routes().size(),
                    sim.buses().stops().size(), sim.buses().buses().size());
        std::printf("  capacity  %u homes, %u jobs for %u citizens\n", city.totalResidentCapacity(),
                    city.totalJobCapacity(), options.sim.agentCount);
        std::printf("  setup     %.0f ms on %d threads, %.1f MB\n\n", ElapsedMs(watch),
                    sim.threadCount(), static_cast<double>(sim.MemoryBytes()) / (1024.0 * 1024.0));

        const ScaleResult result =
            RunOneScale(options.sim, options.sim.agentCount, 24.0f, true, options.loadPath);
        std::printf("\n  mean %.2f ms, p99 %.2f ms, worst %.2f ms over a simulated day\n",
                    result.meanMs, result.p99Ms, result.worstMs);
        std::printf("  peak travelling %u, %llu route queries (%.0f%% cached), %llu gridlock give-ups\n",
                    result.peakTravelling, static_cast<unsigned long long>(result.routeQueries),
                    result.cacheHitRate * 100.0,
                    static_cast<unsigned long long>(result.gridlocked));
        return 0;
    }

    int RunChecksum(const CliOptions& options)
    {
        Simulation sim;
        if (!StartSimulation(sim, options)) return 2;

        ReplayRecorder recorder;
        ReplayRecorder* recording = nullptr;
        if (!options.recordPath.empty())
        {
            if (!recorder.Open(options.recordPath, options.sim))
            {
                std::fprintf(stderr, "cna-city: %s\n", recorder.error().c_str());
                return 2;
            }
            recording = &recorder;
        }

        const float slice = options.sim.timeScale / kTickHz;
        SimulateFor(sim, options.simulateSeconds, slice, recording, options.checkpointInterval);
        const WorldChecksum digest = ComputeChecksum(sim);
        FinishSimulation(sim, options);

        // From the simulation rather than from the command line: with --load the configuration
        // comes out of the file, and printing the seed that was not used is how a digest ends up
        // filed against the wrong city.
        std::printf("cna-city checksum -- seed %llu, %u agents, %.1f simulated hours, %llu ticks\n\n",
                    static_cast<unsigned long long>(sim.config().city.seed),
                    sim.config().agentCount,
                    static_cast<double>(options.simulateSeconds) / 3600.0,
                    static_cast<unsigned long long>(sim.tick()));
        std::printf("CITY      %s\n", ToHex(digest.city).c_str());
        std::printf("AGENTS    %s\n", ToHex(digest.agents).c_str());
        std::printf("TRAFFIC   %s\n", ToHex(digest.traffic).c_str());
        std::printf("TRANSIT   %s\n", ToHex(digest.transit).c_str());
        std::printf("WORLD     %s\n", ToHex(digest.world).c_str());
        std::printf("FINAL     %s\n", ToHex(digest.total).c_str());

        if (recording != nullptr)
        {
            recorder.Close(sim);
            if (!recorder.error().empty())
            {
                std::fprintf(stderr, "cna-city: %s\n", recorder.error().c_str());
                return 2;
            }
            std::printf("\nwrote %s\n", options.recordPath.c_str());
        }

        // The same run again, driven differently. A digest printed once says what this build did;
        // a digest that survives a different step size and a different number of worker threads
        // says the city is a function of its seed -- which is the actual claim, and which was
        // false in three separate ways until a test asked.
        std::printf("\nreproduced");
        bool ok = true;
        {
            Simulation other;
            if (!StartSimulation(other, options, false)) return 2;
            SimulateFor(other, options.simulateSeconds, slice * 0.5f, nullptr, 0);
            const bool same = ComputeChecksum(other) == digest;
            ok = ok && same;
            std::printf("\n  at half the step size            %s", same ? "yes" : "NO");
        }
        {
            // One Initialize, not two: generating the city twice to change one integer is a
            // second's work thrown away, and it was also what exposed Initialize not resetting
            // everything it should.
            Simulation other;
            CliOptions threaded = options;
            threaded.sim.threads = sim.threadCount() == 1 ? 4 : 1;
            if (!StartSimulation(other, threaded, false)) return 2;
            // A snapshot carries the thread count it was taken with, and loading it puts that back
            // -- so the override goes on afterwards, and without rebuilding the world it would
            // otherwise throw away.
            other.SetThreadCount(threaded.sim.threads);
            SimulateFor(other, options.simulateSeconds, slice, nullptr, 0);
            const bool same = ComputeChecksum(other) == digest;
            ok = ok && same;
            // The *actual* counts, not what was configured: --threads 0 means "as many as the
            // machine has", and reporting a comparison against 0 threads helps nobody.
            std::printf("\n  on %d worker threads instead of %d  %s", other.threadCount(),
                        sim.threadCount(), same ? "yes" : "NO");
        }
        std::printf("\n");
        return ok ? 0 : 1;
    }

    int RunReplayFile(const CliOptions& options)
    {
        ReplayFile file;
        std::string error;
        if (!LoadReplay(options.replayPath, file, error))
        {
            std::fprintf(stderr, "cna-city: %s\n", error.c_str());
            return 2;
        }

        // A replay takes its thread count from the file, unless the caller says otherwise --
        // which is how "does this still reproduce on one thread" gets asked.
        if (options.threadsGiven) file.config.threads = options.sim.threads;

        std::printf("cna-city replay -- %s\n", options.replayPath.c_str());
        std::printf("  seed %llu, %u agents, %llu ticks, %zu events, %zu checkpoints\n\n",
                    static_cast<unsigned long long>(file.config.city.seed), file.config.agentCount,
                    static_cast<unsigned long long>(file.ticks), file.events.size(),
                    file.checkpoints.size());

        const ReplayResult result = RunReplay(file);
        if (result.reproduced)
        {
            std::printf("REPRODUCED -- %llu checkpoints, all matching\n",
                        static_cast<unsigned long long>(result.checkpointsChecked));
            return 0;
        }

        std::printf("DIVERGED at tick %llu, in the %s\n",
                    static_cast<unsigned long long>(result.divergedAtTick),
                    result.divergedIn.c_str());
        std::printf("  %-9s %-16s %-16s\n", "", "expected", "actual");
        const auto row = [](const char* name, std::uint64_t expected, std::uint64_t actual) {
            std::printf("  %-9s %-16s %-16s %s\n", name, ToHex(expected).c_str(),
                        ToHex(actual).c_str(), expected == actual ? "" : "<--");
        };
        row("CITY", result.expected.city, result.actual.city);
        row("AGENTS", result.expected.agents, result.actual.agents);
        row("TRAFFIC", result.expected.traffic, result.actual.traffic);
        row("TRANSIT", result.expected.transit, result.actual.transit);
        row("WORLD", result.expected.world, result.actual.world);
        return 1;
    }

    int RunReport(const CliOptions& options)
    {
        Report report;
        report.system = DescribeSystem();
        report.system.seed = options.sim.city.seed;

        std::vector<std::uint32_t> scales = options.benchScales;
        if (scales.empty()) scales = {1000u, 10000u, 50000u, options.sim.agentCount};
        std::sort(scales.begin(), scales.end());
        scales.erase(std::unique(scales.begin(), scales.end()), scales.end());

        std::printf("cna-city report -- %s, %s, seed %llu\n", report.system.os.c_str(),
                    report.system.renderer.c_str(),
                    static_cast<unsigned long long>(options.sim.city.seed));

        // Each scale is measured more than once and the fastest run is the one reported, with the
        // spread beside it. That is not cherry-picking: a slower run differs from a faster one by
        // whatever else the machine was doing, so the minimum is the closest estimate of the cost
        // of *this program* -- and the spread is what says whether the number can be trusted at
        // all. A benchmark that prints one figure from one run on a shared machine is how a 12%
        // regression and a busy desktop become indistinguishable.
        const int repeats = std::max(1, options.reportRepeats);
        for (const std::uint32_t agents : scales)
        {
            std::printf("  %u agents", agents);
            std::fflush(stdout);
            ScaleResult best;
            double slowest = 0.0;
            for (int run = 0; run < repeats; ++run)
            {
                const ScaleResult r =
                    RunOneScale(options.sim, agents, 6.0f, false, options.loadPath);
                if (r.agents == 0) return 2;
                slowest = std::max(slowest, r.meanMs);
                if (best.agents == 0 || r.meanMs < best.meanMs) best = r;
                std::printf(".");
                std::fflush(stdout);
            }
            std::printf(" %.2f ms\n", best.meanMs);
            report.simulation.push_back(SimulationRow{
                best.agents, best.setupMs, best.meanMs, best.p99Ms, best.worstMs, best.decisionMs,
                best.walkMs, best.crowdMs, best.trafficMs, best.metroMs, best.busMs, best.memoryMb,
                best.routeQueries, best.cacheHitRate, best.peakTravelling, best.gridlocked,
                repeats, slowest - best.meanMs});
            if (!options.loadPath.empty()) break;   // a snapshot pins the population
        }

        // The city digest and the worker count, taken from a simulation built the same way the
        // measurements were, so the report names the exact city it is about.
        {
            Simulation sim;
            SimConfig config = options.sim;
            config.agentCount = scales.front();
            sim.Initialize(config);
            report.system.cityDigest = ToHex(ComputeCityChecksum(sim));
            report.system.workerThreads = sim.threadCount();
        }

        // The rendering half, which needs a device and therefore a window. It runs after the
        // simulation sweep rather than beside it: they compete for the same cores, and a frame
        // time measured while a hundred thousand citizens are being simulated on every other
        // thread is a measurement of the machine's scheduler.
        if (options.renderReport)
        {
            std::printf("\nrendering:\n");
            CliOptions gameOptions = options;
            gameOptions.mode = RunMode::Interactive;
            gameOptions.overlay = 0;
            CityGame game(gameOptions);
            game.Run();
            report.rendering = game.renderingRows();
            report.passes = game.passRows();
            if (!report.rendering.empty()) report.system.graphicsCard = game.rendererName();
        }

        std::string error;
        if (!WriteReport(options.reportPath, report, error))
        {
            std::fprintf(stderr, "cna-city: %s\n", error.c_str());
            return 2;
        }
        std::printf("\nwrote %s/report.html and four CSVs\n", options.reportPath.c_str());
        return 0;
    }

    int RunBenchmark(const CliOptions& options)
    {
        if (!options.loadPath.empty() && options.benchScales.size() > 1)
            std::fprintf(stderr,
                         "cna-city: --load pins the population, so --scales is ignored. Measure a "
                         "sweep from the seed, or one scenario from a snapshot.\n");

        std::printf("cna-city benchmark -- simulation only, no graphics device\n");
        std::printf("seed %llu, %.1f km city, time scale %.0f, %d threads\n\n",
                    static_cast<unsigned long long>(options.sim.city.seed),
                    static_cast<double>(options.sim.city.halfSize) * 2.0 / 1000.0,
                    static_cast<double>(options.sim.timeScale),
                    options.sim.threads);

        std::vector<ScaleResult> results;
        for (std::uint32_t agents : options.benchScales)
        {
            std::printf("  %u agents...\n", agents);
            // Six simulated hours starting at half past six covers the morning peak, which is the
            // only part of the day where the numbers are interesting.
            results.push_back(RunOneScale(options.sim, agents, 6.0f, false, options.loadPath));
        }

        std::printf("\n%-9s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s %9s\n", "agents", "setup",
                    "mean", "p99", "worst", "decide", "walk", "crowd", "traffic", "metro", "bus",
                    "MB", "peak");
        for (const ScaleResult& r : results)
            std::printf("%-9u %8.0f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.1f "
                        "%9u\n",
                        r.agents, r.setupMs, r.meanMs, r.p99Ms, r.worstMs, r.decisionMs, r.walkMs,
                        r.crowdMs, r.trafficMs, r.metroMs, r.busMs, r.memoryMb, r.peakTravelling);

        // Scaling, which is the number the whole exercise is for. Linear would be 1.0.
        if (results.size() > 1)
        {
            std::printf("\nscaling against the smallest run:\n");
            const ScaleResult& base = results.front();
            for (std::size_t i = 1; i < results.size(); ++i)
            {
                const double agentRatio = static_cast<double>(results[i].agents) /
                                          static_cast<double>(base.agents);
                const double costRatio = results[i].meanMs / std::max(1e-6, base.meanMs);
                std::printf("  %ux the agents cost %.2fx the tick (%.2f per agent-fold)\n",
                            static_cast<unsigned>(agentRatio + 0.5), costRatio,
                            costRatio / agentRatio);
            }
        }

        if (!options.csvPath.empty())
        {
            std::FILE* file = std::fopen(options.csvPath.c_str(), "w");
            if (file == nullptr)
            {
                std::fprintf(stderr, "cna-city: cannot write %s\n", options.csvPath.c_str());
                return 1;
            }
            std::fprintf(file, "agents,setup_ms,mean_ms,p99_ms,worst_ms,decision_ms,walk_ms,"
                               "crowd_ms,traffic_ms,metro_ms,bus_ms,memory_mb,route_queries,cache_hit,"
                               "peak_travelling,gridlocked\n");
            for (const ScaleResult& r : results)
                std::fprintf(file, "%u,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%llu,"
                                   "%.4f,%u,%llu\n",
                             r.agents, r.setupMs, r.meanMs, r.p99Ms, r.worstMs, r.decisionMs,
                             r.walkMs, r.crowdMs, r.trafficMs, r.metroMs, r.busMs, r.memoryMb,
                             static_cast<unsigned long long>(r.routeQueries), r.cacheHitRate,
                             r.peakTravelling, static_cast<unsigned long long>(r.gridlocked));
            std::fclose(file);
            std::printf("\nwrote %s\n", options.csvPath.c_str());
        }
        return 0;
    }
}
