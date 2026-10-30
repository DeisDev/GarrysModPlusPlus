#include "watchdog.h"

#include "log.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

std::string FunctionName(DWORD64 address)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    std::string module = Describe(address);
    module = module.substr(0, module.find('+'));
    if (SymFromAddr(GetCurrentProcess(), address, nullptr, symbol))
        return module + '!' + symbol->Name;
    return module;
}

void LogTop(const char* title, const std::map<std::string, int>& counts, size_t samples)
{
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [name, count] : counts)
        sorted.emplace_back(count, name);
    std::sort(sorted.begin(), sorted.end(), std::greater<>());
    std::string text;
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 30); ++i)
        text += std::format("\n    {:5.1f}% {}", 100.0 * sorted[i].first / static_cast<double>(samples), sorted[i].second);
    log::Info("Profile, {} of {} samples:{}", title, samples, text);
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

void ProfileThisThread(int seconds)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (thread == nullptr)
        return;
    log::Info("Profile: sampling thread {} for {} s", GetCurrentThreadId(), seconds);
    std::thread([thread, seconds] {
        std::vector<std::vector<DWORD64>> samples;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end) {
            std::array<DWORD64, kMaxFrames> frames;
            size_t count = 0;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT context = {};
                context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(thread, &context))
                    count = Unwind(context, frames);
                ResumeThread(thread);
            }
            if (count != 0)
                samples.emplace_back(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(count));
            Sleep(1);
        }
        CloseHandle(thread);
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        std::unordered_map<DWORD64, std::string> names;
        auto name = [&](DWORD64 address) -> const std::string& {
            auto found = names.find(address);
            return found != names.end() ? found->second : names.emplace(address, FunctionName(address)).first->second;
        };
        std::map<std::string, int> self;
        std::map<std::string, int> inclusive;
        for (const std::vector<DWORD64>& stack : samples) {
            ++self[name(stack[0])];
            std::set<std::string> seen;
            for (DWORD64 address : stack) {
                if (seen.insert(name(address)).second)
                    ++inclusive[name(address)];
            }
        }
        SymCleanup(GetCurrentProcess());
        LogTop("self", self, samples.size());
        LogTop("with callees", inclusive, samples.size());
    }).detach();
}

}
