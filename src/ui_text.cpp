#include "ui_text.h"

#include "command_line.h"
#include "engine_interfaces.h"
#include "log.h"

#include <windows.h>

#include <icu.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <unordered_map>

namespace gmodpp::ui {

namespace {

constexpr char kSourceLanguage[] = "en";
constexpr wchar_t kFileName[] = L"gmodpp.properties";

struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
};
using Table = std::unordered_map<std::string, std::string, Hash, std::equal_to<>>;

Table g_text;
// GMod's language, and the one the text is in.
std::string g_language;
std::string g_textLanguage = kSourceLanguage;
// The language the menu is set to, overriding GMod's while not empty.
std::string g_override;
bool g_rightToLeft = false;
bool g_loaded = false;

// GMod's language codes are folder names such as "pt-br"; anything else could name a path outside the folder.
bool IsLanguageCode(std::string_view code)
{
    return !code.empty() && code.size() <= 16 &&
        std::all_of(code.begin(), code.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::string_view Trim(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// As in GMod's files, a backslash keeps the next character as it is ("\:"), and "\n" is a line break.
std::string Unescape(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            result += text[i];
            continue;
        }
        ++i;
        result += text[i] == 'n' ? '\n' : text[i];
    }
    return result;
}

// key=value lines; lines starting with # or ! are comments. Later files override earlier keys.
bool ReadProperties(const std::filesystem::path& path, Table& table)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::string line;
    for (int number = 1; std::getline(file, line); ++number) {
        std::string_view text = line;
        if (number == 1 && text.starts_with("\xEF\xBB\xBF"))
            text.remove_prefix(3);
        if (text.ends_with('\r'))
            text.remove_suffix(1);
        const std::string_view content = Trim(text);
        if (content.empty() || content.front() == '#' || content.front() == '!')
            continue;
        const size_t equals = content.find('=');
        if (equals == std::string_view::npos) {
            log::Info("Localization: line {} of {} has no '='", number, path.string());
            continue;
        }
        table.insert_or_assign(std::string(Trim(content.substr(0, equals))), Unescape(Trim(content.substr(equals + 1))));
    }
    return true;
}

std::filesystem::path LocalizationDirectory() { return ModDirectory() / L"localization"; }

void Load(const std::string& language)
{
    const std::filesystem::path directory = LocalizationDirectory();
    Table text;
    std::string textLanguage = kSourceLanguage;
    const std::filesystem::path source = directory / kSourceLanguage / kFileName;
    if (!ReadProperties(source, text))
        log::Info("Localization: {} could not be read; the menu shows its text keys", source.string());
    if (language != kSourceLanguage) {
        const std::filesystem::path translation = directory / language / kFileName;
        if (ReadProperties(translation, text)) {
            textLanguage = language;
            log::Info("Localization: language '{}'", language);
        } else {
            log::Info("Localization: no {} for language '{}'; using English", translation.string(), language);
        }
    }
    g_text = std::move(text);
    g_language = language;
    g_textLanguage = textLanguage;
    g_rightToLeft = uloc_isRightToLeft(textLanguage.c_str()) != 0;
    g_loaded = true;
}

std::string Utf8(std::wstring_view text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring LocaleText(LCTYPE type)
{
    wchar_t buffer[16] = {};
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type, buffer, static_cast<int>(std::size(buffer))) > 0 ? buffer : L"";
}

UINT LocaleNumber(LCTYPE type)
{
    DWORD value = 0;
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, type | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&value), sizeof(value) / sizeof(wchar_t));
    return value;
}

// LOCALE_SGROUPING ("3;0", "3;2;0") as NUMBERFMTW::Grouping (3, 32): its digits without the final 0 that marks the last
// group as repeating.
UINT Grouping(std::wstring_view text)
{
    std::wstring digits;
    std::copy_if(text.begin(), text.end(), std::back_inserter(digits), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (digits.size() > 1 && digits.back() == L'0')
        digits.pop_back();
    return digits.empty() ? 0 : static_cast<UINT>(std::stoul(digits));
}

struct NumberFormat {
    std::wstring decimal = LocaleText(LOCALE_SDECIMAL);
    std::wstring thousand = LocaleText(LOCALE_STHOUSAND);
    UINT leadingZero = LocaleNumber(LOCALE_ILZERO);
    UINT grouping = Grouping(LocaleText(LOCALE_SGROUPING));
    UINT negativeOrder = LocaleNumber(LOCALE_INEGNUMBER);
};

NumberFormat& UserNumberFormat()
{
    static NumberFormat format;
    return format;
}

}

void UpdateLanguage()
{
    std::string language = g_override.empty() ? engine::GameLanguage() : g_override;
    if (!IsLanguageCode(language))
        language = kSourceLanguage;
    if (!g_loaded || language != g_language)
        Load(language);
}

const std::string& Language()
{
    if (!g_loaded)
        UpdateLanguage();
    return g_textLanguage;
}

bool RightToLeft()
{
    if (!g_loaded)
        UpdateLanguage();
    return g_rightToLeft;
}

void SetLanguageOverride(std::string language)
{
    if (language == g_override)
        return;
    g_override = std::move(language);
    UpdateLanguage();
}

std::vector<std::string> AvailableLanguages()
{
    std::vector<std::string> languages;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(LocalizationDirectory(), error)) {
        const std::string code = entry.path().filename().string();
        if (entry.is_directory(error) && IsLanguageCode(code) && std::filesystem::exists(entry.path() / kFileName, error))
            languages.push_back(code);
    }
    std::sort(languages.begin(), languages.end());
    const auto english = std::find(languages.begin(), languages.end(), kSourceLanguage);
    if (english != languages.end())
        std::rotate(languages.begin(), english, english + 1);
    return languages;
}

std::string LanguageName(const std::string& language)
{
    const std::wstring locale(language.begin(), language.end());
    wchar_t name[LOCALE_NAME_MAX_LENGTH * 2] = {};
    if (GetLocaleInfoEx(locale.c_str(), LOCALE_SNATIVELANGUAGENAME, name, static_cast<int>(std::size(name))) <= 0)
        return language;
    return Utf8(name);
}

std::string AllText()
{
    Table text;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(LocalizationDirectory(), error)) {
        const std::string code = entry.path().filename().string();
        if (entry.is_directory(error) && IsLanguageCode(code)) {
            Table language;
            if (ReadProperties(entry.path() / kFileName, language))
                text.insert(language.begin(), language.end());
        }
    }
    std::string all;
    // One line each, so Arabic letters at the ends of two texts are not joined.
    for (const auto& [key, value] : text)
        all += value + '\n';
    all += Utf8(UserNumberFormat().decimal) + Utf8(UserNumberFormat().thousand);
    return all;
}

const char* Text(const char* key)
{
    if (!g_loaded)
        UpdateLanguage();
    auto found = g_text.find(std::string_view(key));
    if (found == g_text.end()) {
        log::Info("Localization: no text for '{}'", key);
        found = g_text.emplace(key, key).first;
    }
    return found->second.c_str();
}

std::string Format(const char* key, std::initializer_list<Argument> arguments)
{
    const std::string_view text = Text(key);
    std::string result;
    size_t position = 0;
    for (size_t open = text.find('{'); open != std::string_view::npos; open = text.find('{', position)) {
        const size_t close = text.find('}', open);
        if (close == std::string_view::npos)
            break;
        const std::string_view name = text.substr(open + 1, close - open - 1);
        const auto argument = std::find_if(arguments.begin(), arguments.end(), [&](const Argument& candidate) { return candidate.name == name; });
        result += text.substr(position, open - position);
        result += argument != arguments.end() ? std::string_view(argument->value) : text.substr(open, close - open + 1);
        position = close + 1;
    }
    result += text.substr(position);
    return result;
}

namespace {

// `value` in the user's format; for typing, without digit grouping and with a leading minus sign (negative order 1).
std::string FormatNumber(double value, int decimals, bool typed)
{
    NumberFormat& locale = UserNumberFormat();
    const std::string plain = std::format("{:.{}f}", value, decimals);
    const std::wstring digits(plain.begin(), plain.end());
    NUMBERFMTW format = {static_cast<UINT>(decimals), locale.leadingZero, typed ? 0u : locale.grouping, locale.decimal.data(), locale.thousand.data(),
        typed ? 1u : locale.negativeOrder};
    wchar_t buffer[64];
    const int length = GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, digits.c_str(), &format, buffer, static_cast<int>(std::size(buffer)));
    // Only non-finite values ("inf", "nan") fail to format.
    if (length <= 0)
        return plain;
    return Utf8(std::wstring_view(buffer, static_cast<size_t>(length) - 1));
}

}

std::string Number(double value, int decimals) { return FormatNumber(value, decimals, false); }

std::string TypedNumber(double value, int decimals) { return FormatNumber(value, decimals, true); }

std::optional<double> ParseNumber(std::string_view text)
{
    const std::string decimal = Utf8(UserNumberFormat().decimal);
    std::string plain;
    for (size_t i = 0; i < text.size();) {
        if (!decimal.empty() && text.substr(i).starts_with(decimal)) {
            plain += '.';
            i += decimal.size();
            continue;
        }
        if (text[i] == ',')
            plain += '.';
        else if (text[i] != ' ')
            plain += text[i];
        ++i;
    }
    std::string_view number = plain;
    if (number.starts_with('+'))
        number.remove_prefix(1);
    double value = 0.0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (number.empty() || error != std::errc() || end != number.data() + number.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

}
