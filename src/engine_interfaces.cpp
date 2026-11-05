#include "engine_interfaces.h"

#include "log.h"
#include "module_scan.h"
#include "options.h"


#include <windows.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
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
// IsInGame (engine.dll+0x7f550 in Ghidra) compares the client's signon state with SIGNONSTATE_FULL (6).
constexpr int kIsInGameSlot = 26;
constexpr int kGetLevelNameSlot = 51;
constexpr int kClientCmdUnrestrictedSlot = 106;
// GetLightForPoint (slot 1, engine.dll+0x7ee20) calls this slot (engine.dll+0x7d210), which forwards its
// arguments to the engine's lighting function (engine.dll+0x1df6d0; Ghidra).
constexpr int kComputeLightingSlot = 67;
constexpr int kReadSlot = 0;
constexpr int kOpenSlot = 2;
constexpr int kCloseSlot = 3;
constexpr size_t kReadChunk = 256 * 1024;
// IMaterialSystem::GetRenderContext and IMatRenderContext::BeginRender/EndRender, as client.dll's SetupCurrentView
// calls them (client.dll+0x312d30 in Ghidra); SetIntRenderingParameter is slot 113 there.
constexpr int kGetRenderContextSlot = 98;
constexpr int kBeginRenderSlot = 0;
constexpr int kEndRenderSlot = 1;
// IMatRenderContext::GetFogMode, as the render.GetFogMode Lua binding calls it (client.dll+0xbf300 in Ghidra).
constexpr int kGetFogModeSlot = 45;
// GetFogColor, by the Source SDK 2013 declaration order that puts BeginRender, GetFogMode and SetIntRenderingParameter
// at their confirmed slots.
constexpr int kGetFogColorSlot = 50;
// IMaterial::GetShaderName, slot 49 (the IMaterial:GetShader Lua binding, client.dll+0x12c300, calls it; Ghidra) and
// IMaterial::GetName, slot 0.
constexpr int kGetShaderNameSlot = 49;
constexpr int kGetMaterialNameSlot = 0;
// IMaterial::IsTranslucent, slot 17 as in the SDK: CMaterial's vtable (materialsystem.dll+0xadf08, by RTTI) holds there a
// function (+0xff00) that precaches and returns the translucency test with the alpha modulation; slot 49 there returns
// the shader's name or "shader_error" (REA 2026-11-04).
constexpr int kIsTranslucentSlot = 17;
// A material's variable as it is now (MaterialVector), as Lua's IMaterial:GetVector (client.dll+0x12c740) reads it:
// IMaterialSystem::FindMaterial (slot 71, materialsystem.dll+0x1b760: name, group, complain, prefix; the error material
// when the material does not exist), IMaterial::FindVar (slot 11, +0xe370: name, found, complain; a shared dummy with
// found false when the variable does not exist) and IMaterialVar::GetVecValue (slot 28, +0x26fa0: values, count), which
// reads the render thread's copy under queued rendering. REA 2026-11-05.
constexpr int kFindMaterialSlot = 71;
constexpr int kMaterialFindVarSlot = 11;
constexpr int kGetVecValueSlot = 28;
// IVEfx::CL_GetActiveDLights (engine.dll+0x143320, CVEfx vtable slot 6 found through RTTI in Ghidra). It fills
// up to 32 pointers to GMod's 72-byte dlight_t; CL_AllocDlight (engine.dll+0x9a1a0) stores the key at 40,
// four bytes later than the Source SDK 2013 layout.
constexpr int kGetActiveDLightsSlot = 6;
constexpr int kMaxDynamicLights = 32;
// dlight_t::flags, where the engine's world lighting (engine.dll+0xd7de0) tests DLIGHT_NO_WORLD_ILLUMINATION and its model
// lighting (+0x1ded20) DLIGHT_NO_MODEL_ILLUMINATION, and Lua's DynamicLight sets them for "noworld" and "nomodel"
// (client.dll+0xf6050). REA 2026-11-04.
constexpr size_t kDlightFlags = 0;
constexpr size_t kDlightOrigin = 4;
constexpr size_t kDlightRadius = 16;
constexpr size_t kDlightColor = 20;
constexpr size_t kDlightKey = 40;
constexpr size_t kDlightDirection = 48;
constexpr size_t kDlightInnerAngle = 60;
constexpr size_t kDlightOuterAngle = 64;
constexpr size_t kDlightSize = 72;
// Entity lights (elights), which Source applies to models alone (HL2's muzzle flashes): CVEfx::CL_AllocElight (vtable
// slot 5, engine.dll+0x9a230) passes the array cl_elights (+0x6501a0) and its 64 records of dlight_t to the engine's light
// finder (+0x9a290): mov [rsp+8], rbx; push rdi; sub rsp, 20h; mov ebx, ecx; lea rdi, [cl_elights]; mov r8d, ecx; mov
// edx, 40h. CL_DecayLights (+0x9aae0) zeroes a record's radius once its die time has passed, so the live ones are those
// with a radius, as the engine's model lighting (+0x1ded20) takes them. REA 2026-11-04.
constexpr int kAllocElightSlot = 5;
constexpr int kElightPattern[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xD9, 0x48, 0x8D, 0x3D, -1, -1, -1, -1, 0x44,
    0x8B, 0xC1, 0xBA};
constexpr size_t kElightArrayDisplacement = 15;
constexpr size_t kElightCount = 23;
// The ConVar client Lua reads to know the mod lights the scene (PublishLightsActive). It is built with client.dll's own
// ConVar constructor (this, name, default, flags; client.dll+0x527120), which links it into client.dll's list and, once
// the client has registered its own, registers it at once (tier1's CreateBase and Init). A ConVar is 0x80 bytes (the
// deleting destructor, +0x526360, frees that much). The constructor is the call in muzzleflash_light's static
// initializer (+0x54460): lea rdx, [name]; lea rcx, [object]; call. REA 2026-11-04.
constexpr const char* kLightsConVar = "gmodpp_lights";
constexpr size_t kConVarBytes = 0x80;
// FCVAR_DONTRECORD: left out of demos. Not FCVAR_ARCHIVE, so config.cfg never keeps it.
constexpr int kLightsConVarFlags = 0x20000;
// d_lightstylevalue (engine.dll+0x7dfdd0): the current value of each light style, 0..25 * 22 with 264 for 'm'.
// The engine's world light intensity (engine.dll+0x1e0fa0 and +0x1e0810; Ghidra) scales a light by
// d_lightstylevalue[style] / 264 for styles below 128 and by 1 otherwise:
//   cmp eax, 0x7f; ja ...; lea rcx, [d_lightstylevalue]; movd xmm0, [rcx + rax*4]; cvtdq2ps xmm0, xmm0
constexpr std::string_view kLightStylePattern = "83 F8 7F 77 ?? 48 8D 0D ?? ?? ?? ?? 66 0F 6E 04 81 0F 5B C0";
constexpr size_t kLightStyleDisplacement = 8;
constexpr float kLightStyleNormal = 264.0f;
// ICvar ("VEngineCvar004", vstdlib.dll). FindVar is slot 12 (vstdlib.dll+0x5130: FindCommandBase, then null for
// commands). GMod's inlined ConVar::GetString, in vstdlib.dll's RevertFlaggedConVars (+0x6fa0) and in the
// gmod_language change callback (filesystem_stdio.dll+0xd660), reads the flags at 40 (FCVAR_NEVER_AS_STRING 0x1000)
// and the parent ConVar at 56, then the parent's string at 120 when it is not empty, else its m_pszString at 72.
// filesystem_stdio.dll registers gmod_language (+0x1050) with an empty default. Confirmed with Ghidra (REA).
constexpr int kFindVarSlot = 12;
constexpr size_t kConVarFlags = 40;
constexpr uint32_t kNeverAsString = 0x1000;
constexpr size_t kConVarParent = 56;
constexpr size_t kConVarString = 72;
constexpr size_t kConVarOverride = 120;
// ConVar::Create (engine.dll+0x2a8070) keeps the default value's string at 64. REA 2026-11-03.
constexpr size_t kConVarDefault = 64;
// The frame rate cap while the game window is not the active app (GMod has no engine_no_focus_sleep): engine.dll's
// frame limiter (+0x229950, called by CEngine::Frame at +0x229c40) caps the rate at fps_max_nofocus, at least 10,
// while the IGame object's slot 16 (+0x22e7b0, its flag at 0x48) reports the window inactive; 0 turns the cap off.
// engine.dll registers it (+0x19be0) with "20" and FCVAR_ARCHIVE, so config.cfg keeps it. REA 2026-11-03.
constexpr const char* kUnfocusedFrameCap = "fps_max_nofocus";

void* g_engineClient = nullptr;
void* g_fileSystem = nullptr;
void* g_materialSystem = nullptr;
void* g_effects = nullptr;
const int32_t* g_lightStyleValues = nullptr;
std::vector<int> g_loggedLightKeys;
bool g_attempted = false;

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

// The ConVar that holds the value of the console variable `name`, or null when it is not registered or not a string.
const std::byte* FindConVar(const char* name)
{
    static void* cvar = nullptr;
    if (cvar == nullptr) {
        HMODULE module = nullptr;
        cvar = CreateFrom(L"vstdlib.dll", "VEngineCvar004", module);
        if (cvar == nullptr)
            return nullptr;
    }
    const std::byte* var = Slot<const std::byte*(__fastcall*)(void*, const char*)>(cvar, kFindVarSlot)(cvar, name);
    if (var == nullptr)
        return nullptr;
    uint32_t flags;
    std::memcpy(&flags, var + kConVarFlags, sizeof(flags));
    if ((flags & kNeverAsString) != 0)
        return nullptr;
    const std::byte* parent;
    std::memcpy(&parent, var + kConVarParent, sizeof(parent));
    return parent;
}

std::string StringAt(const std::byte* var, size_t offset)
{
    const char* text;
    std::memcpy(&text, var + offset, sizeof(text));
    return text != nullptr ? std::string(text) : std::string();
}

// As GMod's ConVar::GetString reads it.
std::string ConVarValue(const std::byte* var)
{
    std::string value = StringAt(var, kConVarOverride);
    return value.empty() ? StringAt(var, kConVarString) : value;
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

bool IsUnlitMaterial(void* material)
{
    auto [entry, added] = g_unlitMaterials.try_emplace(material, false);
    if (!added)
        return entry->second;
    const char* shader = Slot<const char*(__fastcall*)(void*)>(material, kGetShaderNameSlot)(material);
    std::string name = shader != nullptr ? shader : "";
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Shaders with DirectX 9 versions are named after them (UnlitTwoTexture_DX9, MonitorScreen_DX9).
    const std::string base = name.substr(0, name.find("_dx"));
    entry->second = base == "unlitgeneric" || base == "unlittwotexture" || base == "monitorscreen";
    if (GetOptions().devMode && entry->second && g_loggedMaterials < 8) {
        ++g_loggedMaterials;
        const char* materialName = Slot<const char*(__fastcall*)(void*)>(material, kGetMaterialNameSlot)(material);
        log::Info("Forward lighting: '{}' ({}) is drawn unlit", materialName != nullptr ? materialName : "?", shader != nullptr ? shader : "?");
    }
    return entry->second;
}

static std::unordered_map<std::string, void*> g_materialVars;

std::optional<std::array<float, 3>> MaterialVector(const std::string& material, const char* variable)
{
    if (g_materialSystem == nullptr)
        return std::nullopt;
    const std::string key = material + '|' + variable;
    auto cached = g_materialVars.find(key);
    if (cached == g_materialVars.end()) {
        void* found = Slot<void*(__fastcall*)(void*, const char*, const char*, bool, const char*)>(g_materialSystem, kFindMaterialSlot)(
            g_materialSystem, material.c_str(), "Model textures", false, nullptr);
        bool exists = false;
        void* var = found != nullptr ? Slot<void*(__fastcall*)(void*, const char*, bool*, bool)>(found, kMaterialFindVarSlot)(found, variable, &exists, false) : nullptr;
        cached = g_materialVars.emplace(key, exists ? var : nullptr).first;
    }
    if (cached->second == nullptr)
        return std::nullopt;
    std::array<float, 3> value = {};
    Slot<void(__fastcall*)(void*, float*, int)>(cached->second, kGetVecValueSlot)(cached->second, value.data(), 3);
    return value;
}

void ForgetMaterials() { g_materialVars.clear(); }

std::string MaterialName(void* material)
{
    const char* name = Slot<const char*(__fastcall*)(void*)>(material, kGetMaterialNameSlot)(material);
    return name != nullptr ? name : "";
}

bool IsTranslucentMaterial(void* material)
{
    return Slot<bool(__fastcall*)(void*)>(material, kIsTranslucentSlot)(material);
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

bool IsInGame()
{
    return Ready() && Slot<bool(__fastcall*)(void*)>(g_engineClient, kIsInGameSlot)(g_engineClient);
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

std::string GameLanguage()
{
    const std::byte* var = FindConVar("gmod_language");
    return var != nullptr ? ConVarValue(var) : std::string();
}

std::optional<std::string> ConVarString(const char* name)
{
    const std::byte* var = FindConVar(name);
    if (var == nullptr)
        return std::nullopt;
    return ConVarValue(var);
}

void CapUnfocusedFrames(bool cap)
{
    // The cap's value from before it was turned off, to put back.
    static std::optional<std::string> restore;
    static std::optional<bool> applied;
    if (applied == cap || !Ready())
        return;
    const std::byte* var = FindConVar(kUnfocusedFrameCap);
    if (var == nullptr)
        return;
    const std::string value = ConVarValue(var);
    // The engine caps only above 0.
    const bool capped = std::strtof(value.c_str(), nullptr) > 0.0f;
    if (!cap) {
        if (capped) {
            restore = value;
            ExecuteCommand(std::format("{} 0", kUnfocusedFrameCap));
        } else if (!applied) {
            // Off at launch: config.cfg kept it from a session with the cap turned off.
            restore = StringAt(var, kConVarDefault);
        }
    } else if (restore) {
        // A value set since then by hand stays.
        if (!capped)
            ExecuteCommand(std::format("{} \"{}\"", kUnfocusedFrameCap, *restore));
        restore.reset();
    }
    applied = cap;
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

namespace {

// A dlight_t record (dlights and elights alike).
DynamicLight ReadLight(const std::byte* record)
{
    DynamicLight light;
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
    std::memcpy(&light.flags, record + kDlightFlags, sizeof(light.flags));
    light.identity = reinterpret_cast<uintptr_t>(record);
    return light;
}

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
        const DynamicLight& light = lights.emplace_back(ReadLight(record));
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

void ActiveEntityLights(std::vector<DynamicLight>& lights)
{
    lights.clear();
    static const std::byte* records = nullptr;
    static int32_t count = 0;
    static bool attempted = false;
    if (!attempted && g_effects != nullptr) {
        attempted = true;
        const auto* code = reinterpret_cast<const uint8_t*>(Slot<const void*>(g_effects, kAllocElightSlot));
        bool matches = true;
        for (size_t i = 0; i < std::size(kElightPattern); ++i)
            matches = matches && (kElightPattern[i] < 0 || code[i] == kElightPattern[i]);
        if (matches) {
            int32_t displacement;
            std::memcpy(&displacement, code + kElightArrayDisplacement, sizeof(displacement));
            records = reinterpret_cast<const std::byte*>(code + kElightArrayDisplacement + sizeof(displacement) + displacement);
            std::memcpy(&count, code + kElightCount, sizeof(count));
        } else {
            log::Info("Entity lights: CL_AllocElight is not as expected; elights (HL2's muzzle flashes) light nothing");
        }
    }
    for (int32_t i = 0; records != nullptr && i < count; ++i) {
        const std::byte* record = records + static_cast<size_t>(i) * kDlightSize;
        float radius;
        std::memcpy(&radius, record + kDlightRadius, sizeof(radius));
        if (radius > 0.0f)
            lights.push_back(ReadLight(record));
    }
}

void PublishLightsActive(bool active)
{
    static bool registered = false;
    static bool failed = false;
    if (!registered) {
        // Built once the client has registered its own ConVars, so it is registered at once too.
        HMODULE client = GetModuleHandleW(L"client.dll");
        if (failed || client == nullptr || FindConVar("muzzleflash_light") == nullptr)
            return;
        using ConstructorFn = void*(__fastcall*)(void* self, const char* name, const char* defaultValue, int flags);
        ConstructorFn constructor = nullptr;
        for (const std::byte* reference : FindStringReferences(client, "muzzleflash_light")) {
            const auto* code = reinterpret_cast<const uint8_t*>(reference);
            // lea rdx, [name]; lea rcx, [object]; call constructor
            if (code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x15 || code[7] != 0x48 || code[8] != 0x8D || code[9] != 0x0D || code[14] != 0xE8)
                continue;
            int32_t displacement;
            std::memcpy(&displacement, code + 15, sizeof(displacement));
            auto found = reinterpret_cast<ConstructorFn>(const_cast<uint8_t*>(code + 19 + displacement));
            if (constructor != nullptr && constructor != found) {
                constructor = nullptr;
                break;
            }
            constructor = found;
        }
        if (constructor == nullptr) {
            failed = true;
            log::Info("Addon signal: client.dll's ConVar constructor was not found; {} is not registered", kLightsConVar);
            return;
        }
        alignas(16) static std::byte storage[kConVarBytes] = {};
        constructor(storage, kLightsConVar, "0", kLightsConVarFlags);
        registered = true;
        log::Info("Addon signal: {} registered", kLightsConVar);
    }
    const char* wanted = active ? "1" : "0";
    if (ConVarString(kLightsConVar) != wanted)
        ExecuteCommand(std::format("{} {}", kLightsConVar, wanted));
}

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
