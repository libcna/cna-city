// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "CityMath.hpp"
#include "Microsoft/Xna/Framework/Vector3.hpp"
#include "Rng.hpp"

namespace CnaCity
{
    /**
     * @brief The simulated clock, and the sun position that follows from it.
     *
     * The sun is computed from a real solar-position formula at a fixed latitude rather than being
     * swung around on a circle, because the two differ in exactly the way that matters here: the
     * real sun rises north of east in summer and the day is not twelve hours long, so dawn and
     * dusk arrive at plausible times and the shadows across the city point somewhere believable.
     */
    class WorldClock
    {
    public:
        /** @param hour Start time in hours; @param scale simulated seconds per real second. */
        void Reset(float hour, float scale);

        /** @brief Advances by @p simulatedSeconds; the scale is applied by the caller's loop. */
        void Advance(float simulatedSeconds);

        /**
         * @brief Sets the clock to @p startHour plus @p simulatedSeconds, without accumulating.
         *
         * `Advance` adds a float to a float, and a thousand additions of 1/3600 do not land where
         * one addition of 1000/3600 does. Two seconds of drift over twenty simulated minutes is
         * enough to move a citizen's departure across a schedule boundary, so the same seed gave a
         * different city at a different frame rate -- through nothing but rounding. The clock is a
         * pure function of the seconds it has been asked for, so this computes it instead.
         */
        void setSimulatedSeconds(float startHour, double simulatedSeconds);

        [[nodiscard]] float hour() const { return hour_; }
        [[nodiscard]] int day() const { return day_; }
        [[nodiscard]] float timeScale() const { return scale_; }
        void setTimeScale(float scale) { scale_ = scale; }
        void setHour(float hour);

        /** @brief The direction light travels, i.e. from the sun toward the ground. */
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 SunDirection() const { return sunDirection_; }
        /** @brief Sine of the solar elevation: negative at night, 1 at the zenith. */
        [[nodiscard]] float SunElevationSin() const { return sunElevationSin_; }
        /** @brief The moon's direction, which is what lights the city on a clear night. */
        [[nodiscard]] Microsoft::Xna::Framework::Vector3 MoonDirection() const { return moonDirection_; }

        /** @brief 0 in full night, 1 in full day, with a soft twilight between. */
        [[nodiscard]] float Daylight() const;
        /** @brief 1 while the street lights are on, with the same soft edges. */
        [[nodiscard]] float StreetLightLevel() const;

        /** @brief "07:42" for the HUD. */
        [[nodiscard]] const char* ClockText() const { return clockText_; }

    private:
        void RecomputeSun();

        float hour_ = 7.0f;
        int day_ = 0;
        float scale_ = 180.0f;
        float sunElevationSin_ = 0.0f;
        Microsoft::Xna::Framework::Vector3 sunDirection_{0.0f, -1.0f, 0.0f};
        Microsoft::Xna::Framework::Vector3 moonDirection_{0.0f, -1.0f, 0.0f};
        char clockText_[8] = "00:00";
    };

    enum class WeatherKind : std::uint8_t
    {
        Clear = 0,
        PartlyCloudy,
        Overcast,
        Rain,
        Storm,
        Fog,
        Snow
    };

    inline constexpr int kWeatherKindCount = 7;

    [[nodiscard]] const char* WeatherName(WeatherKind kind);

    /**
     * @brief The weather, as a set of continuous parameters plus the discrete kind driving them.
     *
     * Everything downstream reads the continuous values, never the enum: the sky reads turbidity
     * and cloudiness, the fog passes read density, the renderer reads wetness to make the asphalt
     * reflective, the particle layer reads precipitation, and the agents read all of it to decide
     * whether they would rather take the metro today. The enum only sets targets that the
     * continuous values chase, which is what makes a change of weather a transition rather than a
     * cut.
     */
    class Weather
    {
    public:
        void Reset(WeatherKind kind, std::uint64_t seed);
        void Update(float simulatedSeconds, float hourOfDay);

        /** @brief Forces a specific weather, as the `--weather` switch and the F key do. */
        void Force(WeatherKind kind);

        /**
         * @brief Whether the weather may change on its own. On by default.
         *
         * `--weather rain --fixed-weather` has to still be raining a simulated day later, or the
         * switch is decoration. The continuous parameters still drift within the kind -- fog still
         * thickens overnight and the wind still turns -- because holding those still would make a
         * fixed forecast look like a paused one.
         */
        void SetRandomChanges(bool enabled) { randomChanges_ = enabled; }
        [[nodiscard]] bool randomChanges() const { return randomChanges_; }

        [[nodiscard]] WeatherKind kind() const { return kind_; }
        [[nodiscard]] float cloudiness() const { return cloudiness_; }
        [[nodiscard]] float precipitation() const { return precipitation_; }
        [[nodiscard]] float fogDensity() const { return fogDensity_; }
        /// How wet the ground is. It lags the rain in both directions -- streets stay shiny for a
        /// while after a shower, which is the single most convincing thing wet weather does.
        [[nodiscard]] float wetness() const { return wetness_; }
        [[nodiscard]] float snowCover() const { return snowCover_; }
        [[nodiscard]] float windSpeed() const { return windSpeed_; }
        [[nodiscard]] float windDirection() const { return windDirection_; }
        [[nodiscard]] float turbidity() const { return turbidity_; }
        [[nodiscard]] float temperatureC() const { return temperature_; }
        /// Multiplies how far a pedestrian is willing to walk before taking a train instead.
        [[nodiscard]] float WalkingAversion() const;

    private:
        void PickTarget();

        Rng rng_;
        WeatherKind kind_ = WeatherKind::PartlyCloudy;
        float remaining_ = 0.0f;
        bool randomChanges_ = true;
        float cloudiness_ = 0.35f, cloudinessTarget_ = 0.35f;
        float precipitation_ = 0.0f, precipitationTarget_ = 0.0f;
        float fogDensity_ = 0.0f, fogTarget_ = 0.0f;
        float turbidity_ = 2.8f, turbidityTarget_ = 2.8f;
        float wetness_ = 0.0f;
        float snowCover_ = 0.0f;
        float windSpeed_ = 3.0f, windTarget_ = 3.0f;
        float windDirection_ = 0.0f;
        float temperature_ = 14.0f;
    };
}
