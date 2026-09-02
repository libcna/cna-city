// SPDX-License-Identifier: MIT
//
// Taken from the sibling cna-rts project (same author, same licence).
#pragma once

#include <cstdint>

#include "Microsoft/Xna/Framework/Color.hpp"

namespace CnaCity
{
    /**
     * @brief Builds a `Color` from a packed AABBGGRR word.
     *
     * XNA's `Color` is a blittable value type with a public packed constructor; CNA's is a
     * polymorphic C++ object whose packed constructor is private, so the packed value goes in
     * through the property instead. Everything in this program that names a colour names it as
     * one hex word -- palettes, fog alphas, HUD tints -- so the conversion lives in one place.
     */
    inline Microsoft::Xna::Framework::Color PackedColor(std::uint32_t abgr)
    {
        Microsoft::Xna::Framework::Color colour;
        colour.setPackedValueProperty(abgr);
        return colour;
    }

    /** @brief Builds a `Color` from four channel bytes. */
    inline Microsoft::Xna::Framework::Color RgbaColor(std::uint8_t r, std::uint8_t g,
                                                      std::uint8_t b, std::uint8_t a = 255)
    {
        return PackedColor(static_cast<std::uint32_t>(r) |
                           (static_cast<std::uint32_t>(g) << 8) |
                           (static_cast<std::uint32_t>(b) << 16) |
                           (static_cast<std::uint32_t>(a) << 24));
    }
}
