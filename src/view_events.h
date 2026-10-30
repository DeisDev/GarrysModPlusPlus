#pragma once

// Called from client.dll (client_hooks.cpp) around each view's opaque pass: DrawWorld until DrawTranslucentRenderables.
// Defined in native_lighting.cpp.
namespace gmodpp::effects {

void OnViewOpaqueBegin();
void OnViewOpaqueEnd(bool inSkybox);
// After DrawTranslucentRenderables: ends the forward lighting of the view's translucent surfaces.
void OnViewTranslucentEnd();

}
