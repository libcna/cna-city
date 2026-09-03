// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Checksum.hpp"
#include "Simulation.hpp"

namespace CnaCity
{
    /**
     * @brief Recording and replaying a run of the city.
     *
     * A replay file holds almost nothing, and that is the whole point of it. The simulation is a
     * pure function of its configuration and its input, and `Step` banks the frame's elapsed time
     * and advances the world in whole ticks -- so "what happened" is entirely described by the seed,
     * the configuration, how many ticks ran, and the handful of moments somebody pressed a key.
     * The state is not stored because it does not need to be: it can be recomputed exactly.
     *
     * That makes a replay a few hundred bytes for a simulated day, and it makes it a *test* rather
     * than a video: replaying compares checkpoints as it goes, so a file that no longer reproduces
     * says which tick it stopped agreeing on and which half of the world stopped agreeing first.
     */

    /** @brief Something outside the simulation that changed it, and when. */
    struct ReplayEvent
    {
        enum class Kind : std::uint8_t
        {
            Weather,   ///< The weather was forced to @ref value, cast to WeatherKind.
            Hour       ///< The clock was moved to @ref value hours.
        };

        std::uint64_t tick = 0;
        Kind kind = Kind::Weather;
        float value = 0.0f;
    };

    /** @brief A digest taken at a known tick, so a divergence can be bisected rather than hunted. */
    struct ReplayCheckpoint
    {
        std::uint64_t tick = 0;
        WorldChecksum checksum;
    };

    struct ReplayFile
    {
        SimConfig config;
        std::uint64_t ticks = 0;
        std::vector<ReplayEvent> events;
        std::vector<ReplayCheckpoint> checkpoints;
    };

    /**
     * @brief Writes a replay as it happens.
     *
     * Held open for the length of the run and closed with the final tick count, because that count
     * is not known until the run ends and it is what the player replays.
     */
    class ReplayRecorder
    {
    public:
        ~ReplayRecorder();

        /** @brief Starts recording @p config to @p path. Returns false and sets @ref error. */
        bool Open(const std::string& path, const SimConfig& config);
        [[nodiscard]] bool recording() const { return open_; }
        [[nodiscard]] const std::string& error() const { return error_; }

        void RecordWeather(std::uint64_t tick, WeatherKind kind);
        void RecordHour(std::uint64_t tick, float hour);

        /**
         * @brief Takes a checkpoint if @p tick is due one.
         *
         * Checkpoints are what turn "this no longer reproduces" into "this stopped reproducing
         * between tick 41 000 and tick 42 000, in the traffic". Cheap enough to take often, since
         * a digest of a hundred thousand agents is one pass over three arrays.
         */
        void MaybeCheckpoint(const Simulation& sim, std::uint64_t interval);

        /** @brief Writes the file. Safe to call twice; the second does nothing. */
        void Close(const Simulation& sim);

    private:
        std::string path_;
        bool open_ = false;
        bool closed_ = false;
        std::string error_;
        ReplayFile file_;
    };

    /** @brief Reads a replay file. On failure fills @p error and returns false. */
    [[nodiscard]] bool LoadReplay(const std::string& path, ReplayFile& out, std::string& error);

    /** @brief Writes a replay file. On failure fills @p error and returns false. */
    [[nodiscard]] bool SaveReplay(const std::string& path, const ReplayFile& file,
                                  std::string& error);

    /** @brief What a replay run concluded. */
    struct ReplayResult
    {
        bool reproduced = false;
        std::uint64_t divergedAtTick = 0;
        std::string divergedIn;          ///< "city", "agents", "traffic", "transit" or "world".
        WorldChecksum expected;
        WorldChecksum actual;
        std::uint64_t checkpointsChecked = 0;
    };

    /**
     * @brief Re-runs @p file and compares every checkpoint in it.
     *
     * Stops at the first disagreement, because everything after it is downstream of the same
     * cause and reporting a thousand mismatches hides the one that matters.
     */
    [[nodiscard]] ReplayResult RunReplay(const ReplayFile& file);
}
