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
#include "FrameWorker.hpp"
#include "Replay.hpp"
#include "Report.hpp"
#include "Simulation.hpp"
#include "SkyLighting.hpp"
#include "TextRenderer.hpp"

#include "CNA/Graphics/AtmosphericSky.hpp"
#include "CNA/Graphics/CascadedShadowMap.hpp"
#include "CNA/Graphics/DebugDraw.hpp"
#include "CNA/Graphics/DepthNormalPrepass.hpp"
#include "CNA/Graphics/DirectionalLightEXT.hpp"
#include "CNA/Graphics/FrustumCullerEXT.hpp"
#include "CNA/Graphics/LodGroupEXT.hpp"
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
     * @brief A quantity painted over the city, on its own key.
     *
     * Separate from @ref Overlay and cycled with `F4` rather than folded into `Tab`, because these
     * answer a different question. The overlays show what the simulation *is* -- the graph, the
     * routes; a heatmap shows what it is *costing*, and the two are worth looking at together.
     *
     * Every one of them is drawn with a scale on the HUD. A heatmap without a legend is a picture
     * of where the red is, which is not information -- the same city looks alarming or fine
     * depending on what the brightest cell happened to be.
     */
    enum class Heatmap : std::uint8_t
    {
        None = 0,
        Traffic,      ///< Mean speed per road segment: red is stopped.
        Density,      ///< People and vehicles per hundred metres of city.
        RenderCost,   ///< Triangles per visible chunk.
        PathCache,    ///< Where the route planner is missing its cache.
        Count
    };

    [[nodiscard]] const char* HeatmapName(Heatmap heatmap);

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
        void DrawDepthNormalPrepass();
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
        SkyLighting skyLight_;
        TextRenderer text_;

        std::unique_ptr<Microsoft::Xna::Framework::Graphics::SpriteBatch> batch_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::PbrEffect> effect_;
        std::unique_ptr<CNA::Graphics::RenderPipeline> pipeline_;
        std::unique_ptr<CNA::Graphics::CascadedShadowMap> shadows_;
        std::unique_ptr<CNA::Graphics::AtmosphericSky> sky_;
        std::unique_ptr<CNA::Graphics::DebugDraw> debug_;
        std::unique_ptr<CNA::Graphics::DepthNormalPrepass> prepass_;

        Camera camera_;
        CameraMode cameraMode_ = CameraMode::Orbit;
        Overlay overlay_ = Overlay::Statistics;
        Heatmap heatmap_ = Heatmap::None;
        /// Scratch for the heatmaps, kept rather than allocated: they are per-segment and
        /// per-cell arrays over a city, rebuilt every frame the overlay is on.
        std::vector<float> heatSpeed_;
        std::vector<std::uint16_t> heatCount_;
        std::vector<std::uint16_t> heatDensity_;
        float heatPeak_ = 0.0f;
        std::string heatLegend_;
        void DrawHeatmap();
        std::uint32_t followAgent_ = kNoIndex;
        bool followSnap_ = true;
        bool followLocked_ = false;
        float followIdleSeconds_ = 0.0f;
        float orbitAngle_ = 0.0f;
        float cinematicTime_ = 0.0f;
        float freeSpeed_ = 60.0f;
        bool paused_ = false;
        bool hudVisible_ = true;
        bool postProcessing_ = true;
        bool mouseLook_ = false;
        bool gpuTiming_ = false;
        bool skyLightRebuiltThisFrame_ = false;
        /// 0 at street level, 1 once the camera is a few metres under it. Gates everything the sky
        /// contributes, because a tunnel roof is not something a shadow cascade can express.
        float undergroundLevel_ = 0.0f;

        Microsoft::Xna::Framework::Input::KeyboardState previousKeys_;
        Microsoft::Xna::Framework::Input::MouseState previousMouse_;

        /// Per-frame culling results. Kept as members so the vectors are not reallocated sixty
        /// times a second.
        CNA::Graphics::FrustumCullerEXT culler_;
        /// The engine's own level selector, one per instanced family. A LOD level here is a
        /// distance and a mesh part; the mesh parts are owned by InstanceRenderer, so these hold
        /// null at every level and are used purely for the *selection* -- which is the part worth
        /// having in one place rather than as three hand-written distance comparisons.
        CNA::Graphics::LodGroupEXT personLod_;
        CNA::Graphics::LodGroupEXT vehicleLod_;
        CNA::Graphics::LodGroupEXT propLod_[kPropKindCount];
        std::vector<std::uint32_t> visibleChunks_;
        CNA::Graphics::DirectionalLightEXT sun_;

        // --- Frame statistics ------------------------------------------------------------------
        double frameMs_ = 0.0;
        double simMs_ = 0.0;
        double shadowMs_ = 0.0;
        double sceneMs_ = 0.0;
        double prepassMs_ = 0.0;
        double instanceMs_ = 0.0;
        double smoothedFrameMs_ = 16.0;
        /// The pipelined frame model: one worker, the step waiting to be started, and how long the
        /// launch and the join actually took. See CityGame::Draw.
        FrameWorker worker_;
        float pendingStepSeconds_ = 0.0f;
        double simLaunchMs_ = 0.0;
        double simJoinMs_ = 0.0;
        int drawCalls_ = 0;
        int visibleTriangles_ = 0;
        std::size_t drawnPeople_ = 0;
        std::size_t drawnVehicles_ = 0;
        std::size_t drawnProps_ = 0;
        std::size_t drawnParked_ = 0;
        std::size_t drawnTrainCars_ = 0;
        std::size_t drawnBuses_ = 0;

        /// Writes a replay of this session when --record was given, and does nothing otherwise.
        /// It is a member rather than a pointer because "not recording" is the common case and a
        /// closed recorder is already the do-nothing one.
        ReplayRecorder recorder_;
        bool savedSnapshot_ = false;

        // --- The scripted render benchmark ------------------------------------------------------
        //
        // `--report` measures the renderer by driving the camera through a fixed set of viewpoints
        // rather than by asking somebody to stand in the right place. The set is small and chosen
        // for contrast: an overview where the shadow cascades dominate, a street where the
        // simulation does, and a junction where neither does.
        struct RenderProbe
        {
            const char* name;
            CameraMode camera;
            Microsoft::Xna::Framework::Vector3 position;
            float yaw;
            float pitch;
        };
        int reportProbe_ = -1;      ///< Index into the probe table; -1 until the first frame.
        int reportFrame_ = 0;       ///< Frames spent on the current probe, warm-up included.
        RenderingRow reportAccum_;  ///< Running totals for the probe being measured.
        std::vector<RenderingRow> renderingRows_;
        std::vector<PassRow> passRows_;
        void StepRenderReport();

    public:
        /** @brief What the scripted render benchmark measured. Empty unless --report ran one. */
        [[nodiscard]] const std::vector<RenderingRow>& renderingRows() const { return renderingRows_; }
        [[nodiscard]] const std::vector<PassRow>& passRows() const { return passRows_; }
        /** @brief What the device called itself, for the report's machine description. */
        [[nodiscard]] const std::string& rendererName() const { return rendererName_; }

    private:
        /// Writes the replay if one is being recorded. Idempotent; called from both exit paths.
        void FinishRecording();

        /// True for the materials that exist only inside the metro tunnels.
        ///
        /// Eleven metres of earth is between them and everything else, so from any camera above
        /// ground they are invisible and they cast nothing -- but the tunnels run through most of
        /// the city's chunks, so every pass was paying four extra draw calls per visible chunk,
        /// four times over in the shadow cascades, for geometry that could not appear.
        [[nodiscard]] static bool IsUnderground(int material)
        {
            return material == static_cast<int>(CityMaterial::MetroTunnel) ||
                   material == static_cast<int>(CityMaterial::MetroFloor) ||
                   material == static_cast<int>(CityMaterial::MetroRail) ||
                   material == static_cast<int>(CityMaterial::TunnelLight);
        }

        /// Which half of the city the follow camera was asked for on the command line.
        [[nodiscard]] Simulation::Focus FollowFocus() const
        {
            return options_.followMetro   ? Simulation::Focus::Metro
                   : options_.followBus   ? Simulation::Focus::Bus
                                          : Simulation::Focus::Anybody;
        }
        int frameCount_ = 0;
        std::string rendererName_;
        std::string diagnostic_;
        bool screenshotTaken_ = false;
    };
}
