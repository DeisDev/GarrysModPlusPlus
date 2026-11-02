#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace gmodpp::ui {

// The text the in-game menu and the loading screen show, read from gmodplusplus\localization\<language>\
// gmodpp.properties (GMod's .properties format, UTF-8) in GMod's language, over the English file.

// Loads the text again when GMod's language has changed. Call on the game's main thread before showing text.
void UpdateLanguage();

// The language of the text shown: GMod's, or "en" when it has no file.
const std::string& Language();
// Whether that language is written from right to left.
bool RightToLeft();

// The text of every language with a file, English included, and the symbols ui::Number writes: everything the
// menu's fonts must be able to draw.
std::string AllText();

// The current language's text for `key`, or the key itself when no file has it.
const char* Text(const char* key);

struct Argument {
    std::string_view name;
    std::string value;
};
// The text for `key` with each {name} in it replaced by its argument.
std::string Format(const char* key, std::initializer_list<Argument> arguments);

// A number in the user's regional format, with `decimals` digits after the decimal separator.
std::string Number(double value, int decimals);

}
