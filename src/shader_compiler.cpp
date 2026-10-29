#include "shader_compiler.h"

#include <windows.h>
#include <wrl/client.h>

#include <dxcapi.h>

#include <format>

using Microsoft::WRL::ComPtr;

namespace gmodpp {

// Since DXC 1.8.2505 the compiler validates and signs with its internal validator, so dxil.dll is not needed.
bool ShaderCompiler::Load(const std::filesystem::path& directory, std::string& error)
{
    const std::filesystem::path path = directory / L"dxcompiler.dll";
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        error = std::format("cannot load {}: {}", path.string(), GetLastError());
        return false;
    }
    m_createInstance = reinterpret_cast<void*>(GetProcAddress(module, "DxcCreateInstance"));
    if (m_createInstance == nullptr) {
        error = std::format("{} has no DxcCreateInstance", path.string());
        return false;
    }
    return true;
}

bool ShaderCompiler::Compile(std::string_view source, const wchar_t* profile, std::vector<unsigned char>& dxil, std::string& error) const
{
    auto create = reinterpret_cast<DxcCreateInstanceProc>(m_createInstance);
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr = create != nullptr ? create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)) : E_FAIL;
    if (FAILED(hr)) {
        error = std::format("DxcCreateInstance failed: {:#x}", static_cast<unsigned>(hr));
        return false;
    }
    const DxcBuffer buffer{ source.data(), source.size(), DXC_CP_UTF8 };
    LPCWSTR arguments[] = { L"-E", L"main", L"-T", profile, L"-HV", L"2021", DXC_ARG_OPTIMIZATION_LEVEL3 };
    ComPtr<IDxcResult> result;
    hr = compiler->Compile(&buffer, arguments, static_cast<UINT32>(std::size(arguments)), nullptr, IID_PPV_ARGS(&result));
    HRESULT status = hr;
    if (SUCCEEDED(hr))
        result->GetStatus(&status);
    if (FAILED(status)) {
        ComPtr<IDxcBlobUtf8> messages;
        if (result != nullptr)
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr);
        error = messages != nullptr && messages->GetStringLength() > 0 ? std::string(messages->GetStringPointer(), messages->GetStringLength())
                                                                       : std::format("compilation failed: {:#x}", static_cast<unsigned>(status));
        return false;
    }
    ComPtr<IDxcBlob> object;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (object == nullptr) {
        error = "the compiler returned no object";
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(object->GetBufferPointer());
    dxil.assign(bytes, bytes + object->GetBufferSize());
    return true;
}

}
