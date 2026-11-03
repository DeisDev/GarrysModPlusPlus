#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace gmodpp::overlay::widgets {

// Text passed to these widgets is in stored (logical) order; they lay it out in the language's direction
// (text_layout.h) and mirror the layout for right-to-left languages.

struct Fonts {
    ImFont* body = nullptr;
    ImFont* strong = nullptr;
    ImFont* title = nullptr;
    ImFont* figure = nullptr;
};

// Loads the game's own fonts (Roboto, and Coolvetica for the title) from `fontDirectory`, with the fonts the game falls
// back to for characters they lack (overlay_fonts.h), for every character of every installed language, sized for a
// display `displayHeight` pixels tall, and sets the menu's colours and spacing. Call before the first frame.
void LoadTheme(const std::filesystem::path& fontDirectory, int displayHeight);
const Fonts& GetFonts();

// Whether the menu is laid out from right to left.
bool RightToLeft();

// The title row with a hint and a close button; returns true when the close button is clicked.
bool Header(const char* title, const char* hint);
bool NavItem(const char* label, bool selected);

// Text wrapped to the available width and aligned to the start of the line.
void Text(const char* text);
void Heading(const char* text);
// A panel with an optional heading above it. EndCard must follow every BeginCard.
void BeginCard(const char* id, const char* heading = nullptr);
void EndCard();

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
// Sliders `width` wide (0: the available width) with `text` (the value) in `textWidth` after them.
bool Slider(const char* id, float* value, float minimum, float maximum, const char* text, float textWidth, float width = 0.0f);
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
// A large figure over a short label, `width` wide.
void Tile(const char* figure, const char* label, float width);
// A thin bar filled to `fraction` of the available width from the start of the line.
void Meter(float fraction);

}
