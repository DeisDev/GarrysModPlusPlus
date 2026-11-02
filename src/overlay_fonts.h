#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace gmodpp::overlay::fonts {

struct Face {
    std::filesystem::path path;
    // The face's index in a font collection (.ttc); 0 in a single font file.
    uint32_t index = 0;
};

// The fonts that draw `characters` in the game font at `path`, in the order GMod's VGUI uses them: the font itself
// for the glyphs it has, then Tahoma, the game's fallback for every other character (vguimatsurface.dll's
// SetFontGlyphSet_Extended at +0x98470 and GetForeignFallbackFontName at +0x97b60, REA 2026-11-02), then the fonts
// Windows links to Tahoma for GDI, which draws the game's text (FontLink\SystemLink). A game font that can't be read
// leaves Tahoma first, as in the game. Fonts that add no glyph to those before them are left out. `weight` is a
// DWRITE_FONT_WEIGHT for the fallback fonts.
std::vector<Face> GameFontChain(const std::filesystem::path& path, uint32_t weight, std::u16string_view characters);

}
