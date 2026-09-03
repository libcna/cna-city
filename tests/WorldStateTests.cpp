// SPDX-License-Identifier: MIT
//
// The clock and the weather. Small, but they drive the sun, the shadows, the street lights, the
// lit windows, the mode-share model and the road surface, so a defect here moves every other
// number in the program.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    TEST(WorldClock, TheDayWrapsAndCounts)
    {
        WorldClock clock;
        clock.Reset(23.5f, 1.0f);
        EXPECT_EQ(clock.day(), 0);
        clock.Advance(3600.0f);                      // half past midnight
        EXPECT_NEAR(clock.hour(), 0.5f, 0.01f);
        EXPECT_EQ(clock.day(), 1) << "midnight did not turn the day over";
    }

    TEST(WorldClock, TheSunIsUpAtNoonAndDownAtMidnight)
    {
        WorldClock clock;
        clock.Reset(12.0f, 1.0f);
        EXPECT_GT(clock.SunElevationSin(), 0.0f);
        EXPECT_GT(clock.Daylight(), 0.8f);
        EXPECT_LT(clock.StreetLightLevel(), 0.2f);

        clock.setHour(0.0f);
        EXPECT_LT(clock.SunElevationSin(), 0.0f);
        EXPECT_LT(clock.Daylight(), 0.05f);
        EXPECT_GT(clock.StreetLightLevel(), 0.8f);
    }

    TEST(WorldClock, TheMoonOpposesTheSun)
    {
        // The key light becomes the moon below the horizon, and the moon points the other way.
        // Fading the sun out instead lit the whole city from underground for an hour either side
        // of midnight.
        WorldClock clock;
        clock.Reset(2.0f, 1.0f);
        const auto sun = clock.SunDirection();
        const auto moon = clock.MoonDirection();
        const float dot = sun.X * moon.X + sun.Y * moon.Y + sun.Z * moon.Z;
        EXPECT_LT(dot, 0.0f) << "the moon is on the same side of the sky as the sun";
    }

    TEST(WorldClock, DaylightIsMonotoneThroughTheMorning)
    {
        WorldClock clock;
        clock.Reset(4.0f, 1.0f);
        float previous = -1.0f;
        for (float hour = 5.0f; hour <= 12.0f; hour += 0.5f)
        {
            clock.setHour(hour);
            const float light = clock.Daylight();
            EXPECT_GE(light, previous - 1e-4f) << "daylight went down at " << hour << ":00";
            previous = light;
        }
    }

    TEST(Weather, ForcingAKindPinsIt)
    {
        Weather weather;
        weather.Reset(WeatherKind::Clear, 1);
        weather.SetRandomChanges(false);
        weather.Force(WeatherKind::Snow);
        for (int i = 0; i < 400; ++i) weather.Update(60.0f, 10.0f);
        EXPECT_EQ(weather.kind(), WeatherKind::Snow)
            << "--fixed-weather does not actually fix the weather";
        EXPECT_GT(weather.snowCover(), 0.0f);
    }

    TEST(Weather, EveryScalarStaysInItsRange)
    {
        // These feed the sky's turbidity, the fog, the material wetness and the tonemapper. A
        // value out of range does not crash; it produces an olive sky or a white city.
        Weather weather;
        weather.Reset(WeatherKind::Clear, 5);
        for (int i = 0; i < 4000; ++i)
        {
            weather.Update(30.0f, static_cast<float>(i % 24));
            EXPECT_GE(weather.cloudiness(), 0.0f);
            EXPECT_LE(weather.cloudiness(), 1.0f);
            EXPECT_GE(weather.precipitation(), 0.0f);
            EXPECT_LE(weather.precipitation(), 1.0f);
            EXPECT_GE(weather.wetness(), 0.0f);
            EXPECT_LE(weather.wetness(), 1.0f);
            EXPECT_GE(weather.snowCover(), 0.0f);
            EXPECT_LE(weather.snowCover(), 1.0f);
            EXPECT_GE(weather.fogDensity(), 0.0f);
            EXPECT_GE(weather.turbidity(), 1.0f);
            EXPECT_GE(weather.WalkingAversion(), 1.0f);
        }
    }

    TEST(Weather, RainWetsTheRoadAndClearWeatherDriesIt)
    {
        Weather weather;
        weather.Reset(WeatherKind::Clear, 9);
        weather.SetRandomChanges(false);
        weather.Force(WeatherKind::Rain);
        for (int i = 0; i < 200; ++i) weather.Update(60.0f, 12.0f);
        const float wet = weather.wetness();
        EXPECT_GT(wet, 0.3f);

        weather.Force(WeatherKind::Clear);
        for (int i = 0; i < 600; ++i) weather.Update(60.0f, 13.0f);
        EXPECT_LT(weather.wetness(), wet) << "the road never dries out";
    }
}
