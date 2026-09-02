// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "CityMath.hpp"

namespace CnaCity
{
    class City;

    /** @brief The depth the whole system sits at. Flat: this demo has no gradients underground. */
    inline constexpr float kMetroDepth = -11.5f;

    inline constexpr std::uint32_t kNoStation = 0xFFFFFFFFu;

    struct MetroStation
    {
        Vec2 position{0.0f, 0.0f};      ///< The platform centre, directly under @ref entrance.
        Vec2 entrance{0.0f, 0.0f};      ///< Street level, on the pavement.
        std::uint32_t doorNode = 0;     ///< The road node an arriving pedestrian aims for.
        std::string name;
        /// Every (line, index-on-line) this platform serves. An interchange has more than one, and
        /// the router treats changing between them as free, which is what an interchange is.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> lines;
    };

    struct MetroLine
    {
        std::vector<std::uint32_t> stations;   ///< In order along the line.
        std::vector<Vec2> points;              ///< Station positions, i.e. the track polyline.
        std::vector<float> distance;           ///< Cumulative distance to each station, metres.
        float length = 0.0f;
        bool loop = false;
        std::uint8_t colorIndex = 0;
    };

    /** @brief One train, somewhere along its line. */
    struct MetroTrain
    {
        std::uint32_t line = 0;
        float position = 0.0f;      ///< Metres along the line from its start.
        float speed = 0.0f;         ///< m/s, signed by @ref direction.
        int direction = 1;          ///< +1 toward the far terminus, -1 back.
        int nextStation = 0;        ///< Index into MetroLine::stations.
        float dwellRemaining = 0.0f;
        std::uint32_t onboard = 0;
        std::uint32_t capacity = 900;
    };

    /**
     * @brief The underground: lines, stations, timetabled trains and the router over them.
     *
     * The metro exists because a hundred thousand people who all walk or drive is not a city, it
     * is a traffic jam. It also happens to be the subsystem that makes the follow camera worth
     * having: the most interesting thirty seconds of a citizen's day is the bit where they
     * disappear down a staircase and come up two kilometres away.
     */
    class MetroNetwork
    {
    public:
        void Generate(const City& city, int lineCount, std::uint64_t seed);

        /** @brief Advances every train by @p dt seconds. */
        void Step(float dt);

        [[nodiscard]] const std::vector<MetroStation>& stations() const { return stations_; }
        [[nodiscard]] const std::vector<MetroLine>& lines() const { return lines_; }
        [[nodiscard]] const std::vector<MetroTrain>& trains() const { return trains_; }
        [[nodiscard]] std::vector<MetroTrain>& mutableTrains() { return trains_; }

        /** @brief The world position of a point @p metres along @p line. */
        [[nodiscard]] Vec2 PointOnLine(std::uint32_t line, float distance) const;

        /** @brief The station nearest @p point, or @ref kNoStation when there are none. */
        [[nodiscard]] std::uint32_t NearestStation(Vec2 point) const;

        /**
         * @brief The best station pair for a trip, or false when the metro cannot help.
         *
         * "Cannot help" is a real answer and the caller must handle it: a trip inside one district
         * is quicker on foot, and the outer suburbs are deliberately not served.
         */
        struct Route
        {
            std::uint32_t boardStation = kNoStation;
            std::uint32_t alightStation = kNoStation;
            std::uint32_t transferStation = kNoStation;  ///< kNoStation when the trip is direct.
            float rideDistance = 0.0f;
        };
        [[nodiscard]] bool PlanRoute(Vec2 from, Vec2 to, Route& route) const;

        /** @brief The line index a passenger at @p from takes toward @p to, or 0xFFFFFFFF. */
        [[nodiscard]] std::uint32_t LineBetween(std::uint32_t fromStation, std::uint32_t toStation,
                                                int* outDirection) const;

    private:
        void BuildStationGraph();

        std::vector<MetroStation> stations_;
        std::vector<MetroLine> lines_;
        std::vector<MetroTrain> trains_;
        /// Straight-line distance between every pair of stations that share a line, plus zero-cost
        /// interchange edges. Sixty-odd stations makes the dense matrix both the simplest and the
        /// fastest structure, and it is built once.
        std::vector<float> pairCost_;
        std::vector<std::uint32_t> pairVia_;
        std::size_t stationCount_ = 0;
    };
}
