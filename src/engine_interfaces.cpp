#include "engine_interfaces.h"

#include "log.h"
#include "mod.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace gmodpp::engine {

namespace {

using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* returnCode);

// Vtable slots follow the Source SDK 2013 declarations of IVEngineClient013 and IBaseFileSystem and were
// confirmed in GMod's engine.dll (GetLevelName, ClientCmd_Unrestricted) and filesystem_stdio.dll
// (Read/Open/Close/Seek/Tell) with Ghidra.
constexpr int kGetLocalPlayerSlot = 12;
constexpr int kGetLevelNameSlot = 51;
constexpr int kClientCmdUnrestrictedSlot = 106;
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

void* g_engineClient = nullptr;
void* g_fileSystem = nullptr;
void* g_materialSystem = nullptr;
void* g_effects = nullptr;
std::vector<int> g_loggedLightKeys;
bool g_attempted = false;

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
    Slot<void(__fastcall*)(void*)>(context, kBeginRenderSlot)(context);
    Slot<void(__fastcall*)(void*, int, int)>(context, kSetIntRenderingParameterSlot)(context, kEnableFixedLightingParameter, mode);
    Slot<void(__fastcall*)(void*)>(context, kEndRenderSlot)(context);
    return true;
}

}
