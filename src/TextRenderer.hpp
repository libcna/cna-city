// SPDX-License-Identifier: MIT
//
// The 5x7 bitmap font and its SpriteBatch renderer are taken unchanged from the sibling cna-rts
// project (same author, same licence). A HUD font is a solved problem and re-solving it here would
// have taught nobody anything.
#pragma once

#include <memory>
#include <string>

#include "Microsoft/Xna/Framework/Color.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsDevice.hpp"
#include "Microsoft/Xna/Framework/Graphics/SpriteBatch.hpp"
#include "Microsoft/Xna/Framework/Graphics/Texture2D.hpp"

namespace CnaCity
{
    /**
     * @brief A 5x7 bitmap font built at runtime, drawn through SpriteBatch.
     *
     * `SpriteFont` is CNA's real text type, and it is the right one for a game -- but it wants a
     * glyph texture and a metrics table from the content pipeline, and a benchmark that has to be
     * cloned and run should not need a content build to print its own frame rate. The glyphs are
     * a few dozen lines of ASCII art in the .cpp, baked into one small texture at start-up.
     */
    class TextRenderer
    {
    public:
        void Load(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device);
        void Unload();

        /** @brief Width in pixels of @p text at @p scale. */
        [[nodiscard]] int Measure(const std::string& text, int scale) const;
        [[nodiscard]] static int LineHeight(int scale) { return (kGlyphHeight + 1) * scale; }

        /**
         * @brief Draws @p text with its top-left corner at (@p x, @p y).
         *
         * @param batch Must already be inside `Begin()`/`End()`.
         * @param scale Integer pixel scale; 1 gives a 5x7 glyph.
         */
        void Draw(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                  const std::string& text, int x, int y, int scale,
                  Microsoft::Xna::Framework::Color color) const;

        /** @brief Draws @p text with a one-pixel dark outline, for legibility over the map. */
        void DrawShadowed(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                          const std::string& text, int x, int y, int scale,
                          Microsoft::Xna::Framework::Color color) const;

        /** @brief A 1x1 white texture, handy for HUD panels and selection rectangles. */
        [[nodiscard]] const Microsoft::Xna::Framework::Graphics::Texture2D* WhitePixel() const
        {
            return white_.get();
        }

        static constexpr int kGlyphWidth = 5;
        static constexpr int kGlyphHeight = 7;

    private:
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> atlas_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> white_;
        int columns_ = 0;
    };
}
