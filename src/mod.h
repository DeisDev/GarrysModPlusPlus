#pragma once

#include "options.h"

#include <windows.h>

namespace gmodpp {

HMODULE SystemD3D9();

template <class Fn>
Fn SystemProc(const char* name)
{
    HMODULE module = SystemD3D9();
    return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

}
