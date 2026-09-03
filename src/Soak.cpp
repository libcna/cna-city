// SPDX-License-Identifier: MIT
#include "Soak.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Checksum.hpp"
#include "FrameWorker.hpp"
#include "Simulation.hpp"
#include "Snapshot.hpp"

#if defined(__linux__)
#include <unistd.h>
#endif

namespace CnaCity
{
    namespace
    {
        constexpr float kTickHz = 30.0f;

        [[nodiscard]] bool Finite(float v) { return std::isfinite(v); }

        /** @brief Resident set in megabytes, or a negative number where the platform will not say. */
        double ResidentMb()
        {
#if defined(__linux__)
            std::ifstream statm("/proc/self/statm");
            long total = 0, resident = 0;
            if (statm >> total >> resident)
                return static_cast<double>(resident) *
                       static_cast<double>(::sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
            return -1.0;
        }

        double Mean(const std::vector<double>& v)
        {
            if (v.empty()) return 0.0;
            double sum = 0.0;
            for (double x : v) sum += x;
            return sum / static_cast<double>(v.size());
        }

    }

    double LeastSquaresSlope(const std::vector<double>& series)
    {
        const auto n = static_cast<double>(series.size());
        if (n < 3.0) return 0.0;
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (std::size_t i = 0; i < series.size(); ++i)
        {
            const auto x = static_cast<double>(i);
            sx += x;
            sy += series[i];
            sxx += x * x;
            sxy += x * series[i];
        }
        const double denominator = n * sxx - sx * sx;
        if (std::abs(denominator) < 1e-9) return 0.0;
        return (n * sxy - sx * sy) / denominator;
    }

    void PipelinedStepper::Step(Simulation& sim, float slice)
    {
        Capture(sim);
        worker_.Run([&sim, slice] { sim.Step(slice); });
        Consume();
        worker_.Wait();
    }

    /** @brief Everything the draw would collect, copied out before the launch. */
    void PipelinedStepper::Capture(const Simulation& sim)
    {
        daylight_ = sim.clock().Daylight();
        night_ = sim.clock().StreetLightLevel();
        cloudiness_ = sim.weather().cloudiness();
        wetness_ = sim.weather().wetness();
        snowCover_ = sim.weather().snowCover();

        points_.clear();
        for (std::uint32_t agent : sim.walkingAgents())
            points_.push_back(sim.agents().position[agent]);
        for (std::uint32_t agent : sim.busQueueAgents())
            points_.push_back(sim.agents().position[agent]);
        for (const Vehicle& vehicle : sim.traffic().vehicles())
            if (vehicle.active != 0) points_.push_back(Vec2(vehicle.s, vehicle.speed));
        for (const Bus& bus : sim.buses().buses())
            points_.push_back(Vec2(bus.position, bus.speed));
        for (const MetroTrain& train : sim.metro().trains())
            points_.push_back(Vec2(train.position, train.speed));
    }

    /** @brief The draw: arithmetic over the captured copies and nothing else. */
    void PipelinedStepper::Consume()
    {
        double sum = 0.0;
        for (const Vec2& point : points_)
            sum += static_cast<double>(point.X) * 0.5 + static_cast<double>(point.Y);
        sink_ += sum * static_cast<double>(daylight_ + night_ + cloudiness_ + wetness_ +
                                           snowCover_);
    }

    double DriftPerDay(const std::vector<double>& series)
    {
        constexpr std::size_t kHoursPerDay = 24;
        const std::size_t days = series.size() / kHoursPerDay;
        if (days < 2) return 0.0;

        // Whole-day means, then a gradient through those. The mean of twenty-four uniform samples
        // of anything with a twenty-four-hour period is exactly zero whatever hour the run started
        // at, so this removes the city's rhythm without touching the drift underneath it -- and
        // without the attenuation that subtracting per-hour means causes, which is a subtler
        // version of the same mistake and reads a real twelve-a-day leak as nine.
        std::vector<double> daily(days, 0.0);
        for (std::size_t d = 0; d < days; ++d)
        {
            double sum = 0.0;
            for (std::size_t h = 0; h < kHoursPerDay; ++h) sum += series[d * kHoursPerDay + h];
            daily[d] = sum / static_cast<double>(kHoursPerDay);
        }
        // Two days is a difference rather than a regression. LeastSquaresSlope declines to fit a
        // line through two points on purpose -- through two *noisy* points it is meaningless --
        // but these are day-long means, and the difference between them is the drift by
        // definition.
        if (days == 2) return daily[1] - daily[0];
        return LeastSquaresSlope(daily);
    }

    std::size_t CheckInvariants(const Simulation& sim, std::vector<Violation>& out,
                                std::size_t limit)
    {
        const Agents& agents = sim.agents();
        const Traffic& traffic = sim.traffic();
        const std::vector<Vehicle>& vehicles = traffic.vehicles();
        const RoutePool& routes = sim.routes();
        const City& city = sim.city();
        const std::size_t buildings = city.buildings().size();
        const std::size_t segments = city.roads().segments().size();
        const std::size_t people = agents.size();

        std::size_t added = 0;
        auto fail = [&](std::string what) {
            if (added >= limit) return;
            out.push_back(Violation{std::move(what), sim.tick(), sim.clock().day(),
                                    sim.clock().hour()});
            ++added;
        };
        auto full = [&] { return added >= limit; };

        // The world the citizens are allowed to be in, derived from the roads rather than from a
        // constant: a bound somebody typed is a bound that is wrong the first time the city gets
        // bigger, and the check that matters -- somebody at 1e17 after a bad normalise -- is
        // caught by any honest bound at all.
        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
        for (const RoadNode& node : city.roads().nodes())
        {
            minX = std::min(minX, node.position.X);
            maxX = std::max(maxX, node.position.X);
            minY = std::min(minY, node.position.Y);
            maxY = std::max(maxY, node.position.Y);
        }
        constexpr float kMargin = 250.0f;   // Pavements, forecourts and kerbside parking.
        minX -= kMargin; maxX += kMargin; minY -= kMargin; maxY += kMargin;

        // --- Per citizen ------------------------------------------------------------------------
        std::vector<std::uint8_t> slotHeld(routes.capacity(), 0);
        std::size_t slotsInUse = 0;
        std::uint32_t byMode[7] = {};
        for (std::uint32_t i = 0; i < people && !full(); ++i)
        {
            // The mode is counted before anything can skip this agent. It used to be counted
            // last, and a single citizen with a NaN in their position produced three violations
            // rather than one: the `continue` that stopped reading a corrupt agent also stopped
            // counting them, so the mode histogram no longer matched the mode lists and the
            // conservation checks fired too. One fault has to read as one fault, or the limit
            // fills with echoes of it and the real second fault never gets printed.
            const std::uint8_t mode = agents.mode[i];
            if (mode > static_cast<std::uint8_t>(Mode::OnBus))
            {
                fail("agent " + std::to_string(i) + " is in mode " + std::to_string(mode));
                continue;
            }
            ++byMode[mode];

            // Reported and then carried on past, rather than skipped. A corrupt agent still holds
            // a route slot and still names a vehicle, and refusing to account for those turns one
            // NaN into a lost route slot and a vehicle with no driver -- three violations about
            // one citizen, which is how a limit of twelve fills up with a single fault.
            const Vec2 p = agents.position[i];
            const bool finite = Finite(p.X) && Finite(p.Y) && Finite(agents.heading[i]) &&
                                Finite(agents.speed[i]) && Finite(agents.desiredSpeed[i]) &&
                                Finite(agents.animationPhase[i]) && Finite(agents.waitTimer[i]);
            if (!finite)
            {
                fail("agent " + std::to_string(i) + " holds a non-finite number");
            }
            else
            {
                if (p.X < minX || p.X > maxX || p.Y < minY || p.Y > maxY)
                    fail("agent " + std::to_string(i) + " is outside the city at " +
                         std::to_string(p.X) + ", " + std::to_string(p.Y));
                if (agents.speed[i] < -0.01f || agents.speed[i] > 80.0f)
                    fail("agent " + std::to_string(i) + " moves at " +
                         std::to_string(agents.speed[i]) + " m/s");
            }
            if (agents.activity[i] >= kActivityCount)
                fail("agent " + std::to_string(i) + " is in activity " +
                     std::to_string(agents.activity[i]));
            if (agents.profile[i] >= kProfileCount)
                fail("agent " + std::to_string(i) + " has profile " +
                     std::to_string(agents.profile[i]));
            if (agents.home[i] >= buildings)
                fail("agent " + std::to_string(i) + " lives in building " +
                     std::to_string(agents.home[i]) + " of " + std::to_string(buildings));

            // Route slots. Two agents on one slot is silent corruption -- they follow each other's
            // paths and the world stays plausible -- and a slot nobody holds is the leak this
            // whole test exists for.
            const std::uint32_t slot = agents.pathSlot[i];
            if (slot != kNoIndex)
            {
                if (slot >= routes.capacity())
                    fail("agent " + std::to_string(i) + " holds route slot " +
                         std::to_string(slot) + " of " + std::to_string(routes.capacity()));
                else if (slotHeld[slot] != 0)
                    fail("route slot " + std::to_string(slot) + " is held by two agents, " +
                         std::to_string(i) + " among them");
                else
                {
                    slotHeld[slot] = 1;
                    ++slotsInUse;
                }
                if (agents.pathLength[i] > kMaxPathNodes)
                    fail("agent " + std::to_string(i) + " has a route of " +
                         std::to_string(agents.pathLength[i]) + " nodes");
                if (agents.pathCursor[i] > agents.pathLength[i])
                    fail("agent " + std::to_string(i) + " is at node " +
                         std::to_string(agents.pathCursor[i]) + " of " +
                         std::to_string(agents.pathLength[i]));
            }

            // A citizen is in exactly one place. The mode says which, and every cross-link that
            // does not belong to that mode has to be empty -- somebody indoors who still owns a
            // car is a vehicle nobody will ever despawn, and a passenger who is also driving is
            // two people as far as every count in the report is concerned.
            const auto m = static_cast<Mode>(mode);
            const bool driving = m == Mode::Driving;
            const bool riding = m == Mode::Riding;
            const bool onBus = m == Mode::OnBus;
            if (!driving && agents.vehicle[i] != kNoIndex)
                fail("agent " + std::to_string(i) + " is " + ModeName(m) + " but owns vehicle " +
                     std::to_string(agents.vehicle[i]));
            if (!riding && agents.metroTrain[i] != kNoIndex)
                fail("agent " + std::to_string(i) + " is " + ModeName(m) + " but is on train " +
                     std::to_string(agents.metroTrain[i]));
            if (!onBus && agents.busVehicle[i] != kNoIndex)
                fail("agent " + std::to_string(i) + " is " + ModeName(m) + " but is on bus " +
                     std::to_string(agents.busVehicle[i]));
            if (m == Mode::Indoors && slot != kNoIndex)
                fail("agent " + std::to_string(i) + " is indoors and still holds route slot " +
                     std::to_string(slot));

            if (driving)
            {
                const std::uint32_t v = agents.vehicle[i];
                if (v == kNoIndex || v >= vehicles.size())
                    fail("agent " + std::to_string(i) + " is driving vehicle " +
                         std::to_string(v));
                else if (vehicles[v].active == 0)
                    fail("agent " + std::to_string(i) + " is driving despawned vehicle " +
                         std::to_string(v));
                else if (vehicles[v].driver != i)
                    fail("agent " + std::to_string(i) + " drives vehicle " + std::to_string(v) +
                         ", which names driver " + std::to_string(vehicles[v].driver));
            }
            if (riding && agents.metroTrain[i] >= sim.metro().trains().size())
                fail("agent " + std::to_string(i) + " rides train " +
                     std::to_string(agents.metroTrain[i]));
            if (onBus && agents.busVehicle[i] >= sim.buses().buses().size())
                fail("agent " + std::to_string(i) + " rides bus " +
                     std::to_string(agents.busVehicle[i]));
        }

        if (!full() && slotsInUse != routes.inUse())
            fail("the route pool says " + std::to_string(routes.inUse()) +
                 " slots are out but citizens hold " + std::to_string(slotsInUse) +
                 " -- " + std::to_string(static_cast<long long>(routes.inUse()) -
                                         static_cast<long long>(slotsInUse)) + " are lost");
        if (routes.inUse() > routes.capacity())
            fail("the route pool has issued more slots than it owns");

        // --- Vehicles ---------------------------------------------------------------------------
        std::uint32_t activeSeen = 0;
        for (std::uint32_t v = 0; v < vehicles.size() && !full(); ++v)
        {
            const Vehicle& vehicle = vehicles[v];
            if (vehicle.active == 0)
            {
                if (vehicle.driver != kNoIndex)
                    fail("despawned vehicle " + std::to_string(v) + " still names driver " +
                         std::to_string(vehicle.driver));
                continue;
            }
            ++activeSeen;
            if (!Finite(vehicle.s) || !Finite(vehicle.speed) || !Finite(vehicle.acceleration))
                fail("vehicle " + std::to_string(v) + " holds a non-finite number");
            else if (vehicle.s < -0.01f || vehicle.speed < -0.01f || vehicle.speed > 80.0f)
                fail("vehicle " + std::to_string(v) + " is at s=" + std::to_string(vehicle.s) +
                     " doing " + std::to_string(vehicle.speed) + " m/s");
            if (vehicle.segment >= segments)
                fail("vehicle " + std::to_string(v) + " is on segment " +
                     std::to_string(vehicle.segment) + " of " + std::to_string(segments));
            if (vehicle.lane >= traffic.lanesPerDirection())
                fail("vehicle " + std::to_string(v) + " is in lane " +
                     std::to_string(vehicle.lane));
            if (vehicle.driver == kNoIndex || vehicle.driver >= people)
                fail("active vehicle " + std::to_string(v) + " has driver " +
                     std::to_string(vehicle.driver));
            else if (agents.vehicle[vehicle.driver] != v)
                fail("vehicle " + std::to_string(v) + " names driver " +
                     std::to_string(vehicle.driver) + ", who owns vehicle " +
                     std::to_string(agents.vehicle[vehicle.driver]));
        }
        if (!full() && activeSeen != traffic.activeCount())
            fail("traffic counts " + std::to_string(traffic.activeCount()) +
                 " active vehicles, " + std::to_string(activeSeen) + " are marked active");

        // --- Conservation: the same people, counted from the other end --------------------------
        //
        // Every one of these is a number the city reports twice. A bus knows how many are aboard
        // and so do the passengers, and the day those two disagree is the day the occupancy in
        // every report becomes fiction -- with nothing crashing and no count looking wrong on its
        // own.
        std::uint64_t onBuses = 0;
        for (std::uint32_t b = 0; b < sim.buses().buses().size() && !full(); ++b)
        {
            const Bus& bus = sim.buses().buses()[b];
            onBuses += bus.onboard;
            if (bus.onboard > bus.capacity)
                fail("bus " + std::to_string(b) + " carries " + std::to_string(bus.onboard) +
                     " with " + std::to_string(bus.capacity) + " seats");
            if (bus.route >= sim.buses().routes().size())
            {
                fail("bus " + std::to_string(b) + " is on route " + std::to_string(bus.route));
                continue;
            }
            const BusRoute& route = sim.buses().routes()[bus.route];
            if (!Finite(bus.position) || bus.position < -0.01f ||
                bus.position > route.length + 1.0f)
                fail("bus " + std::to_string(b) + " is at " + std::to_string(bus.position) +
                     " m round a " + std::to_string(route.length) + " m loop");
            if (!Finite(bus.speed) || bus.speed < -0.01f || bus.speed > 40.0f)
                fail("bus " + std::to_string(b) + " does " + std::to_string(bus.speed) + " m/s");
            if (!route.stops.empty() && bus.nextStop >= route.stops.size())
                fail("bus " + std::to_string(b) + " is heading for stop " +
                     std::to_string(bus.nextStop) + " of " + std::to_string(route.stops.size()));
        }
        // No two buses in the same place. Two vehicles at one point is not a state a gap-based
        // follower model can be asked about -- each reads the other as zero metres ahead, both
        // hold, and neither moves again -- so it is worth naming directly rather than waiting to
        // notice the passengers who never arrive. Pairwise within a route: the fleets are tens of
        // buses and this runs once a simulated hour.
        for (std::uint32_t b = 0; b + 1 < sim.buses().buses().size() && !full(); ++b)
        {
            const Bus& bus = sim.buses().buses()[b];
            for (std::uint32_t o = b + 1; o < sim.buses().buses().size() && !full(); ++o)
            {
                const Bus& other = sim.buses().buses()[o];
                if (other.route != bus.route) continue;
                const float length = bus.route < sim.buses().routes().size()
                                         ? sim.buses().routes()[bus.route].length
                                         : 0.0f;
                float apart = std::abs(bus.position - other.position);
                if (length > 0.0f) apart = std::min(apart, length - apart);
                if (apart < 2.0f)
                    fail("buses " + std::to_string(b) + " and " + std::to_string(o) +
                         " are both at " + std::to_string(bus.position) + " m on route " +
                         std::to_string(bus.route));
            }
        }

        if (!full() && onBuses != byMode[static_cast<int>(Mode::OnBus)])
            fail("the buses carry " + std::to_string(onBuses) + " people, " +
                 std::to_string(byMode[static_cast<int>(Mode::OnBus)]) + " citizens are aboard one");

        std::uint64_t onTrains = 0;
        for (std::uint32_t t = 0; t < sim.metro().trains().size() && !full(); ++t)
        {
            const MetroTrain& train = sim.metro().trains()[t];
            onTrains += train.onboard;
            if (train.onboard > train.capacity)
                fail("train " + std::to_string(t) + " carries " + std::to_string(train.onboard) +
                     " with " + std::to_string(train.capacity) + " places");
            if (train.line >= sim.metro().lines().size())
            {
                fail("train " + std::to_string(t) + " is on line " + std::to_string(train.line));
                continue;
            }
            const MetroLine& line = sim.metro().lines()[train.line];
            if (!Finite(train.position) || train.position < -1.0f ||
                train.position > line.length + 1.0f)
                fail("train " + std::to_string(t) + " is at " + std::to_string(train.position) +
                     " m along a " + std::to_string(line.length) + " m line");
            if (!Finite(train.speed) || std::abs(train.speed) > 60.0f)
                fail("train " + std::to_string(t) + " does " + std::to_string(train.speed) + " m/s");
            if (train.direction != 1 && train.direction != -1)
                fail("train " + std::to_string(t) + " travels in direction " +
                     std::to_string(train.direction));
            if (!line.stations.empty() &&
                (train.nextStation < 0 ||
                 static_cast<std::size_t>(train.nextStation) >= line.stations.size()))
                fail("train " + std::to_string(t) + " is heading for station " +
                     std::to_string(train.nextStation));
        }
        if (!full() && onTrains != byMode[static_cast<int>(Mode::Riding)])
            fail("the trains carry " + std::to_string(onTrains) + " people, " +
                 std::to_string(byMode[static_cast<int>(Mode::Riding)]) + " citizens are aboard one");

        // --- The waiting, who are the ones a leak forgets ---------------------------------------
        auto checkQueues = [&](const std::vector<std::vector<std::uint32_t>>& queues, Mode mode,
                               const char* where) {
            std::vector<std::uint8_t> seen(people, 0);
            std::uint64_t queued = 0;
            for (std::uint32_t q = 0; q < queues.size() && !full(); ++q)
                for (std::uint32_t agent : queues[q])
                {
                    ++queued;
                    if (agent >= people)
                    {
                        fail(std::string("a ") + where + " queue holds agent " +
                             std::to_string(agent));
                        continue;
                    }
                    if (seen[agent] != 0)
                        fail("agent " + std::to_string(agent) + " is in two " + where + " queues");
                    seen[agent] = 1;
                    if (agents.mode[agent] != static_cast<std::uint8_t>(mode))
                        fail("agent " + std::to_string(agent) + " is queued at a " + where +
                             " but is " + ModeName(static_cast<Mode>(agents.mode[agent])));
                }
            if (!full() && queued != byMode[static_cast<int>(mode)])
                fail(std::string("the ") + where + " queues hold " + std::to_string(queued) +
                     " people, " + std::to_string(byMode[static_cast<int>(mode)]) +
                     " citizens are waiting at one");
            return queued;
        };
        checkQueues(sim.platformQueues(), Mode::WaitingTrain, "platform");
        checkQueues(sim.stopQueues(), Mode::WaitingBus, "stop");

        // --- The lists the renderer draws from ---------------------------------------------------
        if (!full())
        {
            if (sim.walkingAgents().size() != byMode[static_cast<int>(Mode::Walking)])
                fail("the walking list has " + std::to_string(sim.walkingAgents().size()) +
                     " entries for " + std::to_string(byMode[static_cast<int>(Mode::Walking)]) +
                     " walkers");
            if (sim.waitingAgents().size() != byMode[static_cast<int>(Mode::WaitingTrain)])
                fail("the platform list has " + std::to_string(sim.waitingAgents().size()) +
                     " entries for " +
                     std::to_string(byMode[static_cast<int>(Mode::WaitingTrain)]) + " waiting");
            if (sim.busQueueAgents().size() != byMode[static_cast<int>(Mode::WaitingBus)])
                fail("the bus stop list has " + std::to_string(sim.busQueueAgents().size()) +
                     " entries for " + std::to_string(byMode[static_cast<int>(Mode::WaitingBus)]) +
                     " waiting");
        }

        // --- The world ----------------------------------------------------------------------------
        const WorldClock& clock = sim.clock();
        if (!(clock.hour() >= 0.0f && clock.hour() < 24.0f))
            fail("the clock says " + std::to_string(clock.hour()));
        if (clock.day() < 0) fail("the calendar says day " + std::to_string(clock.day()));
        const Weather& weather = sim.weather();
        auto unitRange = [&](const char* name, float value) {
            if (!Finite(value) || value < -0.001f || value > 1.001f)
                fail(std::string("weather ") + name + " is " + std::to_string(value));
        };
        unitRange("cloudiness", weather.cloudiness());
        unitRange("precipitation", weather.precipitation());
        unitRange("wetness", weather.wetness());
        unitRange("snow cover", weather.snowCover());
        if (!Finite(weather.fogDensity()) || weather.fogDensity() < 0.0f)
            fail("weather fog density is " + std::to_string(weather.fogDensity()));

        return added;
    }

    namespace
    {
        /** @brief One accumulation test: a quantity that must not trend upwards. */
        struct Trend
        {
            std::string name;
            double perDay = 0.0;      ///< Least-squares gradient, in units per simulated day.
            double mean = 0.0;
            double allowed = 0.0;     ///< The gradient at which this is called a leak.
            bool measured = false;
            [[nodiscard]] bool Failed() const { return measured && perDay > allowed; }
        };

        /**
         * @brief Measures whether @p series drifts upward faster than it is allowed to.
         *
         * The tolerance is proportional *and* absolute, and it needs to be both. A pure percentage
         * calls a route pool that idles at three slots broken when it idles at four; a pure
         * absolute figure lets a hundred-megabyte heap grow by ninety-nine and pass.
         */
        Trend Measure(std::string name, const std::vector<double>& series, double relative,
                      double floor)
        {
            Trend trend;
            trend.name = std::move(name);
            if (series.size() < 48) return trend;   // Two whole days, hourly.
            trend.measured = true;
            trend.mean = Mean(series);
            trend.perDay = DriftPerDay(series);
            trend.allowed = std::max(std::abs(trend.mean) * relative, floor);
            return trend;
        }

        /** @brief Mean of whatever @p pick returns over the small hours, when the city is asleep. */
        template <typename Pick>
        double NightMean(const std::vector<SoakSample>& samples, Pick pick)
        {
            std::vector<double> night;
            for (const SoakSample& s : samples)
                if (s.hour >= 2.0f && s.hour < 5.0f) night.push_back(pick(s));
            return Mean(night);
        }

        bool WriteSoakCsv(const std::string& path, const std::vector<SoakSample>& samples,
                          std::string& error)
        {
            std::ofstream file(path, std::ios::trunc);
            if (!file)
            {
                error = "cannot write " + path;
                return false;
            }
            file << "day,hour,tick,indoors,walking,driving,waiting_train,riding,waiting_bus,"
                    "on_bus,at_home,at_work,routes_in_use,route_capacity,deferred,pool_exhausted,"
                    "route_failures,vehicles_active,vehicles_blocked,gridlocked,queued_stations,"
                    "queued_stops,sim_memory_mb,resident_mb,path_cache_mb,daylight,violations\n";
            for (const SoakSample& s : samples)
                file << s.day << ',' << s.hour << ',' << s.tick << ',' << s.indoors << ','
                     << s.walking << ',' << s.driving << ',' << s.waitingTrain << ',' << s.riding
                     << ',' << s.waitingBus << ',' << s.onBus << ',' << s.atHome << ',' << s.atWork
                     << ',' << s.routesInUse << ',' << s.routeCapacity << ',' << s.deferred << ','
                     << s.poolExhausted << ',' << s.routeFailures << ',' << s.vehiclesActive << ','
                     << s.vehiclesBlocked << ',' << s.gridlocked << ',' << s.queuedAtStations << ','
                     << s.queuedAtStops << ',' << s.simMemoryMb << ',' << s.residentMb << ','
                     << s.pathCacheMb << ',' << s.daylight << ',' << s.violations << '\n';
            return static_cast<bool>(file);
        }
    }

    namespace
    {
        /**
         * @brief Turns the checkpoints into a verdict, and says why.
         *
         * The first simulated day is warm-up and is thrown away. A city that starts at half past
         * six with everybody indoors and no route in flight fills up over the following hours, and
         * a gradient measured across that fill is a gradient measuring the fill.
         */
        int Verdict(const std::vector<SoakSample>& samples, const std::vector<Violation>& violations,
                    bool modelsAgree, const std::string& divergence, bool snapshotOk,
                    const std::string& snapshotNote, std::uint32_t agentCount, int days)
        {
            std::printf("\n");
            bool ok = true;

            // --- What must never have happened, at any instant ----------------------------------
            if (!violations.empty())
            {
                ok = false;
                std::printf("INVARIANTS  %zu violation%s\n", violations.size(),
                            violations.size() == 1 ? "" : "s");
                const std::size_t shown = std::min<std::size_t>(violations.size(), 12);
                for (std::size_t i = 0; i < shown; ++i)
                    std::printf("    day %d %05.2f h, tick %llu: %s\n", violations[i].day,
                                static_cast<double>(violations[i].hour),
                                static_cast<unsigned long long>(violations[i].tick),
                                violations[i].what.c_str());
                if (violations.size() > shown)
                    std::printf("    ... and %zu more\n", violations.size() - shown);
            }
            else
            {
                std::printf("INVARIANTS  every check held at all %zu checkpoints\n",
                            samples.size());
            }

            // --- Whether the frame model is part of the simulation -------------------------------
            if (!modelsAgree)
            {
                ok = false;
                std::printf("FRAME MODEL %s\n", divergence.c_str());
            }
            else
            {
                std::printf("FRAME MODEL serial and pipelined agreed on every digest\n");
            }

            if (!snapshotOk)
            {
                ok = false;
                std::printf("SNAPSHOT    %s\n", snapshotNote.c_str());
            }
            else
            {
                std::printf("SNAPSHOT    saved mid-run, reloaded, and carried on identically\n");
            }

            // --- What must not accumulate -------------------------------------------------------
            std::vector<SoakSample> steady;
            for (std::size_t i = 24; i < samples.size(); ++i) steady.push_back(samples[i]);
            if (steady.size() < 48)
            {
                std::printf("ACCUMULATION not measured: %d day%s leaves %zu checkpoints after the "
                            "warm-up day, and a drift wants two whole days (48)\n",
                            days, days == 1 ? "" : "s", steady.size());
            }
            else
            {
                auto column = [&](auto pick) {
                    std::vector<double> v;
                    v.reserve(steady.size());
                    for (const SoakSample& s : steady) v.push_back(static_cast<double>(pick(s)));
                    return v;
                };
                std::vector<Trend> trends;
                trends.push_back(Measure("route slots in use",
                                         column([](const SoakSample& s) { return s.routesInUse; }),
                                         0.02, 20.0));
                trends.push_back(Measure("queues at platforms and stops",
                                         column([](const SoakSample& s) {
                                             return s.queuedAtStations + s.queuedAtStops;
                                         }),
                                         0.02, 25.0));
                trends.push_back(Measure("simulation memory (MB)",
                                         column([](const SoakSample& s) { return s.simMemoryMb; }),
                                         0.01, 1.0));
                trends.push_back(Measure("path cache (MB)",
                                         column([](const SoakSample& s) { return s.pathCacheMb; }),
                                         0.01, 0.5));
                if (steady.front().residentMb >= 0.0)
                    trends.push_back(Measure("resident set (MB)",
                                             column([](const SoakSample& s) { return s.residentMb; }),
                                             0.01, 4.0));

                std::printf("ACCUMULATION over %zu checkpoints after the warm-up day\n",
                            steady.size());
                for (const Trend& trend : trends)
                {
                    if (!trend.measured) continue;
                    const bool bad = trend.Failed();
                    ok = ok && !bad;
                    std::printf("    %-32s mean %10.2f   %+8.3f per day   (allowed %+.3f) %s\n",
                                trend.name.c_str(), trend.mean, trend.perDay, trend.allowed,
                                bad ? "GROWING" : "ok");
                }
            }

            // --- What must come back down ---------------------------------------------------------
            //
            // A leak is not the only way a simulation stops being one. A city where the morning
            // peak never clears, or where the planner's backlog survives the night, is still
            // producing numbers -- they are just no longer numbers about a city.
            std::uint64_t poolExhausted = 0;
            std::uint32_t peakDeferred = 0, peakVehicles = 0;
            float minDaylight = 1e9f, maxDaylight = -1e9f;
            for (const SoakSample& s : samples)
            {
                poolExhausted = std::max(poolExhausted, s.poolExhausted);
                peakDeferred = std::max(peakDeferred, s.deferred);
                peakVehicles = std::max(peakVehicles, s.vehiclesActive);
                minDaylight = std::min(minDaylight, s.daylight);
                maxDaylight = std::max(maxDaylight, s.daylight);
            }
            const double nightDeferred =
                NightMean(samples, [](const SoakSample& s) { return s.deferred; });
            const double nightVehicles =
                NightMean(samples, [](const SoakSample& s) { return s.vehiclesActive; });
            const double nightIndoors =
                NightMean(samples, [](const SoakSample& s) { return s.indoors; });
            const double nightQueues = NightMean(samples, [](const SoakSample& s) {
                return static_cast<double>(s.queuedAtStations + s.queuedAtStops);
            });

            std::printf("RECOVERY\n");
            auto expect = [&](const char* what, bool good, const char* detail) {
                ok = ok && good;
                std::printf("    %-32s %s  %s\n", what, good ? "ok    " : "FAILED", detail);
            };
            char detail[160];

            std::snprintf(detail, sizeof(detail), "%llu times",
                          static_cast<unsigned long long>(poolExhausted));
            expect("route pool never exhausted", poolExhausted == 0, detail);

            std::snprintf(detail, sizeof(detail), "peak %u, %.1f overnight", peakDeferred,
                          nightDeferred);
            expect("planner backlog clears", nightDeferred <= 1.0 + 0.01 * peakDeferred, detail);

            std::snprintf(detail, sizeof(detail), "peak %u, %.0f overnight", peakVehicles,
                          nightVehicles);
            expect("traffic clears after the peak",
                   nightVehicles <= 0.25 * static_cast<double>(peakVehicles) + 1.0, detail);

            const double homeShare =
                agentCount > 0 ? nightIndoors / static_cast<double>(agentCount) : 0.0;
            std::snprintf(detail, sizeof(detail), "%.1f%% indoors at 03:00", homeShare * 100.0);
            expect("the city goes home at night", homeShare >= 0.90, detail);

            std::snprintf(detail, sizeof(detail), "%.0f people still waiting at 03:00", nightQueues);
            expect("nobody is left waiting overnight",
                   nightQueues <= 0.001 * static_cast<double>(agentCount) + 5.0, detail);

            std::snprintf(detail, sizeof(detail), "daylight ranged %.2f to %.2f",
                          static_cast<double>(minDaylight), static_cast<double>(maxDaylight));
            expect("day and night keep cycling", minDaylight < 0.05f && maxDaylight > 0.50f,
                   detail);

            std::printf("\n%s\n", ok ? "SOAK PASSED" : "SOAK FAILED");
            return ok ? 0 : 1;
        }
    }

    int RunSoak(const CliOptions& options)
    {
        const int days = std::max(1, options.soakDays);
        const float slice = options.sim.timeScale / kTickHz;
        const auto ticksPerHour = static_cast<std::uint64_t>(3600.0f / slice);
        if (ticksPerHour == 0)
        {
            std::fprintf(stderr, "cna-city: a time scale of %.1f makes a simulated hour shorter "
                                 "than one tick\n",
                         static_cast<double>(options.sim.timeScale));
            return 2;
        }
        const std::uint64_t totalTicks = ticksPerHour * 24ULL * static_cast<std::uint64_t>(days);

        Simulation serial;
        Simulation pipelined;
        serial.Initialize(options.sim);
        pipelined.Initialize(options.sim);
        PipelinedStepper driver;

        std::printf("cna-city soak -- %u citizens, %d simulated day%s, %llu ticks of %.2f s\n",
                    options.sim.agentCount, days, days == 1 ? "" : "s",
                    static_cast<unsigned long long>(totalTicks), static_cast<double>(slice));
        std::printf("  seed %llu, %d worker threads, one checkpoint per simulated hour\n",
                    static_cast<unsigned long long>(options.sim.city.seed), serial.threadCount());
        std::printf("  both frame models run the same slices, and their digests are compared at "
                    "every checkpoint\n\n");
        std::printf("  day  time   indoors    walk     car    plat   train    stop     bus |"
                    "  routes  defer | vehicles  blocked |    RSS | inv\n");

        std::vector<SoakSample> samples;
        std::vector<Violation> violations;
        bool modelsAgree = true;
        std::string divergence;

        // The snapshot check, spread across the run rather than done in one place: save at the end
        // of the first day, load into a third world, and step that one alongside for an hour
        // before comparing. Comparing at the moment of loading only proves the file round-trips
        // the numbers a digest looks at; carrying on proves it round-tripped the ones it does not.
        std::unique_ptr<Simulation> resumed;
        std::uint64_t resumedTicksLeft = 0;
        bool snapshotStarted = false;
        bool snapshotOk = true;
        std::string snapshotNote = "not reached: the run was shorter than a day";

        for (std::uint64_t t = 0; t < totalTicks; ++t)
        {
            serial.Step(slice);
            driver.Step(pipelined, slice);
            if (resumed && resumedTicksLeft > 0)
            {
                resumed->Step(slice);
                if (--resumedTicksLeft == 0)
                {
                    if (!(ComputeChecksum(*resumed) == ComputeChecksum(serial)))
                    {
                        snapshotOk = false;
                        snapshotNote = "a world reloaded from a snapshot drifted from the one it "
                                       "was saved from within a simulated hour";
                    }
                    else
                    {
                        snapshotNote = "saved mid-run, reloaded, and still identical a simulated "
                                       "hour later";
                    }
                    resumed.reset();
                }
            }

            if ((t + 1) % ticksPerHour != 0) continue;

            SoakSample sample;
            const SimStats& stats = serial.stats();
            sample.day = serial.clock().day();
            sample.hour = serial.clock().hour();
            sample.tick = serial.tick();
            sample.indoors = stats.indoors;
            sample.walking = stats.walking;
            sample.driving = stats.driving;
            sample.waitingTrain = stats.waitingTrain;
            sample.riding = stats.riding;
            sample.waitingBus = stats.waitingBus;
            sample.onBus = stats.onBus;
            sample.atHome = stats.activityCount[static_cast<int>(Activity::Asleep)] +
                            stats.activityCount[static_cast<int>(Activity::AtHome)];
            sample.atWork = stats.activityCount[static_cast<int>(Activity::AtWork)];
            sample.routesInUse = serial.routes().inUse();
            sample.routeCapacity = serial.routes().capacity();
            sample.deferred = stats.tripsDeferred;
            sample.poolExhausted = serial.routes().exhaustedCount();
            sample.routeFailures = stats.routeFailures;
            sample.vehiclesActive = serial.traffic().activeCount();
            sample.vehiclesBlocked = serial.traffic().blockedCount();
            sample.gridlocked = serial.traffic().gridlockedCount();
            for (const std::vector<std::uint32_t>& queue : serial.platformQueues())
                sample.queuedAtStations += queue.size();
            for (const std::vector<std::uint32_t>& queue : serial.stopQueues())
                sample.queuedAtStops += queue.size();
            sample.simMemoryMb = static_cast<double>(serial.MemoryBytes()) / (1024.0 * 1024.0);
            sample.residentMb = ResidentMb();
            sample.pathCacheMb =
                static_cast<double>(serial.pathfinder().cacheBytes()) / (1024.0 * 1024.0);
            sample.daylight = serial.clock().Daylight();

            const std::size_t before = violations.size();
            CheckInvariants(serial, violations);
            CheckInvariants(pipelined, violations);
            sample.violations = violations.size() - before;

            // The claim under test: the frame model is not part of the simulation. Compared every
            // hour rather than once at the end, because "they differ after three days" is a
            // sentence with nowhere to go, and "they differ at 08:00 on day one, in the traffic
            // digest" is the first line of a bug report.
            if (modelsAgree)
            {
                const WorldChecksum mine = ComputeChecksum(serial);
                const WorldChecksum theirs = ComputeChecksum(pipelined);
                if (!(mine == theirs))
                {
                    modelsAgree = false;
                    auto part = [](const char* name, std::uint64_t a, std::uint64_t b) {
                        return a == b ? std::string()
                                      : std::string("\n            ") + name + "  serial " +
                                            ToHex(a) + "  pipelined " + ToHex(b);
                    };
                    char when[64];
                    std::snprintf(when, sizeof(when), "day %d at %02d:%02d", sample.day,
                                  static_cast<int>(sample.hour),
                                  static_cast<int>(sample.hour * 60.0f) % 60);
                    divergence = std::string("diverged on ") + when +
                                 part("city", mine.city, theirs.city) +
                                 part("agents", mine.agents, theirs.agents) +
                                 part("traffic", mine.traffic, theirs.traffic) +
                                 part("transit", mine.transit, theirs.transit) +
                                 part("world", mine.world, theirs.world);
                }
            }

            // Deliberately inside the warm-up day, and not only because a full city in flight is
            // the only moment worth testing a save at. Building a second Simulation, loading it
            // and running it for an hour is a one-off step in the resident set -- 33 MB at a
            // hundred thousand citizens -- which the allocator keeps afterwards. Inside the first
            // day it is discarded with the rest of the warm-up; anywhere later it would read as a
            // memory leak in a run whose whole purpose is to find one.
            if (!snapshotStarted && sample.day >= 1)
            {
                snapshotStarted = true;
                const std::string path =
                    (std::filesystem::temp_directory_path() / "cna-city-soak.snapshot").string();
                std::string error;
                if (!SaveSnapshot(path, serial, "soak resume check", error))
                {
                    snapshotOk = false;
                    snapshotNote = error;
                }
                else
                {
                    auto loaded = std::make_unique<Simulation>();
                    if (!LoadSnapshot(path, *loaded, error))
                    {
                        snapshotOk = false;
                        snapshotNote = error;
                    }
                    else if (!(ComputeChecksum(*loaded) == ComputeChecksum(serial)))
                    {
                        snapshotOk = false;
                        snapshotNote = "the world loaded from a snapshot is not the world that was "
                                       "saved";
                    }
                    else
                    {
                        resumed = std::move(loaded);
                        resumedTicksLeft = ticksPerHour;
                        snapshotNote = "saved and reloaded, but the run ended before the "
                                       "continuation could be compared";
                    }
                }
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }

            char hhmm[16];
            std::snprintf(hhmm, sizeof(hhmm), "%02d:%02d", static_cast<int>(sample.hour),
                          static_cast<int>(sample.hour * 60.0f) % 60);
            std::printf("  %3d  %s  %7u %7u %7u %7u %7u %7u %7u | %7llu %6u | %8u %8u | %6.1f | "
                        "%3zu%s\n",
                        sample.day, hhmm, sample.indoors, sample.walking, sample.driving,
                        sample.waitingTrain, sample.riding, sample.waitingBus, sample.onBus,
                        static_cast<unsigned long long>(sample.routesInUse), sample.deferred,
                        sample.vehiclesActive, sample.vehiclesBlocked, sample.residentMb,
                        sample.violations, sample.poolExhausted != 0 ? "  POOL FULL" : "");
            std::fflush(stdout);
            samples.push_back(sample);
        }

        if (!options.soakCsvPath.empty())
        {
            std::string error;
            if (WriteSoakCsv(options.soakCsvPath, samples, error))
                std::printf("\nwrote %s\n", options.soakCsvPath.c_str());
            else
                std::fprintf(stderr, "\ncna-city: %s\n", error.c_str());
        }

        return Verdict(samples, violations, modelsAgree, divergence, snapshotOk, snapshotNote,
                       options.sim.agentCount, days);
    }
}
