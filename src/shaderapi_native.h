#pragma once

#include "shaderapi_slots.h"

#include <atomic>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// The native Direct3D 12 shader API: gmodpp.dll's own implementations of the interfaces shaderapidx9.dll exports.
// Each object starts with a vtable in GMod's slot order (shaderapi_slots.cpp). Every slot defaults to a stub that
// logs the method's name once and returns 0; implementations are bound by method name. They are free functions whose
// first parameter is the object, which matches MSVC's x64 member calls except for methods returning a class by value:
// those take the result pointer as their second parameter and return it.

namespace gmodpp::native {

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

class NativeClass;

struct NativeObject {
    const void* const* vtable;
    NativeClass* nativeClass;
};
static_assert(sizeof(NativeObject) == 16, "shaderapi_thunks.asm's missing stubs pass the object to gmodpp_ReportMissing");

class NativeClass {
public:
    NativeClass(const char* name, std::span<const shim::SlotInfo> slots, size_t vtableSize);

    template <class Function>
    void Bind(std::string_view method, Function function, int overload = 0)
    {
        BindAddress(method, reinterpret_cast<const void*>(function), overload);
    }
    // For the classes' own slots past the SDK interface.
    template <class Function>
    void BindSlot(unsigned slot, Function function)
    {
        m_vtable[slot] = reinterpret_cast<const void*>(function);
    }

    const void* const* Vtable() const { return m_vtable.data(); }
    void ReportMissing(unsigned slot);

private:
    void BindAddress(std::string_view method, const void* function, int overload);

    const char* m_name;
    std::span<const shim::SlotInfo> m_slots;
    std::vector<const void*> m_vtable;
    std::unique_ptr<std::atomic<bool>[]> m_reported;
};

// The interfaces' factory, also returned by IShaderDeviceMgr::SetMode.
void* CreateInterface(const char* name, int* returnCode);

// Each interface's singleton, defined with its implementation.
NativeObject* DeviceManager();
NativeObject* Device();
NativeObject* ShaderApi();
NativeObject* ShaderShadow();
NativeObject* HardwareConfig();

}
