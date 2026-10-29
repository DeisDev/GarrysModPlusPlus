#include "shaderapi_native.h"

#include "log.h"

#include <windows.h>

#include <cstring>

extern "C" const void* const gmodpp_missing[];
extern "C" const DWORD gmodpp_thunk_count;

namespace gmodpp::native {

namespace {

constexpr int kInterfaceOk = 0;
constexpr int kInterfaceFailed = 1;
// GMod's vtable lengths (see shaderapi_slots.cpp), so tools that count entries see the same interfaces.
constexpr size_t kDebugTextureInfoSlots = 6;
constexpr size_t kVBAllocTrackerSlots = 3;

void CountVB(NativeObject*, void*, bool, int, int, std::uint64_t) {}
void UnCountVB(NativeObject*, void*) {}
bool TrackMeshAllocations(NativeObject*, const char*) { return false; }

NativeObject* DebugTextureInfo()
{
    static NativeClass nativeClass("IDebugTextureInfo", { shim::kDebugTextureInfoSlots, shim::kDebugTextureInfoSlotsCount }, kDebugTextureInfoSlots);
    static NativeObject object{ nativeClass.Vtable(), &nativeClass };
    return &object;
}

NativeObject* VBAllocTracker()
{
    static NativeClass nativeClass = [] {
        NativeClass c("IVBAllocTracker", { shim::kVBAllocTrackerSlots, shim::kVBAllocTrackerSlotsCount }, kVBAllocTrackerSlots);
        c.Bind("CountVB", &CountVB);
        c.Bind("UnCountVB", &UnCountVB);
        c.Bind("TrackMeshAllocations", &TrackMeshAllocations);
        return c;
    }();
    static NativeObject object{ nativeClass.Vtable(), &nativeClass };
    return &object;
}

}

NativeClass::NativeClass(const char* name, std::span<const shim::SlotInfo> slots, size_t vtableSize)
    : m_name(name), m_slots(slots), m_vtable(gmodpp_missing, gmodpp_missing + vtableSize), m_reported(std::make_unique<std::atomic<bool>[]>(vtableSize))
{
    // A null entry ends the vtable for the shim's VtableSize.
    m_vtable.push_back(nullptr);
}

void NativeClass::BindAddress(std::string_view method, const void* function, int overload)
{
    for (size_t slot = 0; slot < m_slots.size(); ++slot) {
        if (method == m_slots[slot].name && overload-- == 0) {
            m_vtable[slot] = function;
            return;
        }
    }
    log::Info("Native: {} has no method {} in shaderapi_slots.cpp", m_name, method);
}

void NativeClass::ReportMissing(unsigned slot)
{
    if (m_reported[slot].exchange(true))
        return;
    if (slot < m_slots.size())
        log::Info("Native: {}::{} (slot {}) is not implemented", m_name, m_slots[slot].name, slot);
    else
        log::Info("Native: {} slot {} is not implemented", m_name, slot);
}

void* CreateInterface(const char* name, int* returnCode)
{
    struct Entry {
        const char* name;
        NativeObject* (*get)();
    };
    static constexpr Entry kEntries[] = {
        { "ShaderDeviceMgr001", &DeviceManager },
        { "ShaderDevice001", &Device },
        { "ShaderApi030", &ShaderApi },
        { "ShaderShadow010", &ShaderShadow },
        { "MaterialSystemHardwareConfig012", &HardwareConfig },
        { "DebugTextureInfo001", &DebugTextureInfo },
        { "VBAllocTracker001", &VBAllocTracker },
    };
    for (const Entry& entry : kEntries) {
        if (name != nullptr && std::strcmp(entry.name, name) == 0) {
            if (returnCode != nullptr)
                *returnCode = kInterfaceOk;
            return entry.get();
        }
    }
    if (returnCode != nullptr)
        *returnCode = kInterfaceFailed;
    return nullptr;
}

}

extern "C" void gmodpp_ReportMissing(gmodpp::native::NativeObject* object, DWORD slot)
{
    object->nativeClass->ReportMissing(slot);
}
