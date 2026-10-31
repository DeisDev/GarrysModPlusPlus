#pragma once

namespace gmodpp::video_menu {

// Hides the MSAA and Direct3D 9Ex options of GMod's Advanced video options (GameUI.dll's COptionsSubVideoAdvancedDlg),
// which do nothing natively, and keeps MSAA off, once GameUI.dll is loaded; safe to call every frame.
void TryInstall();

}
