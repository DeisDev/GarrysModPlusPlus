#pragma once

namespace gmodpp::client {

// Hooks client.dll once it is loaded; safe to call every frame.
void TryInstall();

}
