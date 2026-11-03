#pragma once

namespace gmodpp::client {

// Hooks client.dll once it is loaded; safe to call every frame.
void TryInstall();

// Whether the game's Lua hid its HUD within the last second (GM:HUDShouldDraw answered no for CHudGMod, as the camera
// and HUD-hiding addons do); false when the hook is unavailable.
bool HudHidden();

}
