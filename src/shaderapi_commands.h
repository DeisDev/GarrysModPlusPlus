#pragma once

namespace gmodpp {

// The command buffers materialsystem passes to IShaderAPI::ExecuteCommandBuffer: a stream of 32-bit opcodes, each
// followed by its arguments. From public/shaderapi/commandbuffer.h (Source SDK 2013), checked against GMod's
// shaderapidx9.dll (ExecuteCommandBuffer at 0x180015980): it implements only the opcodes below, and 2004 is GMod's.
// Pointers are 8 bytes and unaligned, at offset 4.
enum CommandBufferOpcode : int {
    kCommandEnd = 0,
    kCommandJump = 1,                           // void* target
    kCommandJsr = 2,                            // void* subroutine, run by a nested ExecuteCommandBuffer
    kCommandSetPixelShaderFloatConst = 256,     // int first, int count, float values[count * 4]
    kCommandSetVertexShaderFloatConst = 257,    // int first, int count, float values[count * 4]
    kCommandSetPixelShaderFogParams = 259,      // int register
    kCommandStoreEyePosInPixelShaderConst = 260, // int register
    kCommandCommitPixelShaderLighting = 261,    // int register
    kCommandSetPixelShaderAmbientLightCube = 262, // int register
    kCommandSetVertexShaderAmbientLightCube = 263,
    kCommandSetDepthFeatheringConst = 264,      // int register, float blend scale
    kCommandBindStandardTexture = 512,          // int sampler, int StandardTextureId_t
    kCommandBindShaderApiTextureHandle = 513,   // int sampler, ShaderAPITextureHandle_t (8 bytes)
    kCommandSetPixelShaderIndex = 1024,         // int index
    kCommandSetVertexShaderIndex = 1025,        // int index
    // Mode 1 sets the sampler's min, mag and mip filters to point, mode 2 only its mag filter; others do nothing.
    kCommandSetSamplerPointFilter = 2004,       // int sampler, int mode
};

}
