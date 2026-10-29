#include "capture.h"
#include "command_line.h"
#include "log.h"
#include "shaderapi_slots.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" std::uint8_t gmodpp_trace_active;
extern "C" const void* const gmodpp_thunks[];
extern "C" const DWORD gmodpp_thunk_count;

using namespace gmodpp;
using namespace gmodpp::shim;

namespace {

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

constexpr int kInterfaceFailed = 1;
constexpr size_t kMaxSlots = 320;
constexpr int kImageFormatBgra8888 = 12;
constexpr int kDevFilePollFrames = 30;
constexpr int kTraceLineBudget = 200000;

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

// Slots called by the shim itself, in GMod's layout (see shaderapi_slots.cpp).
constexpr unsigned kDeviceMgrConnect = 0;
constexpr unsigned kDeviceMgrQueryInterface = 2;
constexpr unsigned kDeviceMgrSetMode = 12;
constexpr unsigned kDeviceGetBackBufferDimensions = 3;
constexpr unsigned kDevicePresent = 9;
constexpr unsigned kShaderApiReadPixels = 159;
constexpr unsigned kShaderUtilRestoreShaderObjects = 12;
// IVEngineClient013 slots, confirmed in GMod's engine.dll (see engine_interfaces.cpp).
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

struct Shim {
    std::filesystem::path gameDirectory;
    CreateInterfaceFn gameCreateInterface = nullptr;
    CreateInterfaceFn connectFactory = nullptr;
    CreateInterfaceFn internalFactory = nullptr;
    std::array<InterfaceTrace, kInterfaceCount> traces = {};
    std::mutex mutex;
    std::unordered_map<void*, Wrapper*> wrappers;
    std::unordered_set<void*> wrapperObjects;
    void* shaderApi = nullptr;
    void* device = nullptr;
    void* hardwareConfig = nullptr;
    bool devMode = false;
    unsigned frame = 0;
    DWORD mainThread = 0;
    void* engineClient = nullptr;
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

void LogCallCounts()
{
    for (const InterfaceTrace& trace : g_shim->traces) {
        if (trace.info == nullptr)
            continue;
        for (unsigned slot = 0; slot < trace.vtableSize; ++slot) {
            if (trace.calls[slot] != 0)
                log::Info("CALLS {} (slot {}): {}", SlotLabel(*trace.info, slot), slot, trace.calls[slot]);
        }
    }
}

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
        std::string_view returns = entry.returns;
        std::string_view name = entry.name;
        if (name == "SupportsHDRMode") {
            for (int mode = 0; mode < 3; ++mode)
                out << std::format("{} {}({}) = {}\n", slot, name, mode, RealSlot<bool (*)(void*, int)>(config, slot)(config, mode));
            continue;
        }
        if (returns == "void" || entry.params[0] != '\0')
            continue;
        if (returns == "bool")
            out << std::format("{} {} = {}\n", slot, name, RealSlot<bool (*)(void*)>(config, slot)(config));
        else if (returns == "const char *")
            out << std::format("{} {} = \"{}\"\n", slot, name, RealSlot<const char* (*)(void*)>(config, slot)(config));
        else
            out << std::format("{} {} = {}\n", slot, name, RealSlot<int (*)(void*)>(config, slot)(config));
    }
    // CHardwareConfig's own getters after the interface, read by decompiling shaderapidx9.dll (vtable RVA 0x7a8f0):
    // slot 73 returns a string or null, 74-77 return ints.
    const char* text = RealSlot<const char* (*)(void*)>(config, 73)(config);
    out << std::format("73 slot73 = \"{}\"\n", text != nullptr ? text : "");
    for (unsigned slot = 74; slot <= 77; ++slot)
        out << std::format("{} slot{} = {}\n", slot, slot, RealSlot<int (*)(void*)>(config, slot)(config));
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

// Runs gmodpp_console.txt once a map is loaded, on the thread that loaded the shim (the engine's main thread).
void RunConsoleRequest()
{
    std::filesystem::path path = g_shim->gameDirectory / L"gmodpp_console.txt";
    std::ifstream file(path);
    if (!file || GetCurrentThreadId() != g_shim->mainThread)
        return;
    if (g_shim->engineClient == nullptr) {
        HMODULE engine = GetModuleHandleW(L"engine.dll");
        auto create = engine != nullptr ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(engine, "CreateInterface")) : nullptr;
        g_shim->engineClient = create != nullptr ? create("VEngineClient013", nullptr) : nullptr;
        if (g_shim->engineClient == nullptr)
            return;
    }
    const char* level = RealSlot<const char* (*)(void*)>(g_shim->engineClient, kEngineGetLevelName)(g_shim->engineClient);
    if (level == nullptr || level[0] == '\0')
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
    std::filesystem::path path = g_shim->gameDirectory / name;
    return DeleteFileW(path.c_str()) != FALSE;
}

void __fastcall PresentOverride(Wrapper* self)
{
    Record(self, kDevicePresent, "");
    unsigned frame = ++g_shim->frame;
    if (frame == 1)
        log::Info("First IShaderDevice::Present");
    if (gmodpp_trace_active) {
        gmodpp_trace_active = 0;
        log::Info("TRACE end of frame {}", frame);
        LogCallCounts();
    }
    if (g_shim->devMode && frame % kDevFilePollFrames == 0) {
        if (TakeRequest(L"gmodpp_capture.request"))
            Capture(g_shim->gameDirectory / std::format(L"gmodpp_capture_{}.bmp", frame));
        if (TakeRequest(L"gmodpp_caps.request"))
            DumpCaps(g_shim->gameDirectory / L"gmodpp_caps.txt");
        if (TakeRequest(L"gmodpp_trace.request")) {
            log::Info("TRACE frame {}", frame + 1);
            StartTrace();
        }
        RunConsoleRequest();
    }
    RealSlot<void (*)(void*)>(self->real, kDevicePresent)(self->real);
}

// Counts the entries pointing into the code of the module that holds the first one.
size_t VtableSize(void* object)
{
    auto* vtable = *reinterpret_cast<uintptr_t**>(object);
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
    size_t vtableSize = VtableSize(real);
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
    std::filesystem::path gameDirectory = ExecutablePath().parent_path();
    log::Open((gameDirectory / L"gmodpp.log").c_str());
    if (!HasArgument(L"-gmodpp_vanilla")) {
        log::Info("gmodpp.dll: the native backend is not implemented yet; launch with -gmodpp_vanilla");
        return nullptr;
    }

    // shaderapidx9.dll imports d3d9.dll by name, and a loaded module of that name satisfies the import whatever its
    // directory, so loading the system one first keeps our bin\win64\d3d9.dll out of the reference.
    if (LoadSystemD3D9() == nullptr)
        log::Info("Failed to load the system d3d9.dll: {}", GetLastError());
    std::filesystem::path path = gameDirectory / L"bin" / L"win64" / L"shaderapidx9.dll";
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

    auto* shim = new Shim{};
    shim->gameDirectory = gameDirectory;
    shim->gameCreateInterface = createInterface;
    shim->devMode = HasArgument(L"-gmodpp_dev");
    shim->mainThread = GetCurrentThreadId();
    log::Info("Vanilla reference: tracing {}", path.string());
    g_shim = shim;
    if (shim->devMode && TakeRequest(L"gmodpp_trace.request")) {
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

extern "C" void gmodpp_TraceCall(void* wrapper, DWORD slot, const std::uint64_t* regs, const std::uint64_t* stack)
{
    const InterfaceInfo& info = *static_cast<Wrapper*>(wrapper)->trace->info;
    std::string args = slot < info.slotCount ? FormatArgs(info.slots[slot], regs, stack)
                                             : std::format("{:#x}, {:#x}, {:#x}", regs[1], regs[2], regs[3]);
    TraceLine(SlotLabel(info, slot), args);
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
