// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>

#include "WorldState.hpp"
#include "CNA/Graphics/EnvironmentProcessor.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsDevice.hpp"
#include "Microsoft/Xna/Framework/Graphics/ImageBasedLightEXT.hpp"
#include "Microsoft/Xna/Framework/Graphics/Texture2D.hpp"
#include "Microsoft/Xna/Framework/Graphics/TextureCube.hpp"
#include "Microsoft/Xna/Framework/Vector3.hpp"

namespace CnaCity
{
    /**
     * @brief Ambient light taken from the sky that is actually being drawn.
     *
     * `AtmosphericSky::radiance` is a static CPU function -- the same model the sky shader runs,
     * exposed "so a game can ask what colour the sky is without rendering one, which is what an
     * ambient term or a fog colour wants to know". That is exactly this. A small cube is sampled
     * from it, `EnvironmentProcessor` convolves that into diffuse irradiance and a prefiltered
     * specular chain, and `PbrEffect::setImageBasedLightEXT` consumes both.
     *
     * The alternative, and what this replaced, is a hand-tuned ambient constant with a
     * daylight ramp and a separate sodium term for the night. It looked plausible and it was
     * wrong in the way hand-tuned constants are wrong: the ambient did not redden at sunset, did
     * not go blue under a clear noon sky, and had to be re-tuned by eye every time the exposure
     * moved.
     *
     * Rebuilt only when the sun has moved far enough to matter, because the convolution is a
     * fraction of a millisecond but not free, and the sun moves 15 degrees an hour.
     */
    class SkyLighting
    {
    public:
        bool Build(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device);
        void Release();

        /**
         * @brief Rebuilds the environment when the sky has changed enough to be worth it.
         *
         * @return True when a rebuild actually happened this call.
         */
        bool Update(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                    const Microsoft::Xna::Framework::Vector3& sunDirection, float turbidity,
                    float daylight, float cloudiness, float streetLightLevel);

        [[nodiscard]] bool valid() const { return light_.Irradiance != nullptr; }
        [[nodiscard]] const Microsoft::Xna::Framework::Graphics::ImageBasedLightEXT& light() const
        {
            return light_;
        }

        /**
         * @brief The sky's own average colour, for the fog and the clear colour.
         *
         * Taken from the same samples as the cube, so the fog cannot drift out of agreement with
         * the sky the way two separately tuned constants do.
         */
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 averageSkyColor() const { return average_; }
        /** @brief The horizon's colour, which is what distant geometry actually fades into. */
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 horizonColor() const { return horizon_; }

        [[nodiscard]] double lastRebuildMs() const { return lastRebuildMs_; }
        [[nodiscard]] std::uint32_t rebuildCount() const { return rebuilds_; }
        [[nodiscard]] const std::string& diagnostic() const { return diagnostic_; }

    private:
        void SampleSky(const Microsoft::Xna::Framework::Vector3& sunDirection, float turbidity,
                       float daylight, float cloudiness, float streetLightLevel);

        std::unique_ptr<CNA::Graphics::EnvironmentProcessor> processor_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::TextureCube> environment_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::TextureCube> irradiance_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::TextureCube> specular_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> brdf_;
        Microsoft::Xna::Framework::Graphics::ImageBasedLightEXT light_;

        Microsoft::Xna::Framework::Vector3 average_{0.3f, 0.35f, 0.45f};
        Microsoft::Xna::Framework::Vector3 horizon_{0.4f, 0.45f, 0.55f};
        Microsoft::Xna::Framework::Vector3 lastSun_{0.0f, -1.0f, 0.0f};
        float lastTurbidity_ = -1.0f;
        float lastDaylight_ = -1.0f;
        float lastCloud_ = -1.0f;

        double lastRebuildMs_ = 0.0;
        std::uint32_t rebuilds_ = 0;
        std::string diagnostic_;

        /// 16 pixels a face. Irradiance is the lowest-frequency signal there is -- the source only
        /// has to carry the sun's position and the horizon gradient, and this is a hundred times
        /// cheaper to sample on the CPU than a size anyone would call detailed.
        static constexpr int kEnvironmentFace = 16;
        static constexpr int kIrradianceFace = 8;
        static constexpr int kSpecularFace = 8;
        static constexpr int kSpecularMips = 3;
    };
}
