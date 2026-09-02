// SPDX-License-Identifier: MIT
#include "WorldState.hpp"

#include <cmath>
#include <cstdio>

using Microsoft::Xna::Framework::Vector3;

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kDegrees = kPi / 180.0f;
        /// Prague, and the second week of September -- which is a deliberate choice, not a
        /// default: at 50 degrees north in early autumn the sun is low enough to throw long
        /// shadows down the streets for most of the working day.
        constexpr float kLatitude = 50.08f * kDegrees;
        constexpr float kDeclination = 4.5f * kDegrees;
    }

    void WorldClock::Reset(float hour, float scale)
    {
        hour_ = hour;
        day_ = 0;
        scale_ = scale;
        RecomputeSun();
    }

    void WorldClock::setHour(float hour)
    {
        hour_ = std::fmod(hour + 24.0f, 24.0f);
        RecomputeSun();
    }

    void WorldClock::Advance(float simulatedSeconds)
    {
        hour_ += simulatedSeconds / 3600.0f;
        while (hour_ >= 24.0f) { hour_ -= 24.0f; ++day_; }
        RecomputeSun();
    }

    void WorldClock::RecomputeSun()
    {
        const float hourAngle = (hour_ - 12.0f) * 15.0f * kDegrees;
        const float sinElevation = std::sin(kDeclination) * std::sin(kLatitude) +
                                   std::cos(kDeclination) * std::cos(kLatitude) * std::cos(hourAngle);
        sunElevationSin_ = Clamp(sinElevation, -1.0f, 1.0f);
        const float elevation = std::asin(sunElevationSin_);
        const float cosElevation = std::max(1e-4f, std::cos(elevation));
        float cosAzimuth = (std::sin(kDeclination) - sinElevation * std::sin(kLatitude)) /
                           (cosElevation * std::cos(kLatitude));
        cosAzimuth = Clamp(cosAzimuth, -1.0f, 1.0f);
        float azimuth = std::acos(cosAzimuth);              // measured from north
        if (hourAngle > 0.0f) azimuth = 2.0f * kPi - azimuth;

        // World convention: +X east, +Z south, +Y up. The vector below points from the sun toward
        // the ground, which is what a directional light wants.
        const Vector3 toSun(std::sin(azimuth) * cosElevation,
                            std::sin(elevation),
                            -std::cos(azimuth) * cosElevation);
        sunDirection_ = Vector3(-toSun.X, -toSun.Y, -toSun.Z);
        // The moon is opposite the sun, half a day out of phase. It is not astronomy, it is a
        // second key light so that a clear night is legible rather than black.
        moonDirection_ = Vector3(toSun.X, toSun.Y, toSun.Z);

        const int totalMinutes = static_cast<int>(hour_ * 60.0f) % 1440;
        std::snprintf(clockText_, sizeof(clockText_), "%02d:%02d", totalMinutes / 60, totalMinutes % 60);
    }

    float WorldClock::Daylight() const
    {
        // Civil twilight is the sun between 0 and -6 degrees, so the ramp is over that band
        // rather than over an arbitrary one.
        return SmoothStep(-0.105f, 0.06f, sunElevationSin_);
    }

    float WorldClock::StreetLightLevel() const
    {
        return 1.0f - SmoothStep(-0.02f, 0.10f, sunElevationSin_);
    }

    const char* WeatherName(WeatherKind kind)
    {
        switch (kind)
        {
            case WeatherKind::Clear:        return "Clear";
            case WeatherKind::PartlyCloudy: return "Partly cloudy";
            case WeatherKind::Overcast:     return "Overcast";
            case WeatherKind::Rain:         return "Rain";
            case WeatherKind::Storm:        return "Storm";
            case WeatherKind::Fog:          return "Fog";
            case WeatherKind::Snow:         return "Snow";
        }
        return "?";
    }

    void Weather::Reset(WeatherKind kind, std::uint64_t seed)
    {
        rng_ = Rng(seed, 0x5745'4154'4845'5200ULL);
        Force(kind);
        // Snap rather than transition: the opening frame should already look like the weather it
        // was asked for.
        cloudiness_ = cloudinessTarget_;
        precipitation_ = precipitationTarget_;
        fogDensity_ = fogTarget_;
        turbidity_ = turbidityTarget_;
        windSpeed_ = windTarget_;
        wetness_ = precipitation_ > 0.05f ? 0.85f : 0.0f;
        snowCover_ = kind == WeatherKind::Snow ? 0.6f : 0.0f;
    }

    void Weather::Force(WeatherKind kind)
    {
        kind_ = kind;
        remaining_ = rng_.NextFloat(1400.0f, 5200.0f);   // simulated seconds in this weather
        windDirection_ = rng_.NextFloat(-kPi, kPi);
        switch (kind)
        {
            case WeatherKind::Clear:
                cloudinessTarget_ = rng_.NextFloat(0.02f, 0.12f);
                precipitationTarget_ = 0.0f; fogTarget_ = 0.0f;
                turbidityTarget_ = rng_.NextFloat(2.0f, 3.0f);
                windTarget_ = rng_.NextFloat(0.5f, 3.0f);
                temperature_ = rng_.NextFloat(16.0f, 25.0f);
                break;
            case WeatherKind::PartlyCloudy:
                cloudinessTarget_ = rng_.NextFloat(0.25f, 0.55f);
                precipitationTarget_ = 0.0f; fogTarget_ = 0.0f;
                turbidityTarget_ = rng_.NextFloat(2.6f, 4.0f);
                windTarget_ = rng_.NextFloat(1.5f, 5.0f);
                temperature_ = rng_.NextFloat(13.0f, 21.0f);
                break;
            case WeatherKind::Overcast:
                cloudinessTarget_ = rng_.NextFloat(0.8f, 1.0f);
                precipitationTarget_ = 0.0f; fogTarget_ = rng_.NextFloat(0.0f, 0.15f);
                turbidityTarget_ = rng_.NextFloat(4.5f, 6.5f);
                windTarget_ = rng_.NextFloat(2.0f, 6.0f);
                temperature_ = rng_.NextFloat(9.0f, 16.0f);
                break;
            case WeatherKind::Rain:
                cloudinessTarget_ = rng_.NextFloat(0.85f, 1.0f);
                precipitationTarget_ = rng_.NextFloat(0.35f, 0.7f);
                fogTarget_ = rng_.NextFloat(0.1f, 0.3f);
                turbidityTarget_ = rng_.NextFloat(5.0f, 7.5f);
                windTarget_ = rng_.NextFloat(3.0f, 8.0f);
                temperature_ = rng_.NextFloat(7.0f, 14.0f);
                break;
            case WeatherKind::Storm:
                cloudinessTarget_ = 1.0f;
                precipitationTarget_ = rng_.NextFloat(0.75f, 1.0f);
                fogTarget_ = rng_.NextFloat(0.2f, 0.45f);
                turbidityTarget_ = rng_.NextFloat(7.0f, 9.5f);
                windTarget_ = rng_.NextFloat(9.0f, 18.0f);
                temperature_ = rng_.NextFloat(6.0f, 13.0f);
                break;
            case WeatherKind::Fog:
                cloudinessTarget_ = rng_.NextFloat(0.5f, 0.85f);
                precipitationTarget_ = 0.0f;
                fogTarget_ = rng_.NextFloat(0.55f, 0.95f);
                turbidityTarget_ = rng_.NextFloat(6.0f, 9.0f);
                windTarget_ = rng_.NextFloat(0.0f, 1.5f);
                temperature_ = rng_.NextFloat(2.0f, 9.0f);
                break;
            case WeatherKind::Snow:
                cloudinessTarget_ = rng_.NextFloat(0.8f, 1.0f);
                precipitationTarget_ = rng_.NextFloat(0.3f, 0.8f);
                fogTarget_ = rng_.NextFloat(0.15f, 0.4f);
                turbidityTarget_ = rng_.NextFloat(3.5f, 5.5f);
                windTarget_ = rng_.NextFloat(1.0f, 6.0f);
                temperature_ = rng_.NextFloat(-6.0f, 1.0f);
                break;
        }
    }

    void Weather::PickTarget()
    {
        // A weighted walk rather than a uniform draw, so the sequence reads like a forecast:
        // clear weather usually stays clear or clouds over, a storm blows through into rain, and
        // fog burns off into overcast rather than into a thunderstorm.
        static const std::uint8_t kTransitions[kWeatherKindCount][4] = {
            /* Clear        */ {0, 1, 1, 1},
            /* PartlyCloudy */ {0, 1, 2, 2},
            /* Overcast     */ {1, 2, 3, 5},
            /* Rain         */ {2, 2, 3, 4},
            /* Storm        */ {3, 3, 2, 2},
            /* Fog          */ {2, 2, 1, 5},
            /* Snow         */ {2, 6, 6, 2},
        };
        const std::uint8_t next = kTransitions[static_cast<int>(kind_)][rng_.NextUInt(4)];
        // Snow needs the cold; without this the same seed could put a blizzard into a warm
        // afternoon, and the temperature readout would contradict the sky.
        WeatherKind picked = static_cast<WeatherKind>(next);
        if (picked == WeatherKind::Snow && temperature_ > 3.0f) picked = WeatherKind::Rain;
        if (picked == WeatherKind::Rain && temperature_ < -1.0f) picked = WeatherKind::Snow;
        Force(picked);
    }

    void Weather::Update(float simulatedSeconds, float hourOfDay)
    {
        remaining_ -= simulatedSeconds;
        if (remaining_ <= 0.0f) PickTarget();

        // A five-minute time constant on a change of weather. Anything faster is a cut, and a cut
        // is exactly what a demo about atmosphere must not do.
        const float rate = 1.0f - std::exp(-simulatedSeconds / 300.0f);
        cloudiness_ += (cloudinessTarget_ - cloudiness_) * rate;
        precipitation_ += (precipitationTarget_ - precipitation_) * rate;
        turbidity_ += (turbidityTarget_ - turbidity_) * rate;
        windSpeed_ += (windTarget_ - windSpeed_) * rate;

        // Radiation fog forms overnight and burns off after sunrise; that diurnal shape is what
        // makes fog feel like weather rather than like a slider.
        const float nightBoost = 0.5f + 0.5f * std::cos((hourOfDay - 5.0f) * kPi / 12.0f);
        const float wantedFog = fogTarget_ * (0.45f + 0.75f * nightBoost);
        fogDensity_ += (wantedFog - fogDensity_) * rate;

        // Wetness rises quickly in rain and dries slowly, and the drying is slower when it is
        // cold or the air is already saturated.
        const bool freezing = temperature_ < 0.5f;
        if (precipitation_ > 0.02f && !freezing)
            wetness_ = Clamp(wetness_ + precipitation_ * simulatedSeconds / 120.0f, 0.0f, 1.0f);
        else
        {
            const float dryRate = simulatedSeconds / (freezing ? 3600.0f : 900.0f);
            wetness_ = Clamp(wetness_ - dryRate * (0.4f + 0.6f * (1.0f - cloudiness_)), 0.0f, 1.0f);
        }

        if (precipitation_ > 0.02f && freezing)
            snowCover_ = Clamp(snowCover_ + precipitation_ * simulatedSeconds / 900.0f, 0.0f, 1.0f);
        else if (temperature_ > 1.5f)
            snowCover_ = Clamp(snowCover_ - simulatedSeconds / 2400.0f, 0.0f, 1.0f);

        windDirection_ += rng_.NextFloat(-0.02f, 0.02f) * simulatedSeconds / 60.0f;
    }

    float Weather::WalkingAversion() const
    {
        // Rain and cold shorten how far somebody will walk before they would rather be on a
        // train. The numbers are not measured from anything; they are chosen so that the
        // mode-share readout on the HUD visibly moves when the weather turns, which is the point.
        float aversion = 1.0f;
        aversion += precipitation_ * 0.85f;
        aversion += Saturate((2.0f - temperature_) / 12.0f) * 0.4f;
        aversion += windSpeed_ > 10.0f ? 0.25f : 0.0f;
        return aversion;
    }
}
