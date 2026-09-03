// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace CnaCity
{
    class Simulation;

    /**
     * @brief A digest of the whole world, split so that a mismatch says *where*.
     *
     * A single number over everything answers "did this run reproduce" and nothing else. Five
     * answer the next question too: a city that differs means the generator moved, agents alone
     * means the schedule or the steering did, traffic alone means the road model did, and transit
     * alone means the metro or the buses did. That is the difference between a failing check and a
     * lead.
     */
    struct WorldChecksum
    {
        std::uint64_t city = 0;      ///< Roads, blocks, buildings, props, and both transit layouts.
        std::uint64_t agents = 0;    ///< Every citizen's position, mode and activity.
        std::uint64_t traffic = 0;   ///< Every active vehicle's lane, offset and speed.
        std::uint64_t transit = 0;   ///< Every train and bus, and what it is carrying.
        std::uint64_t world = 0;     ///< The clock and the weather.
        std::uint64_t total = 0;     ///< All of the above, in order.

        [[nodiscard]] bool operator==(const WorldChecksum&) const = default;
    };

    /**
     * @brief Digests @p sim as it stands.
     *
     * Floats are quantised to a centimetre before they are hashed. Hashing the bits instead would
     * make the check fail on a compiler flag, an FMA contraction or a different libm, none of
     * which is the thing being tested -- and a centimetre is two orders of magnitude below
     * anything this simulation decides on.
     */
    [[nodiscard]] WorldChecksum ComputeChecksum(const Simulation& sim);

    /** @brief The static half alone, which is a pure function of the seed. */
    [[nodiscard]] std::uint64_t ComputeCityChecksum(const Simulation& sim);

    /** @brief Sixteen lowercase hex digits, for printing and for the replay file. */
    [[nodiscard]] std::string ToHex(std::uint64_t value);

    /** @brief Parses what @ref ToHex wrote. Returns false on anything else. */
    [[nodiscard]] bool FromHex(const std::string& text, std::uint64_t& value);
}
