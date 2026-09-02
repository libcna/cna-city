// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Agents.hpp"
#include "City.hpp"
#include "Materials.hpp"
#include "MeshBuilder.hpp"
#include "Traffic.hpp"
#include "CNA/Graphics/InstancedRendererEXT.hpp"
#include "Microsoft/Xna/Framework/Matrix.hpp"

namespace CnaCity
{
    /** @brief Levels of detail for a person. The counts in a frame differ by two orders of magnitude. */
    enum class PersonLod : std::uint8_t
    {
        Near = 0,   ///< A figure with legs, arms and a head, in one of several walk phases.
        Mid,        ///< Torso and head.
        Far         ///< One box. At this distance a person is three pixels tall.
    };

    /**
     * @brief One instanced draw: a prototype mesh, a material, a tint, and the matrices this frame.
     *
     * The tint is per batch rather than per instance, and that is forced rather than chosen. CNA's
     * instance stream occupies vertex attribute locations 12 to 15, and the stock lit shaders use
     * 0 to 11 for the mesh -- XNA's own ceiling of sixteen, and GL ES 3's guaranteed minimum. There
     * is no seventeenth slot for a colour. So the city's people and cars are bucketed into a
     * couple of dozen colours and drawn once per bucket, which costs a few dozen draw calls and
     * gives a crowd that is not uniformly grey.
     */
    struct InstanceBatch
    {
        std::unique_ptr<GpuMesh> mesh;
        std::unique_ptr<CNA::Graphics::InstancedRendererEXT> renderer;
        std::vector<Microsoft::Xna::Framework::Matrix> instances;
        CityMaterial material = CityMaterial::StreetFurniture;
        Microsoft::Xna::Framework::Vector3 tint{1.0f, 1.0f, 1.0f};
        bool emissiveAtNight = false;   ///< Lamp heads and headlights.
    };

    /**
     * @brief Everything drawn many times from one mesh: street furniture, trees, vehicles, people.
     *
     * The split against CityGeometry is by *identity*, not by count. A building is unique -- its
     * facade UVs come from its own dimensions -- so it is baked. A lamp post is the same lamp post
     * twenty thousand times over, so it is instanced, and its twenty thousand world matrices are
     * one buffer upload per frame.
     */
    class InstanceRenderer
    {
    public:
        static constexpr int kColorBuckets = 8;
        static constexpr int kWalkPhases = 4;

        void Build(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                   const MaterialLibrary& materials, std::uint64_t seed);
        void Release();

        /** @brief Clears every batch's instance list. Call once per frame before the Add calls. */
        void BeginFrame();

        void AddProp(PropKind kind, const Microsoft::Xna::Framework::Matrix& world);
        void AddVehicle(VehicleKind kind, std::uint8_t colorBucket,
                        const Microsoft::Xna::Framework::Matrix& world);
        void AddPerson(PersonLod lod, std::uint8_t phase, std::uint8_t colorBucket,
                       const Microsoft::Xna::Framework::Matrix& world);
        void AddTrain(const Microsoft::Xna::Framework::Matrix& world);
        /** @param snow False for a rain streak, true for a snowflake. */
        void AddPrecipitation(bool snow, const Microsoft::Xna::Framework::Matrix& world);

        /**
         * @brief Uploads and draws every non-empty batch.
         *
         * @param nightLevel Scales the emissive batches: lamp heads, signal lenses and headlights.
         * @return The number of draw calls issued.
         */
        int Flush(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                  Microsoft::Xna::Framework::Graphics::PbrEffect& effect,
                  const MaterialLibrary& materials, const Microsoft::Xna::Framework::Matrix& view,
                  const Microsoft::Xna::Framework::Matrix& projection, float nightLevel,
                  float wetness);

        [[nodiscard]] std::size_t instanceCount() const;
        [[nodiscard]] bool instancingSupported() const { return instancingSupported_; }
        [[nodiscard]] const std::string& diagnostic() const { return diagnostic_; }

        /** @brief The colour a bucket paints, for the HUD and for the follow camera's marker. */
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 ClothingColor(std::uint8_t bucket) const;

    private:
        std::size_t AddBatch(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                             MeshData&& data, CityMaterial material,
                             const Microsoft::Xna::Framework::Vector3& tint, bool emissive);

        std::vector<InstanceBatch> batches_;
        /// Index maps into batches_. A prop kind can own more than one batch: a tree is a trunk
        /// and a canopy in two different materials, and both take the same instance matrices.
        std::vector<std::size_t> propBatches_[kPropKindCount];
        std::size_t vehicleBody_[kVehicleKindCount][kColorBuckets] = {};
        std::size_t vehicleGlass_[kVehicleKindCount] = {};
        std::size_t personBatch_[3][kWalkPhases][kColorBuckets] = {};
        std::size_t trainBatch_ = 0;
        std::size_t rainBatch_ = 0;
        std::size_t snowBatch_ = 0;
        bool instancingSupported_ = false;
        std::string diagnostic_;
    };
}
