// SPDX-License-Identifier: MIT
#include "Benchmark.hpp"

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

        ScaleResult RunOneScale(const SimConfig& base, std::uint32_t agents, float simulatedHours,
                                bool verbose)
        {
            SimConfig config = base;
            config.agentCount = agents;

            Simulation sim;
            System::Diagnostics::Stopwatch watch;
            watch.Start();
            sim.Initialize(config);
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
                const std::uint32_t travelling = stats.walking + stats.driving + stats.riding +
                                                 stats.waitingTrain;
                result.peakTravelling = std::max(result.peakTravelling, travelling);

                if (verbose && t % 600 == 0)
                    std::printf("    %s  travelling %6u  %.2f ms\n", sim.clock().ClockText(),
                                travelling, ms);
            }

            const auto count = static_cast<double>(samples.size());
            for (double ms : samples) result.meanMs += ms;
            result.meanMs /= count;
            result.decisionMs /= count;
            result.walkMs /= count;
            result.crowdMs /= count;
            result.trafficMs /= count;
            result.metroMs /= count;

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
    }

    int RunHeadless(const CliOptions& options)
    {
        Simulation sim;
        System::Diagnostics::Stopwatch watch;
        watch.Start();
        sim.Initialize(options.sim);
        watch.Stop();

        const City& city = sim.city();
        std::printf("cna-city (headless)\n");
        std::printf("  city      %zu junctions, %zu roads, %zu blocks, %zu buildings, %.1f km\n",
                    city.roads().nodes().size(), city.roads().segments().size(),
                    city.roads().blocks().size(), city.buildings().size(),
                    city.roads().TotalLength() / 1000.0);
        std::printf("  metro     %zu lines, %zu stations, %zu trains\n", sim.metro().lines().size(),
                    sim.metro().stations().size(), sim.metro().trains().size());
        std::printf("  capacity  %u homes, %u jobs for %u citizens\n", city.totalResidentCapacity(),
                    city.totalJobCapacity(), options.sim.agentCount);
        std::printf("  setup     %.0f ms on %d threads, %.1f MB\n\n", ElapsedMs(watch),
                    sim.threadCount(), static_cast<double>(sim.MemoryBytes()) / (1024.0 * 1024.0));

        const ScaleResult result = RunOneScale(options.sim, options.sim.agentCount, 24.0f, true);
        std::printf("\n  mean %.2f ms, p99 %.2f ms, worst %.2f ms over a simulated day\n",
                    result.meanMs, result.p99Ms, result.worstMs);
        std::printf("  peak travelling %u, %llu route queries (%.0f%% cached), %llu gridlock give-ups\n",
                    result.peakTravelling, static_cast<unsigned long long>(result.routeQueries),
                    result.cacheHitRate * 100.0,
                    static_cast<unsigned long long>(result.gridlocked));
        return 0;
    }

    int RunBenchmark(const CliOptions& options)
    {
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
            results.push_back(RunOneScale(options.sim, agents, 6.0f, false));
        }

        std::printf("\n%-9s %8s %8s %8s %8s %8s %8s %8s %8s %8s %9s\n", "agents", "setup", "mean",
                    "p99", "worst", "decide", "walk", "crowd", "traffic", "MB", "peak");
        for (const ScaleResult& r : results)
            std::printf("%-9u %8.0f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %8.1f %9u\n", r.agents,
                        r.setupMs, r.meanMs, r.p99Ms, r.worstMs, r.decisionMs, r.walkMs, r.crowdMs,
                        r.trafficMs, r.memoryMb, r.peakTravelling);

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
                               "crowd_ms,traffic_ms,metro_ms,memory_mb,route_queries,cache_hit,"
                               "peak_travelling,gridlocked\n");
            for (const ScaleResult& r : results)
                std::fprintf(file, "%u,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%llu,"
                                   "%.4f,%u,%llu\n",
                             r.agents, r.setupMs, r.meanMs, r.p99Ms, r.worstMs, r.decisionMs,
                             r.walkMs, r.crowdMs, r.trafficMs, r.metroMs, r.memoryMb,
                             static_cast<unsigned long long>(r.routeQueries), r.cacheHitRate,
                             r.peakTravelling, static_cast<unsigned long long>(r.gridlocked));
            std::fclose(file);
            std::printf("\nwrote %s\n", options.csvPath.c_str());
        }
        return 0;
    }
}
