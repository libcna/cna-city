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

        /**
         * @brief One quad of an enclosure, wound so that it is visible from @p interior.
         *
         * CNA's rule is a single rule -- a triangle is drawn when its winding normal points away
         * from the camera -- and it is still the easiest thing in this program to get wrong,
         * because the sign flips with which side of the surface you are standing on and a mirrored
         * copy of a correct quad is an incorrect one. The metro was built out of hand-wound quads
         * and four of the six faces of every tunnel were inside out: from a train you looked
         * through the wall at the city, and from the platform you looked through the roof at the
         * sky.
         *
         * So no caller states an order any more. It states a point that is inside the space being
         * enclosed, and both the winding and the shading normal -- which points the opposite way,
         * back into the space -- are derived from that. It is one cross product per quad at
         * generation time and it makes the whole class of bug unrepresentable.
         */
        void AddFacet(MeshData& mesh, Vector3 v0, Vector3 v1, Vector3 v2, Vector3 v3,
                      Vector3 interior, Vec2 uvMin, Vec2 uvMax)
        {
            const Vector3 e1(v1.X - v0.X, v1.Y - v0.Y, v1.Z - v0.Z);
            const Vector3 e2(v2.X - v0.X, v2.Y - v0.Y, v2.Z - v0.Z);
            Vector3 winding(e1.Y * e2.Z - e1.Z * e2.Y, e1.Z * e2.X - e1.X * e2.Z,
                            e1.X * e2.Y - e1.Y * e2.X);
            const Vector3 toInterior(interior.X - v0.X, interior.Y - v0.Y, interior.Z - v0.Z);
            if (winding.X * toInterior.X + winding.Y * toInterior.Y + winding.Z * toInterior.Z > 0.0f)
            {
                std::swap(v0, v3);
                std::swap(v1, v2);
                winding = Vector3(-winding.X, -winding.Y, -winding.Z);
            }
            const float length = std::sqrt(winding.X * winding.X + winding.Y * winding.Y +
                                           winding.Z * winding.Z);
            const float scale = length > 1e-6f ? -1.0f / length : 0.0f;
            mesh.AddQuad(v0, v1, v2, v3,
                         Vector3(winding.X * scale, winding.Y * scale, winding.Z * scale),
                         uvMin, Vec2(uvMax.X - uvMin.X, uvMax.Y - uvMin.Y));
        }

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

        // ---- Pedestrian crossings --------------------------------------------------------------
        //
        // A ladder of bars across each approach to a signalised junction, set back from the
        // stop line. Only signalised junctions get them, which is also true of this city's
        // traffic model: an unsignalised give-way has no phase for a pedestrian to cross on.
        for (std::uint32_t n = 0; n < roads.nodes().size(); ++n)
        {
            const RoadNode& node = roads.nodes()[n];
            if (!node.signalised) continue;
            for (std::uint16_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = roads.incident()[node.firstIncident + k];
                const RoadSegment& segment = roads.segments()[inc.segment];
                if (segment.length < 26.0f) continue;
                const RoadProfile& profile = ProfileOf(segment.roadClass);
                if (profile.sidewalkWidth < 1.0f) continue;

                const Vec2 along = FromHeading(inc.heading);
                const Vec2 across = Perp(along);
                // Beyond the stop line, so a car waiting at a red is behind the crossing rather
                // than parked on it.
                const Vec2 centre = node.position + along * (profile.carriagewayHalfWidth + 2.6f);
                const float halfWidth = profile.carriagewayHalfWidth * 0.97f;

                MeshData& mesh = meshFor(centre, CityMaterial::RoadMarking);
                constexpr int kBars = 7;
                for (int bar = 0; bar < kBars; ++bar)
                {
                    const float t = (static_cast<float>(bar) + 0.5f) / kBars;
                    const Vec2 barCentre = centre + across * ((t * 2.0f - 1.0f) * halfWidth);
                    // A bar runs *along* the direction of travel and is repeated across it, which
                    // is the way round a real zebra is painted and the opposite of what "stripes
                    // across the road" suggests.
                    mesh.AddRibbon(barCentre - along * 1.35f, barCentre + along * 1.35f,
                                   halfWidth / kBars * 0.55f, kRoadY + 0.012f, 0.0f, 1.0f, 0.0f, 1.0f);
                }
            }
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
            // The variant shifts where in the facade tile this building starts. The tile is four
            // bays and four storeys, and which of them have blinds down or a light on is baked
            // into it -- so a quarter-tile offset gives every building in a terrace a different
            // window pattern for nothing at all. Without it a street of thirty houses repeats the
            // same four windows thirty times, which the eye picks up instantly.
            const Vec2 uvOrigin(static_cast<float>(building.variant & 3u) * 0.25f,
                                static_cast<float>((building.variant >> 2) & 1u) * 0.25f);

            // Podium first, where there is one, then the tower stepped back on top of it.
            float base = 0.0f;
            if (building.podiumHeight > 0.0f)
            {
                walls.AddBox(building.center, 0.0f, building.podiumHalfExtent, building.podiumHeight,
                             building.rotation, uvScale, uvOrigin, false, Vec2(1, 1));
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
                         Vec2(uvOrigin.X, uvOrigin.Y + base * uvScale.Y), false, Vec2(1, 1));
            if (building.kind == BuildingKind::House)
            {
                // A ridged roof, running along the building's long axis the way a terrace's does.
                MeshData& tiles = meshFor(building.center, CityMaterial::RoofTile);
                const bool longAxisX = building.halfExtent.X >= building.halfExtent.Y;
                const Vec2 ridgeExtent = longAxisX
                                             ? building.halfExtent
                                             : Vec2(building.halfExtent.Y, building.halfExtent.X);
                const float rotation = longAxisX ? building.rotation : building.rotation + kPi * 0.5f;
                const float pitch = std::min(ridgeExtent.Y, 4.2f) * 0.85f;
                tiles.AddPitchedRoof(building.center, building.height, ridgeExtent, rotation, pitch,
                                     0.35f, Vec2(1.0f / 3.2f, 1.0f / 3.2f));
            }
            else
            {
                // Wound clockwise as seen from above -- see the winding note in MeshBuilder.hpp.
                // The other order leaves every flat roof in the city back-facing, and because a
                // building's walls are culled from the inside, what you see from above instead is
                // the pavement between the buildings. It reads as a roof, which is why this
                // survived a dozen aerial screenshots.
                roof.AddQuad(
                    ToWorld(building.center + Rotate(Vec2(-building.halfExtent.X, -building.halfExtent.Y), building.rotation), building.height),
                    ToWorld(building.center + Rotate(Vec2(building.halfExtent.X, -building.halfExtent.Y), building.rotation), building.height),
                    ToWorld(building.center + Rotate(Vec2(building.halfExtent.X, building.halfExtent.Y), building.rotation), building.height),
                    ToWorld(building.center + Rotate(Vec2(-building.halfExtent.X, building.halfExtent.Y), building.rotation), building.height),
                    Vector3(0.0f, 1.0f, 0.0f), Vec2(0.0f, 0.0f),
                    Vec2(building.halfExtent.X * 2.0f * roofScale,
                         building.halfExtent.Y * 2.0f * roofScale));

                // A parapet on anything flat-roofed and tall enough to have one. It is two hundred
                // triangles per building that changes the whole silhouette of a skyline: without
                // it every roofline is a hard edge against the sky.
                if (building.height > 9.0f)
                {
                    const Vec2 parapet(building.halfExtent.X + 0.25f, building.halfExtent.Y + 0.25f);
                    roof.AddBox(building.center, building.height, parapet, 0.9f, building.rotation,
                                Vec2(roofScale, roofScale), Vec2(0.0f, 0.0f), true,
                                Vec2(parapet.X * 2.0f * roofScale, parapet.Y * 2.0f * roofScale));
                }
            }

            // A roof mast on the tallest towers, which is what gives a downtown its skyline.
            if (building.kind == BuildingKind::Tower && building.height > 95.0f)
            {
                MeshData& mast = meshFor(building.center, CityMaterial::StreetFurniture);
                mast.AddCylinder(building.center, building.height, 0.55f,
                                 rng.NextFloat(9.0f, 26.0f), 6, Vec2(0, 0), Vec2(1, 1), true);
            }
        }

        // ---- The underground ---------------------------------------------------------------------
        //
        // Only the follow camera ever comes down here, which is exactly why it is worth building:
        // the most interesting thirty seconds of a citizen's day is the part where they disappear
        // down a staircase and come up two kilometres away, and until this existed that part of
        // the demonstration was a black screen.
        // The shell is one swept tube per line rather than a box per segment. A box per segment
        // has a joint at every station, a joint is a seam, and a seam between two boxes that meet
        // at an angle is a wedge of open ground -- which from inside a train is the city showing
        // through the tunnel wall. Sweeping a single cross-section along the polyline and mitring
        // it at the bends leaves nothing to butt against, so there is nothing to leak.
        for (const MetroLine& line : metro.lines())
        {
            const std::vector<Vec2>& points = line.points;
            if (points.size() < 2) continue;

            // The mitred cross-section frame at every point on the line: the lateral unit vector,
            // stretched by 1/cos(half the turn) so that the offset walls still meet exactly on the
            // outside of a bend rather than falling short of each other.
            std::vector<Vec2> ribs(points.size());
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const Vec2 in = i > 0 ? Normalized(points[i] - points[i - 1]) : Vec2(0.0f, 0.0f);
                const Vec2 out = i + 1 < points.size() ? Normalized(points[i + 1] - points[i])
                                                       : Vec2(0.0f, 0.0f);
                if (i == 0) { ribs[i] = Perp(out); continue; }
                if (i + 1 == points.size()) { ribs[i] = Perp(in); continue; }
                const Vec2 bisector = Normalized(Perp(in) + Perp(out));
                // Clamped, because a hairpin would otherwise send the mitre off to infinity and
                // take the tunnel wall a kilometre sideways with it.
                ribs[i] = bisector * (1.0f / std::max(0.45f, Dot(bisector, Perp(in))));
            }

            float travelled = 0.0f;
            // A running tunnel between two stations is often four hundred metres long, and the
            // chunk it lands in is three hundred and forty across, so it is cut into pieces short
            // enough that each one stays inside the chunk that owns it. Interpolating the two
            // mitred cross-sections linearly is exact rather than approximate: the wall between
            // them is a straight line, so a point partway along it is the same lerp.
            std::vector<Vec2> cuts;
            std::vector<Vec2> cutRibs;
            for (std::size_t i = 1; i < points.size(); ++i)
            {
                const float span = Distance(points[i - 1], points[i]);
                const int pieces = std::max(1, static_cast<int>(std::ceil(span / 48.0f)));
                for (int k = 0; k < pieces; ++k)
                {
                    const float t = static_cast<float>(k) / static_cast<float>(pieces);
                    cuts.push_back(points[i - 1] + (points[i] - points[i - 1]) * t);
                    cutRibs.push_back(ribs[i - 1] + (ribs[i] - ribs[i - 1]) * t);
                }
            }
            cuts.push_back(points.back());
            cutRibs.push_back(ribs.back());

            for (std::size_t i = 1; i < cuts.size(); ++i)
            {
                const Vec2 from = cuts[i - 1];
                const Vec2 to = cuts[i];
                const float length = Distance(from, to);
                if (length < 0.5f) continue;
                const Vec2 ribA = cutRibs[i - 1];
                const Vec2 ribB = cutRibs[i];
                const Vec2 mid = (from + to) * 0.5f;
                const Vector3 inside = ToWorld(mid, kMetroDepth + (kMetroTunnelRoof + kMetroTrackBed) * 0.5f);

                // A point on the cross-section, at lateral offset `t` from the track centreline.
                const auto at = [&](bool second, float t, float y) {
                    const Vec2 base = second ? to : from;
                    const Vec2 rib = second ? ribB : ribA;
                    return ToWorld(base + rib * t, kMetroDepth + y);
                };
                const float u0 = travelled / 4.0f;
                const float u1 = (travelled + length) / 4.0f;
                travelled += length;

                MeshData& mesh = meshFor(mid, CityMaterial::MetroTunnel);
                MeshData& floor = meshFor(mid, CityMaterial::MetroFloor);
                const float wallV = (kMetroTunnelRoof - kMetroTrackBed) / 4.0f;
                // Floor, roof and the two side walls. Every one of them is handed to AddFacet with
                // a point known to be inside the tube, so none of them can be wound inside out --
                // which four of the six faces of the old shell were, independently.
                AddFacet(floor, at(false, kMetroWallNear, kMetroTrackBed),
                         at(true, kMetroWallNear, kMetroTrackBed),
                         at(true, kMetroWallFar, kMetroTrackBed),
                         at(false, kMetroWallFar, kMetroTrackBed), inside,
                         Vec2(u0, 0.0f), Vec2(u1, (kMetroWallFar - kMetroWallNear) / 4.0f));
                AddFacet(mesh, at(false, kMetroWallNear, kMetroTunnelRoof),
                         at(true, kMetroWallNear, kMetroTunnelRoof),
                         at(true, kMetroWallFar, kMetroTunnelRoof),
                         at(false, kMetroWallFar, kMetroTunnelRoof), inside,
                         Vec2(u0, 0.0f), Vec2(u1, (kMetroWallFar - kMetroWallNear) / 4.0f));
                for (const float t : {kMetroWallNear, kMetroWallFar})
                    AddFacet(mesh, at(false, t, kMetroTrackBed), at(true, t, kMetroTrackBed),
                             at(true, t, kMetroTunnelRoof), at(false, t, kMetroTunnelRoof), inside,
                             Vec2(u0, 0.0f), Vec2(u1, wallV));

                // A raised walkway down the side away from the platform -- the evacuation path a
                // real running tunnel has, and the thing that stops the near side of the tube
                // reading as a flat wall meeting a flat floor.
                AddFacet(floor, at(false, kMetroWallNear, kMetroWalkway),
                         at(true, kMetroWallNear, kMetroWalkway),
                         at(true, kMetroPlatformEdge * -0.4f, kMetroWalkway),
                         at(false, kMetroPlatformEdge * -0.4f, kMetroWalkway), inside,
                         Vec2(u0, 0.0f), Vec2(u1, 0.6f));
                AddFacet(floor, at(false, kMetroPlatformEdge * -0.4f, kMetroTrackBed),
                         at(true, kMetroPlatformEdge * -0.4f, kMetroTrackBed),
                         at(true, kMetroPlatformEdge * -0.4f, kMetroWalkway),
                         at(false, kMetroPlatformEdge * -0.4f, kMetroWalkway), inside,
                         Vec2(u0, 0.0f), Vec2(u1, 0.15f));

                // The two running rails, and a continuous lit strip under the roof. The strip is a
                // strip rather than discrete fittings because from the only place anyone ever sees
                // it -- directly underneath, at 20 m/s -- tube lighting is what it reads as, and
                // fittings would cost a draw call per station.
                MeshData& rails = meshFor(mid, CityMaterial::MetroRail);
                for (const float t : {-0.72f, 0.72f})
                    AddFacet(rails, at(false, t - 0.075f, kMetroRailTop),
                             at(true, t - 0.075f, kMetroRailTop),
                             at(true, t + 0.075f, kMetroRailTop),
                             at(false, t + 0.075f, kMetroRailTop), inside,
                             Vec2(travelled - length, 0.0f), Vec2(travelled, 1.0f));

                MeshData& strip = meshFor(mid, CityMaterial::TunnelLight);
                for (const float t : {-0.9f, kMetroPlatformEdge + 2.2f})
                    AddFacet(strip, at(false, t - 0.30f, kMetroTunnelRoof - 0.05f),
                             at(true, t - 0.30f, kMetroTunnelRoof - 0.05f),
                             at(true, t + 0.30f, kMetroTunnelRoof - 0.05f),
                             at(false, t + 0.30f, kMetroTunnelRoof - 0.05f), inside,
                             Vec2(u0 * 2.0f, 0.0f), Vec2(u1 * 2.0f, 1.0f));
            }

            // Blank walls closing each end of the line, so a passenger on the last train of the
            // day does not look through the buffers at the sky.
            for (const std::size_t i : {std::size_t(0), points.size() - 1})
            {
                if (line.loop) break;
                const Vec2 rib = ribs[i];
                const Vec2 p = points[i];
                const Vector3 inside = ToWorld(
                    i == 0 ? points[1] : points[points.size() - 2],
                    kMetroDepth + (kMetroTunnelRoof + kMetroTrackBed) * 0.5f);
                MeshData& mesh = meshFor(p, CityMaterial::MetroTunnel);
                AddFacet(mesh, ToWorld(p + rib * kMetroWallNear, kMetroDepth + kMetroTrackBed),
                         ToWorld(p + rib * kMetroWallFar, kMetroDepth + kMetroTrackBed),
                         ToWorld(p + rib * kMetroWallFar, kMetroDepth + kMetroTunnelRoof),
                         ToWorld(p + rib * kMetroWallNear, kMetroDepth + kMetroTunnelRoof), inside,
                         Vec2(0.0f, 0.0f), Vec2(2.4f, 1.2f));
            }
        }

        // A station is a slab inside the tube, not a box around it. The tunnel is already wide
        // enough on the platform side to stand a crowd on, so a station adds a platform, its edge,
        // and nothing that has to join up with anything.
        for (const MetroStation& station : metro.stations())
        {
            const Vec2 along = station.axis;
            const Vec2 side = Perp(along);
            const Vec2 a = station.position - along * kMetroPlatformHalfLength;
            const Vec2 b = station.position + along * kMetroPlatformHalfLength;
            const Vector3 inside = ToWorld(station.position,
                                           kMetroDepth + (kMetroTunnelRoof + kMetroTrackBed) * 0.5f);
            const float back = kMetroWallFar - 0.05f;

            MeshData& mesh = meshFor(station.position, CityMaterial::MetroTunnel);
            // The walking surface is the city's own pavement, which is both correct and free: it
            // is already a paving texture, and it puts the platform a clear step lighter than the
            // walls and two steps lighter than the track bed.
            MeshData& slab = meshFor(station.position, CityMaterial::Pavement);
            const auto corner = [&](const Vec2& base, float t, float y) {
                return ToWorld(base + side * t, kMetroDepth + y);
            };
            // The platform top, and the edge face a passenger sees from the train.
            AddFacet(slab, corner(a, kMetroPlatformEdge, kMetroPlatform),
                     corner(b, kMetroPlatformEdge, kMetroPlatform),
                     corner(b, back, kMetroPlatform), corner(a, back, kMetroPlatform),
                     Vector3(inside.X, inside.Y - 6.0f, inside.Z),
                     Vec2(0.0f, 0.0f), Vec2(kMetroPlatformHalfLength * 0.5f, 1.6f));
            AddFacet(mesh, corner(a, kMetroPlatformEdge, kMetroTrackBed),
                     corner(b, kMetroPlatformEdge, kMetroTrackBed),
                     corner(b, kMetroPlatformEdge, kMetroPlatform),
                     corner(a, kMetroPlatformEdge, kMetroPlatform), inside,
                     Vec2(0.0f, 0.0f), Vec2(kMetroPlatformHalfLength * 0.5f, 0.4f));
            // Both ends of the slab, so it reads as a platform that stops rather than a floor that
            // is simply missing beyond it.
            for (const Vec2& endPoint : {a, b})
                AddFacet(mesh, corner(endPoint, kMetroPlatformEdge, kMetroTrackBed),
                         corner(endPoint, back, kMetroTrackBed),
                         corner(endPoint, back, kMetroPlatform),
                         corner(endPoint, kMetroPlatformEdge, kMetroPlatform), inside,
                         Vec2(0.0f, 0.0f), Vec2(1.2f, 0.4f));

            // The platform edge's warning line, which is the one thing that says "this is a
            // platform" from the far end of a train.
            MeshData& marking = meshFor(station.position, CityMaterial::RoadMarking);
            AddFacet(marking, corner(a, kMetroPlatformEdge + 0.10f, kMetroPlatform + 0.012f),
                     corner(b, kMetroPlatformEdge + 0.10f, kMetroPlatform + 0.012f),
                     corner(b, kMetroPlatformEdge + 0.55f, kMetroPlatform + 0.012f),
                     corner(a, kMetroPlatformEdge + 0.55f, kMetroPlatform + 0.012f),
                     Vector3(inside.X, inside.Y - 6.0f, inside.Z),
                     Vec2(0.0f, 0.0f), Vec2(kMetroPlatformHalfLength * 0.5f, 1.0f));
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

            // Measured from the geometry rather than assumed. The vertical extent matters so that
            // a chunk of suburbs is not culled against a bound tall enough for downtown -- but the
            // *horizontal* extent matters more, and assuming it was the chunk's own square was a
            // real bug rather than a conservative approximation. A piece of geometry lands in the
            // chunk that contains its centre and is free to stick out of it: a metro tunnel
            // between two stations is four hundred metres of one segment in a three-hundred-metre
            // chunk. When the chunk holding the middle of that segment left the frustum, the
            // tunnel around the camera disappeared and the city showed through the hole.
            float highest = 1.0f;
            Vec2 low(min.X, min.Y);
            Vec2 high(min.X + chunkSize_, min.Y + chunkSize_);
            for (const MeshData& mesh : staging[c])
                for (const CityVertex& vertex : mesh.vertices)
                {
                    highest = std::max(highest, vertex.Position.Y);
                    low = Vec2(std::min(low.X, vertex.Position.X), std::min(low.Y, vertex.Position.Z));
                    high = Vec2(std::max(high.X, vertex.Position.X), std::max(high.Y, vertex.Position.Z));
                }
            chunk.bounds = BoundingBox(Vector3(low.X, -18.0f, low.Y),
                                       Vector3(high.X, highest + 1.0f, high.Y));

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
                "render house", "metal shed", "flat roof", "roof tile", "road marking", "foliage",
                "bark", "street furniture", "vehicle body", "vehicle glass", "person",
                "metro tunnel", "metro rail", "tunnel light"};
            for (int m = 0; m < kCityMaterialCount; ++m)
                if (perMaterial[m] > 0)
                    std::printf("  geometry: %-18s %8d triangles\n", kNames[m], perMaterial[m]);
        }
    }
}
