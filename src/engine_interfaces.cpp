#include "engine_interfaces.h"

#include "log.h"

#include <windows.h>

namespace gmodpp::engine {

namespace {

using CreateInterfaceFn = void*(__cdecl*)(const char* name, int* returnCode);

// Vtable slots follow the Source SDK 2013 declarations of IVEngineClient013 and IBaseFileSystem and were
// confirmed in GMod's engine.dll (GetLevelName, ClientCmd_Unrestricted) and filesystem_stdio.dll
// (Read/Open/Close/Seek/Tell) with Ghidra.
constexpr int kGetLevelNameSlot = 51;
constexpr int kClientCmdUnrestrictedSlot = 106;
constexpr int kReadSlot = 0;
constexpr int kOpenSlot = 2;
constexpr int kCloseSlot = 3;
constexpr int kSeekSlot = 4;
constexpr int kTellSlot = 5;
constexpr int kSeekTail = 2;
constexpr int kSeekHead = 0;

void* g_engineClient = nullptr;
void* g_fileSystem = nullptr;
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
    void* engineClient = CreateFrom(L"engine.dll", "VEngineClient013", engineModule);
    void* fileSystem = CreateFrom(L"filesystem_stdio.dll", "VBaseFileSystem011", fileSystemModule);
    if (engineModule == nullptr || fileSystemModule == nullptr)
        return;
    g_attempted = true;
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

void ExecuteCommand(const std::string& command)
{
    if (Ready())
        Slot<void(__fastcall*)(void*, const char*)>(g_engineClient, kClientCmdUnrestrictedSlot)(g_engineClient, command.c_str());
}

bool ReadGameFile(const std::string& path, std::vector<std::byte>& data)
{
    if (!Ready())
        return false;
    using OpenFn = void*(__fastcall*)(void*, const char*, const char*, const char*);
    using CloseFn = void(__fastcall*)(void*, void*);
    using SeekFn = void(__fastcall*)(void*, void*, int, int);
    using TellFn = unsigned int(__fastcall*)(void*, void*);
    using ReadFn = int(__fastcall*)(void*, void*, int, void*);

    void* file = Slot<OpenFn>(g_fileSystem, kOpenSlot)(g_fileSystem, path.c_str(), "rb", "GAME");
    if (file == nullptr)
        return false;
    Slot<SeekFn>(g_fileSystem, kSeekSlot)(g_fileSystem, file, 0, kSeekTail);
    unsigned int size = Slot<TellFn>(g_fileSystem, kTellSlot)(g_fileSystem, file);
    Slot<SeekFn>(g_fileSystem, kSeekSlot)(g_fileSystem, file, 0, kSeekHead);
    data.resize(size);
    int read = size > 0 ? Slot<ReadFn>(g_fileSystem, kReadSlot)(g_fileSystem, data.data(), static_cast<int>(size), file) : 0;
    Slot<CloseFn>(g_fileSystem, kCloseSlot)(g_fileSystem, file);
    return read == static_cast<int>(size) && size > 0;
}

}
