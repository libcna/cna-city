// SPDX-License-Identifier: MIT
#include "CityGame.hpp"

#include "Snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Palette.hpp"
#include "CNA/Graphics/RenderPipelineSettings.hpp"
#include "CNA/Graphics/RenderQuality.hpp"
#include "CNA/Graphics/ShadowQuality.hpp"
#include "CNA/Graphics/TonemappingMode.hpp"
#include "Microsoft/Xna/Framework/Graphics/ShaderEffect.hpp"
#include "CNA/GraphicsCapability.hpp"
#include "Microsoft/Xna/Framework/BoundingFrustum.hpp"
#include "Microsoft/Xna/Framework/GameTime.hpp"
#include "Microsoft/Xna/Framework/GameWindow.hpp"
#include "Microsoft/Xna/Framework/Graphics/BlendState.hpp"
#include "Microsoft/Xna/Framework/Graphics/DepthStencilState.hpp"
#include "Microsoft/Xna/Framework/Graphics/RasterizerState.hpp"
#include "Microsoft/Xna/Framework/Graphics/SamplerState.hpp"
#include "Microsoft/Xna/Framework/Graphics/SpriteSortMode.hpp"
#include "Microsoft/Xna/Framework/Graphics/SurfaceFormat.hpp"
#include "Microsoft/Xna/Framework/Graphics/Texture2D.hpp"
#include "Microsoft/Xna/Framework/Input/ButtonState.hpp"
#include "Microsoft/Xna/Framework/Input/Keys.hpp"
#include "Microsoft/Xna/Framework/Rectangle.hpp"
#include "System/Diagnostics/Stopwatch.hpp"
#include "System/NotSupportedException.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;
using namespace Microsoft::Xna::Framework::Input;
using CNA::Graphics::AtmosphericSky;
using CNA::Graphics::CascadedShadowMap;
using CNA::Graphics::DebugDraw;
using CNA::Graphics::DepthNormalPrepass;
using CNA::Graphics::RenderPipeline;
using CNA::Graphics::RenderQuality;
using CNA::Graphics::ShadowQuality;
using CNA::Graphics::TonemappingMode;

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        inline double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }

        /// A person's world transform: yaw, plus a vertical bob taken from their stride. The bob
        /// is two centimetres and it is the difference between a crowd that walks and a crowd that
        /// slides.
        Matrix PersonTransform(Vec2 position, float heading, float phase, float speed)
        {
            const float bob = std::sin(phase * 2.0f) * 0.022f * Saturate(speed);
            return Matrix::CreateRotationY(-heading + kPi * 0.5f) *
                   Matrix::CreateTranslation(position.X, bob, position.Y);
        }

        Matrix VehicleTransform(Vec2 position, float heading)
        {
            return Matrix::CreateRotationY(-heading) *
                   Matrix::CreateTranslation(position.X, 0.0f, position.Y);
        }
    }

    CityGame::CityGame(const CliOptions& options) : options_(options), graphics_(this)
    {
        graphics_.setPreferredBackBufferWidthProperty(options_.windowWidth);
        graphics_.setPreferredBackBufferHeightProperty(options_.windowHeight);
        graphics_.setSynchronizeWithVerticalRetraceProperty(options_.vsync);
        if (options_.fullScreen) graphics_.setIsFullScreenProperty(true);
        // A demo whose frame rate is capped by the harness reports the cap rather than the engine,
        // which is the one number this program exists to produce.
        setIsFixedTimeStepProperty(false);
        setIsMouseVisibleProperty(true);
        cameraMode_ = options_.camera;
        overlay_ = static_cast<Overlay>(Clamp(options_.overlay, 0, static_cast<int>(Overlay::Count) - 1));
        postProcessing_ = !options_.noPost;
        heatmap_ = static_cast<Heatmap>(Clamp(options_.heatmap, 0, static_cast<int>(Heatmap::Count) - 1));
    }

    CityGame::~CityGame()
    {
        // Closed here rather than only in UnloadContent, because a run that ends on `--frames`
        // does not necessarily get an UnloadContent -- and a replay file that is empty because the
        // program exited the wrong way is worse than no replay file at all. Close is idempotent,
        // so the tidy path still writes it at the tidy moment; members are still alive in a
        // destructor body, so the simulation the final checkpoint needs is here.
        FinishRecording();
    }

    void CityGame::StepRenderReport()
    {
        // A fixed tour, chosen for contrast rather than for coverage: an overview where the four
        // shadow cascades and the visible chunk count dominate, a street where the simulation is
        // the larger half even with almost nothing on screen, and a junction where neither is.
        // Which of the two halves dominates depending on where the camera is *is* the result this
        // program exists to produce, so the tour has to include both ends of it.
        static const RenderProbe kProbes[] = {
            {"city overview", CameraMode::Free, Vector3(0.0f, 400.0f, 900.0f), -1.5708f, -0.42f},
            {"downtown skyline", CameraMode::Orbit, Vector3(0.0f, 0.0f, 0.0f), 0.0f, 0.0f},
            {"street level", CameraMode::Free, Vector3(120.0f, 6.0f, 40.0f), 0.35f, -0.05f},
            {"a signalised junction", CameraMode::Free, Vector3(-470.0f, 12.0f, 20.0f), 1.9f, -0.30f},
        };
        constexpr int kProbeCount = static_cast<int>(std::size(kProbes));

        const int warmUp = std::max(30, options_.reportFrames / 2);
        const int measured = std::max(30, options_.reportFrames);

        if (reportProbe_ < 0 || reportFrame_ >= warmUp + measured)
        {
            // Bank the probe that just finished.
            if (reportProbe_ >= 0 && reportProbe_ < kProbeCount)
            {
                const double frames = static_cast<double>(measured);
                RenderingRow row = reportAccum_;
                row.view = kProbes[reportProbe_].name;
                row.width = options_.windowWidth;
                row.height = options_.windowHeight;
                row.quality = QualityName(options_.quality);
                row.frameMs /= frames;
                row.simulationMs /= frames;
                row.drawMs /= frames;
                row.shadowMs /= frames;
                row.prepassMs /= frames;
                row.sceneMs /= frames;
                row.instanceMs /= frames;
                row.drawCalls = static_cast<std::uint32_t>(row.drawCalls / frames);
                row.triangles = static_cast<std::uint32_t>(row.triangles / frames);
                renderingRows_.push_back(row);

                // The GPU's own view of the post chain, from CNA's timer queries rather than from
                // this program's stopwatch: a CPU-side timer around a draw call measures when the
                // driver returned, not when the work finished.
                if (pipeline_ != nullptr)
                    for (const auto& pass : pipeline_->getPassTimingsEXT())
                        passRows_.push_back(
                            PassRow{kProbes[reportProbe_].name, pass.Name, pass.Milliseconds});
            }

            ++reportProbe_;
            reportFrame_ = 0;
            reportAccum_ = RenderingRow{};
            if (reportProbe_ >= kProbeCount)
            {
                Exit();
                return;
            }

            const RenderProbe& probe = kProbes[reportProbe_];
            cameraMode_ = probe.camera;
            if (probe.camera == CameraMode::Free)
            {
                camera_.position = probe.position;
                camera_.yaw = probe.yaw;
                camera_.pitch = probe.pitch;
            }
            if (pipeline_ != nullptr) pipeline_->setGpuTimingEnabledEXT(true);
            gpuTiming_ = true;
            std::printf("  %-22s ", probe.name);
            std::fflush(stdout);
        }

        if (reportFrame_ >= warmUp)
        {
            reportAccum_.frameMs += smoothedFrameMs_;
            reportAccum_.simulationMs += simMs_;
            reportAccum_.drawMs += frameMs_;
            reportAccum_.shadowMs += shadowMs_;
            reportAccum_.prepassMs += prepassMs_;
            reportAccum_.sceneMs += sceneMs_;
            reportAccum_.instanceMs += instanceMs_;
            reportAccum_.drawCalls += static_cast<std::uint32_t>(drawCalls_);
            reportAccum_.triangles += static_cast<std::uint32_t>(visibleTriangles_);
        }
        if (reportFrame_ == warmUp + measured - 1)
            std::printf("%.1f ms\n", reportAccum_.frameMs / std::max(1, measured));
        ++reportFrame_;
    }

    void CityGame::FinishRecording()
    {
        if (!options_.savePath.empty() && !savedSnapshot_)
        {
            savedSnapshot_ = true;
            std::string error;
            if (SaveSnapshot(options_.savePath, sim_, options_.snapshotNote, error))
                std::printf("cna-city: wrote %s\n", options_.savePath.c_str());
            else
                std::fprintf(stderr, "cna-city: %s\n", error.c_str());
        }
        if (!recorder_.recording()) return;
        recorder_.Close(sim_);
        if (recorder_.error().empty())
            std::printf("cna-city: wrote %s\n", options_.recordPath.c_str());
        else
            std::fprintf(stderr, "cna-city: %s\n", recorder_.error().c_str());
    }

    void CityGame::Initialize()
    {
        Game::Initialize();
        getWindowProperty().setTitleProperty("CNA City -- 100 000 citizens");
    }

    void CityGame::ApplyQuality()
    {
        if (pipeline_ == nullptr) return;
        auto& settings = pipeline_->getSettings();

        // Everything below is a bundle rather than a slider, because the passes are not
        // independent: SSAO without the depth-normal prepass is a no-op, bloom without HDR has
        // nothing above white to bloom from, and a shadow cascade count that outruns the atlas
        // costs resolution rather than buying range.
        // Every setting is named, including the ones being switched *off*.
        //
        // Not because the defaults are wrong -- `RenderPipelineSettings` starts every optional
        // pass at zero or false, which was checked rather than assumed -- but because the quality
        // levels are re-applied and a level that only names what it wants would inherit whatever
        // the previous one left behind. Naming all of them is also the cheapest possible answer to
        // "which passes are actually on in this frame".
        settings.setHDREnabled(true);
        settings.setTonemappingMode(TonemappingMode::Aces);
        settings.setExposure(1.0f);
        settings.setGamma(2.2f);
        settings.setShadowsEnabled(true);
        settings.setChromaticAberrationStrength(0.0f);
        settings.setFilmGrainIntensity(0.0f);
        settings.setLensFlareIntensity(0.0f);
        settings.setLightShaftIntensity(0.0f);
        settings.setColorGradeEnabled(false);
        settings.setDOFEnabled(false);
        settings.setMotionBlurStrength(0.0f);
        settings.setSSREnabled(false);
        settings.setSSAOEnabled(false);
        settings.setBloomEnabled(false);
        settings.setFXAAEnabled(false);

        switch (options_.quality)
        {
            case Quality::Low:
                settings.setRenderQuality(RenderQuality::Low);
                settings.setShadowQuality(ShadowQuality::Low);
                settings.setBloomEnabled(false);
                settings.setFXAAEnabled(false);
                settings.setSSAOEnabled(false);
                settings.setHeightFogDensity(0.0f);
                break;
            case Quality::Medium:
                settings.setRenderQuality(RenderQuality::Medium);
                settings.setShadowQuality(ShadowQuality::Medium);
                settings.setBloomEnabled(true);
                settings.setBloomIntensity(0.7f);
                settings.setBloomThreshold(1.05f);
                settings.setFXAAEnabled(true);
                settings.setSSAOEnabled(false);
                break;
            case Quality::High:
                settings.setRenderQuality(RenderQuality::High);
                settings.setShadowQuality(ShadowQuality::High);
                settings.setBloomEnabled(true);
                settings.setBloomIntensity(0.85f);
                settings.setBloomThreshold(1.0f);
                settings.setBloomIterations(5);
                settings.setFXAAEnabled(true);
                settings.setSSAOEnabled(true);
                // The radius is a UV offset compared against normalised depths, not a world
                // distance. The stock default of 0.5 applies half the frame as an offset, which
                // darkens everything a little and is a global dimmer wearing occlusion's name;
                // below about 0.15 the samples cannot clear the depth bias and the term vanishes
                // entirely. A quarter is the middle of the usable band.
                settings.setSSAORadius(0.24f);
                settings.setSSAOIntensity(0.70f);
                settings.setLightShaftThreshold(0.82f);
                break;
            case Quality::Ultra:
                settings.setRenderQuality(RenderQuality::Ultra);
                settings.setShadowQuality(ShadowQuality::Ultra);
                settings.setBloomEnabled(true);
                settings.setBloomIntensity(0.95f);
                settings.setBloomThreshold(0.95f);
                settings.setBloomIterations(6);
                settings.setFXAAEnabled(true);
                settings.setSSAOEnabled(true);
                settings.setSSAORadius(0.24f);
                settings.setSSAOIntensity(0.85f);
                settings.setSSAOSampleCount(16);
                settings.setLightShaftThreshold(0.78f);
                settings.setLensFlareIntensity(0.12f);
                settings.setLensFlareThreshold(1.15f);
                // Both of these are measured in screen widths, not in "a bit". A quarter of a
                // screen width of chromatic aberration is not a lens artefact, it is three copies
                // of the city offset from each other, which is exactly what the first ultra frame
                // rendered.
                settings.setChromaticAberrationStrength(0.0016f);
                settings.setFilmGrainIntensity(0.015f);
                break;
        }
    }

    void CityGame::LoadContent()
    {
        GraphicsDevice& device = getGraphicsDeviceProperty();
        rendererName_ = std::string(device.GetGraphicsRendererName());

        System::Diagnostics::Stopwatch watch;
        watch.Start();
        if (options_.loadPath.empty())
        {
            sim_.Initialize(options_.sim);
        }
        else
        {
            // A snapshot carries its own city, so this replaces the configuration rather than
            // adding to it -- which is also why the camera and quality flags still apply and the
            // seed and population ones do not.
            std::string error;
            if (LoadSnapshot(options_.loadPath, sim_, error))
                std::printf("cna-city: loaded %s\n", options_.loadPath.c_str());
            else
            {
                std::fprintf(stderr, "cna-city: %s\n", error.c_str());
                sim_.Initialize(options_.sim);
            }
        }
        watch.Stop();

        // Opened before the first frame, so a session that cannot write its replay says so now
        // rather than after somebody has spent twenty minutes producing one.
        if (!options_.recordPath.empty() && !recorder_.Open(options_.recordPath, options_.sim))
            std::fprintf(stderr, "cna-city: %s\n", recorder_.error().c_str());
        std::printf("cna-city: simulation ready in %.0f ms -- %u citizens, %zu buildings, %.1f km of road\n",
                    ElapsedMs(watch), options_.sim.agentCount, sim_.city().buildings().size(),
                    sim_.city().roads().TotalLength() / 1000.0);

        watch.Restart();
        materials_.Build(device, options_.sim.city.seed);
        geometry_.Build(device, materials_, sim_.city(), sim_.metro(), options_.sim.city.seed);
        instances_.Build(device, materials_, options_.sim.city.seed);
        watch.Stop();
        std::printf("cna-city: geometry ready in %.0f ms -- %d triangles in %zu chunks, %.1f MB of "
                    "buffers, %.1f MB of textures\n",
                    ElapsedMs(watch), geometry_.totalTriangles(), geometry_.chunks().size(),
                    static_cast<double>(geometry_.bytes()) / (1024.0 * 1024.0),
                    static_cast<double>(materials_.textureBytes()) / (1024.0 * 1024.0));

        batch_ = std::make_unique<SpriteBatch>(device);
        effect_ = std::make_unique<PbrEffect>(device);
        text_.Load(device);

        pipeline_ = std::make_unique<RenderPipeline>(device);
        pipeline_->resize(device.getViewportProperty().getWidthProperty(),
                          device.getViewportProperty().getHeightProperty());
        ApplyQuality();

        // Four cascades over a city-scale view. Anything fewer and a pedestrian's shadow at the
        // camera and a tower's shadow a kilometre away have to share one texel density, which is a
        // choice between soft mush near and no shadows far.
        if (device.SupportsShadowSamplingEXT())
        {
            const ShadowQuality quality = options_.quality == Quality::Low ? ShadowQuality::Low
                                        : options_.quality == Quality::Medium ? ShadowQuality::Medium
                                                                              : ShadowQuality::High;
            shadows_ = std::make_unique<CascadedShadowMap>(device, quality, 4);
            if (!shadows_->isSupported())
            {
                shadows_.reset();
                diagnostic_ = "cascaded shadows unavailable on this renderer";
            }
            else
            {
                // The split lambda trades near detail against far coverage. 0.82 is close to fully
                // logarithmic, which is what a camera that can be on a pavement or above the
                // rooftops needs.
                shadows_->setSplitLambda(0.82f);
                shadows_->setBlendBand(0.12f);
            }
        }
        else
        {
            diagnostic_ = "this renderer does not sample shadow maps";
        }

        // The depth/normal prepass SSAO reads. It is a *contract*, not a switch: the pass has to
        // be filled by the game, with the prepass's own effect, before the pipeline is begun.
        if (pipeline_->getSettings().isSSAOEnabled())
        {
            prepass_ = std::make_unique<DepthNormalPrepass>(
                device, device.getViewportProperty().getWidthProperty(),
                device.getViewportProperty().getHeightProperty());
            if (!prepass_->isSupported(device))
            {
                prepass_.reset();
                pipeline_->getSettings().setSSAOEnabled(false);
                if (diagnostic_.empty())
                    diagnostic_ = "no depth/normal prepass on this renderer -- ambient occlusion off";
            }
        }

        // Ambient light from the sky the frame is actually drawing, rather than from a constant.
        if (!skyLight_.Build(device) && diagnostic_.empty())
            diagnostic_ = skyLight_.diagnostic();

        sky_ = std::make_unique<AtmosphericSky>(device);
        if (!sky_->isSupported())
        {
            sky_.reset();
            if (diagnostic_.empty()) diagnostic_ = "the analytic sky needs shader-source execution";
        }
        debug_ = std::make_unique<DebugDraw>(device);

        if (!instances_.instancingSupported() && !instances_.diagnostic().empty())
            diagnostic_ = instances_.diagnostic();

        // The level tables. `LodGroupEXT` owns the *selection* -- a sorted distance list and a
        // binary search -- while the meshes it would normally hand back are owned per colour
        // bucket by InstanceRenderer, so every level here is registered with a null part and the
        // index is what gets used.
        //
        // Hysteresis stays at zero, deliberately. It is per-object state: the group remembers the
        // level it last returned so a single object hovering on a boundary does not flicker. One
        // group shared by a hundred thousand pedestrians would carry the previous agent's level
        // into the next agent's decision, which is not damping, it is noise.
        personLod_.clear();
        personLod_.addLevel(65.0f, nullptr);      // Near: legs, arms, a walk cycle
        personLod_.addLevel(230.0f, nullptr);     // Mid: torso and head
        personLod_.addLevel(620.0f, nullptr);     // Far: one box
        personLod_.setHysteresis(0.0f);
        vehicleLod_.clear();
        vehicleLod_.addLevel(900.0f, nullptr);
        vehicleLod_.setHysteresis(0.0f);
        {
            const float propRange[kPropKindCount] = {330.0f, 190.0f, 520.0f, 520.0f,
                                                     140.0f, 110.0f, 260.0f, 300.0f};
            for (int kind = 0; kind < kPropKindCount; ++kind)
            {
                propLod_[kind].clear();
                propLod_[kind].addLevel(propRange[kind], nullptr);
                propLod_[kind].setHysteresis(0.0f);
            }
        }

        camera_.aspect = static_cast<float>(device.getViewportProperty().getWidthProperty()) /
                         std::max(1.0f, static_cast<float>(device.getViewportProperty().getHeightProperty()));
        const float half = sim_.city().config().halfSize;
        camera_.position = Vector3(0.0f, half * 0.42f, half * 0.95f);
        camera_.LookAt(Vector3(0.0f, 40.0f, 0.0f));
    }

    void CityGame::UnloadContent()
    {
        FinishRecording();
        debug_.reset();
        skyLight_.Release();
        prepass_.reset();
        sky_.reset();
        shadows_.reset();
        pipeline_.reset();
        effect_.reset();
        batch_.reset();
        text_.Unload();
        instances_.Release();
        geometry_.Release();
        materials_.Release();
    }

    void CityGame::HandleInput(float dt)
    {
        const KeyboardState keys = Keyboard::GetState();
        const MouseState mouse = Mouse::GetState();
        const auto pressed = [&](Keys key) {
            return keys.IsKeyDown(key) && previousKeys_.IsKeyUp(key);
        };

        if (pressed(Keys::Escape)) Exit();
        if (pressed(Keys::D1)) cameraMode_ = CameraMode::Free;
        if (pressed(Keys::D2)) cameraMode_ = CameraMode::Orbit;
        if (pressed(Keys::D3)) cameraMode_ = CameraMode::Follow;
        if (pressed(Keys::D4)) cameraMode_ = CameraMode::Street;
        if (pressed(Keys::D5)) cameraMode_ = CameraMode::Cinematic;
        if (pressed(Keys::P)) paused_ = !paused_;
        if (pressed(Keys::F1)) hudVisible_ = !hudVisible_;
        if (pressed(Keys::F2)) postProcessing_ = !postProcessing_;
        // F4 cycles the heatmap, which is its own layer rather than another Tab stop: an overlay
        // says what the simulation is and a heatmap says what it costs, and the two are worth
        // looking at together.
        if (pressed(Keys::F4))
            heatmap_ = static_cast<Heatmap>((static_cast<int>(heatmap_) + 1) %
                                            static_cast<int>(Heatmap::Count));
        if (pressed(Keys::F3) && pipeline_ != nullptr)
        {
            // GPU timing is off by default because it costs a query per pass and, on a driver
            // without disjoint timer queries, silently reports nothing. It is a key rather than a
            // setting so the cost is only paid while somebody is looking at the numbers.
            gpuTiming_ = !gpuTiming_;
            pipeline_->setGpuTimingEnabledEXT(gpuTiming_);
        }
        if (pressed(Keys::Tab))
            overlay_ = static_cast<Overlay>((static_cast<int>(overlay_) + 1) %
                                            static_cast<int>(Overlay::Count));
        if (pressed(Keys::N))
        {
            followAgent_ = sim_.PickInterestingAgent(static_cast<std::uint32_t>(frameCount_), FollowFocus());
            followSnap_ = true;
            followLocked_ = false;
            cameraMode_ = CameraMode::Follow;
        }
        if (pressed(Keys::L))
        {
            // Locking is what makes "watch one citizen's entire day" possible at all. Unlocked,
            // the camera moves on from anybody who stays indoors, which is the right default --
            // most people are indoors most of the time and a chase camera pointed at a front door
            // is not a demonstration. Locked, it waits outside that door instead.
            followLocked_ = !followLocked_;
            cameraMode_ = CameraMode::Follow;
        }
        if (pressed(Keys::F))
        {
            // Cycling by hand pins the weather: somebody who asked for snow wants to look at snow,
            // not at whatever the forecast wanders into ninety seconds later.
            const int next = (static_cast<int>(sim_.weather().kind()) + 1) % kWeatherKindCount;
            sim_.mutableWeather().Force(static_cast<WeatherKind>(next));
            sim_.mutableWeather().SetRandomChanges(false);
            recorder_.RecordWeather(sim_.tick(), static_cast<WeatherKind>(next));
        }
        if (keys.IsKeyDown(Keys::T) || keys.IsKeyDown(Keys::G))
        {
            const float direction = keys.IsKeyDown(Keys::T) ? 1.0f : -1.0f;
            sim_.mutableClock().setHour(sim_.clock().hour() + direction * dt * 1.4f);
            // Recorded every frame the key is held, which is a lot of events for a long scrub --
            // and correct, because winding the clock is a continuous input and a replay that
            // sampled it would put the city in a different hour than the one that was watched.
            recorder_.RecordHour(sim_.tick(), sim_.clock().hour());
        }
        if (pressed(Keys::OemOpenBrackets))
            sim_.mutableClock().setTimeScale(std::max(1.0f, sim_.clock().timeScale() * 0.5f));
        if (pressed(Keys::OemCloseBrackets))
            sim_.mutableClock().setTimeScale(std::min(600.0f, sim_.clock().timeScale() * 2.0f));

        // Free-camera movement. The speed scales with the wheel and with height, because a camera
        // four hundred metres up that moves at walking pace is unusable.
        if (cameraMode_ == CameraMode::Free)
        {
            const int wheel = mouse.getScrollWheelValueProperty() -
                              previousMouse_.getScrollWheelValueProperty();
            if (wheel != 0) freeSpeed_ = Clamp(freeSpeed_ * (wheel > 0 ? 1.35f : 0.74f), 2.0f, 900.0f);

            const float speed = freeSpeed_ * (keys.IsKeyDown(Keys::LeftShift) ? 4.0f : 1.0f) *
                                (0.35f + camera_.position.Y / 220.0f);
            const Vector3 forward = camera_.Forward();
            const Vector3 right = camera_.Right();
            Vector3 move(0.0f, 0.0f, 0.0f);
            if (keys.IsKeyDown(Keys::W)) move = Vector3(move.X + forward.X, move.Y + forward.Y, move.Z + forward.Z);
            if (keys.IsKeyDown(Keys::S)) move = Vector3(move.X - forward.X, move.Y - forward.Y, move.Z - forward.Z);
            if (keys.IsKeyDown(Keys::D)) move = Vector3(move.X + right.X, move.Y, move.Z + right.Z);
            if (keys.IsKeyDown(Keys::A)) move = Vector3(move.X - right.X, move.Y, move.Z - right.Z);
            if (keys.IsKeyDown(Keys::E)) move = Vector3(move.X, move.Y + 1.0f, move.Z);
            if (keys.IsKeyDown(Keys::Q)) move = Vector3(move.X, move.Y - 1.0f, move.Z);
            const float length = std::sqrt(move.X * move.X + move.Y * move.Y + move.Z * move.Z);
            if (length > 1e-4f)
            {
                const float step = speed * dt / length;
                camera_.position = Vector3(camera_.position.X + move.X * step,
                                            camera_.position.Y + move.Y * step,
                                            camera_.position.Z + move.Z * step);
            }
            camera_.position.Y = Clamp(camera_.position.Y, 1.6f, 1400.0f);
        }

        // Mouse look while the left button is held, in every mode that has a free look.
        const bool dragging = mouse.getLeftButtonProperty() == ButtonState::Pressed;
        if (dragging && mouseLook_ &&
            (cameraMode_ == CameraMode::Free || cameraMode_ == CameraMode::Street))
        {
            const float dx = static_cast<float>(mouse.getXProperty() - previousMouse_.getXProperty());
            const float dy = static_cast<float>(mouse.getYProperty() - previousMouse_.getYProperty());
            camera_.yaw += dx * 0.0042f;
            camera_.pitch = Clamp(camera_.pitch - dy * 0.0042f, -1.45f, 1.45f);
        }
        mouseLook_ = dragging;

        previousKeys_ = keys;
        previousMouse_ = mouse;
    }

    void CityGame::UpdateCamera(float dt)
    {
        const float half = sim_.city().config().halfSize;
        switch (cameraMode_)
        {
            case CameraMode::Free:
                break;

            case CameraMode::Orbit:
            {
                orbitAngle_ += dt * 0.045f;
                const float radius = half * 0.62f;
                const Vector3 wanted(std::cos(orbitAngle_) * radius, half * 0.24f,
                                     std::sin(orbitAngle_) * radius);
                camera_.EaseTo(wanted, dt, 0.35f);
                camera_.LookAt(Vector3(0.0f, 42.0f, 0.0f));
                break;
            }

            case CameraMode::Follow:
            {
                if (followAgent_ == kNoIndex || followAgent_ >= sim_.agents().size())
                {
                    // Picked lazily rather than at load: the list of people currently outdoors is
                    // built by the first simulation step, so asking before then reliably returned
                    // somebody sitting indoors and the camera hung in the sky above them.
                    followAgent_ = sim_.PickInterestingAgent(static_cast<std::uint32_t>(frameCount_), FollowFocus());
                    followSnap_ = true;
                }
                if (followAgent_ == kNoIndex) break;

                // A citizen who has gone indoors and stayed there is not worth watching, and this
                // camera exists to watch somebody's day rather than their front door. After a few
                // seconds of nothing happening it moves on -- which is also what stops the mode
                // from opening on a wall, because the first person it is handed is as likely to be
                // asleep as on their way to work.
                const auto currentMode = static_cast<Mode>(sim_.agents().mode[followAgent_]);
                // `--follow-metro` means "show me the underground", so a subject who has surfaced
                // and is now walking down a street is no longer the subject it asked for. Without
                // this the flag picks a passenger once and then follows them for the rest of their
                // day above ground, which is the ordinary follow camera with extra steps.
                const bool wrongPlace =
                    (options_.followMetro && currentMode != Mode::Riding &&
                     currentMode != Mode::WaitingTrain) ||
                    (options_.followBus && currentMode != Mode::OnBus &&
                     currentMode != Mode::WaitingBus);
                followIdleSeconds_ =
                    (currentMode == Mode::Indoors || wrongPlace) ? followIdleSeconds_ + dt : 0.0f;
                // Short, because `--follow-metro` and `--follow-bus` mean "show me that", and a
                // subject who has finished their ride is now an ordinary pedestrian who happens
                // to be the one the camera is holding.
                const float patience = wrongPlace ? 0.5f : 2.5f;
                if (!followLocked_ && followIdleSeconds_ > patience)
                {
                    followAgent_ = sim_.PickInterestingAgent(
                        static_cast<std::uint32_t>(frameCount_ * 7 + 13), FollowFocus());
                    followIdleSeconds_ = 0.0f;
                    followSnap_ = true;
                    if (followAgent_ == kNoIndex) break;
                }

                const Vector3 subject = sim_.AgentWorldPosition(followAgent_);
                const auto mode = static_cast<Mode>(sim_.agents().mode[followAgent_]);
                // Behind and above the shoulder, and closer when the subject is on foot than when
                // they are in a car or on a train.
                const bool riding = mode == Mode::Riding;
                const bool waiting = mode == Mode::WaitingTrain;
                const bool underground = riding || waiting;
                // A passenger standing on a platform faces the track, so "behind them" is through
                // the back wall of the station -- 10 m of it. The camera stands beside them
                // instead and looks along the platform, which is also the only angle from which a
                // platform looks like a platform: the crowd in a row, the edge, the track beyond.
                // A bus stop is filmed like a pavement, because it is one; a bus is filmed from
                // further back than a car, because it is eleven metres longer.
                const float distance = mode == Mode::Walking    ? 6.5f
                                     : mode == Mode::Indoors    ? 11.0f
                                     : mode == Mode::WaitingBus ? 7.0f
                                     : mode == Mode::OnBus      ? 17.0f
                                     : waiting                  ? -7.5f
                                     : riding                   ? 10.5f
                                                                : 13.0f;
                // Underground the offset has to stay *inside* the tunnel. The running tunnel's
                // roof is 3.4 m above the track, so the 4.6 m a road vehicle gets puts the lens in
                // the earth above it looking down through the roof at the skyline -- which is what
                // the first underground frames showed.
                const float height = mode == Mode::Walking    ? 2.6f
                                   : mode == Mode::Indoors    ? 6.0f
                                   : mode == Mode::WaitingBus ? 2.4f
                                   : mode == Mode::OnBus      ? 5.4f
                                   : waiting                  ? 1.70f
                                   : riding                   ? 1.55f
                                                              : 4.6f;
                // Indoors the subject's heading is stale, so the camera swings off the doorway's
                // own axis instead: standing behind a citizen who is not facing anywhere puts the
                // lens inside the building they just walked into.
                const float heading = mode == Mode::Indoors
                                          ? sim_.agents().heading[followAgent_] + 2.2f
                                          : sim_.agents().heading[followAgent_];
                Vector3 wanted(subject.X - std::cos(heading) * distance, subject.Y + height,
                               subject.Z - std::sin(heading) * distance);
                if (mode == Mode::OnBus)
                {
                    // Out of the traffic lane and onto the pavement side, so the shot is the bus's
                    // flank going past the shops rather than the back of a box.
                    const float right = heading + 1.5708f;
                    wanted = Vector3(wanted.X + std::cos(right) * 5.5f, wanted.Y,
                                     wanted.Z + std::sin(right) * 5.5f);
                }
                if (underground && sim_.MetroCameraPoint(followAgent_, distance,
                                                          waiting ? 0.0f : 2.6f, height, wanted))
                {
                    // Placed off the track's own geometry; nothing more to do to it.
                }
                else if (underground)
                {
                    // Beside the train rather than behind it. A carriage is eighteen metres long,
                    // so a camera twelve metres back in a tunnel is looking at the flat end of one
                    // and nothing else; step out of the four-foot and you get its lit flank
                    // running past you, which is what riding a metro looks like.
                    //
                    // 2.6 m and not more, on either side. The offset is taken from the subject's
                    // heading, and a train's heading reverses when it turns round at a terminus,
                    // so this lands on the platform side of the track half the time and on the
                    // walkway side the other half. The walkway side has a wall 3.1 m out, so an
                    // offset chosen to reach the platform puts the lens in the concrete every
                    // other trip.
                    //
                    // On a platform the same offset runs *along* the platform, which is 84 m
                    // long, so it can be as large as the shot wants without leaving the station.
                    const float right = heading + 1.5708f;
                    const float lateral = waiting ? 6.5f : 2.6f;
                    wanted = Vector3(wanted.X + std::cos(right) * lateral, wanted.Y,
                                     wanted.Z + std::sin(right) * lateral);
                }

                // Pull the camera out of whatever it is standing in. A pedestrian walks along a
                // pavement with a building right behind them, so a chase camera at a fixed
                // distance is inside that building about half the time; the fix is to walk the
                // offset back toward the subject and, failing that, to rise above the roof.
                //
                // Only above ground. `BuildingHeightAt` answers 0 where there is no building, and
                // 0 is *higher* than anything underground -- so following a passenger on a train
                // the loop decided the camera was buried, climbed out step by step, and surfaced
                // in a field. The one place the subject can be is the one place there is nothing
                // to collide with.
                for (int step = 0; step < 6 && wanted.Y > 0.5f; ++step)
                {
                    const float roof = sim_.city().BuildingHeightAt(ToGround(wanted));
                    if (roof < wanted.Y - 0.4f) break;
                    const float shrink = 1.0f - 0.20f * static_cast<float>(step + 1);
                    wanted = Vector3(subject.X + (wanted.X - subject.X) * shrink,
                                     subject.Y + height + static_cast<float>(step) * 1.6f,
                                     subject.Z + (wanted.Z - subject.Z) * shrink);
                }
                if (followSnap_)
                {
                    camera_.position = wanted;
                    followSnap_ = false;
                }
                else
                {
                    // Eased in *simulated* seconds, not real ones. The half-life was 0.28 s of
                    // wall clock while the world runs at sixty times real time, so a metro train
                    // at 21 m/s covers 1 260 m of world for every second the camera spends
                    // catching up -- and the shot was reliably half a kilometre of empty tunnel
                    // with a train the size of a pixel at the end of it. Following a moving
                    // subject is a simulated-time problem and has to be measured in the
                    // simulation's clock.
                    camera_.EaseTo(wanted, dt * sim_.clock().timeScale(),
                                   mode == Mode::Indoors ? 0.9f : 0.28f);
                }
                camera_.LookAt(Vector3(subject.X, subject.Y + 1.2f, subject.Z));
                break;
            }

            case CameraMode::Street:
            {
                // Standing on a pavement corner at eye height, panning slowly. Chosen once and
                // then left alone: the whole point of this mode is that the city moves and the
                // camera does not.
                //
                // The offset off the junction matters. Parking the camera on the node itself puts
                // it in the middle of a crossroads looking down at the tarmac from five metres up,
                // which is the one place on a street where you can see least.
                static Vec2 spot(0.0f, 0.0f);
                static Vec2 watching(0.0f, 0.0f);
                if (LengthSq(spot) < 1e-6f && !sim_.city().roads().nodes().empty())
                {
                    const auto& nodes = sim_.city().roads().nodes();
                    // A signalised junction, because it is the most interesting thing on a street
                    // to stand and watch: the queue builds, the phase turns, the queue discharges.
                    // Only about one junction in twelve here has lights, so a uniformly chosen
                    // node almost never does.
                    std::uint32_t chosen = static_cast<std::uint32_t>((nodes.size() * 7u / 13u) %
                                                                      nodes.size());
                    for (std::uint32_t offset = 0; offset < nodes.size(); ++offset)
                    {
                        const std::uint32_t candidate =
                            static_cast<std::uint32_t>((chosen + offset) % nodes.size());
                        if (nodes[candidate].signalised) { chosen = candidate; break; }
                    }
                    const RoadNode& node = nodes[chosen];
                    spot = node.position;
                    watching = node.position;
                    if (node.incidentCount > 0)
                    {
                        const Incidence& arm = sim_.city().roads().incidenceBegin(chosen)[0];
                        const Vec2 along = FromHeading(arm.heading);
                        const RoadProfile& profile = ProfileOf(node.highestClass);
                        // Well back from the kerb: at a metre and a half the nearest parked car
                        // fills a third of the frame and the street behind it is invisible.
                        // Back along one arm and out to the building line. Standing a metre off
                        // the kerb puts a twelve-metre bus two metres from the lens the first time
                        // one stops at the light, and a bus is all you can see after that.
                        spot = spot + along * 22.0f +
                               Perp(along) * (profile.carriagewayHalfWidth +
                                              profile.sidewalkWidth + 2.4f);
                    }
                }
                camera_.position = ToWorld(spot, 2.35f);
                // Aimed at the junction and held there, with a slow drift either side of it. The
                // first version panned continuously, which meant the one thing the mode exists to
                // show -- a signal turning and a queue discharging -- was in frame about a third of
                // the time and never when a screenshot was taken.
                camera_.LookAt(ToWorld(watching, 1.6f));
                camera_.yaw += std::sin(static_cast<float>(frameCount_) * 0.0016f) * 0.10f;
                break;
            }

            case CameraMode::Cinematic:
            {
                cinematicTime_ += dt;
                // A long dolly across the downtown, rising as it goes, with a slow roll into the
                // turn. Deliberately slower than feels right while flying it: on playback it reads
                // as deliberate rather than as somebody looking for something.
                const float t = cinematicTime_ * 0.028f;
                const float radius = half * (0.72f - 0.28f * std::sin(t * 0.5f));
                camera_.position = Vector3(std::cos(t) * radius, half * (0.10f + 0.13f * (1.0f + std::sin(t * 0.7f))),
                                            std::sin(t) * radius);
                camera_.LookAt(Vector3(std::cos(t + 1.1f) * half * 0.12f, 55.0f,
                                        std::sin(t + 1.1f) * half * 0.12f));
                camera_.roll = std::sin(t * 0.5f) * 0.035f;
                break;
            }
        }

        if (cameraMode_ != CameraMode::Cinematic) camera_.roll *= std::exp(-dt * 3.0f);

        // The far plane follows the camera's height. At street level nothing beyond a kilometre is
        // visible through the buildings anyway, and a tight far plane is worth several bits of
        // depth precision exactly where the pavement meets the road.
        camera_.farPlane = Clamp(600.0f + camera_.position.Y * 9.0f, 900.0f, 7000.0f);
        camera_.nearPlane = camera_.position.Y > 120.0f ? 2.5f : 0.55f;
    }

    Vector3 CityGame::SunColor() const
    {
        const float elevation = sim_.clock().SunElevationSin();
        const float day = sim_.clock().Daylight();
        // Warm and dim at the horizon, white and bright overhead: the reddening is the same
        // Rayleigh argument the sky model makes, applied to the direct beam.
        const float warmth = 1.0f - Saturate(elevation * 2.4f);
        const Vector3 noon(1.0f, 0.975f, 0.94f);
        const Vector3 horizon(1.0f, 0.60f, 0.34f);
        const float overcast = 1.0f - 0.72f * sim_.weather().cloudiness();
        const float strength = day * overcast * (0.45f + 0.85f * Saturate(elevation * 1.8f));
        return Vector3((noon.X + (horizon.X - noon.X) * warmth) * strength,
                       (noon.Y + (horizon.Y - noon.Y) * warmth) * strength,
                       (noon.Z + (horizon.Z - noon.Z) * warmth) * strength);
    }

    Vector3 CityGame::AmbientColor() const
    {
        const float day = sim_.clock().Daylight();
        const float night = sim_.clock().StreetLightLevel();
        const float cloud = sim_.weather().cloudiness();
        // Daylight ambient is the sky: blue, and *stronger* under cloud, because an overcast sky
        // is one enormous area light. Night ambient is sodium bounced off the pavement, which is
        // why an empty street at 3 a.m. is orange rather than blue.
        // These numbers are small, and they were arrived at by measurement rather than by taste.
        // `PbrEffect`'s ambient term is an irradiance that reaches the surface without the albedo
        // division a diffuse BRDF would apply, so it is roughly an order of magnitude stronger
        // than "ambient times albedo" intuition suggests. Setting it to zero and re-rendering was
        // what showed it: the night city went from a bright sepia photograph -- lit windows
        // invisible against their own walls -- to darkness with the windows glowing, which is what
        // a city at night looks like. Everything here is that measurement, scaled back up until
        // the ground is just readable.
        const Vector3 skyBlue(0.34f, 0.42f, 0.58f);
        const Vector3 sodium(0.0125f, 0.0084f, 0.0056f);
        const float dayStrength = day * (0.088f + 0.115f * cloud);
        return Vector3(skyBlue.X * dayStrength + sodium.X * night,
                       skyBlue.Y * dayStrength + sodium.Y * night,
                       skyBlue.Z * dayStrength + sodium.Z * night);
    }

    void CityGame::UpdateLighting()
    {
        // How far the camera is below street level, as a 0..1 blend. Everything the sky does to
        // the city has to stop at the tunnel roof: a metre of concrete is between the viewer and
        // the sun, and no amount of shadow mapping expresses that, because the cascades are fitted
        // to a view frustum that is entirely underground and there is nothing above it to cast.
        // Without this the tunnel is lit by an 8 a.m. sky and comes out as a uniform white tube.
        undergroundLevel_ = Saturate(-camera_.position.Y / 3.5f);
        const float above = 1.0f - undergroundLevel_;
        const float skyReach = 0.035f + 0.965f * above;

        const Vector3 sunDirection = sim_.clock().SunDirection();
        const Vector3 moonDirection = sim_.clock().MoonDirection();
        const float day = sim_.clock().Daylight();

        // Below the horizon the key light becomes the moon, which points the other way. Swapping
        // the direction rather than fading the sun out is what stops the whole city being lit from
        // underground for an hour either side of midnight.
        sun_.Direction = day > 0.02f ? sunDirection
                                     : Vector3(-moonDirection.X, -moonDirection.Y, -moonDirection.Z);

        if (sky_ != nullptr)
        {
            sky_->setSunDirection(sunDirection);
            // Turbidity is haze. The weather drives it, and it is what makes a hot afternoon white
            // at the horizon and a clear morning deep blue overhead.
            // Turbidity is capped well below the model's range. Above about six the analytic sky
            // turns a strong olive-yellow, which is a real property of a very hazy atmosphere and
            // reads, on a rainy afternoon, as a bug.
            sky_->setTurbidity(Clamp(sim_.weather().turbidity(), 1.8f, 5.5f));
            // Held well below 1 and scaled hard by daylight. The sky fills the top half of most
            // frames, and an ACES curve fed a sky brighter than the surfaces under it lifts the
            // whole image's black point -- which is how the first version managed to make a clear
            // morning look like a foggy one. Below the horizon the model keeps producing a deep
            // sunset red, so at night it is turned almost off and the sky colour comes from the
            // overlay below instead.
            // The intensity is a multiplier on a *physical* radiance, and the analytic sky's
            // output is in the tens rather than around one -- so anything near unity here arrives
            // at the tonemapper far above white and comes back as a flat white ceiling with the
            // horizon washed out. Under a tenth is what puts a clear sky at a believable blue.
            sky_->setIntensity(0.014f + 0.075f * day * (1.0f - 0.5f * sim_.weather().cloudiness()));
        }

        // The environment is rebuilt only when the sky has moved enough to matter; `Update`
        // decides that for itself and reports whether it did anything.
        skyLightRebuiltThisFrame_ = skyLight_.Update(
            getGraphicsDeviceProperty(), sunDirection,
            Clamp(sim_.weather().turbidity(), 1.8f, 5.5f), day, sim_.weather().cloudiness(),
            sim_.clock().StreetLightLevel());
        if (skyLight_.valid())
        {
            // `ImageBasedLightEXT::Intensity` is the one multiplier the whole environment has --
            // the cubes themselves are 8-bit and cannot hold a brightness above one -- so it is
            // also the only place the sky can be told it does not reach a tunnel.
            ImageBasedLightEXT ibl = skyLight_.light();
            ibl.Intensity *= skyReach;
            effect_->setImageBasedLightEXT(ibl);
        }

        auto& light0 = effect_->getDirectionalLight0Property();
        light0.setEnabledProperty(true);
        light0.setDirectionProperty(sun_.Direction);
        const Vector3 sunColor = SunColor();
        const float moonlight = (1.0f - day) * 0.010f * (1.0f - 0.7f * sim_.weather().cloudiness());
        light0.setDiffuseColorProperty(Vector3((sunColor.X + moonlight * 0.7f) * skyReach,
                                                (sunColor.Y + moonlight * 0.8f) * skyReach,
                                                (sunColor.Z + moonlight * 1.0f) * skyReach));
        light0.setSpecularColorProperty(Vector3(sunColor.X * 0.6f * skyReach,
                                                sunColor.Y * 0.6f * skyReach,
                                                sunColor.Z * 0.6f * skyReach));

        // A dim fill from the opposite side, which stands in for the bounce a single directional
        // light cannot produce. Without it every north face in the city is flat black.
        auto& light1 = effect_->getDirectionalLight1Property();
        light1.setEnabledProperty(true);
        light1.setDirectionProperty(Vector3(-sun_.Direction.X, 0.45f, -sun_.Direction.Z));
        const Vector3 daylightAmbient = AmbientColor();
        // Underground the ambient does not go to zero, it goes to what the tunnel's own light
        // strips bounce off the walls -- a warm, very dim fill rather than the sky's blue one.
        //
        // This only reaches the frame when there is no environment. `setImageBasedLightEXT` says
        // in its own header that an environment *replaces* the flat ambient rather than adding to
        // it, and `PbrEffect::Apply` zeroes the ambient colour whenever the bundle is valid --
        // both terms stand for the same light, so summing them would count it twice. So on a
        // machine where the environment built, the thing actually dimming the underground is
        // `ImageBasedLightEXT::Intensity` above, and this is the fallback path's version of the
        // same decision.
        const Vector3 tunnelAmbient(0.0225f, 0.0202f, 0.0168f);
        const Vector3 ambient(
            daylightAmbient.X * skyReach + tunnelAmbient.X * undergroundLevel_,
            daylightAmbient.Y * skyReach + tunnelAmbient.Y * undergroundLevel_,
            daylightAmbient.Z * skyReach + tunnelAmbient.Z * undergroundLevel_);
        light1.setDiffuseColorProperty(Vector3(ambient.X * 1.6f, ambient.Y * 1.6f, ambient.Z * 1.8f));
        light1.setSpecularColorProperty(Vector3::Zero);
        effect_->getDirectionalLight2Property().setEnabledProperty(false);
        effect_->setAmbientLightColorProperty(ambient);
        effect_->setLightingEnabledProperty(true);

        // Distance fog, tied to the weather rather than to a constant. Fog is what makes a
        // three-kilometre city feel three kilometres deep.
        // Aerial perspective, kept subtle on a clear day and allowed to close in when the weather
        // says so. The first version started the fog at 30% of the far plane and reached full
        // density by the end of it, which on a clear morning turned everything past the middle of
        // the city white -- three kilometres of haze that no real clear day has.
        const float fogStrength = sim_.weather().fogDensity() * 0.85f +
                                  sim_.weather().precipitation() * 0.30f;
        effect_->setFogEnabledProperty(true);
        effect_->setFogStartProperty(Clamp(camera_.farPlane * (0.62f - 0.50f * fogStrength), 60.0f, 3000.0f));
        effect_->setFogEndProperty(camera_.farPlane * (1.45f - 0.55f * fogStrength));
        if (skyLight_.valid())
        {
            // The horizon's own colour, from the same samples the ambient came from. Two
            // separately tuned constants for "what the sky looks like" and "what distance fades
            // into" agree until somebody edits one of them.
            const Vector3 horizon = skyLight_.horizonColor();
            effect_->setFogColorProperty(Vector3(Saturate(horizon.X), Saturate(horizon.Y),
                                                  Saturate(horizon.Z)));
        }
        else
        {
            const float fogDay = Saturate(day * 1.15f);
            effect_->setFogColorProperty(Vector3(0.04f + 0.36f * fogDay, 0.05f + 0.41f * fogDay,
                                                  0.07f + 0.52f * fogDay));
        }

        if (pipeline_ != nullptr)
        {
            auto& settings = pipeline_->getSettings();
            // Ambient occlusion is a contact effect, and from four hundred metres up there are no
            // contacts left to see -- one screen pixel is several metres of pavement. Switching it
            // off above roof height costs nothing visible and gives the prepass's 2.9 ms back,
            // which from an overview is a fifth of the frame.
            if (prepass_ != nullptr) settings.setSSAOEnabled(camera_.position.Y < 120.0f);
            // Exposure follows the sun, but only just. The first version lifted it to 2.4 after
            // dark on the reasoning that a night scene is dark -- and the result was a city
            // blown to white, because at night the *emitters* dominate: every lit window and
            // every lamp head is an emissive surface well above one, and the exposure was
            // multiplying those rather than the darkness between them.
            const float target = 0.55f + 0.14f * (1.0f - day);
            settings.setExposure(Clamp(target, 0.5f, 0.72f));
            settings.setHeightFogDensity(sim_.weather().fogDensity() * 0.028f);
            settings.setHeightFogFalloff(0.035f);
            settings.setHeightFogBaseHeight(2.0f);
            // Volumetric fog stays off, and not by preference.
            //
            // `RenderPipeline` builds a `VolumetricFogPass`, adds it to the chain whenever this
            // density is above zero, and never calls `setLight` on it -- nor does it expose the
            // pass, so a game cannot either. The march therefore runs with no light direction and
            // no shadow map, and what it produces is not "no fog" but a band of red and green
            // across the sky above the rooflines, which is its slice quantisation of a scattering
            // integral against nothing. It cost an afternoon to find because it only appears in
            // weather with fog in it. See CNA-FINDINGS.md A8.
            settings.setVolumetricFogDensity(0.0f);
            // God rays need a god. After sunset there is no bright source behind the rooflines for
            // the radial blur to smear, and what it produces instead is a band of red and green
            // fringing along every silhouette against the sky -- which is what the first night
            // frames showed. The strength follows the sun, and above a heavy overcast it stops
            // too, because that is also when there are no shafts to see.
            const float shafts = options_.quality == Quality::Low      ? 0.0f
                               : options_.quality == Quality::Ultra    ? 0.45f
                                                                       : 0.32f;
            settings.setLightShaftIntensity(shafts * day * day *
                                            (1.0f - 0.85f * sim_.weather().cloudiness()));
            // A wet road at night is the strongest single effect available here, so the bloom
            // threshold drops after dark to let the lit windows and the lamps spill.
            settings.setBloomThreshold(Clamp(1.15f - 0.45f * (1.0f - day), 0.55f, 1.3f));
        }
    }

    void CityGame::Update(GameTime& gameTime)
    {
        const auto dt = static_cast<float>(gameTime.getElapsedGameTimeProperty().getTotalSecondsProperty());
        const float clamped = Clamp(dt, 0.0f, 0.1f);
        ++frameCount_;

        // The scripted render benchmark drives the camera itself, so it runs instead of the input
        // handling rather than alongside it -- a keypress arriving mid-measurement would move the
        // viewpoint the numbers are about.
        if (options_.renderReport) StepRenderReport();
        else HandleInput(clamped);

        System::Diagnostics::Stopwatch watch;
        watch.Start();
        if (!paused_)
        {
            // The simulated interval is real time times the scale, and the simulation sub-steps it
            // internally. Feeding it the whole interval rather than a fixed tick keeps the clock
            // honest when the frame rate drops -- the city does not slow down because the renderer
            // did.
            // In the pipelined model the step is started inside Draw instead, once the instances
            // have been collected from the settled state -- see CityGame::Draw. Everything Update
            // does above reads the simulation (the follow camera most of all), so it has to run
            // against a world that is not being changed underneath it, which it is: the previous
            // frame's step was joined before this frame began.
            if (options_.frameModel == FrameModel::Serial)
            {
                sim_.Step(clamped * sim_.clock().timeScale());
                recorder_.MaybeCheckpoint(sim_, options_.checkpointInterval);
            }
            else
            {
                pendingStepSeconds_ = clamped * sim_.clock().timeScale();
            }
        }
        watch.Stop();
        simMs_ = ElapsedMs(watch);

        // The camera moves *after* the world does. The other order costs one step of lag, which
        // for a free camera is invisible and for the follow camera is not: at sixty times real
        // time the subject covers a metre between the aim and the frame, and at six metres'
        // distance that is enough to walk them out of shot.
        UpdateCamera(clamped);

        // A capture that is still waiting for its subject keeps the loop alive past the limit.
        const bool waitingForShot = !options_.screenshotPath.empty() && !screenshotTaken_ &&
                                    options_.followMetro;
        if (options_.frameLimit > 0 && frameCount_ >= options_.frameLimit && !waitingForShot) Exit();
        Game::Update(gameTime);
    }

    void CityGame::CollectVisible()
    {
        const Matrix view = camera_.View();
        const Matrix projection = camera_.Projection();
        culler_.setCamera(view, projection);

        visibleChunks_.clear();
        visibleTriangles_ = 0;
        for (std::uint32_t i = 0; i < geometry_.chunks().size(); ++i)
        {
            const GeometryChunk& chunk = geometry_.chunks()[i];
            if (chunk.triangles == 0) continue;
            if (!culler_.isVisible(chunk.bounds)) continue;
            visibleChunks_.push_back(i);
            visibleTriangles_ += chunk.triangles;
        }

        // ---- Instances ---------------------------------------------------------------------
        instances_.BeginFrame();
        drawnPeople_ = drawnVehicles_ = drawnProps_ = drawnParked_ = 0;
        drawnTrainCars_ = 0;
        drawnBuses_ = 0;

        const Vector3 eye = camera_.position;
        const auto distanceSq = [&](Vec2 p) {
            const float dx = p.X - eye.X;
            const float dz = p.Y - eye.Z;
            return dx * dx + dz * dz;
        };

        // Props: the draw distance is per kind, because a lamp column two hundred metres away is
        // one pixel wide and a tree at the same distance is a hundred.
        for (const Prop& prop : sim_.city().props())
        {
            const float d2 = distanceSq(prop.position);
            if (propLod_[static_cast<int>(prop.kind)].selectIndex(std::sqrt(d2)) < 0) continue;
            const BoundingBox bounds(Vector3(prop.position.X - 3.0f, -0.5f, prop.position.Y - 3.0f),
                                      Vector3(prop.position.X + 3.0f, 9.0f, prop.position.Y + 3.0f));
            if (!culler_.isVisible(bounds)) continue;
            const Matrix world = Matrix::CreateScale(prop.scale) *
                                 Matrix::CreateRotationY(-prop.rotation) *
                                 Matrix::CreateTranslation(prop.position.X, 0.0f, prop.position.Y);
            instances_.AddProp(prop.kind, world);
            ++drawnProps_;
            // A signal head asks the traffic model what its own approach is showing. The phase is
            // per approach, not per junction: a five-way crossing has five heads and two of them
            // disagree at any moment, which is the thing that makes a junction legible.
            if (prop.kind == PropKind::TrafficSignal && prop.node != kNoIndex)
                instances_.AddSignalLens(sim_.traffic().SignalColour(prop.node, prop.incidence),
                                         world);
        }
        for (const MetroStation& station : sim_.metro().stations())
        {
            if (distanceSq(station.entrance) > 300.0f * 300.0f) continue;
            instances_.AddProp(PropKind::MetroEntrance,
                               Matrix::CreateTranslation(station.entrance.X, 0.15f, station.entrance.Y));
            ++drawnProps_;
        }

        // Parked cars. They are city furniture rather than traffic -- generated with the streets
        // and never simulated -- so they go through the same instanced batches as the moving ones
        // and cost the tick nothing at all.
        for (const ParkedVehicle& parked : sim_.city().parkedVehicles())
        {
            if (vehicleLod_.selectIndex(std::sqrt(distanceSq(parked.position))) < 0) continue;
            const BoundingBox bounds(Vector3(parked.position.X - 4.0f, -0.5f, parked.position.Y - 4.0f),
                                      Vector3(parked.position.X + 4.0f, 3.0f, parked.position.Y + 4.0f));
            if (!culler_.isVisible(bounds)) continue;
            instances_.AddVehicle(static_cast<VehicleKind>(parked.kind), parked.appearance,
                                  VehicleTransform(parked.position, parked.rotation));
            ++drawnParked_;
        }

        // Vehicles.
        for (const Vehicle& vehicle : sim_.traffic().vehicles())
        {
            if (!vehicle.active) continue;
            Vec2 position(0.0f, 0.0f);
            float heading = 0.0f;
            sim_.traffic().Placement(sim_.city(), vehicle, position, heading);
            if (vehicleLod_.selectIndex(std::sqrt(distanceSq(position))) < 0) continue;
            const BoundingBox bounds(Vector3(position.X - 7.0f, -0.5f, position.Y - 7.0f),
                                      Vector3(position.X + 7.0f, 4.5f, position.Y + 7.0f));
            if (!culler_.isVisible(bounds)) continue;
            instances_.AddVehicle(static_cast<VehicleKind>(vehicle.kind),
                                  static_cast<std::uint8_t>(vehicle.appearance %
                                                            InstanceRenderer::kColorBuckets),
                                  VehicleTransform(position, heading));
            ++drawnVehicles_;
        }

        // A shelter at every stop a service actually calls at, facing the road. This is drawn
        // from the bus network rather than from the city's prop list for the reason the prop list
        // no longer has any: a shelter is a property of the service, not of the street.
        for (const BusStop& stop : sim_.buses().stops())
        {
            const float d2 = distanceSq(stop.position);
            if (propLod_[static_cast<int>(PropKind::BusShelter)].selectIndex(std::sqrt(d2)) < 0)
                continue;
            const BoundingBox bounds(Vector3(stop.position.X - 4.0f, -0.5f, stop.position.Y - 4.0f),
                                      Vector3(stop.position.X + 4.0f, 4.0f, stop.position.Y + 4.0f));
            if (!culler_.isVisible(bounds)) continue;
            const float facing = Heading(Perp(stop.kerb - stop.position));
            instances_.AddProp(PropKind::BusShelter,
                               Matrix::CreateRotationY(-facing) *
                                   Matrix::CreateTranslation(stop.position.X, 0.0f, stop.position.Y));
            ++drawnProps_;
        }

        // Buses. Not in the vehicle array -- they run a timetable rather than a route, so they
        // live in BusNetwork with the trains' shape of code -- but they are drawn with the same
        // instanced body as everything else on the road.
        for (const Bus& bus : sim_.buses().buses())
        {
            Vec2 position(0.0f, 0.0f);
            float heading = 0.0f;
            sim_.buses().Placement(bus, position, heading);
            if (vehicleLod_.selectIndex(std::sqrt(distanceSq(position))) < 0) continue;
            const BoundingBox bounds(Vector3(position.X - 9.0f, -0.5f, position.Y - 9.0f),
                                      Vector3(position.X + 9.0f, 5.0f, position.Y + 9.0f));
            if (!culler_.isVisible(bounds)) continue;
            // Coloured by route rather than at random. Every bus on the 24 is the same colour and
            // the one behind it on the 31 is not, which is the cheapest possible legend for a
            // network overlay you can read from four hundred metres up.
            instances_.AddVehicle(VehicleKind::Bus,
                                  static_cast<std::uint8_t>(bus.route %
                                                            InstanceRenderer::kColorBuckets),
                                  VehicleTransform(position, heading));
            ++drawnBuses_;
        }

        // People. Three levels of detail with hard distance bands: near enough to see a walk
        // cycle, near enough to be a person, and far enough to be a moving speck. The bands are
        // where nearly all of the rendering budget for a hundred thousand agents is decided.
        const Agents& agents = sim_.agents();
        for (std::uint32_t agent : sim_.walkingAgents())
        {
            const Vec2 position = agents.position[agent];
            const int level = personLod_.selectIndex(std::sqrt(distanceSq(position)));
            if (level < 0) continue;
            const BoundingBox bounds(Vector3(position.X - 0.6f, -0.2f, position.Y - 0.6f),
                                      Vector3(position.X + 0.6f, 2.2f, position.Y + 0.6f));
            if (!culler_.isVisible(bounds)) continue;

            const auto lod = static_cast<PersonLod>(level);
            const auto phase = static_cast<std::uint8_t>(
                static_cast<int>(agents.animationPhase[agent] * 0.62f) & (InstanceRenderer::kWalkPhases - 1));
            instances_.AddPerson(lod, phase, agents.Appearance(agent),
                                 PersonTransform(position, agents.heading[agent],
                                                 agents.animationPhase[agent], agents.speed[agent]));
            ++drawnPeople_;
        }

        // Precipitation, as a column of particles that travels with the camera.
        //
        // Rain over a three-kilometre city is not simulated -- there is no useful sense in which
        // the drop that lands two kilometres away matters -- so a fixed budget of particles is
        // kept in a box around the viewer and wrapped as it falls. The count follows the weather,
        // so a shower builds and eases rather than switching on.
        const float precipitation = sim_.weather().precipitation();
        if (precipitation > 0.02f)
        {
            const bool snow = sim_.weather().temperatureC() < 0.5f;
            const int count = static_cast<int>(precipitation * (snow ? 1400.0f : 2600.0f));
            const float boxHalf = snow ? 26.0f : 34.0f;
            const float boxHeight = snow ? 22.0f : 30.0f;
            const float fallSpeed = snow ? 1.3f : 9.5f;
            const float wind = sim_.weather().windSpeed();
            const Vec2 windDir = FromHeading(sim_.weather().windDirection());
            const float time = static_cast<float>(frameCount_) * 0.016f;
            for (int i = 0; i < count; ++i)
            {
                const std::uint32_t bits = static_cast<std::uint32_t>(i) * 2654435761u;
                const float fx = static_cast<float>(bits & 1023u) / 1023.0f;
                const float fz = static_cast<float>((bits >> 10) & 1023u) / 1023.0f;
                const float fy = static_cast<float>((bits >> 20) & 1023u) / 1023.0f;
                const float fall = std::fmod(time * fallSpeed + fy * boxHeight, boxHeight);
                const float height = boxHeight - fall;
                // A snowflake wanders; a raindrop does not. One sine each is enough to tell them
                // apart at a glance, which is the whole job.
                const float wobble = snow ? std::sin(time * 1.7f + fy * 31.0f) * 0.9f : 0.0f;
                const Vec2 at(eye.X + (fx * 2.0f - 1.0f) * boxHalf + windDir.X * fall * wind * 0.05f + wobble,
                              eye.Z + (fz * 2.0f - 1.0f) * boxHalf + windDir.Y * fall * wind * 0.05f);
                if (height < 0.05f) continue;
                // Rain leans into the wind; the lean is the difference between rain and a
                // curtain of hanging sticks.
                const float lean = snow ? 0.0f : Clamp(wind * 0.045f, 0.0f, 0.55f);
                instances_.AddPrecipitation(
                    snow, Matrix::CreateRotationZ(lean) *
                              Matrix::CreateTranslation(at.X, height, at.Y));
            }
        }

        // The people standing on a platform. They are drawn from their own list rather than being
        // folded into the walking one, because they are a metre above the rail and the walking
        // pass assumes ground level -- and because a platform with nobody on it is the clearest
        // possible statement that a simulation is not being shown.
        for (std::uint32_t agent : sim_.busQueueAgents())
        {
            const Vec2 position = agents.position[agent];
            const float d2 = distanceSq(position);
            if (personLod_.selectIndex(std::sqrt(d2)) < 0) continue;
            const BoundingBox bounds(Vector3(position.X - 0.6f, 0.0f, position.Y - 0.6f),
                                      Vector3(position.X + 0.6f, 2.2f, position.Y + 0.6f));
            if (!culler_.isVisible(bounds)) continue;
            const int level = personLod_.selectIndex(std::sqrt(d2));
            instances_.AddPerson(static_cast<PersonLod>(std::max(0, level)), 0,
                                 agents.Appearance(agent),
                                 Matrix::CreateRotationY(-agents.heading[agent] + kPi * 0.5f) *
                                     Matrix::CreateTranslation(position.X, 0.0f, position.Y));
            ++drawnPeople_;
        }

        for (std::uint32_t agent : sim_.waitingAgents())
        {
            const Vec2 position = agents.position[agent];
            if (distanceSq(position) > 180.0f * 180.0f) continue;
            const Vector3 at = sim_.AgentWorldPosition(agent);
            const BoundingBox bounds(Vector3(at.X - 0.6f, at.Y - 0.2f, at.Z - 0.6f),
                                      Vector3(at.X + 0.6f, at.Y + 2.2f, at.Z + 0.6f));
            if (!culler_.isVisible(bounds)) continue;
            const int level = personLod_.selectIndex(std::sqrt(distanceSq(position)));
            instances_.AddPerson(static_cast<PersonLod>(std::max(0, level)), 0,
                                 agents.Appearance(agent),
                                 Matrix::CreateRotationY(-agents.heading[agent] + kPi * 0.5f) *
                                     Matrix::CreateTranslation(at.X, at.Y, at.Z));
            ++drawnPeople_;
        }

        // Trains, three cars each, only when the camera is underground or close enough that the
        // tunnel mouth is visible.
        for (const MetroTrain& train : sim_.metro().trains())
        {
            const Vec2 head = sim_.metro().PointOnLine(train.line, train.position);
            if (distanceSq(head) > 420.0f * 420.0f) continue;
            for (int car = 0; car < 3; ++car)
            {
                const float offset = train.position - static_cast<float>(car) * 19.0f;
                if (offset < 0.0f) continue;
                const Vec2 at = sim_.metro().PointOnLine(train.line, offset);
                const Vec2 ahead = sim_.metro().PointOnLine(train.line, offset + 4.0f);
                instances_.AddTrain(Matrix::CreateRotationY(-Heading(ahead - at)) *
                                    Matrix::CreateTranslation(at.X, kMetroDepth, at.Y));
                ++drawnTrainCars_;
            }
        }
    }

    void CityGame::DrawShadowCascades()
    {
        if (shadows_ == nullptr) return;
        GraphicsDevice& device = getGraphicsDeviceProperty();
        System::Diagnostics::Stopwatch watch;
        watch.Start();

        shadows_->update(sun_, camera_.View(), camera_.Projection());
        device.setRasterizerStateProperty(RasterizerState::CullCounterClockwise);
        device.setDepthStencilStateProperty(DepthStencilState::Default);
        device.setBlendStateProperty(BlendState::Opaque);

        for (int cascade = 0; cascade < shadows_->getCascadeCount(); ++cascade)
        {
            shadows_->begin(cascade);
            // `begin` binds the caster program, so nothing here may call Apply on the scene's own
            // effect: doing so replaces the program and the "depth" the cascade records becomes
            // the shaded frame's red channel. Only the buffers are bound.
            for (std::uint32_t index : visibleChunks_)
            {
                const GeometryChunk& chunk = geometry_.chunks()[index];
                for (int m = 0; m < kCityMaterialCount; ++m)
                {
                    // Ground and pavement cast nothing useful and are half the city's triangles.
                    if (m == static_cast<int>(CityMaterial::Grass) ||
                        m == static_cast<int>(CityMaterial::Asphalt) ||
                        m == static_cast<int>(CityMaterial::Pavement))
                        continue;
                    // Nor does anything eleven metres underground, in either direction: the sun
                    // does not reach it and it shades nothing that the sun does reach.
                    if (IsUnderground(m)) continue;
                    if (chunk.meshes[m] != nullptr) chunk.meshes[m]->Draw(device);
                }
            }
            shadows_->end();
        }
        watch.Stop();
        shadowMs_ = ElapsedMs(watch);
    }

    void CityGame::DrawDepthNormalPrepass()
    {
        if (prepass_ == nullptr || pipeline_ == nullptr) return;
        if (!pipeline_->getSettings().isSSAOEnabled()) return;
        ShaderEffect* effect = prepass_->getPrepassEffect();
        if (effect == nullptr || !effect->IsEffectValid()) return;

        GraphicsDevice& device = getGraphicsDeviceProperty();
        System::Diagnostics::Stopwatch watch;
        watch.Start();

        const Matrix view = camera_.View();
        const Matrix projection = camera_.Projection();
        for (int pass = 0; pass < prepass_->getPassCount(); ++pass)
        {
            prepass_->begin(pass, view, projection, camera_.nearPlane, camera_.farPlane);
            device.setRasterizerStateProperty(RasterizerState::CullCounterClockwise);
            device.setDepthStencilStateProperty(DepthStencilState::Default);
            device.setBlendStateProperty(BlendState::Opaque);
            // `begin` selects the prepass program and sets its uniforms, so nothing below may call
            // Apply on the scene's own effect: every such call would replace the program and the
            // "depth" the pass records would end up being the shaded frame's red channel, which
            // makes SSAO compare shading against shading and produce a weak, plausible dimming
            // everywhere instead of occlusion at contacts.
            effect->Apply();
            for (std::uint32_t index : visibleChunks_)
            {
                const GeometryChunk& chunk = geometry_.chunks()[index];
                for (int m = 0; m < kCityMaterialCount; ++m)
                {
                    if (IsUnderground(m) && camera_.position.Y > 1.0f) continue;
                    if (chunk.meshes[m] != nullptr) chunk.meshes[m]->Draw(device);
                }
            }
            prepass_->end();
        }
        pipeline_->setDepthNormalInputs(prepass_->getDepthTexture(), prepass_->getNormalTexture());

        watch.Stop();
        prepassMs_ = ElapsedMs(watch);
    }

    void CityGame::DrawStaticCity()
    {
        GraphicsDevice& device = getGraphicsDeviceProperty();
        const Matrix view = camera_.View();
        const Matrix projection = camera_.Projection();
        const float night = sim_.clock().StreetLightLevel();
        const float wetness = sim_.weather().wetness();
        const float snow = sim_.weather().snowCover();

        effect_->setWorldProperty(Matrix::getIdentityProperty());
        effect_->setViewProperty(view);
        effect_->setProjectionProperty(projection);
        if (shadows_ != nullptr)
        {
            shadows_->applyToReceiver(*effect_);
            effect_->setShadowsEnabledEXT(true);
        }
        else
        {
            effect_->setShadowsEnabledEXT(false);
        }

        device.setDepthStencilStateProperty(DepthStencilState::Default);
        device.setBlendStateProperty(BlendState::Opaque);
        // Anisotropic wrapping on every texture slot the facades and the road use. A road seen
        // at a grazing angle is the canonical anisotropy case, and trilinear alone turns its lane
        // markings into a grey smear about thirty metres out.
        for (int slot = 0; slot < 4; ++slot)
            device.getSamplerStatesProperty()[slot] = SamplerState::AnisotropicWrap;

        for (int m = 0; m < kCityMaterialCount; ++m)
        {
            const Material& material = materials_.Get(static_cast<CityMaterial>(m));
            if (material.albedo == nullptr) continue;
            if (IsUnderground(m) && camera_.position.Y > 1.0f) continue;
            bool applied = false;
            for (std::uint32_t index : visibleChunks_)
            {
                const GeometryChunk& chunk = geometry_.chunks()[index];
                if (chunk.meshes[m] == nullptr) continue;
                if (!applied)
                {
                    // Material-major, chunk-minor. The other order would re-apply the effect --
                    // and therefore re-bind four textures and re-upload the uniform block -- once
                    // per chunk instead of once per material, which at forty visible chunks is
                    // forty times the state changes for the same pixels.
                    materials_.Apply(*effect_, static_cast<CityMaterial>(m), night, wetness, snow);
                    device.setRasterizerStateProperty(material.doubleSided
                                                          ? RasterizerState::CullNone
                                                          : RasterizerState::CullCounterClockwise);
                    effect_->Apply();
                    applied = true;
                }
                chunk.meshes[m]->Draw(device);
                ++drawCalls_;
            }
        }
        device.setRasterizerStateProperty(RasterizerState::CullCounterClockwise);
    }

    void CityGame::DrawSkyOverlay()
    {
        // Cloud cover and night, as one blended sheet over the analytic sky.
        //
        // `AtmosphericSky` models a clear atmosphere and has no cloud term, and below the horizon
        // it keeps producing sunset red -- so an overcast afternoon came out olive and a clear
        // midnight came out maroon. Rather than replace the model, the two things it does not
        // cover are painted over it: an overcast grey whose weight is the cloud fraction, and a
        // deep blue whose weight is how far past sunset it is. On a clear day both weights are
        // zero and the sky is exactly what the model produced.
        if (batch_ == nullptr || text_.WhitePixel() == nullptr) return;
        const float day = sim_.clock().Daylight();
        const float cloud = sim_.weather().cloudiness();
        const float cloudWeight = cloud * 0.88f;
        // Fully opaque once the analytic sky has stopped being drawn, because at that point there
        // is nothing underneath it but the clear colour.
        const float nightWeight = SmoothStep(0.10f, 0.0f, day);
        const float weight = std::max(cloudWeight, nightWeight);
        if (weight < 0.01f) return;

        // The overcast sheet is lit by the sun, so it darkens with it; the night sheet does not.
        const Vector3 overcast(0.50f * day + 0.020f, 0.53f * day + 0.026f, 0.58f * day + 0.048f);
        const Vector3 night(0.018f, 0.026f, 0.052f);
        const float blend = cloudWeight >= nightWeight ? 0.0f : 1.0f;
        const Vector3 colour(overcast.X + (night.X - overcast.X) * blend,
                             overcast.Y + (night.Y - overcast.Y) * blend,
                             overcast.Z + (night.Z - overcast.Z) * blend);

        GraphicsDevice& device = getGraphicsDeviceProperty();
        const int width = device.getViewportProperty().getWidthProperty();
        const int height = device.getViewportProperty().getHeightProperty();
        device.setDepthStencilStateProperty(DepthStencilState::None);
        batch_->Begin(SpriteSortMode::Deferred, BlendState::AlphaBlend);
        batch_->Draw(*text_.WhitePixel(), Rectangle(0, 0, width, height),
                     RgbaColor(static_cast<std::uint8_t>(Saturate(colour.X) * 255.0f),
                               static_cast<std::uint8_t>(Saturate(colour.Y) * 255.0f),
                               static_cast<std::uint8_t>(Saturate(colour.Z) * 255.0f),
                               static_cast<std::uint8_t>(Saturate(weight) * 255.0f)));
        batch_->End();
        device.setDepthStencilStateProperty(DepthStencilState::Default);
        device.setBlendStateProperty(BlendState::Opaque);
        ++drawCalls_;
    }

    const char* HeatmapName(Heatmap heatmap)
    {
        switch (heatmap)
        {
            case Heatmap::None:       return "off";
            case Heatmap::Traffic:    return "traffic speed";
            case Heatmap::Density:    return "density";
            case Heatmap::RenderCost: return "render cost";
            case Heatmap::PathCache:  return "path planning";
            case Heatmap::Count:      break;
        }
        return "?";
    }

    namespace
    {
        /// Blue through green and yellow to red, for a value already normalised to 0..1.
        ///
        /// Not a rainbow and not a single hue ramp. A rainbow puts its brightest band in the
        /// middle and makes a mid value look like the extreme; a single hue makes the difference
        /// between "busy" and "stopped" a shade of the same colour. This is the compromise every
        /// traffic map converges on, for the reason they all converge on it.
        Color HeatColour(float t)
        {
            t = Clamp(t, 0.0f, 1.0f);
            const float r = Clamp(t < 0.5f ? t * 2.0f : 1.0f, 0.0f, 1.0f);
            const float g = Clamp(t < 0.5f ? 0.35f + t * 1.3f : 2.0f - t * 2.0f, 0.0f, 1.0f);
            const float b = Clamp(t < 0.25f ? 1.0f - t * 4.0f : 0.0f, 0.0f, 1.0f);
            return RgbaColor(static_cast<std::uint8_t>(r * 255.0f),
                             static_cast<std::uint8_t>(g * 255.0f),
                             static_cast<std::uint8_t>(b * 255.0f));
        }
    }

    void CityGame::DrawHeatmap()
    {
        if (debug_ == nullptr || heatmap_ == Heatmap::None) return;
        const Vec2 eye = ToGround(camera_.position);
        const float range = 1500.0f;

        debug_->begin(camera_.View(), camera_.Projection());
        debug_->setDepthTested(false);

        switch (heatmap_)
        {
            case Heatmap::Traffic:
            {
                // Mean speed per road segment, against that road's own limit -- an arterial at
                // 8 m/s is flowing and an alley at 8 m/s is not, so coluring by absolute speed
                // would paint the whole suburb red and say nothing.
                const RoadNetwork& roads = sim_.city().roads();
                heatSpeed_.assign(roads.segments().size(), 0.0f);
                heatCount_.assign(roads.segments().size(), 0);
                for (const Vehicle& vehicle : sim_.traffic().vehicles())
                {
                    if (!vehicle.active || vehicle.segment >= heatSpeed_.size()) continue;
                    heatSpeed_[vehicle.segment] += vehicle.speed;
                    ++heatCount_[vehicle.segment];
                }
                int busiest = 0;
                for (std::uint32_t i = 0; i < roads.segments().size(); ++i)
                {
                    if (heatCount_[i] == 0) continue;
                    const RoadSegment& segment = roads.segments()[i];
                    const Vec2 a = roads.nodes()[segment.nodeA].position;
                    const Vec2 b = roads.nodes()[segment.nodeB].position;
                    if (DistanceSq(a, eye) > range * range) continue;
                    busiest = std::max<int>(busiest, heatCount_[i]);
                    const float mean = heatSpeed_[i] / static_cast<float>(heatCount_[i]);
                    const float ratio = mean / std::max(1.0f, ProfileOf(segment.roadClass).speedLimit);
                    // Inverted: red is *slow*, which is the thing worth finding.
                    debug_->addLine(ToWorld(a, 2.0f), ToWorld(b, 2.0f),
                                    HeatColour(1.0f - Clamp(ratio, 0.0f, 1.0f)));
                }
                char text[96];
                std::snprintf(text, sizeof(text),
                              "red = stopped, blue = at the limit   busiest segment %d vehicles",
                              busiest);
                heatLegend_ = text;
                break;
            }

            case Heatmap::Density:
            {
                // Everybody outdoors, binned into a hundred-metre grid. The cell size is the
                // thing that makes this readable: finer and it is speckle, coarser and a queue at
                // one junction is smeared over a district.
                const float cell = 100.0f;
                const float half = sim_.city().config().halfSize;
                const int side = std::max(1, static_cast<int>(half * 2.0f / cell) + 1);
                heatDensity_.assign(static_cast<std::size_t>(side) * side, 0);
                const auto bin = [&](Vec2 at) {
                    const int x = Clamp(static_cast<int>((at.X + half) / cell), 0, side - 1);
                    const int y = Clamp(static_cast<int>((at.Y + half) / cell), 0, side - 1);
                    auto& value = heatDensity_[static_cast<std::size_t>(y) * side + x];
                    if (value < 65000) ++value;
                };
                for (std::uint32_t agent : sim_.walkingAgents()) bin(sim_.agents().position[agent]);
                for (std::uint32_t agent : sim_.busQueueAgents()) bin(sim_.agents().position[agent]);
                for (const Vehicle& vehicle : sim_.traffic().vehicles())
                {
                    if (!vehicle.active) continue;
                    Vec2 at(0.0f, 0.0f);
                    float heading = 0.0f;
                    sim_.traffic().Placement(sim_.city(), vehicle, at, heading);
                    bin(at);
                }

                std::uint16_t peak = 1;
                for (const std::uint16_t value : heatDensity_) peak = std::max(peak, value);
                for (int y = 0; y < side; ++y)
                    for (int x = 0; x < side; ++x)
                    {
                        const std::uint16_t value = heatDensity_[static_cast<std::size_t>(y) * side + x];
                        if (value == 0) continue;
                        const Vec2 min(-half + x * cell, -half + y * cell);
                        if (DistanceSq(min, eye) > range * range) continue;
                        const float height = 2.0f + 40.0f * static_cast<float>(value) / peak;
                        debug_->addBox(BoundingBox(ToWorld(min, 1.0f),
                                                   ToWorld(min + Vec2(cell, cell), height)),
                                       HeatColour(static_cast<float>(value) / peak));
                    }
                char text[96];
                std::snprintf(text, sizeof(text),
                              "people and vehicles per 100 m cell   busiest %d", peak);
                heatLegend_ = text;
                break;
            }

            case Heatmap::RenderCost:
            {
                // Per chunk, and only the visible ones -- the point of it is to find where the
                // frame is going, and a chunk that was culled is not costing anything.
                std::uint32_t peak = 1;
                for (const std::uint32_t index : visibleChunks_)
                    peak = std::max<std::uint32_t>(peak, geometry_.chunks()[index].triangles);
                for (const std::uint32_t index : visibleChunks_)
                {
                    const GeometryChunk& chunk = geometry_.chunks()[index];
                    if (chunk.triangles == 0) continue;
                    debug_->addBox(chunk.bounds,
                                   HeatColour(static_cast<float>(chunk.triangles) / peak));
                }
                char text[112];
                std::snprintf(text, sizeof(text),
                              "triangles per visible chunk   %zu of %zu chunks, dearest %u",
                              visibleChunks_.size(), geometry_.chunks().size(), peak);
                heatLegend_ = text;
                break;
            }

            case Heatmap::PathCache:
            {
                // Where the route planner is missing its cache, per district, faded on the
                // simulated clock. A cumulative count would be uniform by lunchtime.
                const std::vector<float>& heat = sim_.pathfinder().heatByDistrict();
                const float peak = std::max(1.0f, sim_.pathfinder().peakHeat());
                for (const District& district : sim_.city().districts())
                {
                    if (district.id >= heat.size()) continue;
                    const float value = heat[district.id];
                    if (value < 0.01f) continue;
                    const float height = 3.0f + 90.0f * value / peak;
                    debug_->addBox(BoundingBox(ToWorld(district.rect.min, 1.0f),
                                               ToWorld(district.rect.max, height)),
                                   HeatColour(value / peak));
                }
                char text[112];
                std::snprintf(text, sizeof(text),
                              "route searches per district, ten-second half-life   peak %.0f", peak);
                heatLegend_ = text;
                break;
            }

            case Heatmap::None:
            case Heatmap::Count:
                break;
        }
        debug_->end();
    }

    void CityGame::DrawOverlay()
    {
        DrawHeatmap();
        if (debug_ == nullptr || overlay_ == Overlay::None || overlay_ == Overlay::Statistics) return;
        // `begin` opens the batch and forgets the last one, so the shapes have to be submitted
        // *between* begin and end. Submitting them first and calling begin afterwards -- which is
        // how a SpriteBatch-shaped API reads -- draws nothing at all, silently.
        debug_->begin(camera_.View(), camera_.Projection());
        debug_->setDepthTested(false);

        if (overlay_ == Overlay::RoadNetwork)
        {
            // The graph the route planner actually sees, drawn where the roads are. Colouring by
            // class is what makes an arterial grid, a ring road and three diagonals legible as a
            // network rather than as a scribble.
            const RoadNetwork& roads = sim_.city().roads();
            const Color classColor[kRoadClassCount] = {
                RgbaColor(255, 96, 72), RgbaColor(255, 176, 64), RgbaColor(120, 210, 255),
                RgbaColor(150, 150, 160), RgbaColor(90, 90, 100)};
            for (const RoadSegment& segment : roads.segments())
            {
                const Vec2 a = roads.nodes()[segment.nodeA].position;
                const Vec2 b = roads.nodes()[segment.nodeB].position;
                if (DistanceSq(a, ToGround(camera_.position)) > 1400.0f * 1400.0f) continue;
                debug_->addLine(ToWorld(a, 1.2f), ToWorld(b, 1.2f),
                                classColor[static_cast<int>(segment.roadClass)]);
            }
            for (const RoadNode& node : roads.nodes())
                if (node.signalised &&
                    DistanceSq(node.position, ToGround(camera_.position)) < 900.0f * 900.0f)
                    debug_->addCross(ToWorld(node.position, 3.0f), 3.5f, RgbaColor(80, 255, 130));

            for (const MetroLine& line : sim_.metro().lines())
                for (std::size_t i = 1; i < line.points.size(); ++i)
                    debug_->addLine(ToWorld(line.points[i - 1], kMetroDepth),
                                    ToWorld(line.points[i], kMetroDepth), RgbaColor(255, 90, 220));
        }
        else if (overlay_ == Overlay::Routes)
        {
            // The routes of the citizens near the camera. This is the picture that makes
            // hierarchical planning legible: the corridors are visibly arterial.
            const Agents& agents = sim_.agents();
            int drawn = 0;
            for (std::uint32_t agent : sim_.walkingAgents())
            {
                if (drawn > 400) break;
                if (DistanceSq(agents.position[agent], ToGround(camera_.position)) > 320.0f * 320.0f)
                    continue;
                const std::uint32_t slot = agents.pathSlot[agent];
                if (slot == kNoIndex) continue;
                const std::uint32_t* path = sim_.routes().At(slot);
                for (std::uint16_t i = agents.pathCursor[agent] + 1u; i + 1u < agents.pathLength[agent]; ++i)
                    debug_->addLine(ToWorld(sim_.city().roads().nodes()[path[i]].position, 1.4f),
                                    ToWorld(sim_.city().roads().nodes()[path[i + 1]].position, 1.4f),
                                    RgbaColor(90, 200, 255, 160));
                ++drawn;
            }
            if (followAgent_ != kNoIndex && followAgent_ < agents.size())
            {
                const Vector3 at = sim_.AgentWorldPosition(followAgent_);
                debug_->addSphere(Vector3(at.X, at.Y + 2.3f, at.Z), 0.55f, RgbaColor(255, 230, 60), 12);
            }
        }

        debug_->end();
    }

    void CityGame::Draw(const GameTime& gameTime)
    {
        // The frame time is the *whole* frame, taken from the harness. Timing only the body of
        // Draw was the first version, and it reported 65 fps for a frame that also spent fifteen
        // milliseconds in Update -- half the real rate. A demo built to measure something must not
        // be the thing that is measured wrongly.
        const double elapsed =
            gameTime.getElapsedGameTimeProperty().getTotalSecondsProperty() * 1000.0;
        if (elapsed > 0.0 && elapsed < 500.0)
            smoothedFrameMs_ += (elapsed - smoothedFrameMs_) * 0.06;
        GraphicsDevice& device = getGraphicsDeviceProperty();
        System::Diagnostics::Stopwatch frameWatch;
        frameWatch.Start();
        drawCalls_ = 0;

        // The level tables. `LodGroupEXT` owns the *selection* -- a sorted distance list and a
        // binary search -- while the meshes it would normally hand back are owned per colour
        // bucket by InstanceRenderer, so every level here is registered with a null part and the
        // index is what gets used.
        //
        // Hysteresis stays at zero, deliberately. It is per-object state: the group remembers the
        // level it last returned so a single object hovering on a boundary does not flicker. One
        // group shared by a hundred thousand pedestrians would carry the previous agent's level
        // into the next agent's decision, which is not damping, it is noise.
        personLod_.clear();
        personLod_.addLevel(65.0f, nullptr);      // Near: legs, arms, a walk cycle
        personLod_.addLevel(230.0f, nullptr);     // Mid: torso and head
        personLod_.addLevel(620.0f, nullptr);     // Far: one box
        personLod_.setHysteresis(0.0f);
        vehicleLod_.clear();
        vehicleLod_.addLevel(900.0f, nullptr);
        vehicleLod_.setHysteresis(0.0f);
        {
            const float propRange[kPropKindCount] = {330.0f, 190.0f, 520.0f, 520.0f,
                                                     140.0f, 110.0f, 260.0f, 300.0f};
            for (int kind = 0; kind < kPropKindCount; ++kind)
            {
                propLod_[kind].clear();
                propLod_[kind].addLevel(propRange[kind], nullptr);
                propLod_[kind].setHysteresis(0.0f);
            }
        }

        camera_.aspect = static_cast<float>(device.getViewportProperty().getWidthProperty()) /
                         std::max(1.0f, static_cast<float>(device.getViewportProperty().getHeightProperty()));
        UpdateLighting();
        CollectVisible();

        // The pipelined model, and the whole of it.
        //
        // CollectVisible is the last thing in the frame that reads the simulation: everything
        // after it draws from the instance buffers it just filled and from the static city, which
        // has not changed since start-up. So the step can run beside all of that, on one worker
        // thread, and be joined before the overlay and the HUD -- which read the simulation again
        // -- put it back.
        //
        // No snapshot of the *agents*, and that is the point of putting the launch here rather
        // than in Update: a snapshot of a hundred thousand citizens would cost more than it saved,
        // and the ordering makes one unnecessary. What it costs instead is a frame of latency --
        // the picture is the world as it was when the frame started, which it already was.
        //
        // The four scalars below are the exception, and they are copied rather than read where
        // they were needed. The clock and the weather *are* written by the step, and the draw
        // reads them after this point for the night level, the wetness, the snow and the clear
        // colour. Four unsynchronised floats is a small race and a real one, and the fix is to
        // take them while the world is still standing still.
        const float night = sim_.clock().StreetLightLevel();
        const float wetness = sim_.weather().wetness();
        const float snowCover = sim_.weather().snowCover();
        const float day = sim_.clock().Daylight();

        if (options_.frameModel == FrameModel::Pipelined && pendingStepSeconds_ > 0.0f)
        {
            const float seconds = pendingStepSeconds_;
            pendingStepSeconds_ = 0.0f;
            System::Diagnostics::Stopwatch simWatch;
            simWatch.Start();
            worker_.Run([this, seconds] { sim_.Step(seconds); });
            simLaunchMs_ = ElapsedMs(simWatch);
        }

        DrawShadowCascades();
        DrawDepthNormalPrepass();

        const Matrix view = camera_.View();
        const Matrix projection = camera_.Projection();
        // The clear colour matters only for the frame's first instant -- the sky covers it -- but
        // it is what the viewer sees if the sky is unavailable on this renderer, so it tracks the
        // time of day rather than being a constant. `day`, `night`, `wetness` and `snowCover` were
        // taken above, before the step was started.
        const Color clearColor = RgbaColor(static_cast<std::uint8_t>(12 + 118 * day),
                                           static_cast<std::uint8_t>(14 + 140 * day),
                                           static_cast<std::uint8_t>(22 + 170 * day));

        System::Diagnostics::Stopwatch watch;
        watch.Start();
        if (postProcessing_ && pipeline_ != nullptr)
        {
            pipeline_->setCamera(view, projection, camera_.nearPlane, camera_.farPlane);
            pipeline_->begin(clearColor);
        }
        else
        {
            device.Clear(clearColor);
        }

        // The analytic sky is only meaningful while the sun is above the horizon. `AtmosphericSky`
        // takes any direction and keeps integrating, so below the horizon it goes on producing a
        // deep sunset red that shades into yellow and green further up -- which arrives as a band
        // of colour along every roofline against a night sky. It is not drawn at all after dusk;
        // the overlay below paints the night instead.
        if (sky_ != nullptr && day > 0.015f)
        {
            // The sky writes no depth and is drawn first, so every pixel the city covers is
            // overwritten and the rest is atmosphere.
            device.setDepthStencilStateProperty(DepthStencilState::None);
            sky_->draw(view, projection, device.getViewportProperty().getWidthProperty(),
                       device.getViewportProperty().getHeightProperty());
            device.setDepthStencilStateProperty(DepthStencilState::Default);
            ++drawCalls_;
        }
        DrawSkyOverlay();

        DrawStaticCity();
        watch.Stop();
        sceneMs_ = ElapsedMs(watch);

        watch.Restart();
        drawCalls_ += instances_.Flush(device, *effect_, materials_, view, projection, night, wetness,
                                       snowCover);
        watch.Stop();
        instanceMs_ = ElapsedMs(watch);

        // Back in step before anything reads the simulation again. The wait is measured, because
        // the difference between the two models is exactly how much of it there is: a wait of zero
        // means the draw covered the step, and a wait of most of the step means the two are
        // competing for the same cores rather than overlapping on them.
        if (options_.frameModel == FrameModel::Pipelined)
        {
            System::Diagnostics::Stopwatch joinWatch;
            joinWatch.Start();
            worker_.Wait();
            joinWatch.Stop();
            simJoinMs_ = ElapsedMs(joinWatch);
            simMs_ = simLaunchMs_ + simJoinMs_;
            recorder_.MaybeCheckpoint(sim_, options_.checkpointInterval);
        }

        DrawOverlay();

        if (postProcessing_ && pipeline_ != nullptr) pipeline_->end();

        if (hudVisible_) DrawHud();

        frameWatch.Stop();
        frameMs_ = ElapsedMs(frameWatch);
        smoothedFrameMs_ += (frameMs_ - smoothedFrameMs_) * 0.06f;

        // The capture waits for the frame the caller asked for, not for the first one that has a
        // picture in it. Taking it at frame nine was the first version, and every screenshot in
        // the project was therefore of a city nine frames old -- one that had had a fraction of a
        // simulated second to get anybody out of the house, which made every population figure on
        // them meaningless.
        // One frame before the limit, because Update's own frame-limit check calls Exit() and the
        // harness then never reaches Draw for that frame -- so asking for the capture on exactly
        // the last frame produces no file at all.
        const int captureFrame = options_.frameLimit > 0 ? std::max(1, options_.frameLimit - 1) : 12;
        // With --follow-metro the capture also waits for the subject to actually be underground.
        // A passenger's ride is a couple of real seconds out of a run of thousands of frames, so a
        // capture on a fixed frame lands on the walk at one end or the other about nine times in
        // ten -- which is how the underground went unlooked-at for as long as it did.
        bool subjectReady = true;
        if (options_.followMetro && followAgent_ != kNoIndex && followAgent_ < sim_.agents().size())
        {
            const auto mode = static_cast<Mode>(sim_.agents().mode[followAgent_]);
            subjectReady = mode == Mode::Riding || mode == Mode::WaitingTrain ||
                           frameCount_ > captureFrame * 3;   // give up rather than run for ever
        }
        if (!options_.screenshotPath.empty() && !screenshotTaken_ && frameCount_ >= captureFrame &&
            subjectReady)
        {
            SaveScreenshot();
            screenshotTaken_ = true;
            Exit();
        }
        Game::Draw(gameTime);
    }

    void CityGame::SaveScreenshot()
    {
        GraphicsDevice& device = getGraphicsDeviceProperty();
        try
        {
            const int width = device.getViewportProperty().getWidthProperty();
            const int height = device.getViewportProperty().getHeightProperty();
            const auto count = static_cast<std::size_t>(width) * height;
            std::vector<Color> pixels(count, Color::Transparent);
            device.GetBackBufferData(pixels.data(), static_cast<int>(count));
            std::vector<std::uint8_t> rgba(count * 4);
            for (std::size_t i = 0; i < count; ++i)
            {
                rgba[i * 4 + 0] = static_cast<std::uint8_t>(pixels[i].getRProperty());
                rgba[i * 4 + 1] = static_cast<std::uint8_t>(pixels[i].getGProperty());
                rgba[i * 4 + 2] = static_cast<std::uint8_t>(pixels[i].getBProperty());
                rgba[i * 4 + 3] = 255;
            }
            Texture2D shot = Texture2D::CreateFromPixels(device, width, height, rgba);
            shot.SaveAsPng(options_.screenshotPath);
            std::printf("cna-city: wrote %s\n", options_.screenshotPath.c_str());
        }
        catch (const System::NotSupportedException&)
        {
            std::printf("cna-city: this renderer has no readable back buffer -- no screenshot\n");
        }
    }

    void CityGame::DrawHud()
    {
        if (batch_ == nullptr) return;
        GraphicsDevice& device = getGraphicsDeviceProperty();
        const int width = device.getViewportProperty().getWidthProperty();
        const SimStats& stats = sim_.stats();

        char line[192];
        std::vector<std::string> left;
        const auto add = [&](const char* format, auto... args) {
            std::snprintf(line, sizeof(line), format, args...);
            left.emplace_back(line);
        };

        add("CNA CITY  %s  DAY %d  %s %.0fC", sim_.clock().ClockText(), sim_.clock().day() + 1,
            WeatherName(sim_.weather().kind()), static_cast<double>(sim_.weather().temperatureC()));
        add("");
        add("POPULATION %u", static_cast<unsigned>(sim_.agents().size()));
        add("  INDOORS %u  ON FOOT %u  DRIVING %u  METRO %u", stats.indoors, stats.walking,
            stats.driving, stats.waitingTrain + stats.riding);
        add("  ON A BUS %u  AT A STOP %u", stats.onBus, stats.waitingBus);
        add("  AT WORK %u  AT HOME %u  ASLEEP %u",
            stats.activityCount[static_cast<int>(Activity::AtWork)],
            stats.activityCount[static_cast<int>(Activity::AtHome)],
            stats.activityCount[static_cast<int>(Activity::Asleep)]);
        add("");
        add("TRAFFIC %u VEHICLES  %.1f M/S MEAN  %u QUEUING  %llu GAVE UP",
            sim_.traffic().activeCount(), static_cast<double>(sim_.traffic().meanSpeed()),
            sim_.traffic().blockedCount(),
            static_cast<unsigned long long>(sim_.traffic().gridlockedCount()));
        add("METRO %zu TRAINS  %u RIDING  %u WAITING", sim_.metro().trains().size(), stats.riding,
            stats.waitingTrain);
        add("BUSES %zu ON %zu ROUTES  %zu STOPS  %u RIDING  %u AT STOPS",
            sim_.buses().buses().size(), sim_.buses().routes().size(), sim_.buses().stops().size(),
            stats.onBus, stats.waitingBus);
        add("");
        add("FRAME %.1f MS (%.0f FPS)", smoothedFrameMs_,
            smoothedFrameMs_ > 0.01 ? 1000.0 / smoothedFrameMs_ : 0.0);
        add("  SIM %.1f  DRAW %.1f  (SHADOW %.1f PREPASS %.1f SCENE %.1f INST %.1f)", simMs_,
            frameMs_, shadowMs_, prepassMs_, sceneMs_, instanceMs_);
        add("  SIM SPLIT  DECIDE %.1f WALK %.1f CROWD %.1f TRAFFIC %.1f METRO %.1f BUS %.1f x%d",
            stats.decisionMs, stats.walkMs, stats.crowdMs, stats.trafficMs, stats.metroMs,
            stats.busMs, stats.subSteps);
        add("WALKED FROM A PARKED CAR  %llu",
            static_cast<unsigned long long>(stats.abandonedWalks));
        add("ROUTES %.0f%% CACHED  %u DEFERRED  %u FAILED",
            sim_.pathfinder().stats().queries > 0
                ? 100.0 * static_cast<double>(sim_.pathfinder().stats().hits) /
                      static_cast<double>(sim_.pathfinder().stats().queries)
                : 0.0,
            stats.tripsDeferred, stats.routeFailures);
        add("DRAWS %d  TRIS %dK  CHUNKS %zu/%zu", drawCalls_, visibleTriangles_ / 1000,
            visibleChunks_.size(), geometry_.chunks().size());
        add("DRAWN  PEOPLE %zu  MOVING %zu  PARKED %zu  PROPS %zu  RAIL %zu  BUS %zu",
            drawnPeople_, drawnVehicles_, drawnParked_, drawnProps_, drawnTrainCars_, drawnBuses_);
        add("");
        add("CAMERA %s AT %.0f %.0f %.0f   TIME x%.0f%s", CameraModeName(cameraMode_),
            camera_.position.X, camera_.position.Y, camera_.position.Z,
            static_cast<double>(sim_.clock().timeScale()), paused_ ? "  PAUSED" : "");
        add("QUALITY %s  %s  FRAME %s", QualityName(options_.quality), rendererName_.c_str(),
            FrameModelName(options_.frameModel));
        if (options_.frameModel == FrameModel::Pipelined)
            add("  STEP LAUNCH %.2f MS  JOIN %.2f MS", simLaunchMs_, simJoinMs_);
        if (heatmap_ != Heatmap::None)
        {
            // The scale, always. A heatmap without one is a picture of where the red is, and the
            // same city looks alarming or fine depending on what the brightest cell happened to be.
            add("HEATMAP (F4)  %s", HeatmapName(heatmap_));
            add("  %s", heatLegend_.c_str());
        }
        add("SKY LIGHT  %u REBUILDS  LAST %.2f MS%s", skyLight_.rebuildCount(),
            skyLight_.lastRebuildMs(), skyLight_.valid() ? "" : "  (unavailable)");
        if (gpuTiming_ && pipeline_ != nullptr)
        {
            const auto& timings = pipeline_->getPassTimingsEXT();
            if (timings.empty())
            {
                add("GPU TIMING  no timer queries on this renderer");
            }
            else
            {
                add("GPU TIMING (F3)");
                double total = 0.0;
                for (const auto& pass : timings)
                {
                    add("  %-18s %6.3f MS", pass.Name.c_str(), pass.Milliseconds);
                    total += pass.Milliseconds;
                }
                add("  %-18s %6.3f MS", "post chain total", total);
            }
        }
        if (!diagnostic_.empty()) add("NOTE %s", diagnostic_.c_str());

        // The follow camera's subject panel. It is the whole reason the mode exists: a number on a
        // HUD is a statistic, and one named person walking to a named station is a city.
        std::vector<std::string> right;
        if (cameraMode_ == CameraMode::Follow && followAgent_ != kNoIndex &&
            followAgent_ < sim_.agents().size())
        {
            const Agents& agents = sim_.agents();
            const std::uint32_t agent = followAgent_;
            std::snprintf(line, sizeof(line), "CITIZEN #%u%s", agent,
                          followLocked_ ? "   [LOCKED]" : "");
            right.emplace_back(line);
            right.emplace_back(sim_.DescribeAgent(agent));
            const std::uint32_t home = agents.home[agent];
            const std::uint32_t work = agents.work[agent];
            if (home != kNoIndex)
            {
                const Building& building = sim_.city().buildings()[home];
                std::snprintf(line, sizeof(line), "LIVES IN %s",
                              sim_.city().districts()[building.district].name.c_str());
                right.emplace_back(line);
            }
            if (work != kNoIndex)
            {
                const Building& building = sim_.city().buildings()[work];
                std::snprintf(line, sizeof(line), "WORKS IN %s",
                              sim_.city().districts()[building.district].name.c_str());
                right.emplace_back(line);
            }
            std::snprintf(line, sizeof(line), "LEAVES %02d:%02d  RETURNS %02d:%02d",
                          agents.leaveHomeMinute[agent] / 60, agents.leaveHomeMinute[agent] % 60,
                          agents.leaveWorkMinute[agent] / 60, agents.leaveWorkMinute[agent] % 60);
            right.emplace_back(line);
            std::snprintf(line, sizeof(line), "SPEED %.2f M/S", static_cast<double>(agents.speed[agent]));
            right.emplace_back(line);
            if (agents.metroAlight[agent] != kNoIndex)
            {
                std::snprintf(line, sizeof(line), "ALIGHTS AT %s",
                              sim_.metro().stations()[agents.metroAlight[agent]].name.c_str());
                right.emplace_back(line);
            }
            if (agents.busAlight[agent] != kNoIndex &&
                agents.busAlight[agent] < sim_.buses().stops().size())
            {
                const BusStop& stop = sim_.buses().stops()[agents.busAlight[agent]];
                std::snprintf(line, sizeof(line), "GETS OFF AT %s", stop.name.c_str());
                right.emplace_back(line);
            }
        }

        batch_->Begin(SpriteSortMode::Deferred, BlendState::AlphaBlend);
        constexpr int kScale = 2;
        const int lineHeight = TextRenderer::LineHeight(kScale);
        const int panelHeight = static_cast<int>(left.size()) * lineHeight + 14;
        if (const Texture2D* white = text_.WhitePixel())
            batch_->Draw(*white, Rectangle(8, 8, 430, panelHeight), RgbaColor(6, 8, 12, 168));
        int y = 15;
        for (const std::string& item : left)
        {
            text_.DrawShadowed(*batch_, item, 16, y, kScale, RgbaColor(226, 232, 240));
            y += lineHeight;
        }

        if (!right.empty())
        {
            // Sized to the widest line rather than to a guess. "STUDENT, COMMUTING TO WORK (ON
            // FOOT)" is thirty-six characters, which at this scale is 432 pixels, and the fixed
            // 420 cut the closing bracket off the one panel in the program whose whole job is to
            // tell you what somebody is doing.
            int panelWidth = 0;
            for (const std::string& item : right)
                panelWidth = std::max(panelWidth, text_.Measure(item, kScale));
            panelWidth = std::min(panelWidth + 20, width - 16);
            const int x = width - panelWidth - 8;
            if (const Texture2D* white = text_.WhitePixel())
                batch_->Draw(*white,
                             Rectangle(x, 8, panelWidth,
                                       static_cast<int>(right.size()) * lineHeight + 14),
                             RgbaColor(10, 6, 4, 178));
            int ry = 15;
            for (const std::string& item : right)
            {
                text_.DrawShadowed(*batch_, item, x + 10, ry, kScale, RgbaColor(255, 226, 176));
                ry += lineHeight;
            }
        }

        const char* help = "1-5 CAMERA   N NEXT   L LOCK   F WEATHER   T/G CLOCK   TAB OVERLAY   "
                           "P PAUSE   F1 HUD   F2 POST   F3 GPU   F4 HEAT";
        text_.DrawShadowed(*batch_, help, 16,
                           device.getViewportProperty().getHeightProperty() - lineHeight - 10,
                           kScale, RgbaColor(150, 160, 175));
        batch_->End();
    }
}
