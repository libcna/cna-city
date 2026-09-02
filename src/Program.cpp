// SPDX-License-Identifier: MIT
#include <cstdio>

#include "City.hpp"
#include "System/Diagnostics/Stopwatch.hpp"

int main()
{
    CnaCity::City city;
    CnaCity::CityConfig config;
    System::Diagnostics::Stopwatch watch;
    watch.Start();
    city.Generate(config);
    watch.Stop();
    std::printf("generated in %.1f ms\n",
                static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0);
    std::printf("districts %zu  nodes %zu  segments %zu  blocks %zu\n",
                city.districts().size(), city.roads().nodes().size(),
                city.roads().segments().size(), city.roads().blocks().size());
    std::printf("road length %.1f km\n", city.roads().TotalLength() / 1000.0);
    std::printf("buildings %zu  props %zu\n", city.buildings().size(), city.props().size());
    std::printf("homes %zu (cap %u)  workplaces %zu (cap %u)\n",
                city.homes().size(), city.totalResidentCapacity(),
                city.workplaces().size(), city.totalJobCapacity());
    return 0;
}
