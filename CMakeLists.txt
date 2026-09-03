cmake_minimum_required(VERSION 3.20)

project(CnaCity LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # A hundred thousand agents at -O0 measures the debugger rather than the runtime, and the
    # whole point of this program is the second number.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# cna-city drives ../cnanext (the CNA runtime) through the sibling
# ../sharp-runtimenext checkout, the same layout cna-samples, cna-killer and cna-rts use.
# ---------------------------------------------------------------------------

if(EMSCRIPTEN)
    set(_cna_city_reference_renderer "WEBGL2")
else()
    set(_cna_city_reference_renderer "OPENGLES3")
endif()
set(CNA_GRAPHICS_RENDERER "${_cna_city_reference_renderer}" CACHE STRING
    "CNA City reference renderer")

set(CNA_SHARP_RUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../sharp-runtimenext"
    CACHE PATH "sharp-runtimenext checkout used by cna-city")

# Both are sibling checkouts rather than submodules, so `git submodule update --init` will not
# fetch either. Saying so here costs one branch and saves whoever clones this a confusing wall of
# "no such file" from deep inside CNA's own configure.
foreach(_sibling
        "${CMAKE_CURRENT_SOURCE_DIR}/../cnanext"
        "${CNA_SHARP_RUNTIME_ROOT}")
    if(NOT EXISTS "${_sibling}/CMakeLists.txt")
        message(FATAL_ERROR
            "cna-city: missing sibling checkout at ${_sibling}.\n"
            "  cna-city expects ../cnanext and ../sharp-runtimenext next to it -- they are "
            "separate git checkouts, not submodules. Point elsewhere with "
            "-DCNA_SHARP_RUNTIME_ROOT=<path>.")
    endif()
endforeach()

# The CNAEXT engine layer is OFF by default in CNA. This demo exists to exercise it, so it is the
# one consumer in the workspace that turns it on unconditionally rather than as an option.
set(CNA_CNAEXT ON CACHE BOOL "Enable the CNAEXT extended graphics layer" FORCE)

# cna-city reaches for System::Threading::Tasks::Parallel, which is not in CNA's own component
# closure -- sharp-runtime instantiates a component only when something asks for it, so the target
# would not exist and the include path would not propagate. Naming the closure here, before CNA is
# added, is the supported way to widen it.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cnanext/cmake/SharpRuntimeConsumption.cmake")
set(SHARP_RUNTIME_COMPONENTS "${CNA_SHARP_RUNTIME_DEFAULT_COMPONENTS};Threading.Tasks;Diagnostics"
    CACHE STRING "Sharp Runtime components required by cna-city")

set(CNA_BUILD_TESTS    OFF CACHE BOOL "Build CNA tests")
set(CNA_BUILD_EXAMPLES OFF CACHE BOOL "Build CNA examples")
set(SHARP_RUNTIME_BUILD_TESTS OFF CACHE BOOL "Build sharp-runtime tests")

add_subdirectory(../cnanext CNA_BUILD)

# ---------------------------------------------------------------------------
# The simulation half is a library, and the split is the same one `--headless`
# already draws: everything below needs no graphics device, which is why the
# benchmark can run the whole city without one. Making it a target rather than
# a set of source files is what lets the tests link the city without linking a
# renderer -- and `MeshBuilder` is on this side because the winding rule it
# encodes has cost three defects and is worth asserting on directly.
# ---------------------------------------------------------------------------
add_library(cna-city-sim STATIC
    src/Rng.cpp
    src/CityMath.cpp
    src/RoadNetwork.cpp
    src/City.cpp
    src/MetroNetwork.cpp
    src/BusNetwork.cpp
    src/Pathfinder.cpp
    src/WorldState.cpp
    src/JobSystem.cpp
    src/Agents.cpp
    src/Traffic.cpp
    src/Simulation.cpp
    src/Checksum.cpp
    src/Replay.cpp
    src/MeshBuilder.cpp
)

target_include_directories(cna-city-sim PUBLIC src)
target_link_libraries(cna-city-sim PUBLIC CNA)

find_package(Threads REQUIRED)
target_link_libraries(cna-city-sim PUBLIC Threads::Threads)

add_executable(cna-city
    src/Program.cpp
    src/Materials.cpp
    src/CityGeometry.cpp
    src/SkyLighting.cpp
    src/InstanceRenderer.cpp
    src/TextRenderer.cpp
    src/CliOptions.cpp
    src/CityGame.cpp
    src/Benchmark.cpp
)

target_include_directories(cna-city PRIVATE src)
target_link_libraries(cna-city PRIVATE cna-city-sim CNA)

# CNA links what it needs itself, but cna-city reaches for System::Diagnostics::Stopwatch and
# System::Threading::Tasks::Parallel directly, so it names those components rather than relying on
# CNA's linkage staying PUBLIC.
foreach(_component Core.Base Threading Threading.Tasks Diagnostics)
    if(TARGET SharpRuntime::${_component})
        target_link_libraries(cna-city-sim PUBLIC SharpRuntime::${_component})
    endif()
endforeach()

if(MSVC)
    target_compile_options(cna-city PRIVATE /W4)
    target_compile_options(cna-city-sim PRIVATE /W4)
else()
    target_compile_options(cna-city PRIVATE -Wall -Wextra)
    target_compile_options(cna-city-sim PRIVATE -Wall -Wextra)
endif()

if(EMSCRIPTEN)
    set_target_properties(cna-city PROPERTIES SUFFIX ".html")
    # Game::Run() is CNA's blocking XNA loop contract, so a web build needs the Asyncify-based
    # browser-loop suspension policy cna-samples documents.
    target_link_libraries(cna-city PRIVATE CNA::EmscriptenAsyncify)
    target_link_options(cna-city PRIVATE
        -sALLOW_MEMORY_GROWTH=1
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
    )
endif()


# ---------------------------------------------------------------------------
# Tests
#
# GoogleTest is the workspace's convention and CNA already vendors a checkout,
# so this reuses that one rather than fetching a second: `../cnanext/vendor/
# googletest` is a 30 MB tree and there is no reason for two of them on a disk.
# CNA's own test build is off here (CNA_BUILD_TESTS), so the subdirectory is not
# already added and this has to add it itself.
# ---------------------------------------------------------------------------
option(CNA_CITY_BUILD_TESTS "Build the cna-city test suite" ${PROJECT_IS_TOP_LEVEL})

if(CNA_CITY_BUILD_TESTS)
    set(_cna_city_gtest "${CMAKE_CURRENT_SOURCE_DIR}/../cnanext/vendor/googletest")
    if(NOT EXISTS "${_cna_city_gtest}/CMakeLists.txt")
        message(WARNING
            "cna-city: no googletest at ${_cna_city_gtest}; the test suite will not be built. "
            "It lives in CNA's vendor directory -- run CNA's own bootstrap, or turn this off "
            "with -DCNA_CITY_BUILD_TESTS=OFF.")
        set(CNA_CITY_BUILD_TESTS OFF)
    endif()
endif()

if(CNA_CITY_BUILD_TESTS)
    enable_testing()
    if(NOT TARGET gtest)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_cna_city_gtest}" "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
    endif()

    add_executable(cna-city-tests
        tests/TestMain.cpp
        tests/WindingTests.cpp
        tests/RoadNetworkTests.cpp
        tests/CityGenerationTests.cpp
        tests/PathfinderTests.cpp
        tests/WorldStateTests.cpp
        tests/AgentScheduleTests.cpp
        tests/TrafficTests.cpp
        tests/MetroTests.cpp
        tests/BusTests.cpp
        tests/DeterminismTests.cpp
        tests/ReplayTests.cpp
    )
    target_link_libraries(cna-city-tests PRIVATE cna-city-sim gtest gmock)
    if(NOT MSVC)
        target_compile_options(cna-city-tests PRIVATE -Wall -Wextra)
    endif()

    include(GoogleTest)
    # One CTest case per suite rather than per assertion: several of these build a whole city and
    # simulate hours of it, so gtest_discover_tests' per-case process would pay that cost dozens of
    # times over.
    add_test(NAME CnaCityTests COMMAND cna-city-tests)
    set_tests_properties(CnaCityTests PROPERTIES TIMEOUT 900)
endif()
