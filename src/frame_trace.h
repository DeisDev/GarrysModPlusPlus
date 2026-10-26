#pragma once

#include "log.h"

#include <d3d9.h>

#include <atomic>
#include <string>

namespace gmodpp::trace {

inline std::atomic<bool> g_active = false;

inline bool Active() { return g_active.load(std::memory_order_relaxed); }

void OnFrameBoundary();
std::string Describe(IDirect3DSurface9* surface);
void Draw(const char* kind, UINT count);

template <class... Args>
void Line(std::format_string<Args...> fmt, Args&&... args)
{
    Draw(nullptr, 0);
    log::Write(std::string("TRACE ") + std::format(fmt, std::forward<Args>(args)...));
}

}
