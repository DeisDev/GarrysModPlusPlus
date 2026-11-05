#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace gmodpp::overlay::widgets {

// Text passed to these widgets is in stored (logical) order; they lay it out in the language's direction
// (text_layout.h) and mirror the layout for right-to-left languages.

struct Fonts {
    ImFont* body = nullptr;
    ImFont* strong = nullptr;
    ImFont* title = nullptr;
    ImFont* figure = nullptr;
    // The Lucide icon font at the body size, or null when its file is missing (icons then fall back to text).
    ImFont* icons = nullptr;
};

// The menu's icons: Lucide glyphs (fonts/lucide.ttf) by the codepoints of the font release's codepoints.json, as UTF-8.
namespace icons {
inline constexpr char kRotateCcw[] = "\xEE\x85\x88";
inline constexpr char kCircleHelp[] = "\xEE\x82\x82";
inline constexpr char kChevronDown[] = "\xEE\x81\xAD";
inline constexpr char kChevronRight[] = "\xEE\x81\xAF";
inline constexpr char kClose[] = "\xEE\x86\xB2";
inline constexpr char kExternalLink[] = "\xEE\x82\xB9";
}

// Loads the game's own fonts (Roboto, and Coolvetica for the title) from `fontDirectory`, with the fonts the game falls
// back to for characters they lack (overlay_fonts.h), for every character of every installed language, sized for a
// display `displayHeight` pixels tall times `scale`, and sets the menu's colours and spacing. Call before the first
// frame, and again (between frames, with the font atlas cleared) to change the scale.
void LoadTheme(const std::filesystem::path& fontDirectory, const std::filesystem::path& iconFont, int displayHeight, float scale);
const Fonts& GetFonts();
// Draws `icon` centred on `center`, `size` pixels tall.
void DrawIcon(ImDrawList* draw, const char* icon, ImVec2 center, ImU32 color, float size);

// The theme's colours that pages draw with themselves.
struct Palette {
    ImVec4 text;
    ImVec4 textDim;
    ImVec4 surface;
    ImVec4 frame;
    ImVec4 accent;
    // Log lines that warn or report a failure.
    ImVec4 warning;
    ImVec4 error;
};
const Palette& GetPalette();
// Recolours the accent (sRGB components) and the menu window's opacity; cheap to call every frame.
void SetAccent(const std::array<float, 3>& color);
void SetOpacity(float opacity);
// The info boxes' opacity: a little more see-through than the menu's.
float InfoBoxAlpha();

// Whether the menu is laid out from right to left.
bool RightToLeft();

// The title row with a hint and a close button; returns true when the close button is clicked.
bool Header(const char* title, const char* hint);
bool NavItem(const char* label, bool selected);

// Text wrapped to the available width and aligned to the start of the line.
void Text(const char* text);
void Heading(const char* text);
// A panel with an optional heading above it, and with `help` a help mark at the heading's end that explains the card
// in a tooltip. A card with a heading collapses and expands when the heading is clicked; returns whether its body is
// shown. EndCard must follow every BeginCard, whatever it returned.
bool BeginCard(const char* id, const char* heading = nullptr, const char* help = nullptr);
void EndCard();
// The collapsed cards as text (their ids in hex, comma-separated), to keep between sessions, and setting them back.
std::string CollapsedCards();
void SetCollapsedCards(std::string_view text);
// A small "?" at the end of the current line that shows `text` in a tooltip when hovered.
void HelpMark(const char* text);
// The width a help mark and the gap before it take.
float HelpMarkSpan();
// A tooltip of `text`, wrapped to a short paragraph.
void Tooltip(const char* text);

// How a setting row is marked: not at all, as set by the current map (while editing the map's settings), or as set
// differently on the current map (while editing the settings of all maps), which `markNote` explains on hover.
enum class RowMark { None, Map, MapElsewhere };

// What was clicked in a row: its label (which counts as a click on its control) or its reset button.
struct RowClicks {
    bool label = false;
    bool reset = false;
};

// Setting rows: the label at the start of the line, the control after it. BeginRow leaves the cursor in the control
// column; a row that is not `enabled` is greyed out and shows `reason` when hovered. A row with a `resetHint` has a reset
// button after its label, which works in greyed-out rows too and shows the hint on hover.
bool BeginRows(const char* id);
void EndRows();
void BeginRow(const char* label, bool enabled = true, const char* reason = nullptr, RowMark mark = RowMark::None, const char* markNote = nullptr,
    const char* resetHint = nullptr);
RowClicks EndRow(bool clickableLabel = false);

// An on/off switch.
bool Switch(const char* id, bool* value);
float SwitchWidth();
// The width of a switch and the gap after it.
float SwitchSpan();
// One button per option in a full-width row; options marked unavailable can't be picked.
bool Segmented(const char* id, int* value, std::span<const char* const> options, std::span<const bool> available = {});
// A full-width drop-down list.
bool Combo(const char* id, int* value, std::span<const char* const> options);
// Sliders `width` wide (0: the available width) with `text` (the value) in `textWidth` after them. The arrow keys move
// the slider under the mouse, or the one last clicked, by `step` (whole sliders by 1; Shift: ten steps), and clicking the
// value types one in.
bool Slider(const char* id, float* value, float minimum, float maximum, float step, const char* text, float textWidth, float width = 0.0f);
bool Slider(const char* id, int* value, int minimum, int maximum, const char* text, float textWidth, float width = 0.0f);
// A button at the end of the line.
bool EndButton(const char* label);
// A button `width` wide (0: as wide as its text); a primary one is filled with the accent colour.
bool Button(const char* label, bool primary = false, float width = 0.0f);
// The width Button gives `label`.
float ButtonWidth(const char* label);
// A single-line text field across the available width, showing `hint` while empty. Returns true when Enter is pressed.
bool TextField(const char* id, char* buffer, size_t size, const char* hint);
// One button per option in a full-width row, each turning its bit of `mask` on and off.
bool Toggles(const char* id, uint32_t* mask, std::span<const char* const> options);
// A colour swatch across the available width that opens a colour picker; with `temperatureLabel`, the picker also sets
// the colour of a black body's light from a temperature slider labelled with it.
bool ColorButton(const char* id, std::array<float, 3>* color, const char* temperatureLabel = nullptr);

// Dimmed text wrapped to the available width.
void Note(const char* text);
// A link: accent text, underlined when hovered; returns true when clicked.
bool Link(const char* label);
// A large figure over a short label, `width` wide.
void Tile(const char* figure, const char* label, float width);
// A thin bar filled to `fraction` of the available width from the start of the line.
void Meter(float fraction);

}
