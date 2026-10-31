#pragma once

// Keeps the game's loading screen up until the map's ray-traced lighting is ready, with its progress on the loading
// dialog's bar.
namespace gmodpp::loading {

// Hooks the engine's loading progress once engine.dll is loaded; safe to call every frame.
void TryInstall();
// Whether a loading screen update is running: frames presented meanwhile belong to the loading screen.
bool Active();

}
