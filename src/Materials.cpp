// SPDX-License-Identifier: MIT
#include "Materials.hpp"

#include <algorithm>
#include <cmath>

#include "Microsoft/Xna/Framework/Graphics/SurfaceFormat.hpp"
#include "Rng.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaCity
{
    namespace
    {
        constexpr int kTile = 256;

        Color Mix(Color a, Color b, float t)
        {
            const auto lerp = [&](int x, int y) {
                return static_cast<int>(Clamp(static_cast<float>(x) + (static_cast<float>(y - x)) * t,
                                              0.0f, 255.0f));
            };
            return Color(lerp(a.getRProperty(), b.getRProperty()),
                         lerp(a.getGProperty(), b.getGProperty()),
                         lerp(a.getBProperty(), b.getBProperty()), 255);
        }

        Color Shade(Color c, float factor)
        {
            const auto scale = [&](int v) {
                return static_cast<int>(Clamp(static_cast<float>(v) * factor, 0.0f, 255.0f));
            };
            return Color(scale(c.getRProperty()), scale(c.getGProperty()), scale(c.getBProperty()), 255);
        }

        /** @brief A flat normal, encoded the way a tangent-space normal map is. */
        const Color kFlatNormal(128, 128, 255, 255);

        Color EncodeNormal(float x, float y, float z)
        {
            const float length = std::sqrt(x * x + y * y + z * z);
            const float inv = length > 1e-5f ? 1.0f / length : 0.0f;
            const auto pack = [](float v) {
                return static_cast<int>(Clamp((v * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            };
            return Color(pack(x * inv), pack(y * inv), pack(z * inv), 255);
        }

        /**
         * @brief Fills @p bitmap with fine value noise, as a multiplicative shade.
         *
         * Every real surface in a city is dirty in a way that is uncorrelated at the scale of a
         * texel and correlated at the scale of a metre. Two octaves is enough to read as both.
         */
        void AddNoise(Bitmap& bitmap, Rng& rng, float fine, float coarse)
        {
            const int coarseSize = 16;
            std::vector<float> field(static_cast<std::size_t>(coarseSize) * coarseSize);
            for (float& value : field) value = rng.NextFloat(-1.0f, 1.0f);
            for (int y = 0; y < bitmap.height; ++y)
                for (int x = 0; x < bitmap.width; ++x)
                {
                    // Bilinear over a wrapping coarse lattice, so the tile still tiles.
                    const float fx = static_cast<float>(x) * coarseSize / static_cast<float>(bitmap.width);
                    const float fy = static_cast<float>(y) * coarseSize / static_cast<float>(bitmap.height);
                    const int x0 = static_cast<int>(fx) % coarseSize;
                    const int y0 = static_cast<int>(fy) % coarseSize;
                    const int x1 = (x0 + 1) % coarseSize;
                    const int y1 = (y0 + 1) % coarseSize;
                    const float tx = fx - std::floor(fx);
                    const float ty = fy - std::floor(fy);
                    const float top = field[static_cast<std::size_t>(y0) * coarseSize + x0] * (1 - tx) +
                                      field[static_cast<std::size_t>(y0) * coarseSize + x1] * tx;
                    const float bottom = field[static_cast<std::size_t>(y1) * coarseSize + x0] * (1 - tx) +
                                         field[static_cast<std::size_t>(y1) * coarseSize + x1] * tx;
                    const float value = top * (1 - ty) + bottom * ty;
                    bitmap.At(x, y) = Shade(bitmap.At(x, y),
                                            1.0f + value * coarse + rng.NextFloat(-fine, fine));
                }
        }

        /** @brief A metallic-roughness map: green is roughness, blue is metallic, per glTF. */
        Bitmap MakeMetallicRoughness(int size, float roughness, float metallic, Rng& rng,
                                     float roughnessJitter)
        {
            Bitmap map(size, size, Color(0, 0, 0, 255));
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    const float r = Clamp(roughness + rng.NextFloat(-roughnessJitter, roughnessJitter),
                                          0.03f, 1.0f);
                    map.At(x, y) = Color(0, static_cast<int>(r * 255.0f),
                                         static_cast<int>(metallic * 255.0f), 255);
                }
            return map;
        }

        /**
         * @brief One facade tile: @p bays windows across, @p floors storeys down.
         *
         * The tile is what every building in the city is wrapped in, and the mesh scales its UVs
         * by the building's real size, so a window is 3.2 m wide on a bungalow and 3.2 m wide on a
         * hundred-and-eighty-metre tower. Getting that one thing right is most of what makes a
         * procedural skyline read as a city rather than as a bar chart.
         */
        struct FacadeSpec
        {
            Color wall;
            Color wallDark;
            Color frame;
            Color glass;
            Color glassBright;
            int bays;
            int floors;
            float windowWidth;      ///< Fraction of a bay.
            float windowHeight;     ///< Fraction of a storey.
            float sillFraction;     ///< Where the window starts within the storey.
            bool bandedFloors;      ///< A spandrel band between storeys, as a curtain wall has.
            float litChance;
        };

        struct FacadeMaps
        {
            Bitmap albedo;
            Bitmap normal;
            Bitmap emissive;
            Bitmap metallicRoughness;
        };

        FacadeMaps MakeFacade(const FacadeSpec& spec, Rng& rng, float roughness, float metallic)
        {
            FacadeMaps maps{Bitmap(kTile, kTile, spec.wall), Bitmap(kTile, kTile, kFlatNormal),
                            Bitmap(kTile, kTile, Color(0, 0, 0, 255)),
                            MakeMetallicRoughness(kTile, roughness, metallic, rng, 0.05f)};

            const int bayWidth = kTile / spec.bays;
            const int floorHeight = kTile / spec.floors;

            for (int by = 0; by < spec.floors; ++by)
            {
                if (spec.bandedFloors)
                {
                    // The spandrel: a darker band across the whole storey line.
                    const int y0 = by * floorHeight;
                    maps.albedo.FillRect(0, y0, kTile, y0 + std::max(2, floorHeight / 8), spec.wallDark);
                }
                for (int bx = 0; bx < spec.bays; ++bx)
                {
                    const int x0 = bx * bayWidth;
                    const int y0 = by * floorHeight;
                    const int wWidth = static_cast<int>(static_cast<float>(bayWidth) * spec.windowWidth);
                    const int wHeight = static_cast<int>(static_cast<float>(floorHeight) * spec.windowHeight);
                    const int wx = x0 + (bayWidth - wWidth) / 2;
                    const int wy = y0 + static_cast<int>(static_cast<float>(floorHeight) * spec.sillFraction);

                    // The reveal: a one-texel frame plus a normal-map step. A window painted flat
                    // on a wall reads as a sticker; the step is what makes it a hole.
                    maps.albedo.FillRect(wx - 2, wy - 2, wx + wWidth + 2, wy + wHeight + 2, spec.frame);

                    // Each window gets its own glass tint, which is what stops a curtain wall
                    // looking like one printed sheet.
                    const Color glass = Mix(spec.glass, spec.glassBright, rng.NextFloat(0.0f, 1.0f));
                    maps.albedo.FillRect(wx, wy, wx + wWidth, wy + wHeight, glass);

                    // A blind or a curtain in some of them, drawn as an opaque band from the top.
                    if (rng.Chance(0.28f))
                    {
                        const int blind = static_cast<int>(static_cast<float>(wHeight) *
                                                           rng.NextFloat(0.2f, 0.75f));
                        maps.albedo.FillRect(wx, wy, wx + wWidth, wy + blind,
                                             Shade(spec.wall, rng.NextFloat(1.05f, 1.3f)));
                    }

                    for (int y = wy - 2; y < wy + wHeight + 2; ++y)
                        for (int x = wx - 2; x < wx + wWidth + 2; ++x)
                        {
                            if (x < 0 || y < 0 || x >= kTile || y >= kTile) continue;
                            const bool leftEdge = x < wx;
                            const bool rightEdge = x >= wx + wWidth;
                            const bool topEdge = y < wy;
                            const bool bottomEdge = y >= wy + wHeight;
                            if (!leftEdge && !rightEdge && !topEdge && !bottomEdge) continue;
                            const float nx = leftEdge ? -0.8f : (rightEdge ? 0.8f : 0.0f);
                            const float ny = topEdge ? 0.8f : (bottomEdge ? -0.8f : 0.0f);
                            maps.normal.At(x, y) = EncodeNormal(nx, ny, 0.7f);
                        }

                    // Glass is smooth and slightly reflective whatever the wall is made of.
                    for (int y = wy; y < wy + wHeight; ++y)
                        for (int x = wx; x < wx + wWidth; ++x)
                            if (x >= 0 && y >= 0 && x < kTile && y < kTile)
                                maps.metallicRoughness.At(x, y) = Color(0, 28, 60, 255);

                    if (rng.Chance(spec.litChance))
                    {
                        // Interior light, warm and not quite even. It is the emissive map that
                        // does the whole of the night-time skyline.
                        const Color warm(255, static_cast<int>(rng.NextFloat(190.0f, 226.0f)),
                                         static_cast<int>(rng.NextFloat(130.0f, 176.0f)), 255);
                        maps.emissive.FillRect(wx, wy, wx + wWidth, wy + wHeight,
                                               Shade(warm, rng.NextFloat(0.55f, 1.0f)));
                    }
                }
            }

            AddNoise(maps.albedo, rng, 0.02f, 0.05f);
            return maps;
        }
    }

    Bitmap::Bitmap(int w, int h, Color fill)
        : width(w), height(h), pixels(static_cast<std::size_t>(w) * h, fill)
    {
    }

    void Bitmap::Fill(Color color) { std::fill(pixels.begin(), pixels.end(), color); }

    void Bitmap::FillRect(int x0, int y0, int x1, int y1, Color color)
    {
        for (int y = std::max(0, y0); y < std::min(height, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(width, x1); ++x)
                At(x, y) = color;
    }

    void Bitmap::BlendRect(int x0, int y0, int x1, int y1, Color color, float alpha)
    {
        for (int y = std::max(0, y0); y < std::min(height, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(width, x1); ++x)
                At(x, y) = Mix(At(x, y), color, alpha);
    }

    std::unique_ptr<Texture2D> UploadWithMips(GraphicsDevice& device, const Bitmap& bitmap, bool srgb)
    {
        auto texture = std::make_unique<Texture2D>(device, bitmap.width, bitmap.height, true,
                                                   SurfaceFormat::Color);
        texture->SetData(bitmap.pixels.data(), static_cast<int>(bitmap.pixels.size()));

        // The chain is built here rather than left to the driver because CNA's Texture2D allocates
        // the levels and does not fill them, and a texture whose lower levels are undefined shows
        // it the moment the camera pulls back -- which for a city is most of the time.
        std::vector<Color> current = bitmap.pixels;
        int width = bitmap.width;
        int height = bitmap.height;
        int level = 1;
        while (width > 1 || height > 1)
        {
            const int nextWidth = std::max(1, width / 2);
            const int nextHeight = std::max(1, height / 2);
            std::vector<Color> next(static_cast<std::size_t>(nextWidth) * nextHeight);
            for (int y = 0; y < nextHeight; ++y)
                for (int x = 0; x < nextWidth; ++x)
                {
                    const int sx = std::min(width - 1, x * 2);
                    const int sy = std::min(height - 1, y * 2);
                    const int sx1 = std::min(width - 1, sx + 1);
                    const int sy1 = std::min(height - 1, sy + 1);
                    const Color* row0 = current.data() + static_cast<std::size_t>(sy) * width;
                    const Color* row1 = current.data() + static_cast<std::size_t>(sy1) * width;
                    const auto average = [&](int channel) {
                        const auto get = [channel](const Color& c) {
                            return channel == 0 ? static_cast<int>(c.getRProperty())
                                                : channel == 1 ? static_cast<int>(c.getGProperty())
                                                               : static_cast<int>(c.getBProperty());
                        };
                        // Colour data is averaged in linear light. Doing it in sRGB is the classic
                        // way to make every mip level a little brighter than the one above it,
                        // which on a facade of dark windows reads as the building fading out.
                        if (!srgb)
                            return (get(row0[sx]) + get(row0[sx1]) + get(row1[sx]) + get(row1[sx1]) + 2) / 4;
                        const auto toLinear = [](int v) {
                            const float f = static_cast<float>(v) / 255.0f;
                            return f <= 0.04045f ? f / 12.92f : std::pow((f + 0.055f) / 1.055f, 2.4f);
                        };
                        const float sum = toLinear(get(row0[sx])) + toLinear(get(row0[sx1])) +
                                          toLinear(get(row1[sx])) + toLinear(get(row1[sx1]));
                        const float mean = sum * 0.25f;
                        const float encoded = mean <= 0.0031308f
                                                  ? mean * 12.92f
                                                  : 1.055f * std::pow(mean, 1.0f / 2.4f) - 0.055f;
                        return static_cast<int>(Clamp(encoded * 255.0f, 0.0f, 255.0f));
                    };
                    next[static_cast<std::size_t>(y) * nextWidth + x] =
                        Color(average(0), average(1), average(2), 255);
                }
            texture->SetData(level, nullptr, next.data(), 0, static_cast<int>(next.size()));
            current.swap(next);
            width = nextWidth;
            height = nextHeight;
            ++level;
        }
        return texture;
    }

    Texture2D* MaterialLibrary::Adopt(std::unique_ptr<Texture2D> texture)
    {
        Texture2D* raw = texture.get();
        // 4/3 accounts for the mip chain, which is where a third of the memory actually goes.
        textureBytes_ += static_cast<std::size_t>(texture->getWidthProperty()) *
                         static_cast<std::size_t>(texture->getHeightProperty()) * 4u * 4u / 3u;
        owned_.push_back(std::move(texture));
        return raw;
    }

    void MaterialLibrary::Release()
    {
        owned_.clear();
        textureBytes_ = 0;
        for (Material& material : materials_) material = Material{};
    }

    void MaterialLibrary::Build(GraphicsDevice& device, std::uint64_t seed)
    {
        Release();
        Rng rng(seed, 0x4d41'5445'5249'414cULL);

        // ---- The carriageway ----------------------------------------------------------------
        //
        // One atlas, eight horizontal bands, one per road class with room to spare. The mesh maps
        // its across-the-road coordinate into the band its class owns, so a two-lane local street
        // and a six-lane arterial get their own markings out of a single texture and therefore a
        // single draw call. u runs along the road and wraps.
        {
            Bitmap road(kTile, kTile, Color(58, 59, 62, 255));
            Bitmap roadNormal(kTile, kTile, kFlatNormal);
            Bitmap roadMr = MakeMetallicRoughness(kTile, 0.72f, 0.0f, rng, 0.10f);
            AddNoise(road, rng, 0.055f, 0.10f);

            const Color white(228, 228, 220, 255);
            const Color yellow(226, 190, 70, 255);
            constexpr int kBandHeight = kTile / 8;
            // Lanes each way per band, matching RoadClass order: highway, arterial, collector,
            // local, alley.
            const int lanesPerSide[5] = {3, 2, 1, 1, 1};
            for (int band = 0; band < 5; ++band)
            {
                const int top = band * kBandHeight;
                const int bottom = top + kBandHeight;
                const int lanes = lanesPerSide[band];
                const auto rowFor = [&](float v) {
                    return top + static_cast<int>(v * static_cast<float>(kBandHeight));
                };

                // Kerb edge lines, except on the alley, which has none.
                if (band != 4)
                {
                    road.FillRect(0, rowFor(0.025f), kTile, rowFor(0.045f), white);
                    road.FillRect(0, rowFor(0.955f), kTile, rowFor(0.975f), white);
                }

                // The centre line: solid double yellow on the highway, dashed white elsewhere.
                if (band == 0)
                {
                    road.FillRect(0, rowFor(0.485f), kTile, rowFor(0.497f), yellow);
                    road.FillRect(0, rowFor(0.503f), kTile, rowFor(0.515f), yellow);
                }
                else if (band != 4)
                {
                    for (int x = 0; x < kTile; x += 40)
                        road.FillRect(x, rowFor(0.492f), x + 24, rowFor(0.508f), white);
                }

                // Lane separators within each direction, dashed and shorter.
                for (int side = 0; side < 2; ++side)
                    for (int lane = 1; lane < lanes; ++lane)
                    {
                        const float t = static_cast<float>(lane) / static_cast<float>(lanes);
                        const float v = side == 0 ? 0.5f - t * 0.46f : 0.5f + t * 0.46f;
                        for (int x = 0; x < kTile; x += 32)
                            road.FillRect(x, rowFor(v - 0.006f), x + 14, rowFor(v + 0.006f), white);
                    }

                // A worn strip under each wheel track: darker, smoother, and the thing that makes
                // an empty road look used.
                for (int side = 0; side < 2; ++side)
                    for (int lane = 0; lane < lanes; ++lane)
                    {
                        const float centre = static_cast<float>(lane * 2 + 1) /
                                             static_cast<float>(lanes * 2);
                        const float v = side == 0 ? 0.5f - centre * 0.46f : 0.5f + centre * 0.46f;
                        for (int offset = -1; offset <= 1; offset += 2)
                        {
                            const int y0 = rowFor(v + static_cast<float>(offset) * 0.055f - 0.012f);
                            const int y1 = rowFor(v + static_cast<float>(offset) * 0.055f + 0.012f);
                            road.BlendRect(0, y0, kTile, y1, Color(34, 34, 36, 255), 0.35f);
                            for (int y = std::max(top, y0); y < std::min(bottom, y1); ++y)
                                for (int x = 0; x < kTile; ++x)
                                    roadMr.At(x, y) = Color(0, 140, 0, 255);
                        }
                    }
            }

            Material& asphalt = materials_[static_cast<int>(CityMaterial::Asphalt)];
            asphalt.albedo = Adopt(UploadWithMips(device, road, true));
            asphalt.normal = Adopt(UploadWithMips(device, roadNormal, false));
            asphalt.metallicRoughness = Adopt(UploadWithMips(device, roadMr, false));
            asphalt.roughness = 0.72f;
            asphalt.worldScale = Vec2(9.0f, 1.0f);
            asphalt.horizontal = true;
        }

        // ---- Pavement -------------------------------------------------------------------------
        {
            Bitmap slabs(kTile, kTile, Color(146, 143, 137, 255));
            AddNoise(slabs, rng, 0.05f, 0.09f);
            Bitmap normal(kTile, kTile, kFlatNormal);
            constexpr int kSlab = kTile / 4;
            for (int i = 0; i <= 4; ++i)
            {
                slabs.FillRect(i * kSlab - 1, 0, i * kSlab + 1, kTile, Color(118, 115, 110, 255));
                slabs.FillRect(0, i * kSlab - 1, kTile, i * kSlab + 1, Color(118, 115, 110, 255));
                for (int y = 0; y < kTile; ++y)
                {
                    normal.At(Clamp(i * kSlab - 1, 0, kTile - 1), y) = EncodeNormal(-0.6f, 0.0f, 0.8f);
                    normal.At(Clamp(i * kSlab, 0, kTile - 1), y) = EncodeNormal(0.6f, 0.0f, 0.8f);
                }
                for (int x = 0; x < kTile; ++x)
                {
                    normal.At(x, Clamp(i * kSlab - 1, 0, kTile - 1)) = EncodeNormal(0.0f, -0.6f, 0.8f);
                    normal.At(x, Clamp(i * kSlab, 0, kTile - 1)) = EncodeNormal(0.0f, 0.6f, 0.8f);
                }
            }
            Material& pavement = materials_[static_cast<int>(CityMaterial::Pavement)];
            pavement.albedo = Adopt(UploadWithMips(device, slabs, true));
            pavement.normal = Adopt(UploadWithMips(device, normal, false));
            pavement.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(kTile, 0.80f, 0.0f, rng, 0.08f), false));
            pavement.roughness = 0.80f;
            pavement.worldScale = Vec2(2.4f, 2.4f);
            pavement.horizontal = true;
        }

        // ---- Grass and parkland ----------------------------------------------------------------
        {
            Bitmap grass(kTile, kTile, Color(72, 96, 46, 255));
            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x)
                    grass.At(x, y) = Mix(Color(58, 82, 38, 255), Color(96, 122, 58, 255),
                                         rng.NextFloat(0.0f, 1.0f));
            AddNoise(grass, rng, 0.03f, 0.16f);
            Material& green = materials_[static_cast<int>(CityMaterial::Grass)];
            green.albedo = Adopt(UploadWithMips(device, grass, true));
            green.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(kTile, 0.92f, 0.0f, rng, 0.06f), false));
            green.roughness = 0.92f;
            green.worldScale = Vec2(5.0f, 5.0f);
            green.horizontal = true;
        }

        // ---- The facades ------------------------------------------------------------------------
        struct FacadeEntry
        {
            CityMaterial material;
            FacadeSpec spec;
            float roughness;
            float metallic;
            Vec2 worldScale;   ///< Metres per repeat: bays x 3.2 m across, floors x storey height.
            float nightEmissive;
        };

        const FacadeEntry facades[] = {
            {CityMaterial::GlassTower,
             {Color(58, 70, 84, 255), Color(38, 46, 56, 255), Color(96, 104, 112, 255),
              Color(74, 104, 128, 255), Color(126, 158, 178, 255), 4, 4, 0.92f, 0.80f, 0.10f, true, 0.34f},
             0.22f, 0.30f, Vec2(12.8f, 15.6f), 0.30f},
            {CityMaterial::ConcreteOffice,
             {Color(158, 154, 146, 255), Color(126, 122, 116, 255), Color(96, 94, 90, 255),
              Color(70, 88, 104, 255), Color(110, 132, 150, 255), 4, 4, 0.66f, 0.62f, 0.20f, true, 0.30f},
             0.68f, 0.0f, Vec2(12.8f, 15.2f), 0.28f},
            {CityMaterial::BrickApartment,
             {Color(134, 84, 68, 255), Color(112, 68, 56, 255), Color(214, 210, 202, 255),
              Color(62, 76, 92, 255), Color(96, 116, 134, 255), 4, 4, 0.48f, 0.66f, 0.18f, false, 0.42f},
             0.86f, 0.0f, Vec2(12.8f, 12.8f), 0.34f},
            {CityMaterial::RenderHouse,
             {Color(206, 196, 176, 255), Color(178, 168, 150, 255), Color(238, 236, 230, 255),
              Color(58, 70, 84, 255), Color(92, 108, 124, 255), 2, 2, 0.44f, 0.56f, 0.22f, false, 0.50f},
             0.88f, 0.0f, Vec2(6.4f, 6.2f), 0.34f},
            {CityMaterial::MetalShed,
             {Color(152, 156, 160, 255), Color(128, 132, 136, 255), Color(96, 100, 104, 255),
              Color(70, 80, 90, 255), Color(100, 112, 124, 255), 4, 2, 0.30f, 0.34f, 0.42f, false, 0.14f},
             0.52f, 0.55f, Vec2(12.8f, 11.2f), 0.18f},
        };

        for (const FacadeEntry& entry : facades)
        {
            Rng facadeRng = rng.Split(static_cast<std::uint64_t>(entry.material) + 100u);
            FacadeMaps maps = MakeFacade(entry.spec, facadeRng, entry.roughness, entry.metallic);

            // Brick and ribbed cladding get their pattern stamped into the wall *before* the
            // windows, so a window sits in a brick wall rather than on top of a picture of one.
            if (entry.material == CityMaterial::BrickApartment)
                for (int y = 0; y < kTile; ++y)
                    for (int x = 0; x < kTile; ++x)
                    {
                        const int course = y / 5;
                        const int offset = (course & 1) ? 9 : 0;
                        const bool mortar = (y % 5) == 0 || ((x + offset) % 18) == 0;
                        if (mortar && maps.emissive.At(x, y).getRProperty() == 0)
                            maps.albedo.At(x, y) = Mix(maps.albedo.At(x, y),
                                                       Color(186, 178, 166, 255), 0.55f);
                    }
            if (entry.material == CityMaterial::MetalShed)
                for (int y = 0; y < kTile; ++y)
                    for (int x = 0; x < kTile; ++x)
                        if ((x % 8) < 2)
                        {
                            maps.albedo.At(x, y) = Shade(maps.albedo.At(x, y), 0.82f);
                            maps.normal.At(x, y) = EncodeNormal((x % 8) == 0 ? -0.7f : 0.7f, 0.0f, 0.7f);
                        }

            Material& material = materials_[static_cast<int>(entry.material)];
            material.albedo = Adopt(UploadWithMips(device, maps.albedo, true));
            material.normal = Adopt(UploadWithMips(device, maps.normal, false));
            material.emissive = Adopt(UploadWithMips(device, maps.emissive, true));
            material.metallicRoughness = Adopt(UploadWithMips(device, maps.metallicRoughness, false));
            material.roughness = entry.roughness;
            material.metallic = entry.metallic;
            material.worldScale = entry.worldScale;
            material.nightEmissive = entry.nightEmissive;
        }

        // ---- Roofs ------------------------------------------------------------------------------
        {
            Bitmap roof(kTile, kTile, Color(66, 66, 68, 255));
            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x)
                    roof.At(x, y) = Mix(Color(54, 54, 56, 255), Color(88, 88, 90, 255),
                                        rng.NextFloat(0.0f, 1.0f));
            AddNoise(roof, rng, 0.04f, 0.12f);
            // Plant rooms, tanks and ducts, painted straight into the texture. From a helicopter
            // they are what stops a roof being a grey rectangle, and they cost no geometry at all.
            for (int i = 0; i < 26; ++i)
            {
                const int x0 = rng.NextInt(0, kTile - 40);
                const int y0 = rng.NextInt(0, kTile - 40);
                const int w = rng.NextInt(10, 38);
                const int h = rng.NextInt(8, 30);
                roof.FillRect(x0, y0, x0 + w, y0 + h, Shade(Color(126, 128, 130, 255),
                                                            rng.NextFloat(0.7f, 1.15f)));
                roof.FillRect(x0 + 2, y0 + h - 3, x0 + w, y0 + h, Color(40, 40, 42, 255));
            }
            Material& material = materials_[static_cast<int>(CityMaterial::Roof)];
            material.albedo = Adopt(UploadWithMips(device, roof, true));
            material.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(kTile, 0.86f, 0.06f, rng, 0.10f), false));
            material.roughness = 0.86f;
            material.worldScale = Vec2(18.0f, 18.0f);
            material.horizontal = true;
        }

        // ---- Pantiles, and the paint on the road ------------------------------------------------
        {
            Bitmap tiles(kTile, kTile, Color(146, 78, 54, 255));
            // Courses of pantiles: a ridged profile in the normal map is what makes a pitched roof
            // read as tiled rather than as a coloured plane, at any distance a house is visible.
            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x)
                {
                    const int course = y / 16;
                    const int offset = (course & 1) ? 12 : 0;
                    const bool gap = (y % 16) < 2;
                    const bool seam = ((x + offset) % 24) < 2;
                    Color c = Mix(Color(128, 64, 44, 255), Color(178, 100, 68, 255),
                                  rng.NextFloat(0.0f, 1.0f));
                    if (gap || seam) c = Shade(c, 0.62f);
                    tiles.At(x, y) = c;
                }
            Bitmap tileNormal(kTile, kTile, kFlatNormal);
            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x)
                {
                    const float phase = static_cast<float>((x % 24)) / 24.0f;
                    tileNormal.At(x, y) = EncodeNormal(std::cos(phase * 6.2831853f) * 0.55f,
                                                       (y % 16) < 2 ? -0.5f : 0.0f, 0.85f);
                }
            Material& tile = materials_[static_cast<int>(CityMaterial::RoofTile)];
            tile.albedo = Adopt(UploadWithMips(device, tiles, true));
            tile.normal = Adopt(UploadWithMips(device, tileNormal, false));
            tile.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(kTile, 0.78f, 0.0f, rng, 0.08f), false));
            tile.roughness = 0.78f;
            tile.worldScale = Vec2(3.2f, 3.2f);
            tile.horizontal = true;

            // Thermoplastic road paint: bright, slightly rough, and a little dirty at the edges of
            // the bar, because a crossing that is pure white reads as a decal.
            Bitmap paint(64, 64, Color(226, 226, 218, 255));
            AddNoise(paint, rng, 0.05f, 0.07f);
            Material& marking = materials_[static_cast<int>(CityMaterial::RoadMarking)];
            marking.albedo = Adopt(UploadWithMips(device, paint, true));
            marking.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(64, 0.55f, 0.0f, rng, 0.06f), false));
            marking.roughness = 0.55f;
            marking.worldScale = Vec2(1.0f, 1.0f);
            marking.horizontal = true;
        }

        // ---- Vegetation --------------------------------------------------------------------------
        {
            Bitmap leaves(kTile, kTile, Color(58, 92, 40, 255));
            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x)
                    leaves.At(x, y) = Mix(Color(38, 68, 28, 255), Color(96, 132, 58, 255),
                                          rng.NextFloat(0.0f, 1.0f));
            AddNoise(leaves, rng, 0.06f, 0.22f);
            Material& foliage = materials_[static_cast<int>(CityMaterial::Foliage)];
            foliage.albedo = Adopt(UploadWithMips(device, leaves, true));
            foliage.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(kTile, 0.90f, 0.0f, rng, 0.08f), false));
            foliage.roughness = 0.90f;
            foliage.worldScale = Vec2(2.0f, 2.0f);
            foliage.doubleSided = true;

            Bitmap bark(kTile, kTile, Color(76, 60, 46, 255));
            for (int x = 0; x < kTile; ++x)
            {
                const float shade = rng.NextFloat(0.72f, 1.24f);
                for (int y = 0; y < kTile; ++y) bark.At(x, y) = Shade(bark.At(x, y), shade);
            }
            AddNoise(bark, rng, 0.06f, 0.10f);
            Material& trunk = materials_[static_cast<int>(CityMaterial::Bark)];
            trunk.albedo = Adopt(UploadWithMips(device, bark, true));
            trunk.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(kTile, 0.94f, 0.0f, rng, 0.06f), false));
            trunk.roughness = 0.94f;
            trunk.worldScale = Vec2(1.5f, 3.0f);
        }

        // ---- Painted metal, glass, paint and skin -------------------------------------------------
        //
        // These four are near-white on purpose. Their colour comes from the effect's diffuse
        // factor, set per draw call, which is how a hundred thousand people and several thousand
        // cars are drawn in a couple of dozen colours without a per-instance attribute the stock
        // effects have no attribute slot left to carry.
        const auto plainMaterial = [&](CityMaterial which, Color tint, float roughness,
                                       float metallic, float emissive) {
            Bitmap flat(64, 64, tint);
            AddNoise(flat, rng, 0.02f, 0.03f);
            Material& material = materials_[static_cast<int>(which)];
            material.albedo = Adopt(UploadWithMips(device, flat, true));
            material.metallicRoughness = Adopt(UploadWithMips(device, MakeMetallicRoughness(64, roughness, metallic, rng, 0.03f), false));
            material.roughness = roughness;
            material.metallic = metallic;
            material.nightEmissive = emissive;
            material.worldScale = Vec2(1.0f, 1.0f);
        };
        plainMaterial(CityMaterial::StreetFurniture, Color(74, 78, 82, 255), 0.42f, 0.65f, 0.0f);
        plainMaterial(CityMaterial::VehicleBody, Color(240, 240, 240, 255), 0.26f, 0.12f, 0.0f);
        plainMaterial(CityMaterial::VehicleGlass, Color(38, 44, 52, 255), 0.10f, 0.20f, 0.0f);
        plainMaterial(CityMaterial::Person, Color(236, 236, 236, 255), 0.74f, 0.0f, 0.0f);
        plainMaterial(CityMaterial::MetroTunnel, Color(122, 120, 116, 255), 0.86f, 0.0f, 0.0f);
        {
            // The underground is the one place the sun never reaches and the sky cannot light, so
            // it carries its own. Without this the follow camera takes you down a staircase with a
            // commuter and shows you a black screen -- the geometry is all there and none of it is
            // lit by anything.
            Material& tunnel = materials_[static_cast<int>(CityMaterial::MetroTunnel)];
            Bitmap glow(32, 32, Color(255, 250, 236, 255));
            AddNoise(glow, rng, 0.03f, 0.06f);
            tunnel.emissive = Adopt(UploadWithMips(device, glow, true));
            tunnel.constantEmissive = 0.20f;
            tunnel.worldScale = Vec2(6.0f, 6.0f);
        }
    }

    void MaterialLibrary::Apply(PbrEffect& effect, CityMaterial which, float nightLevel,
                                float wetness, float snow) const
    {
        const Material& material = materials_[static_cast<int>(which)];
        effect.setTextureProperty(material.albedo);
        effect.setNormalMapProperty(material.normal);
        effect.setMetallicRoughnessMapProperty(material.metallicRoughness);
        effect.setEmissiveMapProperty(material.emissive);

        // Water fills the surface's micro-relief, so it darkens the albedo and collapses the
        // roughness. Both halves matter: darkening alone gives wet-looking mud, and smoothing
        // alone gives polished stone.
        const float wet = Saturate(wetness);
        // Lying snow settles on horizontal surfaces and not on walls, which is why it is a material
        // property rather than a global. It also cancels wetness: a surface cannot be both under
        // standing water and under snow.
        const float lying = material.horizontal ? Saturate(snow) : 0.0f;
        const Vector3 tint = material.baseColor;
        const float darken = 1.0f - 0.42f * wet * (1.0f - lying);
        const Vector3 wetted(tint.X * darken, tint.Y * darken, tint.Z * darken);
        const Vector3 snowColor(0.92f, 0.94f, 0.98f);
        effect.setDiffuseColorProperty(
            Vector3(wetted.X + (snowColor.X - wetted.X) * lying,
                    wetted.Y + (snowColor.Y - wetted.Y) * lying,
                    wetted.Z + (snowColor.Z - wetted.Z) * lying));
        const float wetRoughness = material.roughness * (1.0f - 0.72f * wet);
        effect.setRoughnessFactorProperty(
            Clamp(wetRoughness + (0.94f - wetRoughness) * lying, 0.035f, 1.0f));
        effect.setMetallicFactorProperty(material.metallic);

        const float emissive = std::max(material.constantEmissive,
                                        material.nightEmissive * Saturate(nightLevel));
        effect.setEmissiveFactorProperty(Vector3(emissive, emissive * 0.94f, emissive * 0.84f));
    }
}
