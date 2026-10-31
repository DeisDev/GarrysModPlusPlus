#include "overlay_widgets.h"

#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>

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
constexpr ImVec4 kAccent(0.180f, 0.608f, 1.0f, 1.0f);
constexpr ImVec4 kAccentHover(0.333f, 0.686f, 1.0f, 1.0f);
constexpr ImVec4 kAccentActive(0.122f, 0.525f, 0.902f, 1.0f);
constexpr ImVec4 kKnobOff(0.70f, 0.74f, 0.80f, 1.0f);
constexpr ImVec4 kTransparent(0.0f, 0.0f, 0.0f, 0.0f);

Fonts g_fonts;
// Font size over the 16 pixels the spacing was designed at.
float g_scale = 1.0f;

ImVec4 WithAlpha(ImVec4 color, float alpha) { return ImVec4(color.x, color.y, color.z, alpha); }

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImU32 Color(const ImVec4& color) { return ImGui::GetColorU32(color); }

ImFont* LoadFont(const std::filesystem::path& path, float pixels)
{
    const std::u8string utf8 = path.u8string();
    const char* name = reinterpret_cast<const char*>(utf8.c_str());
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        log::Info("Overlay: font {} not found; using Dear ImGui's", name);
        return nullptr;
    }
    ImFontConfig config;
    config.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(name, pixels, &config);
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
}

void SetSizes(ImGuiStyle& style)
{
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.WindowRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildRounding = 6.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 6.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.FrameRounding = 5.0f;
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(5.0f, 3.0f);
    style.ScrollbarSize = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabMinSize = 12.0f;
    style.GrabRounding = 6.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DisabledAlpha = 0.4f;
}

template <class T>
bool SliderOf(const char* id, T* value, T minimum, T maximum)
{
    constexpr bool kInteger = std::is_integral_v<T>;
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    char text[32];
    if constexpr (kInteger)
        std::snprintf(text, sizeof(text), "%d", *value);
    else
        std::snprintf(text, sizeof(text), "%.2f", *value);
    const float valueWidth = std::max(ImGui::CalcTextSize(kInteger ? "0000" : "00.00").x, ImGui::CalcTextSize(text).x);
    const float spacing = style.ItemInnerSpacing.x * 1.5f;
    const float total = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(std::max(total - valueWidth - spacing, ImGui::GetFontSize() * 4.0f));
    for (ImGuiCol hidden : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive})
        ImGui::PushStyleColor(hidden, kTransparent);
    // Whole numbers go through a float slider too: ImGui widens an integer slider's grab to one unit, which would
    // start each track at a different place.
    float position = static_cast<float>(*value);
    const bool moved = ImGui::SliderFloat("##slider", &position, static_cast<float>(minimum), static_cast<float>(maximum), "", ImGuiSliderFlags_NoInput);
    ImGui::PopStyleColor(5);
    const T previous = *value;
    if (moved) {
        if constexpr (kInteger)
            *value = std::clamp(static_cast<T>(std::lround(position)), minimum, maximum);
        else
            *value = std::round(position * 100.0f) / 100.0f;
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
    const float x = left + t * (right - left);
    const float y = (min.y + max.y) * 0.5f;
    const float track = std::max(2.0f, ImGui::GetFontSize() * 0.12f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(left, y - track), ImVec2(right, y + track), Color(kFrameActive), track);
    draw->AddRectFilled(ImVec2(left, y - track), ImVec2(x, y + track), Color(hot ? kAccentHover : kAccent), track);
    const float radius = style.GrabMinSize * 0.5f;
    draw->AddCircleFilled(ImVec2(x, y), radius, Color(kOnAccent));
    if (hot)
        draw->AddCircle(ImVec2(x, y), radius + 1.5f * g_scale, Color(WithAlpha(kAccent, 0.6f)), 0, 2.0f * g_scale);

    ImGui::SameLine(0.0f, spacing + valueWidth - ImGui::CalcTextSize(text).x);
    ImGui::TextUnformatted(text);
    ImGui::PopID();
    return changed;
}

}

void LoadTheme(const std::filesystem::path& fontDirectory, int displayHeight)
{
    const float size = std::clamp(std::round(static_cast<float>(displayHeight) * 14.0f / 1080.0f), 13.0f, 28.0f);
    g_scale = size / 16.0f;
    ImGuiIO& io = ImGui::GetIO();
    g_fonts.body = LoadFont(fontDirectory / L"Roboto-Regular.ttf", size);
    if (g_fonts.body == nullptr)
        g_fonts.body = io.Fonts->AddFontDefault();
    g_fonts.strong = LoadFont(fontDirectory / L"Roboto-Medium.ttf", size);
    g_fonts.title = LoadFont(fontDirectory / L"coolvetica.ttf", std::round(size * 1.5f));
    g_fonts.figure = LoadFont(fontDirectory / L"Roboto-Light.ttf", std::round(size * 1.6f));
    for (ImFont** font : {&g_fonts.strong, &g_fonts.title, &g_fonts.figure}) {
        if (*font == nullptr)
            *font = g_fonts.body;
    }
    io.FontDefault = g_fonts.body;

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    SetSizes(style);
    SetColors(style);
    style.ScaleAllSizes(g_scale);
    style.MouseCursorScale = std::max(1.0f, g_scale);
}

const Fonts& GetFonts() { return g_fonts; }

bool Header(const char* title, const char* hint)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = std::max(g_fonts.title->FontSize, ImGui::GetFrameHeight());

    draw->AddText(g_fonts.title, g_fonts.title->FontSize, ImVec2(origin.x, origin.y + (height - g_fonts.title->FontSize) * 0.5f), Color(kText), title);

    const float close = ImGui::GetFrameHeight();
    const float hintWidth = ImGui::CalcTextSize(hint).x;
    const float hintX = origin.x + width - close - style.ItemSpacing.x - hintWidth;
    draw->AddText(ImVec2(std::floor(hintX), std::floor(origin.y + (height - ImGui::GetFontSize()) * 0.5f)), Color(kTextDim), hint);

    ImGui::SetCursorScreenPos(ImVec2(origin.x + width - close, origin.y + (height - close) * 0.5f));
    const bool clicked = ImGui::InvisibleButton("##close", ImVec2(close, close));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 center(origin.x + width - close * 0.5f, origin.y + height * 0.5f);
    if (hovered)
        draw->AddCircleFilled(center, close * 0.5f, Color(kFrameHover));
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
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * 1.15f);
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    if (selected) {
        // The outline is drawn inside the item so its child window doesn't clip it.
        const float border = std::max(1.0f, 1.5f * g_scale);
        const float inset = border * 0.5f;
        draw->AddRectFilled(origin, end, Color(WithAlpha(kAccent, 0.16f)), style.FrameRounding);
        draw->AddRect(ImVec2(origin.x + inset, origin.y + inset), ImVec2(end.x - inset, end.y - inset), Color(kAccent), style.FrameRounding, 0, border);
    } else if (hovered) {
        draw->AddRectFilled(origin, end, Color(kSurfaceHover), style.FrameRounding);
    }
    ImFont* font = g_fonts.strong;
    const ImVec2 text(std::floor(origin.x + style.FramePadding.x * 1.4f), std::floor(origin.y + (size.y - font->FontSize) * 0.5f));
    const ImVec4 clip(origin.x, origin.y, end.x, end.y);
    draw->AddText(font, font->FontSize, text, Color(selected || hovered ? kText : kTextDim), label, nullptr, 0.0f, &clip);
    return clicked;
}

void Heading(const char* text)
{
    ImGui::PushFont(g_fonts.strong);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void BeginCard(const char* id, const char* heading)
{
    if (heading != nullptr)
        Heading(heading);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSurface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * g_scale, 8.0f * g_scale));
    ImGui::BeginChild(id, ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void EndCard()
{
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
}

bool Toggle(const char* text, bool* value)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float frame = ImGui::GetFrameHeight();
    const float height = ImGui::GetFontSize() * 1.15f;
    const float width = height * 1.8f;
    const float textWidth = ImGui::CalcTextSize(text).x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(text, ImVec2(width + style.ItemInnerSpacing.x + textWidth, frame));
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
    const ImVec2 min(origin.x, origin.y + (frame - height) * 0.5f);
    const ImVec2 max(min.x + width, min.y + height);
    draw->AddRectFilled(min, max, Color(Mix(hovered ? kFrameHover : kFrameActive, hovered ? kAccentHover : kAccent, t)), height * 0.5f);
    const float x = min.x + height * 0.5f + t * (width - height);
    draw->AddCircleFilled(ImVec2(x, min.y + height * 0.5f), height * 0.36f, Color(Mix(kKnobOff, kOnAccent, t)));
    const ImVec2 textPos(std::floor(max.x + style.ItemInnerSpacing.x), std::floor(origin.y + (frame - ImGui::GetFontSize()) * 0.5f));
    draw->AddText(textPos, Color(kText), text);
    return clicked;
}

bool Segmented(const char* id, int* value, std::span<const char* const> options, std::span<const bool> available)
{
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight();
    const float segment = width / static_cast<float>(options.size());
    const float inset = std::max(2.0f, style.FramePadding.y * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), Color(kFrame), style.FrameRounding);
    bool changed = false;
    for (size_t i = 0; i < options.size(); ++i) {
        const int index = static_cast<int>(i);
        const bool enabled = available.empty() || available[i];
        const float left = origin.x + segment * static_cast<float>(i);
        ImGui::BeginDisabled(!enabled);
        ImGui::PushID(index);
        ImGui::SetCursorScreenPos(ImVec2(left, origin.y));
        if (ImGui::InvisibleButton("##segment", ImVec2(segment, height)) && *value != index) {
            *value = index;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 min(left + inset, origin.y + inset);
        const ImVec2 max(left + segment - inset, origin.y + height - inset);
        const bool selected = *value == index;
        if (selected)
            draw->AddRectFilled(min, max, Color(kAccent), std::max(0.0f, style.FrameRounding - inset));
        else if (hovered)
            draw->AddRectFilled(min, max, Color(kFrameHover), std::max(0.0f, style.FrameRounding - inset));
        const ImVec2 size = ImGui::CalcTextSize(options[i]);
        const ImVec2 text(std::floor(std::max(min.x + style.FramePadding.x * 0.5f, (min.x + max.x - size.x) * 0.5f)), std::floor(origin.y + (height - size.y) * 0.5f));
        const ImVec4 clip(min.x, min.y, max.x, max.y);
        draw->AddText(nullptr, 0.0f, text, Color(selected ? kOnAccent : kText), options[i], nullptr, 0.0f, &clip);
        ImGui::EndDisabled();
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
}

bool Slider(const char* id, float* value, float minimum, float maximum) { return SliderOf(id, value, minimum, maximum); }

bool Slider(const char* id, int* value, int minimum, int maximum) { return SliderOf(id, value, minimum, maximum); }

void Note(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Tile(const char* figure, const char* label, float width)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 padding(10.0f * g_scale, 6.0f * g_scale);
    const float height = padding.y * 2.0f + g_fonts.figure->FontSize + ImGui::GetFontSize();
    const ImVec2 end(origin.x + width, origin.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, end, Color(kSurface), style.ChildRounding);
    draw->AddRect(origin, end, Color(kBorder), style.ChildRounding);
    const ImVec4 clip(origin.x, origin.y, end.x - padding.x * 0.5f, end.y);
    draw->AddText(g_fonts.figure, g_fonts.figure->FontSize, ImVec2(origin.x + padding.x, origin.y + padding.y), Color(kText), figure, nullptr, 0.0f, &clip);
    draw->AddText(nullptr, 0.0f, ImVec2(origin.x + padding.x, origin.y + padding.y + g_fonts.figure->FontSize), Color(kTextDim), label, nullptr, 0.0f, &clip);
    ImGui::Dummy(ImVec2(width, height));
}

void Meter(float fraction)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float frame = ImGui::GetTextLineHeight();
    const float thickness = std::max(3.0f, ImGui::GetFontSize() * 0.3f);
    const float y = origin.y + (frame - thickness) * 0.5f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + thickness), Color(kFrame), thickness);
    const float filled = width * std::clamp(fraction, 0.0f, 1.0f);
    if (filled > 0.0f)
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + std::max(filled, thickness), y + thickness), Color(WithAlpha(kAccent, 0.8f)), thickness);
    ImGui::Dummy(ImVec2(width, frame));
}

}
