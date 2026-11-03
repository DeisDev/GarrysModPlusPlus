#pragma once

#include <windows.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace gmodpp {

// Finds the function whose code loads the address of `text` (a NUL-terminated string in the module's
// read-only data) with a RIP-relative LEA. Returns nullptr when no unique match exists.
void* FindFunctionReferencingString(HMODULE module, std::string_view text);

// The RIP-relative LEA instructions in the module's code that load the address of `text`.
std::vector<const std::byte*> FindStringReferences(HMODULE module, std::string_view text);

// Every match in the module's code of `pattern`: hex bytes separated by spaces, "??" for any byte.
std::vector<const std::byte*> FindCodePattern(HMODULE module, std::string_view pattern);
// The function holding the only match of `pattern` (as FindCodePattern takes it), or nullptr without exactly one.
void* FindFunctionWithPattern(HMODULE module, std::string_view pattern);

// The virtual function table of the class with the RTTI name `decoratedName` (".?AVCMeshDX8@@") for its subobject at
// `offset`, or nullptr when the module has no unique one.
const void* const* FindVtable(HMODULE module, std::string_view decoratedName, DWORD offset);

}
