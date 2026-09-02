// SPDX-License-Identifier: MIT
#include "TextRenderer.hpp"

#include <algorithm>
#include <vector>

#include "Microsoft/Xna/Framework/Rectangle.hpp"
#include "Palette.hpp"
#include "Microsoft/Xna/Framework/Graphics/SurfaceFormat.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaCity
{
    namespace
    {
        /**
         * @brief The font, as ASCII art: 95 printable characters, each 35 characters of '#' and
         * '.' read as seven rows of five.
         *
         * Spelled out rather than packed into hex because a font is the one table where a typo is
         * invisible in the source and obvious on the screen -- and in this shape it is obvious in
         * both. Lowercase letters share their uppercase glyphs; the HUD is all caps anyway.
         */
        const char* const kGlyphArt[95] = {
        /* sp  */ "...................................",
        /* '!' */ ".#....#....#....#....#.........#...",
        /* '\"' */ "#.#..#.#...........................",
        /* '#' */ ".#.#.#####.#.#.#####.#.#...........",
        /* '$' */ "######...##...##...##...##...######",
        /* '%' */ "##..###..#...#...#...#...#..###..##",
        /* '&' */ "######...##...##...##...##...######",
        /* ''' */ "######...##...##...##...##...######",
        /* '(' */ "..#...#....#....#....#....#.....#..",
        /* ')' */ ".#.....#....#....#....#....#...#...",
        /* '*' */ ".....#.#...#...#.#.................",
        /* '+' */ ".......#....#..#####..#............",
        /* ',' */ "..........................#...#....",
        /* '-' */ "...............####................",
        /* '.' */ "...............................#...",
        /* '/' */ "....#...#...#....#...#...#....#....",
        /* '0' */ ".###.#...##..###.#.###..##...#.###.",
        /* '1' */ "..#...##....#....#....#....#...###.",
        /* '2' */ ".###.#...#....#...#...#...#...#####",
        /* '3' */ "#####...#...#.....#.....##...#.###.",
        /* '4' */ "...#...##..#.#.#..#.#####...#....#.",
        /* '5' */ "######....####.....#....##...#.###.",
        /* '6' */ "..##..#...#....####.#...##...#.###.",
        /* '7' */ "#####....#...#...#...#....#....#...",
        /* '8' */ ".###.#...##...#.###.#...##...#.###.",
        /* '9' */ ".###.#...##...#.####....#...#..##..",
        /* ':' */ "...........#..............#........",
        /* ';' */ "######...##...##...##...##...######",
        /* '<' */ "...#...#...#...#.....#.....#.....#.",
        /* '=' */ "..........####......####...........",
        /* '>' */ "#.....#.....#.....#...#...#...#....",
        /* '?' */ ".###.#...#....#...#...#........#...",
        /* '@' */ "######...##...##...##...##...######",
        /* 'A' */ ".###.#...##...#######...##...##...#",
        /* 'B' */ "####.#...##...#####.#...##...#####.",
        /* 'C' */ ".###.#...##....#....#....#...#.###.",
        /* 'D' */ "###..#..#.#...##...##...##..#.###..",
        /* 'E' */ "######....#....####.#....#....#####",
        /* 'F' */ "######....#....####.#....#....#....",
        /* 'G' */ ".###.#...##....#.####...##...#.###.",
        /* 'H' */ "#...##...##...#######...##...##...#",
        /* 'I' */ ".###...#....#....#....#....#...###.",
        /* 'J' */ "..###...#....#....#....#.#..#..##..",
        /* 'K' */ "#...##..#.#.#..##...#.#..#..#.#...#",
        /* 'L' */ "#....#....#....#....#....#....#####",
        /* 'M' */ "#...###.###.#.##.#.##...##...##...#",
        /* 'N' */ "#...###..##.#.##..###...##...##...#",
        /* 'O' */ ".###.#...##...##...##...##...#.###.",
        /* 'P' */ "####.#...##...#####.#....#....#....",
        /* 'Q' */ ".###.#...##...##...##.#.##..#..##.#",
        /* 'R' */ "####.#...##...#####.#.#..#..#.#...#",
        /* 'S' */ ".#####....#.....###.....#....#####.",
        /* 'T' */ "#####..#....#....#....#....#....#..",
        /* 'U' */ "#...##...##...##...##...##...#.###.",
        /* 'V' */ "#...##...##...##...##...#.#.#...#..",
        /* 'W' */ "#...##...##...##.#.##.#.###.###...#",
        /* 'X' */ "#...##...#.#.#...#...#.#.#...##...#",
        /* 'Y' */ "#...##...#.#.#...#....#....#....#..",
        /* 'Z' */ "#####....#...#...#...#...#....#####",
        /* '[' */ ".##...#....#....#....#....#....##..",
        /* '\\' */ "######...##...##...##...##...######",
        /* ']' */ "##.....#....#....#....#....#..##...",
        /* '^' */ "######...##...##...##...##...######",
        /* '_' */ "..............................#####",
        /* '`' */ "######...##...##...##...##...######",
        /* 'a' */ ".###.#...##...#######...##...##...#",
        /* 'b' */ "####.#...##...#####.#...##...#####.",
        /* 'c' */ ".###.#...##....#....#....#...#.###.",
        /* 'd' */ "###..#..#.#...##...##...##..#.###..",
        /* 'e' */ "######....#....####.#....#....#####",
        /* 'f' */ "######....#....####.#....#....#....",
        /* 'g' */ ".###.#...##....#.####...##...#.###.",
        /* 'h' */ "#...##...##...#######...##...##...#",
        /* 'i' */ ".###...#....#....#....#....#...###.",
        /* 'j' */ "..###...#....#....#....#.#..#..##..",
        /* 'k' */ "#...##..#.#.#..##...#.#..#..#.#...#",
        /* 'l' */ "#....#....#....#....#....#....#####",
        /* 'm' */ "#...###.###.#.##.#.##...##...##...#",
        /* 'n' */ "#...###..##.#.##..###...##...##...#",
        /* 'o' */ ".###.#...##...##...##...##...#.###.",
        /* 'p' */ "####.#...##...#####.#....#....#....",
        /* 'q' */ ".###.#...##...##...##.#.##..#..##.#",
        /* 'r' */ "####.#...##...#####.#.#..#..#.#...#",
        /* 's' */ ".#####....#.....###.....#....#####.",
        /* 't' */ "#####..#....#....#....#....#....#..",
        /* 'u' */ "#...##...##...##...##...##...#.###.",
        /* 'v' */ "#...##...##...##...##...#.#.#...#..",
        /* 'w' */ "#...##...##...##.#.##.#.###.###...#",
        /* 'x' */ "#...##...#.#.#...#...#.#.#...##...#",
        /* 'y' */ "#...##...#.#.#...#....#....#....#..",
        /* 'z' */ "#####....#...#...#...#...#....#####",
        /* '{' */ "######...##...##...##...##...######",
        /* '|' */ ".#....#....#....#....#....#....#...",
        /* '}' */ "######...##...##...##...##...######",
        /* '~' */ "######...##...##...##...##...######",
        };

        constexpr int kFirstGlyph = 32;
        constexpr int kGlyphCount = 95;
        constexpr int kAtlasColumns = 16;
    }

    void TextRenderer::Load(GraphicsDevice& device)
    {
        columns_ = kAtlasColumns;
        const int rows = (kGlyphCount + kAtlasColumns - 1) / kAtlasColumns;
        const int width = kAtlasColumns * kGlyphWidth;
        const int height = rows * kGlyphHeight;

        std::vector<Color> pixels(static_cast<std::size_t>(width) * height, PackedColor(0u));
        for (int g = 0; g < kGlyphCount; ++g)
        {
            const char* art = kGlyphArt[g];
            const int originX = (g % kAtlasColumns) * kGlyphWidth;
            const int originY = (g / kAtlasColumns) * kGlyphHeight;
            for (int y = 0; y < kGlyphHeight; ++y)
            {
                for (int x = 0; x < kGlyphWidth; ++x)
                {
                    if (art[y * kGlyphWidth + x] != '#') continue;
                    pixels[static_cast<std::size_t>(originY + y) * width + (originX + x)] =
                        PackedColor(0xFFFFFFFFu);
                }
            }
        }

        atlas_ = std::make_unique<Texture2D>(device, width, height, false, SurfaceFormat::Color);
        atlas_->SetData(pixels.data(), static_cast<int>(pixels.size()));

        const Color white = PackedColor(0xFFFFFFFFu);
        white_ = std::make_unique<Texture2D>(device, 1, 1, false, SurfaceFormat::Color);
        white_->SetData(&white, 1);
    }

    void TextRenderer::Unload()
    {
        atlas_.reset();
        white_.reset();
    }

    int TextRenderer::Measure(const std::string& text, int scale) const
    {
        return static_cast<int>(text.size()) * (kGlyphWidth + 1) * scale;
    }

    void TextRenderer::Draw(SpriteBatch& batch, const std::string& text, int x, int y, int scale,
                            Color color) const
    {
        if (!atlas_) return;
        const int advance = (kGlyphWidth + 1) * scale;
        int penX = x;
        for (const char raw : text)
        {
            const auto code = static_cast<unsigned char>(raw);
            if (code >= kFirstGlyph && code < kFirstGlyph + kGlyphCount && code != ' ')
            {
                const int g = code - kFirstGlyph;
                const Rectangle source((g % columns_) * kGlyphWidth, (g / columns_) * kGlyphHeight,
                                       kGlyphWidth, kGlyphHeight);
                const Rectangle dest(penX, y, kGlyphWidth * scale, kGlyphHeight * scale);
                batch.Draw(*atlas_, dest, source, color);
            }
            penX += advance;
        }
    }

    void TextRenderer::DrawShadowed(SpriteBatch& batch, const std::string& text, int x, int y,
                                    int scale, Color color) const
    {
        const Color shadow = RgbaColor(0, 0, 0, 190);
        Draw(batch, text, x + scale, y + scale, scale, shadow);
        Draw(batch, text, x, y, scale, color);
    }
}
