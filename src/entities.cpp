#include "entities.h"

#include "log.h"

#include <windows.h>

#include <cstring>

namespace gmodpp::entities {

namespace {

using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* returnCode);

// Vtable slots follow the Source SDK 2013 declarations (IClientEntityList, IClientUnknown,
// IClientRenderable, IVModelInfo, IVEngineClient013) and were confirmed in GMod's client.dll and
// engine.dll with Ghidra. GMod's SetupBones and IVEngineClient::Time use double-precision time.
constexpr int kGetClientEntitySlot = 3;
constexpr int kGetHighestEntityIndexSlot = 6;
constexpr int kGetClientRenderableSlot = 5;
constexpr int kShouldDrawSlot = 3;
constexpr int kGetModelSlot = 9;
constexpr int kRenderableToWorldTransformSlot = 34;
constexpr int kGetModelNameSlot = 3;
constexpr int kGetModelTypeSlot = 9;
// modtype_t: mod_bad, mod_brush, mod_sprite, mod_studio.
constexpr int kModelTypeBrush = 1;
constexpr int kModelTypeStudio = 3;
constexpr int kSetupBonesSlot = 16;
constexpr int kEngineTimeSlot = 14;
constexpr int kBoneUsedByAnything = 0x0007FF00;

void* g_entityList = nullptr;
void* g_modelInfo = nullptr;
void* g_engineClient = nullptr;
bool g_attempted = false;

void* Create(const wchar_t* moduleName, const char* name)
{
    HMODULE module = GetModuleHandleW(moduleName);
    auto create = module ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface")) : nullptr;
    int code = 0;
    return create ? create(name, &code) : nullptr;
}

template <class Fn>
Fn Slot(void* object, int slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot]);
}

}

void TryInitialize()
{
    if (g_attempted || GetModuleHandleW(L"client.dll") == nullptr || GetModuleHandleW(L"engine.dll") == nullptr)
        return;
    g_attempted = true;
    void* entityList = Create(L"client.dll", "VClientEntityList003");
    void* modelInfo = Create(L"engine.dll", "VModelInfoClient006");
    if (entityList == nullptr || modelInfo == nullptr) {
        log::Info("Entity interfaces unavailable; dynamic shadow casters disabled");
        return;
    }
    g_entityList = entityList;
    g_modelInfo = modelInfo;
    g_engineClient = Create(L"engine.dll", "VEngineClient013");
}

bool Ready() { return g_entityList != nullptr && g_modelInfo != nullptr; }

void Collect(std::vector<ModelEntity>& entities)
{
    entities.clear();
    if (!Ready())
        return;
    const int highest = Slot<int(__fastcall*)(void*)>(g_entityList, kGetHighestEntityIndexSlot)(g_entityList);
    for (int index = 1; index <= highest; ++index) {
        void* entity = Slot<void*(__fastcall*)(void*, int)>(g_entityList, kGetClientEntitySlot)(g_entityList, index);
        if (entity == nullptr)
            continue;
        void* renderable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientRenderableSlot)(entity);
        if (renderable == nullptr || !Slot<bool(__fastcall*)(void*)>(renderable, kShouldDrawSlot)(renderable))
            continue;
        const void* model = Slot<const void*(__fastcall*)(void*)>(renderable, kGetModelSlot)(renderable);
        if (model == nullptr)
            continue;
        const int type = Slot<int(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelTypeSlot)(g_modelInfo, model);
        if (type != kModelTypeStudio && type != kModelTypeBrush)
            continue;
        const char* name = Slot<const char*(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelNameSlot)(g_modelInfo, model);
        const float* transform = Slot<const float*(__fastcall*)(void*)>(renderable, kRenderableToWorldTransformSlot)(renderable);
        if (name == nullptr || transform == nullptr)
            continue;
        ModelEntity& out = entities.emplace_back();
        out.index = index;
        out.renderable = renderable;
        out.model = name;
        std::memcpy(out.transform, transform, sizeof(out.transform));
    }
}

bool SetupBones(void* renderable, float (*boneToWorld)[3][4], int maxBones)
{
    if (g_engineClient == nullptr)
        return false;
    const double now = Slot<double(__fastcall*)(void*)>(g_engineClient, kEngineTimeSlot)(g_engineClient);
    return Slot<bool(__fastcall*)(void*, float (*)[3][4], int, int, double)>(renderable, kSetupBonesSlot)(
        renderable, boneToWorld, maxBones, kBoneUsedByAnything, now);
}

}
