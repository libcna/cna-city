// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "Simulation.hpp"

namespace CnaCity
{
    /**
     * @brief Saving and restoring a moment in the city's day.
     *
     * The reason this exists is the benchmark. Measuring the morning peak means simulating up to
     * the morning peak first, and at a hundred thousand citizens that is a couple of minutes of
     * warm-up before the first number -- paid again for every scale, every renderer and every run.
     * A snapshot pays it once.
     *
     * It stores what cannot be recomputed and nothing else. The streets, the buildings, the metro
     * lines and the bus routes are a pure function of the seed, so what goes in the file is the
     * population, the traffic, the fleets, the clock, the weather, every random generator's state
     * and the queues at the platforms -- and the *digest* of the city, so that a snapshot taken
     * against a generator that has since changed is refused rather than loaded into a world whose
     * roads have moved under its traffic.
     */

    /** @brief What a snapshot says about itself before it is loaded. */
    struct SnapshotInfo
    {
        SimConfig config;
        std::uint64_t cityChecksum = 0;
        std::uint64_t tick = 0;
        float hour = 0.0f;
        int day = 0;
        std::uint32_t travelling = 0;   ///< Citizens outdoors when it was taken; a scenario's label.
        std::string note;               ///< Free text: "morning rush", "rain gridlock".
    };

    /**
     * @brief Writes @p sim to @p path.
     *
     * @param note Recorded in the header so a directory of scenarios describes itself.
     * @return False on failure, with @p error filled.
     */
    [[nodiscard]] bool SaveSnapshot(const std::string& path, const Simulation& sim,
                                    const std::string& note, std::string& error);

    /**
     * @brief Reads @p path into @p sim, regenerating the city from the snapshot's own seed.
     *
     * The configuration comes from the file rather than from the command line: a snapshot is a
     * moment in a particular city, and loading it into a different one is not a thing that can be
     * done.
     */
    [[nodiscard]] bool LoadSnapshot(const std::string& path, Simulation& sim, std::string& error);

    /** @brief Reads the header only, without generating a city. */
    [[nodiscard]] bool ReadSnapshotInfo(const std::string& path, SnapshotInfo& info,
                                        std::string& error);
}
