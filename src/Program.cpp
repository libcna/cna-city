// SPDX-License-Identifier: MIT
//
// Temporary headless entry point: it runs the simulation with no graphics device at all, which is
// the property that makes `--bench` possible later. The interactive game replaces it next.
#include <cstdio>
#include <cstdlib>

#include "Simulation.hpp"
#include "System/Diagnostics/Stopwatch.hpp"

using namespace CnaCity;

int main()
{
    Simulation sim;
    SimConfig config;
    if (const char* env = std::getenv("CNA_CITY_AGENTS"))
        config.agentCount = static_cast<std::uint32_t>(std::atoi(env));

    System::Diagnostics::Stopwatch watch;
    watch.Start();
    sim.Initialize(config);
    watch.Stop();

    const City& city = sim.city();
    std::printf("city: %zu nodes, %zu segments, %zu blocks, %zu buildings, %.1f km of road\n",
                city.roads().nodes().size(), city.roads().segments().size(),
                city.roads().blocks().size(), city.buildings().size(),
                city.roads().TotalLength() / 1000.0);
    std::printf("metro: %zu lines, %zu stations, %zu trains\n",
                sim.metro().lines().size(), sim.metro().stations().size(),
                sim.metro().trains().size());
    std::printf("setup: %.0f ms for %u agents on %d threads, %.1f MB resident\n\n",
                static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0,
                config.agentCount, sim.threadCount(),
                static_cast<double>(sim.MemoryBytes()) / (1024.0 * 1024.0));

    constexpr float kTickSeconds = 1.0f / 30.0f;
    const float simStep = kTickSeconds * config.timeScale;
    const int ticks = static_cast<int>(6.0f * 3600.0f / simStep);
    double worst = 0.0, total = 0.0;

    std::printf("%-6s %-6s %8s %8s %8s %8s %8s %7s\n",
                "tick", "clock", "indoors", "walking", "driving", "metro", "trips", "ms");
    for (int t = 0; t < ticks; ++t)
    {
        watch.Restart();
        sim.Step(simStep);
        watch.Stop();
        const double ms = static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        worst = ms > worst ? ms : worst;
        total += ms;
        if (t % 900 == 0)
        {
            const SimStats& s = sim.stats();
            std::printf("%-6d %-6s %8u %8u %8u %8u %8u %7.2f  (cars %u at %.1f m/s)\n",
                        t, sim.clock().ClockText(), s.indoors, s.walking, s.driving,
                        s.waitingTrain + s.riding, s.tripsStarted, ms,
                        sim.traffic().activeCount(), sim.traffic().meanSpeed());
        }
    }

    const Pathfinder::Stats& p = sim.pathfinder().stats();
    const SimStats& s = sim.stats();
    std::printf("\nticks %d, mean %.2f ms, worst %.2f ms\n", ticks, total / ticks, worst);
    std::printf("routes: %llu queries, %.1f%% cached, %llu nodes expanded, %llu corridor fallbacks\n",
                static_cast<unsigned long long>(p.queries),
                p.queries > 0 ? 100.0 * static_cast<double>(p.hits) / static_cast<double>(p.queries) : 0.0,
                static_cast<unsigned long long>(p.nodesExpanded),
                static_cast<unsigned long long>(p.corridorFallbacks));
    std::printf("car trips: %llu started, %llu finished, %llu abandoned to gridlock\n",
                static_cast<unsigned long long>(s.carTripsStarted),
                static_cast<unsigned long long>(s.carTripsFinished),
                static_cast<unsigned long long>(sim.traffic().gridlockedCount()));
    return 0;
}
