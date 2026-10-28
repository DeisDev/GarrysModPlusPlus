#pragma once

#include <windows.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace gmodpp {

// Finds the function whose code loads the address of `text` (a NUL-terminated string in the module's
// read-only data) with a RIP-relative LEA. Returns nullptr when no unique match exists.
void* FindFunctionReferencingString(HMODULE module, std::string_view text);

// Every match in the module's code of `pattern`: hex bytes separated by spaces, "??" for any byte.
std::vector<const std::byte*> FindCodePattern(HMODULE module, std::string_view pattern);

}
