// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "CityMath.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsDevice.hpp"
#include "Microsoft/Xna/Framework/Graphics/IndexBuffer.hpp"
#include "Microsoft/Xna/Framework/Graphics/ModelMeshPart.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexBuffer.hpp"
#include "Microsoft/Xna/Framework/Graphics/VertexPositionNormalTangentTexture.hpp"
#include "Microsoft/Xna/Framework/Vector4.hpp"

namespace CnaCity
{
    using CityVertex = Microsoft::Xna::Framework::Graphics::VertexPositionNormalTangentTexture;

    /**
     * @brief A CPU-side triangle mesh being assembled, before it becomes a GPU buffer.
     *
     * Everything visible in this city is built here at start-up: there is no content pipeline and
     * no asset on disk, which is deliberate. A demo that has to ship a gigabyte of models to look
     * like a city is measuring the disk, and a procedural city that then loads its buildings from
     * files is only half a procedural city.
     *
     * The vertex format carries a tangent because the PBR effect's normal map needs one, and a
     * tangent computed per-face at build time is both cheaper and more correct than one derived in
     * a shader from screen-space derivatives.
     */
    struct MeshData
    {
        std::vector<CityVertex> vertices;
        std::vector<std::uint32_t> indices;

        void Clear() { vertices.clear(); indices.clear(); }
        [[nodiscard]] bool empty() const { return indices.empty(); }
        [[nodiscard]] std::size_t triangleCount() const { return indices.size() / 3; }

        /** @brief One quad, wound a-b-c-d counter-clockwise as seen from the normal's side. */
        void AddQuad(const Microsoft::Xna::Framework::Vector3& a,
                     const Microsoft::Xna::Framework::Vector3& b,
                     const Microsoft::Xna::Framework::Vector3& c,
                     const Microsoft::Xna::Framework::Vector3& d,
                     const Microsoft::Xna::Framework::Vector3& normal,
                     Vec2 uvMin, Vec2 uvMax);

        /**
         * @brief An axis-aligned-in-local-space box, rotated about Y and placed in the world.
         *
         * @param uvScaleSides Texture repeats across the walls: X along the wall, Y up it. Passing
         *        the building's real size in metres divided by the facade tile size is what makes
         *        every building's windows the same physical size regardless of how big it is.
         */
        void AddBox(Vec2 center, float baseY, Vec2 halfExtent, float height, float rotation,
                    Vec2 uvScaleSides, Vec2 uvOriginSides, bool includeTop, Vec2 topUvScale);

        /** @brief A flat horizontal ribbon along a polyline segment: roads, pavements, markings. */
        void AddRibbon(Vec2 from, Vec2 to, float halfWidth, float y, float uStart, float uEnd,
                       float vMin, float vMax);

        /** @brief A closed vertical prism from an arbitrary convex-ish polygon. */
        void AddPrism(const std::vector<Vec2>& outline, float baseY, float height, float uvScale);

        /** @brief A cylinder around the Y axis; @p sides 6 is plenty for a lamp post. */
        void AddCylinder(Vec2 center, float baseY, float radius, float height, int sides,
                         Vec2 uvMin, Vec2 uvMax, bool cap);

        /** @brief A cone, for conifers and lamp shades. */
        void AddCone(Vec2 center, float baseY, float radius, float height, int sides,
                     Vec2 uvMin, Vec2 uvMax);

        /** @brief A low-detail sphere, for tree canopies. */
        void AddSphere(const Microsoft::Xna::Framework::Vector3& center, float radius, int slices,
                       int stacks, Vec2 uvMin, Vec2 uvMax);

        /** @brief Appends @p other transformed by a yaw, a uniform scale and a translation. */
        void Append(const MeshData& other, const Microsoft::Xna::Framework::Vector3& translation,
                    float rotation, float scale);
    };

    /**
     * @brief A mesh living on the device: the two buffers plus the part that names them.
     *
     * Index buffers are 32-bit throughout. The building layer alone puts a quarter of a million
     * vertices into a single chunk's buffer, and a 16-bit index is a ceiling somebody hits at
     * exactly the wrong moment.
     */
    class GpuMesh
    {
    public:
        GpuMesh() = default;
        ~GpuMesh();
        GpuMesh(const GpuMesh&) = delete;
        GpuMesh& operator=(const GpuMesh&) = delete;
        GpuMesh(GpuMesh&&) noexcept = default;
        GpuMesh& operator=(GpuMesh&&) noexcept = default;

        bool Upload(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device, const MeshData& data);
        void Release();

        [[nodiscard]] bool valid() const { return part_ != nullptr; }
        [[nodiscard]] Microsoft::Xna::Framework::Graphics::ModelMeshPart* part() const { return part_.get(); }
        [[nodiscard]] int triangleCount() const { return triangles_; }
        [[nodiscard]] int vertexCount() const { return vertices_; }
        [[nodiscard]] std::size_t bytes() const;

        /** @brief Binds the buffers and issues one indexed draw. The effect must already be applied. */
        void Draw(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device) const;

    private:
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::VertexBuffer> vertexBuffer_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::IndexBuffer> indexBuffer_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::ModelMeshPart> part_;
        int triangles_ = 0;
        int vertices_ = 0;
    };
}
