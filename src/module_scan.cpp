#include "module_scan.h"

#include "log.h"

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace gmodpp {

namespace {

struct Section {
    std::span<const std::byte> bytes;
    bool executable;
};

std::vector<Section> Sections(HMODULE module)
{
    auto* base = reinterpret_cast<const std::byte*>(module);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    std::vector<Section> sections;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        sections.push_back({std::span<const std::byte>(base + section->VirtualAddress, section->Misc.VirtualSize),
            (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    }
    return sections;
}

const std::byte* FindString(const std::vector<Section>& sections, std::string_view text)
{
    for (const Section& section : sections) {
        if (section.executable || section.bytes.size() <= text.size())
            continue;
        const std::byte* data = section.bytes.data();
        const size_t last = section.bytes.size() - text.size() - 1;
        for (size_t i = 0; i <= last; ++i) {
            if (std::memcmp(data + i, text.data(), text.size()) == 0 && data[i + text.size()] == std::byte{0} &&
                (i == 0 || data[i - 1] == std::byte{0}))
                return data + i;
        }
    }
    return nullptr;
}

DWORD64 PrimaryFunctionStart(DWORD64 address)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(address, &imageBase, nullptr);
    if (function == nullptr)
        return 0;
    for (;;) {
        auto* unwind = reinterpret_cast<const BYTE*>(imageBase + function->UnwindData);
        const BYTE flags = unwind[0] >> 3;
        if ((flags & UNW_FLAG_CHAININFO) == 0)
            return imageBase + function->BeginAddress;
        const BYTE codeCount = unwind[2];
        function = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<BYTE*>(unwind + 4 + ((codeCount + 1) & ~1) * 2));
    }
}

}

std::vector<const std::byte*> FindStringReferences(HMODULE module, std::string_view text)
{
    std::vector<Section> sections = Sections(module);
    const std::byte* target = FindString(sections, text);
    if (target == nullptr) {
        log::Info("Scan: string '{}' not found", text);
        return {};
    }
    std::vector<const std::byte*> references;
    for (const Section& section : sections) {
        if (!section.executable || section.bytes.size() < 7)
            continue;
        const auto* code = reinterpret_cast<const uint8_t*>(section.bytes.data());
        for (size_t i = 0; i + 7 <= section.bytes.size(); ++i) {
            if ((code[i] != 0x48 && code[i] != 0x4C) || code[i + 1] != 0x8D || (code[i + 2] & 0xC7) != 0x05)
                continue;
            int32_t displacement;
            std::memcpy(&displacement, code + i + 3, sizeof(displacement));
            if (reinterpret_cast<const std::byte*>(code + i + 7) + displacement == target)
                references.push_back(section.bytes.data() + i);
        }
    }
    return references;
}

void* FindFunctionReferencingString(HMODULE module, std::string_view text)
{
    DWORD64 found = 0;
    for (const std::byte* reference : FindStringReferences(module, text)) {
        DWORD64 start = PrimaryFunctionStart(reinterpret_cast<DWORD64>(reference));
        if (start == 0)
            continue;
        if (found != 0 && found != start) {
            log::Info("Scan: string '{}' is referenced by more than one function", text);
            return nullptr;
        }
        found = start;
    }
    if (found == 0)
        log::Info("Scan: no code references '{}'", text);
    return reinterpret_cast<void*>(found);
}

std::vector<const std::byte*> FindCodePattern(HMODULE module, std::string_view pattern)
{
    std::vector<int> bytes;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') {
            ++i;
            continue;
        }
        const std::string_view token = pattern.substr(i, 2);
        bytes.push_back(token == "??" ? -1 : std::stoi(std::string(token), nullptr, 16));
        i += 2;
    }
    std::vector<const std::byte*> matches;
    for (const Section& section : Sections(module)) {
        if (!section.executable || section.bytes.size() < bytes.size())
            continue;
        const auto* code = reinterpret_cast<const uint8_t*>(section.bytes.data());
        for (size_t i = 0; i + bytes.size() <= section.bytes.size(); ++i) {
            size_t k = 0;
            while (k < bytes.size() && (bytes[k] < 0 || code[i + k] == bytes[k]))
                ++k;
            if (k == bytes.size())
                matches.push_back(section.bytes.data() + i);
        }
    }
    return matches;
}

void* FindFunctionWithPattern(HMODULE module, std::string_view pattern)
{
    const std::vector<const std::byte*> matches = FindCodePattern(module, pattern);
    if (matches.size() != 1) {
        log::Info("Scan: pattern '{}' matched {} time(s)", pattern, matches.size());
        return nullptr;
    }
    return reinterpret_cast<void*>(PrimaryFunctionStart(reinterpret_cast<DWORD64>(matches.front())));
}

// MSVC's x64 RTTI (rttidata.h, ehdata_forceinclude.h in MSVC 14.51): a TypeDescriptor is { vftable, spare, name },
// and a complete object locator { signature = COL_SIG_REV1, offset, cdOffset, then image-relative pTypeDescriptor,
// pClassDescriptor, pSelf } is referenced by the slot before each vtable of the class.
const void* const* FindVtable(HMODULE module, std::string_view decoratedName, DWORD offset)
{
    struct Locator {
        DWORD signature, offset, cdOffset;
        DWORD typeDescriptor, classDescriptor, self;
    };
    constexpr DWORD kLocatorSignature = 1;
    const auto base = reinterpret_cast<uintptr_t>(module);
    std::vector<Section> sections = Sections(module);
    const std::byte* name = FindString(sections, decoratedName);
    if (name == nullptr)
        return nullptr;
    const auto typeDescriptor = static_cast<DWORD>(reinterpret_cast<uintptr_t>(name) - 2 * sizeof(void*) - base);

    const Locator* locator = nullptr;
    for (const Section& section : sections) {
        if (section.executable)
            continue;
        for (size_t i = 0; i + sizeof(Locator) <= section.bytes.size(); i += 4) {
            Locator candidate;
            std::memcpy(&candidate, section.bytes.data() + i, sizeof(candidate));
            if (candidate.signature == kLocatorSignature && candidate.offset == offset && candidate.typeDescriptor == typeDescriptor &&
                candidate.self == reinterpret_cast<uintptr_t>(section.bytes.data() + i) - base)
                locator = reinterpret_cast<const Locator*>(section.bytes.data() + i);
        }
    }
    if (locator == nullptr)
        return nullptr;

    const void* const* vtable = nullptr;
    for (const Section& section : sections) {
        if (section.executable)
            continue;
        for (size_t i = 0; i + 2 * sizeof(void*) <= section.bytes.size(); i += sizeof(void*)) {
            const void* entry;
            std::memcpy(&entry, section.bytes.data() + i, sizeof(entry));
            if (entry != locator)
                continue;
            if (vtable != nullptr)
                return nullptr;
            vtable = reinterpret_cast<const void* const*>(section.bytes.data() + i + sizeof(void*));
        }
    }
    return vtable;
}

}
