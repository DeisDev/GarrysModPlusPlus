#pragma once

namespace gmodpp::exposure {

// Hooks materialsystem's tone mapping (CMatRenderContext's SetGoalToneMappingScale, TurnOnToneMapping and
// ResetToneMappingScale) so the menu's auto exposure settings tune Source's own: its goal biased and kept within limits,
// and reached at a chosen speed. Settings::exposureAdaptation off leaves Source's as it is. Once; logs what fails.
void TryInstall();

}
