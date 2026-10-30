#pragma once

namespace gmodpp::video_menu {

// Adds the real-time lighting settings to GMod's Advanced video options (GameUI.dll's
// COptionsSubVideoAdvancedDlg) and hides its options that do nothing natively, once GameUI.dll is loaded; safe to call
// every frame.
void TryInstall();

}
