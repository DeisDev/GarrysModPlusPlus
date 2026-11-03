#include "overlay_widgets.h"

#include "color.h"
#include "log.h"
#include "overlay_fonts.h"
#include "text_layout.h"
#include "ui_text.h"

#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace gmodpp::overlay::widgets {

namespace {

constexpr ImVec4 kText(0.90f, 0.92f, 0.95f, 1.0f);
constexpr ImVec4 kTextDim(0.55f, 0.59f, 0.65f, 1.0f);
constexpr ImVec4 kOnAccent(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kBackground(0.063f, 0.075f, 0.094f, 0.96f);
constexpr ImVec4 kSurface(0.094f, 0.110f, 0.137f, 1.0f);
constexpr ImVec4 kSurfaceHover(0.125f, 0.141f, 0.176f, 1.0f);
constexpr ImVec4 kFrame(0.137f, 0.161f, 0.200f, 1.0f);
constexpr ImVec4 kFrameHover(0.165f, 0.192f, 0.239f, 1.0f);
constexpr ImVec4 kFrameActive(0.192f, 0.224f, 0.278f, 1.0f);
constexpr ImVec4 kBorder(1.0f, 1.0f, 1.0f, 0.07f);
constexpr ImVec4 kRowHover(1.0f, 1.0f, 1.0f, 0.035f);
constexpr ImVec4 kAccent(0.180f, 0.608f, 1.0f, 1.0f);
constexpr ImVec4 kAccentHover(0.333f, 0.686f, 1.0f, 1.0f);
constexpr ImVec4 kAccentActive(0.122f, 0.525f, 0.902f, 1.0f);
constexpr ImVec4 kKnobOff(0.70f, 0.74f, 0.80f, 1.0f);
constexpr ImVec4 kTransparent(0.0f, 0.0f, 0.0f, 0.0f);

Fonts g_fonts;
// Font size over the 16 pixels the spacing was designed at.
float g_scale = 1.0f;
// Every character the fonts load, as Dear ImGui glyph ranges; the atlas reads them until it is built.
ImVector<ImWchar> g_glyphRanges;
// Font files by path while the fonts load; Dear ImGui keeps its own copy of each.
std::map<std::filesystem::path, std::vector<char>> g_fontData;

// The row between BeginRow and EndRow.
struct Row {
    float top = 0.0f;
    float labelBottom = 0.0f;
    float labelStart = 0.0f;
    float labelEnd = 0.0f;
    float controlStart = 0.0f;
    float controlEnd = 0.0f;
    bool enabled = true;
    const char* reason = nullptr;
    RowMark mark = RowMark::None;
    const char* markNote = nullptr;
    const char* resetHint = nullptr;
};
Row g_row;

// The card being drawn: whether its table is open, and its left and right edges on screen (0, 0 outside a card).
struct Card {
    bool open = false;
    float min = 0.0f;
    float max = 0.0f;
};
Card g_card;

// A row's horizontal extent for its highlight and mark: the card's, or the window's outside a card.
std::pair<float, float> RowExtent()
{
    if (g_card.max > g_card.min)
        return {g_card.min, g_card.max};
    return {ImGui::GetWindowPos().x, ImGui::GetWindowPos().x + ImGui::GetWindowWidth()};
}

ImVec4 WithAlpha(ImVec4 color, float alpha) { return ImVec4(color.x, color.y, color.z, alpha); }

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImU32 Color(const ImVec4& color) { return ImGui::GetColorU32(color); }

const std::vector<char>& FontData(const std::filesystem::path& path)
{
    auto [entry, added] = g_fontData.try_emplace(path);
    if (added) {
        std::ifstream file(path, std::ios::binary);
        entry->second.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (entry->second.empty())
            log::Info("Overlay: font file {} could not be read", path.string());
    }
    return entry->second;
}

// A game font and the fonts the game falls back to for the characters it lacks, merged into one Dear ImGui font in
// that order (Dear ImGui, like GDI font linking, never replaces a glyph an earlier font has).
ImFont* LoadFont(const std::filesystem::path& path, float pixels, DWRITE_FONT_WEIGHT weight, std::u16string_view characters)
{
    ImFont* font = nullptr;
    for (const fonts::Face& face : fonts::GameFontChain(path, weight, characters)) {
        const std::vector<char>& data = FontData(face.path);
        if (data.empty())
            continue;
        ImFontConfig config;
        config.OversampleH = 2;
        config.FontNo = static_cast<int>(face.index);
        config.FontDataOwnedByAtlas = false;
        config.MergeMode = font != nullptr;
        ImFont* added = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<char*>(data.data()), static_cast<int>(data.size()), pixels, &config, g_glyphRanges.Data);
        if (font == nullptr)
            font = added;
    }
    return font;
}

void SetColors(ImGuiStyle& style)
{
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kTextDim;
    colors[ImGuiCol_WindowBg] = kBackground;
    colors[ImGuiCol_ChildBg] = kTransparent;
    colors[ImGuiCol_PopupBg] = WithAlpha(kSurface, 0.98f);
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_BorderShadow] = kTransparent;
    colors[ImGuiCol_FrameBg] = kFrame;
    colors[ImGuiCol_FrameBgHovered] = kFrameHover;
    colors[ImGuiCol_FrameBgActive] = kFrameActive;
    colors[ImGuiCol_TitleBg] = kBackground;
    colors[ImGuiCol_TitleBgActive] = kBackground;
    colors[ImGuiCol_TitleBgCollapsed] = kBackground;
    colors[ImGuiCol_MenuBarBg] = kSurface;
    colors[ImGuiCol_ScrollbarBg] = kTransparent;
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.18f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    colors[ImGuiCol_CheckMark] = kAccent;
    colors[ImGuiCol_SliderGrab] = kAccent;
    colors[ImGuiCol_SliderGrabActive] = kAccentActive;
    colors[ImGuiCol_Button] = kFrame;
    colors[ImGuiCol_ButtonHovered] = kFrameHover;
    colors[ImGuiCol_ButtonActive] = kFrameActive;
    colors[ImGuiCol_Header] = WithAlpha(kAccent, 0.20f);
    colors[ImGuiCol_HeaderHovered] = WithAlpha(kAccent, 0.28f);
    colors[ImGuiCol_HeaderActive] = WithAlpha(kAccent, 0.36f);
    colors[ImGuiCol_Separator] = kBorder;
    colors[ImGuiCol_SeparatorHovered] = WithAlpha(kAccent, 0.50f);
    colors[ImGuiCol_SeparatorActive] = kAccent;
    colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    colors[ImGuiCol_ResizeGripHovered] = WithAlpha(kAccent, 0.50f);
    colors[ImGuiCol_ResizeGripActive] = kAccent;
    colors[ImGuiCol_PlotLines] = kAccent;
    colors[ImGuiCol_PlotLinesHovered] = kAccentHover;
    colors[ImGuiCol_PlotHistogram] = kAccent;
    colors[ImGuiCol_PlotHistogramHovered] = kAccentHover;
    colors[ImGuiCol_TableHeaderBg] = kSurface;
    colors[ImGuiCol_TableBorderStrong] = kBorder;
    colors[ImGuiCol_TableBorderLight] = kBorder;
    colors[ImGuiCol_TableRowBg] = kTransparent;
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    colors[ImGuiCol_TextLink] = kAccent;
    colors[ImGuiCol_TextSelectedBg] = WithAlpha(kAccent, 0.35f);
    colors[ImGuiCol_NavCursor] = kAccent;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
}

void SetSizes(ImGuiStyle& style)
{
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.FrameRounding = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 8.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabMinSize = 12.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DisabledAlpha = 0.4f;
}

float SwitchHeight() { return std::round(ImGui::GetFontSize() * 1.05f); }

// `text` in display order and its width in the current font.
struct Line {
    std::string text;
    float width;
};

Line VisualLine(const char* text)
{
    std::string visual = ui::Visual(text);
    const float width = ImGui::CalcTextSize(visual.c_str()).x;
    return {std::move(visual), width};
}

// Where a line `width` wide starts in [start, end]: at the start of the line in the language's direction.
float AlignedX(float start, float end, float width) { return RightToLeft() ? end - width : start; }

// Text wrapped to `width` from the window position `start`, each line aligned to the start of the line.
void WrappedText(const char* text, float start, float width)
{
    ImFont* font = ImGui::GetFont();
    width = std::max(width, 1.0f);
    const float scale = ImGui::GetFontSize() / font->FontSize;
    for (const std::string& line : ui::VisualLines(text, width, [&](char16_t c) { return font->GetCharAdvance(static_cast<ImWchar>(c)) * scale; })) {
        ImGui::SetCursorPosX(AlignedX(start, start + width, ImGui::CalcTextSize(line.c_str()).x));
        ImGui::TextUnformatted(line.c_str());
    }
}

float ResetSize() { return std::round(ImGui::GetFrameHeight() * 0.85f); }

// A circular arrow, the reset button's icon.
void DrawResetIcon(ImDrawList* draw, ImVec2 min, float size, ImU32 color)
{
    constexpr float kPi = 3.14159265f;
    const ImVec2 center(min.x + size * 0.5f, min.y + size * 0.5f);
    const float radius = size * 0.26f;
    const float start = -0.5f * kPi;
    const float end = start + 1.55f * kPi;
    draw->PathArcTo(center, radius, start, end, 16);
    draw->PathStroke(color, 0, std::max(1.0f, 1.5f * g_scale));
    // The arrowhead points along the arc at its end.
    const ImVec2 radial(std::cos(end), std::sin(end));
    const ImVec2 tangent(-radial.y, radial.x);
    const ImVec2 base(center.x + radial.x * radius, center.y + radial.y * radius);
    const ImVec2 tip(base.x + tangent.x * size * 0.16f, base.y + tangent.y * size * 0.16f);
    const float half = size * 0.13f;
    draw->AddTriangleFilled(tip, ImVec2(base.x + radial.x * half, base.y + radial.y * half), ImVec2(base.x - radial.x * half, base.y - radial.y * half), color);
}

// A black body's colour at `kelvin`, brightest component 1, encoded with gamma 1 / 2.2 like the colours the menu stores.
std::array<float, 3> BlackbodySwatch(float kelvin)
{
    const std::array<float, 3> linear = BlackbodyColor(kelvin);
    const float brightest = std::max({linear[0], linear[1], linear[2], 1e-6f});
    std::array<float, 3> color;
    for (size_t i = 0; i < 3; ++i)
        color[i] = std::pow(linear[i] / brightest, 1.0f / 2.2f);
    return color;
}

template <class T>
bool SliderOf(const char* id, T* value, T minimum, T maximum, const char* text, float textWidth, float width)
{
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float valueWidth = std::max(textWidth, ImGui::CalcTextSize(text).x);
    const float spacing = style.ItemInnerSpacing.x * 1.5f;
    const float trackWidth = std::max(total - valueWidth - spacing, ImGui::GetFontSize() * 4.0f);
    const float trackX = rightToLeft ? origin.x + total - trackWidth : origin.x;
    ImGui::SetCursorScreenPos(ImVec2(trackX, origin.y));
    ImGui::SetNextItemWidth(trackWidth);
    for (ImGuiCol hidden : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive})
        ImGui::PushStyleColor(hidden, kTransparent);
    // Whole numbers go through a float slider too: ImGui widens an integer slider's grab to one unit, which would
    // start each track at a different place. Right to left, the slider's position runs from the maximum.
    const float low = static_cast<float>(minimum);
    const float high = static_cast<float>(maximum);
    float position = rightToLeft ? low + high - static_cast<float>(*value) : static_cast<float>(*value);
    const bool moved = ImGui::SliderFloat("##slider", &position, low, high, "", ImGuiSliderFlags_NoInput);
    ImGui::PopStyleColor(5);
    const T previous = *value;
    if (moved) {
        const float picked = rightToLeft ? low + high - position : position;
        if constexpr (std::is_integral_v<T>)
            *value = std::clamp(static_cast<T>(std::lround(picked)), minimum, maximum);
        else
            *value = std::clamp(std::round(picked * 100.0f) / 100.0f, minimum, maximum);
    }
    const bool changed = *value != previous;

    // The knob sits where ImGui's SliderBehaviorT puts its grab (2-pixel padding), so it stays under the cursor.
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    constexpr float kPadding = 2.0f;
    const float range = static_cast<float>(maximum - minimum);
    const float grab = std::min(style.GrabMinSize, max.x - min.x - kPadding * 2.0f);
    const float t = range > 0.0f ? std::clamp(static_cast<float>(*value - minimum) / range, 0.0f, 1.0f) : 0.0f;
    const float left = min.x + kPadding + grab * 0.5f;
    const float right = max.x - kPadding - grab * 0.5f;
    const float x = std::round(rightToLeft ? right - t * (right - left) : left + t * (right - left));
    const float y = std::round((min.y + max.y) * 0.5f);
    const float track = std::max(1.0f, std::round(ImGui::GetFontSize() * 0.1f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(left, y - track), ImVec2(right, y + track), Color(kFrameActive));
    const ImU32 fill = Color(hot ? kAccentHover : kAccent);
    if (rightToLeft)
        draw->AddRectFilled(ImVec2(x, y - track), ImVec2(right, y + track), fill);
    else
        draw->AddRectFilled(ImVec2(left, y - track), ImVec2(x, y + track), fill);
    const float half = std::round(style.GrabMinSize * 0.5f);
    draw->AddRectFilled(ImVec2(x - half, y - half), ImVec2(x + half, y + half), Color(kOnAccent));
    if (hot) {
        const float ring = half + std::round(2.0f * g_scale);
        draw->AddRect(ImVec2(x - ring, y - ring), ImVec2(x + ring, y + ring), Color(WithAlpha(kAccent, 0.6f)), 0.0f, 0, std::max(1.0f, 1.5f * g_scale));
    }

    // The value is right-aligned in its space, after the track in the line's direction.
    const float textX = rightToLeft ? origin.x + valueWidth - ImGui::CalcTextSize(text).x : origin.x + total - ImGui::CalcTextSize(text).x;
    draw->AddText(ImVec2(std::floor(textX), std::floor(origin.y + style.FramePadding.y)), ImGui::GetColorU32(ImGuiCol_Text), text);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(total, ImGui::GetFrameHeight()));
    ImGui::PopID();
    return changed;
}

}

void LoadTheme(const std::filesystem::path& fontDirectory, int displayHeight)
{
    const float size = std::clamp(std::round(static_cast<float>(displayHeight) * 15.0f / 1080.0f), 14.0f, 30.0f);
    g_scale = size / 16.0f;

    std::u16string characters = ui::DrawnCharacters(ui::AllText());
    for (char16_t c = 0x20; c < 0x7F; ++c)
        characters += c;
    ImFontGlyphRangesBuilder ranges;
    for (char16_t c : characters) {
        if (c >= 0x20)
            ranges.AddChar(static_cast<ImWchar>(c));
    }
    ranges.BuildRanges(&g_glyphRanges);

    ImGuiIO& io = ImGui::GetIO();
    g_fonts.body = LoadFont(fontDirectory / L"Roboto-Regular.ttf", size, DWRITE_FONT_WEIGHT_NORMAL, characters);
    if (g_fonts.body == nullptr)
        g_fonts.body = io.Fonts->AddFontDefault();
    g_fonts.strong = LoadFont(fontDirectory / L"Roboto-Medium.ttf", size, DWRITE_FONT_WEIGHT_MEDIUM, characters);
    g_fonts.title = LoadFont(fontDirectory / L"coolvetica.ttf", std::round(size * 1.5f), DWRITE_FONT_WEIGHT_NORMAL, characters);
    g_fonts.figure = LoadFont(fontDirectory / L"Roboto-Light.ttf", std::round(size * 1.6f), DWRITE_FONT_WEIGHT_LIGHT, characters);
    for (ImFont** font : {&g_fonts.strong, &g_fonts.title, &g_fonts.figure}) {
        if (*font == nullptr)
            *font = g_fonts.body;
    }
    io.FontDefault = g_fonts.body;
    g_fontData.clear();

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    SetSizes(style);
    SetColors(style);
    style.ScaleAllSizes(g_scale);
    style.MouseCursorScale = std::max(1.0f, g_scale);
}

const Fonts& GetFonts() { return g_fonts; }

bool RightToLeft() { return ui::RightToLeft(); }

bool Header(const char* title, const char* hint)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = std::max(g_fonts.title->FontSize, ImGui::GetFrameHeight());
    const float end = origin.x + width;

    const std::string titleText = ui::Visual(title);
    const float titleWidth = g_fonts.title->CalcTextSizeA(g_fonts.title->FontSize, FLT_MAX, 0.0f, titleText.c_str()).x;
    draw->AddText(g_fonts.title, g_fonts.title->FontSize, ImVec2(AlignedX(origin.x, end, titleWidth), origin.y + (height - g_fonts.title->FontSize) * 0.5f),
        Color(kText), titleText.c_str());

    // The close button and the hint before it sit at the end of the line.
    const float close = ImGui::GetFrameHeight();
    const Line hintLine = VisualLine(hint);
    const float closeX = rightToLeft ? origin.x : end - close;
    const float hintX = rightToLeft ? origin.x + close + style.ItemSpacing.x : closeX - style.ItemSpacing.x - hintLine.width;
    draw->AddText(ImVec2(std::floor(hintX), std::floor(origin.y + (height - ImGui::GetFontSize()) * 0.5f)), Color(kTextDim), hintLine.text.c_str());

    const ImVec2 closeMin(closeX, std::round(origin.y + (height - close) * 0.5f));
    ImGui::SetCursorScreenPos(closeMin);
    const bool clicked = ImGui::InvisibleButton("##close", ImVec2(close, close));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        draw->AddRectFilled(closeMin, ImVec2(closeMin.x + close, closeMin.y + close), Color(kFrameHover));
    const ImVec2 center(closeMin.x + close * 0.5f, closeMin.y + close * 0.5f);
    const float arm = close * 0.18f;
    const ImU32 cross = Color(hovered ? kText : kTextDim);
    draw->AddLine(ImVec2(center.x - arm, center.y - arm), ImVec2(center.x + arm, center.y + arm), cross, 1.6f * g_scale);
    draw->AddLine(ImVec2(center.x - arm, center.y + arm), ImVec2(center.x + arm, center.y - arm), cross, 1.6f * g_scale);

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    return clicked;
}

bool NavItem(const char* label, bool selected)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::round(ImGui::GetFrameHeight() * 1.2f));
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    if (selected) {
        const float bar = std::max(2.0f, std::round(3.0f * g_scale));
        draw->AddRectFilled(origin, end, Color(WithAlpha(kAccent, 0.16f)));
        if (rightToLeft)
            draw->AddRectFilled(ImVec2(end.x - bar, origin.y), end, Color(kAccent));
        else
            draw->AddRectFilled(origin, ImVec2(origin.x + bar, end.y), Color(kAccent));
    } else if (hovered) {
        draw->AddRectFilled(origin, end, Color(kSurfaceHover));
    }
    ImFont* font = g_fonts.strong;
    const std::string text = ui::Visual(label);
    const float textWidth = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text.c_str()).x;
    const float padding = style.FramePadding.x * 1.6f;
    const ImVec2 position(std::floor(AlignedX(origin.x + padding, end.x - padding, textWidth)), std::floor(origin.y + (size.y - font->FontSize) * 0.5f));
    const ImVec4 clip(origin.x, origin.y, end.x, end.y);
    draw->AddText(font, font->FontSize, position, Color(selected || hovered ? kText : kTextDim), text.c_str(), nullptr, 0.0f, &clip);
    return clicked;
}

void Text(const char* text) { WrappedText(text, ImGui::GetCursorPosX(), ImGui::GetContentRegionAvail().x); }

void Heading(const char* text)
{
    ImGui::PushFont(g_fonts.strong);
    Text(text);
    ImGui::PopFont();
}

// A card is a one-cell table: it takes its size in the frame its content is drawn, where a child window sizes itself
// from the frame before (and not at all while scrolled out of view), so pages would settle over a few frames.
void BeginCard(const char* id, const char* heading)
{
    if (heading != nullptr)
        Heading(heading);
    const ImVec2 padding(std::round(10.0f * g_scale), std::round(8.0f * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, padding);
    g_card.open = ImGui::BeginTable(id, 1, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_PadOuterX);
    ImGui::PopStyleVar();
    g_card.min = ImGui::GetCursorScreenPos().x;
    g_card.max = g_card.min + ImGui::GetContentRegionAvail().x;
    if (!g_card.open)
        return;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, Color(kSurface));
    g_card.min = ImGui::GetCursorScreenPos().x - padding.x;
    g_card.max = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x + padding.x;
}

void EndCard()
{
    if (g_card.open)
        ImGui::EndTable();
    g_card = {};
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
}

bool BeginRows(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
        return false;
    const bool rightToLeft = RightToLeft();
    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, rightToLeft ? 0.58f : 0.42f);
    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, rightToLeft ? 0.42f : 0.58f);
    return true;
}

void EndRows() { ImGui::EndTable(); }

void BeginRow(const char* label, bool enabled, const char* reason, RowMark mark, const char* markNote, const char* resetHint)
{
    // Right to left, the label is in the second column, on the right.
    const bool rightToLeft = RightToLeft();
    const int labelColumn = rightToLeft ? 1 : 0;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(labelColumn);
    g_row = {.top = ImGui::GetCursorScreenPos().y, .enabled = enabled, .reason = reason, .mark = mark, .markNote = markNote, .resetHint = resetHint};
    const float start = ImGui::GetCursorPosX();
    const float width = ImGui::GetContentRegionAvail().x;
    g_row.labelStart = ImGui::GetCursorScreenPos().x;
    g_row.labelEnd = g_row.labelStart + width;
    // The reset button sits at the label's end, next to the control.
    const float reserved = resetHint != nullptr ? ResetSize() + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
    ImGui::BeginDisabled(!enabled);
    ImGui::AlignTextToFramePadding();
    WrappedText(label, rightToLeft ? start + reserved : start, width - reserved);
    g_row.labelBottom = ImGui::GetItemRectMax().y;
    ImGui::TableSetColumnIndex(1 - labelColumn);
    g_row.controlStart = ImGui::GetCursorScreenPos().x;
    g_row.controlEnd = g_row.controlStart + ImGui::GetContentRegionAvail().x;
}

RowClicks EndRow(bool clickableLabel)
{
    ImGui::EndDisabled();
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    const auto [left, right] = RowExtent();
    const ImVec2 min(left, g_row.top - style.CellPadding.y);
    const ImVec2 max(right, std::max(g_row.labelBottom, ImGui::GetItemRectMax().y) + style.CellPadding.y);
    RowClicks clicks;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (g_row.mark != RowMark::None) {
        // A bar on the row's starting edge, in the card's padding.
        const float bar = std::max(2.0f, std::round(3.0f * g_scale));
        const ImU32 color = Color(g_row.mark == RowMark::Map ? kAccent : WithAlpha(kAccent, 0.4f));
        draw->PushClipRect(min, max, false);
        if (rightToLeft)
            draw->AddRectFilled(ImVec2(max.x - bar, min.y), max, color);
        else
            draw->AddRectFilled(min, ImVec2(min.x + bar, max.y), color);
        draw->PopClipRect();
    }
    bool resetHovered = false;
    if (g_row.resetHint != nullptr) {
        ImGui::TableSetColumnIndex(rightToLeft ? 1 : 0);
        const float size = ResetSize();
        const ImVec2 position(rightToLeft ? g_row.labelStart : g_row.labelEnd - size, std::round(g_row.top + (ImGui::GetFrameHeight() - size) * 0.5f));
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(position);
        clicks.reset = ImGui::InvisibleButton("##reset", ImVec2(size, size));
        resetHovered = ImGui::IsItemHovered();
        if (resetHovered) {
            draw->AddRectFilled(position, ImVec2(position.x + size, position.y + size), Color(kFrameHover));
            ImGui::SetTooltip("%s", ui::Visual(g_row.resetHint).c_str());
        }
        DrawResetIcon(draw, position, size, Color(resetHovered ? kText : kTextDim));
        ImGui::SetCursorScreenPos(cursor);
    }
    if (!ImGui::IsWindowHovered() || !ImGui::IsMouseHoveringRect(min, max, false))
        return clicks;
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Color(kRowHover));
    if (resetHovered)
        return clicks;
    if (!g_row.enabled) {
        if (g_row.reason != nullptr)
            ImGui::SetTooltip("%s", ui::Visual(g_row.reason).c_str());
        return clicks;
    }
    const float mouse = ImGui::GetIO().MousePos.x;
    const bool onLabel = rightToLeft ? mouse > g_row.controlEnd + style.CellPadding.x : mouse < g_row.controlStart - style.CellPadding.x;
    if (onLabel && g_row.markNote != nullptr)
        ImGui::SetTooltip("%s", ui::Visual(g_row.markNote).c_str());
    if (!clickableLabel || !onLabel)
        return clicks;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    clicks.label = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    return clicks;
}

bool Switch(const char* id, bool* value)
{
    const float frame = ImGui::GetFrameHeight();
    const float height = SwitchHeight();
    const float width = SwitchWidth();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, frame));
    if (clicked)
        *value = !*value;
    const bool hovered = ImGui::IsItemHovered();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetItemID();
    const float target = *value ? 1.0f : 0.0f;
    const float step = ImGui::GetIO().DeltaTime * 10.0f;
    float t = storage->GetFloat(key, target);
    t = target > t ? std::min(target, t + step) : std::max(target, t - step);
    storage->SetFloat(key, t);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min(origin.x, std::round(origin.y + (frame - height) * 0.5f));
    const ImVec2 max(min.x + width, min.y + height);
    draw->AddRectFilled(min, max, Color(Mix(hovered ? kFrameHover : kFrameActive, hovered ? kAccentHover : kAccent, t)));
    const float inset = std::max(2.0f, std::round(height * 0.15f));
    const float knob = height - inset * 2.0f;
    // On is at the end of the line: right, or left for right-to-left languages.
    const float travel = RightToLeft() ? 1.0f - t : t;
    const float x = std::round(min.x + inset + travel * (width - height));
    draw->AddRectFilled(ImVec2(x, min.y + inset), ImVec2(x + knob, min.y + inset + knob), Color(Mix(kKnobOff, kOnAccent, t)));
    return clicked;
}

float SwitchWidth() { return std::round(SwitchHeight() * 1.9f); }

float SwitchSpan() { return SwitchWidth() + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f; }

bool Segmented(const char* id, int* value, std::span<const char* const> options, std::span<const bool> available)
{
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    const float segment = width / static_cast<float>(options.size());
    const float inset = std::max(2.0f, std::round(style.FramePadding.y * 0.5f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), Color(kFrame));
    bool changed = false;
    for (size_t i = 0; i < options.size(); ++i) {
        const int index = static_cast<int>(i);
        const bool enabled = available.empty() || available[i];
        const size_t slot = rightToLeft ? options.size() - 1 - i : i;
        const float left = std::round(origin.x + segment * static_cast<float>(slot));
        const float right = std::round(origin.x + segment * static_cast<float>(slot + 1));
        ImGui::BeginDisabled(!enabled);
        ImGui::PushID(index);
        ImGui::SetCursorScreenPos(ImVec2(left, origin.y));
        if (ImGui::InvisibleButton("##segment", ImVec2(right - left, height)) && *value != index) {
            *value = index;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 min(left + inset, origin.y + inset);
        const ImVec2 max(right - inset, origin.y + height - inset);
        const bool selected = *value == index;
        if (selected)
            draw->AddRectFilled(min, max, Color(kAccent));
        else if (hovered)
            draw->AddRectFilled(min, max, Color(kFrameHover));
        const Line line = VisualLine(options[i]);
        const float padding = style.FramePadding.x * 0.5f;
        const float centred = std::max(min.x + padding, (min.x + max.x - line.width) * 0.5f);
        const float x = rightToLeft ? std::min(max.x - padding - line.width, centred) : centred;
        const ImVec2 position(std::floor(x), std::floor(origin.y + (height - ImGui::GetFontSize()) * 0.5f));
        const ImVec4 clip(min.x, min.y, max.x, max.y);
        draw->AddText(nullptr, 0.0f, position, Color(selected ? kOnAccent : kText), line.text.c_str(), nullptr, 0.0f, &clip);
        ImGui::EndDisabled();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
}

bool Combo(const char* id, int* value, std::span<const char* const> options)
{
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    if (ImGui::InvisibleButton("##combo", ImVec2(width, height)))
        ImGui::OpenPopup("##options");
    const bool hovered = ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen("##options");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, end, Color(open ? kFrameActive : hovered ? kFrameHover : kFrame));

    // The arrow is at the end of the line, the choice at its start.
    const float arrowX = rightToLeft ? origin.x : end.x - height;
    const ImVec2 arrow(arrowX + height * 0.5f, origin.y + height * 0.5f);
    const float arm = std::round(height * 0.16f);
    draw->AddTriangleFilled(ImVec2(arrow.x - arm, arrow.y - arm * 0.5f), ImVec2(arrow.x + arm, arrow.y - arm * 0.5f), ImVec2(arrow.x, arrow.y + arm * 0.6f),
        ImGui::GetColorU32(ImGuiCol_Text));
    const int current = std::clamp(*value, 0, static_cast<int>(options.size()) - 1);
    const Line line = VisualLine(options[static_cast<size_t>(current)]);
    const float textStart = rightToLeft ? origin.x + height : origin.x + style.FramePadding.x;
    const float textEnd = rightToLeft ? end.x - style.FramePadding.x : end.x - height;
    const ImVec4 clip(textStart, origin.y, textEnd, end.y);
    draw->AddText(nullptr, 0.0f, ImVec2(std::floor(AlignedX(textStart, textEnd, line.width)), std::floor(origin.y + style.FramePadding.y)),
        ImGui::GetColorU32(ImGuiCol_Text), line.text.c_str(), nullptr, 0.0f, &clip);

    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(origin.x, end.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    if (ImGui::BeginPopup("##options", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(rightToLeft ? 1.0f : 0.0f, 0.5f));
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(ui::Visual(options[static_cast<size_t>(i)]).c_str(), i == current) && i != current) {
                *value = i;
                changed = true;
            }
            if (i == current && ImGui::IsWindowAppearing())
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
}

bool Slider(const char* id, float* value, float minimum, float maximum, const char* text, float textWidth, float width)
{
    return SliderOf(id, value, minimum, maximum, text, textWidth, width);
}

bool Slider(const char* id, int* value, int minimum, int maximum, const char* text, float textWidth, float width)
{
    return SliderOf(id, value, minimum, maximum, text, textWidth, width);
}

bool EndButton(const char* label)
{
    const std::string text = ui::Visual(label);
    const float width = ImGui::CalcTextSize(text.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (!RightToLeft())
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    return ImGui::Button(text.c_str());
}

float ButtonWidth(const char* label) { return ImGui::CalcTextSize(ui::Visual(label).c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f; }

bool Button(const char* label, bool primary, float width)
{
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text, kOnAccent);
    }
    const bool clicked = ImGui::Button(ui::Visual(label).c_str(), ImVec2(width, 0.0f));
    if (primary)
        ImGui::PopStyleColor(4);
    return clicked;
}

bool TextField(const char* id, char* buffer, size_t size, const char* hint)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputTextWithHint(id, ui::Visual(hint).c_str(), buffer, size, ImGuiInputTextFlags_EnterReturnsTrue);
}

bool Toggles(const char* id, uint32_t* mask, std::span<const char* const> options)
{
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    const float segment = width / static_cast<float>(options.size());
    const float inset = std::max(2.0f, std::round(style.FramePadding.y * 0.5f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), Color(kFrame));
    bool changed = false;
    for (size_t i = 0; i < options.size(); ++i) {
        const uint32_t bit = 1u << i;
        const size_t slot = rightToLeft ? options.size() - 1 - i : i;
        const float left = std::round(origin.x + segment * static_cast<float>(slot));
        const float right = std::round(origin.x + segment * static_cast<float>(slot + 1));
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos(ImVec2(left, origin.y));
        if (ImGui::InvisibleButton("##toggle", ImVec2(right - left, height))) {
            *mask ^= bit;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 min(left + inset, origin.y + inset);
        const ImVec2 max(right - inset, origin.y + height - inset);
        const bool on = (*mask & bit) != 0;
        if (on)
            draw->AddRectFilled(min, max, Color(hovered ? kAccentHover : kAccent));
        else if (hovered)
            draw->AddRectFilled(min, max, Color(kFrameHover));
        const Line line = VisualLine(options[i]);
        const float padding = style.FramePadding.x * 0.5f;
        const float centred = std::max(min.x + padding, (min.x + max.x - line.width) * 0.5f);
        const float x = rightToLeft ? std::min(max.x - padding - line.width, centred) : centred;
        const ImVec4 clip(min.x, min.y, max.x, max.y);
        draw->AddText(nullptr, 0.0f, ImVec2(std::floor(x), std::floor(origin.y + (height - ImGui::GetFontSize()) * 0.5f)), Color(on ? kOnAccent : kText),
            line.text.c_str(), nullptr, 0.0f, &clip);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
}

bool ColorButton(const char* id, std::array<float, 3>* color, const char* temperatureLabel)
{
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool rightToLeft = RightToLeft();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    if (ImGui::InvisibleButton("##swatch", ImVec2(width, height)))
        ImGui::OpenPopup("##picker");
    const bool hovered = ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen("##picker");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, end, Color(open ? kFrameActive : hovered ? kFrameHover : kFrame));
    // The colour fills a block at the start of the line and its hex code follows it.
    const float inset = std::max(2.0f, std::round(style.FramePadding.y * 0.6f));
    const float block = std::round(height * 2.4f);
    const ImVec2 blockMin(rightToLeft ? end.x - inset - block : origin.x + inset, origin.y + inset);
    const ImVec2 blockMax(blockMin.x + block, end.y - inset);
    draw->AddRectFilled(blockMin, blockMax, ImGui::ColorConvertFloat4ToU32(ImVec4((*color)[0], (*color)[1], (*color)[2], 1.0f)));
    draw->AddRect(blockMin, blockMax, Color(kBorder));
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(std::lround((*color)[0] * 255.0f)), static_cast<int>(std::lround((*color)[1] * 255.0f)),
        static_cast<int>(std::lround((*color)[2] * 255.0f)));
    const float hexWidth = ImGui::CalcTextSize(hex).x;
    const float hexX = rightToLeft ? blockMin.x - style.ItemInnerSpacing.x * 1.5f - hexWidth : blockMax.x + style.ItemInnerSpacing.x * 1.5f;
    draw->AddText(ImVec2(std::floor(hexX), std::floor(origin.y + style.FramePadding.y)), ImGui::GetColorU32(ImGuiCol_Text), hex);

    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(rightToLeft ? end.x : origin.x, end.y), ImGuiCond_Always, ImVec2(rightToLeft ? 1.0f : 0.0f, 0.0f));
    if (ImGui::BeginPopup("##picker", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        const float pickerWidth = std::round(ImGui::GetFontSize() * 14.0f);
        ImGui::SetNextItemWidth(pickerWidth);
        constexpr ImGuiColorEditFlags kFlags = ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha |
            ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueBar;
        changed = ImGui::ColorPicker3("##colour", color->data(), kFlags);
        if (temperatureLabel != nullptr) {
            // The temperature is the slider's own: picking a colour by hand leaves it where it was.
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const ImGuiID key = ImGui::GetID("##kelvin");
            float kelvin = storage->GetFloat(key, 6500.0f);
            ImGui::Spacing();
            Text(temperatureLabel);
            const std::string value = ui::Format("menu.kelvin", {{"value", ui::Number(kelvin, 0)}});
            const float valueWidth = ImGui::CalcTextSize(ui::Format("menu.kelvin", {{"value", ui::Number(12000.0, 0)}}).c_str()).x;
            if (Slider("##temperature", &kelvin, 1700.0f, 12000.0f, value.c_str(), valueWidth, pickerWidth)) {
                kelvin = std::round(kelvin / 50.0f) * 50.0f;
                *color = BlackbodySwatch(kelvin);
                changed = true;
            }
            storage->SetFloat(key, kelvin);
        }
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
}

void Note(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    Text(text);
    ImGui::PopStyleColor();
}

void Tile(const char* figure, const char* label, float width)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 padding(std::round(10.0f * g_scale), std::round(6.0f * g_scale));
    const float height = padding.y * 2.0f + g_fonts.figure->FontSize + ImGui::GetFontSize();
    const ImVec2 end(origin.x + width, origin.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, end, Color(kSurface));
    draw->AddRect(origin, end, Color(kBorder));
    const ImVec4 clip(origin.x + padding.x * 0.5f, origin.y, end.x - padding.x * 0.5f, end.y);
    const std::string figureText = ui::Visual(figure);
    const float figureWidth = g_fonts.figure->CalcTextSizeA(g_fonts.figure->FontSize, FLT_MAX, 0.0f, figureText.c_str()).x;
    draw->AddText(g_fonts.figure, g_fonts.figure->FontSize, ImVec2(AlignedX(origin.x + padding.x, end.x - padding.x, figureWidth), origin.y + padding.y), Color(kText),
        figureText.c_str(), nullptr, 0.0f, &clip);
    const Line line = VisualLine(label);
    draw->AddText(nullptr, 0.0f, ImVec2(AlignedX(origin.x + padding.x, end.x - padding.x, line.width), origin.y + padding.y + g_fonts.figure->FontSize),
        Color(kTextDim), line.text.c_str(), nullptr, 0.0f, &clip);
    ImGui::Dummy(ImVec2(width, height));
}

void Meter(float fraction)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float frame = ImGui::GetTextLineHeight();
    const float thickness = std::max(3.0f, std::round(ImGui::GetFontSize() * 0.3f));
    const float y = std::round(origin.y + (frame - thickness) * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + thickness), Color(kFrame));
    const float filled = std::max(width * std::clamp(fraction, 0.0f, 1.0f), 1.0f);
    if (fraction > 0.0f) {
        const float start = RightToLeft() ? origin.x + width - filled : origin.x;
        draw->AddRectFilled(ImVec2(start, y), ImVec2(start + filled, y + thickness), Color(WithAlpha(kAccent, 0.8f)));
    }
    ImGui::Dummy(ImVec2(width, frame));
}

}
