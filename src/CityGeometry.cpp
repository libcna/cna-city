// SPDX-License-Identifier: MIT
#include "CityGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Rng.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        /// Heights, in metres, of the layers that share the ground plane. They are separated by
        /// centimetres rather than by a depth-bias trick because a city seen from four hundred
        /// metres up has a depth buffer stretched thin enough that polygon offset stops working,
        /// and a road that flickers against the pavement is the first thing anybody notices.
        constexpr float kGroundY   = -0.40f;   ///< Open land outside the city.
        constexpr float kFillY     = -0.55f;   ///< Well under the carriageway; a safety net only.
        constexpr float kRoadY     =  0.00f;
        constexpr float kKerbY     =  0.12f;   ///< The raised footway.
        constexpr float kBlockY    =  0.16f;   ///< Courtyards, gardens and parkland.

        /// Which facade material a building kind wears.
        CityMaterial FacadeOf(BuildingKind kind)
        {
            switch (kind)
            {
                case BuildingKind::Tower:     return CityMaterial::GlassTower;
                case BuildingKind::Office:    return CityMaterial::ConcreteOffice;
                case BuildingKind::Apartment: return CityMaterial::BrickApartment;
                case BuildingKind::Shop:      return CityMaterial::BrickApartment;
                case BuildingKind::House:     return CityMaterial::RenderHouse;
                case BuildingKind::Warehouse: return CityMaterial::MetalShed;
            }
            return CityMaterial::ConcreteOffice;
        }

        /// The atlas band a road class occupies, as a v range. See MaterialLibrary::Build.
        void RoadBand(RoadClass roadClass, float& vMin, float& vMax)
        {
            const float band = 1.0f / 8.0f;
            const float base = static_cast<float>(static_cast<int>(roadClass)) * band;
            // Half a texel of inset at 256 px keeps the neighbouring band out of the bilinear tap.
            vMin = base + 0.002f;
            vMax = base + band - 0.002f;
        }
    }

    std::uint32_t CityGeometry::ChunkOf(Vec2 point) const
    {
        const int x = Clamp(static_cast<int>((point.X - origin_.X) / chunkSize_), 0, side_ - 1);
        const int y = Clamp(static_cast<int>((point.Y - origin_.Y) / chunkSize_), 0, side_ - 1);
        return static_cast<std::uint32_t>(y) * side_ + x;
    }

    void CityGeometry::Release()
    {
        chunks_.clear();
        totalTriangles_ = 0;
        bytes_ = 0;
    }

    void CityGeometry::Build(GraphicsDevice& device, const MaterialLibrary& materials,
                             const City& city, const MetroNetwork& metro, std::uint64_t seed)
    {
        Release();
        Rng rng(seed, 0x4745'4f4d'4554'5259ULL);

        const float half = city.config().halfSize;
        origin_ = Vec2(-half - 60.0f, -half - 60.0f);
        const float span = (half + 60.0f) * 2.0f;
        side_ = std::max(1, static_cast<int>(std::ceil(span / chunkSize_)));
        chunks_.resize(static_cast<std::size_t>(side_) * side_);

        // The staging buffers: one MeshData per chunk per material, filled on the CPU and uploaded
        // once at the end. Building them chunk-major would mean walking the whole city once per
        // chunk; building them material-major and scattering is one walk.
        std::vector<std::array<MeshData, kCityMaterialCount>> staging(chunks_.size());
        const auto meshFor = [&](Vec2 at, CityMaterial material) -> MeshData& {
            return staging[ChunkOf(at)][static_cast<int>(material)];
        };

        // ---- The ground ---------------------------------------------------------------------
        //
        // One quad per chunk rather than one for the whole city: a single ground plane cannot be
        // culled, cannot be split across chunks, and at this scale its texture coordinates run
        // into the thousands, where float precision starts to show as visible swimming.
        for (int y = 0; y < side_; ++y)
            for (int x = 0; x < side_; ++x)
            {
                const Vec2 min = origin_ + Vec2(static_cast<float>(x) * chunkSize_,
                                                static_cast<float>(y) * chunkSize_);
                const Vec2 max = min + Vec2(chunkSize_, chunkSize_);
                MeshData& mesh = staging[static_cast<std::size_t>(y) * side_ + x]
                                        [static_cast<int>(CityMaterial::Grass)];
                mesh.AddQuad(ToWorld(Vec2(min.X, min.Y), kGroundY), ToWorld(Vec2(max.X, min.Y), kGroundY),
                             ToWorld(Vec2(max.X, max.Y), kGroundY), ToWorld(Vec2(min.X, max.Y), kGroundY),
                             Vector3(0.0f, 1.0f, 0.0f), Vec2(min.X * 0.2f, min.Y * 0.2f),
                             Vec2(max.X * 0.2f, max.Y * 0.2f));
            }

        // ---- Carriageways ------------------------------------------------------------------------
        const RoadNetwork& roads = city.roads();
        for (const RoadSegment& segment : roads.segments())
        {
            const RoadProfile& profile = ProfileOf(segment.roadClass);
            const Vec2 a = roads.nodes()[segment.nodeA].position;
            const Vec2 b = roads.nodes()[segment.nodeB].position;
            float vMin = 0.0f, vMax = 0.0f;
            RoadBand(segment.roadClass, vMin, vMax);

            // The ribbon is pulled back from each node by the junction's own radius, and the gap
            // is filled by the junction patch below. Without that, two roads of different widths
            // meeting at an angle leave a wedge of grass in the middle of the crossing.
            const float startTrim = std::min(segment.length * 0.35f, profile.carriagewayHalfWidth * 0.9f);
            const float endTrim = startTrim;
            const Vec2 from = a + segment.direction * startTrim;
            const Vec2 to = b - segment.direction * endTrim;
            const float length = std::max(1.0f, segment.length - startTrim - endTrim);
            const float uScale = length / 9.0f;   // matches Material::worldScale.X for asphalt

            MeshData& mesh = meshFor((a + b) * 0.5f, CityMaterial::Asphalt);
            mesh.AddRibbon(from, to, profile.carriagewayHalfWidth, kRoadY, 0.0f, uScale, vMin, vMax);

        }

        // ---- Junction patches ---------------------------------------------------------------------
        for (const RoadNode& node : roads.nodes())
        {
            float radius = 0.0f;
            for (std::uint16_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = roads.incident()[node.firstIncident + k];
                radius = std::max(radius, ProfileOf(roads.segments()[inc.segment].roadClass)
                                              .carriagewayHalfWidth);
            }
            if (radius <= 0.0f) continue;
            float vMin = 0.0f, vMax = 0.0f;
            RoadBand(node.highestClass, vMin, vMax);

            // A twelve-gon: the junction is asphalt, and at any distance where its outline would
            // read as a polygon the whole crossing is a dozen pixels across.
            MeshData& mesh = meshFor(node.position, CityMaterial::Asphalt);
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            const Vector3 up(0.0f, 1.0f, 0.0f);
            const Vector4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
            constexpr int kSides = 12;
            mesh.vertices.emplace_back(ToWorld(node.position, kRoadY), up, tangent,
                                       Vector2(0.5f, (vMin + vMax) * 0.5f));
            for (int i = 0; i <= kSides; ++i)
            {
                const float angle = 2.0f * kPi * static_cast<float>(i % kSides) / kSides;
                const Vec2 p = node.position + FromHeading(angle) * (radius * 1.06f);
                mesh.vertices.emplace_back(ToWorld(p, kRoadY - 0.005f), up, tangent,
                                           Vector2(0.5f + std::cos(angle) * 0.4f,
                                                   (vMin + vMax) * 0.5f + std::sin(angle) *
                                                       (vMax - vMin) * 0.4f));
            }
            for (std::uint32_t i = 1; i <= kSides; ++i)
                mesh.indices.insert(mesh.indices.end(), {base, base + i, base + i + 1});
        }

        // ---- Block interiors -----------------------------------------------------------------------
        //
        // Two rings per block. The outer one runs to the kerb and is the footway; the inner one is
        // the block's interior -- a courtyard, a garden or, in a park, the park. Filling the
        // *whole* area between the kerbs is what closes the gaps a per-street ribbon leaves at
        // every junction and every setback, which is why an earlier version of this had grass
        // showing through the middle of crossroads.
        const auto fillRing = [&](const std::vector<Vec2>& ring, CityMaterial material, float y,
                                  float uvScale) {
            if (ring.size() < 3) return;
            MeshData& mesh = meshFor(PolygonCentroid(ring), material);
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            const Vector3 up(0.0f, 1.0f, 0.0f);
            const Vector4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
            for (const Vec2& p : ring)
                mesh.vertices.emplace_back(ToWorld(p, y), up, tangent,
                                           Vector2(p.X * uvScale, p.Y * uvScale));
            // A fan: these rings are insets of a planar face, and the inset is rejected when it
            // stops being simple, so a fan is safe here.
            for (std::uint32_t i = 1; i + 1 < ring.size(); ++i)
                mesh.indices.insert(mesh.indices.end(), {base, base + i, base + i + 1});
        };

        for (const CityBlock& block : roads.blocks())
        {
            const ZoneType zone = city.districts()[city.DistrictAt(block.centroid)].zone;
            const bool park = zone == ZoneType::Park;
            const CityMaterial hard = park ? CityMaterial::Grass : CityMaterial::Pavement;
            const float hardUv = park ? 1.0f / 5.0f : 1.0f / 2.4f;

            // A safety fill at the very bottom, out to the road centrelines. It is never meant to
            // be the visible surface -- the carriageway and the footway both sit above it -- but
            // it guarantees that no arrangement of insets can leave open ground showing through
            // the middle of a street.
            fillRing(block.outline, hard, kFillY, hardUv);

            // The footway proper, as one quad per block edge running from *that edge's* kerb line
            // to the block's inner ring.
            //
            // This is the second attempt and the first one is worth recording. Filling the whole
            // block polygon out to the road centrelines and letting the carriageway be drawn on
            // top of it works for about eighty metres and then stops: the two surfaces are four
            // centimetres apart, the view ray meets them at a grazing angle, and past a hundred
            // metres the depth difference between them falls below what a 24-bit buffer can
            // resolve. The whole street reads as pavement with a short strip of asphalt at the
            // camera's feet. Two coplanar surfaces that must never overlap have to be made not to
            // overlap, not separated by a distance and hoped for.
            const bool haveKerb = block.kerb.size() == block.outline.size() && block.kerb.size() >= 3;
            if (haveKerb)
            {
                MeshData& mesh = meshFor(block.centroid, hard);
                const Vector3 up(0.0f, 1.0f, 0.0f);
                const Vector4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
                const std::size_t n = block.outline.size();
                for (std::size_t i = 0; i < n; ++i)
                {
                    const Vec2 a = block.outline[i];
                    const Vec2 b = block.outline[(i + 1) % n];
                    const Vec2 along = Normalized(b - a);
                    if (LengthSq(along) < 0.5f) continue;
                    const Vec2 inward = Perp(along);   // the face walk leaves the ring CCW
                    // The half-width of the road this edge actually runs along, rather than the
                    // block's widest: that difference is the four-metre band the first version
                    // left unpaved on every local street of a block that also touched an arterial.
                    const std::uint32_t segment = roads.FindNearestSegment((a + b) * 0.5f);
                    const float halfWidth =
                        segment == 0xFFFFFFFFu
                            ? 3.5f
                            : ProfileOf(roads.segments()[segment].roadClass).carriagewayHalfWidth;
                    const Vec2 kerbA = a + inward * halfWidth;
                    const Vec2 kerbB = b + inward * halfWidth;
                    const Vec2 innerA = block.kerb[i];
                    const Vec2 innerB = block.kerb[(i + 1) % n];

                    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                    const Vec2 corners[4] = {kerbA, kerbB, innerB, innerA};
                    for (const Vec2& p : corners)
                        mesh.vertices.emplace_back(ToWorld(p, kKerbY), up, tangent,
                                                   Vector2(p.X * hardUv, p.Y * hardUv));
                    mesh.indices.insert(mesh.indices.end(),
                                        {base, base + 1, base + 2, base, base + 2, base + 3});
                }
                fillRing(block.kerb, hard, kKerbY, hardUv);
            }

            // The interior: parkland, a suburban garden, or the courtyard of a perimeter block.
            const bool green = park || zone == ZoneType::Suburb || zone == ZoneType::Residential;
            if (green) fillRing(block.buildable, CityMaterial::Grass, kBlockY, 1.0f / 5.0f);
        }

        // ---- Buildings ------------------------------------------------------------------------------
        const float roofScale = 1.0f / std::max(1.0f, materials.Get(CityMaterial::Roof).worldScale.X);
        for (const Building& building : city.buildings())
        {
            const CityMaterial facade = FacadeOf(building.kind);
            MeshData& walls = meshFor(building.center, facade);
            MeshData& roof = meshFor(building.center, CityMaterial::Roof);

            // The UV scale is the whole reason buildings are not instanced: repeats per metre are
            // derived from the *material's* tile size, so one repeat is one bay and one storey
            // whatever the building happens to be. Instanced geometry would have to share one
            // scale across every instance, and the stretched facades that produces are the classic
            // tell of a generated city.
            const Vec2 tile = materials.Get(facade).worldScale;
            const Vec2 uvScale(1.0f / std::max(0.5f, tile.X), 1.0f / std::max(0.5f, tile.Y));

            // Podium first, where there is one, then the tower stepped back on top of it.
            float base = 0.0f;
            if (building.podiumHeight > 0.0f)
            {
                walls.AddBox(building.center, 0.0f, building.podiumHalfExtent, building.podiumHeight,
                             building.rotation, uvScale, Vec2(0.0f, 0.0f), false, Vec2(1, 1));
                roof.AddQuad(
                    ToWorld(building.center + Rotate(Vec2(-building.podiumHalfExtent.X, -building.podiumHalfExtent.Y), building.rotation), building.podiumHeight),
                    ToWorld(building.center + Rotate(Vec2(building.podiumHalfExtent.X, -building.podiumHalfExtent.Y), building.rotation), building.podiumHeight),
                    ToWorld(building.center + Rotate(Vec2(building.podiumHalfExtent.X, building.podiumHalfExtent.Y), building.rotation), building.podiumHeight),
                    ToWorld(building.center + Rotate(Vec2(-building.podiumHalfExtent.X, building.podiumHalfExtent.Y), building.rotation), building.podiumHeight),
                    Vector3(0.0f, 1.0f, 0.0f), Vec2(0.0f, 0.0f),
                    Vec2(building.podiumHalfExtent.X * 2.0f * roofScale,
                         building.podiumHalfExtent.Y * 2.0f * roofScale));
                base = building.podiumHeight;
            }

            walls.AddBox(building.center, base, building.halfExtent,
                         std::max(2.0f, building.height - base), building.rotation, uvScale,
                         Vec2(0.0f, base * uvScale.Y), false, Vec2(1, 1));
            // Wound clockwise as seen from above -- see the winding note in MeshBuilder.hpp. The
            // other order leaves every flat roof in the city back-facing, and because a building's
            // walls are culled from the inside, what you see from above instead is the pavement
            // between the buildings. It reads as a roof, which is why this survived a dozen aerial
            // screenshots.
            roof.AddQuad(
                ToWorld(building.center + Rotate(Vec2(-building.halfExtent.X, -building.halfExtent.Y), building.rotation), building.height),
                ToWorld(building.center + Rotate(Vec2(building.halfExtent.X, -building.halfExtent.Y), building.rotation), building.height),
                ToWorld(building.center + Rotate(Vec2(building.halfExtent.X, building.halfExtent.Y), building.rotation), building.height),
                ToWorld(building.center + Rotate(Vec2(-building.halfExtent.X, building.halfExtent.Y), building.rotation), building.height),
                Vector3(0.0f, 1.0f, 0.0f), Vec2(0.0f, 0.0f),
                Vec2(building.halfExtent.X * 2.0f * roofScale,
                     building.halfExtent.Y * 2.0f * roofScale));

            // A parapet on anything flat-roofed and tall enough to have one. It is two hundred
            // triangles per building that changes the whole silhouette of a skyline: without it
            // every roofline is a hard edge against the sky and the city looks extruded.
            if (building.height > 9.0f)
            {
                const Vec2 parapet(building.halfExtent.X + 0.25f, building.halfExtent.Y + 0.25f);
                roof.AddBox(building.center, building.height, parapet, 0.9f, building.rotation,
                            Vec2(roofScale, roofScale), Vec2(0.0f, 0.0f), true,
                            Vec2(parapet.X * 2.0f * roofScale, parapet.Y * 2.0f * roofScale));
            }

            // A roof mast on the tallest towers, which is what gives a downtown its skyline.
            if (building.kind == BuildingKind::Tower && building.height > 95.0f)
            {
                MeshData& mast = meshFor(building.center, CityMaterial::StreetFurniture);
                mast.AddCylinder(building.center, building.height, 0.55f,
                                 rng.NextFloat(9.0f, 26.0f), 6, Vec2(0, 0), Vec2(1, 1), true);
            }
        }

        // ---- Metro stations and tunnels -----------------------------------------------------------
        for (const MetroLine& line : metro.lines())
            for (std::size_t i = 1; i < line.points.size(); ++i)
            {
                const Vec2 a = line.points[i - 1];
                const Vec2 b = line.points[i];
                const float length = Distance(a, b);
                if (length < 1.0f) continue;
                MeshData& mesh = meshFor((a + b) * 0.5f, CityMaterial::MetroTunnel);
                // A tunnel floor and two walls. Only ever seen by the follow camera, which is
                // exactly when it matters: a citizen riding to work should be riding through
                // something.
                mesh.AddRibbon(a, b, 4.2f, kMetroDepth - 2.6f, 0.0f, length / 6.0f, 0.0f, 1.4f);
                const Vec2 side = Perp(Normalized(b - a)) * 4.2f;
                for (int s = -1; s <= 1; s += 2)
                {
                    const Vec2 shift = side * static_cast<float>(s);
                    mesh.AddQuad(ToWorld(a + shift, kMetroDepth - 2.6f), ToWorld(b + shift, kMetroDepth - 2.6f),
                                 ToWorld(b + shift, kMetroDepth + 3.4f), ToWorld(a + shift, kMetroDepth + 3.4f),
                                 Vector3(-side.X, 0.0f, -side.Y), Vec2(0.0f, 0.0f),
                                 Vec2(length / 6.0f, 1.0f));
                }
            }
        for (const MetroStation& station : metro.stations())
        {
            MeshData& mesh = meshFor(station.position, CityMaterial::MetroTunnel);
            mesh.AddRibbon(station.position - Vec2(0.0f, 30.0f), station.position + Vec2(0.0f, 30.0f),
                           9.0f, kMetroDepth - 2.4f, 0.0f, 10.0f, 0.0f, 3.0f);
        }

        // ---- Upload ---------------------------------------------------------------------------------
        bounds_ = BoundingBox(Vector3(origin_.X, -20.0f, origin_.Y),
                              Vector3(origin_.X + span, 220.0f, origin_.Y + span));
        for (std::size_t c = 0; c < chunks_.size(); ++c)
        {
            GeometryChunk& chunk = chunks_[c];
            const int cx = static_cast<int>(c % side_);
            const int cy = static_cast<int>(c / side_);
            const Vec2 min = origin_ + Vec2(static_cast<float>(cx) * chunkSize_,
                                            static_cast<float>(cy) * chunkSize_);

            // The vertical extent is measured from the geometry rather than assumed, so a chunk of
            // suburbs is not culled against a bound tall enough for downtown.
            float highest = 1.0f;
            for (const MeshData& mesh : staging[c])
                for (const CityVertex& vertex : mesh.vertices)
                    highest = std::max(highest, vertex.Position.Y);
            chunk.bounds = BoundingBox(Vector3(min.X, -18.0f, min.Y),
                                       Vector3(min.X + chunkSize_, highest + 1.0f, min.Y + chunkSize_));

            for (int m = 0; m < kCityMaterialCount; ++m)
            {
                MeshData& data = staging[c][m];
                if (data.empty()) continue;
                auto mesh = std::make_unique<GpuMesh>();
                if (!mesh->Upload(device, data)) continue;
                chunk.triangles += mesh->triangleCount();
                totalTriangles_ += mesh->triangleCount();
                bytes_ += mesh->bytes();
                chunk.meshes[m] = std::move(mesh);
            }
        }

        if (std::getenv("CNA_CITY_GEOMETRY_REPORT") != nullptr)
        {
            int perMaterial[kCityMaterialCount] = {};
            for (const GeometryChunk& chunk : chunks_)
                for (int m = 0; m < kCityMaterialCount; ++m)
                    if (chunk.meshes[m] != nullptr) perMaterial[m] += chunk.meshes[m]->triangleCount();
            static const char* const kNames[kCityMaterialCount] = {
                "asphalt", "pavement", "grass", "glass tower", "concrete office", "brick apartment",
                "render house", "metal shed", "roof", "foliage", "bark", "street furniture",
                "vehicle body", "vehicle glass", "person", "metro tunnel"};
            for (int m = 0; m < kCityMaterialCount; ++m)
                if (perMaterial[m] > 0)
                    std::printf("  geometry: %-18s %8d triangles\n", kNames[m], perMaterial[m]);
        }
    }
}
