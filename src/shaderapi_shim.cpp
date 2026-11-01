#include "capture.h"
#include "command_line.h"
#include "log.h"
#include "module_scan.h"
#include "options.h"
#include "shader_precache.h"
#include "shaderapi_native.h"
#include "shaderapi_commands.h"
#include "shaderapi_slots.h"
#include "watchdog.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" std::uint8_t gmodpp_trace_active;
extern "C" const void* const gmodpp_thunks[];
extern "C" const DWORD gmodpp_thunk_count;
extern "C" std::uintptr_t gmodpp_internal_begin;
extern "C" std::uintptr_t gmodpp_internal_end;
extern "C" void gmodpp_object_thunk();

using namespace gmodpp;
using namespace gmodpp::shim;

namespace {

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

constexpr int kInterfaceFailed = 1;
constexpr size_t kMaxSlots = 320;
constexpr int kImageFormatBgra8888 = 12;
constexpr int kDevFilePollFrames = 10;
constexpr int kTraceLineBudget = 200000;
constexpr int kTracedCommandFloats = 16;
constexpr int kMaxCommandBufferDepth = 8;
constexpr int kDefaultBenchmarkSeconds = 60;
// NUM_IMAGE_FORMATS on PC (public/bitmap/imageformat.h).
constexpr int kImageFormatCount = 41;
constexpr int kMaxMsaaSamples = 16;

struct InterfaceInfo {
    const char* version;
    const char* className;
    const SlotInfo* slots;
    size_t slotCount;
};

enum InterfaceIndex { kDeviceMgr, kDevice, kShaderApi, kShadow, kHardwareConfig, kDebugTextureInfo, kVBAllocTracker, kShaderUtil, kInterfaceCount };

const InterfaceInfo kInterfaces[kInterfaceCount] = {
    { "ShaderDeviceMgr001", "IShaderDeviceMgr", kShaderDeviceMgrSlots, kShaderDeviceMgrSlotsCount },
    { "ShaderDevice001", "IShaderDevice", kShaderDeviceSlots, kShaderDeviceSlotsCount },
    { "ShaderApi030", "IShaderAPI", kShaderApiSlots, kShaderApiSlotsCount },
    { "ShaderShadow010", "IShaderShadow", kShaderShadowSlots, kShaderShadowSlotsCount },
    { "MaterialSystemHardwareConfig012", "IMaterialSystemHardwareConfig", kHardwareConfigSlots, kHardwareConfigSlotsCount },
    { "DebugTextureInfo001", "IDebugTextureInfo", kDebugTextureInfoSlots, kDebugTextureInfoSlotsCount },
    { "VBAllocTracker001", "IVBAllocTracker", kVBAllocTrackerSlots, kVBAllocTrackerSlotsCount },
    { "VShaderUtil001", "IShaderUtil", kShaderUtilSlots, kShaderUtilSlotsCount },
};

struct ObjectClass {
    const char* rttiName;
    DWORD subobjectOffset;
    InterfaceInfo info;
};

// The shader API's mesh and buffer classes, whose vtables are patched (see shaderapi_slots.cpp).
const ObjectClass kObjectClasses[] = {
    { ".?AVCVertexBufferDx8@@", 0, { "", "CVertexBufferDx8", kVertexBufferSlots, kVertexBufferSlotsCount } },
    { ".?AVCIndexBufferDx8@@", 0, { "", "CIndexBufferDx8", kIndexBufferSlots, kIndexBufferSlotsCount } },
    { ".?AVCMeshDX8@@", 0, { "", "CMeshDX8", kMeshSlots, kMeshSlotsCount } },
    { ".?AVCMeshDX8@@", 8, { "", "CMeshDX8(IIndexBuffer)", kIndexBufferSlots, kIndexBufferSlotsCount } },
    { ".?AVCDynamicMeshDX8@@", 0, { "", "CDynamicMeshDX8", kMeshSlots, kMeshSlotsCount } },
    { ".?AVCDynamicMeshDX8@@", 8, { "", "CDynamicMeshDX8(IIndexBuffer)", kIndexBufferSlots, kIndexBufferSlotsCount } },
    { ".?AVCTempMeshDX8@@", 0, { "", "CTempMeshDX8", kMeshSlots, kMeshSlotsCount } },
    { ".?AVCTempMeshDX8@@", 8, { "", "CTempMeshDX8(IIndexBuffer)", kIndexBufferSlots, kIndexBufferSlotsCount } },
    { ".?AVCBufferedMeshDX8@@", 0, { "", "CBufferedMeshDX8", kMeshSlots, kMeshSlotsCount } },
    { ".?AVCBufferedMeshDX8@@", 8, { "", "CBufferedMeshDX8(IIndexBuffer)", kIndexBufferSlots, kIndexBufferSlotsCount } },
};
constexpr size_t kObjectClassCount = std::size(kObjectClasses);

// Slots called by the shim itself, in GMod's layout (see shaderapi_slots.cpp).
constexpr unsigned kDeviceMgrConnect = 0;
constexpr unsigned kDeviceMgrQueryInterface = 2;
constexpr unsigned kDeviceMgrSetMode = 12;
constexpr unsigned kDeviceGetBackBufferDimensions = 3;
constexpr unsigned kDevicePresent = 9;
constexpr unsigned kShaderApiExecuteCommandBuffer = 94;
constexpr unsigned kShaderApiReadPixels = 159;
constexpr unsigned kShaderUtilRestoreShaderObjects = 12;
constexpr unsigned kShadowSetVertexShader = 23;
constexpr unsigned kShadowSetPixelShader = 24;
// IVEngineClient013 slots, confirmed in GMod's engine.dll (see engine_interfaces.cpp). IsInGame
// (engine.dll+0x7f550, Ghidra) compares the client's signon state with SIGNONSTATE_FULL (6).
constexpr unsigned kEngineIsInGame = 26;
constexpr unsigned kEngineGetLevelName = 51;
constexpr unsigned kEngineClientCmdUnrestricted = 106;

struct InterfaceTrace {
    const InterfaceInfo* info;
    std::uint32_t calls[kMaxSlots];
    size_t vtableSize;
};
static_assert(offsetof(InterfaceTrace, calls) == 8, "shaderapi_thunks.asm reads the counters at offset 8");

struct Wrapper {
    const void* const* vtable;
    void* real;
    InterfaceTrace* trace;
    std::array<const void*, kMaxSlots> slots;
};
static_assert(offsetof(Wrapper, real) == 8 && offsetof(Wrapper, trace) == 16, "shaderapi_thunks.asm reads these offsets");

struct ObjectSlot {
    const void* original;
    std::uint32_t* counter;
    const InterfaceTrace* trace;
    unsigned slot;
};
static_assert(offsetof(ObjectSlot, original) == 0 && offsetof(ObjectSlot, counter) == 8, "shaderapi_thunks.asm reads these offsets");

// mov r10, slot; jmp [rip]; dq gmodpp_object_thunk
#pragma pack(push, 1)
struct ObjectStub {
    std::uint8_t movR10[2];
    const ObjectSlot* slot;
    std::uint8_t jump[6];
    const void* target;
};
#pragma pack(pop)
static_assert(sizeof(ObjectStub) == 24);

struct Benchmark {
    using Clock = std::chrono::steady_clock;
    std::chrono::seconds duration{};
    Clock::time_point start;
    Clock::time_point last;
    std::vector<double> frameMs;
};

struct Shim {
    CreateInterfaceFn gameCreateInterface = nullptr;
    CreateInterfaceFn connectFactory = nullptr;
    CreateInterfaceFn internalFactory = nullptr;
    std::array<InterfaceTrace, kInterfaceCount> traces = {};
    std::array<InterfaceTrace, kObjectClassCount> objectTraces = {};
    std::mutex mutex;
    std::unordered_map<void*, Wrapper*> wrappers;
    std::unordered_set<void*> wrapperObjects;
    std::unordered_set<std::string> shaderFiles;
    void* shaderApi = nullptr;
    void* device = nullptr;
    void* hardwareConfig = nullptr;
    bool devMode = false;
    unsigned frame = 0;
    DWORD mainThread = 0;
    void* engineClient = nullptr;
    std::string inGameLevel;
    bool benchmarking = false;
    Benchmark benchmark;
};

Shim* g_shim = nullptr;
std::atomic<int> g_traceLines = 0;

void StartTrace()
{
    g_traceLines = kTraceLineBudget;
    gmodpp_trace_active = 1;
}

void TraceLine(std::string_view label, std::string_view args)
{
    if (--g_traceLines <= 0) {
        gmodpp_trace_active = 0;
        log::Info("TRACE stopped after {} lines", kTraceLineBudget);
        return;
    }
    log::Info("TRACE {}({})", label, args);
}

template <class Fn>
Fn RealSlot(void* real, unsigned slot)
{
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(real))[slot]);
}

const char* SlotName(const InterfaceInfo& info, unsigned slot)
{
    return slot < info.slotCount ? info.slots[slot].name : nullptr;
}

std::string SlotLabel(const InterfaceInfo& info, unsigned slot)
{
    const char* name = SlotName(info, slot);
    return name != nullptr ? std::format("{}::{}", info.className, name) : std::format("{}::slot{}", info.className, slot);
}

// Reads strings the game passes as arguments; a few (shader source) are not terminated, so the read is bounded and
// guarded.
bool CopyCString(const char* text, char* buffer, size_t capacity)
{
    __try {
        for (size_t i = 0; i + 1 < capacity && text[i] != '\0'; ++i)
            buffer[i] = text[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

std::string ReadCString(const char* text)
{
    char buffer[65] = {};
    return CopyCString(text, buffer, sizeof(buffer)) ? buffer : "<unreadable>";
}

struct Param {
    std::string type;
    std::string name;
};

std::vector<Param> ParseParams(std::string_view params)
{
    std::vector<Param> result;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= params.size(); ++i) {
        char c = i < params.size() ? params[i] : ',';
        if (c == '(')
            ++depth;
        else if (c == ')')
            --depth;
        if (c != ',' || depth != 0)
            continue;
        std::string text(params.substr(start, i - start));
        start = i + 1;
        if (size_t equals = text.find('='); equals != std::string::npos)
            text.resize(equals);
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        while (!text.empty() && text.front() == ' ')
            text.erase(text.begin());
        if (text.empty())
            continue;
        size_t split = text.find_last_of(" *&");
        bool unnamed = split == std::string::npos || text.back() == '*' || text.back() == '&' || text == "...";
        if (unnamed)
            result.push_back({ text, "" });
        else
            result.push_back({ text.substr(0, split + 1), text.substr(split + 1) });
    }
    return result;
}

std::string FormatArg(const Param& param, std::uint64_t bits)
{
    std::string type;
    for (char c : param.type) {
        if (c != ' ')
            type += c;
    }
    if (type.starts_with("const"))
        type.erase(0, 5);
    if (type.starts_with("PRINTF_FORMAT_STRING"))
        type.erase(0, 20);
    bool array = param.name.find('[') != std::string::npos;
    if (type == "char*")
        return bits == 0 ? "null" : std::format("\"{}\"", ReadCString(reinterpret_cast<const char*>(bits)));
    if (type.ends_with('*') || type.ends_with('&') || array || type == "Vector" || type == "va_list")
        return std::format("{:#x}", bits);
    if (type == "float")
        return std::format("{}", std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
    if (type == "double")
        return std::format("{}", std::bit_cast<double>(bits));
    if (type == "bool")
        return (bits & 0xff) != 0 ? "true" : "false";
    if (type == "unsignedchar")
        return std::format("{}", bits & 0xff);
    if (type == "StateSnapshot_t")
        return std::format("{}", static_cast<std::int16_t>(bits));
    if (type.ends_with("Handle_t") || type.ends_with("Fn") || type == "VertexFormat_t" || type == "size_t"
        || type == "ShaderAPIOcclusionQuery_t" || type == "ShaderModeChangeCallbackFunc_t")
        return std::format("{:#x}", bits);
    if (type.starts_with("unsigned") || type == "uint32")
        return std::format("{}", static_cast<std::uint32_t>(bits));
    return std::format("{}", static_cast<std::int32_t>(bits));
}

// regs: rcx, rdx, r8, r9, then xmm0-3 (16 bytes each). stack: the caller's stack pointer at the call, so stack[0]
// is the return address and argument n >= 4 (this is argument 0) is stack[n + 1].
std::string FormatArgs(const SlotInfo& slot, const std::uint64_t* regs, const std::uint64_t* stack)
{
    std::vector<Param> params = ParseParams(slot.params);
    std::string_view returns = slot.returns;
    unsigned position = returns == "Vector" ? 2 : 1;
    std::string text;
    for (const Param& param : params) {
        if (!text.empty())
            text += ", ";
        if (param.type == "...") {
            text += "...";
            break;
        }
        bool floating = param.type == "float" || param.type == "double";
        std::uint64_t bits = 0;
        if (position < 4) {
            if (floating)
                std::memcpy(&bits, reinterpret_cast<const std::uint8_t*>(regs) + 32 + 16 * position, sizeof(bits));
            else
                bits = regs[position];
        } else {
            bits = stack[position + 1];
        }
        ++position;
        if (!param.name.empty())
            text += param.name + '=';
        text += FormatArg(param, bits);
    }
    return text;
}

void Record(Wrapper* wrapper, unsigned slot, std::string_view args)
{
    ++wrapper->trace->calls[slot];
    if (gmodpp_trace_active)
        TraceLine(SlotLabel(*wrapper->trace->info, slot), args);
}

void* WrapIfKnown(const char* name, void* real);

void* __fastcall QueryInterfaceOverride(Wrapper* self, const char* name)
{
    Record(self, kDeviceMgrQueryInterface, name != nullptr ? name : "null");
    void* result = RealSlot<void* (*)(void*, const char*)>(self->real, kDeviceMgrQueryInterface)(self->real, name);
    return name != nullptr ? WrapIfKnown(name, result) : result;
}

// shaderapidx9.dll fetches materialsystem's IShaderUtil from the factory given to IShaderDeviceMgr::Connect.
void* ConnectFactory(const char* name, int* returnCode)
{
    return WrapIfKnown(name, g_shim->connectFactory(name, returnCode));
}

bool __fastcall ConnectOverride(Wrapper* self, CreateInterfaceFn factory)
{
    Record(self, kDeviceMgrConnect, std::format("factory={}", reinterpret_cast<void*>(factory)));
    g_shim->connectFactory = factory;
    return RealSlot<bool (*)(void*, CreateInterfaceFn)>(self->real, kDeviceMgrConnect)(self->real, factory != nullptr ? &ConnectFactory : nullptr);
}

// The shader API's internal factory (shaderapidx9.dll 0x1800241b0) hands out the device, API and shadow interfaces.
// materialsystem fetches them again from it in IShaderUtil::RestoreShaderObjects, which shaderapidx9.dll calls after
// creating the device, and it is also what IShaderDeviceMgr::SetMode (0x180027ef0) returns.
void* InternalFactory(const char* name, int* returnCode)
{
    return WrapIfKnown(name, g_shim->internalFactory(name, returnCode));
}

CreateInterfaceFn WrapInternalFactory(CreateInterfaceFn factory)
{
    if (factory == nullptr)
        return nullptr;
    g_shim->internalFactory = factory;
    return &InternalFactory;
}

CreateInterfaceFn __fastcall SetModeOverride(Wrapper* self, void* window, int adapter, const void* info)
{
    Record(self, kDeviceMgrSetMode, std::format("hWnd={}, nAdapter={}, mode={}", window, adapter, info));
    return WrapInternalFactory(RealSlot<CreateInterfaceFn (*)(void*, void*, int, const void*)>(self->real, kDeviceMgrSetMode)(self->real, window, adapter, info));
}

void __fastcall RestoreShaderObjectsOverride(Wrapper* self, CreateInterfaceFn factory, int changeFlags)
{
    Record(self, kShaderUtilRestoreShaderObjects, std::format("shaderFactory={}, nChangeFlags={}", reinterpret_cast<void*>(factory), changeFlags));
    RealSlot<void (*)(void*, CreateInterfaceFn, int)>(self->real, kShaderUtilRestoreShaderObjects)(self->real, WrapInternalFactory(factory), changeFlags);
}

// Logs each shader file the materials ask for once, for the coverage of the translator and the native backend.
void NoteShaderFile(const char* fileName)
{
    std::string name = ReadCString(fileName);
    std::scoped_lock lock(g_shim->mutex);
    if (g_shim->shaderFiles.insert(name).second)
        log::Info("Shader file: {}", name);
}

void __fastcall SetVertexShaderOverride(Wrapper* self, const char* fileName, int staticIndex)
{
    Record(self, kShadowSetVertexShader, std::format("pFileName=\"{}\", nStaticVshIndex={}", ReadCString(fileName), staticIndex));
    NoteShaderFile(fileName);
    RealSlot<void (*)(void*, const char*, int)>(self->real, kShadowSetVertexShader)(self->real, fileName, staticIndex);
}

void __fastcall SetPixelShaderOverride(Wrapper* self, const char* fileName, int staticIndex)
{
    Record(self, kShadowSetPixelShader, std::format("pFileName=\"{}\", nStaticPshIndex={}", ReadCString(fileName), staticIndex));
    NoteShaderFile(fileName);
    RealSlot<void (*)(void*, const char*, int)>(self->real, kShadowSetPixelShader)(self->real, fileName, staticIndex);
}

void LogCallCounts(std::span<const InterfaceTrace> traces)
{
    for (const InterfaceTrace& trace : traces) {
        if (trace.info == nullptr)
            continue;
        for (unsigned slot = 0; slot < trace.vtableSize; ++slot) {
            if (trace.calls[slot] != 0)
                log::Info("CALLS {} (slot {}): {}", SlotLabel(*trace.info, slot), slot, trace.calls[slot]);
        }
    }
}

bool ReadBytes(const void* source, void* destination, size_t size)
{
    __try {
        std::memcpy(destination, source, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

const char* CommandName(int opcode)
{
    switch (opcode) {
    case kCommandEnd: return "END";
    case kCommandJump: return "JUMP";
    case kCommandJsr: return "JSR";
    case kCommandSetPixelShaderFloatConst: return "SET_PIXEL_SHADER_FLOAT_CONST";
    case kCommandSetVertexShaderFloatConst: return "SET_VERTEX_SHADER_FLOAT_CONST";
    case kCommandSetPixelShaderFogParams: return "SETPIXELSHADERFOGPARAMS";
    case kCommandStoreEyePosInPixelShaderConst: return "STORE_EYE_POS_IN_PSCONST";
    case kCommandCommitPixelShaderLighting: return "COMMITPIXELSHADERLIGHTING";
    case kCommandSetPixelShaderAmbientLightCube: return "SETPIXELSHADERSTATEAMBIENTLIGHTCUBE";
    case kCommandSetVertexShaderAmbientLightCube: return "SETAMBIENTCUBEDYNAMICSTATEVERTEXSHADER";
    case kCommandSetDepthFeatheringConst: return "SET_DEPTH_FEATHERING_CONST";
    case kCommandBindStandardTexture: return "BIND_STANDARD_TEXTURE";
    case kCommandBindShaderApiTextureHandle: return "BIND_SHADERAPI_TEXTURE_HANDLE";
    case kCommandSetPixelShaderIndex: return "SET_PSHINDEX";
    case kCommandSetVertexShaderIndex: return "SET_VSHINDEX";
    case kCommandSetSamplerPointFilter: return "SET_SAMPLER_POINT_FILTER";
    default: return nullptr;
    }
}

// Logs a command buffer the way GMod's ExecuteCommandBuffer walks it (see shaderapi_commands.h).
void TraceCommandBuffer(const std::uint8_t* buffer, int depth)
{
    while (gmodpp_trace_active) {
        int header[3] = {};
        if (!ReadBytes(buffer, header, sizeof(header))) {
            TraceLine("  CBCMD", std::format("unreadable at {}", static_cast<const void*>(buffer)));
            return;
        }
        const int opcode = header[0];
        const char* name = CommandName(opcode);
        if (name == nullptr) {
            TraceLine("  CBCMD", std::format("unknown opcode {} at {}", opcode, static_cast<const void*>(buffer)));
            return;
        }
        std::string args;
        size_t size = 8;
        switch (opcode) {
        case kCommandEnd:
            TraceLine("  CBCMD", name);
            return;
        case kCommandJump:
        case kCommandJsr: {
            const std::uint8_t* target = nullptr;
            ReadBytes(buffer + 4, &target, sizeof(target));
            TraceLine("  CBCMD", std::format("{} {}", name, static_cast<const void*>(target)));
            if (opcode == kCommandJump) {
                buffer = target;
                continue;
            }
            if (depth < kMaxCommandBufferDepth)
                TraceCommandBuffer(target, depth + 1);
            buffer += 12;
            continue;
        }
        case kCommandSetPixelShaderFloatConst:
        case kCommandSetVertexShaderFloatConst: {
            const int floats = header[2] * 4;
            float values[kTracedCommandFloats] = {};
            const int shown = std::clamp(floats, 0, kTracedCommandFloats);
            ReadBytes(buffer + 12, values, shown * sizeof(float));
            args = std::format("first={}, count={}, values=", header[1], header[2]);
            for (int i = 0; i < shown; ++i)
                args += std::format("{}{}", i == 0 ? "" : " ", values[i]);
            if (shown < floats)
                args += " ...";
            size = 12 + static_cast<size_t>(std::max(floats, 0)) * sizeof(float);
            break;
        }
        case kCommandSetVertexShaderAmbientLightCube:
            size = 4;
            break;
        case kCommandSetDepthFeatheringConst:
            args = std::format("register={}, scale={}", header[1], std::bit_cast<float>(header[2]));
            size = 12;
            break;
        case kCommandBindStandardTexture:
        case kCommandSetSamplerPointFilter:
            args = std::format("{}, {}", header[1], header[2]);
            size = 12;
            break;
        case kCommandBindShaderApiTextureHandle: {
            std::uint64_t handle = 0;
            ReadBytes(buffer + 8, &handle, sizeof(handle));
            args = std::format("sampler={}, handle={:#x}", header[1], handle);
            size = 16;
            break;
        }
        default:
            args = std::format("{}", header[1]);
            break;
        }
        TraceLine("  CBCMD", std::format("{} {}", name, args));
        buffer += size;
    }
}

void WriteGetter(std::ofstream& out, std::string_view prefix, void* object, unsigned slot, const SlotInfo& entry)
{
    std::string_view returns = entry.returns;
    if (returns == "bool")
        out << std::format("{}{} {} = {}\n", prefix, slot, entry.name, RealSlot<bool (*)(void*)>(object, slot)(object));
    else if (returns == "const char *")
        out << std::format("{}{} {} = \"{}\"\n", prefix, slot, entry.name, RealSlot<const char* (*)(void*)>(object, slot)(object));
    else
        out << std::format("{}{} {} = {}\n", prefix, slot, entry.name, RealSlot<int (*)(void*)>(object, slot)(object));
}

unsigned FindSlot(const InterfaceInfo& info, std::string_view name)
{
    for (unsigned slot = 0; slot < info.slotCount; ++slot) {
        if (name == info.slots[slot].name)
            return slot;
    }
    log::Info("Caps: {} has no {} in shaderapi_slots.cpp", info.className, name);
    return static_cast<unsigned>(info.slotCount);
}

// The answers of the real shader API that the native backend has to reproduce: every hardware-config getter, the
// argument-free getters of IShaderAPI and IShaderDevice, the formats it substitutes and the MSAA modes it accepts.
void DumpCaps(const std::filesystem::path& path)
{
    void* config = g_shim->hardwareConfig;
    if (config == nullptr) {
        log::Info("Caps: the hardware config has not been requested yet");
        return;
    }
    std::ofstream out(path);
    const InterfaceInfo& info = kInterfaces[kHardwareConfig];
    for (unsigned slot = 0; slot < info.slotCount; ++slot) {
        const SlotInfo& entry = info.slots[slot];
        std::string_view name = entry.name;
        if (name == "SupportsHDRMode") {
            for (int mode = 0; mode < 3; ++mode)
                out << std::format("{} {}({}) = {}\n", slot, name, mode, RealSlot<bool (*)(void*, int)>(config, slot)(config, mode));
            continue;
        }
        if (std::string_view(entry.returns) != "void" && entry.params[0] == '\0')
            WriteGetter(out, "", config, slot, entry);
    }
    // CHardwareConfig's own getters after the interface, read by decompiling shaderapidx9.dll (vtable RVA 0x7a8f0):
    // slot 73 returns a string or null, 74-77 return ints; the vertex lighting commit checks 74 and 75 before it
    // writes the bool and int registers.
    const char* text = RealSlot<const char* (*)(void*)>(config, 73)(config);
    out << std::format("73 slot73 = \"{}\"\n", text != nullptr ? text : "");
    for (unsigned slot = 74; slot <= 77; ++slot)
        out << std::format("{} slot{} = {}\n", slot, slot, RealSlot<int (*)(void*)>(config, slot)(config));

    if (void* device = g_shim->device) {
        const InterfaceInfo& deviceInfo = kInterfaces[kDevice];
        for (std::string_view name : { "GetBackBufferFormat", "GetCurrentAdapter", "StencilBufferBits", "IsAAEnabled" }) {
            unsigned slot = FindSlot(deviceInfo, name);
            if (slot < deviceInfo.slotCount)
                WriteGetter(out, "IShaderDevice ", device, slot, deviceInfo.slots[slot]);
        }
    }
    void* api = g_shim->shaderApi;
    if (api == nullptr) {
        log::Info("Caps: wrote {} without the shader API", path.string());
        return;
    }
    const InterfaceInfo& apiInfo = kInterfaces[kShaderApi];
    for (std::string_view name : { "GetMaxLights", "DoRenderTargetsNeedSeparateDepthBuffer", "CanDownloadTextures", "GetCurrentDynamicVBSize",
             "GetMaxIndicesToRender", "SupportsShadowDepthTextures", "GetShadowDepthTextureFormat", "SupportsFetch4", "GetNullTextureFormat",
             "SupportsNormalMapCompression", "IsHWMorphingEnabled" }) {
        unsigned slot = FindSlot(apiInfo, name);
        if (slot < apiInfo.slotCount)
            WriteGetter(out, "IShaderAPI ", api, slot, apiInfo.slots[slot]);
    }
    unsigned nearest = FindSlot(apiInfo, "GetNearestSupportedFormat");
    unsigned nearestTarget = FindSlot(apiInfo, "GetNearestRenderTargetFormat");
    unsigned msaa = FindSlot(apiInfo, "SupportsMSAAMode");
    if (nearest < apiInfo.slotCount && nearestTarget < apiInfo.slotCount) {
        for (int format = 0; format < kImageFormatCount; ++format) {
            out << std::format("IShaderAPI format {}: nearest filtered {}, unfiltered {}, render target {}\n", format,
                RealSlot<int (*)(void*, int, bool)>(api, nearest)(api, format, true),
                RealSlot<int (*)(void*, int, bool)>(api, nearest)(api, format, false),
                RealSlot<int (*)(void*, int)>(api, nearestTarget)(api, format));
        }
    }
    if (msaa < apiInfo.slotCount) {
        for (int samples = 0; samples <= kMaxMsaaSamples; ++samples)
            out << std::format("IShaderAPI SupportsMSAAMode({}) = {}\n", samples, RealSlot<bool (*)(void*, int)>(api, msaa)(api, samples));
    }
    log::Info("Caps: wrote {}", path.string());
}

void Capture(const std::filesystem::path& path)
{
    if (g_shim->shaderApi == nullptr || g_shim->device == nullptr) {
        log::Info("Capture: the shader API has not been requested yet");
        return;
    }
    int width = 0, height = 0;
    RealSlot<void (*)(void*, int&, int&)>(g_shim->device, kDeviceGetBackBufferDimensions)(g_shim->device, width, height);
    if (width <= 0 || height <= 0) {
        log::Info("Capture: no back buffer ({}x{})", width, height);
        return;
    }
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    RealSlot<void (*)(void*, int, int, int, int, unsigned char*, int)>(g_shim->shaderApi, kShaderApiReadPixels)(
        g_shim->shaderApi, 0, 0, width, height, pixels.data(), kImageFormatBgra8888);
    bool written = WriteBmp(path, static_cast<UINT>(width), static_cast<UINT>(height), pixels.data(), static_cast<size_t>(width) * 4);
    log::Info("Capture: {} {}", written ? "wrote" : "failed to write", path.string());
}

void* EngineClient()
{
    if (g_shim->engineClient == nullptr) {
        HMODULE engine = GetModuleHandleW(L"engine.dll");
        auto create = engine != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
        g_shim->engineClient = create != nullptr ? create("VEngineClient013", nullptr) : nullptr;
    }
    return g_shim->engineClient;
}

// Logs when the client has fully joined a map and when it leaves, so test scripts can wait for these lines.
void TrackLevel()
{
    void* engine = EngineClient();
    if (engine == nullptr || GetCurrentThreadId() != g_shim->mainThread)
        return;
    const char* level = RealSlot<bool (*)(void*)>(engine, kEngineIsInGame)(engine)
        ? RealSlot<const char* (*)(void*)>(engine, kEngineGetLevelName)(engine)
        : nullptr;
    std::string current = level != nullptr ? level : "";
    if (current == g_shim->inGameLevel)
        return;
    if (current.empty())
        log::Info("Dev: left {} at frame {}", g_shim->inGameLevel, g_shim->frame);
    else
        log::Info("Dev: in game on {} at frame {}", current, g_shim->frame);
    g_shim->inGameLevel = std::move(current);
}

// Runs console.txt once the client is in game, on the thread that loaded the shim (the engine's main thread).
void RunConsoleRequest()
{
    std::filesystem::path path = ModDirectory() / L"console.txt";
    if (g_shim->inGameLevel.empty())
        return;
    std::ifstream file(path);
    if (!file)
        return;
    std::string commands((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    DeleteFileW(path.c_str());
    if (!commands.empty() && commands.back() != '\n')
        commands += '\n';
    log::Info("Console request: {}", commands);
    RealSlot<void (*)(void*, const char*)>(g_shim->engineClient, kEngineClientCmdUnrestricted)(g_shim->engineClient, commands.c_str());
}

bool TakeRequest(const wchar_t* name)
{
    std::filesystem::path path = ModDirectory() / name;
    return DeleteFileW(path.c_str()) != FALSE;
}

// bench.request holds the duration in seconds, or nothing for the default.
void StartBenchmark()
{
    std::filesystem::path path = ModDirectory() / L"bench.request";
    std::ifstream file(path);
    if (!file)
        return;
    int seconds = kDefaultBenchmarkSeconds;
    if (!(file >> seconds) || seconds <= 0)
        seconds = kDefaultBenchmarkSeconds;
    file.close();
    DeleteFileW(path.c_str());
    Benchmark& benchmark = g_shim->benchmark;
    benchmark.duration = std::chrono::seconds(seconds);
    benchmark.start = benchmark.last = Benchmark::Clock::now();
    benchmark.frameMs.clear();
    g_shim->benchmarking = true;
    log::Info("Benchmark: {} s from frame {}", seconds, g_shim->frame);
}

void RecordBenchmarkFrame()
{
    Benchmark& benchmark = g_shim->benchmark;
    Benchmark::Clock::time_point now = Benchmark::Clock::now();
    benchmark.frameMs.push_back(std::chrono::duration<double, std::milli>(now - benchmark.last).count());
    benchmark.last = now;
    if (now - benchmark.start < benchmark.duration)
        return;
    g_shim->benchmarking = false;
    std::vector<double>& frames = benchmark.frameMs;
    double total = 0;
    for (double ms : frames)
        total += ms;
    double average = total / static_cast<double>(frames.size());
    size_t slowest = frames.size() / 100;
    std::nth_element(frames.begin(), frames.begin() + slowest, frames.end(), std::greater<>());
    double percentile99 = frames[slowest];
    log::Info("Benchmark: {} frames, average {:.3f} ms ({:.1f} fps), 99th percentile {:.3f} ms ({:.1f} fps)", frames.size(),
        average, 1000.0 / average, percentile99, 1000.0 / percentile99);
}

void __fastcall PresentOverride(Wrapper* self)
{
    Record(self, kDevicePresent, "");
    unsigned frame = ++g_shim->frame;
    if (frame == 1)
        log::Info("First IShaderDevice::Present");
    if (g_shim->benchmarking)
        RecordBenchmarkFrame();
    if (gmodpp_trace_active) {
        gmodpp_trace_active = 0;
        log::Info("TRACE end of frame {}", frame);
        LogCallCounts(g_shim->traces);
        LogCallCounts(g_shim->objectTraces);
    }
    if (g_shim->devMode && frame % kDevFilePollFrames == 0) {
        if (TakeRequest(L"capture.request")) {
            std::error_code error;
            std::filesystem::create_directories(ModDirectory() / L"captures", error);
            Capture(ModDirectory() / L"captures" / std::format(L"capture_{}.bmp", frame));
        }
        if (TakeRequest(L"caps.request"))
            DumpCaps(ModDirectory() / L"caps.txt");
        if (TakeRequest(L"trace.request")) {
            log::Info("TRACE frame {}", frame + 1);
            StartTrace();
        }
        if (!g_shim->benchmarking)
            StartBenchmark();
        if (TakeRequest(L"profile.request"))
            watchdog::ProfileThisThread(10);
        if (TakeRequest(L"shaders.request"))
            shader_precache::EnqueueEveryShader(GetOptions().gameDirectory);
        TrackLevel();
        RunConsoleRequest();
    }
    RealSlot<void (*)(void*)>(self->real, kDevicePresent)(self->real);
}

// Counts the entries pointing into the code of the module that holds the first one.
size_t VtableSize(const void* const* vtableEntries)
{
    auto* vtable = reinterpret_cast<const uintptr_t*>(vtableEntries);
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(vtable[0]), &module))
        return 0;
    auto base = reinterpret_cast<uintptr_t>(module);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uintptr_t codeBegin = base + nt->OptionalHeader.BaseOfCode;
    uintptr_t codeEnd = codeBegin + nt->OptionalHeader.SizeOfCode;
    size_t count = 0;
    while (vtable[count] >= codeBegin && vtable[count] < codeEnd)
        ++count;
    return count;
}

void* WrapIfKnown(const char* name, void* real)
{
    if (real == nullptr)
        return nullptr;
    int index = -1;
    for (int i = 0; i < kInterfaceCount; ++i) {
        if (std::strcmp(kInterfaces[i].version, name) == 0)
            index = i;
    }
    if (index < 0)
        return real;

    std::scoped_lock lock(g_shim->mutex);
    if (auto found = g_shim->wrappers.find(real); found != g_shim->wrappers.end())
        return found->second;
    // materialsystem's factory hands back interfaces it got from us.
    if (g_shim->wrapperObjects.contains(real))
        return real;

    const InterfaceInfo& info = kInterfaces[index];
    size_t vtableSize = VtableSize(*static_cast<const void* const* const*>(real));
    if (vtableSize > kMaxSlots || vtableSize > gmodpp_thunk_count || vtableSize < info.slotCount) {
        log::Info("{}: {} vtable entries, expected {} to {}; not traced (re-check shaderapi_slots.cpp)", name, vtableSize, info.slotCount, kMaxSlots);
        return real;
    }
    InterfaceTrace& trace = g_shim->traces[index];
    trace.info = &info;
    trace.vtableSize = vtableSize;

    auto* wrapper = new Wrapper{};
    wrapper->real = real;
    wrapper->trace = &trace;
    for (size_t slot = 0; slot < kMaxSlots; ++slot)
        wrapper->slots[slot] = gmodpp_thunks[slot];
    switch (index) {
    case kDeviceMgr:
        wrapper->slots[kDeviceMgrConnect] = reinterpret_cast<const void*>(&ConnectOverride);
        wrapper->slots[kDeviceMgrQueryInterface] = reinterpret_cast<const void*>(&QueryInterfaceOverride);
        wrapper->slots[kDeviceMgrSetMode] = reinterpret_cast<const void*>(&SetModeOverride);
        break;
    case kDevice:
        wrapper->slots[kDevicePresent] = reinterpret_cast<const void*>(&PresentOverride);
        g_shim->device = real;
        break;
    case kShaderApi:
        g_shim->shaderApi = real;
        break;
    case kHardwareConfig:
        g_shim->hardwareConfig = real;
        break;
    case kShadow:
        wrapper->slots[kShadowSetVertexShader] = reinterpret_cast<const void*>(&SetVertexShaderOverride);
        wrapper->slots[kShadowSetPixelShader] = reinterpret_cast<const void*>(&SetPixelShaderOverride);
        break;
    case kShaderUtil:
        wrapper->slots[kShaderUtilRestoreShaderObjects] = reinterpret_cast<const void*>(&RestoreShaderObjectsOverride);
        break;
    default:
        break;
    }
    wrapper->vtable = wrapper->slots.data();
    g_shim->wrappers.emplace(real, wrapper);
    g_shim->wrapperObjects.insert(wrapper);
    log::Info("Tracing {} ({} slots, {} named)", name, vtableSize, info.slotCount);
    return wrapper;
}

// Points every slot of the shader API's mesh and buffer vtables at a counting stub. The vtables are patched in
// place before any object exists, so the objects stay the game's own and pointers to them need no translation.
void TraceObjectClasses(HMODULE module)
{
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
    gmodpp_internal_begin = reinterpret_cast<std::uintptr_t>(module);
    gmodpp_internal_end = gmodpp_internal_begin + nt->OptionalHeader.SizeOfImage;

    for (size_t index = 0; index < kObjectClassCount; ++index) {
        const ObjectClass& objectClass = kObjectClasses[index];
        const InterfaceInfo& info = objectClass.info;
        auto* vtable = const_cast<const void**>(FindVtable(module, objectClass.rttiName, objectClass.subobjectOffset));
        size_t vtableSize = vtable != nullptr ? VtableSize(vtable) : 0;
        if (vtableSize < info.slotCount || vtableSize > kMaxSlots) {
            log::Info("{}: vtable {} with {} entries, expected {} to {}; not traced (re-check shaderapi_slots.cpp)", info.className,
                static_cast<const void*>(vtable), vtableSize, info.slotCount, kMaxSlots);
            continue;
        }
        auto* stubs = static_cast<ObjectStub*>(VirtualAlloc(nullptr, vtableSize * sizeof(ObjectStub), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        DWORD protection = 0;
        if (stubs == nullptr || !VirtualProtect(vtable, vtableSize * sizeof(void*), PAGE_READWRITE, &protection)) {
            log::Info("{}: cannot patch the vtable: {}", info.className, GetLastError());
            continue;
        }
        InterfaceTrace& trace = g_shim->objectTraces[index];
        trace.info = &info;
        trace.vtableSize = vtableSize;
        auto* slots = new ObjectSlot[vtableSize];
        for (unsigned slot = 0; slot < vtableSize; ++slot) {
            slots[slot] = { vtable[slot], &trace.calls[slot], &trace, slot };
            stubs[slot] = { { 0x49, 0xba }, &slots[slot], { 0xff, 0x25, 0, 0, 0, 0 }, reinterpret_cast<const void*>(&gmodpp_object_thunk) };
        }
        DWORD stubProtection = 0;
        VirtualProtect(stubs, vtableSize * sizeof(ObjectStub), PAGE_EXECUTE_READ, &stubProtection);
        FlushInstructionCache(GetCurrentProcess(), stubs, vtableSize * sizeof(ObjectStub));
        for (unsigned slot = 0; slot < vtableSize; ++slot)
            vtable[slot] = &stubs[slot];
        VirtualProtect(vtable, vtableSize * sizeof(void*), protection, &protection);
        log::Info("Tracing {} ({} slots, {} named)", info.className, vtableSize, info.slotCount);
    }
}

HMODULE LoadSystemD3D9()
{
    wchar_t directory[MAX_PATH];
    UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return nullptr;
    return LoadLibraryW((std::filesystem::path(directory) / L"d3d9.dll").c_str());
}

Shim* LoadShim()
{
    std::error_code error;
    std::filesystem::create_directories(ModDirectory() / L"logs", error);
    log::Open((ModDirectory() / L"logs" / L"gmodpp.log").c_str());
    auto* shim = new Shim{};
    shim->devMode = HasArgument(L"-gmodpp_dev");
    shim->mainThread = GetCurrentThreadId();
    g_shim = shim;

    if (!HasArgument(L"-gmodpp_vanilla")) {
        log::Info("Native Direct3D 12 shader API");
        shim->gameCreateInterface = &native::CreateInterface;
    } else {
        // shaderapidx9.dll imports d3d9.dll by name, and a loaded module of that name satisfies the import whatever
        // its directory, so loading the system one first keeps our bin\win64\d3d9.dll out of the reference.
        if (LoadSystemD3D9() == nullptr)
            log::Info("Failed to load the system d3d9.dll: {}", GetLastError());
        std::filesystem::path path = GetOptions().gameDirectory / L"bin" / L"win64" / L"shaderapidx9.dll";
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr) {
            log::Info("Failed to load {}: {}", path.string(), GetLastError());
            return nullptr;
        }
        auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
        if (createInterface == nullptr) {
            log::Info("{} has no CreateInterface export", path.string());
            return nullptr;
        }
        shim->gameCreateInterface = createInterface;
        log::Info("Vanilla reference: tracing {}", path.string());
        TraceObjectClasses(module);
    }
    if (shim->devMode && TakeRequest(L"trace.request")) {
        log::Info("TRACE from startup");
        StartTrace();
    }
    return shim;
}

void* CreateInterfaceExport(const char* name, int* returnCode)
{
    static Shim* const shim = LoadShim();
    if (shim == nullptr) {
        if (returnCode != nullptr)
            *returnCode = kInterfaceFailed;
        return nullptr;
    }
    // This factory is in the engine's global chain, so most requests are other modules' interfaces.
    return WrapIfKnown(name, shim->gameCreateInterface(name, returnCode));
}

}

namespace gmodpp {

const Options& GetOptions()
{
    static const Options options = [] {
        Options loaded;
        loaded.devMode = HasArgument(L"-gmodpp_dev");
        loaded.d3d12Debug = HasArgument(L"-gmodpp_debug");
        loaded.gpuValidation = HasArgument(L"-gmodpp_gbv");
        loaded.gameDirectory = ExecutablePath().parent_path();
        return loaded;
    }();
    return options;
}

}

extern "C" void gmodpp_TraceCall(void* wrapper, DWORD slot, const std::uint64_t* regs, const std::uint64_t* stack)
{
    const InterfaceInfo& info = *static_cast<Wrapper*>(wrapper)->trace->info;
    std::string args = slot < info.slotCount ? FormatArgs(info.slots[slot], regs, stack)
                                             : std::format("{:#x}, {:#x}, {:#x}", regs[1], regs[2], regs[3]);
    TraceLine(SlotLabel(info, slot), args);
    if (&info == &kInterfaces[kShaderApi] && slot == kShaderApiExecuteCommandBuffer)
        TraceCommandBuffer(reinterpret_cast<const std::uint8_t*>(regs[1]), 0);
    // Constants set directly (not through a command buffer) are logged with their values, as command buffers' are.
    const char* name = SlotName(info, slot);
    if (&info == &kInterfaces[kShaderApi] && name != nullptr &&
        (std::string_view(name) == "SetVertexShaderConstant" || std::string_view(name) == "SetPixelShaderConstant")) {
        float values[kTracedCommandFloats] = {};
        const int shown = std::clamp(static_cast<int>(regs[3]) * 4, 0, kTracedCommandFloats);
        if (ReadBytes(reinterpret_cast<const std::uint8_t*>(regs[2]), values, shown * sizeof(float))) {
            std::string text = "values=";
            for (int i = 0; i < shown; ++i)
                text += std::format("{}{}", i == 0 ? "" : " ", values[i]);
            TraceLine("  CONST", text);
        }
    }
}

extern "C" void gmodpp_TraceObjectCall(const ObjectSlot* slot, const std::uint64_t* regs, const std::uint64_t* stack)
{
    const InterfaceInfo& info = *slot->trace->info;
    const char* name = SlotName(info, slot->slot);
    std::string label = name != nullptr ? std::format("{}[{:#x}]::{}", info.className, regs[0], name)
                                        : std::format("{}[{:#x}]::slot{}", info.className, regs[0], slot->slot);
    std::string args = name != nullptr ? FormatArgs(info.slots[slot->slot], regs, stack)
                                       : std::format("{:#x}, {:#x}, {:#x}", regs[1], regs[2], regs[3]);
    TraceLine(label, args);
}

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* returnCode)
{
    return CreateInterfaceExport(name, returnCode);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
