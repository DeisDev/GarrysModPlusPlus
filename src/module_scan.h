#pragma once

#include <windows.h>

#include <string_view>

namespace gmodpp {

// Finds the function whose code loads the address of `text` (a NUL-terminated string in the module's
// read-only data) with a RIP-relative LEA. Returns nullptr when no unique match exists.
void* FindFunctionReferencingString(HMODULE module, std::string_view text);

}
