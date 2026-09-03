// SPDX-License-Identifier: MIT
#include "Report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <thread>

namespace CnaCity
{
    namespace
    {
        bool WriteFile(const std::filesystem::path& path, const std::string& text,
                       std::string& error)
        {
            std::FILE* handle = std::fopen(path.string().c_str(), "wb");
            if (handle == nullptr)
            {
                error = "cannot write " + path.string();
                return false;
            }
            const std::size_t written = std::fwrite(text.data(), 1, text.size(), handle);
            std::fclose(handle);
            if (written != text.size())
            {
                error = "short write to " + path.string();
                return false;
            }
            return true;
        }

        /// Minimal JSON escaping. The only strings here are machine descriptions, but a GPU name
        /// with a backslash in it would otherwise produce a file nothing can parse.
        std::string JsonString(const std::string& value)
        {
            std::string out = "\"";
            for (const char c : value)
            {
                if (c == '"' || c == '\\') { out += '\\'; out += c; }
                else if (c == '\n') out += "\\n";
                else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
            }
            return out + "\"";
        }

        std::string Number(double value, int decimals = 3)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
            return buffer;
        }

        // --- The charts -----------------------------------------------------------------------
        //
        // Inline SVG, generated here. A report that pulls a charting library off a CDN stops
        // working the day it is opened without a network, which for an artefact whose whole
        // purpose is to be kept and compared later is most of the time.

        struct Series
        {
            std::string name;
            std::string colour;
            std::vector<double> values;
        };

        /**
         * @brief A grouped bar chart over @p labels.
         *
         * Bars rather than a line, because the x axis is a handful of populations rather than a
         * continuum, and a line between 10 000 and 100 000 agents invites reading a value at
         * 50 000 off the slope.
         */
        std::string BarChart(const std::string& title, const std::vector<std::string>& labels,
                             const std::vector<Series>& series, const std::string& unit)
        {
            constexpr double kWidth = 720.0;
            constexpr double kHeight = 260.0;
            constexpr double kLeft = 64.0;
            constexpr double kBottom = 34.0;
            constexpr double kTop = 16.0;

            double peak = 0.0;
            for (const Series& s : series)
                for (const double v : s.values) peak = std::max(peak, v);
            if (peak <= 0.0) peak = 1.0;
            // A round ceiling, so the gridline labels are numbers a person would choose.
            const double magnitude = std::pow(10.0, std::floor(std::log10(peak)));
            const double ceiling = std::ceil(peak / (magnitude / 2.0)) * (magnitude / 2.0);

            const double plotWidth = kWidth - kLeft - 12.0;
            const double plotHeight = kHeight - kBottom - kTop;
            const auto groups = static_cast<double>(std::max<std::size_t>(labels.size(), 1));
            const double groupWidth = plotWidth / groups;
            const double barWidth =
                groupWidth * 0.7 / static_cast<double>(std::max<std::size_t>(series.size(), 1));

            std::ostringstream svg;
            svg << "<svg viewBox=\"0 0 " << kWidth << " " << kHeight
                << "\" class=\"chart\" role=\"img\" aria-label=\"" << title << "\">";
            svg << "<title>" << title << "</title>";

            for (int line = 0; line <= 4; ++line)
            {
                const double value = ceiling * line / 4.0;
                const double y = kTop + plotHeight - plotHeight * line / 4.0;
                svg << "<line x1=\"" << kLeft << "\" y1=\"" << y << "\" x2=\"" << (kWidth - 12.0)
                    << "\" y2=\"" << y << "\" class=\"grid\"/>";
                svg << "<text x=\"" << (kLeft - 8.0) << "\" y=\"" << (y + 4.0)
                    << "\" class=\"tick\" text-anchor=\"end\">" << Number(value, value < 10 ? 1 : 0)
                    << "</text>";
            }

            for (std::size_t g = 0; g < labels.size(); ++g)
            {
                const double groupX = kLeft + groupWidth * static_cast<double>(g);
                for (std::size_t s = 0; s < series.size(); ++s)
                {
                    const double value = g < series[s].values.size() ? series[s].values[g] : 0.0;
                    const double height = plotHeight * value / ceiling;
                    const double x = groupX + groupWidth * 0.15 + barWidth * static_cast<double>(s);
                    svg << "<rect x=\"" << x << "\" y=\"" << (kTop + plotHeight - height)
                        << "\" width=\"" << (barWidth - 2.0) << "\" height=\"" << std::max(height, 0.5)
                        << "\" fill=\"" << series[s].colour << "\"><title>" << series[s].name << " @ "
                        << labels[g] << ": " << Number(value) << " " << unit << "</title></rect>";
                }
                svg << "<text x=\"" << (groupX + groupWidth * 0.5) << "\" y=\"" << (kHeight - 12.0)
                    << "\" class=\"tick\" text-anchor=\"middle\">" << labels[g] << "</text>";
            }

            svg << "<line x1=\"" << kLeft << "\" y1=\"" << (kTop + plotHeight) << "\" x2=\""
                << (kWidth - 12.0) << "\" y2=\"" << (kTop + plotHeight) << "\" class=\"axis\"/>";
            svg << "</svg>";
            return svg.str();
        }

        std::string Legend(const std::vector<Series>& series)
        {
            std::ostringstream out;
            out << "<p class=\"legend\">";
            for (const Series& s : series)
                out << "<span><i style=\"background:" << s.colour << "\"></i>" << s.name << "</span>";
            out << "</p>";
            return out.str();
        }

        std::string Escape(const std::string& value)
        {
            std::string out;
            for (const char c : value)
            {
                if (c == '<') out += "&lt;";
                else if (c == '>') out += "&gt;";
                else if (c == '&') out += "&amp;";
                else out += c;
            }
            return out;
        }
    }

    SystemInfo DescribeSystem()
    {
        SystemInfo info;
#ifdef CNA_CITY_RENDERER
        info.renderer = CNA_CITY_RENDERER;
#else
        info.renderer = "unknown";
#endif
#ifdef CNA_CITY_BUILD_TYPE
        info.buildType = CNA_CITY_BUILD_TYPE;
#endif
#ifdef CNA_CITY_COMPILER
        info.compiler = CNA_CITY_COMPILER;
#endif
#if defined(_WIN32)
        info.os = "Windows";
#elif defined(__APPLE__)
        info.os = "macOS";
#elif defined(__EMSCRIPTEN__)
        info.os = "Emscripten";
#elif defined(__linux__)
        info.os = "Linux";
#else
        info.os = "unknown";
#endif
        info.hardwareThreads = static_cast<int>(std::thread::hardware_concurrency());

        const std::time_t now = std::time(nullptr);
        char stamp[64] = {};
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        info.takenAt = stamp;

#if defined(__linux__)
        if (std::FILE* handle = std::fopen("/proc/loadavg", "r"))
        {
            double one = 0.0;
            if (std::fscanf(handle, "%lf", &one) == 1) info.loadAverage = one;
            std::fclose(handle);
        }
#endif
        return info;
    }

    bool WriteReport(const std::string& directory, const Report& report, std::string& error)
    {
        std::error_code code;
        std::filesystem::create_directories(directory, code);
        if (code)
        {
            error = "cannot create " + directory + ": " + code.message();
            return false;
        }
        const std::filesystem::path root(directory);

        // --- system.json ---------------------------------------------------------------------
        {
            std::ostringstream json;
            json << "{\n";
            json << "  \"renderer\": " << JsonString(report.system.renderer) << ",\n";
            json << "  \"graphicsCard\": " << JsonString(report.system.graphicsCard) << ",\n";
            json << "  \"buildType\": " << JsonString(report.system.buildType) << ",\n";
            json << "  \"compiler\": " << JsonString(report.system.compiler) << ",\n";
            json << "  \"os\": " << JsonString(report.system.os) << ",\n";
            json << "  \"hardwareThreads\": " << report.system.hardwareThreads << ",\n";
            json << "  \"workerThreads\": " << report.system.workerThreads << ",\n";
            json << "  \"seed\": " << report.system.seed << ",\n";
            json << "  \"cityDigest\": " << JsonString(report.system.cityDigest) << ",\n";
            json << "  \"takenAt\": " << JsonString(report.system.takenAt) << ",\n";
            json << "  \"loadAverage\": " << Number(report.system.loadAverage, 2) << "\n";
            json << "}\n";
            if (!WriteFile(root / "system.json", json.str(), error)) return false;
        }

        // --- simulation.csv ------------------------------------------------------------------
        {
            std::ostringstream csv;
            csv << "agents,setup_ms,mean_ms,p99_ms,worst_ms,decision_ms,walk_ms,crowd_ms,"
                   "traffic_ms,metro_ms,bus_ms,memory_mb,route_queries,cache_hit,peak_travelling,"
                   "gridlocked,runs,spread_ms\n";
            for (const SimulationRow& row : report.simulation)
                csv << row.agents << ',' << Number(row.setupMs) << ',' << Number(row.meanMs, 4)
                    << ',' << Number(row.p99Ms, 4) << ',' << Number(row.worstMs, 4) << ','
                    << Number(row.decisionMs, 4) << ',' << Number(row.walkMs, 4) << ','
                    << Number(row.crowdMs, 4) << ',' << Number(row.trafficMs, 4) << ','
                    << Number(row.metroMs, 4) << ',' << Number(row.busMs, 4) << ','
                    << Number(row.memoryMb, 2) << ',' << row.routeQueries << ','
                    << Number(row.cacheHitRate, 4) << ',' << row.peakTravelling << ','
                    << row.gridlocked << ',' << row.runs << ',' << Number(row.spreadMs, 4) << '\n';
            if (!WriteFile(root / "simulation.csv", csv.str(), error)) return false;
        }

        // --- memory.csv ----------------------------------------------------------------------
        {
            std::ostringstream csv;
            csv << "agents,memory_mb,bytes_per_agent\n";
            for (const SimulationRow& row : report.simulation)
            {
                const double bytes = row.agents > 0
                                         ? row.memoryMb * 1024.0 * 1024.0 / row.agents
                                         : 0.0;
                csv << row.agents << ',' << Number(row.memoryMb, 2) << ',' << Number(bytes, 1)
                    << '\n';
            }
            if (!WriteFile(root / "memory.csv", csv.str(), error)) return false;
        }

        // --- rendering.csv and passes.csv ----------------------------------------------------
        {
            std::ostringstream csv;
            csv << "view,width,height,quality,frame_ms,simulation_ms,draw_ms,shadow_ms,"
                   "prepass_ms,scene_ms,instance_ms,draw_calls,triangles\n";
            for (const RenderingRow& row : report.rendering)
                csv << row.view << ',' << row.width << ',' << row.height << ',' << row.quality
                    << ',' << Number(row.frameMs) << ',' << Number(row.simulationMs) << ','
                    << Number(row.drawMs) << ',' << Number(row.shadowMs) << ','
                    << Number(row.prepassMs) << ',' << Number(row.sceneMs) << ','
                    << Number(row.instanceMs) << ',' << row.drawCalls << ',' << row.triangles
                    << '\n';
            if (!WriteFile(root / "rendering.csv", csv.str(), error)) return false;

            std::ostringstream passes;
            passes << "view,pass,milliseconds\n";
            for (const PassRow& row : report.passes)
                passes << row.view << ',' << row.pass << ',' << Number(row.milliseconds, 4) << '\n';
            if (!WriteFile(root / "passes.csv", passes.str(), error)) return false;
        }

        // --- report.html ---------------------------------------------------------------------
        {
            std::vector<std::string> labels;
            Series mean{"mean tick", "#3b7dd8", {}};
            Series p99{"p99 tick", "#d8873b", {}};
            Series memory{"resident MB", "#57a773", {}};
            Series decide{"decide", "#3b7dd8", {}};
            Series walk{"walk", "#d8873b", {}};
            Series crowd{"crowd", "#57a773", {}};
            Series traffic{"traffic", "#b5539c", {}};
            Series transit{"metro + buses", "#c0392b", {}};
            for (const SimulationRow& row : report.simulation)
            {
                labels.push_back(std::to_string(row.agents));
                mean.values.push_back(row.meanMs);
                p99.values.push_back(row.p99Ms);
                memory.values.push_back(row.memoryMb);
                decide.values.push_back(row.decisionMs);
                walk.values.push_back(row.walkMs);
                crowd.values.push_back(row.crowdMs);
                traffic.values.push_back(row.trafficMs);
                transit.values.push_back(row.metroMs + row.busMs);
            }

            std::ostringstream html;
            html << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                    "<title>CNA City benchmark report</title>\n<style>\n"
                    "body{font:14px/1.5 system-ui,sans-serif;margin:0;padding:32px;"
                    "background:#f6f7f9;color:#1d2530}\n"
                    "main{max-width:820px;margin:0 auto}\n"
                    "h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:32px 0 8px}\n"
                    ".sub{color:#5a6675;margin:0 0 24px}\n"
                    "table{border-collapse:collapse;width:100%;background:#fff;font-variant-numeric:"
                    "tabular-nums}\n"
                    "th,td{padding:6px 10px;border-bottom:1px solid #e4e7ec;text-align:right}\n"
                    "th:first-child,td:first-child{text-align:left}\n"
                    "th{background:#eef1f5;font-weight:600}\n"
                    ".chart{width:100%;height:auto;background:#fff;border:1px solid #e4e7ec}\n"
                    ".grid{stroke:#e4e7ec;stroke-width:1}.axis{stroke:#98a2b3;stroke-width:1}\n"
                    ".tick{font:11px system-ui,sans-serif;fill:#5a6675}\n"
                    ".legend{color:#5a6675;margin:8px 0 0}\n"
                    ".legend span{margin-right:16px}\n"
                    ".legend i{display:inline-block;width:10px;height:10px;margin-right:6px;"
                    "border-radius:2px}\n"
                    "dl{display:grid;grid-template-columns:max-content 1fr;gap:2px 16px;margin:0;"
                    "background:#fff;padding:12px 16px;border:1px solid #e4e7ec}\n"
                    "dt{color:#5a6675}dd{margin:0}\n"
                    "footer{color:#5a6675;margin-top:40px;font-size:12px}\n"
                    ".warn{background:#fff4e5;border:1px solid #f0c37b;padding:8px 12px;margin:8px 0}\n"
                    "td.wide{color:#b1560f;font-weight:600}\n"
                    "</style>\n</head>\n<body>\n<main>\n";
            html << "<h1>CNA City benchmark report</h1>\n";
            html << "<p class=\"sub\">" << Escape(report.system.takenAt) << " &middot; "
                 << Escape(report.system.os) << " &middot; " << Escape(report.system.renderer)
                 << "</p>\n";

            html << "<h2>Machine</h2>\n<dl>";
            const auto row = [&html](const char* name, const std::string& value) {
                if (value.empty()) return;
                html << "<dt>" << name << "</dt><dd>" << Escape(value) << "</dd>";
            };
            row("Renderer", report.system.renderer);
            row("Graphics", report.system.graphicsCard);
            row("Operating system", report.system.os);
            row("Compiler", report.system.compiler);
            row("Build", report.system.buildType);
            row("Hardware threads", std::to_string(report.system.hardwareThreads));
            row("Worker threads", std::to_string(report.system.workerThreads));
            row("Seed", std::to_string(report.system.seed));
            row("City digest", report.system.cityDigest);
            if (report.system.loadAverage >= 0.0)
                row("Load average when taken", Number(report.system.loadAverage, 2));
            html << "</dl>\n";
            if (report.system.loadAverage > 1.5)
                html << "<p class=\"warn\">The machine was not idle when this was measured (load "
                     << Number(report.system.loadAverage, 2)
                     << "). Compare the spread column before trusting a difference.</p>\n";

            if (!report.simulation.empty())
            {
                html << "<h2>Simulation</h2>\n";
                const std::vector<Series> tickSeries{mean, p99};
                html << BarChart("Tick cost by population", labels, tickSeries, "ms");
                html << Legend(tickSeries);

                const std::vector<Series> splitSeries{decide, walk, crowd, traffic, transit};
                html << "<h2>Where the tick goes</h2>\n";
                html << BarChart("Tick split by population", labels, splitSeries, "ms");
                html << Legend(splitSeries);

                html << "<h2>Memory</h2>\n";
                const std::vector<Series> memorySeries{memory};
                html << BarChart("Resident memory by population", labels, memorySeries, "MB");
                html << Legend(memorySeries);

                html << "<h2>Numbers</h2>\n<table><thead><tr><th>agents</th><th>setup</th>"
                        "<th>mean</th><th>p99</th><th>worst</th><th>decide</th><th>walk</th>"
                        "<th>crowd</th><th>traffic</th><th>metro</th><th>bus</th><th>memory</th>"
                        "<th>cache</th><th>peak out</th><th>spread</th></tr></thead><tbody>";
                for (const SimulationRow& r : report.simulation)
                    html << "<tr><td>" << r.agents << "</td><td>" << Number(r.setupMs, 0)
                         << " ms</td><td>" << Number(r.meanMs, 2) << "</td><td>"
                         << Number(r.p99Ms, 2) << "</td><td>" << Number(r.worstMs, 2)
                         << "</td><td>" << Number(r.decisionMs, 2) << "</td><td>"
                         << Number(r.walkMs, 2) << "</td><td>" << Number(r.crowdMs, 2)
                         << "</td><td>" << Number(r.trafficMs, 2) << "</td><td>"
                         << Number(r.metroMs, 2) << "</td><td>" << Number(r.busMs, 2)
                         << "</td><td>" << Number(r.memoryMb, 1) << " MB</td><td>"
                         << Number(r.cacheHitRate * 100.0, 0) << "%</td><td>" << r.peakTravelling
                         << "</td><td" << (r.meanMs > 0.0 && r.spreadMs > r.meanMs * 0.15
                                               ? " class=\"wide\""
                                               : "")
                         << ">&plusmn;" << Number(r.spreadMs, 2) << " over " << r.runs
                         << "</td></tr>";
                html << "</tbody></table>\n";

                if (report.simulation.size() > 1)
                {
                    const SimulationRow& first = report.simulation.front();
                    const SimulationRow& last = report.simulation.back();
                    if (first.meanMs > 0.0 && first.agents > 0)
                    {
                        const double agentRatio =
                            static_cast<double>(last.agents) / static_cast<double>(first.agents);
                        const double costRatio = last.meanMs / first.meanMs;
                        html << "<p class=\"sub\"><strong>" << Number(agentRatio, 0)
                             << "&times; the agents costs " << Number(costRatio, 1)
                             << "&times; the tick.</strong> The route cache's hit rate rises with "
                                "population &mdash; "
                             << Number(first.cacheHitRate * 100.0, 0) << "% at " << first.agents
                             << " agents, " << Number(last.cacheHitRate * 100.0, 0) << "% at "
                             << last.agents
                             << " &mdash; because citizens do not have random destinations.</p>\n";
                    }
                }
            }

            if (!report.rendering.empty())
            {
                std::vector<std::string> views;
                Series frame{"frame", "#3b7dd8", {}};
                Series draw{"draw", "#d8873b", {}};
                Series shadow{"shadows", "#57a773", {}};
                for (const RenderingRow& r : report.rendering)
                {
                    views.push_back(r.view);
                    frame.values.push_back(r.frameMs);
                    draw.values.push_back(r.drawMs);
                    shadow.values.push_back(r.shadowMs);
                }
                html << "<h2>Rendering</h2>\n";
                const std::vector<Series> renderSeries{frame, draw, shadow};
                html << BarChart("Frame cost by viewpoint", views, renderSeries, "ms");
                html << Legend(renderSeries);

                html << "<table><thead><tr><th>view</th><th>size</th><th>quality</th>"
                        "<th>frame</th><th>fps</th><th>sim</th><th>draw</th><th>shadow</th>"
                        "<th>prepass</th><th>scene</th><th>draws</th><th>tris</th></tr></thead>"
                        "<tbody>";
                for (const RenderingRow& r : report.rendering)
                    html << "<tr><td>" << Escape(r.view) << "</td><td>" << r.width << "&times;"
                         << r.height << "</td><td>" << Escape(r.quality) << "</td><td>"
                         << Number(r.frameMs, 1) << " ms</td><td>"
                         << Number(r.frameMs > 0.0 ? 1000.0 / r.frameMs : 0.0, 0) << "</td><td>"
                         << Number(r.simulationMs, 1) << "</td><td>" << Number(r.drawMs, 1)
                         << "</td><td>" << Number(r.shadowMs, 1) << "</td><td>"
                         << Number(r.prepassMs, 1) << "</td><td>" << Number(r.sceneMs, 1)
                         << "</td><td>" << r.drawCalls << "</td><td>" << (r.triangles / 1000)
                         << "k</td></tr>";
                html << "</tbody></table>\n";
            }

            if (!report.passes.empty())
            {
                html << "<h2>GPU passes</h2>\n<table><thead><tr><th>view</th><th>pass</th>"
                        "<th>ms</th></tr></thead><tbody>";
                for (const PassRow& r : report.passes)
                    html << "<tr><td>" << Escape(r.view) << "</td><td>" << Escape(r.pass)
                         << "</td><td>" << Number(r.milliseconds, 3) << "</td></tr>";
                html << "</tbody></table>\n";
            }

            html << "<footer>Written by <code>cna-city --report</code>. The CSVs beside this page "
                    "hold the same numbers for a script to diff.</footer>\n";
            html << "</main>\n</body>\n</html>\n";
            if (!WriteFile(root / "report.html", html.str(), error)) return false;
        }
        return true;
    }
}
