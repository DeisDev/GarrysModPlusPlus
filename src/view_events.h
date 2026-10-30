#pragma once

// Called from client.dll (client_hooks.cpp) around each view's opaque pass: DrawWorld until DrawTranslucentRenderables.
// Defined by effects.cpp in d3d9.dll and native_effects.cpp in gmodpp.dll.
namespace gmodpp::effects {

void OnViewOpaqueBegin();
void OnViewOpaqueEnd(bool inSkybox);
// After DrawTranslucentRenderables: ends the forward lighting of the view's translucent surfaces.
void OnViewTranslucentEnd();

}
