#include "engine_interfaces.h"

#include "log.h"
#include "mod.h"
#include "module_scan.h"

#include <MinHook.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <format>
#include <string_view>

namespace gmodpp::engine {

namespace {

using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* returnCode);

// Vtable slots follow the Source SDK 2013 declarations of IVEngineClient013 and IBaseFileSystem and were
// confirmed in GMod's engine.dll (GetLevelName, ClientCmd_Unrestricted) and filesystem_stdio.dll
// (Read/Open/Close/Seek/Tell) with Ghidra.
constexpr int kGetLocalPlayerSlot = 12;
constexpr int kGetLevelNameSlot = 51;
constexpr int kClientCmdUnrestrictedSlot = 106;
// GetLightForPoint (slot 1, engine.dll+0x7ee20) calls this slot (engine.dll+0x7d210), which forwards its
// arguments to the engine's lighting function (engine.dll+0x1df6d0; Ghidra).
constexpr int kComputeLightingSlot = 67;
constexpr int kReadSlot = 0;
constexpr int kOpenSlot = 2;
constexpr int kCloseSlot = 3;
constexpr size_t kReadChunk = 256 * 1024;
// IMaterialSystem::GetRenderContext and IMatRenderContext::BeginRender/EndRender/SetIntRenderingParameter,
// as client.dll's SetupCurrentView calls them (client.dll+0x312d30 in Ghidra).
constexpr int kGetRenderContextSlot = 98;
constexpr int kBeginRenderSlot = 0;
constexpr int kEndRenderSlot = 1;
constexpr int kSetIntRenderingParameterSlot = 113;
constexpr int kEnableFixedLightingParameter = 0;
// IMatRenderContext::GetFogMode, as the render.GetFogMode Lua binding calls it (client.dll+0xbf300 in Ghidra).
constexpr int kGetFogModeSlot = 45;
// GetFogColor, by the Source SDK 2013 declaration order that puts BeginRender, GetFogMode and SetIntRenderingParameter
// at their confirmed slots.
constexpr int kGetFogColorSlot = 50;
// IMatRenderContext::Bind(IMaterial*, void* proxy), slot 7 by the Source SDK 2013 declaration order that puts
// BeginRender, GetFogMode and SetIntRenderingParameter at the confirmed 0, 45 and 113; IMaterial::GetShaderName,
// slot 49 (the IMaterial:GetShader Lua binding, client.dll+0x12c300, calls it; Ghidra) and IMaterial::GetName, slot 0.
constexpr int kBindSlot = 7;
constexpr int kGetShaderNameSlot = 49;
constexpr int kGetMaterialNameSlot = 0;
// IVEfx::CL_GetActiveDLights (engine.dll+0x143320, CVEfx vtable slot 6 found through RTTI in Ghidra). It fills
// up to 32 pointers to GMod's 72-byte dlight_t; CL_AllocDlight (engine.dll+0x9a1a0) stores the key at 40,
// four bytes later than the Source SDK 2013 layout.
constexpr int kGetActiveDLightsSlot = 6;
constexpr int kMaxDynamicLights = 32;
constexpr size_t kDlightOrigin = 4;
constexpr size_t kDlightRadius = 16;
constexpr size_t kDlightColor = 20;
constexpr size_t kDlightKey = 40;
constexpr size_t kDlightDirection = 48;
constexpr size_t kDlightInnerAngle = 60;
constexpr size_t kDlightOuterAngle = 64;
constexpr size_t kDlightSize = 72;
// d_lightstylevalue (engine.dll+0x7dfdd0): the current value of each light style, 0..25 * 22 with 264 for 'm'.
// The engine's world light intensity (engine.dll+0x1e0fa0 and +0x1e0810; Ghidra) scales a light by
// d_lightstylevalue[style] / 264 for styles below 128 and by 1 otherwise:
//   cmp eax, 0x7f; ja ...; lea rcx, [d_lightstylevalue]; movd xmm0, [rcx + rax*4]; cvtdq2ps xmm0, xmm0
constexpr std::string_view kLightStylePattern = "83 F8 7F 77 ?? 48 8D 0D ?? ?? ?? ?? 66 0F 6E 04 81 0F 5B C0";
constexpr size_t kLightStyleDisplacement = 8;
constexpr float kLightStyleNormal = 264.0f;

void* g_engineClient = nullptr;
void* g_fileSystem = nullptr;
void* g_materialSystem = nullptr;
void* g_effects = nullptr;
const int32_t* g_lightStyleValues = nullptr;
std::vector<int> g_loggedLightKeys;
bool g_attempted = false;

using BindFn = void(__fastcall*)(void* context, void* material, void* proxy);
BindFn g_bind = nullptr;
bool g_bindHookAttempted = false;
bool g_unlitFilter = false;
int g_fixedLighting = 0;
// Whether each material bound during forward lighting is unlit, by material.
std::unordered_map<void*, bool> g_unlitMaterials;
uint32_t g_loggedMaterials = 0;

template <class Fn>
Fn Slot(void* object, int slot)
{
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot]);
}

void* CreateFrom(const wchar_t* moduleName, const char* interfaceName, HMODULE& module)
{
    module = GetModuleHandleW(moduleName);
    if (module == nullptr)
        return nullptr;
    auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
    if (create == nullptr)
        return nullptr;
    int code = 0;
    return create(interfaceName, &code);
}

bool IsUnlitMaterial(void* material)
{
    auto [entry, added] = g_unlitMaterials.try_emplace(material, false);
    if (!added)
        return entry->second;
    const char* shader = Slot<const char*(__fastcall*)(void*)>(material, kGetShaderNameSlot)(material);
    std::string name = shader != nullptr ? shader : "";
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    entry->second = name == "unlitgeneric" || name == "unlittwotexture" || name == "monitorscreen";
    if (GetOptions().devMode && entry->second && g_loggedMaterials < 8) {
        ++g_loggedMaterials;
        const char* materialName = Slot<const char*(__fastcall*)(void*)>(material, kGetMaterialNameSlot)(material);
        log::Info("Forward lighting: '{}' ({}) is drawn unlit", materialName != nullptr ? materialName : "?", shader != nullptr ? shader : "?");
    }
    return entry->second;
}

void SetFixedLightingOn(void* context, int mode)
{
    if (g_fixedLighting == mode)
        return;
    g_fixedLighting = mode;
    Slot<void(__fastcall*)(void*, int, int)>(context, kSetIntRenderingParameterSlot)(context, kEnableFixedLightingParameter, mode);
}

// Unlit materials keep the game's own shaders during forward lighting: their preview combos leave out vertex colour,
// and lighting them with the probe volume hid the snow of gm_antarctic_base's night.
void __fastcall HookBind(void* context, void* material, void* proxy)
{
    g_bind(context, material, proxy);
    if (g_unlitFilter && material != nullptr)
        SetFixedLightingOn(context, IsUnlitMaterial(material) ? 0 : 2);
}

void InstallBindHook(void* context)
{
    if (g_bindHookAttempted)
        return;
    g_bindHookAttempted = true;
    void* target = (*static_cast<void***>(context))[kBindSlot];
    MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
        status = MH_CreateHook(target, reinterpret_cast<void*>(&HookBind), reinterpret_cast<void**>(&g_bind));
    if (status == MH_OK)
        status = MH_EnableHook(target);
    if (status != MH_OK) {
        g_bind = nullptr;
        log::Info("Hooking IMatRenderContext::Bind failed: {}; unlit translucent materials are lit like the others", MH_StatusToString(status));
    }
}

// The address every match of kLightStylePattern loads, or null when there is no match or they disagree.
const int32_t* FindLightStyleValues(HMODULE engine)
{
    const std::vector<const std::byte*> matches = FindCodePattern(engine, kLightStylePattern);
    const std::byte* found = nullptr;
    for (const std::byte* match : matches) {
        int32_t displacement;
        std::memcpy(&displacement, match + kLightStyleDisplacement, sizeof(displacement));
        const std::byte* target = match + kLightStyleDisplacement + sizeof(displacement) + displacement;
        if (found != nullptr && found != target) {
            log::Info("Light styles: the engine's style table could not be found (two candidates); lights ignore their styles");
            return nullptr;
        }
        found = target;
    }
    if (found == nullptr)
        log::Info("Light styles: the engine's style table could not be found; lights ignore their styles");
    return reinterpret_cast<const int32_t*>(found);
}

}

void TryInitialize()
{
    if (g_attempted)
        return;
    HMODULE engineModule = nullptr;
    HMODULE fileSystemModule = nullptr;
    HMODULE materialSystemModule = nullptr;
    void* engineClient = CreateFrom(L"engine.dll", "VEngineClient013", engineModule);
    void* fileSystem = CreateFrom(L"filesystem_stdio.dll", "VBaseFileSystem011", fileSystemModule);
    void* materialSystem = CreateFrom(L"materialsystem.dll", "VMaterialSystem080", materialSystemModule);
    void* effects = CreateFrom(L"engine.dll", "VEngineEffects001", engineModule);
    if (engineModule == nullptr || fileSystemModule == nullptr || materialSystemModule == nullptr)
        return;
    g_attempted = true;
    g_lightStyleValues = FindLightStyleValues(engineModule);
    g_materialSystem = materialSystem;
    g_effects = effects;
    if (materialSystem == nullptr)
        log::Info("VMaterialSystem080 unavailable; deferred lighting disabled");
    if (engineClient == nullptr || fileSystem == nullptr) {
        log::Info("Engine interfaces unavailable (VEngineClient013 {}, VBaseFileSystem011 {}); map geometry disabled",
            engineClient != nullptr, fileSystem != nullptr);
        return;
    }
    g_engineClient = engineClient;
    g_fileSystem = fileSystem;
}

bool Ready() { return g_engineClient != nullptr && g_fileSystem != nullptr; }

ComputeLightingFn ComputeLightingFunction()
{
    return g_engineClient != nullptr ? Slot<ComputeLightingFn>(g_engineClient, kComputeLightingSlot) : nullptr;
}

std::string LevelName()
{
    if (!Ready())
        return {};
    const char* name = Slot<const char*(__fastcall*)(void*)>(g_engineClient, kGetLevelNameSlot)(g_engineClient);
    return name != nullptr ? std::string(name) : std::string();
}

int LocalPlayerIndex()
{
    if (!Ready())
        return 0;
    return Slot<int(__fastcall*)(void*)>(g_engineClient, kGetLocalPlayerSlot)(g_engineClient);
}

void ExecuteCommand(const std::string& command)
{
    if (Ready())
        Slot<void(__fastcall*)(void*, const char*)>(g_engineClient, kClientCmdUnrestrictedSlot)(g_engineClient, command.c_str());
}

bool ReadGameFile(const std::string& path, std::vector<std::byte>& data, size_t maxBytes)
{
    if (!Ready())
        return false;
    using OpenFn = void*(__fastcall*)(void*, const char*, const char*, const char*);
    using CloseFn = void(__fastcall*)(void*, void*);
    using ReadFn = int(__fastcall*)(void*, void*, int, void*);

    void* file = Slot<OpenFn>(g_fileSystem, kOpenSlot)(g_fileSystem, path.c_str(), "rb", "GAME");
    if (file == nullptr)
        return false;
    // Read to the end rather than trusting a seek to the end: Tell reports one byte short for some files in VPKs.
    data.clear();
    while (data.size() < maxBytes) {
        const size_t offset = data.size();
        const size_t wanted = std::min(kReadChunk, maxBytes - offset);
        data.resize(offset + wanted);
        const int read = Slot<ReadFn>(g_fileSystem, kReadSlot)(g_fileSystem, data.data() + offset, static_cast<int>(wanted), file);
        data.resize(offset + static_cast<size_t>(std::max(read, 0)));
        if (read < static_cast<int>(wanted))
            break;
    }
    Slot<CloseFn>(g_fileSystem, kCloseSlot)(g_fileSystem, file);
    return !data.empty();
}

void ActiveDynamicLights(std::vector<DynamicLight>& lights)
{
    lights.clear();
    if (g_effects == nullptr)
        return;
    const std::byte* active[kMaxDynamicLights] = {};
    const int count = Slot<int(__fastcall*)(void*, const std::byte**)>(g_effects, kGetActiveDLightsSlot)(g_effects, active);
    for (int i = 0; i < count && i < kMaxDynamicLights; ++i) {
        const std::byte* record = active[i];
        DynamicLight& light = lights.emplace_back();
        std::memcpy(light.origin, record + kDlightOrigin, sizeof(light.origin));
        std::memcpy(&light.radius, record + kDlightRadius, sizeof(light.radius));
        // ColorRGBExp32: linear value = channel * 2^exponent / 255.
        const auto* color = reinterpret_cast<const uint8_t*>(record + kDlightColor);
        const float scale = std::ldexp(1.0f, static_cast<int8_t>(color[3])) / 255.0f;
        for (int c = 0; c < 3; ++c)
            light.color[c] = color[c] * scale;
        std::memcpy(light.direction, record + kDlightDirection, sizeof(light.direction));
        std::memcpy(&light.innerAngle, record + kDlightInnerAngle, sizeof(light.innerAngle));
        std::memcpy(&light.outerAngle, record + kDlightOuterAngle, sizeof(light.outerAngle));
        std::memcpy(&light.key, record + kDlightKey, sizeof(light.key));
        if (GetOptions().devMode && std::find(g_loggedLightKeys.begin(), g_loggedLightKeys.end(), light.key) == g_loggedLightKeys.end()) {
            g_loggedLightKeys.push_back(light.key);
            std::string bytes;
            for (size_t b = 0; b < kDlightSize; ++b)
                bytes += std::format("{:02x}{}", static_cast<unsigned>(record[b]), b % 4 == 3 ? " " : "");
            log::Info("Dynamic light key {} at ({:.1f} {:.1f} {:.1f}) radius {:.1f} color ({:.2f} {:.2f} {:.2f}) outer {:.1f}: {}", light.key,
                light.origin[0], light.origin[1], light.origin[2], light.radius, light.color[0], light.color[1], light.color[2], light.outerAngle, bytes);
        }
    }
}

bool SetFixedLighting(int mode)
{
    if (g_materialSystem == nullptr)
        return false;
    void* context = Slot<void*(__fastcall*)(void*)>(g_materialSystem, kGetRenderContextSlot)(g_materialSystem);
    if (context == nullptr)
        return false;
    InstallBindHook(context);
    Slot<void(__fastcall*)(void*)>(context, kBeginRenderSlot)(context);
    g_fixedLighting = -1;
    SetFixedLightingOn(context, mode);
    Slot<void(__fastcall*)(void*)>(context, kEndRenderSlot)(context);
    return true;
}

void SetUnlitFilter(bool enabled) { g_unlitFilter = enabled && g_bind != nullptr; }

std::optional<FogState> CurrentFog()
{
    if (g_materialSystem == nullptr)
        return std::nullopt;
    void* context = Slot<void*(__fastcall*)(void*)>(g_materialSystem, kGetRenderContextSlot)(g_materialSystem);
    if (context == nullptr)
        return std::nullopt;
    FogState fog;
    Slot<void(__fastcall*)(void*)>(context, kBeginRenderSlot)(context);
    fog.mode = Slot<int(__fastcall*)(void*)>(context, kGetFogModeSlot)(context);
    Slot<void(__fastcall*)(void*, unsigned char*)>(context, kGetFogColorSlot)(context, fog.color.data());
    Slot<void(__fastcall*)(void*)>(context, kEndRenderSlot)(context);
    return fog;
}

bool LightStyleValues(std::array<float, kLightStyles>& values)
{
    if (g_lightStyleValues == nullptr)
        return false;
    for (size_t i = 0; i < kLightStyles; ++i)
        values[i] = static_cast<float>(g_lightStyleValues[i]) / kLightStyleNormal;
    return true;
}

}
