// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Agents.hpp"
#include "City.hpp"
#include "JobSystem.hpp"
#include "MetroNetwork.hpp"
#include "Pathfinder.hpp"
#include "Traffic.hpp"
#include "WorldState.hpp"

namespace CnaCity
{
    /** @brief Everything the simulation needs to be told; the rest comes from the seed. */
    struct SimConfig
    {
        CityConfig city;
        std::uint32_t agentCount = 100000;
        float startHour = 6.5f;
        float timeScale = 60.0f;         ///< Simulated seconds per real second: a day in 24 minutes.
        WeatherKind weather = WeatherKind::PartlyCloudy;
        bool randomWeather = true;
        float carOwnership = 0.62f;      ///< Share of adults with a car available.
        int metroLines = 5;
        int threads = 0;                 ///< 0 means "as many as the machine has".
    };

    /** @brief Per-tick numbers, for the HUD and for the benchmark's CSV. */
    struct SimStats
    {
        std::uint32_t indoors = 0;
        std::uint32_t walking = 0;
        std::uint32_t driving = 0;
        std::uint32_t waitingTrain = 0;
        std::uint32_t riding = 0;
        std::uint32_t tripsStarted = 0;
        std::uint32_t tripsDeferred = 0;   ///< Wanted to leave but the tick's planning budget was spent.
        std::uint32_t routeFailures = 0;
        std::uint64_t carTripsFinished = 0;
        std::uint64_t carTripsStarted = 0;
        int subSteps = 1;
        std::uint32_t activityCount[kActivityCount] = {};
        double decisionMs = 0.0;
        double walkMs = 0.0;
        double crowdMs = 0.0;
        double trafficMs = 0.0;
        double metroMs = 0.0;
    };

    /**
     * @brief The living city: a hundred thousand daily routines, the traffic they generate, and
     * the clock and weather they happen under.
     *
     * The tick is a fixed step and is deliberately ordered: the world first (clock, weather,
     * trains and signals), then decisions, then movement, then the crowd resolution that movement
     * needs. Nothing in here draws or knows how to; the renderer reads this state and never writes
     * it, which is what lets `--bench` run the whole simulation with no graphics device at all.
     */
    class Simulation
    {
    public:
        Simulation();
        ~Simulation();
        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        void Initialize(const SimConfig& config);

        /**
         * @brief Advances the world by @p simulatedSeconds.
         *
         * The clock, the weather and the daily decisions move by the whole interval, but movement
         * is sub-stepped at @ref kMovementStep. That split is not an optimisation, it is a
         * correctness requirement, and finding out why cost a day of simulated time: at a time
         * scale of 180 a frame is six simulated seconds, a pedestrian covers eight metres in one
         * step, and a waypoint with a three-metre arrival radius is *never reached*. Thirteen
         * thousand citizens ended up oscillating across their last junction forever, at a
         * perfectly healthy 1.3 m/s, and the population of walkers never went down.
         */
        void Step(float simulatedSeconds);

        /** @brief The integration step for everything that moves. See @ref Step. */
        static constexpr float kMovementStep = 0.5f;
        /** @brief The most sub-steps one call will run; beyond this the time scale outruns motion. */
        static constexpr int kMaxSubSteps = 10;

        [[nodiscard]] const SimConfig& config() const { return config_; }
        [[nodiscard]] const City& city() const { return city_; }
        [[nodiscard]] const MetroNetwork& metro() const { return metro_; }
        [[nodiscard]] const Traffic& traffic() const { return traffic_; }
        [[nodiscard]] const Agents& agents() const { return agents_; }
        [[nodiscard]] const WorldClock& clock() const { return clock_; }
        [[nodiscard]] WorldClock& mutableClock() { return clock_; }
        [[nodiscard]] const Weather& weather() const { return weather_; }
        [[nodiscard]] Weather& mutableWeather() { return weather_; }
        [[nodiscard]] const Pathfinder& pathfinder() const { return pathfinder_; }
        [[nodiscard]] const SimStats& stats() const { return stats_; }
        [[nodiscard]] std::uint64_t tick() const { return tick_; }
        [[nodiscard]] const RoutePool& routes() const { return routes_; }
        [[nodiscard]] int threadCount() const { return jobs_ != nullptr ? jobs_->threadCount() : 1; }

        /** @brief The agents currently on foot, as indices; the renderer draws exactly these. */
        [[nodiscard]] const std::vector<std::uint32_t>& walkingAgents() const { return walking_; }

        /** @brief Where an agent is in the world, including its height when it is underground. */
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 AgentWorldPosition(std::uint32_t agent) const;

        /** @brief A one-line description of what an agent is doing, for the follow camera's panel. */
        [[nodiscard]] std::string DescribeAgent(std::uint32_t agent) const;

        /** @brief Picks an agent that is currently outdoors, for the follow camera to adopt. */
        [[nodiscard]] std::uint32_t PickInterestingAgent(std::uint32_t hint) const;

        /** @brief Estimated resident memory of the simulation, for the HUD. */
        [[nodiscard]] std::size_t MemoryBytes() const;

    private:
        void Populate();
        void RunDecisions(float dt);
        void StartTrip(std::uint32_t agent, std::uint32_t destination, Activity nextActivity);
        void FinishTrip(std::uint32_t agent);
        void ReleaseRoute(std::uint32_t agent);
        void StepMovement(float dt);
        void StepWalking(float dt);
        void StepMetroPassengers(float dt);
        void RebuildCrowdGrid();
        void CollectModeLists(bool withActivityHistogram);
        [[nodiscard]] Vec2 SidewalkPoint(std::uint32_t fromNode, std::uint32_t toNode,
                                         float alongMetres, bool atEnd) const;
        [[nodiscard]] std::uint32_t DoorNodeOf(std::uint32_t building) const;

        SimConfig config_;
        City city_;
        MetroNetwork metro_;
        Pathfinder pathfinder_;
        Traffic traffic_;
        Agents agents_;
        RoutePool routes_;
        WorldClock clock_;
        Weather weather_;
        /// Created in Initialize rather than in the constructor, because its width comes from the
        /// configuration and the constructor has not seen one yet. It used to be a member built
        /// with 0, which is why `--threads` was accepted and then ignored.
        std::unique_ptr<JobSystem> jobs_;
        SimStats stats_;
        std::uint64_t tick_ = 0;
        /// Simulated seconds since Initialize, and the half-second counter derived from it.
        ///
        /// Every per-agent decision hashes on the *epoch*, never on the tick. Hashing on the tick
        /// made the city depend on the frame rate: a machine drawing at 120 fps stepped twice as
        /// often and hashed twice as many times, so it produced a different set of trips from the
        /// same seed. The epoch advances with the simulated clock and is therefore the same on any
        /// machine and at any fixed step.
        double simulatedSeconds_ = 0.0;
        std::uint32_t decisionEpoch_ = 0;
        float decisionAccumulator_ = 0.0f;
        std::uint32_t decisionPass_ = 0;

        std::vector<std::uint32_t> walking_;
        std::vector<std::uint32_t> waiting_;
        std::vector<std::uint32_t> riding_;
        /// Agents that want to start a trip this tick, gathered in parallel and then planned in
        /// order. Planning touches the shared Pathfinder and the route pool, neither of which is
        /// thread-safe, and making them so would cost more than the serial pass does.
        std::vector<std::uint32_t> wantsToLeave_;
        std::vector<std::uint32_t> wantsDestination_;
        std::vector<std::uint8_t> wantsActivity_;
        std::vector<std::uint8_t> wantsFlag_;
        std::atomic<std::uint32_t> wantsCount_{0};
        std::uint32_t planRotation_ = 0;
        int lastDayReset_ = -1;
        std::vector<std::uint32_t> scratchPath_;

        /// Per station, the agents standing on the platform. A train that dwells drains these.
        std::vector<std::vector<std::uint32_t>> platformQueue_;

        // --- The crowd grid ---------------------------------------------------------------------
        // A hashed uniform grid over pedestrians, rebuilt every tick. It is hashed rather than
        // dense because a dense grid over a 3.3 km city at pedestrian resolution is a million
        // cells to clear per tick for the twenty thousand people actually standing in them.
        static constexpr std::uint32_t kCrowdBuckets = 1u << 16;
        static constexpr float kCrowdCell = 3.0f;
        std::vector<std::uint32_t> crowdStart_;
        std::vector<std::uint32_t> crowdItems_;
        std::vector<Vec2> crowdPush_;
        /// Scratch that used to be allocated inside the tick. The crowd cursor alone is a 262 KB
        /// vector, and it was built and thrown away once per movement sub-step.
        std::vector<std::uint32_t> crowdCursor_;
        std::vector<std::uint32_t> arrivedScratch_;
    };
}
