#include "text_layout.h"

#include "log.h"
#include "ui_text.h"

#include <icu.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

namespace gmodpp::ui {

namespace {

struct BidiDeleter {
    void operator()(UBiDi* bidi) const { ubidi_close(bidi); }
};
struct BreakDeleter {
    void operator()(UBreakIterator* breaks) const { ubrk_close(breaks); }
};

// Reused between calls, all on the game's main thread; the line breaks follow the language's rules.
struct Layout {
    std::unique_ptr<UBiDi, BidiDeleter> paragraph;
    std::unique_ptr<UBiDi, BidiDeleter> line;
    std::unique_ptr<UBreakIterator, BreakDeleter> breaks;
    std::string breaksLanguage;
};
Layout g_layout;

// An ICU failure is logged once per operation; the text is then drawn as it is stored.
bool Failed(UErrorCode status, const char* operation)
{
    if (U_SUCCESS(status))
        return false;
    static std::unordered_set<const char*> logged;
    if (logged.insert(operation).second)
        log::Info("Text layout: {} failed ({})", operation, u_errorName(status));
    return true;
}

std::u16string Utf16(std::string_view text)
{
    std::u16string result(text.size(), u'\0');
    int32_t length = 0;
    UErrorCode status = U_ZERO_ERROR;
    u_strFromUTF8WithSub(result.data(), static_cast<int32_t>(result.size()), &length, text.data(), static_cast<int32_t>(text.size()), 0xFFFD, nullptr, &status);
    if (Failed(status, "u_strFromUTF8WithSub"))
        return {};
    result.resize(static_cast<size_t>(length));
    return result;
}

std::string Utf8(std::u16string_view text)
{
    std::string result(text.size() * 3, '\0');
    int32_t length = 0;
    UErrorCode status = U_ZERO_ERROR;
    u_strToUTF8(result.data(), static_cast<int32_t>(result.size()), &length, text.data(), static_cast<int32_t>(text.size()), &status);
    if (Failed(status, "u_strToUTF8"))
        return {};
    result.resize(static_cast<size_t>(length));
    return result;
}

bool HasArabic(std::u16string_view text)
{
    return std::any_of(text.begin(), text.end(), [](char16_t c) { return (c >= 0x0600 && c <= 0x06FF) || (c >= 0x0750 && c <= 0x077F) || (c >= 0x08A0 && c <= 0x08FF); });
}

// Arabic letters take their joined forms (Arabic Presentation Forms), since glyphs are drawn one by one.
std::u16string Shape(std::u16string text)
{
    if (!HasArabic(text))
        return text;
    constexpr uint32_t kOptions = U_SHAPE_LETTERS_SHAPE | U_SHAPE_TEXT_DIRECTION_LOGICAL | U_SHAPE_LENGTH_GROW_SHRINK;
    std::u16string shaped(text.size() * 2, u'\0');
    UErrorCode status = U_ZERO_ERROR;
    const int32_t length = u_shapeArabic(text.data(), static_cast<int32_t>(text.size()), shaped.data(), static_cast<int32_t>(shaped.size()), kOptions, &status);
    if (Failed(status, "u_shapeArabic"))
        return text;
    shaped.resize(static_cast<size_t>(length));
    return shaped;
}

UBiDiLevel ParagraphLevel() { return RightToLeft() ? 1 : 0; }

UBiDi* Bidi(std::unique_ptr<UBiDi, BidiDeleter>& slot)
{
    if (slot == nullptr) {
        UErrorCode status = U_ZERO_ERROR;
        slot.reset(ubidi_openSized(0, 0, &status));
        if (Failed(status, "ubidi_openSized"))
            slot.reset();
    }
    return slot.get();
}

// `text` set as the paragraph to reorder; it must outlive the paragraph's use.
UBiDi* Paragraph(const std::u16string& text)
{
    UBiDi* bidi = Bidi(g_layout.paragraph);
    if (bidi == nullptr)
        return nullptr;
    UErrorCode status = U_ZERO_ERROR;
    ubidi_setPara(bidi, text.data(), static_cast<int32_t>(text.size()), ParagraphLevel(), nullptr, &status);
    return Failed(status, "ubidi_setPara") ? nullptr : bidi;
}

std::optional<std::u16string> Reordered(UBiDi* bidi)
{
    std::u16string result(static_cast<size_t>(ubidi_getLength(bidi)), u'\0');
    UErrorCode status = U_ZERO_ERROR;
    const int32_t length = ubidi_writeReordered(bidi, result.data(), static_cast<int32_t>(result.size()), UBIDI_DO_MIRRORING | UBIDI_REMOVE_BIDI_CONTROLS, &status);
    if (Failed(status, "ubidi_writeReordered"))
        return std::nullopt;
    result.resize(static_cast<size_t>(length));
    return result;
}

UBreakIterator* LineBreaks(const std::u16string& text)
{
    UErrorCode status = U_ZERO_ERROR;
    if (g_layout.breaks == nullptr || g_layout.breaksLanguage != Language()) {
        g_layout.breaks.reset(ubrk_open(UBRK_LINE, Language().c_str(), nullptr, 0, &status));
        g_layout.breaksLanguage = Language();
        if (Failed(status, "ubrk_open")) {
            g_layout.breaks.reset();
            return nullptr;
        }
    }
    ubrk_setText(g_layout.breaks.get(), text.data(), static_cast<int32_t>(text.size()), &status);
    return Failed(status, "ubrk_setText") ? nullptr : g_layout.breaks.get();
}

bool IsBlank(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x3000; }

// Lines as [start, end) ranges of `text`, without the spaces and line breaks they end with: greedy, at line break
// opportunities, and within a word only where the word alone is wider than the line.
std::vector<std::pair<int32_t, int32_t>> BreakLines(const std::u16string& text, float width, const std::function<float(char16_t)>& advance)
{
    const int32_t length = static_cast<int32_t>(text.size());
    std::vector<float> offsets(text.size() + 1, 0.0f);
    for (size_t i = 0; i < text.size(); ++i)
        offsets[i + 1] = offsets[i] + (IsBlank(text[i]) && text[i] != u' ' ? 0.0f : advance(text[i]));
    auto trimmed = [&](int32_t start, int32_t end) {
        while (end > start && IsBlank(text[static_cast<size_t>(end) - 1]))
            --end;
        return end;
    };
    auto span = [&](int32_t start, int32_t end) { return offsets[static_cast<size_t>(trimmed(start, end))] - offsets[static_cast<size_t>(start)]; };

    std::vector<std::pair<int32_t, int32_t>> lines;
    auto emit = [&](int32_t start, int32_t end) { lines.emplace_back(start, trimmed(start, end)); };
    UBreakIterator* breaks = LineBreaks(text);
    if (breaks == nullptr) {
        emit(0, length);
        return lines;
    }
    int32_t start = 0;
    int32_t fits = 0;
    for (int32_t boundary = ubrk_next(breaks); boundary != UBRK_DONE; boundary = ubrk_next(breaks)) {
        const int32_t status = ubrk_getRuleStatus(breaks);
        if (span(start, boundary) > width && fits > start) {
            emit(start, fits);
            start = fits;
        }
        while (span(start, boundary) > width) {
            int32_t end = start + 1;
            while (end < boundary && span(start, end + 1) <= width)
                ++end;
            if (end < boundary && U16_IS_LEAD(text[static_cast<size_t>(end) - 1]) && end - 1 > start)
                --end;
            if (end >= trimmed(start, boundary))
                break;
            emit(start, end);
            start = end;
        }
        fits = boundary;
        if (status >= UBRK_LINE_HARD && status < UBRK_LINE_HARD_LIMIT) {
            emit(start, boundary);
            start = boundary;
        }
    }
    if (start < length || lines.empty())
        emit(start, length);
    return lines;
}

}

std::string Visual(std::string_view text)
{
    if (!RightToLeft() && std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }))
        return std::string(text);
    const std::u16string logical = Shape(Utf16(text));
    UBiDi* paragraph = Paragraph(logical);
    const std::optional<std::u16string> visual = paragraph != nullptr ? Reordered(paragraph) : std::nullopt;
    return visual ? Utf8(*visual) : std::string(text);
}

std::vector<std::string> VisualLines(std::string_view text, float width, const std::function<float(char16_t)>& advance)
{
    const std::u16string logical = Shape(Utf16(text));
    const std::vector<std::pair<int32_t, int32_t>> ranges = BreakLines(logical, width, advance);
    UBiDi* paragraph = Paragraph(logical);
    UBiDi* line = Bidi(g_layout.line);
    std::vector<std::string> lines;
    lines.reserve(ranges.size());
    for (const auto& [start, end] : ranges) {
        const std::u16string_view stored(logical.data() + start, static_cast<size_t>(end - start));
        std::optional<std::u16string> visual;
        if (paragraph != nullptr && line != nullptr && end > start) {
            UErrorCode status = U_ZERO_ERROR;
            ubidi_setLine(paragraph, start, end, line, &status);
            if (!Failed(status, "ubidi_setLine"))
                visual = Reordered(line);
        }
        lines.push_back(Utf8(visual ? std::u16string_view(*visual) : stored));
    }
    return lines;
}

std::u16string DrawnCharacters(std::string_view text)
{
    const std::u16string logical = Utf16(text);
    std::u16string characters = logical + Shape(logical);
    for (char16_t c : logical) {
        const UChar32 mirrored = u_charMirror(c);
        if (mirrored != c && mirrored <= 0xFFFF)
            characters += static_cast<char16_t>(mirrored);
    }
    return characters;
}

}
