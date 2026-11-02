#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gmodpp::ui {

// Text laid out for drawing glyph by glyph from left to right, as Dear ImGui and the game's VGUI draw it, in the
// current language (ui_text.h). Uses the ICU that ships with Windows (icu.dll).

// One line in display order: Arabic letters in their joined forms, and right-to-left runs reversed with their
// brackets mirrored (the Unicode Bidirectional Algorithm, in the language's direction).
std::string Visual(std::string_view text);

// `text` broken into lines no wider than `width` at the language's line break opportunities, each in display order.
// `advance` is a UTF-16 code unit's width.
std::vector<std::string> VisualLines(std::string_view text, float width, const std::function<float(char16_t)>& advance);

// The characters fonts must have to draw `text`: its own, its joined Arabic forms and mirrored brackets.
std::u16string DrawnCharacters(std::string_view text);

}
