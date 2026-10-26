#include "watchdog.h"

#include "log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <atomic>
#include <string>
#include <thread>

namespace gmodpp::watchdog {

namespace {

constexpr ULONGLONG kStallMilliseconds = 15000;
constexpr size_t kMaxFrames = 48;

std::atomic<ULONGLONG> g_lastPresent = 0;
std::atomic<bool> g_started = false;

size_t Unwind(CONTEXT context, std::array<DWORD64, kMaxFrames>& frames)
{
    size_t count = 0;
    while (count < frames.size() && context.Rip != 0) {
        frames[count++] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function == nullptr) {
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
    }
    return count;
}

std::string Describe(DWORD64 address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module))
        return std::format("{:#x}", address);
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    std::string_view name(path, length);
    if (size_t slash = name.find_last_of("\\/"); slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    return std::format("{}+{:#x}", name, address - reinterpret_cast<DWORD64>(module));
}

void DumpThreads()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry = {sizeof(entry)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
            continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr)
            continue;
        std::array<DWORD64, kMaxFrames> frames;
        size_t count = 0;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context = {};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context))
                count = Unwind(context, frames);
            ResumeThread(thread);
        }
        CloseHandle(thread);

        std::string stack;
        for (size_t i = 0; i < count; ++i)
            stack += std::format("\n    #{} {}", i, Describe(frames[i]));
        log::Info("Thread {}:{}", entry.th32ThreadID, stack);
    }
    CloseHandle(snapshot);
}

}

void Start()
{
    if (g_started.exchange(true))
        return;
    NotePresent();
    std::thread([] {
        bool dumped = false;
        for (;;) {
            Sleep(1000);
            ULONGLONG stalled = GetTickCount64() - g_lastPresent.load(std::memory_order_relaxed);
            if (stalled < kStallMilliseconds) {
                dumped = false;
            } else if (!dumped) {
                log::Info("No present for {} ms; dumping thread stacks", stalled);
                DumpThreads();
                dumped = true;
            }
        }
    }).detach();
}

void NotePresent()
{
    g_lastPresent.store(GetTickCount64(), std::memory_order_relaxed);
}

}
