// SPDX-License-Identifier: MIT
#include "SkyLighting.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "CityMath.hpp"
#include "CNA/Graphics/AtmosphericSky.hpp"
#include "Microsoft/Xna/Framework/Color.hpp"
#include "Microsoft/Xna/Framework/Graphics/CubeMapFace.hpp"
#include "Microsoft/Xna/Framework/Graphics/SurfaceFormat.hpp"
#include "System/Diagnostics/Stopwatch.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaCity
{
    namespace
    {
        /// The direction a cube face's texel looks along, in the layout TextureCube uses:
        /// +X, -X, +Y, -Y, +Z, -Z.
        Vector3 FaceDirection(int face, float u, float v)
        {
            const float a = u * 2.0f - 1.0f;
            const float b = v * 2.0f - 1.0f;
            switch (face)
            {
                case 0:  return Vector3(1.0f, -b, -a);
                case 1:  return Vector3(-1.0f, -b, a);
                case 2:  return Vector3(a, 1.0f, b);
                case 3:  return Vector3(a, -1.0f, -b);
                case 4:  return Vector3(a, -b, 1.0f);
                default: return Vector3(-a, -b, -1.0f);
            }
        }

        int Encode(float value)
        {
            // Stored as-is, not through an sRGB transfer.
            //
            // Nothing between here and the shader decodes one: CNA's `SurfaceFormat::Color` is
            // eight bits a channel sampled at face value, which is also why every albedo texture
            // in this project is authored in the space it is displayed in. Putting the sky through
            // an sRGB curve here made the stored texels about twice their intended value, and an
            // irradiance cube convolved from that lit the whole city as though it were standing
            // under a white dome at noon.
            return static_cast<int>(Clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        float Luminance(const Vector3& c) { return 0.2126f * c.X + 0.7152f * c.Y + 0.0722f * c.Z; }
    }

    bool SkyLighting::Build(GraphicsDevice& device)
    {
        Release();
        if (!device.SupportsImageBasedLightingEXT())
        {
            diagnostic_ = "this renderer does not sample image-based lighting";
            return false;
        }

        processor_ = std::make_unique<CNA::Graphics::EnvironmentProcessor>(device);
        environment_ = std::make_unique<TextureCube>(device, kEnvironmentFace, false,
                                                     SurfaceFormat::Color);
        // The BRDF lookup depends only on the microfacet model, not on the sky, so it is generated
        // once and never again.
        brdf_ = processor_->generateBrdfLut(64, 64);
        light_.BrdfLut = brdf_.get();
        lastTurbidity_ = -1.0f;   // force the first Update to build
        return true;
    }

    void SkyLighting::Release()
    {
        light_ = ImageBasedLightEXT{};
        brdf_.reset();
        specular_.reset();
        irradiance_.reset();
        environment_.reset();
        processor_.reset();
        rebuilds_ = 0;
        lastRebuildMs_ = 0.0;
    }

    void SkyLighting::SampleSky(const Vector3& sunDirection, float turbidity, float daylight,
                                float cloudiness, float streetLightLevel)
    {
        // The model's radiance is unbounded and in physical units; the cube is eight bits. So the
        // sky is sampled once into a float buffer, and then normalised so that its *mean* lands on
        // a target -- which is the one number that used to be hand-tuned as the ambient constant.
        //
        // That is the whole trade this class makes, stated plainly: the overall level is still a
        // chosen number, and everything else -- the sun-side gradient, the reddening at sunset, the
        // blue of a clear zenith, the way an overcast sky flattens it -- now comes from the model
        // rather than from a second chosen number that had to be kept in agreement with the first.
        constexpr int kTexelsPerFace = kEnvironmentFace * kEnvironmentFace;
        std::vector<Vector3> raw(static_cast<std::size_t>(kTexelsPerFace) * 6);
        std::vector<Color> face(kTexelsPerFace);

        const float sky = Saturate(daylight);
        const Vector3 overcast(0.50f * sky + 0.020f, 0.53f * sky + 0.026f, 0.58f * sky + 0.048f);
        const Vector3 night(0.018f, 0.026f, 0.052f);
        const float cloudWeight = Saturate(cloudiness) * 0.88f;
        const float nightWeight = SmoothStep(0.10f, 0.0f, sky);
        const Vector3 sheet = cloudWeight >= nightWeight ? overcast : night;
        const float sheetWeight = std::max(cloudWeight, nightWeight);
        const Vector3 sodium(0.055f, 0.038f, 0.024f);
        const float lamps = Saturate(streetLightLevel);

        double luminanceSum = 0.0;
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < kEnvironmentFace; ++y)
                for (int x = 0; x < kEnvironmentFace; ++x)
                {
                    const float u = (static_cast<float>(x) + 0.5f) / kEnvironmentFace;
                    const float v = (static_cast<float>(y) + 0.5f) / kEnvironmentFace;
                    const Vector3 direction = Vector3::Normalize(FaceDirection(f, u, v));

                    Vector3 radiance(0.0f, 0.0f, 0.0f);
                    if (sky > 0.015f)
                        radiance = CNA::Graphics::AtmosphericSky::radiance(direction, sunDirection,
                                                                           turbidity);
                    // Everything the frame does to the sky has to be done here too, or the ambient
                    // disagrees with the sky above it: the analytic model is not drawn below the
                    // horizon, and cloud and night are a sheet blended over it.
                    radiance = Vector3(radiance.X + (sheet.X - radiance.X) * sheetWeight,
                                       radiance.Y + (sheet.Y - radiance.Y) * sheetWeight,
                                       radiance.Z + (sheet.Z - radiance.Z) * sheetWeight);
                    // Below the horizon what a surface sees is the ground: the city bouncing
                    // daylight back up and its own lamps after dark. Leaving it black lights every
                    // upward-facing surface and no downward-facing one, which reads as a room.
                    if (direction.Y < 0.0f)
                    {
                        const float below = std::min(1.0f, -direction.Y * 2.0f);
                        const Vector3 ground(0.11f * sky + sodium.X * lamps,
                                             0.11f * sky + sodium.Y * lamps,
                                             0.10f * sky + sodium.Z * lamps);
                        radiance = Vector3(radiance.X + (ground.X - radiance.X) * below,
                                           radiance.Y + (ground.Y - radiance.Y) * below,
                                           radiance.Z + (ground.Z - radiance.Z) * below);
                    }

                    raw[static_cast<std::size_t>(f) * kTexelsPerFace + y * kEnvironmentFace + x] =
                        radiance;
                    luminanceSum += Luminance(radiance);
                }

        const auto texels = static_cast<float>(kTexelsPerFace * 6);
        const float mean = std::max(1e-5f, static_cast<float>(luminanceSum) / texels);
        // The target: what the ambient used to be set to by hand, kept as one scalar. Daylight
        // dominates, cloud lifts it slightly because an overcast sky is one enormous area light,
        // and after dark it is the city's own lamps and nothing else.
        // Calibrated against the hand-tuned ambient this replaced, so the change is a change of
        // *colour* and not of level: at a clear noon the old constant was 0.094 and this is 0.101.
        // Anything brighter and the asphalt goes white, which is what the first attempt did -- it
        // targeted nearly twice this and lit the city as though under a white dome.
        // Divided by pi, and that factor is the whole reason the first two attempts washed the
        // city out. `AmbientLightColor` is a *radiance* the shader adds to the light sum; an
        // irradiance cube holds the cosine-weighted integral over the hemisphere, which for a
        // uniform environment of radiance L is pi times L. Targeting the old ambient's number
        // directly therefore asked for pi times the light it used to give, and asphalt at three
        // times its intended brightness is white.
        // Half the constant this replaced, and the halving was measured rather than chosen.
        //
        // Sampling the same pixels with the old constant ambient, with no ambient at all, and with
        // this at several levels produced: legacy (54, 58, 64) on the ground and (69, 64, 68) on a
        // wall; *no ambient at all* (54, 59, 65) and (69, 63, 68) -- the same to within noise. The
        // hand-tuned ambient this class replaces was, by day, contributing nothing: at 0.09 it is
        // lost next to a directional sun at 0.9, and it only ever mattered after dark, where the
        // sun contributes nothing at all.
        //
        // At full strength this lifts a wall to (112, 109, 105), which is brighter than the ground
        // it stands on and therefore wrong. Half lifts it to (91, 88, 84) -- about a third above
        // the unlit case, which is roughly what sky fill does to a vertical surface on a clear day.
        constexpr float kSkyFill = 0.5f;
        const float target = kSkyFill *
                             (0.010f + 0.088f * sky * (1.0f + 0.60f * Saturate(cloudiness)) +
                              0.014f * lamps * (1.0f - sky));
        const float scale = target / mean;

        Vector3 sum(0.0f, 0.0f, 0.0f);
        Vector3 horizonSum(0.0f, 0.0f, 0.0f);
        int horizonCount = 0;
        for (int f = 0; f < 6; ++f)
        {
            for (int y = 0; y < kEnvironmentFace; ++y)
                for (int x = 0; x < kEnvironmentFace; ++x)
                {
                    const std::size_t index =
                        static_cast<std::size_t>(f) * kTexelsPerFace + y * kEnvironmentFace + x;
                    const Vector3 c(raw[index].X * scale, raw[index].Y * scale, raw[index].Z * scale);
                    sum = Vector3(sum.X + c.X, sum.Y + c.Y, sum.Z + c.Z);
                    const float u = (static_cast<float>(x) + 0.5f) / kEnvironmentFace;
                    const float v = (static_cast<float>(y) + 0.5f) / kEnvironmentFace;
                    if (std::fabs(Vector3::Normalize(FaceDirection(f, u, v)).Y) < 0.22f)
                    {
                        horizonSum = Vector3(horizonSum.X + c.X, horizonSum.Y + c.Y,
                                             horizonSum.Z + c.Z);
                        ++horizonCount;
                    }
                    face[static_cast<std::size_t>(y) * kEnvironmentFace + x] =
                        Color(Encode(c.X), Encode(c.Y), Encode(c.Z), 255);
                }
            environment_->SetData(static_cast<CubeMapFace>(f), face.data(),
                                  static_cast<int>(face.size()));
        }

        average_ = Vector3(sum.X / texels, sum.Y / texels, sum.Z / texels);
        horizon_ = horizonCount > 0
                       ? Vector3(horizonSum.X / static_cast<float>(horizonCount),
                                 horizonSum.Y / static_cast<float>(horizonCount),
                                 horizonSum.Z / static_cast<float>(horizonCount))
                       : average_;
    }

    bool SkyLighting::Update(GraphicsDevice& device, const Vector3& sunDirection, float turbidity,
                             float daylight, float cloudiness, float streetLightLevel)
    {
        if (processor_ == nullptr || environment_ == nullptr) return false;

        // The sun moves fifteen degrees an hour, so a threshold of about a degree and a half is
        // several simulated minutes at the default time scale and imperceptible in the frame.
        const float moved = 1.0f - Vector3::Dot(Vector3::Normalize(sunDirection),
                                                 Vector3::Normalize(lastSun_));
        const bool changed = moved > 0.0012f ||
                             std::fabs(turbidity - lastTurbidity_) > 0.15f ||
                             std::fabs(daylight - lastDaylight_) > 0.02f ||
                             std::fabs(cloudiness - lastCloud_) > 0.03f;
        if (!changed) return false;

        System::Diagnostics::Stopwatch watch;
        watch.Start();
        SampleSky(sunDirection, turbidity, daylight, cloudiness, streetLightLevel);
        // Eight samples an axis, sixty-four a texel. Irradiance is the lowest-frequency signal in
        // rendering -- there is nothing in it above a couple of cycles across the sphere -- so the
        // sample count buys smoothness and nothing else, and the convolution is the whole cost of
        // this rebuild.
        irradiance_ = processor_->generateIrradiance(environment_.get(), kIrradianceFace, 8);
        specular_ = processor_->generatePrefilteredSpecular(environment_.get(), kSpecularFace,
                                                            kSpecularMips, 10);
        watch.Stop();

        light_.Irradiance = irradiance_.get();
        light_.PrefilteredSpecular = specular_.get();
        light_.PrefilteredMipCount = kSpecularMips;
        lastSun_ = sunDirection;
        lastTurbidity_ = turbidity;
        lastDaylight_ = daylight;
        lastCloud_ = cloudiness;
        lastRebuildMs_ = static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        ++rebuilds_;
        (void)device;
        return true;
    }
}
