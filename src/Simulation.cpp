// SPDX-License-Identifier: MIT
#include "Simulation.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "System/Diagnostics/Stopwatch.hpp"
#include "System/Threading/Tasks/Parallel.hpp"

using Microsoft::Xna::Framework::Vector3;

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        /// How many routes may be planned in one tick. A hundred thousand people do not all leave
        /// for work on the same tick, but eight thousand of them can, and an unbounded planning
        /// pass would turn one tick of the morning peak into a visible stall. The overflow is
        /// deferred to the next tick, which in a simulation whose clock runs at 180x is a delay of
        /// well under a simulated second.
        constexpr std::uint32_t kPlanBudgetPerTick = 900;
        /// Decisions are checked at a fraction of the tick rate, staggered across agents.
        constexpr std::uint32_t kDecisionStride = 8;
        constexpr float kArriveRadius = 3.2f;
        constexpr float kPersonalSpace = 1.15f;

        inline float MinutesToHours(std::uint16_t minutes) { return static_cast<float>(minutes) / 60.0f; }

        /// A cheap, well-mixed hash used wherever a decision needs a per-agent random bit without
        /// carrying a generator around. Determinism matters: the same seed and the same tick must
        /// produce the same city, and thread-local generators would not.
        inline std::uint32_t Hash(std::uint32_t a, std::uint32_t b)
        {
            std::uint64_t h = (static_cast<std::uint64_t>(a) << 32) ^ b;
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return static_cast<std::uint32_t>(h);
        }

        inline float HashFloat(std::uint32_t a, std::uint32_t b)
        {
            return static_cast<float>(Hash(a, b) >> 8) * (1.0f / 16777216.0f);
        }

        /**
         * @brief About one citizen in seventy is a professional driver, all day.
         *
         * Vans, taxis and lorries do not commute: they circulate. Without them the roads are empty
         * between the peaks and completely empty at night, which is both wrong -- commercial
         * traffic is a fifth of urban vehicle-kilometres -- and, for something meant to be looked
         * at, the worst possible property. Derived from a hash rather than stored, so it costs no
         * memory across a hundred thousand agents and survives a change of scale unchanged.
         */
        inline bool IsCommercialDriver(std::uint32_t agent)
        {
            return Hash(agent, 0xC0FFEEu) % 1000u < 14u;
        }

        inline double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }
    }

    Simulation::Simulation() : jobs_(0) {}

    void Simulation::Initialize(const SimConfig& config)
    {
        config_ = config;
        city_.Generate(config.city);
        metro_.Generate(city_, config.metroLines, config.city.seed);
        pathfinder_.Build(city_);

        // The fleet is sized from the population rather than fixed: at the morning peak roughly a
        // sixth of the car-owning adults are on the road at once, and a fleet smaller than that
        // silently converts drivers into pedestrians.
        const auto fleet = static_cast<std::uint32_t>(
            std::max(64.0f, static_cast<float>(config.agentCount) * config.carOwnership * 0.30f));
        traffic_.Build(city_, fleet);

        agents_.Resize(config.agentCount);
        // Routes are only held while somebody is travelling. Two fifths of the population moving
        // at once is well past any peak this simulation produces.
        routes_.Reset(std::max<std::size_t>(256, static_cast<std::size_t>(config.agentCount) * 2 / 5));
        platformQueue_.assign(metro_.stations().size(), {});

        clock_.Reset(config.startHour, config.timeScale);
        weather_.Reset(config.weather, config.city.seed ^ 0x9e37ULL);
        crowdStart_.assign(kCrowdBuckets + 1, 0);

        Populate();
        tick_ = 0;
    }

    void Simulation::Populate()
    {
        const std::size_t count = agents_.size();
        const std::vector<Building>& buildings = city_.buildings();
        const std::vector<std::uint32_t>& homes = city_.homes();
        const std::vector<std::uint32_t>& jobs = city_.workplaces();
        const std::vector<std::uint32_t>& leisure = city_.leisureVenues();
        if (homes.empty()) return;

        // Homes and jobs are drawn in proportion to their capacity, so a tower block houses more
        // people than a semi and the morning flow is toward the office districts because that is
        // where the desks are -- not because anything told it to be.
        std::vector<std::uint64_t> homeWeight(homes.size() + 1, 0);
        for (std::size_t i = 0; i < homes.size(); ++i)
            homeWeight[i + 1] = homeWeight[i] + buildings[homes[i]].residents;
        std::vector<std::uint64_t> jobWeight(jobs.size() + 1, 0);
        for (std::size_t i = 0; i < jobs.size(); ++i)
            jobWeight[i + 1] = jobWeight[i] + buildings[jobs[i]].jobs;

        const auto pick = [](const std::vector<std::uint64_t>& weights, std::uint64_t roll) {
            const std::uint64_t total = weights.back();
            if (total == 0) return static_cast<std::size_t>(0);
            const auto it = std::upper_bound(weights.begin(), weights.end(), roll % total);
            return static_cast<std::size_t>(it - weights.begin()) - 1;
        };

        // This one loop is where sharp-runtime's own Parallel::For is used. It is the right tool
        // here for the same reason it is the wrong one on the tick path: it runs once, over a
        // hundred thousand agents, and a thread per chunk amortises away completely.
        const int chunks = std::max(1, jobs_.threadCount());
        const std::size_t chunkSize = (count + chunks - 1) / static_cast<std::size_t>(chunks);
        const std::uint64_t seed = config_.city.seed;
        System::Threading::Tasks::Parallel::For(0, chunks, [&](std::int32_t chunk) {
            const std::size_t begin = static_cast<std::size_t>(chunk) * chunkSize;
            const std::size_t end = std::min(count, begin + chunkSize);
            for (std::size_t i = begin; i < end; ++i)
            {
                Rng rng(seed ^ (static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL), i * 2u + 1u);

                // A plausible age structure, because it is what decides who commutes, who is out
                // at three in the afternoon, and who never leaves their district.
                const float roll = rng.NextFloat();
                Profile profile;
                if (roll < 0.16f)      profile = Profile::Child;
                else if (roll < 0.26f) profile = Profile::Student;
                else if (roll < 0.63f) profile = Profile::Worker;
                else if (roll < 0.73f) profile = Profile::ShiftWorker;
                else if (roll < 0.90f) profile = Profile::Retired;
                else                   profile = Profile::Unemployed;
                agents_.profile[i] = static_cast<std::uint8_t>(profile);

                const std::size_t homeIndex = pick(homeWeight, rng.NextUInt() * 0x100000001ULL + rng.NextUInt());
                agents_.home[i] = homes[homeIndex];
                agents_.position[i] = buildings[agents_.home[i]].doorway;

                const bool works = profile == Profile::Worker || profile == Profile::ShiftWorker ||
                                   profile == Profile::Student;
                if (works && !jobs.empty())
                {
                    // A gravity model by rejection: draw a desk in proportion to how many there
                    // are, accept it with probability exp(-d/D), give up after four tries. The
                    // shape of the commute-distance distribution is what decides the whole mode
                    // split downstream -- an earlier version kept the nearer of two draws, which
                    // is a much stronger bias than it looks and left almost every trip under
                    // eight hundred metres, so nobody drove and nobody used the metro.
                    constexpr float kDecay = 2100.0f;
                    const Vec2 home = agents_.position[i];
                    std::size_t chosen = 0;
                    for (int attempt = 0; attempt < 4; ++attempt)
                    {
                        chosen = pick(jobWeight, rng.NextUInt() * 0x100000001ULL + rng.NextUInt());
                        const float d = Distance(buildings[jobs[chosen]].doorway, home);
                        if (rng.NextFloat() < std::exp(-d / kDecay)) break;
                    }
                    agents_.work[i] = jobs[chosen];
                }
                if (!leisure.empty())
                    agents_.haunt[i] = leisure[rng.NextUInt(static_cast<std::uint32_t>(leisure.size()))];

                // Schedules. The spread is what stops a hundred thousand people leaving on the
                // same tick, and the profiles are what make the day have more than one peak.
                float leaveHome = 7.8f, leaveWork = 17.2f, lunch = 12.3f, bed = 23.0f;
                switch (profile)
                {
                    case Profile::Worker:
                        leaveHome = rng.NextGaussian(7.75f, 0.62f);
                        leaveWork = rng.NextGaussian(17.1f, 0.95f);
                        break;
                    case Profile::ShiftWorker:
                        // Two shifts, so the city is never entirely asleep and the night has
                        // headlights in it.
                        if (rng.Chance(0.5f)) { leaveHome = rng.NextGaussian(5.4f, 0.5f); leaveWork = rng.NextGaussian(14.3f, 0.6f); }
                        else                  { leaveHome = rng.NextGaussian(13.9f, 0.6f); leaveWork = rng.NextGaussian(22.6f, 0.8f); }
                        break;
                    case Profile::Student:
                        leaveHome = rng.NextGaussian(8.1f, 0.7f);
                        leaveWork = rng.NextGaussian(15.9f, 1.5f);
                        break;
                    case Profile::Retired:
                        leaveHome = rng.NextGaussian(9.9f, 1.4f);
                        leaveWork = rng.NextGaussian(13.4f, 1.6f);
                        break;
                    case Profile::Child:
                        leaveHome = rng.NextGaussian(7.9f, 0.4f);
                        leaveWork = rng.NextGaussian(15.2f, 0.7f);
                        break;
                    case Profile::Unemployed:
                        leaveHome = rng.NextGaussian(11.0f, 2.2f);
                        leaveWork = rng.NextGaussian(16.0f, 2.4f);
                        break;
                }
                lunch = rng.NextGaussian(12.35f, 0.45f);
                bed = rng.NextGaussian(profile == Profile::Retired ? 22.2f : 23.3f, 1.0f);

                const auto toMinutes = [](float hour) {
                    return static_cast<std::uint16_t>(Clamp(static_cast<int>(hour * 60.0f), 0, 1439));
                };
                agents_.leaveHomeMinute[i] = toMinutes(leaveHome);
                agents_.leaveWorkMinute[i] = toMinutes(std::max(leaveWork, leaveHome + 1.5f));
                agents_.lunchMinute[i] = toMinutes(lunch);
                agents_.bedMinute[i] = toMinutes(bed);

                // Walking speed by demographic. The spread is deliberately wide: a crowd in which
                // everybody walks at exactly 1.35 m/s never bunches, and bunching is most of what
                // a pavement looks like.
                const float base = profile == Profile::Child   ? 1.12f
                                 : profile == Profile::Retired ? 1.05f
                                                               : 1.38f;
                agents_.desiredSpeed[i] = Clamp(rng.NextGaussian(base, 0.16f), 0.55f, 2.05f);
                agents_.animationPhase[i] = rng.NextFloat(0.0f, 6.28f);

                const bool adult = profile != Profile::Child && profile != Profile::Student;
                const bool car = adult && rng.Chance(config_.carOwnership);
                agents_.flags[i] = static_cast<std::uint8_t>((car ? 0x01u : 0u) |
                                                             (rng.NextUInt(12) << 4));
                agents_.activity[i] = static_cast<std::uint8_t>(Activity::AtHome);
                agents_.mode[i] = static_cast<std::uint8_t>(Mode::Indoors);
            }
        });
    }

    std::uint32_t Simulation::DoorNodeOf(std::uint32_t building) const
    {
        return building == kNoIndex ? kNoNode : city_.buildings()[building].doorNode;
    }

    void Simulation::ReleaseRoute(std::uint32_t agent)
    {
        if (agents_.pathSlot[agent] != kNoIndex)
        {
            routes_.Release(agents_.pathSlot[agent]);
            agents_.pathSlot[agent] = kNoIndex;
        }
        agents_.pathLength[agent] = 0;
        agents_.pathCursor[agent] = 0;
    }

    void Simulation::StartTrip(std::uint32_t agent, std::uint32_t destination, Activity nextActivity)
    {
        if (destination == kNoIndex) return;
        const Building& target = city_.buildings()[destination];
        const Vec2 from = agents_.position[agent];
        const float straight = Distance(from, target.doorway);

        agents_.targetBuilding[agent] = destination;
        agents_.activity[agent] = static_cast<std::uint8_t>(nextActivity);

        // --- Mode choice --------------------------------------------------------------------
        //
        // Three options, and the numbers below are the whole of the demo's mode-share model. It
        // is intentionally legible rather than calibrated: the point is that the shares visibly
        // move when the weather turns or the clock reaches the peak, not that they match a
        // published survey.
        const std::uint32_t bits = Hash(agent, static_cast<std::uint32_t>(tick_));
        const float aversion = weather_.WalkingAversion();
        const bool commercial = IsCommercialDriver(agent);
        const bool hasCar = agents_.OwnsCar(agent) || commercial;

        MetroNetwork::Route metroRoute;
        const bool metroWorks = metro_.PlanRoute(from, target.doorway, metroRoute);

        bool drive = commercial;
        if (!drive && hasCar && straight > 400.0f)
        {
            // A car is chosen more often for long trips, less often in the centre where it would
            // be slower and there is nowhere to park, and more often in bad weather.
            const float centreness = 1.0f - Saturate(Length(from) / config_.city.halfSize);
            float chance = 0.86f - 0.40f * centreness + 0.14f * (aversion - 1.0f);
            chance *= Saturate(straight / 1800.0f) * 0.62f + 0.38f;
            drive = HashFloat(agent, static_cast<std::uint32_t>(tick_ * 7 + 1)) < chance;
        }

        const std::uint32_t startNode = city_.roads().FindNearestNode(from);
        std::uint32_t goalNode = DoorNodeOf(destination);
        TravelMode travelMode = TravelMode::Foot;

        if (drive)
        {
            travelMode = TravelMode::Car;
        }
        else if (metroWorks && straight > 480.0f * aversion)
        {
            // The metro leg replaces the middle of the trip: the route planned now is only as far
            // as the station entrance, and the walk at the far end is planned on arrival.
            agents_.metroBoard[agent] = metroRoute.boardStation;
            agents_.metroAlight[agent] = metroRoute.alightStation;
            goalNode = metro_.stations()[metroRoute.boardStation].doorNode;
        }
        else
        {
            agents_.metroBoard[agent] = kNoIndex;
            agents_.metroAlight[agent] = kNoIndex;
        }

        ReleaseRoute(agent);
        scratchPath_.clear();
        const std::uint32_t length = pathfinder_.FindPath(startNode, goalNode, travelMode, scratchPath_);
        if (length < 2)
        {
            // No route: the citizen stays put and tries again shortly. Teleporting them would
            // hide exactly the kind of network defect this program exists to find.
            ++stats_.routeFailures;
            agents_.activity[agent] = static_cast<std::uint8_t>(Activity::AtHome);
            agents_.waitTimer[agent] = 45.0f;
            agents_.metroBoard[agent] = kNoIndex;
            return;
        }

        const std::uint32_t slot = routes_.Acquire();
        if (slot == kNoIndex)
        {
            agents_.waitTimer[agent] = 30.0f;
            return;
        }
        const std::uint32_t stored = std::min<std::uint32_t>(length, kMaxPathNodes);
        std::copy(scratchPath_.begin(), scratchPath_.begin() + stored, routes_.At(slot));
        agents_.pathSlot[agent] = slot;
        agents_.pathLength[agent] = static_cast<std::uint16_t>(stored);
        agents_.pathCursor[agent] = 0;

        if (drive)
        {
            const std::uint32_t vehicle = traffic_.Spawn(city_, agents_, agent, routes_.At(slot),
                                                         stored, bits, from);
            if (vehicle != kNoIndex)
            {
                agents_.vehicle[agent] = vehicle;
                agents_.mode[agent] = static_cast<std::uint8_t>(Mode::Driving);
                ++stats_.tripsStarted;
                ++stats_.carTripsStarted;
                return;
            }
            // The fleet was full: walk instead, on the route already planned. A driver's route is
            // a legal walking route in this network, so nothing has to be re-planned.
        }

        agents_.mode[agent] = static_cast<std::uint8_t>(Mode::Walking);
        agents_.speed[agent] = 0.0f;
        ++stats_.tripsStarted;
    }

    void Simulation::FinishTrip(std::uint32_t agent)
    {
        ReleaseRoute(agent);
        if (agents_.vehicle[agent] != kNoIndex)
        {
            traffic_.Despawn(agents_.vehicle[agent]);
            agents_.vehicle[agent] = kNoIndex;
        }
        const std::uint32_t destination = agents_.targetBuilding[agent];
        if (destination != kNoIndex)
            agents_.position[agent] = city_.buildings()[destination].doorway;
        agents_.mode[agent] = static_cast<std::uint8_t>(Mode::Indoors);
        agents_.speed[agent] = 0.0f;
        agents_.metroBoard[agent] = kNoIndex;
        agents_.metroAlight[agent] = kNoIndex;
        agents_.metroTrain[agent] = kNoIndex;

        switch (static_cast<Activity>(agents_.activity[agent]))
        {
            case Activity::ToWork:    agents_.activity[agent] = static_cast<std::uint8_t>(Activity::AtWork); break;
            case Activity::ToLunch:   agents_.activity[agent] = static_cast<std::uint8_t>(Activity::AtLunch);
                                      agents_.waitTimer[agent] = 1500.0f + HashFloat(agent, 11u) * 900.0f; break;
            case Activity::ToLeisure:
                agents_.activity[agent] = static_cast<std::uint8_t>(Activity::AtLeisure);
                // A driver's "stop" is a delivery, not an evening out.
                agents_.waitTimer[agent] = IsCommercialDriver(agent)
                                               ? 45.0f + HashFloat(agent, 17u) * 180.0f
                                               : 3600.0f + HashFloat(agent, 13u) * 5400.0f;
                break;
            case Activity::ToHome:    agents_.activity[agent] = static_cast<std::uint8_t>(Activity::AtHome); break;
            default:                  agents_.activity[agent] = static_cast<std::uint8_t>(Activity::AtHome); break;
        }
    }

    void Simulation::RunDecisions(float dt)
    {
        const std::size_t count = agents_.size();
        const float minuteOfDay = clock_.hour() * 60.0f;

        // The day rolls over at four in the morning rather than at midnight, because a simulated
        // day should turn over when the city is at its emptiest and not in the middle of the shift
        // workers' evening.
        const int dayKey = clock_.day() * 2 + (minuteOfDay >= 240.0f ? 1 : 0);
        if (dayKey != lastDayReset_)
        {
            lastDayReset_ = dayKey;
            jobs_.ParallelFor(count, 4096, [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i)
                    agents_.flags[i] &= static_cast<std::uint8_t>(~0x0Eu);
            });
        }

        wantsCount_.store(0, std::memory_order_relaxed);
        wantsToLeave_.resize(count);
        wantsDestination_.resize(count);
        wantsActivity_.resize(count);

        const auto stride = static_cast<std::uint32_t>(tick_ % kDecisionStride);
        jobs_.ParallelFor(count, 2048, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i)
            {
                if (agents_.mode[i] != static_cast<std::uint8_t>(Mode::Indoors)) continue;
                // Staggered: each agent considers its schedule on one tick in eight, so the
                // decision pass costs an eighth of the population rather than all of it.
                if ((i & (kDecisionStride - 1)) != stride) continue;
                if (agents_.waitTimer[i] > 0.0f)
                {
                    agents_.waitTimer[i] -= dt * static_cast<float>(kDecisionStride);
                    continue;
                }

                const auto activity = static_cast<Activity>(agents_.activity[i]);
                std::uint32_t destination = kNoIndex;
                Activity next = activity;
                std::uint8_t setFlag = 0;

                // A professional driver's day is one delivery after another. They pick a fresh
                // destination the moment they finish the last one, which is what keeps the
                // arterials busy at eleven at night.
                if (IsCommercialDriver(static_cast<std::uint32_t>(i)) &&
                    minuteOfDay > 285.0f && minuteOfDay < 1395.0f)
                {
                    const std::vector<std::uint32_t>& venues =
                        (Hash(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(tick_)) & 1u)
                            ? city_.workplaces() : city_.homes();
                    if (!venues.empty())
                    {
                        destination = venues[Hash(static_cast<std::uint32_t>(i),
                                                  static_cast<std::uint32_t>(tick_ * 31u + 7u)) %
                                             venues.size()];
                        next = Activity::ToLeisure;
                        const std::uint32_t slotIndex =
                            wantsCount_.fetch_add(1, std::memory_order_relaxed);
                        if (slotIndex < count)
                        {
                            wantsToLeave_[slotIndex] = static_cast<std::uint32_t>(i);
                            wantsDestination_[slotIndex] = destination;
                            wantsActivity_[slotIndex] = static_cast<std::uint8_t>(next);
                            agents_.waitTimer[i] = 0.01f;
                        }
                        continue;
                    }
                }

                switch (activity)
                {
                    case Activity::Asleep:
                        if (minuteOfDay > 240.0f && minuteOfDay + 90.0f > agents_.leaveHomeMinute[i])
                            agents_.activity[i] = static_cast<std::uint8_t>(Activity::AtHome);
                        break;

                    case Activity::AtHome:
                    {
                        const bool commuted = (agents_.flags[i] & 0x02u) != 0;
                        const float leave = agents_.leaveHomeMinute[i];
                        if (!commuted && agents_.work[i] != kNoIndex &&
                            minuteOfDay >= leave && minuteOfDay < leave + 150.0f)
                        {
                            destination = agents_.work[i];
                            next = Activity::ToWork;
                            setFlag = 0x02u;
                            break;
                        }
                        const bool wentOut = (agents_.flags[i] & 0x08u) != 0;
                        if (!wentOut && agents_.haunt[i] != kNoIndex && minuteOfDay > 1020.0f &&
                            minuteOfDay < static_cast<float>(agents_.bedMinute[i]) - 90.0f &&
                            HashFloat(static_cast<std::uint32_t>(i), 5u) < 0.34f)
                        {
                            destination = agents_.haunt[i];
                            next = Activity::ToLeisure;
                            setFlag = 0x08u;
                            break;
                        }
                        // Errands. Without them the city between the peaks is a photograph: at
                        // half past ten in the morning literally nobody was outdoors, which is
                        // both wrong and, for a demo, the worst possible moment to look at.
                        if (agents_.haunt[i] != kNoIndex && minuteOfDay > 540.0f &&
                            minuteOfDay < 1140.0f &&
                            HashFloat(static_cast<std::uint32_t>(i),
                                      static_cast<std::uint32_t>(tick_ / 384u)) < 0.028f)
                        {
                            destination = agents_.haunt[i];
                            next = Activity::ToLeisure;
                            break;
                        }
                        if (minuteOfDay > static_cast<float>(agents_.bedMinute[i]) || minuteOfDay < 240.0f)
                            agents_.activity[i] = static_cast<std::uint8_t>(Activity::Asleep);
                        break;
                    }

                    case Activity::AtWork:
                    {
                        const bool lunched = (agents_.flags[i] & 0x04u) != 0;
                        if (!lunched && agents_.haunt[i] != kNoIndex &&
                            minuteOfDay >= agents_.lunchMinute[i] &&
                            minuteOfDay < static_cast<float>(agents_.lunchMinute[i]) + 40.0f &&
                            HashFloat(static_cast<std::uint32_t>(i), 7u) < 0.45f)
                        {
                            destination = agents_.haunt[i];
                            next = Activity::ToLunch;
                            setFlag = 0x04u;
                            break;
                        }
                        if (minuteOfDay >= agents_.leaveWorkMinute[i])
                        {
                            destination = agents_.home[i];
                            next = Activity::ToHome;
                        }
                        break;
                    }

                    case Activity::AtLunch:
                        destination = agents_.work[i] != kNoIndex ? agents_.work[i] : agents_.home[i];
                        next = agents_.work[i] != kNoIndex ? Activity::ToWork : Activity::ToHome;
                        break;

                    case Activity::AtLeisure:
                        destination = agents_.home[i];
                        next = Activity::ToHome;
                        break;

                    default:
                        break;
                }

                if (destination == kNoIndex) continue;
                const std::uint32_t slot = wantsCount_.fetch_add(1, std::memory_order_relaxed);
                if (slot >= count) continue;
                wantsToLeave_[slot] = static_cast<std::uint32_t>(i);
                wantsDestination_[slot] = destination;
                wantsActivity_[slot] = static_cast<std::uint8_t>(next);
                agents_.flags[i] |= setFlag;
                // Claimed immediately so the agent is not re-offered on the next tick while it is
                // waiting in the planning queue.
                agents_.waitTimer[i] = 0.01f;
            }
        });

        const std::uint32_t wanted = std::min<std::uint32_t>(wantsCount_.load(),
                                                             static_cast<std::uint32_t>(count));
        const std::uint32_t budget = std::min(wanted, kPlanBudgetPerTick);
        stats_.tripsDeferred = wanted - budget;
        // Rotating where the budget starts stops low agent indices from always winning the queue,
        // which would show up as one corner of the city leaving for work before the rest of it.
        if (wanted > 0) planRotation_ = (planRotation_ + budget) % wanted;
        for (std::uint32_t k = 0; k < budget; ++k)
        {
            const std::uint32_t index = (planRotation_ + k) % wanted;
            const std::uint32_t agent = wantsToLeave_[index];
            agents_.waitTimer[agent] = 0.0f;
            StartTrip(agent, wantsDestination_[index], static_cast<Activity>(wantsActivity_[index]));
        }
        for (std::uint32_t k = budget; k < wanted; ++k)
        {
            // Deferred, not dropped: clear the claim so the next tick re-offers them.
            const std::uint32_t index = (planRotation_ + k) % wanted;
            agents_.waitTimer[wantsToLeave_[index]] = 0.0f;
        }
    }

    Vec2 Simulation::SidewalkPoint(std::uint32_t fromNode, std::uint32_t toNode, float alongMetres,
                                   bool atEnd) const
    {
        const RoadNetwork& roads = city_.roads();
        const std::uint32_t segment = roads.FindSegmentBetween(fromNode, toNode);
        const Vec2 a = roads.nodes()[fromNode].position;
        const Vec2 b = roads.nodes()[toNode].position;
        const Vec2 dir = Normalized(b - a);
        float offset = 4.0f;
        float length = Distance(a, b);
        if (segment != 0xFFFFFFFFu)
        {
            const RoadProfile& profile = ProfileOf(roads.segments()[segment].roadClass);
            // Walk on the right, at the middle of the pavement.
            offset = profile.carriagewayHalfWidth + std::max(1.4f, profile.sidewalkWidth) * 0.5f;
            length = roads.segments()[segment].length;
        }
        const float along = atEnd ? std::max(0.0f, length - 2.0f) : Clamp(alongMetres, 0.0f, length);
        return a + dir * along - Perp(dir) * offset;
    }

    void Simulation::RebuildCrowdGrid()
    {
        std::fill(crowdStart_.begin(), crowdStart_.end(), 0u);
        const auto bucketOf = [](Vec2 p) {
            const auto cx = static_cast<std::int32_t>(std::floor(p.X / kCrowdCell));
            const auto cy = static_cast<std::int32_t>(std::floor(p.Y / kCrowdCell));
            return Hash(static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cy)) & (kCrowdBuckets - 1);
        };
        for (std::uint32_t agent : walking_) ++crowdStart_[bucketOf(agents_.position[agent]) + 1];
        for (std::uint32_t i = 1; i <= kCrowdBuckets; ++i) crowdStart_[i] += crowdStart_[i - 1];
        crowdItems_.resize(walking_.size());
        std::vector<std::uint32_t> cursor(crowdStart_.begin(), crowdStart_.end() - 1);
        for (std::uint32_t agent : walking_) crowdItems_[cursor[bucketOf(agents_.position[agent])]++] = agent;
    }

    void Simulation::CollectModeLists()
    {
        walking_.clear();
        waiting_.clear();
        riding_.clear();
        std::uint32_t indoors = 0, driving = 0;
        for (std::uint32_t i = 0; i < agents_.size(); ++i)
        {
            switch (static_cast<Mode>(agents_.mode[i]))
            {
                case Mode::Indoors:      ++indoors; break;
                case Mode::Walking:      walking_.push_back(i); break;
                case Mode::Driving:      ++driving; break;
                case Mode::WaitingTrain: waiting_.push_back(i); break;
                case Mode::Riding:       riding_.push_back(i); break;
            }
        }
        stats_.indoors = indoors;
        stats_.driving = driving;
        stats_.walking = static_cast<std::uint32_t>(walking_.size());
        stats_.waitingTrain = static_cast<std::uint32_t>(waiting_.size());
        stats_.riding = static_cast<std::uint32_t>(riding_.size());

        for (int a = 0; a < kActivityCount; ++a) stats_.activityCount[a] = 0;
        for (std::uint32_t i = 0; i < agents_.size(); ++i)
            ++stats_.activityCount[agents_.activity[i]];
    }

    void Simulation::StepWalking(float dt)
    {
        if (walking_.empty()) return;
        const RoadNetwork& roads = city_.roads();

        // --- Crowd resolution, from a snapshot -------------------------------------------------
        //
        // The push is computed from the positions as they are *now*, for everybody, before anybody
        // moves. Folding it into the movement pass would have each agent reacting to a mixture of
        // this tick's and last tick's neighbours depending on which thread got there first, which
        // is both non-deterministic and visibly jittery.
        crowdPush_.assign(walking_.size(), Vec2(0.0f, 0.0f));
        jobs_.ParallelFor(walking_.size(), 512, [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k)
            {
                const std::uint32_t agent = walking_[k];
                const Vec2 position = agents_.position[agent];
                const auto cx = static_cast<std::int32_t>(std::floor(position.X / kCrowdCell));
                const auto cy = static_cast<std::int32_t>(std::floor(position.Y / kCrowdCell));
                Vec2 push(0.0f, 0.0f);
                int neighbours = 0;
                for (std::int32_t dy = -1; dy <= 1; ++dy)
                    for (std::int32_t dx = -1; dx <= 1; ++dx)
                    {
                        const std::uint32_t bucket =
                            Hash(static_cast<std::uint32_t>(cx + dx),
                                 static_cast<std::uint32_t>(cy + dy)) & (kCrowdBuckets - 1);
                        for (std::uint32_t j = crowdStart_[bucket]; j < crowdStart_[bucket + 1]; ++j)
                        {
                            const std::uint32_t other = crowdItems_[j];
                            if (other == agent) continue;
                            const Vec2 delta = position - agents_.position[other];
                            const float distSq = LengthSq(delta);
                            if (distSq > kPersonalSpace * kPersonalSpace || distSq < 1e-6f) continue;
                            const float dist = std::sqrt(distSq);
                            // Linear falloff rather than an inverse square: an inverse square
                            // makes two people who happen to coincide fire each other across the
                            // street, which at a hundred thousand agents happens constantly.
                            push = push + delta * ((kPersonalSpace - dist) / (dist * kPersonalSpace));
                            ++neighbours;
                        }
                    }
                if (neighbours > 0)
                    crowdPush_[k] = push * (1.0f / static_cast<float>(neighbours));
            }
        });

        // --- Movement ---------------------------------------------------------------------------
        std::atomic<std::uint32_t> arrivalCount{0};
        std::vector<std::uint32_t> arrived(walking_.size(), kNoIndex);
        jobs_.ParallelFor(walking_.size(), 512, [&](std::size_t begin, std::size_t end) {
            for (std::size_t k = begin; k < end; ++k)
            {
                const std::uint32_t agent = walking_[k];
                const std::uint32_t slot = agents_.pathSlot[agent];
                const std::uint16_t cursor = agents_.pathCursor[agent];
                const std::uint16_t length = agents_.pathLength[agent];

                Vec2 target;
                bool finalLeg = false;
                if (slot != kNoIndex && cursor + 1u < length)
                {
                    const std::uint32_t* path = routes_.At(slot);
                    target = SidewalkPoint(path[cursor], path[cursor + 1], 0.0f, true);
                }
                else
                {
                    finalLeg = true;
                    if (agents_.metroBoard[agent] != kNoIndex)
                        target = metro_.stations()[agents_.metroBoard[agent]].entrance;
                    else if (agents_.targetBuilding[agent] != kNoIndex)
                        target = city_.buildings()[agents_.targetBuilding[agent]].doorway;
                    else
                        target = agents_.position[agent];
                }

                const Vec2 toTarget = target - agents_.position[agent];
                const float distance = Length(toTarget);

                if (distance < kArriveRadius)
                {
                    if (!finalLeg)
                    {
                        agents_.pathCursor[agent] = static_cast<std::uint16_t>(cursor + 1);
                        continue;
                    }
                    const std::uint32_t index = arrivalCount.fetch_add(1, std::memory_order_relaxed);
                    arrived[index] = agent;
                    continue;
                }

                Vec2 desired = toTarget * (1.0f / std::max(distance, 1e-4f));

                // A pedestrian at a signalised junction crosses with the traffic beside them, not
                // against the traffic in front: the arm they are joining has to be the one with
                // the green. Turning along the same arm needs no crossing and never waits.
                bool waiting = false;
                if (!finalLeg && slot != kNoIndex && distance < 7.0f &&
                    static_cast<std::uint32_t>(cursor) + 2u < length)
                {
                    const std::uint32_t* path = routes_.At(slot);
                    const std::uint32_t node = path[cursor + 1];
                    if (roads.nodes()[node].signalised)
                    {
                        const std::uint32_t nextSegment = roads.FindSegmentBetween(node, path[cursor + 2]);
                        const std::uint32_t nextSlot = nextSegment == 0xFFFFFFFFu
                                                           ? 0xFFFFFFFFu
                                                           : roads.IncidenceSlot(node, nextSegment);
                        // Jaywalking, deterministically per agent: about one person in eight does
                        // not wait, which is what stops a crossing looking like a queue of clones.
                        const bool patient = (Hash(agent, 3u) & 7u) != 0;
                        if (nextSlot != 0xFFFFFFFFu && patient && !traffic_.IsGreen(node, nextSlot))
                            waiting = true;
                    }
                }

                const float wanted = waiting ? 0.0f : agents_.desiredSpeed[agent];
                // The crowd push is a steering term, not a teleport: it bends the path and slows
                // the walker down, and both are what a busy pavement does to you.
                desired = desired + crowdPush_[k] * 0.9f;
                const float steer = Length(desired);
                if (steer > 1e-4f) desired = desired * (1.0f / steer);
                const float crowdSlow = 1.0f / (1.0f + Length(crowdPush_[k]) * 0.85f);

                const float targetSpeed = wanted * crowdSlow;
                agents_.speed[agent] += (targetSpeed - agents_.speed[agent]) * std::min(1.0f, dt * 3.5f);
                const float step = agents_.speed[agent] * dt;
                agents_.position[agent] = agents_.position[agent] + desired * step;
                agents_.heading[agent] = ApproachAngle(agents_.heading[agent], Heading(desired), dt * 6.0f);
                // Stride length scales with speed, so the walk cycle matches the ground rather
                // than sliding over it -- the single most obvious tell in an animated crowd.
                agents_.animationPhase[agent] += step * 2.05f;
            }
        });

        const std::uint32_t arrivals = arrivalCount.load();
        for (std::uint32_t k = 0; k < arrivals; ++k)
        {
            const std::uint32_t agent = arrived[k];
            if (agent == kNoIndex) continue;
            if (agents_.metroBoard[agent] != kNoIndex)
            {
                // Down the stairs: the agent leaves the street and joins a platform queue.
                const std::uint32_t station = agents_.metroBoard[agent];
                ReleaseRoute(agent);
                agents_.mode[agent] = static_cast<std::uint8_t>(Mode::WaitingTrain);
                agents_.position[agent] = metro_.stations()[station].position;
                agents_.speed[agent] = 0.0f;
                agents_.waitTimer[agent] = 0.0f;
                platformQueue_[station].push_back(agent);
                continue;
            }
            FinishTrip(agent);
        }
    }

    void Simulation::StepMetroPassengers(float dt)
    {
        const std::vector<MetroLine>& lines = metro_.lines();
        std::vector<MetroTrain>& trains = metro_.mutableTrains();

        // --- Boarding ---------------------------------------------------------------------------
        for (std::uint32_t t = 0; t < trains.size(); ++t)
        {
            MetroTrain& train = trains[t];
            if (train.dwellRemaining <= 0.0f) continue;
            const MetroLine& line = lines[train.line];
            // While dwelling, `nextStation` has already advanced past the platform the train is
            // standing at, so the station being served is the one before it in the direction of
            // travel.
            const int servedIndex = Clamp(train.nextStation - train.direction, 0,
                                          static_cast<int>(line.stations.size()) - 1);
            const std::uint32_t station = line.stations[static_cast<std::size_t>(servedIndex)];

            std::vector<std::uint32_t>& queue = platformQueue_[station];
            if (queue.empty()) continue;

            std::size_t write = 0;
            for (std::size_t k = 0; k < queue.size(); ++k)
            {
                const std::uint32_t agent = queue[k];
                if (agents_.mode[agent] != static_cast<std::uint8_t>(Mode::WaitingTrain))
                    continue;   // already gone; drop from the queue
                const std::uint32_t alight = agents_.metroAlight[agent];
                int direction = 0;
                const std::uint32_t wantedLine = metro_.LineBetween(station, alight, &direction);
                const bool sameLine = wantedLine == train.line;
                const bool sameWay = direction == train.direction || line.loop;
                if (sameLine && sameWay && train.onboard < train.capacity)
                {
                    agents_.mode[agent] = static_cast<std::uint8_t>(Mode::Riding);
                    agents_.metroTrain[agent] = t;
                    agents_.metroBoard[agent] = kNoIndex;
                    ++train.onboard;
                    continue;
                }
                queue[write++] = agent;
            }
            queue.resize(write);
        }

        // --- The platform give-up rule -----------------------------------------------------------
        //
        // Nothing here may wait forever. A queue is drained by trains and trains are reliable, but
        // "reliable" is a property of code that can change, and a passenger who has stood on a
        // platform for seven simulated minutes climbs the stairs and walks instead. It is also
        // exactly what a person does.
        for (std::uint32_t agent : waiting_)
        {
            agents_.waitTimer[agent] += dt;
            if (agents_.waitTimer[agent] < 420.0f) continue;
            const std::uint32_t station = agents_.metroBoard[agent] != kNoIndex
                                              ? agents_.metroBoard[agent]
                                              : agents_.metroAlight[agent];
            agents_.waitTimer[agent] = 0.0f;
            agents_.metroBoard[agent] = kNoIndex;
            agents_.metroAlight[agent] = kNoIndex;
            agents_.mode[agent] = static_cast<std::uint8_t>(Mode::Walking);
            if (station != kNoIndex && station < metro_.stations().size())
            {
                agents_.position[agent] = metro_.stations()[station].entrance;
                const std::uint32_t startNode = metro_.stations()[station].doorNode;
                const std::uint32_t goalNode = DoorNodeOf(agents_.targetBuilding[agent]);
                scratchPath_.clear();
                const std::uint32_t length =
                    pathfinder_.FindPath(startNode, goalNode, TravelMode::Foot, scratchPath_);
                ReleaseRoute(agent);
                if (length >= 2)
                {
                    const std::uint32_t slot = routes_.Acquire();
                    if (slot != kNoIndex)
                    {
                        const std::uint32_t stored = std::min<std::uint32_t>(length, kMaxPathNodes);
                        std::copy(scratchPath_.begin(), scratchPath_.begin() + stored, routes_.At(slot));
                        agents_.pathSlot[agent] = slot;
                        agents_.pathLength[agent] = static_cast<std::uint16_t>(stored);
                        agents_.pathCursor[agent] = 0;
                    }
                }
            }
        }

        // --- Riding and alighting ---------------------------------------------------------------
        for (std::uint32_t agent : riding_)
        {
            const std::uint32_t trainIndex = agents_.metroTrain[agent];
            if (trainIndex == kNoIndex || trainIndex >= trains.size())
            {
                agents_.mode[agent] = static_cast<std::uint8_t>(Mode::Indoors);
                continue;
            }
            MetroTrain& train = trains[trainIndex];
            const MetroLine& line = lines[train.line];
            agents_.position[agent] = metro_.PointOnLine(train.line, train.position);
            agents_.speed[agent] = train.speed;

            if (train.dwellRemaining <= 0.0f) continue;
            const int servedIndex = Clamp(train.nextStation - train.direction, 0,
                                          static_cast<int>(line.stations.size()) - 1);
            const std::uint32_t station = line.stations[static_cast<std::size_t>(servedIndex)];
            if (station != agents_.metroAlight[agent]) continue;

            if (train.onboard > 0) --train.onboard;
            agents_.metroTrain[agent] = kNoIndex;
            agents_.metroAlight[agent] = kNoIndex;
            agents_.position[agent] = metro_.stations()[station].entrance;
            agents_.speed[agent] = 0.0f;

            // Up the stairs and the last walk. Planning it now rather than at the start of the
            // trip is what makes an interchange work without a multi-leg route structure.
            const std::uint32_t destination = agents_.targetBuilding[agent];
            const std::uint32_t startNode = metro_.stations()[station].doorNode;
            const std::uint32_t goalNode = DoorNodeOf(destination);
            scratchPath_.clear();
            const std::uint32_t length = pathfinder_.FindPath(startNode, goalNode, TravelMode::Foot,
                                                              scratchPath_);
            ReleaseRoute(agent);
            if (length >= 2)
            {
                const std::uint32_t slot = routes_.Acquire();
                if (slot != kNoIndex)
                {
                    const std::uint32_t stored = std::min<std::uint32_t>(length, kMaxPathNodes);
                    std::copy(scratchPath_.begin(), scratchPath_.begin() + stored, routes_.At(slot));
                    agents_.pathSlot[agent] = slot;
                    agents_.pathLength[agent] = static_cast<std::uint16_t>(stored);
                    agents_.pathCursor[agent] = 0;
                }
            }
            agents_.mode[agent] = static_cast<std::uint8_t>(Mode::Walking);
        }
    }

    void Simulation::StepMovement(float dt)
    {
        System::Diagnostics::Stopwatch watch;

        watch.Restart();
        metro_.Step(dt);
        traffic_.StepSignals(dt);
        stats_.metroMs += ElapsedMs(watch);

        watch.Restart();
        RebuildCrowdGrid();
        stats_.crowdMs += ElapsedMs(watch);

        watch.Restart();
        StepWalking(dt);
        stats_.walkMs += ElapsedMs(watch);

        watch.Restart();
        traffic_.Step(city_, agents_, routes_, dt, jobs_);
        for (std::uint32_t driver : traffic_.arrivals())
        {
            if (agents_.mode[driver] != static_cast<std::uint8_t>(Mode::Driving)) continue;
            ++stats_.carTripsFinished;
            FinishTrip(driver);
        }
        traffic_.ClearArrivals();
        stats_.trafficMs += ElapsedMs(watch);

        watch.Restart();
        StepMetroPassengers(dt);
        stats_.metroMs += ElapsedMs(watch);
    }

    void Simulation::Step(float simulatedSeconds)
    {
        System::Diagnostics::Stopwatch watch;
        stats_.tripsStarted = 0;
        stats_.routeFailures = 0;
        stats_.walkMs = stats_.crowdMs = stats_.trafficMs = stats_.metroMs = 0.0;

        clock_.Advance(simulatedSeconds);
        weather_.Update(simulatedSeconds, clock_.hour());

        CollectModeLists();

        watch.Restart();
        RunDecisions(simulatedSeconds);
        stats_.decisionMs = ElapsedMs(watch);

        // One decision pass, several movement passes. Sub-stepping the decisions too would cost
        // four times as much and change nothing: nobody's schedule turns over inside half a second.
        const int subSteps = Clamp(static_cast<int>(std::ceil(simulatedSeconds / kMovementStep)),
                                   1, kMaxSubSteps);
        const float h = simulatedSeconds / static_cast<float>(subSteps);
        for (int i = 0; i < subSteps; ++i)
        {
            if (i > 0) CollectModeLists();
            StepMovement(h);
        }
        stats_.subSteps = subSteps;
        stats_.planMs = static_cast<double>(pathfinder_.stats().queries);

        ++tick_;
    }

    Vector3 Simulation::AgentWorldPosition(std::uint32_t agent) const
    {
        const Vec2 ground = agents_.position[agent];
        const auto mode = static_cast<Mode>(agents_.mode[agent]);
        if (mode == Mode::WaitingTrain || mode == Mode::Riding)
            return ToWorld(ground, kMetroDepth);
        return ToWorld(ground, 0.0f);
    }

    std::string Simulation::DescribeAgent(std::uint32_t agent) const
    {
        const auto activity = static_cast<Activity>(agents_.activity[agent]);
        const auto mode = static_cast<Mode>(agents_.mode[agent]);
        const auto profile = static_cast<Profile>(agents_.profile[agent]);
        std::string text = std::string(ProfileName(profile)) + ", " + ActivityName(activity);
        if (mode != Mode::Indoors) text += " (" + std::string(ModeName(mode)) + ")";
        return text;
    }

    std::uint32_t Simulation::PickInterestingAgent(std::uint32_t hint) const
    {
        // Somebody on foot, or nobody. Falling back to a uniformly random citizen was the first
        // version and it is worse than useless: before the first tick the list of people outdoors
        // is empty, so the follow camera's opening shot was reliably a random person asleep in a
        // building, filmed from inside the wall.
        if (walking_.empty()) return kNoIndex;
        return walking_[Hash(hint, static_cast<std::uint32_t>(tick_)) % walking_.size()];
    }

    std::size_t Simulation::MemoryBytes() const
    {
        const std::size_t agentBytes = agents_.size() *
            (sizeof(Vec2) + sizeof(float) * 4 + sizeof(std::uint8_t) * 4 +
             sizeof(std::uint32_t) * 8 + sizeof(std::uint16_t) * 6);
        return agentBytes + routes_.bytes() + pathfinder_.cacheBytes() +
               city_.buildings().size() * sizeof(Building) +
               city_.props().size() * sizeof(Prop) +
               traffic_.vehicles().size() * sizeof(Vehicle);
    }
}
