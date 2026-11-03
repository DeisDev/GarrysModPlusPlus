#pragma once

namespace gmodpp::hardware_check {

// The launch check, run before the shader API is chosen: whether the display adapter the engine will draw on can run
// the native renderer and its ray tracing, with a warning when it can't (Settings::hardwareWarning). False when the game
// should start with its own shader API instead. Ends the process when the player chooses to quit.
bool NativeRendererChosen();

}
