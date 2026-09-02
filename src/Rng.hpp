// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace CnaCity
{
    /**
     * @brief A PCG32 stream: deterministic, cheap, and splittable into independent sub-streams.
     *
     * Determinism is a hard requirement here rather than a nicety. The whole city -- the road
     * network, every block, every building, and the home, workplace and daily schedule of every
     * one of a hundred thousand citizens -- is a pure function of one 64-bit seed, so that a
     * benchmark comparing two agent counts is comparing the same city rather than two different
     * ones. `std::mt19937` would do as well numerically and costs 2.5 KB of state per stream,
     * which is the wrong shape when the generator is split per subsystem and per worker thread.
     *
     * The sub-stream mechanism is PCG's own: two generators with the same seed and different
     * `stream` values produce different, uncorrelated sequences. @ref Split names one.
     */
    class Rng
    {
    public:
        constexpr Rng() : Rng(0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL) {}

        constexpr explicit Rng(std::uint64_t seed, std::uint64_t stream = 0xda3e39cb94b95bdbULL)
            : state_(0), inc_((stream << 1u) | 1u)
        {
            NextUInt();
            state_ += seed;
            NextUInt();
        }

        /**
         * @brief A generator that is independent of this one and reproducible from it.
         *
         * Each subsystem takes its own split so that adding a draw in one of them does not shift
         * every number in the others -- the failure that makes "deterministic" useless in practice.
         */
        [[nodiscard]] constexpr Rng Split(std::uint64_t label) const
        {
            return Rng(state_ ^ (label * 0x9e3779b97f4a7c15ULL), inc_ ^ (label + 0x2545f4914f6cdd1dULL));
        }

        constexpr std::uint32_t NextUInt()
        {
            const std::uint64_t old = state_;
            state_ = old * 6364136223846793005ULL + inc_;
            const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
            const auto rot = static_cast<std::uint32_t>(old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
        }

        /** @brief Uniform in [0, bound); returns 0 when @p bound is 0. Debiased by rejection. */
        constexpr std::uint32_t NextUInt(std::uint32_t bound)
        {
            if (bound == 0) return 0;
            const std::uint32_t threshold = (~bound + 1u) % bound;
            for (;;)
            {
                const std::uint32_t value = NextUInt();
                if (value >= threshold) return value % bound;
            }
        }

        /** @brief Uniform in [min, max]; returns @p min when the range is empty. */
        constexpr int NextInt(int min, int max)
        {
            if (max <= min) return min;
            return min + static_cast<int>(NextUInt(static_cast<std::uint32_t>(max - min + 1)));
        }

        /** @brief Uniform in [0, 1). */
        constexpr float NextFloat()
        {
            return static_cast<float>(NextUInt() >> 8) * (1.0f / 16777216.0f);
        }

        /** @brief Uniform in [min, max). */
        constexpr float NextFloat(float min, float max) { return min + NextFloat() * (max - min); }

        /** @brief True with probability @p chance. */
        constexpr bool Chance(float chance) { return NextFloat() < chance; }

        /**
         * @brief A standard normal deviate, by the polar Box-Muller method.
         *
         * Used wherever a city quantity is naturally clustered rather than flat -- walking speed,
         * the hour someone leaves for work, a building's height within its zone. Uniform draws
         * there are what make a procedural city look procedural.
         */
        float NextGaussian()
        {
            if (hasSpare_)
            {
                hasSpare_ = false;
                return spare_;
            }
            float u, v, s;
            do
            {
                u = NextFloat() * 2.0f - 1.0f;
                v = NextFloat() * 2.0f - 1.0f;
                s = u * u + v * v;
            } while (s >= 1.0f || s == 0.0f);
            // __builtin_sqrt/log keep this constexpr-friendly in spirit; <cmath> is included by
            // the callers that need the value, not by the header.
            const float factor = SqrtNegTwoLogOver(s);
            spare_ = v * factor;
            hasSpare_ = true;
            return u * factor;
        }

        /** @brief A normal deviate with the given mean and standard deviation. */
        float NextGaussian(float mean, float stddev) { return mean + stddev * NextGaussian(); }

    private:
        static float SqrtNegTwoLogOver(float s);

        std::uint64_t state_;
        std::uint64_t inc_;
        float spare_ = 0.0f;
        bool  hasSpare_ = false;
    };
}
