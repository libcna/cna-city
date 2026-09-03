// SPDX-License-Identifier: MIT
#include "InstanceRenderer.hpp"

#include <cmath>

#include "Microsoft/Xna/Framework/Graphics/BlendState.hpp"
#include "Microsoft/Xna/Framework/Graphics/DepthStencilState.hpp"
#include "Microsoft/Xna/Framework/Graphics/RasterizerState.hpp"
#include "Rng.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        /// Clothing. Muted rather than saturated, because a crowd of primary colours reads as a
        /// bag of sweets from above; these are roughly what a street of people actually looks like.
        const Vector3 kClothing[InstanceRenderer::kColorBuckets] = {
            {0.19f, 0.21f, 0.26f},  // charcoal
            {0.36f, 0.30f, 0.26f},  // brown
            {0.14f, 0.24f, 0.38f},  // navy
            {0.62f, 0.60f, 0.55f},  // stone
            {0.44f, 0.15f, 0.16f},  // maroon
            {0.20f, 0.34f, 0.24f},  // olive
            {0.78f, 0.76f, 0.72f},  // cream
            {0.30f, 0.28f, 0.36f},  // slate violet
        };

        /// Car paint. Real fleets are overwhelmingly white, black, grey and silver, and only the
        /// last two buckets are colours anybody would notice -- which is exactly what makes a red
        /// car in traffic read as a red car.
        const Vector3 kPaint[InstanceRenderer::kColorBuckets] = {
            {0.86f, 0.87f, 0.88f},  // white
            {0.09f, 0.09f, 0.10f},  // black
            {0.44f, 0.45f, 0.47f},  // grey
            {0.70f, 0.71f, 0.74f},  // silver
            {0.16f, 0.20f, 0.34f},  // dark blue
            {0.62f, 0.60f, 0.56f},  // beige
            {0.52f, 0.09f, 0.10f},  // red
            {0.13f, 0.30f, 0.22f},  // green
        };

        /** @brief A lamp column with an outreach arm and a head, built about the origin. */
        MeshData BuildLampColumn()
        {
            MeshData mesh;
            mesh.AddCylinder(Vec2(0.0f, 0.0f), 0.0f, 0.09f, 7.4f, 6, Vec2(0, 0), Vec2(1, 2), false);
            // The outreach: a short arm leaning over the carriageway, which is what makes a row of
            // lamps read as street lighting rather than as fence posts.
            for (int i = 0; i < 5; ++i)
            {
                const float t = static_cast<float>(i) / 4.0f;
                const float x = t * 1.5f;
                const float y = 7.4f + std::sin(t * kPi * 0.5f) * 0.55f;
                mesh.AddCylinder(Vec2(x, 0.0f), y - 0.12f, 0.07f, 0.24f, 5, Vec2(0, 0), Vec2(1, 1), false);
            }
            return mesh;
        }

        MeshData BuildLampHead()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(1.62f, 0.0f), 7.62f, Vec2(0.34f, 0.16f), 0.18f, 0.0f, Vec2(1, 1),
                        Vec2(0, 0), true, Vec2(1, 1));
            return mesh;
        }

        MeshData BuildSignalHead()
        {
            MeshData mesh;
            mesh.AddCylinder(Vec2(0.0f, 0.0f), 0.0f, 0.07f, 3.1f, 6, Vec2(0, 0), Vec2(1, 2), false);
            mesh.AddBox(Vec2(0.0f, 0.0f), 2.4f, Vec2(0.16f, 0.13f), 0.86f, 0.0f, Vec2(1, 1),
                        Vec2(0, 0), true, Vec2(1, 1));
            return mesh;
        }

        /// The lens: a small box on the front of the signal housing, proud of it by a centimetre.
        /// It is drawn emissive, because a traffic light is legible at night precisely because it
        /// emits rather than reflects.
        MeshData BuildSignalLens()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(0.0f, -0.135f), 2.62f, Vec2(0.105f, 0.02f), 0.19f, 0.0f, Vec2(1, 1),
                        Vec2(0, 0), true, Vec2(1, 1), true);
            return mesh;
        }

        MeshData BuildTreeTrunk()
        {
            MeshData mesh;
            mesh.AddCylinder(Vec2(0.0f, 0.0f), 0.0f, 0.19f, 2.6f, 6, Vec2(0, 0), Vec2(1, 2), false);
            return mesh;
        }

        MeshData BuildTreeCanopy()
        {
            MeshData mesh;
            // Three overlapping spheres rather than one: a single ball is unmistakably a ball, and
            // three of different sizes read as foliage from any distance a tree is visible at.
            mesh.AddSphere(Vector3(0.0f, 4.0f, 0.0f), 2.05f, 8, 6, Vec2(0, 0), Vec2(1, 1));
            mesh.AddSphere(Vector3(1.05f, 3.3f, 0.5f), 1.35f, 7, 5, Vec2(0, 0), Vec2(1, 1));
            mesh.AddSphere(Vector3(-0.85f, 3.5f, -0.7f), 1.5f, 7, 5, Vec2(0, 0), Vec2(1, 1));
            return mesh;
        }

        MeshData BuildConiferCanopy()
        {
            MeshData mesh;
            mesh.AddCone(Vec2(0.0f, 0.0f), 1.4f, 1.9f, 3.4f, 8, Vec2(0, 0), Vec2(1, 1));
            mesh.AddCone(Vec2(0.0f, 0.0f), 3.4f, 1.35f, 3.0f, 8, Vec2(0, 0), Vec2(1, 1));
            mesh.AddCone(Vec2(0.0f, 0.0f), 5.2f, 0.85f, 2.4f, 8, Vec2(0, 0), Vec2(1, 1));
            return mesh;
        }

        MeshData BuildBench()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(0.0f, 0.0f), 0.42f, Vec2(0.85f, 0.24f), 0.08f, 0.0f, Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1));
            mesh.AddBox(Vec2(0.0f, -0.22f), 0.5f, Vec2(0.85f, 0.05f), 0.42f, 0.0f, Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1));
            for (int s = -1; s <= 1; s += 2)
                mesh.AddBox(Vec2(0.72f * static_cast<float>(s), 0.0f), 0.0f, Vec2(0.06f, 0.22f), 0.42f,
                            0.0f, Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
            return mesh;
        }

        MeshData BuildBin()
        {
            MeshData mesh;
            mesh.AddCylinder(Vec2(0.0f, 0.0f), 0.0f, 0.26f, 0.92f, 8, Vec2(0, 0), Vec2(1, 1), true);
            return mesh;
        }

        MeshData BuildBusShelter()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(0.0f, 0.0f), 2.35f, Vec2(2.0f, 0.75f), 0.1f, 0.0f, Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1));
            for (int s = -1; s <= 1; s += 2)
                mesh.AddBox(Vec2(1.9f * static_cast<float>(s), 0.0f), 0.0f, Vec2(0.06f, 0.7f), 2.35f,
                            0.0f, Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
            mesh.AddBox(Vec2(0.0f, 0.68f), 0.0f, Vec2(1.9f, 0.05f), 2.3f, 0.0f, Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
            return mesh;
        }

        MeshData BuildMetroEntrance()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(0.0f, 0.0f), 0.0f, Vec2(1.5f, 1.1f), 0.35f, 0.0f, Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1));
            for (int s = -1; s <= 1; s += 2)
                mesh.AddBox(Vec2(1.4f * static_cast<float>(s), 0.0f), 0.35f, Vec2(0.08f, 1.05f), 2.4f,
                            0.0f, Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
            mesh.AddBox(Vec2(0.0f, 0.0f), 2.75f, Vec2(1.5f, 1.1f), 0.12f, 0.0f, Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1));
            return mesh;
        }

        /** @brief A vehicle's painted shell: body, and a greenhouse on anything that is not a box. */
        MeshData BuildVehicleBody(VehicleKind kind)
        {
            const VehicleProfile& profile = ProfileOf(kind);
            MeshData mesh;
            const float halfLength = profile.length * 0.5f;
            const float halfWidth = profile.width * 0.5f;
            const bool boxy = kind == VehicleKind::Bus || kind == VehicleKind::Truck ||
                              kind == VehicleKind::Van;
            // A real car's sills come down to about a quarter of a metre and its wheels fill the
            // arches. At 0.34 with narrow wheels the body stood clear of them and the whole thing
            // read as a slab on four stilts -- which is exactly what a kerb full of parked cars
            // looked like from the pavement.
            const float sill = boxy ? 0.30f : 0.24f;
            const float bodyHeight = boxy ? profile.height - sill - 0.05f
                                          : (profile.height - sill) * 0.52f;
            mesh.AddBox(Vec2(0.0f, 0.0f), sill, Vec2(halfLength, halfWidth), bodyHeight, 0.0f,
                        Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1), true);
            if (!boxy)
            {
                // The greenhouse: set back from the nose, inset from the sides, and roughly as
                // tall as the body under it. It is the one shape that separates a car from a
                // brick at fifty metres, and the first version made it a third of the height,
                // which is why the traffic read as a row of paving slabs.
                const float roofHeight = profile.height - sill - bodyHeight;
                mesh.AddBox(Vec2(-halfLength * 0.06f, 0.0f), sill + bodyHeight,
                            Vec2(halfLength * 0.60f, halfWidth * 0.90f), roofHeight, 0.0f,
                            Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1));
            }
            return mesh;
        }

        /**
         * @brief A vehicle's dark parts: the glazing and the wheels.
         *
         * They share the body's instance matrices and are drawn once per vehicle kind rather than
         * once per colour, because a wheel is black on every car ever made. Putting the wheels
         * here rather than in the painted shell is not tidiness -- in the shell they came out
         * body-coloured, which is a look no car has had since 1958.
         */
        MeshData BuildVehicleGlass(VehicleKind kind)
        {
            const VehicleProfile& profile = ProfileOf(kind);
            MeshData mesh;
            const bool boxy = kind == VehicleKind::Bus || kind == VehicleKind::Truck ||
                              kind == VehicleKind::Van;
            const float halfLength = profile.length * 0.5f;
            const float halfWidth = profile.width * 0.5f;
            const float sill = boxy ? 0.30f : 0.24f;
            const float bodyHeight = boxy ? profile.height - sill - 0.05f
                                          : (profile.height - sill) * 0.52f;

            // A band of glazing, a centimetre proud of the shell so it wins the depth test
            // cleanly. It has to be *inside* the painted volume in every other dimension: the
            // first version made the car's glass wider and taller than the greenhouse it sits in,
            // which swallowed the roof and left every car looking like a white slab with a black
            // lid.
            if (boxy)
                mesh.AddBox(Vec2(0.0f, 0.0f), sill + bodyHeight * 0.46f,
                            Vec2(halfLength * 0.94f, halfWidth * 1.012f), bodyHeight * 0.34f, 0.0f,
                            Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
            else
            {
                const float roofHeight = profile.height - sill - bodyHeight;
                mesh.AddBox(Vec2(-halfLength * 0.06f, 0.0f), sill + bodyHeight + roofHeight * 0.14f,
                            Vec2(halfLength * 0.555f, halfWidth * 0.912f), roofHeight * 0.60f,
                            0.0f, Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
            }

            // Wheels: tucked under the sill and just inside the flanks, so they read as wheels in
            // arches rather than as blocks bolted to the outside.
            // Wheels flush with the flanks and tall enough to reach up into the body, so the arch
            // is a shadow under the wing rather than a gap under a floating slab.
            const float wheelRadius = boxy ? 0.44f : 0.32f;
            const float wheelWidth = boxy ? 0.19f : 0.15f;
            for (int fx = -1; fx <= 1; fx += 2)
                for (int fz = -1; fz <= 1; fz += 2)
                    mesh.AddBox(Vec2(static_cast<float>(fx) * halfLength * 0.62f,
                                     static_cast<float>(fz) * (halfWidth - wheelWidth * 0.85f)),
                                0.0f, Vec2(wheelRadius, wheelWidth), sill + wheelRadius * 0.7f,
                                0.0f, Vec2(1, 1), Vec2(0, 0), true, Vec2(1, 1), true);
            return mesh;
        }

        /**
         * @brief A walking figure with the legs and arms swung to @p phase of the cycle.
         *
         * Four baked phases, drawn as four separate instanced batches, rather than one mesh
         * animated per instance. CNA's instance stream carries a world matrix and nothing else --
         * there is no attribute slot left for a bone index, let alone a skeleton -- so the choice
         * is between baked poses and no animation at all. Four is enough for a walk to read at the
         * distance a person is more than a few pixels tall.
         */
        MeshData BuildPerson(float phase, PersonLod lod)
        {
            MeshData mesh;
            if (lod == PersonLod::Far)
            {
                mesh.AddBox(Vec2(0.0f, 0.0f), 0.0f, Vec2(0.20f, 0.14f), 1.72f, 0.0f, Vec2(1, 1),
                            Vec2(0, 0), true, Vec2(1, 1));
                return mesh;
            }
            if (lod == PersonLod::Mid)
            {
                mesh.AddBox(Vec2(0.0f, 0.0f), 0.0f, Vec2(0.17f, 0.13f), 1.42f, 0.0f, Vec2(1, 1),
                            Vec2(0, 0), true, Vec2(1, 1));
                mesh.AddBox(Vec2(0.0f, 0.0f), 1.42f, Vec2(0.11f, 0.10f), 0.24f, 0.0f, Vec2(1, 1),
                            Vec2(0, 0), true, Vec2(1, 1));
                return mesh;
            }

            const float swing = std::sin(phase * 2.0f * kPi);
            const float lift = std::fabs(std::cos(phase * 2.0f * kPi)) * 0.06f;

            // Torso and head.
            mesh.AddBox(Vec2(0.0f, 0.0f), 0.82f, Vec2(0.16f, 0.115f), 0.56f, 0.0f, Vec2(1, 1),
                        Vec2(0, 0), true, Vec2(1, 1));
            mesh.AddBox(Vec2(0.0f, 0.0f), 1.40f, Vec2(0.095f, 0.09f), 0.23f, 0.0f, Vec2(1, 1),
                        Vec2(0, 0), true, Vec2(1, 1));

            // Legs: the swing is applied as a longitudinal offset at the foot, which is a shear
            // rather than a rotation and is indistinguishable at this size.
            for (int side = -1; side <= 1; side += 2)
            {
                const float direction = static_cast<float>(side) * swing;
                mesh.AddBox(Vec2(direction * 0.20f, static_cast<float>(side) * 0.075f),
                            lift * (side > 0 ? 1.0f : 0.0f), Vec2(0.075f, 0.065f), 0.82f - lift,
                            0.0f, Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
                // Arms counter-swing, which is the half of a walk cycle people notice when it is
                // missing without being able to say why.
                mesh.AddBox(Vec2(-direction * 0.17f, static_cast<float>(side) * 0.20f), 0.86f,
                            Vec2(0.055f, 0.052f), 0.50f, 0.0f, Vec2(1, 1), Vec2(0, 0), false, Vec2(1, 1));
            }
            return mesh;
        }

        /// A falling raindrop, as a short vertical streak. A drop drawn as a point is invisible;
        /// what the eye actually reads as rain is the motion blur of one, and a 40 cm streak is
        /// that blur made geometry.
        MeshData BuildRainStreak()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(0.0f, 0.0f), 0.0f, Vec2(0.011f, 0.011f), 0.42f, 0.0f, Vec2(1, 1),
                        Vec2(0, 0), true, Vec2(1, 1));
            return mesh;
        }

        MeshData BuildSnowFlake()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(0.0f, 0.0f), 0.0f, Vec2(0.035f, 0.035f), 0.07f, 0.0f, Vec2(1, 1),
                        Vec2(0, 0), true, Vec2(1, 1));
            return mesh;
        }

        MeshData BuildTrainCar()
        {
            MeshData mesh;
            mesh.AddBox(Vec2(0.0f, 0.0f), 0.6f, Vec2(9.0f, 1.35f), 3.0f, 0.0f, Vec2(0.2f, 0.2f),
                        Vec2(0, 0), true, Vec2(1, 1));
            return mesh;
        }
    }

    void InstanceRenderer::Release()
    {
        batches_.clear();
        for (auto& list : propBatches_) list.clear();
    }

    std::size_t InstanceRenderer::AddBatch(GraphicsDevice& device, MeshData&& data,
                                           CityMaterial material, const Vector3& tint, bool emissive)
    {
        InstanceBatch batch;
        batch.mesh = std::make_unique<GpuMesh>();
        if (!batch.mesh->Upload(device, data)) return static_cast<std::size_t>(-1);
        batch.renderer = std::make_unique<CNA::Graphics::InstancedRendererEXT>(device,
                                                                               batch.mesh->part());
        // The per-instance fallback draws one call per instance. For twenty thousand lamps that is
        // not a fallback, it is a different program -- so it stays off and the renderer reports
        // instead. See InstancedRendererEXT::setFallbackEnabled.
        batch.renderer->setFallbackEnabled(false);
        batch.material = material;
        batch.tint = tint;
        batch.emissiveAtNight = emissive;
        batch.emissiveFloor = 0.0f;
        instancingSupported_ = batch.renderer->isInstancingSupported();
        batches_.push_back(std::move(batch));
        return batches_.size() - 1;
    }

    void InstanceRenderer::Build(GraphicsDevice& device, const MaterialLibrary& materials,
                                 std::uint64_t seed)
    {
        Release();
        (void)materials;
        Rng rng(seed, 0x494e'5354'414e'4345ULL);
        (void)rng;

        const Vector3 white(1.0f, 1.0f, 1.0f);

        propBatches_[static_cast<int>(PropKind::StreetLamp)] = {
            AddBatch(device, BuildLampColumn(), CityMaterial::StreetFurniture, Vector3(0.30f, 0.31f, 0.33f), false),
            AddBatch(device, BuildLampHead(), CityMaterial::StreetFurniture, Vector3(1.0f, 0.86f, 0.62f), true)};
        propBatches_[static_cast<int>(PropKind::TrafficSignal)] = {
            AddBatch(device, BuildSignalHead(), CityMaterial::StreetFurniture, Vector3(0.20f, 0.21f, 0.22f), false)};
        propBatches_[static_cast<int>(PropKind::TreeRound)] = {
            AddBatch(device, BuildTreeTrunk(), CityMaterial::Bark, white, false),
            AddBatch(device, BuildTreeCanopy(), CityMaterial::Foliage, white, false)};
        propBatches_[static_cast<int>(PropKind::TreeConifer)] = {
            AddBatch(device, BuildTreeTrunk(), CityMaterial::Bark, white, false),
            AddBatch(device, BuildConiferCanopy(), CityMaterial::Foliage, Vector3(0.72f, 0.85f, 0.74f), false)};
        propBatches_[static_cast<int>(PropKind::Bench)] = {
            AddBatch(device, BuildBench(), CityMaterial::Bark, Vector3(0.62f, 0.48f, 0.34f), false)};
        propBatches_[static_cast<int>(PropKind::Bin)] = {
            AddBatch(device, BuildBin(), CityMaterial::StreetFurniture, Vector3(0.26f, 0.30f, 0.28f), false)};
        propBatches_[static_cast<int>(PropKind::BusShelter)] = {
            AddBatch(device, BuildBusShelter(), CityMaterial::StreetFurniture, Vector3(0.34f, 0.36f, 0.38f), false)};
        propBatches_[static_cast<int>(PropKind::MetroEntrance)] = {
            AddBatch(device, BuildMetroEntrance(), CityMaterial::StreetFurniture, Vector3(0.42f, 0.30f, 0.26f), false)};

        for (int kind = 0; kind < kVehicleKindCount; ++kind)
        {
            for (int bucket = 0; bucket < kColorBuckets; ++bucket)
                vehicleBody_[kind][bucket] =
                    AddBatch(device, BuildVehicleBody(static_cast<VehicleKind>(kind)),
                             CityMaterial::VehicleBody, kPaint[bucket], false);
            vehicleGlass_[kind] = AddBatch(device, BuildVehicleGlass(static_cast<VehicleKind>(kind)),
                                           CityMaterial::VehicleGlass, white, false);
        }

        for (int lod = 0; lod < 3; ++lod)
            for (int phase = 0; phase < kWalkPhases; ++phase)
                for (int bucket = 0; bucket < kColorBuckets; ++bucket)
                {
                    // Only the near level has distinct phases; the other two share one pose, so
                    // the same batch is reused and the frame does not pay for empty uploads.
                    if (lod != 0 && phase > 0)
                    {
                        personBatch_[lod][phase][bucket] = personBatch_[lod][0][bucket];
                        continue;
                    }
                    personBatch_[lod][phase][bucket] = AddBatch(
                        device,
                        BuildPerson(static_cast<float>(phase) / static_cast<float>(kWalkPhases),
                                    static_cast<PersonLod>(lod)),
                        CityMaterial::Person, kClothing[bucket], false);
                }

        // One batch per aspect rather than a per-instance colour, for the same reason the crowd is
        // bucketed: there is no attribute slot left for a tint. Three batches is a cheap price for
        // the most legible piece of machinery a city has.
        signalLens_[0] = AddBatch(device, BuildSignalLens(), CityMaterial::StreetFurniture,
                                  Vector3(1.0f, 0.10f, 0.08f), true);
        signalLens_[1] = AddBatch(device, BuildSignalLens(), CityMaterial::StreetFurniture,
                                  Vector3(1.0f, 0.62f, 0.06f), true);
        signalLens_[2] = AddBatch(device, BuildSignalLens(), CityMaterial::StreetFurniture,
                                  Vector3(0.15f, 1.0f, 0.30f), true);

        for (std::size_t index : signalLens_)
            if (index < batches_.size()) batches_[index].emissiveFloor = 1.35f;

        trainBatch_ = AddBatch(device, BuildTrainCar(), CityMaterial::StreetFurniture,
                               Vector3(0.72f, 0.74f, 0.78f), false);
        // Precipitation is emissive so it stays visible against a dark wet street at night, which
        // is exactly the frame it matters most in.
        rainBatch_ = AddBatch(device, BuildRainStreak(), CityMaterial::VehicleGlass,
                              Vector3(0.62f, 0.70f, 0.86f), true);
        snowBatch_ = AddBatch(device, BuildSnowFlake(), CityMaterial::Person,
                              Vector3(0.94f, 0.96f, 1.0f), true);

        if (!instancingSupported_)
            diagnostic_ = "hardware instancing unavailable on this renderer -- props, vehicles and "
                          "people are not drawn";
    }

    void InstanceRenderer::BeginFrame()
    {
        for (InstanceBatch& batch : batches_) batch.instances.clear();
    }

    void InstanceRenderer::AddProp(PropKind kind, const Matrix& world)
    {
        for (std::size_t index : propBatches_[static_cast<int>(kind)])
            if (index < batches_.size()) batches_[index].instances.push_back(world);
    }

    void InstanceRenderer::AddVehicle(VehicleKind kind, std::uint8_t colorBucket, const Matrix& world)
    {
        const int k = static_cast<int>(kind);
        const std::size_t body = vehicleBody_[k][colorBucket % kColorBuckets];
        if (body < batches_.size()) batches_[body].instances.push_back(world);
        if (vehicleGlass_[k] < batches_.size()) batches_[vehicleGlass_[k]].instances.push_back(world);
    }

    void InstanceRenderer::AddPerson(PersonLod lod, std::uint8_t phase, std::uint8_t colorBucket,
                                     const Matrix& world)
    {
        const std::size_t index = personBatch_[static_cast<int>(lod)][phase % kWalkPhases]
                                              [colorBucket % kColorBuckets];
        if (index < batches_.size()) batches_[index].instances.push_back(world);
    }

    void InstanceRenderer::AddTrain(const Matrix& world)
    {
        if (trainBatch_ < batches_.size()) batches_[trainBatch_].instances.push_back(world);
    }

    void InstanceRenderer::AddSignalLens(std::uint8_t colour, const Matrix& world)
    {
        const std::size_t index = signalLens_[colour % 3];
        if (index < batches_.size()) batches_[index].instances.push_back(world);
    }

    void InstanceRenderer::AddPrecipitation(bool snow, const Matrix& world)
    {
        const std::size_t index = snow ? snowBatch_ : rainBatch_;
        if (index < batches_.size()) batches_[index].instances.push_back(world);
    }

    std::size_t InstanceRenderer::instanceCount() const
    {
        std::size_t total = 0;
        for (const InstanceBatch& batch : batches_) total += batch.instances.size();
        return total;
    }

    Vector3 InstanceRenderer::ClothingColor(std::uint8_t bucket) const
    {
        return kClothing[bucket % kColorBuckets];
    }

    int InstanceRenderer::Flush(GraphicsDevice& device, PbrEffect& effect,
                                const MaterialLibrary& materials, const Matrix& view,
                                const Matrix& projection, float nightLevel, float wetness,
                                float snow)
    {
        if (!instancingSupported_) return 0;
        int draws = 0;
        effect.setWorldProperty(Matrix::getIdentityProperty());
        effect.setViewProperty(view);
        effect.setProjectionProperty(projection);

        for (InstanceBatch& batch : batches_)
        {
            if (batch.instances.empty() || batch.renderer == nullptr) continue;
            materials.Apply(effect, batch.material, nightLevel, wetness, snow);
            effect.setDiffuseColorProperty(batch.tint);
            if (batch.emissiveAtNight)
            {
                // A lamp head is a light source in its own right, and the emissive term is what
                // makes it visible as one rather than as a dark box under a pool of light. A
                // signal lens keeps a floor under it in daylight, because a traffic light that
                // only glows after dark is not a traffic light.
                const float glow = std::max(batch.emissiveFloor, Saturate(nightLevel) * 1.7f);
                effect.setEmissiveFactorProperty(
                    Vector3(glow * batch.tint.X, glow * batch.tint.Y, glow * batch.tint.Z));
            }
            device.setRasterizerStateProperty(materials.Get(batch.material).doubleSided
                                                  ? RasterizerState::CullNone
                                                  : RasterizerState::CullCounterClockwise);
            batch.renderer->setInstances(batch.instances);
            batch.renderer->draw(effect);
            ++draws;
        }
        device.setRasterizerStateProperty(RasterizerState::CullCounterClockwise);
        return draws;
    }
}
