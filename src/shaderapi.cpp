#include "shaderapi_device.h"

#include <mutex>

namespace gmodpp::native {

namespace {

constexpr size_t kShaderApiSlots = 289;
constexpr size_t kShaderShadowSlots = 57;
// IShaderUtil (materialsystem.dll), in the SDK's order (shaderapi_slots.cpp).
constexpr unsigned kShaderUtilOnFlushBufferedPrimitives = 25;

struct ShaderApiObject : NativeObject {
    std::recursive_mutex mutex;
    bool mutexEnabled = false;
};

template <class Fn>
Fn ShaderUtilSlot(unsigned slot)
{
    void* util = GetBackend().shaderUtil;
    return reinterpret_cast<Fn>((*static_cast<void***>(util))[slot]);
}

bool SetMode(ShaderApiObject*, void* window, int, const ShaderDeviceInfo& info)
{
    return GetBackend().SetMode(static_cast<HWND>(window), info);
}

void EnableShaderShaderMutex(ShaderApiObject* self, bool enable) { self->mutexEnabled = enable; }

void ShaderLock(ShaderApiObject* self)
{
    if (self->mutexEnabled)
        self->mutex.lock();
}

void ShaderUnlock(ShaderApiObject* self)
{
    if (self->mutexEnabled)
        self->mutex.unlock();
}

void FlushBufferedPrimitives(ShaderApiObject*)
{
    if (GetBackend().shaderUtil != nullptr)
        ShaderUtilSlot<bool (*)(void*)>(kShaderUtilOnFlushBufferedPrimitives)(GetBackend().shaderUtil);
}

// shaderapidx9.dll 0x180011840: whether the device exists and is not deactivated.
bool CanDownloadTextures(ShaderApiObject*) { return GetBackend().HasDevice(); }
bool DoRenderTargetsNeedSeparateDepthBuffer(ShaderApiObject*) { return false; }

void GetBackBufferDimensions(ShaderApiObject*, int& width, int& height)
{
    width = static_cast<int>(GetBackend().Width());
    height = static_cast<int>(GetBackend().Height());
}

}

NativeObject* ShaderApi()
{
    static NativeClass nativeClass = [] {
        NativeClass c("IShaderAPI", { shim::kShaderApiSlots, shim::kShaderApiSlotsCount }, kShaderApiSlots);
        c.Bind("SetMode", &SetMode);
        c.Bind("EnableShaderShaderMutex", &EnableShaderShaderMutex);
        c.Bind("ShaderLock", &ShaderLock);
        c.Bind("ShaderUnlock", &ShaderUnlock);
        c.Bind("FlushBufferedPrimitives", &FlushBufferedPrimitives);
        c.Bind("CanDownloadTextures", &CanDownloadTextures);
        c.Bind("DoRenderTargetsNeedSeparateDepthBuffer", &DoRenderTargetsNeedSeparateDepthBuffer);
        c.Bind("GetBackBufferDimensions", &GetBackBufferDimensions);
        return c;
    }();
    static ShaderApiObject object{ { nativeClass.Vtable(), &nativeClass } };
    return &object;
}

NativeObject* ShaderShadow()
{
    static NativeClass nativeClass("IShaderShadow", { shim::kShaderShadowSlots, shim::kShaderShadowSlotsCount }, kShaderShadowSlots);
    static NativeObject object{ nativeClass.Vtable(), &nativeClass };
    return &object;
}

}
