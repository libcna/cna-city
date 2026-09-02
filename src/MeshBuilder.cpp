// SPDX-License-Identifier: MIT
#include "MeshBuilder.hpp"

#include <cmath>

#include "Microsoft/Xna/Framework/Graphics/BufferUsage.hpp"
#include "Microsoft/Xna/Framework/Graphics/IndexElementSize.hpp"
#include "Microsoft/Xna/Framework/Graphics/PrimitiveType.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        /**
         * @brief A tangent for a face whose normal is @p normal, aligned with the U axis.
         *
         * The W component is the handedness the shader multiplies the bitangent by. Everything
         * here is built with a consistent winding and a consistent UV direction, so it is always
         * +1; it is written out rather than assumed because a normal map on a mirrored face with
         * the wrong handedness lights from the wrong side and looks like a shading bug rather than
         * a tangent bug.
         */
        Vector4 TangentFor(const Vector3& normal, const Vector3& uDirection)
        {
            Vector3 tangent = uDirection - normal * Vector3::Dot(normal, uDirection);
            const float length = tangent.Length();
            if (length < 1e-5f)
            {
                // Degenerate: pick any axis not parallel to the normal.
                tangent = std::fabs(normal.Y) > 0.9f ? Vector3(1.0f, 0.0f, 0.0f)
                                                     : Vector3(0.0f, 1.0f, 0.0f);
                tangent = tangent - normal * Vector3::Dot(normal, tangent);
            }
            tangent = Vector3::Normalize(tangent);
            return Vector4(tangent.X, tangent.Y, tangent.Z, 1.0f);
        }
    }

    void MeshData::AddQuad(const Vector3& a, const Vector3& b, const Vector3& c, const Vector3& d,
                           const Vector3& normal, Vec2 uvMin, Vec2 uvMax)
    {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const Vector4 tangent = TangentFor(normal, Vector3::Normalize(b - a));
        vertices.emplace_back(a, normal, tangent, Vector2(uvMin.X, uvMax.Y));
        vertices.emplace_back(b, normal, tangent, Vector2(uvMax.X, uvMax.Y));
        vertices.emplace_back(c, normal, tangent, Vector2(uvMax.X, uvMin.Y));
        vertices.emplace_back(d, normal, tangent, Vector2(uvMin.X, uvMin.Y));
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    void MeshData::AddBox(Vec2 center, float baseY, Vec2 halfExtent, float height, float rotation,
                          Vec2 uvScaleSides, Vec2 uvOriginSides, bool includeTop, Vec2 topUvScale)
    {
        const Vec2 axisU = FromHeading(rotation);
        const Vec2 axisV = Perp(axisU);
        const Vec2 u = axisU * halfExtent.X;
        const Vec2 v = axisV * halfExtent.Y;
        const float top = baseY + height;

        // The four ground corners, counter-clockwise seen from above.
        const Vec2 corner[4] = {center - u - v, center + u - v, center + u + v, center - u + v};

        // Each wall's U range continues from the last, so a facade texture wraps the building
        // rather than restarting at every corner -- the difference between a building and four
        // billboards stapled together.
        float uCursor = uvOriginSides.X;
        for (int i = 0; i < 4; ++i)
        {
            const Vec2 a2 = corner[i];
            const Vec2 b2 = corner[(i + 1) % 4];
            const Vec2 edge = b2 - a2;
            const float width = Length(edge);
            const Vec2 outward = Normalized(Perp(edge)) * -1.0f;   // CCW ground ring => right is out
            const Vector3 normal(outward.X, 0.0f, outward.Y);
            const float uEnd = uCursor + width * uvScaleSides.X;
            AddQuad(ToWorld(a2, baseY), ToWorld(b2, baseY), ToWorld(b2, top), ToWorld(a2, top),
                    normal, Vec2(uCursor, uvOriginSides.Y),
                    Vec2(uEnd, uvOriginSides.Y + height * uvScaleSides.Y));
            uCursor = uEnd;
        }

        if (includeTop)
            AddQuad(ToWorld(corner[3], top), ToWorld(corner[2], top), ToWorld(corner[1], top),
                    ToWorld(corner[0], top), Vector3(0.0f, 1.0f, 0.0f), Vec2(0.0f, 0.0f),
                    Vec2(topUvScale.X, topUvScale.Y));
    }

    void MeshData::AddRibbon(Vec2 from, Vec2 to, float halfWidth, float y, float uStart, float uEnd,
                             float vMin, float vMax)
    {
        const Vec2 dir = Normalized(to - from);
        if (LengthSq(dir) < 0.5f) return;
        const Vec2 side = Perp(dir) * halfWidth;
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const Vector3 normal(0.0f, 1.0f, 0.0f);
        const Vector4 tangent = TangentFor(normal, Vector3(dir.X, 0.0f, dir.Y));
        // The ring runs +side, -side, so that its winding matches every other upward-facing
        // surface in this file. It used to run the other way, and the consequence was not subtle:
        // every carriageway in the city was back-facing and therefore invisible, and what showed
        // through instead was the ground plane -- a strip of grass down the middle of every
        // street, with the lamp posts and the street trees correctly placed on either side of it.
        //
        // The convention is worth writing down because it is not the one the vertex normal
        // suggests. Under this renderer's default rasterizer state a triangle is drawn when its
        // right-hand-rule winding normal points *away* from the camera, so an upward-facing ground
        // quad must be wound clockwise as seen from above -- the opposite of what "counter-
        // clockwise is front-facing" would lead you to write.
        vertices.emplace_back(ToWorld(from + side, y), normal, tangent, Vector2(uStart, vMax));
        vertices.emplace_back(ToWorld(from - side, y), normal, tangent, Vector2(uStart, vMin));
        vertices.emplace_back(ToWorld(to - side, y), normal, tangent, Vector2(uEnd, vMin));
        vertices.emplace_back(ToWorld(to + side, y), normal, tangent, Vector2(uEnd, vMax));
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    void MeshData::AddPrism(const std::vector<Vec2>& outline, float baseY, float height, float uvScale)
    {
        const std::size_t n = outline.size();
        if (n < 3) return;
        const float top = baseY + height;
        float uCursor = 0.0f;
        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec2 a2 = outline[i];
            const Vec2 b2 = outline[(i + 1) % n];
            const Vec2 edge = b2 - a2;
            const float width = Length(edge);
            if (width < 1e-3f) continue;
            const Vec2 outward = Normalized(Perp(edge)) * -1.0f;
            const Vector3 normal(outward.X, 0.0f, outward.Y);
            const float uEnd = uCursor + width * uvScale;
            AddQuad(ToWorld(a2, baseY), ToWorld(b2, baseY), ToWorld(b2, top), ToWorld(a2, top),
                    normal, Vec2(uCursor, 0.0f), Vec2(uEnd, height * uvScale));
            uCursor = uEnd;
        }

        // A fan from the first vertex. Correct for the convex outlines this is called with, and
        // the block insets that produce them are rejected when they stop being convex enough.
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const Vector3 up(0.0f, 1.0f, 0.0f);
        const Vector4 tangent = TangentFor(up, Vector3(1.0f, 0.0f, 0.0f));
        for (const Vec2& p : outline)
            vertices.emplace_back(ToWorld(p, top), up, tangent,
                                  Vector2(p.X * uvScale, p.Y * uvScale));
        for (std::uint32_t i = 1; i + 1 < n; ++i)
            indices.insert(indices.end(), {base, base + i + 1, base + i});
    }

    void MeshData::AddCylinder(Vec2 center, float baseY, float radius, float height, int sides,
                               Vec2 uvMin, Vec2 uvMax, bool cap)
    {
        if (sides < 3) sides = 3;
        const float top = baseY + height;
        for (int i = 0; i < sides; ++i)
        {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
            const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / static_cast<float>(sides);
            const Vec2 p0 = center + FromHeading(a0) * radius;
            const Vec2 p1 = center + FromHeading(a1) * radius;
            const Vec2 mid = FromHeading((a0 + a1) * 0.5f);
            const Vector3 normal(mid.X, 0.0f, mid.Y);
            const float u0 = Lerp(Vec2(uvMin.X, 0.0f), Vec2(uvMax.X, 0.0f),
                                  static_cast<float>(i) / static_cast<float>(sides)).X;
            const float u1 = Lerp(Vec2(uvMin.X, 0.0f), Vec2(uvMax.X, 0.0f),
                                  static_cast<float>(i + 1) / static_cast<float>(sides)).X;
            AddQuad(ToWorld(p0, baseY), ToWorld(p1, baseY), ToWorld(p1, top), ToWorld(p0, top),
                    normal, Vec2(u0, uvMin.Y), Vec2(u1, uvMax.Y));
        }
        if (!cap) return;
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const Vector3 up(0.0f, 1.0f, 0.0f);
        const Vector4 tangent = TangentFor(up, Vector3(1.0f, 0.0f, 0.0f));
        for (int i = 0; i < sides; ++i)
        {
            const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
            const Vec2 p = center + FromHeading(a) * radius;
            vertices.emplace_back(ToWorld(p, top), up, tangent,
                                  Vector2(Lerp(uvMin, uvMax, 0.5f + 0.5f * std::cos(a)).X,
                                          Lerp(uvMin, uvMax, 0.5f + 0.5f * std::sin(a)).Y));
        }
        for (std::uint32_t i = 1; i + 1 < static_cast<std::uint32_t>(sides); ++i)
            indices.insert(indices.end(), {base, base + i, base + i + 1});
    }

    void MeshData::AddCone(Vec2 center, float baseY, float radius, float height, int sides,
                           Vec2 uvMin, Vec2 uvMax)
    {
        if (sides < 3) sides = 3;
        const Vector3 apex = ToWorld(center, baseY + height);
        for (int i = 0; i < sides; ++i)
        {
            const float a0 = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
            const float a1 = 2.0f * kPi * static_cast<float>(i + 1) / static_cast<float>(sides);
            const Vec2 p0 = center + FromHeading(a0) * radius;
            const Vec2 p1 = center + FromHeading(a1) * radius;
            const Vec2 mid = FromHeading((a0 + a1) * 0.5f);
            // The slope is what the normal has to follow; a purely horizontal one makes a cone
            // shade like a cylinder, which for a conifer reads as a cardboard tube.
            const float slope = radius / std::max(0.01f, height);
            Vector3 normal(mid.X, slope, mid.Y);
            normal = Vector3::Normalize(normal);
            const auto base = static_cast<std::uint32_t>(vertices.size());
            const Vector4 tangent = TangentFor(normal, ToWorld(p1 - p0, 0.0f));
            vertices.emplace_back(ToWorld(p0, baseY), normal, tangent, Vector2(uvMin.X, uvMax.Y));
            vertices.emplace_back(ToWorld(p1, baseY), normal, tangent, Vector2(uvMax.X, uvMax.Y));
            vertices.emplace_back(apex, normal, tangent,
                                  Vector2((uvMin.X + uvMax.X) * 0.5f, uvMin.Y));
            indices.insert(indices.end(), {base, base + 1, base + 2});
        }
    }

    void MeshData::AddSphere(const Vector3& center, float radius, int slices, int stacks,
                             Vec2 uvMin, Vec2 uvMax)
    {
        if (slices < 3) slices = 3;
        if (stacks < 2) stacks = 2;
        const auto base = static_cast<std::uint32_t>(vertices.size());
        for (int y = 0; y <= stacks; ++y)
        {
            const float v = static_cast<float>(y) / static_cast<float>(stacks);
            const float phi = v * kPi;
            for (int x = 0; x <= slices; ++x)
            {
                const float u = static_cast<float>(x) / static_cast<float>(slices);
                const float theta = u * 2.0f * kPi;
                const Vector3 normal(std::sin(phi) * std::cos(theta), std::cos(phi),
                                     std::sin(phi) * std::sin(theta));
                const Vector3 position(center.X + normal.X * radius, center.Y + normal.Y * radius,
                                       center.Z + normal.Z * radius);
                const Vector4 tangent = TangentFor(normal, Vector3(-std::sin(theta), 0.0f, std::cos(theta)));
                vertices.emplace_back(position, normal, tangent,
                                      Vector2(Lerp(uvMin, uvMax, u).X, Lerp(uvMin, uvMax, v).Y));
            }
        }
        const auto stride = static_cast<std::uint32_t>(slices + 1);
        for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(stacks); ++y)
            for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(slices); ++x)
            {
                const std::uint32_t i0 = base + y * stride + x;
                indices.insert(indices.end(), {i0, i0 + stride, i0 + 1,
                                               i0 + 1, i0 + stride, i0 + stride + 1});
            }
    }

    void MeshData::Append(const MeshData& other, const Vector3& translation, float rotation,
                          float scale)
    {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const float c = std::cos(rotation);
        const float s = std::sin(rotation);
        vertices.reserve(vertices.size() + other.vertices.size());
        for (const CityVertex& vertex : other.vertices)
        {
            const Vector3& p = vertex.Position;
            const Vector3& n = vertex.Normal;
            const Vector4& t = vertex.Tangent;
            const Vector3 rotatedPosition(p.X * c - p.Z * s, p.Y, p.X * s + p.Z * c);
            const Vector3 rotatedNormal(n.X * c - n.Z * s, n.Y, n.X * s + n.Z * c);
            const Vector4 rotatedTangent(t.X * c - t.Z * s, t.Y, t.X * s + t.Z * c, t.W);
            vertices.emplace_back(Vector3(rotatedPosition.X * scale + translation.X,
                                          rotatedPosition.Y * scale + translation.Y,
                                          rotatedPosition.Z * scale + translation.Z),
                                  rotatedNormal, rotatedTangent, vertex.TextureCoordinate);
        }
        indices.reserve(indices.size() + other.indices.size());
        for (std::uint32_t index : other.indices) indices.push_back(base + index);
    }

    GpuMesh::~GpuMesh() = default;

    void GpuMesh::Release()
    {
        part_.reset();
        indexBuffer_.reset();
        vertexBuffer_.reset();
        triangles_ = 0;
        vertices_ = 0;
    }

    std::size_t GpuMesh::bytes() const
    {
        return static_cast<std::size_t>(vertices_) * sizeof(CityVertex) +
               static_cast<std::size_t>(triangles_) * 3 * sizeof(std::uint32_t);
    }

    bool GpuMesh::Upload(GraphicsDevice& device, const MeshData& data)
    {
        Release();
        if (data.indices.empty() || data.vertices.empty()) return false;

        vertexBuffer_ = std::make_unique<VertexBuffer>(
            device, CityVertex::getVertexDeclarationStatic(),
            static_cast<int>(data.vertices.size()), BufferUsage::WriteOnly);
        vertexBuffer_->SetData(data.vertices.data(), static_cast<int>(data.vertices.size()));

        indexBuffer_ = std::make_unique<IndexBuffer>(device, IndexElementSize::ThirtyTwoBits,
                                                     static_cast<int>(data.indices.size()),
                                                     BufferUsage::WriteOnly);
        indexBuffer_->SetData(data.indices.data(), static_cast<int>(data.indices.size()));

        vertices_ = static_cast<int>(data.vertices.size());
        triangles_ = static_cast<int>(data.indices.size() / 3);
        part_ = std::make_unique<ModelMeshPart>(vertexBuffer_.get(), indexBuffer_.get(), vertices_,
                                                triangles_, 0, 0);
        return true;
    }

    void GpuMesh::Draw(GraphicsDevice& device) const
    {
        if (part_ == nullptr) return;
        device.SetVertexBuffer(vertexBuffer_.get());
        device.setIndicesProperty(indexBuffer_.get());
        device.DrawIndexedPrimitives(PrimitiveType::TriangleList, 0, 0, vertices_, 0, triangles_);
    }
}
