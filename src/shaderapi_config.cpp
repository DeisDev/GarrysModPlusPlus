#include "shaderapi.h"
#include "shaderapi_device.h"
#include "shaderapi_native.h"

// IMaterialSystemHardwareConfig answers as GMod's shaderapidx9.dll does on the reference machine (an RTX GPU at DX
// level 95), from the shim's caps dump (gmodpp_caps.request), so materials pick the same shader combos as vanilla.

namespace gmodpp::native {

namespace {

constexpr size_t kHardwareConfigSlots = 78;
constexpr unsigned kHwSpecificShaderDllNameSlot = 73;
constexpr unsigned kRegisterCountSlots[] = { 74, 75, 76, 77 };
constexpr int kBoolAndIntRegisters = 16;

struct Config : NativeObject {
    bool hdrEnabled = true;
};

template <auto Value>
auto Constant(NativeObject*)
{
    return Value;
}

const char* ShaderDllName(NativeObject*) { return "DEFAULT"; }
const char* NoName(NativeObject*) { return nullptr; }
bool SupportsHdrMode(NativeObject*, int) { return true; }
bool GetHdrEnabled(Config* self) { return self->hdrEnabled; }
// 0x1800049a0: the caps' HDR type (integer, from vanilla's dump) while HDR is enabled at DX level 90 or above.
int GetHdrType(Config* self) { return self->hdrEnabled ? kHdrInteger : kHdrNone; }
void SetHdrEnabled(Config* self, bool enable) { self->hdrEnabled = enable; }
void OverrideStreamOffsetSupport(NativeObject*, bool, bool) {}
bool IsAAEnabled(NativeObject*) { return GetBackend().Samples() > 1; }

NativeClass& ConfigClass()
{
    static NativeClass nativeClass = [] {
        NativeClass c("IMaterialSystemHardwareConfig", { shim::kHardwareConfigSlots, shim::kHardwareConfigSlotsCount }, kHardwareConfigSlots);
        c.Bind("HasDestAlphaBuffer", &Constant<true>);
        c.Bind("HasStencilBuffer", &Constant<true>);
        c.Bind("GetFrameBufferColorDepth", &Constant<4>);
        c.Bind("GetSamplerCount", &Constant<16>);
        c.Bind("HasSetDeviceGammaRamp", &Constant<false>);
        c.Bind("SupportsCompressedTextures", &Constant<true>);
        c.Bind("SupportsCompressedVertices", &Constant<1>);
        c.Bind("SupportsNormalMapCompression", &Constant<false>);
        c.Bind("SupportsVertexAndPixelShaders", &Constant<true>);
        c.Bind("SupportsPixelShaders_1_4", &Constant<true>);
        c.Bind("SupportsStaticControlFlow", &Constant<true>);
        c.Bind("SupportsPixelShaders_2_0", &Constant<true>);
        c.Bind("SupportsVertexShaders_2_0", &Constant<true>);
        c.Bind("MaximumAnisotropicLevel", &Constant<kMaxAnisotropy>);
        c.Bind("MaxTextureWidth", &Constant<16384>);
        c.Bind("MaxTextureHeight", &Constant<16384>);
        c.Bind("TextureMemorySize", &Constant<536870912>);
        c.Bind("SupportsOverbright", &Constant<true>);
        c.Bind("SupportsCubeMaps", &Constant<true>);
        c.Bind("SupportsMipmappedCubemaps", &Constant<true>);
        c.Bind("SupportsNonPow2Textures", &Constant<true>);
        c.Bind("GetTextureStageCount", &Constant<2>);
        c.Bind("NumVertexShaderConstants", &Constant<256>);
        c.Bind("NumPixelShaderConstants", &Constant<32>);
        c.Bind("MaxNumLights", &Constant<4>);
        c.Bind("SupportsHardwareLighting", &Constant<true>);
        c.Bind("MaxBlendMatrices", &Constant<4>);
        c.Bind("MaxBlendMatrixIndices", &Constant<0>);
        c.Bind("MaxTextureAspectRatio", &Constant<16384>);
        c.Bind("MaxVertexShaderBlendMatrices", &Constant<53>);
        c.Bind("MaxUserClipPlanes", &Constant<6>);
        c.Bind("UseFastClipping", &Constant<false>);
        c.Bind("GetDXSupportLevel", &Constant<95>);
        c.Bind("GetShaderDLLName", &ShaderDllName);
        c.Bind("ReadPixelsFromFrontBuffer", &Constant<false>);
        c.Bind("PreferDynamicTextures", &Constant<true>);
        c.Bind("SupportsHDR", &Constant<false>);
        c.Bind("HasProjectedBumpEnv", &Constant<true>);
        c.Bind("SupportsSpheremapping", &Constant<true>);
        c.Bind("NeedsAAClamp", &Constant<false>);
        c.Bind("NeedsATICentroidHack", &Constant<true>);
        c.Bind("SupportsColorOnSecondStream", &Constant<true>);
        c.Bind("SupportsStaticPlusDynamicLighting", &Constant<true>);
        c.Bind("PreferReducedFillrate", &Constant<false>);
        c.Bind("GetMaxDXSupportLevel", &Constant<95>);
        c.Bind("SpecifiesFogColorInLinearSpace", &Constant<true>);
        c.Bind("SupportsSRGB", &Constant<true>);
        c.Bind("FakeSRGBWrite", &Constant<false>);
        c.Bind("CanDoSRGBReadFromRTs", &Constant<true>);
        c.Bind("SupportsGLMixedSizeTargets", &Constant<false>);
        c.Bind("IsAAEnabled", &IsAAEnabled);
        c.Bind("GetVertexTextureCount", &Constant<4>);
        c.Bind("GetMaxVertexTextureDimension", &Constant<4096>);
        c.Bind("MaxTextureDepth", &Constant<2048>);
        c.Bind("GetHDRType", &GetHdrType);
        c.Bind("GetHardwareHDRType", &Constant<kHdrInteger>);
        c.Bind("SupportsPixelShaders_2_b", &Constant<true>);
        c.Bind("SupportsStreamOffset", &Constant<true>);
        c.Bind("StencilBufferBits", &Constant<8>);
        c.Bind("MaxViewports", &Constant<1>);
        c.Bind("OverrideStreamOffsetSupport", &OverrideStreamOffsetSupport);
        c.Bind("GetShadowFilterMode", &Constant<0>);
        c.Bind("NeedsShaderSRGBConversion", &Constant<0>);
        c.Bind("UsesSRGBCorrectBlending", &Constant<true>);
        c.Bind("SupportsShaderModel_3_0", &Constant<true>);
        c.Bind("HasFastVertexTextures", &Constant<false>);
        c.Bind("MaxHWMorphBatchCount", &Constant<4>);
        c.Bind("ActuallySupportsPixelShaders_2_b", &Constant<true>);
        c.Bind("SupportsHDRMode", &SupportsHdrMode);
        c.Bind("GetHDREnabled", &GetHdrEnabled);
        c.Bind("SetHDREnabled", &SetHdrEnabled);
        c.Bind("SupportsBorderColor", &Constant<false>);
        c.Bind("SupportsFetch4", &Constant<false>);
        c.BindSlot(kHwSpecificShaderDllNameSlot, &NoName);
        for (unsigned slot : kRegisterCountSlots)
            c.BindSlot(slot, &Constant<kBoolAndIntRegisters>);
        return c;
    }();
    return nativeClass;
}

}

NativeObject* HardwareConfig()
{
    static Config object{ { ConfigClass().Vtable(), &ConfigClass() } };
    return &object;
}

int HdrType() { return GetHdrType(static_cast<Config*>(HardwareConfig())); }

}
