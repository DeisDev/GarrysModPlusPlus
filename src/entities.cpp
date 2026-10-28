#include "entities.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
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
// IClientRenderable::GetShadowParent: decodes the move parent handle and returns that entity's renderable
// (client.dll+0x1b4860, C_BaseCombatWeapon's renderable vtable found through RTTI in Ghidra).
constexpr int kGetShadowParentSlot = 28;
constexpr int kRenderableToWorldTransformSlot = 34;
constexpr int kGetModelNameSlot = 3;
constexpr int kGetModelTypeSlot = 9;
// modtype_t: mod_bad, mod_brush, mod_sprite, mod_studio.
constexpr int kModelTypeBrush = 1;
constexpr int kModelTypeStudio = 3;
constexpr int kSetupBonesSlot = 16;
constexpr int kEngineTimeSlot = 14;
constexpr int kBoneUsedByAnything = 0x0007FF00;
constexpr int kGetClientNetworkableSlot = 4;
constexpr int kGetClientClassSlot = 2;
constexpr size_t kClientClassName = 16;
constexpr int kModelInfoGetModelSlot = 1;
// C_BaseCombatWeapon's m_iViewModelIndex and m_iWorldModelIndex (their DT_BaseCombatWeapon RecvProps are built at
// client.dll+0x27990, confirmed in Ghidra).
constexpr size_t kWeaponViewModelIndex = 0x1c10;
constexpr size_t kWeaponWorldModelIndex = 0x1c14;

// C_RopeKeyframe in GMod's client.dll (DT_RopeKeyframe, DrawModel and OnDataChanged, confirmed in Ghidra). Between the
// networked m_iRopeMaterialModelIndex and m_nSegments lie CRopePhysics<10> m_RopePhysics (SDK public/rope_physics.h:
// node pointer, node count, then the nodes, each m_vPos, m_vPrevPos and the drawn m_vPredicted) and m_LightValues.
constexpr char kRopeClass[] = "CRopeKeyframe";
constexpr size_t kRopeMaterialIndex = 0x16fc;
constexpr size_t kRopeNodePointer = 0x1710;
constexpr size_t kRopeNodeCount = 0x1718;
constexpr size_t kRopeNodes = 0x1750;
constexpr size_t kRopeNodeSize = 36;
constexpr size_t kRopeNodePredicted = 24;
constexpr size_t kRopeSubdiv = 0x19f8;
constexpr size_t kRopeWidth = 0x1a0c;
constexpr int kRopeMaxNodes = 10;
// m_Subdiv 255 takes the rope_subdiv convar (default 2); the game clamps it below MAX_ROPE_SUBDIVS (8).
constexpr int kRopeSubdivFromConvar = 255;
constexpr int kRopeDefaultSubdiv = 2;
constexpr int kRopeMaxSubdiv = 7;
constexpr float kWorldLimit = 65536.0f;

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

template <class T>
T Field(const void* object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof(T));
    return value;
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
        out.entity = entity;
        out.renderable = renderable;
        out.parent = Slot<void*(__fastcall*)(void*)>(renderable, kGetShadowParentSlot)(renderable);
        out.model = name;
        std::memcpy(out.transform, transform, sizeof(out.transform));
    }
}

void CollectRopes(std::vector<Rope>& ropes)
{
    ropes.clear();
    if (!Ready())
        return;
    const int highest = Slot<int(__fastcall*)(void*)>(g_entityList, kGetHighestEntityIndexSlot)(g_entityList);
    for (int index = 1; index <= highest; ++index) {
        void* entity = Slot<void*(__fastcall*)(void*, int)>(g_entityList, kGetClientEntitySlot)(g_entityList, index);
        if (entity == nullptr)
            continue;
        void* networkable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientNetworkableSlot)(entity);
        const void* clientClass = networkable != nullptr ? Slot<const void*(__fastcall*)(void*)>(networkable, kGetClientClassSlot)(networkable) : nullptr;
        if (clientClass == nullptr || std::strcmp(Field<const char*>(clientClass, kClientClassName), kRopeClass) != 0)
            continue;
        void* renderable = Slot<void*(__fastcall*)(void*)>(entity, kGetClientRenderableSlot)(entity);
        if (renderable == nullptr || !Slot<bool(__fastcall*)(void*)>(renderable, kShouldDrawSlot)(renderable))
            continue;
        // The node pointer points at the nodes inside the entity only where the layout above holds.
        const int nodes = Field<int>(entity, kRopeNodeCount);
        if (Field<const std::byte*>(entity, kRopeNodePointer) != static_cast<const std::byte*>(entity) + kRopeNodes || nodes < 2 || nodes > kRopeMaxNodes)
            continue;
        Rope rope;
        rope.index = index;
        rope.width = Field<float>(entity, kRopeWidth);
        const int subdiv = Field<uint8_t>(entity, kRopeSubdiv);
        rope.subdivisions = std::min(subdiv == kRopeSubdivFromConvar ? kRopeDefaultSubdiv : subdiv, kRopeMaxSubdiv);
        bool valid = std::isfinite(rope.width) && rope.width > 0.0f;
        for (int n = 0; n < nodes && valid; ++n) {
            const std::array<float, 3> node = Field<std::array<float, 3>>(entity, kRopeNodes + static_cast<size_t>(n) * kRopeNodeSize + kRopeNodePredicted);
            for (float v : node)
                valid = valid && std::isfinite(v) && std::fabs(v) < kWorldLimit;
            rope.nodes.push_back(node);
        }
        const void* model = Slot<const void*(__fastcall*)(void*, int)>(g_modelInfo, kModelInfoGetModelSlot)(g_modelInfo, Field<int>(entity, kRopeMaterialIndex));
        const char* material = model != nullptr ? Slot<const char*(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelNameSlot)(g_modelInfo, model) : nullptr;
        if (!valid || material == nullptr)
            continue;
        rope.material = material;
        ropes.push_back(std::move(rope));
    }
}

std::string WeaponWorldModel(const ModelEntity& weapon)
{
    if (!Ready())
        return {};
    const auto modelAt = [](int index) { return Slot<const void*(__fastcall*)(void*, int)>(g_modelInfo, kModelInfoGetModelSlot)(g_modelInfo, index); };
    const void* shown = Slot<const void*(__fastcall*)(void*)>(weapon.renderable, kGetModelSlot)(weapon.renderable);
    if (shown == nullptr || modelAt(Field<int>(weapon.entity, kWeaponViewModelIndex)) != shown)
        return {};
    const void* world = modelAt(Field<int>(weapon.entity, kWeaponWorldModelIndex));
    const char* name = world != nullptr ? Slot<const char*(__fastcall*)(void*, const void*)>(g_modelInfo, kGetModelNameSlot)(g_modelInfo, world) : nullptr;
    return name != nullptr ? name : std::string{};
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
