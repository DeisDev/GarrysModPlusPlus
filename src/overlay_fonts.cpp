#include "overlay_fonts.h"

#include "log.h"

#include <windows.h>

#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace gmodpp::overlay::fonts {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kForeignFallback[] = L"Tahoma";
constexpr wchar_t kSystemLinkKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink";

struct Candidate {
    Face face;
    ComPtr<IDWriteFontFace> fontFace;
};

std::optional<Face> Locate(IDWriteFontFace* face)
{
    UINT32 count = 1;
    ComPtr<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&count, file.GetAddressOf())) || file == nullptr)
        return std::nullopt;
    const void* key = nullptr;
    UINT32 keySize = 0;
    ComPtr<IDWriteFontFileLoader> loader;
    ComPtr<IDWriteLocalFontFileLoader> local;
    if (FAILED(file->GetReferenceKey(&key, &keySize)) || FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&local)))
        return std::nullopt;
    UINT32 length = 0;
    if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &length)))
        return std::nullopt;
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, path.data(), length + 1)))
        return std::nullopt;
    path.resize(length);
    return Face{path, face->GetIndex()};
}

ComPtr<IDWriteFontFace> FamilyFace(IDWriteFontCollection* collection, const std::wstring& family, DWRITE_FONT_WEIGHT weight)
{
    UINT32 index = 0;
    BOOL exists = FALSE;
    ComPtr<IDWriteFontFamily> fontFamily;
    ComPtr<IDWriteFont> font;
    ComPtr<IDWriteFontFace> face;
    if (FAILED(collection->FindFamilyName(family.c_str(), &index, &exists)) || !exists || FAILED(collection->GetFontFamily(index, &fontFamily)) ||
        FAILED(fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) || FAILED(font->CreateFontFace(&face)))
        return nullptr;
    return face;
}

ComPtr<IDWriteFontFace> FileFace(IDWriteFactory* factory, const std::filesystem::path& path)
{
    ComPtr<IDWriteFontFile> file;
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 faces = 0;
    if (FAILED(factory->CreateFontFileReference(path.c_str(), nullptr, &file)) || FAILED(file->Analyze(&supported, &fileType, &faceType, &faces)) || !supported)
        return nullptr;
    IDWriteFontFile* files[] = {file.Get()};
    ComPtr<IDWriteFontFace> face;
    if (FAILED(factory->CreateFontFace(faceType, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE, &face)))
        return nullptr;
    return face;
}

// Tahoma's linked fonts in GDI's order, as "FILE,Face name[,scale,scale]" entries. Entries without a face name name a
// file in the Windows fonts folder. The scaling factors are not applied.
std::vector<std::pair<std::wstring, std::wstring>> TahomaLinks()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSystemLinkKey, kForeignFallback, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return {};
    std::wstring data(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSystemLinkKey, kForeignFallback, RRF_RT_REG_MULTI_SZ, nullptr, data.data(), &size) != ERROR_SUCCESS)
        return {};
    std::vector<std::pair<std::wstring, std::wstring>> links;
    for (const wchar_t* entry = data.c_str(); *entry != L'\0'; entry += wcslen(entry) + 1) {
        const std::wstring_view text(entry);
        const size_t comma = text.find(L',');
        const std::wstring_view face = comma == std::wstring_view::npos ? std::wstring_view() : text.substr(comma + 1);
        links.emplace_back(std::wstring(text.substr(0, comma)), std::wstring(face.substr(0, face.find(L','))));
    }
    return links;
}

std::string Utf8Name(const std::filesystem::path& path)
{
    const std::u8string name = path.filename().u8string();
    return std::string(name.begin(), name.end());
}

std::filesystem::path WindowsFontsDirectory()
{
    wchar_t windows[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::filesystem::path(windows) / L"Fonts" : std::filesystem::path();
}

std::vector<Candidate> Candidates(IDWriteFactory* factory, const std::filesystem::path& gameFont, DWRITE_FONT_WEIGHT weight)
{
    std::vector<Candidate> candidates;
    auto add = [&](ComPtr<IDWriteFontFace> face) {
        if (face == nullptr)
            return;
        const std::optional<Face> location = Locate(face.Get());
        if (!location)
            return;
        const bool known = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
            std::error_code error;
            return candidate.face.index == location->index && std::filesystem::equivalent(candidate.face.path, location->path, error);
        });
        if (!known)
            candidates.push_back({*location, std::move(face)});
    };
    if (ComPtr<IDWriteFontFace> face = FileFace(factory, gameFont))
        candidates.push_back({{gameFont, 0}, std::move(face)});
    else
        log::Info("Overlay: font {} could not be read; Tahoma draws its text, as in the game", gameFont.string());

    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(&collection)))
        return candidates;
    add(FamilyFace(collection.Get(), kForeignFallback, weight));
    for (const auto& [file, family] : TahomaLinks())
        add(!family.empty() ? FamilyFace(collection.Get(), family, weight) : FileFace(factory, WindowsFontsDirectory() / file));
    return candidates;
}

}

std::vector<Face> GameFontChain(const std::filesystem::path& path, uint32_t weight, std::u16string_view characters)
{
    ComPtr<IDWriteFactory> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) {
        log::Info("Overlay: DirectWrite is unavailable; {} draws only its own characters", path.string());
        return {{path, 0}};
    }
    std::vector<UINT32> remaining;
    for (char16_t c : characters) {
        if (c >= 0x20)
            remaining.push_back(c);
    }
    std::sort(remaining.begin(), remaining.end());
    remaining.erase(std::unique(remaining.begin(), remaining.end()), remaining.end());

    std::vector<Face> chain;
    std::string names;
    for (const Candidate& candidate : Candidates(factory.Get(), path, static_cast<DWRITE_FONT_WEIGHT>(weight))) {
        if (remaining.empty() && !chain.empty())
            break;
        std::vector<UINT16> glyphs(remaining.size());
        if (!remaining.empty() && FAILED(candidate.fontFace->GetGlyphIndices(remaining.data(), static_cast<UINT32>(remaining.size()), glyphs.data())))
            continue;
        std::vector<UINT32> missing;
        for (size_t i = 0; i < remaining.size(); ++i) {
            if (glyphs[i] == 0)
                missing.push_back(remaining[i]);
        }
        if (missing.size() == remaining.size() && !chain.empty())
            continue;
        chain.push_back(candidate.face);
        names += (names.empty() ? "" : ", ") + Utf8Name(candidate.face.path);
        remaining = std::move(missing);
    }
    log::Info("Overlay: {} draws with {}", Utf8Name(path), names);
    if (!remaining.empty())
        log::Info("Overlay: no font has {} of the menu's characters (first U+{:04X})", remaining.size(), remaining.front());
    return chain;
}

}
