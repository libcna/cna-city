// SPDX-License-Identifier: MIT
//
// The report writer. A number printed to a terminal is a number nobody has next week, so every
// benchmark run leaves a directory behind: the machine, the tables as CSV for a script, and one
// self-contained HTML page for a person.
//
// What is worth testing here is not the arithmetic -- there is none -- but the promises: that the
// files are parseable, that the page needs nothing from a network, and that the spread survives
// into the output, because the spread is what says whether the rest of the numbers mean anything.

#include <filesystem>
#include <fstream>
#include <sstream>

#include "Report.hpp"
#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        std::string ReportDir(const char* name)
        {
            const std::filesystem::path directory =
                std::filesystem::temp_directory_path() / "cna-city-tests" / name;
            std::filesystem::remove_all(directory);
            return directory.string();
        }

        std::string Slurp(const std::string& directory, const char* file)
        {
            std::ifstream in(std::filesystem::path(directory) / file, std::ios::binary);
            std::ostringstream out;
            out << in.rdbuf();
            return out.str();
        }

        Report SampleReport()
        {
            Report report;
            report.system = DescribeSystem();
            report.system.seed = 4242;
            report.system.cityDigest = "0123456789abcdef";
            report.system.workerThreads = 4;
            report.simulation.push_back(SimulationRow{1000, 20.0, 0.43, 0.70, 3.0, 0.01, 0.02, 0.11,
                                                      0.16, 0.0, 0.12, 7.7, 2328, 0.11, 100, 0, 3,
                                                      0.01});
            report.simulation.push_back(SimulationRow{100000, 30.0, 2.84, 4.84, 15.0, 0.44, 0.65,
                                                      0.43, 0.40, 0.04, 0.24, 25.8, 208643, 0.37,
                                                      8586, 922, 3, 0.51});
            report.rendering.push_back(RenderingRow{"street", 1600, 900, "high", 11.5, 6.1, 7.7,
                                                    1.6, 0.7, 1.5, 1.3, 210, 45000});
            report.passes.push_back(PassRow{"street", "bloom", 0.41});
            return report;
        }
    }

    TEST(ReportWriter, ItWritesEverySixFileAndTheyParse)
    {
        const std::string directory = ReportDir("full");
        std::string error;
        ASSERT_TRUE(WriteReport(directory, SampleReport(), error)) << error;

        for (const char* name : {"system.json", "simulation.csv", "memory.csv", "rendering.csv",
                                 "passes.csv", "report.html"})
            EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(directory) / name))
                << name << " was not written";

        // The CSVs must have a header and one line per row, with the same number of columns on
        // every line -- a table a script cannot parse is a table nobody will diff.
        const std::string csv = Slurp(directory, "simulation.csv");
        std::istringstream lines(csv);
        std::string line;
        int lineCount = 0;
        std::size_t columns = 0;
        while (std::getline(lines, line))
        {
            if (line.empty()) continue;
            const std::size_t here =
                static_cast<std::size_t>(std::count(line.begin(), line.end(), ',')) + 1;
            if (lineCount == 0) columns = here;
            else EXPECT_EQ(here, columns) << "ragged CSV at line " << lineCount;
            ++lineCount;
        }
        EXPECT_EQ(lineCount, 3) << "a header and two rows";
        EXPECT_NE(csv.find("spread_ms"), std::string::npos);

        const std::string json = Slurp(directory, "system.json");
        EXPECT_EQ(std::count(json.begin(), json.end(), '{'),
                  std::count(json.begin(), json.end(), '}'));
        EXPECT_NE(json.find("\"seed\": 4242"), std::string::npos);
        EXPECT_NE(json.find("\"cityDigest\": \"0123456789abcdef\""), std::string::npos);
    }

    TEST(ReportWriter, ThePageNeedsNothingFromANetwork)
    {
        // The whole point of the artefact is that it can be kept and opened later, quite possibly
        // on a machine with no network. A charting library from a CDN would defeat that silently:
        // the page would still open, and the graphs would be blank.
        const std::string directory = ReportDir("offline");
        std::string error;
        ASSERT_TRUE(WriteReport(directory, SampleReport(), error)) << error;

        const std::string html = Slurp(directory, "report.html");
        EXPECT_EQ(html.find("http://"), std::string::npos);
        EXPECT_EQ(html.find("https://"), std::string::npos);
        EXPECT_EQ(html.find("<script"), std::string::npos) << "the page should not need scripting";
        EXPECT_NE(html.find("<svg"), std::string::npos) << "the charts are missing";
        // Balanced enough to render: every element opened is closed.
        EXPECT_EQ(std::count(html.begin(), html.end(), '<'),
                  std::count(html.begin(), html.end(), '>'));
    }

    TEST(ReportWriter, TheSpreadReachesThePageAndIsFlaggedWhenItIsWide)
    {
        std::string error;
        {
            const std::string directory = ReportDir("tight");
            Report report = SampleReport();
            report.simulation[1].spreadMs = 0.05;   // 2% of the mean
            ASSERT_TRUE(WriteReport(directory, report, error)) << error;
            const std::string html = Slurp(directory, "report.html");
            EXPECT_NE(html.find("over 3"), std::string::npos) << "the run count is not shown";
            EXPECT_EQ(html.find("class=\"wide\""), std::string::npos)
                << "a tight spread should not be flagged";
        }
        {
            const std::string directory = ReportDir("wide");
            Report report = SampleReport();
            report.simulation[1].spreadMs = 3.7;    // most of the mean; the number is noise
            ASSERT_TRUE(WriteReport(directory, report, error)) << error;
            const std::string html = Slurp(directory, "report.html");
            EXPECT_NE(html.find("class=\"wide\""), std::string::npos)
                << "a spread most of the size of the measurement was not flagged";
        }
    }

    TEST(ReportWriter, AnEmptyRunStillProducesAReadablePage)
    {
        // A report with nothing in it is what a failed run leaves behind, and it should say so
        // rather than crash or produce half a file.
        const std::string directory = ReportDir("empty");
        Report report;
        report.system = DescribeSystem();
        std::string error;
        ASSERT_TRUE(WriteReport(directory, report, error)) << error;
        const std::string html = Slurp(directory, "report.html");
        EXPECT_NE(html.find("</html>"), std::string::npos);
        EXPECT_EQ(html.find("<svg"), std::string::npos) << "no data, so no charts";
    }

    TEST(ReportWriter, ADirectoryThatCannotBeWrittenIsReported)
    {
        std::string error;
        Report report;
        report.system = DescribeSystem();
        // A path under a regular file cannot be a directory on any platform this runs on.
        const std::filesystem::path file =
            std::filesystem::temp_directory_path() / "cna-city-tests" / "not-a-directory";
        std::filesystem::create_directories(file.parent_path());
        { std::ofstream(file) << "x"; }
        EXPECT_FALSE(WriteReport((file / "inside").string(), report, error));
        EXPECT_FALSE(error.empty());
        std::filesystem::remove(file);
    }

    TEST(ReportWriter, TheSystemDescriptionNamesTheBuild)
    {
        // A benchmark that does not say what it measured cannot be compared with anybody else's.
        const SystemInfo info = DescribeSystem();
        EXPECT_FALSE(info.renderer.empty());
        EXPECT_NE(info.renderer, "unknown") << "the renderer is not reaching the binary";
        EXPECT_FALSE(info.os.empty());
        EXPECT_FALSE(info.compiler.empty());
        EXPECT_FALSE(info.buildType.empty());
        EXPECT_GT(info.hardwareThreads, 0);
        EXPECT_EQ(info.takenAt.size(), 19u) << "YYYY-MM-DD HH:MM:SS";
    }
}
