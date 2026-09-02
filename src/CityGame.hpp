// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Camera.hpp"
#include "CityGeometry.hpp"
#include "CliOptions.hpp"
#include "InstanceRenderer.hpp"
#include "Materials.hpp"
#include "Simulation.hpp"
#include "TextRenderer.hpp"

#include "CNA/Graphics/AtmosphericSky.hpp"
#include "CNA/Graphics/CascadedShadowMap.hpp"
#include "CNA/Graphics/DebugDraw.hpp"
#include "CNA/Graphics/DepthNormalPrepass.hpp"
#include "CNA/Graphics/DirectionalLightEXT.hpp"
#include "CNA/Graphics/FrustumCullerEXT.hpp"
#include "CNA/Graphics/RenderPipeline.hpp"
#include "Microsoft/Xna/Framework/Game.hpp"
#include "Microsoft/Xna/Framework/GraphicsDeviceManager.hpp"
#include "Microsoft/Xna/Framework/Graphics/PbrEffect.hpp"
#include "Microsoft/Xna/Framework/Graphics/SpriteBatch.hpp"
#include "Microsoft/Xna/Framework/Input/Keyboard.hpp"
#include "Microsoft/Xna/Framework/Input/Mouse.hpp"

namespace CnaCity
{
    /** @brief What the Tab key cycles through. */
    enum class Overlay : std::uint8_t
    {
        None = 0,
        Statistics,
        RoadNetwork,
        Routes,
        Count
    };

    /**
     * @brief The whole demo: a CNA `Game` that owns the simulation and draws it.
     *
     * The class is deliberately the only thing in the project that knows about both halves. The
     * simulation cannot see a graphics device and the renderers cannot write simulation state, so
     * `--headless` and `--bench` are able to run the entire city with no window at all -- which is
     * what makes the benchmark measure the simulation rather than the driver.
     */
    class CityGame : public Microsoft::Xna::Framework::Game
    {
    public:
        explicit CityGame(const CliOptions& options);
        ~CityGame() override;

    protected:
        void Initialize() override;
        void LoadContent() override;
        void UnloadContent() override;
        void Update(Microsoft::Xna::Framework::GameTime& gameTime) override;
        void Draw(const Microsoft::Xna::Framework::GameTime& gameTime) override;

    private:
        void ApplyQuality();
        void HandleInput(float dt);
        void UpdateCamera(float dt);
        void UpdateLighting();
        void CollectVisible();
        void DrawShadowCascades();
        void DrawStaticCity();
        void DrawSkyOverlay();
        void DrawOverlay();
        void DrawHud();
        void SaveScreenshot();
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 SunColor() const;
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 AmbientColor() const;

        CliOptions options_;
        Microsoft::Xna::Framework::GraphicsDeviceManager graphics_;
        Simulation sim_;

        MaterialLibrary materials_;
        CityGeometry geometry_;
        InstanceRenderer instances_;
        TextRenderer text_;

        std::unique_ptr<Microsoft::Xna::Framework::Graphics::SpriteBatch> batch_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::PbrEffect> effect_;
        std::unique_ptr<CNA::Graphics::RenderPipeline> pipeline_;
        std::unique_ptr<CNA::Graphics::CascadedShadowMap> shadows_;
        std::unique_ptr<CNA::Graphics::AtmosphericSky> sky_;
        std::unique_ptr<CNA::Graphics::DebugDraw> debug_;

        Camera camera_;
        CameraMode cameraMode_ = CameraMode::Orbit;
        Overlay overlay_ = Overlay::Statistics;
        std::uint32_t followAgent_ = kNoIndex;
        bool followSnap_ = true;
        float followIdleSeconds_ = 0.0f;
        float orbitAngle_ = 0.0f;
        float cinematicTime_ = 0.0f;
        float freeSpeed_ = 60.0f;
        bool paused_ = false;
        bool hudVisible_ = true;
        bool postProcessing_ = true;
        bool mouseLook_ = false;

        Microsoft::Xna::Framework::Input::KeyboardState previousKeys_;
        Microsoft::Xna::Framework::Input::MouseState previousMouse_;

        /// Per-frame culling results. Kept as members so the vectors are not reallocated sixty
        /// times a second.
        CNA::Graphics::FrustumCullerEXT culler_;
        std::vector<std::uint32_t> visibleChunks_;
        CNA::Graphics::DirectionalLightEXT sun_;

        // --- Frame statistics ------------------------------------------------------------------
        double frameMs_ = 0.0;
        double simMs_ = 0.0;
        double shadowMs_ = 0.0;
        double sceneMs_ = 0.0;
        double instanceMs_ = 0.0;
        double smoothedFrameMs_ = 16.0;
        int drawCalls_ = 0;
        int visibleTriangles_ = 0;
        std::size_t drawnPeople_ = 0;
        std::size_t drawnVehicles_ = 0;
        std::size_t drawnProps_ = 0;
        int frameCount_ = 0;
        std::string rendererName_;
        std::string diagnostic_;
        bool screenshotTaken_ = false;
    };
}
