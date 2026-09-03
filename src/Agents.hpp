// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "CityMath.hpp"

namespace CnaCity
{
    /** @brief How many junctions of a route are kept resident; longer routes are re-planned en route. */
    inline constexpr std::uint32_t kMaxPathNodes = 64;
    inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

    /** @brief What a citizen is doing, which is the part of the state a HUD would report. */
    enum class Activity : std::uint8_t
    {
        Asleep = 0,
        AtHome,
        ToWork,
        AtWork,
        ToLunch,
        AtLunch,
        ToLeisure,
        AtLeisure,
        ToHome
    };

    inline constexpr int kActivityCount = 9;

    /** @brief How they are getting there, which is the part the renderer and the traffic model care about. */
    enum class Mode : std::uint8_t
    {
        Indoors = 0,     ///< Inside a building; not drawn, not stepped.
        Walking,
        Driving,
        WaitingTrain,    ///< On a platform, underground.
        Riding,          ///< On a train, moving with it.
        WaitingBus,      ///< At a stop on the pavement.
        OnBus            ///< Aboard, moving with the bus.
    };

    /** @brief The demographic, which decides the daily schedule and whether they own a car. */
    enum class Profile : std::uint8_t
    {
        Worker = 0,
        ShiftWorker,
        Student,
        Retired,
        Child,
        Unemployed
    };

    inline constexpr int kProfileCount = 6;

    [[nodiscard]] const char* ActivityName(Activity activity);
    [[nodiscard]] const char* ModeName(Mode mode);
    [[nodiscard]] const char* ProfileName(Profile profile);

    /**
     * @brief Fixed-size route slots, handed out for the duration of a trip.
     *
     * A route is a list of junctions and it lives only while somebody is following it, so the
     * natural structure is a pool: no allocation on the tick path, no fragmentation, and a hard
     * ceiling on what routes can cost in memory. The ceiling is real -- @ref kMaxPathNodes
     * junctions -- and a route longer than that is followed to its end and then re-planned from
     * there, which costs one extra query on the small number of trips that cross the whole city.
     */
    class RoutePool
    {
    public:
        void Reset(std::size_t slots);

        /** @return A free slot, or kNoIndex when the pool is exhausted. */
        [[nodiscard]] std::uint32_t Acquire();
        void Release(std::uint32_t slot);

        [[nodiscard]] std::uint32_t* At(std::uint32_t slot)
        {
            return nodes_.data() + static_cast<std::size_t>(slot) * kMaxPathNodes;
        }
        [[nodiscard]] const std::uint32_t* At(std::uint32_t slot) const
        {
            return nodes_.data() + static_cast<std::size_t>(slot) * kMaxPathNodes;
        }

        [[nodiscard]] std::size_t capacity() const { return slotCount_; }
        [[nodiscard]] std::size_t inUse() const { return slotCount_ - free_.size(); }
        [[nodiscard]] std::size_t exhaustedCount() const { return exhausted_; }
        [[nodiscard]] std::size_t bytes() const { return nodes_.size() * sizeof(std::uint32_t); }

    private:
        std::vector<std::uint32_t> nodes_;
        std::vector<std::uint32_t> free_;
        std::size_t slotCount_ = 0;
        std::size_t exhausted_ = 0;
    };

    /**
     * @brief A hundred thousand citizens, as arrays rather than as objects.
     *
     * Struct-of-arrays is not decoration here: the tick touches `position`, `heading` and `speed`
     * for every moving agent and nothing else, and an array-of-structs would pull ninety bytes of
     * schedule and route state through the cache for each of them. The layout is the reason the
     * headline number is a hundred thousand rather than ten.
     *
     * The fields are grouped by when they are read: the movement block is the hot one, the plan
     * block is touched when a trip starts or ends, and the schedule block is read once per agent
     * per simulated day.
     */
    struct Agents
    {
        // --- Movement: read and written every tick for every moving agent ----------------------
        std::vector<Vec2> position;
        std::vector<float> heading;        ///< Yaw of travel, radians.
        std::vector<float> speed;          ///< Current speed, m/s.
        std::vector<float> desiredSpeed;   ///< Free-walking speed; varies per person and with age.
        std::vector<float> animationPhase; ///< Accumulated stride, so the walk cycle is continuous.
        std::vector<std::uint8_t> mode;
        std::vector<std::uint8_t> activity;

        // --- Plan: read when a leg starts or finishes -------------------------------------------
        std::vector<std::uint32_t> pathSlot;     ///< Index into the route pool, or kNoIndex.
        std::vector<std::uint16_t> pathLength;
        std::vector<std::uint16_t> pathCursor;   ///< The node being walked or driven toward.
        std::vector<std::uint32_t> targetBuilding;
        std::vector<std::uint32_t> vehicle;      ///< Index into Traffic's vehicle array, or kNoIndex.
        std::vector<std::uint32_t> metroBoard;
        std::vector<std::uint32_t> metroAlight;
        std::vector<std::uint32_t> metroTrain;
        std::vector<std::uint32_t> busBoard;     ///< Stop being walked to, or kNoIndex.
        std::vector<std::uint32_t> busAlight;
        std::vector<std::uint32_t> busVehicle;   ///< Index into BusNetwork's fleet, or kNoIndex.
        std::vector<float> waitTimer;            ///< Seconds left indoors, or on a platform.

        // --- Identity and schedule: read once per day -------------------------------------------
        std::vector<std::uint32_t> home;
        std::vector<std::uint32_t> work;
        std::vector<std::uint32_t> haunt;        ///< The evening destination: a shop, bar or park.
        std::vector<std::uint16_t> leaveHomeMinute;
        std::vector<std::uint16_t> leaveWorkMinute;
        std::vector<std::uint16_t> lunchMinute;
        std::vector<std::uint16_t> bedMinute;
        std::vector<std::uint8_t> profile;
        /// Bit 0: owns a car. Bit 1: went out this evening. Bits 4..7: appearance bucket, which is
        /// how the renderer groups a hundred thousand people into a few dozen draw calls.
        std::vector<std::uint8_t> flags;

        [[nodiscard]] std::size_t size() const { return position.size(); }

        void Resize(std::size_t count);

        [[nodiscard]] bool OwnsCar(std::size_t i) const { return (flags[i] & 0x01u) != 0; }
        [[nodiscard]] std::uint8_t Appearance(std::size_t i) const { return flags[i] >> 4; }
        [[nodiscard]] bool IsOutdoors(std::size_t i) const
        {
            return mode[i] == static_cast<std::uint8_t>(Mode::Walking) ||
                   mode[i] == static_cast<std::uint8_t>(Mode::Driving);
        }
    };
}
