// SPDX-License-Identifier: MIT
#include "MetroNetwork.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "City.hpp"
#include "Rng.hpp"

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kStationSpacing = 560.0f;   ///< Metres; typical for an urban metro.
        constexpr float kCruiseSpeed    = 21.0f;    ///< ~75 km/h between stations.
        constexpr float kAcceleration   = 1.15f;    ///< m/s^2, and the same figure for braking.
        constexpr float kDwellSeconds   = 26.0f;
        constexpr float kHeadwaySeconds = 165.0f;
        constexpr float kInterchangeRadius = 130.0f;

        const char* const kStationSuffix[] = {
            "Central", "Cross", "Gate", "Park", "Square", "Bridge", "Market", "North",
            "South", "East", "West", "Junction", "Quay", "Green"};
    }

    void MetroNetwork::Generate(const City& city, int lineCount, std::uint64_t seed)
    {
        stations_.clear();
        lines_.clear();
        trains_.clear();
        if (lineCount <= 0) return;

        Rng rng(seed, 0x4d45'5452'4f00'0001ULL);
        const float half = city.config().halfSize;
        const RoadNetwork& roads = city.roads();

        // Stations are welded the same way road nodes are: two lines whose platforms land within
        // an interchange radius of each other become one station serving both, which is what makes
        // a network out of a set of lines.
        auto stationAt = [&](Vec2 wanted, std::uint32_t line, std::uint32_t indexOnLine) {
            // Snap to a real junction so the entrance lands on a pavement rather than in the
            // middle of a block.
            const std::uint32_t node = roads.FindNearestNodeOfClass(wanted, RoadClass::Collector);
            const Vec2 position = node == 0xFFFFFFFFu ? wanted : roads.nodes()[node].position;

            for (std::uint32_t s = 0; s < stations_.size(); ++s)
                if (DistanceSq(stations_[s].position, position) < kInterchangeRadius * kInterchangeRadius)
                {
                    stations_[s].lines.emplace_back(line, indexOnLine);
                    return s;
                }

            MetroStation station;
            station.position = position;
            station.doorNode = node == 0xFFFFFFFFu ? roads.FindNearestNode(position) : node;
            station.entrance = roads.nodes()[station.doorNode].position;
            const std::uint16_t district = city.DistrictAt(position);
            station.name = city.districts()[district].name + " " +
                           kStationSuffix[rng.NextUInt(14)];
            station.lines.emplace_back(line, indexOnLine);
            stations_.push_back(std::move(station));
            return static_cast<std::uint32_t>(stations_.size() - 1);
        };

        for (int l = 0; l < lineCount; ++l)
        {
            MetroLine line;
            line.colorIndex = static_cast<std::uint8_t>(l);

            // The last line of a four-or-more network is a circle. Real networks grow that way --
            // radials first, then an orbital to stop every cross-town trip going through the
            // middle -- and it shows up in this simulation as a genuinely different load pattern.
            const bool ring = lineCount >= 4 && l == lineCount - 1;
            std::vector<Vec2> route;
            if (ring)
            {
                line.loop = true;
                const float radius = half * 0.44f;
                const int steps = std::max(8, static_cast<int>(2.0f * kPi * radius / kStationSpacing));
                for (int i = 0; i < steps; ++i)
                {
                    const float angle = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(steps);
                    route.push_back(FromHeading(angle) * (radius * rng.NextFloat(0.94f, 1.06f)));
                }
                route.push_back(route.front());
            }
            else
            {
                // A radial chord that misses the exact centre, so the lines interchange at
                // several different places instead of all crossing at one impossible station.
                const float angle = kPi * static_cast<float>(l) /
                                    std::max(1.0f, static_cast<float>(lineCount - (lineCount >= 4 ? 1 : 0))) +
                                    rng.NextFloat(-0.14f, 0.14f);
                const Vec2 dir = FromHeading(angle);
                const Vec2 offset = Perp(dir) * rng.NextFloat(-0.16f, 0.16f) * half;
                const float reach = half * 0.92f;
                const int steps = std::max(3, static_cast<int>(2.0f * reach / kStationSpacing));
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = -reach + 2.0f * reach * static_cast<float>(i) /
                                                 static_cast<float>(steps);
                    // The wobble is what stops a metro line looking like a ruler on the overlay.
                    const Vec2 wobble = Perp(dir) * (std::sin(t * 0.0016f + static_cast<float>(l)) * 95.0f);
                    route.push_back(offset + dir * t + wobble);
                }
            }

            for (std::uint32_t i = 0; i < route.size(); ++i)
            {
                const std::uint32_t station = stationAt(route[i], static_cast<std::uint32_t>(l), i);
                // A snapped platform can land on the one before it; a line that stops twice in the
                // same place would leave a train dwelling forever.
                if (!line.stations.empty() && line.stations.back() == station) continue;
                line.stations.push_back(station);
                line.points.push_back(stations_[station].position);
            }
            if (line.stations.size() < 2) continue;

            line.distance.assign(line.points.size(), 0.0f);
            for (std::size_t i = 1; i < line.points.size(); ++i)
                line.distance[i] = line.distance[i - 1] + Distance(line.points[i - 1], line.points[i]);
            line.length = line.distance.back();
            lines_.push_back(std::move(line));
        }

        // The line indices recorded on the stations were handed out before short lines were
        // dropped, so they are rebuilt from the surviving lines rather than patched.
        for (MetroStation& station : stations_) station.lines.clear();
        for (std::uint32_t l = 0; l < lines_.size(); ++l)
            for (std::uint32_t i = 0; i < lines_[l].stations.size(); ++i)
                stations_[lines_[l].stations[i]].lines.emplace_back(l, i);

        // Each platform runs along the line through it. An interchange takes the first line's
        // direction, which is the same simplification as giving it one platform: two lines meeting
        // at an angle would need two, and one is enough to stand on.
        for (MetroStation& station : stations_)
        {
            if (station.lines.empty()) continue;
            const auto [line, index] = station.lines.front();
            const MetroLine& l = lines_[line];
            const std::size_t next = index + 1 < l.points.size() ? index + 1 : index - 1;
            if (next < l.points.size())
            {
                const Vec2 delta = l.points[next] - l.points[index];
                if (LengthSq(delta) > 1e-4f) station.axis = Normalized(delta);
            }
        }

        // Trains, spaced by headway rather than by count, so a longer line gets more of them --
        // which is how a real timetable works and why the ring line is the busy one.
        for (std::uint32_t l = 0; l < lines_.size(); ++l)
        {
            const MetroLine& line = lines_[l];
            const float roundTrip = line.loop ? line.length : line.length * 2.0f;
            const float journeyTime = roundTrip / (kCruiseSpeed * 0.62f) +
                                      kDwellSeconds * static_cast<float>(line.stations.size()) *
                                      (line.loop ? 1.0f : 2.0f);
            const int count = std::max(2, static_cast<int>(journeyTime / kHeadwaySeconds));
            for (int t = 0; t < count; ++t)
            {
                MetroTrain train;
                train.line = l;
                const float fraction = static_cast<float>(t) / static_cast<float>(count);
                if (line.loop)
                {
                    train.position = fraction * line.length;
                    train.direction = 1;
                }
                else
                {
                    // Half the trains start on the return leg, so the service is bidirectional
                    // from the first tick rather than after one full traversal.
                    const float doubled = fraction * 2.0f;
                    train.direction = doubled < 1.0f ? 1 : -1;
                    train.position = doubled < 1.0f ? doubled * line.length
                                                    : (2.0f - doubled) * line.length;
                }
                train.speed = kCruiseSpeed * 0.5f;
                // The next stop is whichever station lies ahead in the direction of travel.
                train.nextStation = 0;
                for (std::uint32_t i = 0; i < line.stations.size(); ++i)
                {
                    const std::uint32_t index = train.direction > 0
                                                    ? i
                                                    : static_cast<std::uint32_t>(line.stations.size() - 1 - i);
                    const float ahead = (line.distance[index] - train.position) *
                                        static_cast<float>(train.direction);
                    if (ahead > 1.0f) { train.nextStation = static_cast<int>(index); break; }
                }
                trains_.push_back(train);
            }
        }

        BuildStationGraph();
    }

    void MetroNetwork::BuildStationGraph()
    {
        stationCount_ = stations_.size();
        if (stationCount_ == 0) return;
        const std::size_t n = stationCount_;
        constexpr float kInfinity = std::numeric_limits<float>::infinity();
        pairCost_.assign(n * n, kInfinity);
        pairVia_.assign(n * n, kNoStation);
        for (std::size_t i = 0; i < n; ++i) pairCost_[i * n + i] = 0.0f;

        // Adjacent stations on a line, at their track distance. Interchanges are already one
        // station, so there is no separate transfer edge to add.
        for (const MetroLine& line : lines_)
            for (std::size_t i = 1; i < line.stations.size(); ++i)
            {
                const std::uint32_t a = line.stations[i - 1];
                const std::uint32_t b = line.stations[i];
                const float cost = line.distance[i] - line.distance[i - 1];
                if (cost < pairCost_[a * n + b]) { pairCost_[a * n + b] = cost; pairCost_[b * n + a] = cost; }
            }

        // Floyd-Warshall. Sixty stations is 216 000 relaxations -- microseconds, once, and it
        // gives every agent's route planner an O(1) lookup for the rest of the run.
        for (std::size_t k = 0; k < n; ++k)
            for (std::size_t i = 0; i < n; ++i)
            {
                const float ik = pairCost_[i * n + k];
                if (ik == kInfinity) continue;
                for (std::size_t j = 0; j < n; ++j)
                {
                    const float through = ik + pairCost_[k * n + j];
                    if (through < pairCost_[i * n + j])
                    {
                        pairCost_[i * n + j] = through;
                        pairVia_[i * n + j] = static_cast<std::uint32_t>(k);
                    }
                }
            }
    }

    Vec2 MetroNetwork::PointOnLine(std::uint32_t line, float distance) const
    {
        const MetroLine& l = lines_[line];
        if (l.points.size() < 2) return l.points.empty() ? Vec2(0.0f, 0.0f) : l.points[0];
        const float clamped = Clamp(distance, 0.0f, l.length);
        const auto it = std::upper_bound(l.distance.begin(), l.distance.end(), clamped);
        const std::size_t index = static_cast<std::size_t>(
            Clamp(static_cast<int>(it - l.distance.begin()) - 1, 0,
                  static_cast<int>(l.points.size()) - 2));
        const float span = l.distance[index + 1] - l.distance[index];
        const float t = span > 1e-3f ? (clamped - l.distance[index]) / span : 0.0f;
        return Lerp(l.points[index], l.points[index + 1], t);
    }

    std::uint32_t MetroNetwork::NearestStation(Vec2 point) const
    {
        std::uint32_t best = kNoStation;
        float bestDist = std::numeric_limits<float>::max();
        for (std::uint32_t s = 0; s < stations_.size(); ++s)
        {
            const float d = DistanceSq(stations_[s].position, point);
            if (d < bestDist) { bestDist = d; best = s; }
        }
        return best;
    }

    bool MetroNetwork::PlanRoute(Vec2 from, Vec2 to, Route& route) const
    {
        if (stations_.size() < 2) return false;
        const std::uint32_t board = NearestStation(from);
        if (board == kNoStation) return false;

        // Only stations this one has a *direct* line to are considered. Allowing an interchange
        // here was the first version and it deadlocked the underground: an agent whose plan needed
        // two lines stood on a platform waiting for a train that, by construction, was never going
        // to serve its destination, and four hundred of them were still standing there at three in
        // the morning. A change of trains is a second trip, and this network is dense enough that
        // one line plus a walk gets almost everybody there.
        std::uint32_t alight = kNoStation;
        float bestWalk = std::numeric_limits<float>::max();
        for (const auto& [line, index] : stations_[board].lines)
        {
            const MetroLine& l = lines_[line];
            for (std::uint32_t other : l.stations)
            {
                if (other == board) continue;
                const float walk = Distance(stations_[other].entrance, to);
                if (walk < bestWalk) { bestWalk = walk; alight = other; }
            }
        }
        if (alight == kNoStation) return false;

        const float walkToBoard = Distance(from, stations_[board].entrance);
        const float direct = Distance(from, to);
        const float ride = pairCost_[board * stationCount_ + alight];
        if (!std::isfinite(ride)) return false;

        // The metro has to actually be worth it. Walking is ~1.35 m/s, a train averages ~12.5 m/s
        // door to door once dwells are counted, and the two walks at either end plus the wait for
        // a train are what a short trip cannot amortise. An agent that took the tube one stop
        // would look exactly as silly as a person doing it.
        const float walkTime = direct / 1.35f;
        const float metroTime = (walkToBoard + bestWalk) / 1.35f + ride / 12.5f +
                                kHeadwaySeconds * 0.5f + kDwellSeconds;
        if (metroTime >= walkTime) return false;

        route.boardStation = board;
        route.alightStation = alight;
        route.transferStation = kNoStation;
        route.rideDistance = ride;
        return true;
    }

    std::uint32_t MetroNetwork::LineBetween(std::uint32_t fromStation, std::uint32_t toStation,
                                            int* outDirection) const
    {
        for (const auto& [line, indexFrom] : stations_[fromStation].lines)
            for (const auto& [otherLine, indexTo] : stations_[toStation].lines)
                if (line == otherLine)
                {
                    if (outDirection != nullptr)
                        *outDirection = indexTo > indexFrom ? 1 : -1;
                    return line;
                }
        return 0xFFFFFFFFu;
    }

    void MetroNetwork::Step(float dt)
    {
        for (MetroTrain& train : trains_)
        {
            const MetroLine& line = lines_[train.line];
            if (line.stations.size() < 2) continue;

            if (train.dwellRemaining > 0.0f)
            {
                train.dwellRemaining -= dt;
                train.speed = 0.0f;
                continue;
            }

            const float target = line.distance[static_cast<std::size_t>(train.nextStation)];
            const float remaining = (target - train.position) * static_cast<float>(train.direction);

            // A trapezoidal speed profile: the braking distance for the current speed is what
            // decides whether the train is still accelerating, and it is what makes a train
            // arriving at a station look like a train rather than like a lift.
            const float brakingDistance = (train.speed * train.speed) / (2.0f * kAcceleration);
            if (remaining <= brakingDistance + 0.5f)
                train.speed = std::max(1.2f, train.speed - kAcceleration * dt);
            else
                train.speed = std::min(kCruiseSpeed, train.speed + kAcceleration * dt);

            train.position += train.speed * dt * static_cast<float>(train.direction);

            const float nowRemaining = (target - train.position) * static_cast<float>(train.direction);
            if (nowRemaining <= 1.0f)
            {
                train.position = target;
                train.speed = 0.0f;
                train.dwellRemaining = kDwellSeconds;

                const int last = static_cast<int>(line.stations.size()) - 1;
                int next = train.nextStation + train.direction;
                if (line.loop)
                {
                    // The loop's two ends are the same platform, so wrapping has to skip one of
                    // them or the train dwells twice in the same place.
                    if (next > last) { next = 1; train.position = 0.0f; }
                    else if (next < 0) { next = last - 1; train.position = line.length; }
                }
                else if (next > last || next < 0)
                {
                    train.direction = -train.direction;
                    next = train.nextStation + train.direction;
                    train.dwellRemaining = kDwellSeconds * 1.8f;   // turn-round at a terminus
                }
                train.nextStation = Clamp(next, 0, last);
            }
        }
    }
}
