// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "City.hpp"
#include "MeshBuilder.hpp"
#include "Simulation.hpp"

namespace CnaCityTests
{
    using namespace CnaCity;
    using Microsoft::Xna::Framework::Vector3;

    /**
     * @brief A city small enough that a test can build one per suite, large enough to be a city.
     *
     * The default is 3.3 km across with twelve thousand buildings, which takes about 25 ms to
     * generate -- fine once, wasteful thirty times. This is a quarter of the side, which still
     * produces a full arterial grid, districts of every zone, blocks, buildings and a metro.
     */
    inline CityConfig SmallCityConfig(std::uint64_t seed = 4242)
    {
        CityConfig config;
        config.seed = seed;
        config.halfSize = 620.0f;
        config.arterialSpacing = 300.0f;
        config.diagonalAvenues = 1;
        return config;
    }

    inline SimConfig SmallSimConfig(std::uint32_t agents = 4000, std::uint64_t seed = 4242)
    {
        SimConfig config;
        config.city = SmallCityConfig(seed);
        config.agentCount = agents;
        config.metroLines = 3;
        config.busRoutes = 4;
        config.randomWeather = false;
        config.threads = 1;   // Deterministic ordering; the thread-count sweep is its own test.
        return config;
    }

    /**
     * @brief CNA's face rule, as a predicate a test can assert on.
     *
     * `modules/graphics/examples/frontface_winding_test.cpp` states it: under
     * `CullCounterClockwise` a triangle that appears clockwise on screen is the front face, so in
     * world terms a drawn triangle's right-hand-rule normal points *away* from the viewer -- into
     * the solid it bounds. Everything in the winding tests is this one line.
     */
    [[nodiscard]] inline bool TriangleFacesViewer(const Vector3& a, const Vector3& b,
                                                  const Vector3& c, const Vector3& eye)
    {
        const Vector3 e1(b.X - a.X, b.Y - a.Y, b.Z - a.Z);
        const Vector3 e2(c.X - a.X, c.Y - a.Y, c.Z - a.Z);
        const Vector3 winding(e1.Y * e2.Z - e1.Z * e2.Y, e1.Z * e2.X - e1.X * e2.Z,
                              e1.X * e2.Y - e1.Y * e2.X);
        const Vector3 toEye(eye.X - a.X, eye.Y - a.Y, eye.Z - a.Z);
        return winding.X * toEye.X + winding.Y * toEye.Y + winding.Z * toEye.Z < 0.0f;
    }

    /** @brief How many of @p mesh's triangles would be drawn for a viewer at @p eye. */
    [[nodiscard]] inline int VisibleTriangleCount(const MeshData& mesh, const Vector3& eye)
    {
        int visible = 0;
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            if (TriangleFacesViewer(mesh.vertices[mesh.indices[i]].Position,
                                    mesh.vertices[mesh.indices[i + 1]].Position,
                                    mesh.vertices[mesh.indices[i + 2]].Position, eye))
                ++visible;
        return visible;
    }

    [[nodiscard]] inline int TriangleCount(const MeshData& mesh)
    {
        return static_cast<int>(mesh.indices.size() / 3);
    }

    /** @brief Steps @p sim by @p simulatedSeconds in realistic frame-sized slices. */
    inline void RunFor(Simulation& sim, float simulatedSeconds, float sliceSeconds = 1.0f)
    {
        const int slices = static_cast<int>(simulatedSeconds / sliceSeconds);
        for (int i = 0; i < slices; ++i) sim.Step(sliceSeconds);
    }
}
