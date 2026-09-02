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

add_executable(cna-city
    src/Program.cpp
    src/Rng.cpp
    src/CityMath.cpp
    src/RoadNetwork.cpp
    src/City.cpp
    src/MetroNetwork.cpp
    src/Pathfinder.cpp
    src/WorldState.cpp
    src/JobSystem.cpp
    src/Agents.cpp
    src/Traffic.cpp
    src/Simulation.cpp
    src/MeshBuilder.cpp
    src/Materials.cpp
    src/CityGeometry.cpp
    src/InstanceRenderer.cpp
    src/TextRenderer.cpp
    src/CliOptions.cpp
    src/CityGame.cpp
    src/Benchmark.cpp
)

target_include_directories(cna-city PRIVATE src)
target_link_libraries(cna-city PRIVATE CNA)

find_package(Threads REQUIRED)
target_link_libraries(cna-city PRIVATE Threads::Threads)

# CNA links what it needs itself, but cna-city reaches for System::Diagnostics::Stopwatch and
# System::Threading::Tasks::Parallel directly, so it names those components rather than relying on
# CNA's linkage staying PUBLIC.
foreach(_component Core.Base Threading Threading.Tasks Diagnostics)
    if(TARGET SharpRuntime::${_component})
        target_link_libraries(cna-city PRIVATE SharpRuntime::${_component})
    endif()
endforeach()

if(MSVC)
    target_compile_options(cna-city PRIVATE /W4)
else()
    target_compile_options(cna-city PRIVATE -Wall -Wextra)
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

