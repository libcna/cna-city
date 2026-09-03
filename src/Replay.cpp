// SPDX-License-Identifier: MIT
#include "Replay.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace CnaCity
{
    namespace
    {
        constexpr const char* kMagic = "cna-city-replay";
        constexpr int kVersion = 1;

        /// Text rather than binary, deliberately. A replay is a bug report: somebody has to be
        /// able to open it, see that it is a 100 000-agent city from seed 42 that ran for a
        /// simulated day, and diff two of them. It is a few hundred bytes either way.
        void WriteConfig(std::ostringstream& out, const SimConfig& config)
        {
            out << "seed " << config.city.seed << "\n";
            out << "halfsize " << config.city.halfSize << "\n";
            out << "arterialspacing " << config.city.arterialSpacing << "\n";
            out << "diagonals " << config.city.diagonalAvenues << "\n";
            out << "parkfraction " << config.city.parkFraction << "\n";
            out << "kerboccupancy " << config.city.kerbOccupancy << "\n";
            out << "agents " << config.agentCount << "\n";
            out << "starthour " << config.startHour << "\n";
            out << "timescale " << config.timeScale << "\n";
            out << "weather " << static_cast<int>(config.weather) << "\n";
            out << "randomweather " << (config.randomWeather ? 1 : 0) << "\n";
            out << "carownership " << config.carOwnership << "\n";
            out << "metrolines " << config.metroLines << "\n";
            out << "busroutes " << config.busRoutes << "\n";
            out << "threads " << config.threads << "\n";
        }

        bool ReadConfigLine(const std::string& key, const std::string& value, SimConfig& config)
        {
            if (key == "seed") config.city.seed = std::strtoull(value.c_str(), nullptr, 10);
            else if (key == "halfsize") config.city.halfSize = std::strtof(value.c_str(), nullptr);
            else if (key == "arterialspacing")
                config.city.arterialSpacing = std::strtof(value.c_str(), nullptr);
            else if (key == "diagonals") config.city.diagonalAvenues = std::atoi(value.c_str());
            else if (key == "parkfraction")
                config.city.parkFraction = std::strtof(value.c_str(), nullptr);
            else if (key == "kerboccupancy")
                config.city.kerbOccupancy = std::strtof(value.c_str(), nullptr);
            else if (key == "agents")
                config.agentCount = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            else if (key == "starthour") config.startHour = std::strtof(value.c_str(), nullptr);
            else if (key == "timescale") config.timeScale = std::strtof(value.c_str(), nullptr);
            else if (key == "weather")
                config.weather = static_cast<WeatherKind>(std::atoi(value.c_str()));
            else if (key == "randomweather") config.randomWeather = std::atoi(value.c_str()) != 0;
            else if (key == "carownership")
                config.carOwnership = std::strtof(value.c_str(), nullptr);
            else if (key == "metrolines") config.metroLines = std::atoi(value.c_str());
            else if (key == "busroutes") config.busRoutes = std::atoi(value.c_str());
            else if (key == "threads") config.threads = std::atoi(value.c_str());
            else return false;
            return true;
        }
    }

    ReplayRecorder::~ReplayRecorder() = default;

    bool ReplayRecorder::Open(const std::string& path, const SimConfig& config)
    {
        // Opened for writing straight away rather than only at Close, so a run that cannot write
        // its replay says so at the start instead of after a simulated day.
        std::FILE* probe = std::fopen(path.c_str(), "wb");
        if (probe == nullptr)
        {
            error_ = "cannot write " + path;
            return false;
        }
        std::fclose(probe);

        path_ = path;
        file_ = ReplayFile{};
        file_.config = config;
        open_ = true;
        closed_ = false;
        return true;
    }

    void ReplayRecorder::RecordWeather(std::uint64_t tick, WeatherKind kind)
    {
        if (!open_) return;
        file_.events.push_back(
            ReplayEvent{tick, ReplayEvent::Kind::Weather, static_cast<float>(kind)});
    }

    void ReplayRecorder::RecordHour(std::uint64_t tick, float hour)
    {
        if (!open_) return;
        file_.events.push_back(ReplayEvent{tick, ReplayEvent::Kind::Hour, hour});
    }

    void ReplayRecorder::MaybeCheckpoint(const Simulation& sim, std::uint64_t interval)
    {
        if (!open_ || interval == 0) return;
        const std::uint64_t tick = sim.tick();
        if (!file_.checkpoints.empty() && tick < file_.checkpoints.back().tick + interval) return;
        if (!file_.checkpoints.empty() && tick == file_.checkpoints.back().tick) return;
        file_.checkpoints.push_back(ReplayCheckpoint{tick, ComputeChecksum(sim)});
    }

    void ReplayRecorder::Close(const Simulation& sim)
    {
        if (!open_ || closed_) return;
        closed_ = true;
        file_.ticks = sim.tick();
        // The last checkpoint is always the final state, so a replay always checks the end even if
        // the run was shorter than one checkpoint interval.
        if (file_.checkpoints.empty() || file_.checkpoints.back().tick != file_.ticks)
            file_.checkpoints.push_back(ReplayCheckpoint{file_.ticks, ComputeChecksum(sim)});

        std::string error;
        if (!SaveReplay(path_, file_, error)) error_ = error;
    }

    bool SaveReplay(const std::string& path, const ReplayFile& file, std::string& error)
    {
        std::ostringstream out;
        out << kMagic << " " << kVersion << "\n";
        WriteConfig(out, file.config);
        out << "ticks " << file.ticks << "\n";
        for (const ReplayEvent& event : file.events)
            out << "event " << event.tick << " "
                << (event.kind == ReplayEvent::Kind::Weather ? "weather" : "hour") << " "
                << event.value << "\n";
        for (const ReplayCheckpoint& point : file.checkpoints)
            out << "check " << point.tick << " " << ToHex(point.checksum.city) << " "
                << ToHex(point.checksum.agents) << " " << ToHex(point.checksum.traffic) << " "
                << ToHex(point.checksum.transit) << " " << ToHex(point.checksum.world) << " "
                << ToHex(point.checksum.total) << "\n";

        const std::string text = out.str();
        std::FILE* handle = std::fopen(path.c_str(), "wb");
        if (handle == nullptr)
        {
            error = "cannot write " + path;
            return false;
        }
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), handle);
        std::fclose(handle);
        if (written != text.size())
        {
            error = "short write to " + path;
            return false;
        }
        return true;
    }

    bool LoadReplay(const std::string& path, ReplayFile& out, std::string& error)
    {
        std::FILE* handle = std::fopen(path.c_str(), "rb");
        if (handle == nullptr)
        {
            error = "cannot read " + path;
            return false;
        }
        std::string text;
        char buffer[4096];
        std::size_t got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) text.append(buffer, got);
        std::fclose(handle);

        std::istringstream in(text);
        std::string magic;
        int version = 0;
        if (!(in >> magic >> version) || magic != kMagic)
        {
            error = path + " is not a cna-city replay";
            return false;
        }
        if (version != kVersion)
        {
            error = path + " is version " + std::to_string(version) + "; this build writes " +
                    std::to_string(kVersion);
            return false;
        }

        out = ReplayFile{};
        std::string key;
        while (in >> key)
        {
            if (key == "ticks")
            {
                if (!(in >> out.ticks)) { error = "malformed ticks"; return false; }
            }
            else if (key == "event")
            {
                ReplayEvent event;
                std::string kind;
                if (!(in >> event.tick >> kind >> event.value))
                {
                    error = "malformed event";
                    return false;
                }
                if (kind == "weather") event.kind = ReplayEvent::Kind::Weather;
                else if (kind == "hour") event.kind = ReplayEvent::Kind::Hour;
                else { error = "unknown event kind '" + kind + "'"; return false; }
                out.events.push_back(event);
            }
            else if (key == "check")
            {
                ReplayCheckpoint point;
                std::string city, agents, traffic, transit, world, total;
                if (!(in >> point.tick >> city >> agents >> traffic >> transit >> world >> total))
                {
                    error = "malformed checkpoint";
                    return false;
                }
                if (!FromHex(city, point.checksum.city) || !FromHex(agents, point.checksum.agents) ||
                    !FromHex(traffic, point.checksum.traffic) ||
                    !FromHex(transit, point.checksum.transit) ||
                    !FromHex(world, point.checksum.world) || !FromHex(total, point.checksum.total))
                {
                    error = "malformed checkpoint digest";
                    return false;
                }
                out.checkpoints.push_back(point);
            }
            else
            {
                std::string value;
                if (!(in >> value)) { error = "malformed '" + key + "'"; return false; }
                if (!ReadConfigLine(key, value, out.config))
                {
                    error = "unknown key '" + key + "'";
                    return false;
                }
            }
        }
        return true;
    }

    ReplayResult RunReplay(const ReplayFile& file)
    {
        ReplayResult result;
        Simulation sim;
        sim.Initialize(file.config);

        std::size_t nextEvent = 0;
        std::size_t nextCheck = 0;

        // The city is checked before a single tick runs, because a generator that has moved makes
        // every later mismatch meaningless -- and it is the one component whose divergence points
        // at a change in the *build* rather than in the simulation.
        if (!file.checkpoints.empty())
        {
            const std::uint64_t city = ComputeCityChecksum(sim);
            if (city != file.checkpoints.front().checksum.city)
            {
                result.divergedAtTick = 0;
                result.divergedIn = "city";
                result.expected = file.checkpoints.front().checksum;
                result.actual = ComputeChecksum(sim);
                return result;
            }
        }

        // Compare, then step -- rather than step, then compare. A recording taken from the game
        // loop can hold a checkpoint at tick 0, because the first frame's elapsed time can be too
        // small to make a whole tick, and a player that only ever looked after stepping could
        // never match it and reported the whole file as diverged.
        for (std::uint64_t tick = 0;; ++tick)
        {
            while (nextEvent < file.events.size() && file.events[nextEvent].tick == tick)
            {
                const ReplayEvent& event = file.events[nextEvent];
                if (event.kind == ReplayEvent::Kind::Weather)
                    sim.mutableWeather().Force(static_cast<WeatherKind>(static_cast<int>(event.value)));
                else
                    sim.mutableClock().setHour(event.value);
                ++nextEvent;
            }

            while (nextCheck < file.checkpoints.size() &&
                   file.checkpoints[nextCheck].tick == sim.tick())
            {
                const WorldChecksum actual = ComputeChecksum(sim);
                const WorldChecksum& expected = file.checkpoints[nextCheck].checksum;
                ++result.checkpointsChecked;
                if (actual != expected)
                {
                    result.divergedAtTick = sim.tick();
                    result.expected = expected;
                    result.actual = actual;
                    result.divergedIn = actual.city != expected.city         ? "city"
                                        : actual.agents != expected.agents   ? "agents"
                                        : actual.traffic != expected.traffic ? "traffic"
                                        : actual.transit != expected.transit ? "transit"
                                                                             : "world";
                    return result;
                }
                ++nextCheck;
            }

            if (tick >= file.ticks) break;

            // One tick per Step. The fixed-timestep loop makes that exactly the tick the original
            // run took, whatever step sizes it was actually driven with -- which is why a replay
            // does not have to record them.
            sim.Step(Simulation::kMovementStep);
        }

        result.reproduced = nextCheck == file.checkpoints.size();
        if (!result.reproduced)
        {
            result.divergedAtTick = file.checkpoints[nextCheck].tick;
            result.divergedIn = "tick count";
        }
        return result;
    }
}
