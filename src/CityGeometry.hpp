// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "City.hpp"
#include "Materials.hpp"
#include "MeshBuilder.hpp"
#include "MetroNetwork.hpp"
#include "Microsoft/Xna/Framework/BoundingBox.hpp"

namespace CnaCity
{
    /**
     * @brief One spatial tile of the static city, holding one mesh per material it uses.
     *
     * The chunk is the unit of culling and the unit of drawing. It has to be both: culling per
     * building would cost more than drawing them, and drawing the whole city in one buffer would
     * mean no culling at all. A few hundred metres square puts a typical frame at fifteen to forty
     * visible chunks and two or three materials each -- around a hundred draw calls for a city of
     * twelve thousand buildings.
     */
    struct GeometryChunk
    {
        Microsoft::Xna::Framework::BoundingBox bounds;
        std::array<std::unique_ptr<GpuMesh>, kCityMaterialCount> meshes;
        int triangles = 0;
    };

    /**
     * @brief The static city as GPU geometry: ground, carriageways, footways, blocks and buildings.
     *
     * Buildings are *not* instanced, and that is a deliberate reversal of what a demo like this
     * usually does. Twelve thousand boxes is a quarter of a million vertices -- nothing -- and
     * baking them into per-chunk buffers buys the one thing instancing cannot: every building's
     * UVs are computed from its own dimensions, so a window is 3.2 m wide on a bungalow and 3.2 m
     * wide on a tower. Instancing would force one UV scale for every instance of a mesh, and the
     * stretched facades that produces are the classic tell of a generated city.
     *
     * Instancing is used where it belongs -- for the tens of thousands of *identical* lamps,
     * trees, vehicles and people -- in InstanceRenderer.
     */
    class CityGeometry
    {
    public:
        void Build(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                   const MaterialLibrary& materials, const City& city, const MetroNetwork& metro,
                   std::uint64_t seed);
        void Release();

        [[nodiscard]] const std::vector<GeometryChunk>& chunks() const { return chunks_; }
        [[nodiscard]] int totalTriangles() const { return totalTriangles_; }
        [[nodiscard]] std::size_t bytes() const { return bytes_; }
        [[nodiscard]] const Microsoft::Xna::Framework::BoundingBox& bounds() const { return bounds_; }

    private:
        [[nodiscard]] std::uint32_t ChunkOf(Vec2 point) const;

        std::vector<GeometryChunk> chunks_;
        Microsoft::Xna::Framework::BoundingBox bounds_;
        Vec2 origin_{0.0f, 0.0f};
        float chunkSize_ = 340.0f;
        int side_ = 1;
        int totalTriangles_ = 0;
        std::size_t bytes_ = 0;
    };
}
