#pragma once

// Real-time lighting on the native backend: the shader API's calls that start and end view captures, and the views
// client.dll reports (view_events.h). Turned on by the realtime_lighting setting.
namespace gmodpp::native::realtime {

// After IShaderAPI::SetMode has created the device and swap chain.
void OnDeviceReady();
// The game set a viewport with this depth range; the viewmodel is drawn with 0..0.1.
void OnViewport(float minDepth, float maxDepth);
// Before the back buffer is copied or read: post-processing copies it straight after the viewmodel.
void OnBackBufferRead();
// Before the frame is presented.
void OnPresent();

}
