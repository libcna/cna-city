// SPDX-License-Identifier: MIT
#include "Traffic.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "City.hpp"
#include "JobSystem.hpp"

namespace CnaCity
{
    namespace
    {
        /// Treiber's Intelligent Driver Model, in its published parameterisation.
        constexpr float kDesiredGap   = 2.4f;   ///< s0, the standstill distance, metres.
        constexpr float kHeadwayTime  = 1.25f;  ///< T, the time headway a driver keeps, seconds.
        constexpr float kIdmExponent  = 4.0f;   ///< delta.

        /// The signal cycle, in seconds. Two phases, with an amber and an all-red between them --
        /// the all-red is what stops a car that entered on amber meeting one that entered on the
        /// green it turned into.
        constexpr float kGreenA   = 24.0f;
        constexpr float kAmberA   = 3.0f;
        constexpr float kClearA   = 1.5f;
        constexpr float kGreenB   = 20.0f;
        constexpr float kAmberB   = 3.0f;
        constexpr float kClearB   = 1.5f;
        constexpr float kCycle    = kGreenA + kAmberA + kClearA + kGreenB + kAmberB + kClearB;

        constexpr VehicleProfile kVehicleProfiles[kVehicleKindCount] = {
            /* Car       */ {4.45f, 1.82f, 1.46f, 1.85f, 2.30f, 1.00f},
            /* Hatchback */ {3.95f, 1.74f, 1.50f, 1.70f, 2.30f, 0.97f},
            /* Van       */ {5.40f, 2.02f, 2.35f, 1.30f, 2.00f, 0.92f},
            /* Bus       */ {12.0f, 2.55f, 3.20f, 0.95f, 1.70f, 0.85f},
            /* Truck     */ {9.60f, 2.50f, 3.60f, 0.80f, 1.60f, 0.82f},
            /* Taxi      */ {4.60f, 1.84f, 1.48f, 2.05f, 2.55f, 1.08f},
        };
    }

    const VehicleProfile& ProfileOf(VehicleKind kind)
    {
        return kVehicleProfiles[static_cast<int>(kind)];
    }

    void Traffic::Build(const City& city, std::uint32_t vehicleCapacity)
    {
        roads_ = &city.roads();
        vehicles_.assign(vehicleCapacity, Vehicle{});
        freeList_.resize(vehicleCapacity);
        for (std::uint32_t i = 0; i < vehicleCapacity; ++i)
            freeList_[i] = vehicleCapacity - 1 - i;
        activeCount_ = 0;
        laneOfVehicle_.assign(vehicleCapacity, 0);
        arrivals_.clear();

        // One lane array width for the whole city, taken from the widest class, so that a lane id
        // is arithmetic rather than a lookup. The cost is empty buckets on local streets, which is
        // a few hundred kilobytes of nothing.
        lanesPerDirection_ = 1;
        for (int c = 0; c < kRoadClassCount; ++c)
            lanesPerDirection_ = std::max<std::uint32_t>(lanesPerDirection_,
                                                         ProfileOf(static_cast<RoadClass>(c)).lanesPerSide);

        const std::size_t laneCount = roads_->segments().size() * 2 * lanesPerDirection_;
        laneStart_.assign(laneCount + 1, 0);
        laneItems_.clear();
        laneRearS_.assign(laneCount, 1e9f);

        // ---- Signal phase groups ---------------------------------------------------------------
        //
        // Two phases per junction, and the split is by the *axis* of the approach rather than by
        // its direction: an arm at 10 degrees and its opposite at 190 are the same road and must
        // go green together, or nothing ever crosses the junction.
        signalGroup_.assign(roads_->incident().size(), 0);
        signalOffset_.assign(roads_->nodes().size(), 0.0f);
        for (std::uint32_t n = 0; n < roads_->nodes().size(); ++n)
        {
            const RoadNode& node = roads_->nodes()[n];
            // Deterministic and spread: hashing the node index keeps the offsets stable across
            // runs while making sure neighbouring junctions are not in step.
            const std::uint32_t hash = (n * 2654435761u) >> 8;
            signalOffset_[n] = static_cast<float>(hash % 1000u) * 0.001f * kCycle;
            if (!node.signalised) continue;

            // The busiest arm defines the axis; everything within 45 degrees of it (either way
            // round) joins its phase, and the rest take the other one.
            float axis = 0.0f;
            int bestClass = kRoadClassCount;
            for (std::uint16_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = roads_->incident()[node.firstIncident + k];
                const int cls = static_cast<int>(roads_->segments()[inc.segment].roadClass);
                if (cls < bestClass) { bestClass = cls; axis = inc.heading; }
            }
            for (std::uint16_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = roads_->incident()[node.firstIncident + k];
                const float delta = std::fabs(AngleDelta(axis, inc.heading));
                const bool sameAxis = delta < 0.785f || delta > 2.356f;   // within 45 deg of the axis or its opposite
                signalGroup_[node.firstIncident + k] = sameAxis ? 0 : 1;
            }
        }
    }

    void Traffic::StepSignals(float dt)
    {
        signalClock_ = std::fmod(signalClock_ + dt, kCycle);
    }

    std::uint8_t Traffic::SignalColour(std::uint32_t node, std::uint32_t incidenceSlot) const
    {
        if (!roads_->nodes()[node].signalised) return 2;
        const float t = std::fmod(signalClock_ + signalOffset_[node], kCycle);
        const std::uint8_t group = signalGroup_[incidenceSlot];
        if (group == 0)
        {
            if (t < kGreenA) return 2;
            if (t < kGreenA + kAmberA) return 1;
            return 0;
        }
        const float bStart = kGreenA + kAmberA + kClearA;
        if (t < bStart) return 0;
        if (t < bStart + kGreenB) return 2;
        if (t < bStart + kGreenB + kAmberB) return 1;
        return 0;
    }

    bool Traffic::IsGreen(std::uint32_t node, std::uint32_t incidenceSlot) const
    {
        return SignalColour(node, incidenceSlot) == 2;
    }

    float Traffic::SegmentLength(const City& city, std::uint32_t segment) const
    {
        return city.roads().segments()[segment].length;
    }

    std::uint32_t Traffic::LaneIdOf(const Vehicle& vehicle) const
    {
        return (vehicle.segment * 2u + vehicle.forward) * lanesPerDirection_ + vehicle.lane;
    }

    void Traffic::Placement(const City& city, const Vehicle& vehicle, Vec2& outPosition,
                            float& outHeading) const
    {
        const RoadSegment& segment = city.roads().segments()[vehicle.segment];
        const RoadProfile& profile = ProfileOf(segment.roadClass);
        const Vec2 from = vehicle.forward
                              ? city.roads().nodes()[segment.nodeA].position
                              : city.roads().nodes()[segment.nodeB].position;
        const Vec2 dir = vehicle.forward ? segment.direction : -segment.direction;
        // This city drives on the left. `Perp` of the direction of travel is the vehicle's own
        // right -- checked by putting a camera at `+Perp` from a bus and seeing which side of the
        // frame the bus came out on -- so negating the offset puts the lane centre to the
        // vehicle's left of the road centreline, which is what the markings and the kerbside
        // parking assume. The comment here used to say "right-hand traffic", which was a
        // description of the intent rather than of the code.
        const float offset = profile.laneWidth * (static_cast<float>(vehicle.lane) + 0.5f);
        outPosition = from + dir * vehicle.s - Perp(dir) * offset;
        outHeading = Heading(dir);
    }

    std::uint32_t Traffic::Spawn(const City& city, const Agents& agents, std::uint32_t driver,
                                 const std::uint32_t* path, std::uint32_t pathLength,
                                 std::uint32_t rngBits, Vec2 parkedAt)
    {
        if (freeList_.empty() || pathLength < 2) return kNoIndex;
        const std::uint32_t segment = city.roads().FindSegmentBetween(path[0], path[1]);
        if (segment == 0xFFFFFFFFu) return kNoIndex;

        const RoadSegment& road = city.roads().segments()[segment];
        const std::uint8_t forward = road.nodeA == path[0] ? 1u : 0u;
        const RoadProfile& roadProfile = ProfileOf(road.roadClass);
        const auto lanes = static_cast<std::uint8_t>(std::max<std::uint8_t>(1, roadProfile.lanesPerSide));

        // The car is parked at the kerb outside the building, not teleported to the junction. That
        // is both more truthful and, as it turned out, the fix for a real defect: every car used to
        // start at s = 0.5 on its first segment, so two drivers leaving the same street within a
        // few seconds of each other were spawned *inside* each other. The IDM sees a negative gap,
        // brakes at its maximum for ever, and neither car moves again -- by half past eight some
        // four thousand vehicles were sitting in permanent gridlock at a mean speed of 0.2 m/s,
        // which read as congestion and was not.
        const Vec2 from = forward ? city.roads().nodes()[road.nodeA].position
                                  : city.roads().nodes()[road.nodeB].position;
        const Vec2 direction = forward ? road.direction : -road.direction;
        const float projected = Dot(parkedAt - from, direction);
        const float own = ProfileOf(static_cast<VehicleKind>(
            rngBits % 100u < 46 ? VehicleKind::Car : VehicleKind::Hatchback)).length;
        float startS = Clamp(projected, 1.0f, std::max(1.0f, road.length - own - 1.0f));

        // Find a lane with room at that point. Refusing is a legitimate answer -- the caller then
        // walks -- and it is what keeps a busy street from being handed more cars than it holds.
        std::uint8_t chosenLane = 0xFFu;
        for (std::uint8_t attempt = 0; attempt < lanes; ++attempt)
        {
            const auto lane = static_cast<std::uint8_t>(((rngBits >> 16) + attempt) % lanes);
            const std::uint32_t laneId = (segment * 2u + forward) * lanesPerDirection_ + lane;
            bool room = true;
            if (laneId + 1 < laneStart_.size())
                for (std::uint32_t k = laneStart_[laneId]; k < laneStart_[laneId + 1]; ++k)
                {
                    const Vehicle& other = vehicles_[laneItems_[k]];
                    const float needed = own + ProfileOf(static_cast<VehicleKind>(other.kind)).length + 2.0f;
                    if (std::fabs(other.s - startS) < needed) { room = false; break; }
                }
            if (room && laneId < laneRearS_.size() && std::fabs(laneRearS_[laneId] - startS) < own + 6.0f)
                room = false;
            if (room) { chosenLane = lane; break; }
        }
        if (chosenLane == 0xFFu) return kNoIndex;

        const std::uint32_t index = freeList_.back();
        freeList_.pop_back();
        Vehicle& vehicle = vehicles_[index];
        vehicle = Vehicle{};
        vehicle.driver = driver;
        vehicle.segment = segment;
        vehicle.forward = forward;
        vehicle.lane = chosenLane;
        vehicle.s = startS;
        vehicle.speed = 0.0f;
        vehicle.active = 1;

        // The fleet mix. Lorries are rare on purpose -- roughly one vehicle in twenty -- and that
        // is enough for them to be the thing everybody else is queuing behind.
        //
        // No buses. This used to hand a twelve-metre bus body to one private commuter in
        // twenty-five, so four hundred citizens a day drove a bus to work by themselves while
        // every shelter on every arterial was decoration. A bus is a service, not a body shape;
        // it lives in BusNetwork now and carries passengers.
        const std::uint32_t roll = rngBits % 100u;
        vehicle.kind = static_cast<std::uint8_t>(
            roll < 46 ? VehicleKind::Car
                      : roll < 78 ? VehicleKind::Hatchback
                                  : roll < 90 ? VehicleKind::Van
                                              : roll < 96 ? VehicleKind::Taxi
                                                          : VehicleKind::Truck);
        vehicle.appearance = static_cast<std::uint8_t>((rngBits >> 8) & 0x0Fu);
        (void)agents;

        const std::uint32_t laneId = (segment * 2u + forward) * lanesPerDirection_ + chosenLane;
        if (laneId < laneRearS_.size()) laneRearS_[laneId] = startS;

        ++activeCount_;
        return index;
    }

    void Traffic::Despawn(std::uint32_t vehicleIndex)
    {
        if (vehicleIndex == kNoIndex || !vehicles_[vehicleIndex].active) return;
        vehicles_[vehicleIndex].active = 0;
        vehicles_[vehicleIndex].driver = kNoIndex;
        freeList_.push_back(vehicleIndex);
        --activeCount_;
    }

    void Traffic::RebuildLanes(const City& city)
    {
        const std::size_t laneCount = laneStart_.size() - 1;
        std::fill(laneStart_.begin(), laneStart_.end(), 0u);
        for (const Vehicle& vehicle : vehicles_)
            if (vehicle.active) ++laneStart_[LaneIdOf(vehicle) + 1];
        for (std::size_t i = 1; i <= laneCount; ++i) laneStart_[i] += laneStart_[i - 1];

        laneItems_.resize(laneStart_[laneCount]);
        std::fill(laneRearS_.begin(), laneRearS_.end(), 1e9f);
        laneCursor_.assign(laneStart_.begin(), laneStart_.end() - 1);
        for (std::uint32_t i = 0; i < vehicles_.size(); ++i)
            if (vehicles_[i].active)
            {
                const std::uint32_t lane = LaneIdOf(vehicles_[i]);
                laneOfVehicle_[i] = lane;
                laneItems_[laneCursor_[lane]++] = i;
            }
        (void)city;
    }

    void Traffic::Step(const City& city, Agents& agents, const RoutePool& routes, float dt,
                       JobSystem& jobs)
    {
        if (vehicles_.empty()) return;
        RebuildLanes(city);

        const std::size_t laneCount = laneStart_.size() - 1;
        std::atomic<std::uint32_t> blocked{0};
        std::atomic<std::uint64_t> speedSum{0};
        junctionQueue_.clear();

        // Every lane is independent: a vehicle is in exactly one of them, only its own lane's
        // worker touches it, and the junction step that *would* couple two lanes is deferred to
        // the serial pass below. That is what makes the parallel split safe without a single lock.
        jobs.ParallelFor(laneCount, 256, [&](std::size_t begin, std::size_t end) {
            std::uint32_t localBlocked = 0;
            std::uint64_t localSpeed = 0;
            for (std::size_t lane = begin; lane < end; ++lane)
            {
                const std::uint32_t first = laneStart_[lane];
                const std::uint32_t last = laneStart_[lane + 1];
                if (first == last) continue;

                // Sorted front-to-back so the leader of item k is item k+1. Insertion sort because
                // the order is almost always already correct: vehicles only overtake at junctions.
                for (std::uint32_t i = first + 1; i < last; ++i)
                {
                    const std::uint32_t value = laneItems_[i];
                    const float key = vehicles_[value].s;
                    std::uint32_t j = i;
                    while (j > first && vehicles_[laneItems_[j - 1]].s > key)
                    {
                        laneItems_[j] = laneItems_[j - 1];
                        --j;
                    }
                    laneItems_[j] = value;
                }

                for (std::uint32_t i = first; i < last; ++i)
                {
                    const std::uint32_t index = laneItems_[i];
                    Vehicle& vehicle = vehicles_[index];
                    const RoadSegment& segment = city.roads().segments()[vehicle.segment];
                    const VehicleProfile& profile = ProfileOf(static_cast<VehicleKind>(vehicle.kind));
                    const RoadProfile& roadProfile = ProfileOf(segment.roadClass);
                    const float desired = roadProfile.speedLimit * profile.speedFactor;

                    // The obstacle is whichever comes first: the vehicle ahead, or the stop line.
                    float gap = 1e6f;
                    float leaderSpeed = desired;
                    if (i + 1 < last)
                    {
                        const Vehicle& leader = vehicles_[laneItems_[i + 1]];
                        const float leaderLength = ProfileOf(static_cast<VehicleKind>(leader.kind)).length;
                        gap = leader.s - leaderLength - vehicle.s;
                        leaderSpeed = leader.speed;
                        // A negative gap means the two have ended up inside each other, which the
                        // IDM cannot recover from on its own: it answers with maximum braking for
                        // ever and the pair never separates. Pushing the follower back is the one
                        // place this model needs a positional correction rather than a force.
                        if (gap < 0.0f)
                        {
                            vehicle.s = std::max(0.0f, leader.s - leaderLength - 0.5f);
                            gap = 0.5f;
                            vehicle.speed = std::min(vehicle.speed, leader.speed);
                        }
                    }

                    const std::uint32_t exitNode = vehicle.forward ? segment.nodeB : segment.nodeA;
                    const std::uint32_t slot = city.roads().IncidenceSlot(exitNode, vehicle.segment);
                    const bool mustStop = city.roads().nodes()[exitNode].signalised &&
                                          slot != 0xFFFFFFFFu && !IsGreen(exitNode, slot);
                    if (mustStop)
                    {
                        const float toStopLine = segment.length - roadProfile.carriagewayHalfWidth -
                                                 vehicle.s;
                        if (toStopLine < gap) { gap = toStopLine; leaderSpeed = 0.0f; }
                    }

                    // IDM proper. The two terms are "how far below my desired speed am I" and
                    // "how much closer than I would like am I to the thing in front"; the second
                    // is what produces a queue rather than a pile-up.
                    const float speedRatio = vehicle.speed / std::max(0.5f, desired);
                    const float approach = vehicle.speed - leaderSpeed;
                    const float desiredGap =
                        kDesiredGap + std::max(0.0f, vehicle.speed * kHeadwayTime +
                                                         vehicle.speed * approach /
                                                             (2.0f * std::sqrt(profile.maxAcceleration *
                                                                               profile.comfortBraking)));
                    const float safeGap = std::max(0.35f, gap);
                    const float gapRatio = desiredGap / safeGap;
                    float acceleration = profile.maxAcceleration *
                                         (1.0f - std::pow(speedRatio, kIdmExponent) - gapRatio * gapRatio);
                    acceleration = Clamp(acceleration, -8.0f, profile.maxAcceleration);

                    vehicle.acceleration = acceleration;
                    vehicle.speed = std::max(0.0f, vehicle.speed + acceleration * dt);
                    vehicle.s += vehicle.speed * dt;
                    vehicle.blocked = acceleration < -1.5f ? 1 : 0;
                    if (mustStop) vehicle.stall = 1; else if (vehicle.stall == 1) vehicle.stall = 0;
                    vehicle.stuckSeconds = vehicle.speed < 0.15f ? vehicle.stuckSeconds + dt : 0.0f;
                    localBlocked += vehicle.blocked;
                    localSpeed += static_cast<std::uint64_t>(vehicle.speed * 100.0f);

                    // Never past the stop line, and never past the far node: crossing a junction
                    // is a decision, and decisions are made serially below.
                    const float limit = mustStop ? segment.length - roadProfile.carriagewayHalfWidth
                                                 : segment.length;
                    if (vehicle.s > limit)
                    {
                        vehicle.s = limit;
                        // Only a car held at a stop line loses its speed. Clamping it at every
                        // junction, green or not, was costing every vehicle its momentum roughly
                        // once per block and halving the achievable mean speed.
                        if (mustStop) vehicle.speed = 0.0f;
                    }
                }
            }
            blocked.fetch_add(localBlocked, std::memory_order_relaxed);
            speedSum.fetch_add(localSpeed, std::memory_order_relaxed);
        });

        blockedCount_ = blocked.load();
        meanSpeed_ = activeCount_ > 0
                         ? static_cast<float>(speedSum.load()) / (100.0f * static_cast<float>(activeCount_))
                         : 0.0f;

        AdvanceOverJunctions(city, agents, routes);
    }

    void Traffic::AdvanceOverJunctions(const City& city, Agents& agents, const RoutePool& routes)
    {
        const RoadNetwork& roads = city.roads();
        for (std::uint32_t index = 0; index < vehicles_.size(); ++index)
        {
            Vehicle& vehicle = vehicles_[index];
            if (!vehicle.active) continue;

            // The safety valve. Nothing in a microscopic traffic model guarantees that a cycle of
            // vehicles each waiting for the next one cannot form, and once it has, no local rule
            // dissolves it -- that is what a gridlock *is*. Rather than pretend it cannot happen,
            // a vehicle that has not moved for four simulated minutes gives up: the driver is
            // handed back to the simulation, which finishes the trip on foot, and the count is
            // reported on the HUD. A number that climbs is a real result about the network, and
            // hiding it would defeat the point of the program.
            if (vehicle.stuckSeconds > 240.0f)
            {
                ++gridlocked_;
                arrivals_.push_back(vehicle.driver);
                continue;
            }
            const RoadSegment& segment = roads.segments()[vehicle.segment];
            if (vehicle.s < segment.length - 0.25f) continue;

            const std::uint32_t driver = vehicle.driver;
            const std::uint32_t slot = agents.pathSlot[driver];
            if (slot == kNoIndex)
            {
                arrivals_.push_back(driver);
                continue;
            }
            const std::uint32_t* path = routes.At(slot);
            std::uint16_t cursor = agents.pathCursor[driver];
            const std::uint16_t length = agents.pathLength[driver];

            // The junction just reached is path[cursor + 1]; the leg after it is cursor + 2.
            if (static_cast<std::uint32_t>(cursor) + 2u >= length)
            {
                arrivals_.push_back(driver);
                continue;
            }

            const std::uint32_t exitNode = vehicle.forward ? segment.nodeB : segment.nodeA;
            const std::uint32_t nextSegment = roads.FindSegmentBetween(exitNode, path[cursor + 2]);
            if (nextSegment == 0xFFFFFFFFu)
            {
                vehicle.stall = 3;
                arrivals_.push_back(driver);
                continue;
            }

            // Room to enter? The lane the vehicle is joining has to have space at its head, or the
            // vehicle waits at the line -- which is what makes a jam propagate backwards through a
            // junction instead of vehicles teleporting into a solid queue.
            const RoadSegment& next = roads.segments()[nextSegment];
            const std::uint8_t forward = next.nodeA == exitNode ? 1u : 0u;
            const RoadProfile& nextProfile = ProfileOf(next.roadClass);
            const std::uint8_t lane = static_cast<std::uint8_t>(
                std::min<std::uint32_t>(vehicle.lane, std::max<std::uint8_t>(1, nextProfile.lanesPerSide) - 1));
            const std::uint32_t laneId = (nextSegment * 2u + forward) * lanesPerDirection_ + lane;
            bool clear = true;
            if (laneId + 1 < laneStart_.size())
                for (std::uint32_t k = laneStart_[laneId]; k < laneStart_[laneId + 1]; ++k)
                {
                    const Vehicle& other = vehicles_[laneItems_[k]];
                    const float need = ProfileOf(static_cast<VehicleKind>(vehicle.kind)).length + 2.0f;
                    if (other.s < need) { clear = false; break; }
                }
            if (!clear)
            {
                vehicle.s = segment.length - 0.3f;
                vehicle.speed = 0.0f;
                vehicle.blocked = 1;
                vehicle.stall = 2;
                continue;
            }
            vehicle.stall = 0;

            vehicle.segment = nextSegment;
            vehicle.forward = forward;
            vehicle.lane = lane;
            vehicle.s = 0.0f;
            agents.pathCursor[driver] = static_cast<std::uint16_t>(cursor + 1);
        }
    }
}
