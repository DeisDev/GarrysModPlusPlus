#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gmodpp::ui {

// The text the in-game menu and the loading screen show, read from gmodplusplus\localization\<language>\
// gmodpp.properties (GMod's .properties format, UTF-8) in GMod's language, over the English file.

// Loads the text again when GMod's language has changed. Call on the game's main thread before showing text.
void UpdateLanguage();

// The language of the text shown: GMod's, or "en" when it has no file.
const std::string& Language();
// Shows the text in `language` (a localization folder name) instead of GMod's language; empty follows GMod again.
void SetLanguageOverride(std::string language);
// The languages with a file, sorted, "en" first.
std::vector<std::string> AvailableLanguages();
// A language's name in that language ("Deutsch"), from Windows; the code itself when Windows has no name for it.
std::string LanguageName(const std::string& language);
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
// The same for typing it again: no digit grouping, a leading minus sign.
std::string TypedNumber(double value, int decimals);
// A number typed in the user's format, a comma or a point taken as the decimal separator; nullopt when the text is not
// one.
std::optional<double> ParseNumber(std::string_view text);

}
