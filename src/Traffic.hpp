// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "Agents.hpp"
#include "Archive.hpp"
#include "CityMath.hpp"
#include "RoadNetwork.hpp"

namespace CnaCity
{
    class City;
    class JobSystem;

    /** @brief The body shapes on the road. Each is one instanced draw call per colour bucket. */
    enum class VehicleKind : std::uint8_t
    {
        Car = 0,
        Hatchback,
        Van,
        Bus,
        Truck,
        Taxi
    };

    inline constexpr int kVehicleKindCount = 6;

    /** @brief Physical dimensions in metres, and the driving behaviour that goes with them. */
    struct VehicleProfile
    {
        float length;
        float width;
        float height;
        float maxAcceleration;   ///< m/s^2
        float comfortBraking;    ///< m/s^2
        float speedFactor;       ///< Of the limit; a truck does not do the arterial's 50.
    };

    [[nodiscard]] const VehicleProfile& ProfileOf(VehicleKind kind);

    /**
     * @brief One vehicle, on one lane of one directed road segment.
     *
     * Position is `(segment, forward, lane, s)` rather than a world point: car-following needs the
     * gap to the vehicle in front measured *along the lane*, and a world-space distance between
     * two cars either side of a bend is not that gap.
     */
    struct Vehicle
    {
        std::uint32_t driver = kNoIndex;
        std::uint32_t segment = kNoIndex;
        float s = 0.0f;              ///< Metres travelled along the directed segment.
        float speed = 0.0f;
        float acceleration = 0.0f;
        std::uint8_t forward = 1;    ///< 1 when travelling A to B.
        std::uint8_t lane = 0;
        std::uint8_t kind = 0;
        std::uint8_t appearance = 0; ///< Colour bucket; the renderer groups on it.
        std::uint8_t blocked = 0;    ///< Set when the last tick's acceleration was braking hard.
        std::uint8_t active = 0;
        std::uint8_t stall = 0;      ///< 0 running, 1 red light, 2 junction occupied, 3 no next segment.
        float stuckSeconds = 0.0f;   ///< How long this vehicle has been at a standstill.
    };

    /**
     * @brief Everything that happens on the carriageway: signals, car-following, junctions.
     *
     * The car-following model is Treiber's Intelligent Driver Model, unchanged from the published
     * form. It is used rather than something simpler because it is the cheapest model that produces
     * the two behaviours a city demo lives or dies by: a queue that forms behind a red light and
     * discharges in a wave when it turns green, and stop-and-go waves that appear on a busy road
     * with no obstacle at all.
     */
    class Traffic
    {
    public:
        void Build(const City& city, std::uint32_t vehicleCapacity);

        /** @brief Advances the signal cycle. Cheap, and deliberately separate from the vehicles. */
        void StepSignals(float dt);

        /** @brief One vehicle tick: car-following in parallel, then junctions in order. */
        void Step(const City& city, Agents& agents, const RoutePool& routes, float dt,
                  JobSystem& jobs);

        /**
         * @brief Drivers whose vehicle finished its route since the last call.
         *
         * Traffic parks the car and stops there: what happens next -- walking the last few metres,
         * going indoors, starting the next leg -- is the simulation's decision, not the road's.
         */
        [[nodiscard]] const std::vector<std::uint32_t>& arrivals() const { return arrivals_; }
        void ClearArrivals() { arrivals_.clear(); }

        /**
         * @brief Puts @p driver on the road at the start of their route.
         * @return The vehicle index, or kNoIndex when the fleet is full or the route is unusable.
         */
        std::uint32_t Spawn(const City& city, const Agents& agents, std::uint32_t driver,
                            const std::uint32_t* path, std::uint32_t pathLength,
                            std::uint32_t rngBits, Vec2 parkedAt);

        void Despawn(std::uint32_t vehicleIndex);

        [[nodiscard]] const std::vector<Vehicle>& vehicles() const { return vehicles_; }
        [[nodiscard]] std::uint32_t activeCount() const { return activeCount_; }
        [[nodiscard]] std::uint32_t blockedCount() const { return blockedCount_; }
        [[nodiscard]] float meanSpeed() const { return meanSpeed_; }
        /** @brief Vehicles abandoned because they had not moved for minutes. See Traffic::Step. */
        [[nodiscard]] std::uint64_t gridlockedCount() const { return gridlocked_; }

        /** @brief World position and heading of a vehicle, for the renderer. */
        void Placement(const City& city, const Vehicle& vehicle, Vec2& outPosition,
                       float& outHeading) const;

        /** @brief True when the approach at @p incidenceSlot currently has a green. */
        [[nodiscard]] bool IsGreen(std::uint32_t node, std::uint32_t incidenceSlot) const;

        /** @brief The signal colour at a junction arm, for the renderer: 0 red, 1 amber, 2 green. */
        [[nodiscard]] std::uint8_t SignalColour(std::uint32_t node, std::uint32_t incidenceSlot) const;

        [[nodiscard]] float signalClock() const { return signalClock_; }

        /**
         * @brief Reads or writes the fleet and the signals.
         *
         * The lane buckets are *not* stored: they are rebuilt from the vehicles every sub-step
         * anyway, so storing them would be storing a cache -- and a stale one is the kind of thing
         * that loads without complaint and then puts two cars in the same place.
         */
        void Serialize(Archive& archive);

    private:
        void RebuildLanes(const City& city);
        [[nodiscard]] std::uint32_t LaneIdOf(const Vehicle& vehicle) const;
        [[nodiscard]] float SegmentLength(const City& city, std::uint32_t segment) const;
        void AdvanceOverJunctions(const City& city, Agents& agents, const RoutePool& routes);

        const RoadNetwork* roads_ = nullptr;
        std::vector<Vehicle> vehicles_;
        std::vector<std::uint32_t> freeList_;
        std::uint32_t activeCount_ = 0;
        std::uint32_t blockedCount_ = 0;
        float meanSpeed_ = 0.0f;
        std::uint64_t gridlocked_ = 0;

        /// A CSR bucketing of vehicles by lane, rebuilt every tick and sorted by `s`. Rebuilding
        /// rather than maintaining is the right call at this scale: ten thousand vehicles is one
        /// counting sort plus a short insertion sort per lane, and it removes every ordering bug a
        /// maintained structure would have.
        std::uint32_t lanesPerDirection_ = 1;
        std::vector<std::uint32_t> laneStart_;
        std::vector<std::uint32_t> laneItems_;
        std::vector<std::uint32_t> laneOfVehicle_;
        /// The rearmost vehicle on each lane, so a car can be parked behind it rather than inside
        /// it. Refreshed every tick from the buckets and updated as cars are spawned, because
        /// several may join the same street between two ticks.
        std::vector<float> laneRearS_;
        /// Counting-sort cursor, kept rather than rebuilt: RebuildLanes runs every sub-step.
        std::vector<std::uint32_t> laneCursor_;

        /// Per incidence: which of the two signal phases the approach belongs to.
        std::vector<std::uint8_t> signalGroup_;
        /// Per node: where in the cycle this junction is. Randomised, so the city does not blink
        /// in unison -- and so that a green wave along an arterial is something you could build
        /// deliberately rather than something that happens by accident.
        std::vector<float> signalOffset_;
        float signalClock_ = 0.0f;

        std::vector<std::uint32_t> junctionQueue_;
        std::vector<std::uint32_t> arrivals_;
    };
}
