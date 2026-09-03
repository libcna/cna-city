// SPDX-License-Identifier: MIT
#include "CliOptions.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace CnaCity
{
    namespace
    {
        bool ParseFloat(const char* text, float& out)
        {
            char* end = nullptr;
            const double value = std::strtod(text, &end);
            if (end == text || *end != '\0') return false;
            out = static_cast<float>(value);
            return true;
        }

        bool ParseUInt(const char* text, std::uint32_t& out)
        {
            char* end = nullptr;
            const long long value = std::strtoll(text, &end, 10);
            if (end == text || *end != '\0' || value < 0) return false;
            out = static_cast<std::uint32_t>(value);
            return true;
        }

        bool ParseWeather(const std::string& name, WeatherKind& out)
        {
            for (int i = 0; i < kWeatherKindCount; ++i)
            {
                const auto kind = static_cast<WeatherKind>(i);
                std::string candidate = WeatherName(kind);
                for (char& c : candidate) c = static_cast<char>(std::tolower(c));
                std::string wanted = name;
                for (char& c : wanted) c = static_cast<char>(std::tolower(c));
                // "partly" is enough for "partly cloudy": the full names have spaces in them and
                // a shell argument with a space is a nuisance nobody should have to think about.
                if (candidate == wanted || candidate.rfind(wanted, 0) == 0)
                {
                    out = kind;
                    return true;
                }
            }
            return false;
        }
    }

    const char* QualityName(Quality quality)
    {
        switch (quality)
        {
            case Quality::Low:    return "low";
            case Quality::Medium: return "medium";
            case Quality::High:   return "high";
            case Quality::Ultra:  return "ultra";
        }
        return "?";
    }

    void PrintUsage()
    {
        std::printf(
            "cna-city -- a procedural city with a hundred thousand simulated inhabitants\n"
            "\n"
            "Usage: cna-city [options]\n"
            "\n"
            "Population and world\n"
            "  --agents N            Citizens to simulate (default 100000)\n"
            "  --seed N              World seed; the same seed is always the same city\n"
            "  --size M              Half the city's side, in metres (default 1650)\n"
            "  --metro N             Metro lines (default 5; the last one is a circle line)\n"
            "  --cars F              Share of adults with a car, 0..1 (default 0.62)\n"
            "  --time H              Start hour, 0..24, fractional allowed (default 6.5)\n"
            "  --timescale F         Simulated seconds per real second (default 60: a day in 24 min)\n"
            "  --weather NAME        clear, partly, overcast, rain, storm, fog, snow; also pins it\n"
            "  --fixed-weather       Pin whatever weather the seed starts with\n"
            "  --threads N           Worker threads (default: as many as the machine has)\n"
            "\n"
            "Presentation\n"
            "  --quality Q           low, medium, high (default), ultra\n"
            "  --width N --height N  Back buffer size (default 1600x900)\n"
            "  --fullscreen          Full screen\n"
            "  --no-vsync            Do not wait for the vertical retrace\n"
            "  --camera M            free, orbit (default), follow, street, cinematic\n"
            "  --follow              Shorthand for --camera follow\n"
            "  --follow-metro        Follow somebody using the underground, not somebody on foot\n"
            "  --follow-bus          Follow somebody using the buses\n"
            "  --no-post             Start with the post chain bypassed (what F2 toggles)\n"
            "  --overlay M           none, stats (default), roads, routes\n"
            "\n"
            "Measurement\n"
            "  --bench               Run the scale sweep and exit\n"
            "  --scales A,B,C        Agent counts for --bench (default 1000,10000,100000)\n"
            "  --csv FILE            Write the benchmark table to FILE\n"
            "  --report DIR          Run the sweep and write CSVs, system.json and report.html\n"
            "  --repeat N            Measure each scale N times for --report (default 3)\n"
            "  --compare A B [...]   Compare report directories and write comparison.html\n"
            "  --comparison-out F    Where --compare writes its page\n"
            "  --headless            Simulate with no graphics device at all\n"
            "  --checksum            Simulate, print a digest of the world, and exit\n"
            "  --simulate D          How long --checksum and --record run: 24h, 90m, 600s\n"
            "  --record FILE         Write a replay of this run\n"
            "  --replay FILE         Re-run a replay and report whether it still reproduces\n"
            "  --checkpoint-every N  Ticks between replay checkpoints (default 1200)\n"
            "  --save FILE           Write a snapshot of the world when the run ends\n"
            "  --load FILE           Start from a snapshot instead of from the start hour\n"
            "  --note TEXT           Description stored in the snapshot header\n"
            "  --frames N            Stop after N frames\n"
            "  --screenshot FILE     Write one PNG and exit\n"
            "  --help                This text\n"
            "\n"
            "Keys while running\n"
            "  1-5     camera mode: free, orbit, follow, street, cinematic\n"
            "  WASD    move (free camera), mouse drag to look, wheel to change speed\n"
            "  F       cycle the weather        T / G  advance / rewind the clock\n"
            "  N       pick another citizen to follow\n"
            "  Tab     cycle the overlay: off, statistics, road network, routes\n"
            "  P       pause      [ / ]  slow down / speed up time\n"
            "  F1      toggle the HUD          F2  toggle the post-processing chain\n");
    }

    bool ParseCommandLine(int argc, char** argv, CliOptions& options)
    {
        options.benchScales.clear();
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const auto next = [&](const char*& out) {
                if (i + 1 >= argc) return false;
                out = argv[++i];
                return true;
            };
            const char* value = nullptr;

            if (arg == "--help" || arg == "-h") { options.showHelp = true; return true; }
            else if (arg == "--agents") { if (!next(value) || !ParseUInt(value, options.sim.agentCount)) { options.error = "--agents needs a count"; return false; } }
            else if (arg == "--seed") { std::uint32_t seed = 0; if (!next(value) || !ParseUInt(value, seed)) { options.error = "--seed needs a number"; return false; } options.sim.city.seed = seed; }
            else if (arg == "--size") { if (!next(value) || !ParseFloat(value, options.sim.city.halfSize)) { options.error = "--size needs metres"; return false; } }
            else if (arg == "--metro") { std::uint32_t n = 0; if (!next(value) || !ParseUInt(value, n)) { options.error = "--metro needs a count"; return false; } options.sim.metroLines = static_cast<int>(n); }
            else if (arg == "--cars") { if (!next(value) || !ParseFloat(value, options.sim.carOwnership)) { options.error = "--cars needs a fraction"; return false; } }
            else if (arg == "--time") { if (!next(value) || !ParseFloat(value, options.sim.startHour)) { options.error = "--time needs an hour"; return false; } }
            else if (arg == "--timescale") { if (!next(value) || !ParseFloat(value, options.sim.timeScale)) { options.error = "--timescale needs a factor"; return false; } }
            else if (arg == "--weather")
            {
                if (!next(value) || !ParseWeather(value, options.sim.weather))
                {
                    options.error = "--weather: unknown weather (try clear, rain, fog, snow)";
                    return false;
                }
                // Naming a weather also pins it. Somebody who asks for rain wants to look at
                // rain, not at whatever the forecast wanders into ninety seconds later -- which is
                // exactly what made a set of screenshots taken in one batch disagree about what
                // the weather was.
                options.sim.randomWeather = false;
            }
            else if (arg == "--fixed-weather") options.sim.randomWeather = false;
            else if (arg == "--threads") { std::uint32_t n = 0; if (!next(value) || !ParseUInt(value, n)) { options.error = "--threads needs a count"; return false; } options.sim.threads = static_cast<int>(n); options.threadsGiven = true; }
            else if (arg == "--quality")
            {
                if (!next(value)) { options.error = "--quality needs a level"; return false; }
                const std::string level = value;
                if (level == "low") options.quality = Quality::Low;
                else if (level == "medium") options.quality = Quality::Medium;
                else if (level == "high") options.quality = Quality::High;
                else if (level == "ultra") options.quality = Quality::Ultra;
                else { options.error = "--quality: expected low, medium, high or ultra"; return false; }
            }
            else if (arg == "--width") { std::uint32_t n = 0; if (!next(value) || !ParseUInt(value, n)) { options.error = "--width needs pixels"; return false; } options.windowWidth = static_cast<int>(n); }
            else if (arg == "--height") { std::uint32_t n = 0; if (!next(value) || !ParseUInt(value, n)) { options.error = "--height needs pixels"; return false; } options.windowHeight = static_cast<int>(n); }
            else if (arg == "--fullscreen") options.fullScreen = true;
            else if (arg == "--no-vsync") options.vsync = false;
            else if (arg == "--follow") options.camera = CameraMode::Follow;
            else if (arg == "--follow-metro") { options.camera = CameraMode::Follow; options.followMetro = true; }
            else if (arg == "--no-post") options.noPost = true;
            else if (arg == "--checksum") options.mode = RunMode::Checksum;
            else if (arg == "--repeat")
            {
                std::uint32_t n = 0;
                if (!next(value) || !ParseUInt(value, n) || n == 0)
                {
                    options.error = "--repeat needs a positive count";
                    return false;
                }
                options.reportRepeats = static_cast<int>(n);
            }
            else if (arg == "--compare")
            {
                // Everything up to the next option is a report directory, so the usual shape --
                // `--compare before after` -- works without repeating the flag.
                while (i + 1 < argc && argv[i + 1][0] != '-')
                    options.comparePaths.emplace_back(argv[++i]);
                if (options.comparePaths.size() < 2)
                {
                    options.error = "--compare needs at least two report directories";
                    return false;
                }
                options.mode = RunMode::Compare;
            }
            else if (arg == "--comparison-out")
            {
                if (!next(value)) { options.error = "--comparison-out needs a path"; return false; }
                options.comparisonPath = value;
            }
            else if (arg == "--report")
            {
                if (!next(value)) { options.error = "--report needs a directory"; return false; }
                options.reportPath = value;
                options.mode = RunMode::Report;
                // The rendering half needs a device, so it is on unless --headless says otherwise.
                // Ordering matters: --headless --report and --report --headless must agree, which
                // is why the decision is deferred to the end of parsing.
                options.renderReport = true;
            }
            else if (arg == "--save")
            {
                if (!next(value)) { options.error = "--save needs a path"; return false; }
                options.savePath = value;
            }
            else if (arg == "--load")
            {
                if (!next(value)) { options.error = "--load needs a path"; return false; }
                options.loadPath = value;
            }
            else if (arg == "--note")
            {
                if (!next(value)) { options.error = "--note needs some text"; return false; }
                options.snapshotNote = value;
            }
            else if (arg == "--simulate")
            {
                // "24h", "90m", "600s", or a bare number of hours. The unit matters more than the
                // brevity: "--simulate 24" meaning seconds would be a very confusing way to ask
                // for an empty city.
                if (!next(value)) { options.error = "--simulate needs a duration"; return false; }
                const std::string text = value;
                const char unit = text.empty() ? 'h' : text.back();
                const double amount = std::strtod(text.c_str(), nullptr);
                const double scale = unit == 's' ? 1.0 : unit == 'm' ? 60.0 : 3600.0;
                if (!(amount > 0.0))
                {
                    options.error = "--simulate wants a positive duration";
                    return false;
                }
                options.simulateSeconds = static_cast<float>(amount * scale);
            }
            else if (arg == "--record")
            {
                if (!next(value)) { options.error = "--record needs a path"; return false; }
                options.recordPath = value;
            }
            else if (arg == "--replay")
            {
                if (!next(value)) { options.error = "--replay needs a path"; return false; }
                options.replayPath = value;
                options.mode = RunMode::Replay;
            }
            else if (arg == "--checkpoint-every")
            {
                if (!next(value)) { options.error = "--checkpoint-every needs a tick count"; return false; }
                options.checkpointInterval = std::strtoull(value, nullptr, 10);
            }
            else if (arg == "--follow-bus") { options.camera = CameraMode::Follow; options.followBus = true; }
            else if (arg == "--camera")
            {
                if (!next(value)) { options.error = "--camera needs a mode"; return false; }
                const std::string mode = value;
                if (mode == "free") options.camera = CameraMode::Free;
                else if (mode == "orbit") options.camera = CameraMode::Orbit;
                else if (mode == "follow") options.camera = CameraMode::Follow;
                else if (mode == "street") options.camera = CameraMode::Street;
                else if (mode == "cinematic") options.camera = CameraMode::Cinematic;
                else { options.error = "--camera: expected free, orbit, follow, street or cinematic"; return false; }
            }
            else if (arg == "--overlay")
            {
                if (!next(value)) { options.error = "--overlay needs a name"; return false; }
                const std::string name = value;
                if (name == "none") options.overlay = 0;
                else if (name == "stats") options.overlay = 1;
                else if (name == "roads") options.overlay = 2;
                else if (name == "routes") options.overlay = 3;
                else { options.error = "--overlay: expected none, stats, roads or routes"; return false; }
            }
            else if (arg == "--bench") options.mode = RunMode::Benchmark;
            else if (arg == "--headless") { options.mode = RunMode::Headless; options.headlessRequested = true; }
            else if (arg == "--csv") { if (!next(value)) { options.error = "--csv needs a path"; return false; } options.csvPath = value; }
            else if (arg == "--screenshot") { if (!next(value)) { options.error = "--screenshot needs a path"; return false; } options.screenshotPath = value; }
            else if (arg == "--frames") { std::uint32_t n = 0; if (!next(value) || !ParseUInt(value, n)) { options.error = "--frames needs a count"; return false; } options.frameLimit = static_cast<int>(n); }
            else if (arg == "--scales")
            {
                if (!next(value)) { options.error = "--scales needs a list"; return false; }
                std::string list = value;
                std::size_t start = 0;
                while (start <= list.size())
                {
                    const std::size_t comma = list.find(',', start);
                    const std::string piece = list.substr(start, comma == std::string::npos
                                                                     ? std::string::npos
                                                                     : comma - start);
                    std::uint32_t scale = 0;
                    if (!piece.empty() && ParseUInt(piece.c_str(), scale)) options.benchScales.push_back(scale);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
                if (options.benchScales.empty()) { options.error = "--scales: no usable counts"; return false; }
            }
            else
            {
                options.error = "unknown option '" + arg + "' (try --help)";
                return false;
            }
        }

        // --headless and --report can arrive in either order, so the decision is made once
        // parsing is done rather than by whichever of them was last on the line.
        if (!options.reportPath.empty())
        {
            options.mode = options.mode == RunMode::Headless ? RunMode::Report : RunMode::Report;
            options.renderReport = options.renderReport && !options.headlessRequested;
        }
        if (options.mode == RunMode::Benchmark && options.benchScales.empty())
            options.benchScales = {1000, 10000, 100000};
        // A start hour outside the day is a typo, not a request for yesterday.
        while (options.sim.startHour < 0.0f) options.sim.startHour += 24.0f;
        while (options.sim.startHour >= 24.0f) options.sim.startHour -= 24.0f;
        return true;
    }
}
