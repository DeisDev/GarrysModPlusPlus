#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gmodpp {

// Compiles HLSL to DXIL with the DirectX Shader Compiler (dxcompiler.dll, loaded from `directory`). Safe to use from
// several threads; each call creates its own compiler.
class ShaderCompiler {
public:
    bool Load(const std::filesystem::path& directory, std::string& error);

    // `profile` is a target such as "vs_6_6". Returns false with the compiler's messages in `error`.
    bool Compile(std::string_view source, const wchar_t* profile, std::vector<unsigned char>& dxil, std::string& error) const;

private:
    void* m_createInstance = nullptr;
};

}
