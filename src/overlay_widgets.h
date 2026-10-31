#pragma once

#include <imgui.h>

#include <filesystem>
#include <span>

namespace gmodpp::overlay::widgets {

struct Fonts {
    ImFont* body = nullptr;
    ImFont* strong = nullptr;
    ImFont* title = nullptr;
    ImFont* figure = nullptr;
};

// Loads the game's own fonts (Roboto, and Coolvetica for the title) from `fontDirectory`, sized for a display
// `displayHeight` pixels tall, and sets the menu's colours and spacing. Call before the first frame.
void LoadTheme(const std::filesystem::path& fontDirectory, int displayHeight);
const Fonts& GetFonts();

// The title row with a hint and a close button; returns true when the close button is clicked.
bool Header(const char* title, const char* hint);
bool NavItem(const char* label, bool selected);

void Heading(const char* text);
// A rounded panel with an optional heading above it. EndCard must follow every BeginCard.
void BeginCard(const char* id, const char* heading = nullptr);
void EndCard();

// A switch followed by its text; clicking either flips it.
bool Toggle(const char* text, bool* value);
// One button per option in a full-width row; options marked unavailable can't be picked.
bool Segmented(const char* id, int* value, std::span<const char* const> options, std::span<const bool> available = {});
// Full-width sliders with the value printed after them.
bool Slider(const char* id, float* value, float minimum, float maximum);
bool Slider(const char* id, int* value, int minimum, int maximum);

// Dimmed text wrapped to the available width.
void Note(const char* text);
// A large figure over a short label, `width` wide.
void Tile(const char* figure, const char* label, float width);
// A thin bar filled to `fraction` of the available width.
void Meter(float fraction);

}
