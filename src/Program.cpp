// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Benchmark.hpp"
#include "CityGame.hpp"
#include "CliOptions.hpp"

int main(int argc, char** argv)
{
    CnaCity::CliOptions options;
    if (!CnaCity::ParseCommandLine(argc, argv, options))
    {
        std::fprintf(stderr, "cna-city: %s\n", options.error.c_str());
        return 2;
    }
    if (options.showHelp)
    {
        CnaCity::PrintUsage();
        return 0;
    }

    switch (options.mode)
    {
        case CnaCity::RunMode::Benchmark:
            return CnaCity::RunBenchmark(options);
        case CnaCity::RunMode::Headless:
            return CnaCity::RunHeadless(options);
        case CnaCity::RunMode::Interactive:
            break;
    }

    CnaCity::CityGame game(options);
    game.Run();
    return 0;
}
