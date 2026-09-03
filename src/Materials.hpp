// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "CityMath.hpp"
#include "Microsoft/Xna/Framework/Color.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsDevice.hpp"
#include "Microsoft/Xna/Framework/Graphics/PbrEffect.hpp"
#include "Microsoft/Xna/Framework/Graphics/Texture2D.hpp"
#include "Microsoft/Xna/Framework/Vector3.hpp"

namespace CnaCity
{
    /**
     * @brief Every surface in the city. The order is also the draw order within a chunk.
     *
     * Grouping by material is what turns eleven thousand buildings into a handful of draw calls:
     * each chunk of the city holds one mesh per material, and a frame is a loop over visible
     * chunks and their materials rather than over anything the size of the population.
     */
    enum class CityMaterial : std::uint8_t
    {
        Asphalt = 0,      ///< Carriageway, with lane markings baked into the texture.
        Pavement,         ///< Kerbs and footways.
        Grass,            ///< Parks, verges and block interiors.
        GlassTower,       ///< Downtown curtain wall.
        ConcreteOffice,   ///< Mid-rise commercial.
        BrickApartment,   ///< Perimeter-block residential.
        RenderHouse,      ///< Suburban render and brick.
        MetalShed,        ///< Industrial cladding.
        Roof,             ///< Flat roofs: felt, gravel and plant rooms.
        RoofTile,         ///< Pitched roofs: clay pantiles on the suburbs.
        RoadMarking,      ///< White thermoplastic: crossings and stop lines.
        Foliage,
        Bark,
        StreetFurniture,  ///< Painted metal: lamp columns, signals, shelters.
        VehicleBody,
        VehicleGlass,
        Person,
        MetroTunnel,      ///< Tunnel walls and roof: cast concrete.
        MetroFloor,       ///< Track bed and walkway: darker, so the tube is not one flat tone.
        MetroRail,        ///< Running rail: worn steel.
        TunnelLight,      ///< The lit strip along a tunnel roof; emissive at every hour.
        Count
    };

    inline constexpr int kCityMaterialCount = static_cast<int>(CityMaterial::Count);

    /** @brief A PBR material: the maps, the factors, and the UV scale its geometry assumes. */
    struct Material
    {
        Microsoft::Xna::Framework::Graphics::Texture2D* albedo = nullptr;
        Microsoft::Xna::Framework::Graphics::Texture2D* normal = nullptr;
        Microsoft::Xna::Framework::Graphics::Texture2D* emissive = nullptr;
        Microsoft::Xna::Framework::Graphics::Texture2D* metallicRoughness = nullptr;
        Microsoft::Xna::Framework::Vector3 baseColor{1.0f, 1.0f, 1.0f};
        float metallic = 0.0f;
        float roughness = 0.85f;
        /// How much of the emissive map shows at night. Lit windows, shop fronts and lamp heads
        /// all ride on this one number, which the clock drives.
        float nightEmissive = 0.0f;
        /// Emissive regardless of the hour. A tunnel is lit at noon; the sun never reaches it.
        float constantEmissive = 0.0f;
        /// Metres of world per texture repeat, horizontally and vertically. The mesh builder uses
        /// it so that a window is the same size on a house and on a tower.
        Vec2 worldScale{4.0f, 4.0f};
        bool doubleSided = false;
        /// Whether lying snow settles on it. It does not stick to a wall.
        bool horizontal = false;
    };

    /**
     * @brief Bakes every texture the city uses, procedurally, at start-up.
     *
     * There is not a single image file in this project, and that is the point: a procedural city
     * that loads its facades from disk is only half procedural. Every texture here is a few dozen
     * lines of arithmetic, and each carries its own mip chain built by box filtering -- a city
     * seen from four hundred metres up is almost entirely minified, and unfiltered facades at that
     * distance shimmer so badly that no amount of anti-aliasing afterwards recovers it.
     */
    class MaterialLibrary
    {
    public:
        void Build(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device, std::uint64_t seed);
        void Release();

        [[nodiscard]] const Material& Get(CityMaterial material) const
        {
            return materials_[static_cast<int>(material)];
        }

        /**
         * @brief Configures @p effect for @p material, with the night-time emissive scaled by
         * @p nightLevel and the surface wetted by @p wetness.
         *
         * Wetness is not a texture swap: it darkens the albedo and collapses the roughness, which
         * is what water on a surface physically does and why wet asphalt at night is the single
         * most convincing thing a renderer can do with a street.
         */
        void Apply(Microsoft::Xna::Framework::Graphics::PbrEffect& effect, CityMaterial material,
                   float nightLevel, float wetness, float snow = 0.0f) const;

        [[nodiscard]] std::size_t textureBytes() const { return textureBytes_; }
        [[nodiscard]] std::size_t textureCount() const { return owned_.size(); }

    private:
        Microsoft::Xna::Framework::Graphics::Texture2D* Adopt(
            std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> texture);

        std::vector<std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D>> owned_;
        Material materials_[kCityMaterialCount];
        std::size_t textureBytes_ = 0;
    };

    /** @brief A CPU pixel buffer being painted before it becomes a texture. */
    struct Bitmap
    {
        int width = 0;
        int height = 0;
        std::vector<Microsoft::Xna::Framework::Color> pixels;

        Bitmap() = default;
        Bitmap(int w, int h, Microsoft::Xna::Framework::Color fill);

        [[nodiscard]] Microsoft::Xna::Framework::Color& At(int x, int y)
        {
            return pixels[static_cast<std::size_t>(y) * width + x];
        }
        [[nodiscard]] const Microsoft::Xna::Framework::Color& At(int x, int y) const
        {
            return pixels[static_cast<std::size_t>(y) * width + x];
        }

        void Fill(Microsoft::Xna::Framework::Color color);
        void FillRect(int x0, int y0, int x1, int y1, Microsoft::Xna::Framework::Color color);
        /** @brief Blends @p color over the rectangle with the given alpha. */
        void BlendRect(int x0, int y0, int x1, int y1, Microsoft::Xna::Framework::Color color,
                       float alpha);
    };

    /**
     * @brief Uploads @p bitmap as a texture with a complete box-filtered mip chain.
     *
     * @param srgb True for colour data, false for data maps (normals, roughness). A normal map
     *        averaged in sRGB space bends its normals toward the light, which shows up as a
     *        surface that gets shinier the further away it is.
     */
    std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> UploadWithMips(
        Microsoft::Xna::Framework::Graphics::GraphicsDevice& device, const Bitmap& bitmap, bool srgb);
}
