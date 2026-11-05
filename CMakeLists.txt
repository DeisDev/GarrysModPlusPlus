cmake_minimum_required(VERSION 3.25)

# The mod's version (Semantic Versioning 2.0.0), set here only: src/gmodpp_version.h.in takes it to the code, the
# version resources of both DLLs and the log.
set(GMODPP_VERSION 0.9.0-alpha)
if(NOT GMODPP_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "GMODPP_VERSION '${GMODPP_VERSION}' is not a semantic version")
endif()
set(GMODPP_VERSION_MAJOR ${CMAKE_MATCH_1})
set(GMODPP_VERSION_MINOR ${CMAKE_MATCH_2})
set(GMODPP_VERSION_PATCH ${CMAKE_MATCH_3})
if(CMAKE_MATCH_4)
    set(GMODPP_VERSION_PRERELEASE 1)
else()
    set(GMODPP_VERSION_PRERELEASE 0)
endif()
project(GarrysModPP VERSION ${GMODPP_VERSION_MAJOR}.${GMODPP_VERSION_MINOR}.${GMODPP_VERSION_PATCH} LANGUAGES C CXX ASM_MASM)
configure_file(src/gmodpp_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/gmodpp_version.h" @ONLY)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "GarrysMod++ targets the 64-bit branch of Garry's Mod; configure with an x64 toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GMOD_DIR "" CACHE PATH "Garry's Mod install directory; enables the deploy target")

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
)
FetchContent_MakeAvailable(minhook imgui)

# NVIDIA NRD 4.18.0 (the denoiser without DLSS Ray Reconstruction), built as a static library whose DXIL shaders it embeds,
# compiled by its ShaderMake with the DXC below. Its dependencies are declared first, pinned by hash, so NRD's own
# declarations of them (by the same URLs) are not used. MathLib (MIT) is compiled into NRD and its ShaderMakeBlob (MIT)
# linked into it.
FetchContent_Declare(shadermake
    URL https://github.com/NVIDIA-RTX/ShaderMake/archive/3f01623a76b092ac127d2debd79a672f55d77665.zip
    URL_HASH SHA256=becdd4593f09f42b191c3a9579569d6497869ad624fdb0275f7708508bfcadd7
    DOWNLOAD_EXTRACT_TIMESTAMP 1
)
FetchContent_Declare(mathlib
    URL https://github.com/NVIDIA-RTX/MathLib/archive/refs/tags/v11.zip
    URL_HASH SHA256=4a0772103fd7ef832f8c2fb3ec7fb6bf5b3bebbd41ecafc476671d11eca28a06
    DOWNLOAD_EXTRACT_TIMESTAMP 1
)
FetchContent_Declare(nrd
    URL https://github.com/NVIDIA-RTX/NRD/archive/refs/tags/v4.18.0.zip
    URL_HASH SHA256=d9cb9476c11a8773bfe32d99d26e7038bac0ef3e09957f63e35c3b0fe9f29790
    DOWNLOAD_EXTRACT_TIMESTAMP 1
)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)

# DirectX Shader Compiler with Shader Model 6.9 (opacity micromaps in inline ray queries), pinned by hash.
set(DXC_VERSION v1.9.2602)
set(DXC_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/dxc-${DXC_VERSION}")
if(NOT EXISTS "${DXC_DIR}/bin/x64/dxc.exe")
    file(DOWNLOAD "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_VERSION}/dxc_2026_02_20.zip" "${DXC_DIR}.zip"
        EXPECTED_HASH SHA256=a1e89031421cf3c1fca6627766ab3020ca4f962ac7e2caa7fab2b33a8436151e TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${DXC_DIR}.zip" DESTINATION "${DXC_DIR}")
endif()
set(DXC_EXECUTABLE "${DXC_DIR}/bin/x64/dxc.exe")

set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(NRD_NRI OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXIL_SHADERS ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_COMPILERS OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_DXC_PATH "${DXC_EXECUTABLE}" CACHE INTERNAL "")
FetchContent_MakeAvailable(nrd)
# NRD made its dependencies available in its own scope; their source directories are needed here for their licences.
FetchContent_GetProperties(mathlib SOURCE_DIR MATHLIB_SOURCE_DIR)
FetchContent_GetProperties(shadermake SOURCE_DIR SHADERMAKE_SOURCE_DIR)
find_program(FXC_EXECUTABLE fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)

# DirectX 12 Agility SDK: headers for opacity micromaps and the D3D12Core.dll runtime the device is created
# from (renderer.cpp), pinned by hash.
set(AGILITY_SDK_VERSION 1.619.6)
set(AGILITY_SDK_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/agility-${AGILITY_SDK_VERSION}")
if(NOT EXISTS "${AGILITY_SDK_DIR}/build/native/include/d3d12.h")
    file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${AGILITY_SDK_VERSION}/microsoft.direct3d.d3d12.${AGILITY_SDK_VERSION}.nupkg"
        "${AGILITY_SDK_DIR}.zip" EXPECTED_HASH SHA256=08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9 TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${AGILITY_SDK_DIR}.zip" DESTINATION "${AGILITY_SDK_DIR}")
endif()
set(AGILITY_RUNTIME_DIR "${AGILITY_SDK_DIR}/build/native/bin/x64")
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
set(SHADER_HEADERS)

# NVIDIA SHaRC 1.8.3 (spatially hashed radiance cache), pinned by commit and each file by hash. Its headers are compiled
# into the lighting passes that use the radiance cache (shaders/nvidia) and never shipped as source.
set(SHARC_COMMIT 4e21b585c33c83d723ca9a1e11bbb1090d145793)
set(SHARC_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/sharc-${SHARC_COMMIT}")
set(SHARC_FILES
    "include/HashGridCommon.h|74585a8af1eee32c90c9f56ef0db5d50ca8929ac4d1fbc093ea8664957b02334"
    "include/HashGridTypes.h|f5a04ff7f7a511cb8ccd7150fdf39e689f10afa4c0c355982ef52f6859e9340f"
    "include/SharcCommon.h|097e5238a3c3a3360d1e8685b56189a75b5c663dbdcc74383d5384c61b936a76"
    "include/SharcTypes.h|cde3f200e4e84f029968dffd9eede5228f2ae0b0bbc166d2babb1aa88dc044b3"
    "License.md|5575ad921bfe6027dc830887ff51ceb4097e31c2d538b6932cd4bbdf5c1899a9"
)
set(SHARC_PATHS "")
foreach(entry IN LISTS SHARC_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/NVIDIA-RTX/SHARC/${SHARC_COMMIT}/${file}" "${SHARC_DIR}/${file}"
        EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
    list(APPEND SHARC_PATHS "${SHARC_DIR}/${file}")
endforeach()
# NVIDIA RTXDI-Library (RTXDI 3.1.0) for many-light direct lighting (ReSTIR DI with a ReGIR light grid), pinned by commit
# and each file by hash: the DI, ReGIR, light sampling and utility headers the lighting passes include (shaders/nvidia), and
# the host sources of the ReSTIR DI and ReGIR contexts compiled into gmodpp.dll. Never shipped as source.
set(RTXDI_COMMIT f12037fa8e97ebc08e9e3edfd2de528ed1772a4b)
set(RTXDI_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/rtxdi-${RTXDI_COMMIT}")
set(RTXDI_FILES
    "Include/Rtxdi/RtxdiParameters.h|70947be081ce6456bc724f18f3c06a6b88cad90d336bda470c4794416d6cfe30"
    "Include/Rtxdi/RtxdiTypes.h|3e0b0593cf23503f3d7c5fdc449a5ecb0f686e334b394ca328130b4fd1af6336"
    "Include/Rtxdi/RtxdiUtils.h|9e77e4aecc7f575fe166ffa068aedfe4c5a3d8788b11bc09bd3305c061584fd5"
    "Include/Rtxdi/DI/ReSTIRDI.h|ff8572e7852e7e3a271641721be101adbcdec50539282481b7f8561487038256"
    "Include/Rtxdi/DI/ReSTIRDIParameters.h|39fc383642be6b3c0f90c376b8be261b1d5eccf361efc002e684bfc28c6e4e39"
    "Include/Rtxdi/DI/BoilingFilter.hlsli|28530097ce3ea47156d345152e14e4c1f9661d210f0e719af7694dee3fdb6806"
    "Include/Rtxdi/DI/InitialSampling.hlsli|53f27901d296bac2a1d5fb9c4f73b643a4c39c632c970407ab595e6ddaf098ec"
    "Include/Rtxdi/DI/PairwiseStreaming.hlsli|fd15e9051036a211a3f516a186edef0f36cf99f9392bd840121b0dbe449fac8c"
    "Include/Rtxdi/DI/Reservoir.hlsli|be474731482b1edb0669f19a1c07f4abe23d8ceee895948e933f685566a0111d"
    "Include/Rtxdi/DI/ReservoirStorage.hlsli|564bdaf5e82dc214f9f90a055e388eae33280347d5aed531cecdc3ab47afa139"
    "Include/Rtxdi/DI/SpatialResampling.hlsli|7ece19806068de3e12a088544af90a2597bf3444b50b7d1331b5e2a61cda55f5"
    "Include/Rtxdi/DI/SpatioTemporalResampling.hlsli|f1d6c303aa9500db46426d93b80dd0ea5868e190671727649de2ce9c357adb6e"
    "Include/Rtxdi/DI/TemporalResampling.hlsli|ef2394f6959b24fb2098a41aae2c14284708c410f17ea071eb3216ea4ec90d59"
    "Include/Rtxdi/LightSampling/LocalLightSelection.hlsli|7376cf1cea74ea9710c7da350db193d8edfcdd0b49d16f80020e0fa06c9da92c"
    "Include/Rtxdi/LightSampling/PresamplingFunctions.hlsli|cec1bc2f34f39563d4fe04e88ecf89afaf2dfb49f80e91dc2ac6fb716b27a6c5"
    "Include/Rtxdi/LightSampling/RISBuffer.hlsli|23b7dd3d870ce210fc51925fdc3a66f6fca2960962256287631a65da28f15f1c"
    "Include/Rtxdi/LightSampling/RISBufferSegmentAllocator.h|83e2520d782efd0afc06fc87fe74c22cb17d6a5060cf141e85958dfd6a61b046"
    "Include/Rtxdi/LightSampling/RISBufferSegmentParameters.h|7c3028089f236e2dd8d37f8c9f91f65de10b2940b028aa43d448aecc82af96d2"
    "Include/Rtxdi/LightSampling/UniformSampling.hlsli|3d3779cf58a4b2e68658d56d4acc3240be5ca92f387c8a70ab553793a466675c"
    "Include/Rtxdi/ReGIR/ReGIR.h|0bed923e912f56470595bbc9d4120b91c470691b2e7fdac0314101b19dff3323"
    "Include/Rtxdi/ReGIR/ReGIRParameters.h|efa12becd55831bb8e079fd9ed2201616bcda6ada8c03c07fd0cd781be1d344e"
    "Include/Rtxdi/ReGIR/ReGIRSampling.hlsli|fba9b81a34d63d8d5b3b6fda7a5792557e3ab309ae6d98c0ab95fb8bfc153720"
    "Include/Rtxdi/Utils/BoilingFilter.hlsli|80c42405fc0b22ac8383baaf0737394b892257e7c812effce0fbd78ce10ea418"
    "Include/Rtxdi/Utils/BrdfRaySample.hlsli|4c49daad4f9549daf55bd397db89882f8eab0a089f2d60626103d001a795d516"
    "Include/Rtxdi/Utils/Checkerboard.hlsli|9d787231c7c4d32a9e7b575166fd77eeecc74759e540ca44f98149e5901865b5"
    "Include/Rtxdi/Utils/Color.hlsli|4dbc197d6b38c19becb80adf6e361d16c99684b9689a1cc4678a842d21179114"
    "Include/Rtxdi/Utils/Math.hlsli|ec4bb38fac6d922e75799449bc311b9fa2c91f79bdfa50e197e4d2c95f381954"
    "Include/Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli|c83a644b6cbd6256a523ed0a719af4d49a9ef1a7133db320339114c3645cfbd9"
    "Include/Rtxdi/Utils/RandomSamplerState.hlsli|ea29055daa730794146adb25612889b9d7fe2adb1ff940b462ae13c731e0274d"
    "Include/Rtxdi/Utils/ReservoirAddressing.hlsli|df44bf8fbc1d5bbcda24e40d4f4acc71bedab5ffa24d7f2db8c3e515cf4a86da"
    "Include/Rtxdi/Utils/SampledLightData.hlsli|8224d0ba265d5daca0df8629c91a8dd4576f848d2b9f461acaab91b7ff64cf88"
    "Include/Rtxdi/Utils/TypePacking.hlsli|e9bda8d1c534329ba19ee5d46819adac829da17869f3864c5749236f434873f0"
    "Source/ReSTIRDI.cpp|ae6e5157ee9cf752ff3200a455203753cb484bfcf93f62f0e4a6cd81f2ee7f16"
    "Source/ReGIR.cpp|1c542e1085e1b6c1a22de309eff9c4b8b7f08f337c77c4f85ac08610d4d301d6"
    "Source/RtxdiUtils.cpp|3b8613b9e2b191d031103c5be307eef2249b211788d0c1b02e0397210732a261"
    "Source/RISBufferSegmentAllocator.cpp|d17bf9c94e1e69a3810132a7983f3c6c79f770d4c28fac6aa2e1657b8bcbc1a6"
    "LICENSE.txt|120a5af845068454e8498718e230689660abd96b15f48a19858c56f0e3ecfb4c"
)
set(RTXDI_PATHS "")
foreach(entry IN LISTS RTXDI_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/NVIDIA-RTX/RTXDI-Library/${RTXDI_COMMIT}/${file}" "${RTXDI_DIR}/${file}"
        EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
    list(APPEND RTXDI_PATHS "${RTXDI_DIR}/${file}")
endforeach()
set(RTXDI_SHADER_OPTIONS -D GMODPP_RTXDI=1 -I "${RTXDI_DIR}/Include")
# AMD FidelityFX SDK 1.1.4 (MIT), its last release with Depth of Field and Lens, pinned by commit and each file by hash:
# the GPU headers of DoF, CAS and Lens that the post effects' passes include (shaders/amd).
set(FFX_COMMIT c6efa6bf7f2027b3ec94f28578bb5965eabb9e55)
set(FFX_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/fidelityfx-${FFX_COMMIT}")
set(FFX_FILES
    "LICENSE.txt|82cf74fc23885107c7f69249ac3f1dac55898ff68320eb2c753984cae6b2d78a"
    "sdk/include/FidelityFX/gpu/ffx_core.h|e439aa5f516b2d2ebf070ff1d24561f04fa5f00262a2e4f760764a7d07de7997"
    "sdk/include/FidelityFX/gpu/ffx_common_types.h|a19560f837c09f0c990e98b5925b731135d908e372af41c5895cbfa70cc9e5c8"
    "sdk/include/FidelityFX/gpu/ffx_core_hlsl.h|98722d4b1331c437f5aee645e6f12ed722b721f2ffcb0ec523ee817857f394c7"
    "sdk/include/FidelityFX/gpu/ffx_core_gpu_common.h|0e3c50cb0b4592c051d87e028a1dfa54fbd097942ebb3146964db801dec095d0"
    "sdk/include/FidelityFX/gpu/ffx_core_gpu_common_half.h|d10466cbf027c8c23858a25f8e8cbbbf521d29f373db663c9354221651e9a498"
    "sdk/include/FidelityFX/gpu/ffx_core_portability.h|a7c3a1c213ec083bec401181036cffe7a222af2c430c0bab6bd8c14e83e8a567"
    "sdk/include/FidelityFX/gpu/dof/ffx_dof_common.h|7df8bba17643097d40bcec033756ae203ae4a3c646e57cf23c171cd4115fa8e8"
    "sdk/include/FidelityFX/gpu/dof/ffx_dof_resources.h|f23eb5492808c2f2cbf94177938fcce305045788803b60266c9a92a6ac930ead"
    "sdk/include/FidelityFX/gpu/dof/ffx_dof_downsample_depth.h|73eadc0f237003757014c8621d3e40656db089a57be5ea4ba4096a353c8a1d46"
    "sdk/include/FidelityFX/gpu/dof/ffx_dof_downsample_color.h|3d38d0b638ff7a9561c417cba1a33efbfeb253faf39baf339ff5286ddd4b5061"
    "sdk/include/FidelityFX/gpu/dof/ffx_dof_dilate_depth.h|38ddd1d21ba032855f7cd0c838973451eadd8128e67e030261fcd569c547d1ae"
    "sdk/include/FidelityFX/gpu/dof/ffx_dof_blur.h|533e0846268e484f4d1f8341536e73f703f4065d03be2bf25c5fbe172c8ae408"
    "sdk/include/FidelityFX/gpu/dof/ffx_dof_composite.h|91f5148f43ef6b73253d2ab58052a23c74b004161065aa8269d95f2abb4591fb"
    "sdk/include/FidelityFX/gpu/spd/ffx_spd.h|d276f32ee18948cb053c991f3c8821f9664c93f12cb2483dd2978f333bca67b2"
    "sdk/include/FidelityFX/gpu/cas/ffx_cas.h|5d8a93b77128895f7aef4647ec1da55c44999f10603508fb7eaa6ec8f74c9067"
    "sdk/include/FidelityFX/gpu/lens/ffx_lens.h|4dc1d49808b3f1ad7379f914925aa4fd931f597d3879a827db2ac62f63a2aacd"
)
set(FFX_PATHS "")
foreach(entry IN LISTS FFX_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FFX_COMMIT}/${file}" "${FFX_DIR}/${file}"
        EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
    list(APPEND FFX_PATHS "${FFX_DIR}/${file}")
endforeach()
# AMD FSR SDK 2.4.0 (MIT), pinned by commit and each file by hash: the FSR API's headers (upscaling, Ray Regeneration,
# frame generation) and AMD's signed DLLs, which the FSR API must be used through: the loader, loaded at runtime from
# gmodplusplus, finds the effect DLLs beside it. Anti-Lag 2's header comes with it.
set(FSR_COMMIT 7539ae8b6568ad29ed78a753404351daf3b5cca7)
set(FSR_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/fsr-${FSR_COMMIT}")
set(FSR_FILES
    "Kits/FidelityFX/docs/license.md|32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb"
    "Kits/FidelityFX/api/include/ffx_api.h|4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf"
    "Kits/FidelityFX/api/include/ffx_api_types.h|53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c"
    "Kits/FidelityFX/api/include/ffx_api_loader.h|18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e"
    "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h|c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530"
    "Kits/FidelityFX/upscalers/include/ffx_upscale.h|13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d"
    "Kits/FidelityFX/denoisers/include/ffx_denoiser.h|64deddd5482a302903b06d1a319b83e66d1792f493447204a2356de2d48f3ca5"
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h|bc78a18aa95f5157b4699b0602700592b624788c5bc55d1762cb46a1b6557ca1"
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration_api_types.h|396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4"
    "Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h|8ddb1cb78f83caf3598509df4968e84c851504cd6c00d9923261a5fc2c7ef0ed"
    "Kits/OpenSource/amd/antilag2/ffx_antilag2_dx12.h|a4bea0c4e8c3d800a3cf4f1e9690bf927aa7b9b7f02ffa2bece28d9aac60b4a6"
    "Kits/OpenSource/amd/antilag2/LICENSE.txt|b516b8fe7e52d5afe96045f43266c6d2fd4c10963cb624bcf1142b615d3ad4a7"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll|e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll|aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll|e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6"
    "Kits/FidelityFX/signedbin/amd_fidelityfx_denoiser_dx12.dll|4ca5182d5c8a855441e0c82673716e4776f9afcfadfdb4225d2fc6113b0f853c"
)
foreach(entry IN LISTS FSR_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FSR_COMMIT}/${file}" "${FSR_DIR}/${file}"
        EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
endforeach()
set(FSR_DLLS "${FSR_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll" "${FSR_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll"
    "${FSR_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll" "${FSR_DIR}/Kits/FidelityFX/signedbin/amd_fidelityfx_denoiser_dx12.dll")
set(FFX_SHADER_OPTIONS -D FFX_GPU=1 -D FFX_HLSL=1 -D FFX_HLSL_SM=66 -D FFX_HALF=0 -I "${FFX_DIR}/sdk/include/FidelityFX/gpu"
    -I "${FFX_DIR}/sdk/include/FidelityFX/gpu/dof" -Wno-ambig-lit-shift)

# Lighting passes built with the radiance cache: DXC options and the files they read.
set(SHARC_SHADER_OPTIONS -D GMODPP_SHARC=1 -I "${SHARC_DIR}/include" -enable-16bit-types)
file(GLOB NVIDIA_SHADER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nvidia/*.hlsli")
file(GLOB AMD_SHADER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/amd/*.hlsli")
file(GLOB POST_SHADER_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/post/*.hlsli")
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli")

# Extra arguments are dxc options.
function(add_shader source entry profile variable)
    set(output "${SHADER_OUTPUT_DIR}/${variable}.h")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${DXC_EXECUTABLE}" -nologo -T ${profile} -E ${entry} -O3 -HV 2021 ${ARGN} -Fh "${output}" -Vn ${variable} "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" ${SHADER_INCLUDES} ${NVIDIA_SHADER_HEADERS} ${AMD_SHADER_HEADERS} ${POST_SHADER_HEADERS} ${SHARC_PATHS} ${RTXDI_PATHS}
            ${FFX_PATHS}
            "${nrd_SOURCE_DIR}/Shaders/NRD.hlsli" "${nrd_SOURCE_DIR}/Shaders/NRDConfig.hlsli"
        COMMENT "Compiling ${source} (${entry})"
        VERBATIM
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} "${output}" PARENT_SCOPE)
endfunction()

# Direct3D 9 pixel shaders that replace the game's own, compiled with fxc for ps_3_0.
# Extra arguments are fxc options; PROFILE <profile> replaces ps_3_0.
function(add_d3d9_shader source entry variable)
    cmake_parse_arguments(PARSE_ARGV 3 SHADER "" "PROFILE" "")
    if(NOT SHADER_PROFILE)
        set(SHADER_PROFILE ps_3_0)
    endif()
    set(output "${SHADER_OUTPUT_DIR}/${variable}.h")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /T ${SHADER_PROFILE} /E ${entry} /O3 ${SHADER_UNPARSED_ARGUMENTS} /Fh "${output}" /Vn ${variable} "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/forward_light.hlsli"
        COMMENT "Compiling ${source} (${entry})"
        VERBATIM
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} "${output}" PARENT_SCOPE)
endfunction()

add_shader(shaders/surface.hlsl main cs_6_6 g_surfaceCS)
# Passes that trace rays, and their Shader Model 6.9 variants that let ray queries use opacity micromaps. Every pass with a
# ray query needs one: a query without RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS that meets an opacity micromap is undefined
# behaviour (DirectX Raytracing spec, "RayQuery flags").
foreach(pass primary probes ao volumetrics direct reflections gi_trace)
    string(REPLACE "_t" "T" variable ${pass})
    add_shader(shaders/${pass}.hlsl main cs_6_6 g_${variable}CS)
    add_shader(shaders/${pass}.hlsl main cs_6_9 g_${variable}OmmCS -D OPACITY_MICROMAPS=1)
endforeach()
# The passes that read the radiance cache, built with it for devices that support it (16-bit types and 64-bit atomics),
# and the cache's own passes.
foreach(pass probes reflections gi_trace)
    string(REPLACE "_t" "T" variable ${pass})
    add_shader(shaders/${pass}.hlsl main cs_6_6 g_${variable}CacheCS ${SHARC_SHADER_OPTIONS})
    add_shader(shaders/${pass}.hlsl main cs_6_9 g_${variable}CacheOmmCS -D OPACITY_MICROMAPS=1 ${SHARC_SHADER_OPTIONS})
endforeach()
add_shader(shaders/deferred_lighting.hlsl main cs_6_6 g_deferredLightingCacheCS ${SHARC_SHADER_OPTIONS})
add_shader(shaders/deferred_lighting.hlsl main cs_6_9 g_deferredLightingCacheOmmCS -D OPACITY_MICROMAPS=1 ${SHARC_SHADER_OPTIONS})
add_shader(shaders/sharc_update.hlsl main cs_6_6 g_sharcUpdateCS ${SHARC_SHADER_OPTIONS})
add_shader(shaders/sharc_update.hlsl main cs_6_9 g_sharcUpdateOmmCS -D OPACITY_MICROMAPS=1 ${SHARC_SHADER_OPTIONS})
add_shader(shaders/sharc_resolve.hlsl main cs_6_6 g_sharcResolveCS ${SHARC_SHADER_OPTIONS})
# Many-light direct lighting (NVIDIA RTXDI): the sun alone in the direct pass, the light PDF, presampling, ReGIR, and ReSTIR
# DI's initial sampling, reuse and shading.
add_shader(shaders/direct.hlsl main cs_6_6 g_directSunCS -D SUN_ONLY=1)
add_shader(shaders/direct.hlsl main cs_6_9 g_directSunOmmCS -D SUN_ONLY=1 -D OPACITY_MICROMAPS=1)
foreach(entry Pdf Presample Regir Initial Temporal Spatial Shade)
    add_shader(shaders/restir.hlsl ${entry}Main cs_6_6 g_restir${entry}CS ${RTXDI_SHADER_OPTIONS})
endforeach()
foreach(entry Initial Shade)
    add_shader(shaders/restir.hlsl ${entry}Main cs_6_9 g_restir${entry}OmmCS -D OPACITY_MICROMAPS=1 ${RTXDI_SHADER_OPTIONS})
endforeach()
# NVIDIA NRD's inputs and outputs (its own shaders are embedded in the NRD library).
add_shader(shaders/nrd.hlsl InputsMain cs_6_6 g_nrdInputsCS -I "${nrd_SOURCE_DIR}/Shaders")
add_shader(shaders/nrd.hlsl ResolveMain cs_6_6 g_nrdResolveCS -I "${nrd_SOURCE_DIR}/Shaders")
# AMD FSR Ray Regeneration's inputs and outputs.
add_shader(shaders/ray_regeneration.hlsl InputsMain cs_6_6 g_regenInputsCS)
add_shader(shaders/ray_regeneration.hlsl ResolveMain cs_6_6 g_regenResolveCS)
# The main view's sun shadows, traced to the sun's disc for NRD's SIGMA.
add_shader(shaders/sun_shadow.hlsl main cs_6_6 g_sunShadowCS -I "${nrd_SOURCE_DIR}/Shaders")
add_shader(shaders/sun_shadow.hlsl main cs_6_9 g_sunShadowOmmCS -D OPACITY_MICROMAPS=1 -I "${nrd_SOURCE_DIR}/Shaders")
add_shader(shaders/direct.hlsl main cs_6_6 g_directWideCS -D WIDE_LIGHT_SETS=1)
add_shader(shaders/direct.hlsl main cs_6_9 g_directWideOmmCS -D WIDE_LIGHT_SETS=1 -D OPACITY_MICROMAPS=1)
add_shader(shaders/reflection_temporal.hlsl main cs_6_6 g_reflectionTemporalCS)
add_shader(shaders/baked_bounce.hlsl main cs_6_6 g_bakedBounceCS)
add_shader(shaders/baked_bounce.hlsl main cs_6_9 g_bakedBounceOmmCS -D OPACITY_MICROMAPS=1)
add_shader(shaders/sway.hlsl main cs_6_6 g_swayCS)
add_shader(shaders/skin.hlsl main cs_6_6 g_skinCS)
add_shader(shaders/dlss_inputs.hlsl main cs_6_6 g_dlssInputsCS)
add_shader(shaders/dlss_resolve.hlsl main cs_6_6 g_dlssResolveCS)
add_shader(shaders/gi_temporal.hlsl main cs_6_6 g_giTemporalCS)
add_shader(shaders/gi_filter.hlsl main cs_6_6 g_giFilterCS)
add_shader(shaders/deferred_lighting.hlsl main cs_6_6 g_deferredLightingCS)
add_shader(shaders/deferred_lighting.hlsl main cs_6_9 g_deferredLightingOmmCS -D OPACITY_MICROMAPS=1)
add_shader(shaders/taa.hlsl main cs_6_6 g_taaCS)
add_shader(shaders/taa_sharpen.hlsl main cs_6_6 g_taaSharpenCS)
add_shader(shaders/composite.hlsl VSMain vs_6_0 g_compositeVS)
add_shader(shaders/composite.hlsl PSMain ps_6_0 g_compositePS)
add_shader(shaders/composite.hlsl DepthPS ps_6_0 g_compositeDepthPS)
add_shader(shaders/composite.hlsl CoveredPS ps_6_0 g_compositeCoveredPS)
add_shader(shaders/view_rect.hlsl main cs_6_6 g_viewRectCS)
add_shader(shaders/sky.hlsl main cs_6_6 g_skyCS)
# The procedural sky: its atmosphere and colours, and the pixel shaders of the game's 2D sky draws.
add_shader(shaders/sky_atmosphere.hlsl main cs_6_6 g_skyAtmosphereCS)
add_shader(shaders/clouds.hlsl main cs_6_6 g_cloudsCS)
add_shader(shaders/particle_shadows.hlsl main cs_6_6 g_particleShadowsCS)
add_shader(shaders/sky_draw.hlsl SkyMain ps_6_6 g_skyDrawPS)
add_shader(shaders/sky_draw.hlsl DiscMain ps_6_6 g_sunDiscPS)
add_shader(shaders/translucent_depth.hlsl main cs_6_6 g_translucentDepthCS)
add_shader(shaders/translucent_depth.hlsl main cs_6_9 g_translucentDepthOmmCS -D OPACITY_MICROMAPS=1)
add_shader(shaders/caustics.hlsl main cs_6_6 g_causticsCS)
add_shader(shaders/caustics.hlsl main cs_6_9 g_causticsOmmCS -D OPACITY_MICROMAPS=1)
add_shader(shaders/flare_visibility.hlsl main cs_6_6 g_flareVisibilityCS)
add_shader(shaders/flare_visibility.hlsl main cs_6_9 g_flareVisibilityOmmCS -D OPACITY_MICROMAPS=1)
# The camera effects after the game's post-processing (src/post_effects.cpp): our passes, and AMD FidelityFX's.
add_shader(shaders/post/post.hlsl VSMain vs_6_6 g_postVS)
foreach(entry Prepare Camera TileMax NeighborMax MotionBlur FrameMotion ExposureHistogram Exposure)
    add_shader(shaders/post/post.hlsl ${entry}Main cs_6_6 g_post${entry}CS)
endforeach()
add_shader(shaders/post/bloom.hlsl DownMain cs_6_6 g_bloomDownCS)
add_shader(shaders/post/bloom.hlsl UpMain cs_6_6 g_bloomUpCS)
add_shader(shaders/post/local_exposure.hlsl GridMain cs_6_6 g_localGridCS)
add_shader(shaders/post/local_exposure.hlsl BlurGridMain cs_6_6 g_localBlurGridCS)
add_shader(shaders/post/local_exposure.hlsl BlurMeanMain cs_6_6 g_localBlurMeanCS)
foreach(entry Aperture FftRows FftColumns Starburst)
    add_shader(shaders/post/flare.hlsl ${entry}Main cs_6_6 g_flare${entry}CS)
endforeach()
add_shader(shaders/post/flare.hlsl GhostVS vs_6_6 g_flareGhostVS)
add_shader(shaders/post/flare.hlsl GhostPS ps_6_6 g_flareGhostPS)
foreach(pass 0 1 2 3 4)
    add_shader(shaders/amd/dof.hlsl main cs_6_6 g_dof${pass}CS -D DOF_PASS=${pass} ${FFX_SHADER_OPTIONS})
endforeach()
add_shader(shaders/amd/dof.hlsl main cs_6_6 g_dof0ReverseCS -D DOF_PASS=0 -D FFX_DOF_OPTION_REVERSE_DEPTH=1 ${FFX_SHADER_OPTIONS})
add_shader(shaders/amd/cas.hlsl main cs_6_6 g_casCS ${FFX_SHADER_OPTIONS})
add_shader(shaders/amd/lens.hlsl LensMain cs_6_6 g_lensCS ${FFX_SHADER_OPTIONS})
add_shader(shaders/amd/lens.hlsl FinalMain ps_6_6 g_postFinalPS ${FFX_SHADER_OPTIONS})
# The display mapping at present (src/display_output.cpp).
add_shader(shaders/post/display_output.hlsl VSMain vs_6_6 g_displayOutputVS)
add_shader(shaders/post/display_output.hlsl PSMain ps_6_6 g_displayOutputPS)
add_shader(shaders/post/display_output.hlsl UiAlphaMain ps_6_6 g_displayUiAlphaPS)
add_d3d9_shader(shaders/forward_snippet.hlsl main g_forwardSnippetPS)
add_d3d9_shader(shaders/forward_snippet.hlsl main g_forwardSnippetPS2 PROFILE ps_2_b /D FORWARD_PS2=1)
add_d3d9_shader(shaders/water_surface.hlsl main g_waterSurfacePS PROFILE ps_2_b)
add_d3d9_shader(shaders/water_surface.hlsl main g_waterSurfaceAlphaFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1)
add_d3d9_shader(shaders/water_surface.hlsl main g_waterSurfaceLightmapFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1 /D LIGHTMAP_FOG=1)
add_d3d9_shader(shaders/water_surface.hlsl main g_waterSurfaceOpaqueFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1 /D LIGHTMAP_FOG=1 /D OPAQUE_FOG=1)
add_d3d9_shader(shaders/water_textured_surface.hlsl main g_waterTexturedSurfacePS PROFILE ps_2_b)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfacePS PROFILE ps_2_b)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfaceColorPS PROFILE ps_2_b /D FLOW_COLOR=1)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfaceColorLightmapPS PROFILE ps_2_b /D FLOW_COLOR=1 /D LIGHTMAP_FOG=1)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfaceAlphaFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfaceLightmapFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1 /D LIGHTMAP_FOG=1)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfaceOpaqueFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1 /D LIGHTMAP_FOG=1 /D OPAQUE_FOG=1)

# NVIDIA DLSS SDK (Ray Reconstruction): headers, the NGX static library for the static runtime, the
# release feature DLL and the licence, each pinned by hash.
set(DLSS_SDK_VERSION v310.9.16)
set(DLSS_SDK_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/dlss-${DLSS_SDK_VERSION}")
set(DLSS_SDK_FILES
    "include/nvsdk_ngx.h|dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"
    "include/nvsdk_ngx_defs.h|ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
    "include/nvsdk_ngx_defs_dlssd.h|2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716"
    "include/nvsdk_ngx_helpers.h|5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"
    "include/nvsdk_ngx_helpers_cuda.h|f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"
    "include/nvsdk_ngx_helpers_d3d.h|ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"
    "include/nvsdk_ngx_helpers_dlssd_d3d.h|6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293"
    "include/nvsdk_ngx_params.h|943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
    "include/nvsdk_ngx_params_dlssd.h|d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e"
    "lib/Windows_x86_64/x64/nvsdk_ngx_s.lib|7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765"
    "lib/Windows_x86_64/rel/nvngx_dlssd.dll|b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee"
    "LICENSE.txt|d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"
)
foreach(entry IN LISTS DLSS_SDK_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/NVIDIA/DLSS/${DLSS_SDK_VERSION}/${file}" "${DLSS_SDK_DIR}/${file}"
        EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
endforeach()
set(DLSS_FEATURE_DLL "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")

# NVIDIA Streamline 2.14.16 (MIT; its prebuilt plugins under their own licences), the release package pinned by hash:
# the headers, the interposer's import library (delay-loaded, so gmodpp.dll loads without it) and NVIDIA's signed
# production DLLs for Reflex, PC latency markers and DLSS Frame Generation. Like nvngx_dlssd.dll, nvngx_dlssg.dll is
# distributed only inside the release zip.
set(STREAMLINE_VERSION v2.14.16)
set(STREAMLINE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/streamline-${STREAMLINE_VERSION}")
if(NOT EXISTS "${STREAMLINE_DIR}/include/sl.h")
    file(DOWNLOAD "https://github.com/NVIDIA-RTX/Streamline/releases/download/${STREAMLINE_VERSION}/streamline-sdk-${STREAMLINE_VERSION}.zip"
        "${STREAMLINE_DIR}.zip" EXPECTED_HASH SHA256=32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${STREAMLINE_DIR}.zip" DESTINATION "${STREAMLINE_DIR}"
        PATTERNS "include/*" "lib/x64/sl.interposer.lib" "bin/x64/sl.interposer.dll" "bin/x64/sl.common.dll" "bin/x64/sl.reflex.dll"
        "bin/x64/sl.pcl.dll" "bin/x64/sl.dlss_g.dll" "bin/x64/nvngx_dlssg.dll" "bin/x64/nvngx_dlss.license.txt" "bin/x64/reflex.license.txt" "license.txt")
    file(REMOVE "${STREAMLINE_DIR}.zip")
endif()
set(STREAMLINE_DLLS "${STREAMLINE_DIR}/bin/x64/sl.interposer.dll" "${STREAMLINE_DIR}/bin/x64/sl.common.dll" "${STREAMLINE_DIR}/bin/x64/sl.reflex.dll"
    "${STREAMLINE_DIR}/bin/x64/sl.pcl.dll" "${STREAMLINE_DIR}/bin/x64/sl.dlss_g.dll" "${STREAMLINE_DIR}/bin/x64/nvngx_dlssg.dll")

# Lucide icon font (ISC) for the menu's icons, from the release's font zip, with the repository's licence file.
set(LUCIDE_VERSION 1.67.0)
set(LUCIDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/lucide-${LUCIDE_VERSION}")
if(NOT EXISTS "${LUCIDE_DIR}/lucide-font/lucide.ttf")
    file(DOWNLOAD "https://github.com/lucide-icons/lucide/releases/download/${LUCIDE_VERSION}/lucide-font-${LUCIDE_VERSION}.zip"
        "${LUCIDE_DIR}.zip" EXPECTED_HASH SHA256=db8ed26ed8ec84d7251e2d9c0eb028358db59c3f4bab3532d65e2f4b17e60163 TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${LUCIDE_DIR}.zip" DESTINATION "${LUCIDE_DIR}" PATTERNS "lucide-font/lucide.ttf" "lucide-font/codepoints.json")
    file(REMOVE "${LUCIDE_DIR}.zip")
endif()
if(NOT EXISTS "${LUCIDE_DIR}/LICENSE")
    file(DOWNLOAD "https://raw.githubusercontent.com/lucide-icons/lucide/${LUCIDE_VERSION}/LICENSE" "${LUCIDE_DIR}/LICENSE"
        EXPECTED_HASH SHA256=b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57 TLS_VERIFY ON)
endif()

# LZMA SDK decoder (public domain) for maps with compressed lumps, each file pinned by hash.
set(LZMA_SDK_VERSION 26.04)
set(LZMA_SDK_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/lzma-${LZMA_SDK_VERSION}")
set(LZMA_SDK_FILES
    "7zTypes.h|a5d03b8cb65cd3a2a56bde46dd68fb356d2ebacd9d70283e7d4d20b4e0a64713"
    "Compiler.h|d5e42be77d26beaa8c81d3e62ef99fc5685736cfc079779fdb9154d05fa8bc4a"
    "Precomp.h|c8903f2e36a771a272d8981e1e9ffed5ba16ca3d2f3dca6d1414d2340a6829d0"
    "LzmaDec.h|3aaf07b4ae4173a2d103179455dc7089b5ddbc7fc3db3c0e40964a7499c69266"
    "LzmaDec.c|4e6ec665a6df01f1722e2b2fc36dc5e3aa7c3890e0f8cb11c6a07c9a01f3309b"
)
foreach(entry IN LISTS LZMA_SDK_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/ip7z/7zip/${LZMA_SDK_VERSION}/C/${file}" "${LZMA_SDK_DIR}/${file}"
        EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
endforeach()
add_library(lzma_dec STATIC "${LZMA_SDK_DIR}/LzmaDec.c")

# The Source SDK 2013 shader headers the G-buffer twins (shaders/twins) include, from the commit GMod's shaders derive
# from, each file pinned by hash. They are compiled at runtime with the twins.
set(SDK_SHADERS_COMMIT 7309a5f13f63fdcc7b1e090f6c176113a9d95061)
set(SDK_SHADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/sdk2013-shaders")
set(SDK_SHADER_FILES
    "common_flashlight_fxc.h|d111736b5496f50c3ccc8c37519cb2bb428be1761e7f6614bdd3e98e911f7fc8"
    "common_vertexlitgeneric_dx9.h|a60fbc9211f740961a0bfd76aeb158d5e2ba8853b32589e9f246fa2dcb085952"
    "common_ps_fxc.h|9ec1c3ebbd9b9c06aa89906c91cc1464cd2be903afc38401aa2b45339ce9b556"
    "common_fxc.h|91c2beb8b06e007169ea0abf4f16687dfff122005658bc5eb7a3dc55296b5a11"
    "common_pragmas.h|a4a844949c9b352627e6f3e00d18c32a132451a0d011b62bd0a4d8e269186235"
    "common_lightmappedgeneric_fxc.h|dc1c9afbd858d3315daea70cd1f26eff226a9f2f4081f0b5538c81fe2102ef3a"
    "common_hlsl_cpp_consts.h|147d39bfdf6d0266be3fd6b60dcdbbaf839e2be7853349bac0813d5605de68c5"
    "shader_constant_register_map.h|2f26fec6bf575edb22aed53862faea49f8b2054338728e80c6edb5bc14b4dc7a"
)
set(SDK_SHADER_PATHS "")
foreach(entry IN LISTS SDK_SHADER_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/ValveSoftware/source-sdk-2013/${SDK_SHADERS_COMMIT}/mp/src/materialsystem/stdshaders/${file}"
        "${SDK_SHADERS_DIR}/${file}" EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
    list(APPEND SDK_SHADER_PATHS "${SDK_SHADERS_DIR}/${file}")
endforeach()
file(GLOB LIGHTING_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.hlsli")
file(GLOB TWIN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/twins/*.fxc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/twins/*.h")
target_include_directories(lzma_dec PUBLIC "${LZMA_SDK_DIR}")


# winmm.dll loader for the game root: forwards every export of the system winmm.dll (winmm_exports.txt holds
# their ordinals and names) and, with -insecure, sends the game's shaderapidx9.dll load to gmodpp.dll.
file(STRINGS src/winmm_exports.txt WINMM_EXPORTS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/winmm_exports.txt)
list(LENGTH WINMM_EXPORTS WINMM_COUNT)
set(WINMM_DEF_EXPORTS "")
set(WINMM_NAMES "")
set(WINMM_STRINGS "")
set(WINMM_STUBS "")
set(index 0)
foreach(entry IN LISTS WINMM_EXPORTS)
    string(REPLACE " " ";" entry "${entry}")
    list(GET entry 0 ordinal)
    list(GET entry 1 name)
    if(name STREQUAL "NONAME")
        string(APPEND WINMM_DEF_EXPORTS "    Ordinal${ordinal}=gmodpp_winmm_${index} @${ordinal} NONAME\n")
        string(APPEND WINMM_NAMES "    QWORD ${ordinal}\n")
    else()
        string(APPEND WINMM_DEF_EXPORTS "    ${name}=gmodpp_winmm_${index} @${ordinal}\n")
        string(APPEND WINMM_NAMES "    QWORD winmmName${index}\n")
        string(APPEND WINMM_STRINGS "winmmName${index} BYTE \"${name}\", 0\n")
    endif()
    string(APPEND WINMM_STUBS "WINMM_STUB ${index}\n")
    math(EXPR index "${index} + 1")
endforeach()
configure_file(src/winmm.def.in "${CMAKE_CURRENT_BINARY_DIR}/winmm.def" @ONLY)
configure_file(src/winmm_stubs.asm.in "${CMAKE_CURRENT_BINARY_DIR}/winmm_stubs.asm" @ONLY)

add_library(gmodpp_loader SHARED
    src/loader.cpp
    src/loader.rc
    src/command_line.cpp
    src/log.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/winmm.def"
    "${CMAKE_CURRENT_BINARY_DIR}/winmm_stubs.asm"
)
set_target_properties(gmodpp_loader PROPERTIES OUTPUT_NAME winmm)
target_compile_definitions(gmodpp_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(gmodpp_loader PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(gmodpp_loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_link_libraries(gmodpp_loader PRIVATE minhook)

# The game's shader API replacement, loaded from bin\win64\gmodpp through the loader.
add_library(gmodpp SHARED
    src/gmodpp.rc
    src/shaderapi_shim.cpp
    src/shaderapi_slots.cpp
    src/shaderapi_thunks.asm
    src/busy_window.cpp
    src/capture.cpp
    src/color.cpp
    src/command_line.cpp
    src/log.cpp
    src/module_scan.cpp
    src/d3d12_device.cpp
    src/descriptor_heap.cpp
    src/shader_bytecode.cpp
    src/shader_compiler.cpp
    src/shader_cache.cpp
    src/shader_precache.cpp
    src/shader_translator.cpp
    src/text_layout.cpp
    src/ui_text.cpp
    src/vcs.cpp
    src/vpk.cpp
    src/shaderapi.cpp
    src/shaderapi_config.cpp
    src/shaderapi_device.cpp
    src/shaderapi_native.cpp
    src/shaderapi_meshes.cpp
    src/shaderapi_pipelines.cpp
    src/post_effects.cpp
    src/lens_flare.cpp
    src/display_output.cpp
    src/exposure.cpp
    src/fsr.cpp
    src/anti_lag.cpp
    src/hardware_check.cpp
    src/streamline.cpp
    src/shaderapi_shadow.cpp
    src/shaderapi_textures.cpp
    src/shaderapi_capture.cpp
    src/shader_twins.cpp
    src/video_menu.cpp
    src/watchdog.cpp
    # Real-time lighting.
    src/native_lighting.cpp
    src/world.cpp
    src/lighting.cpp
    src/night_sky.cpp
    src/clouds.cpp
    src/gbuffer_shaders.cpp
    src/scene.cpp
    src/bsp.cpp
    src/studio_model.cpp
    src/opacity_micromaps.cpp
    src/skinned_casters.cpp
    src/viewmodel_casters.cpp
    src/particle_shadows.cpp
    src/swaying_props.cpp
    src/ropes.cpp
    src/settings.cpp
    src/dlss.cpp
    src/entities.cpp
    src/engine_interfaces.cpp
    src/client_hooks.cpp
    src/light_queries.cpp
    src/loading_screen.cpp
    src/overlay.cpp
    src/overlay_fonts.cpp
    src/overlay_widgets.cpp
    src/restir.cpp
    src/nrd_denoiser.cpp
    "${RTXDI_DIR}/Source/ReSTIRDI.cpp"
    "${RTXDI_DIR}/Source/ReGIR.cpp"
    "${RTXDI_DIR}/Source/RtxdiUtils.cpp"
    "${RTXDI_DIR}/Source/RISBufferSegmentAllocator.cpp"
    ${SHADER_HEADERS}
)
target_compile_definitions(gmodpp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(gmodpp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_include_directories(gmodpp BEFORE PRIVATE "${AGILITY_SDK_DIR}/build/native/include")
target_include_directories(gmodpp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${DXC_DIR}/inc" "${SHADER_OUTPUT_DIR}" "${DLSS_SDK_DIR}/include" "${RTXDI_DIR}/Include"
    "${FSR_DIR}/Kits/FidelityFX/api/include" "${FSR_DIR}/Kits/FidelityFX/upscalers/include" "${FSR_DIR}/Kits/FidelityFX/denoisers/include"
    "${FSR_DIR}/Kits/FidelityFX/framegeneration/include" "${FSR_DIR}/Kits/OpenSource/amd/antilag2" "${STREAMLINE_DIR}/include")
# ReSTIRDI.cpp leaves a parameter unused in release builds (its check is an assert).
set_source_files_properties("${RTXDI_DIR}/Source/ReSTIRDI.cpp" PROPERTIES COMPILE_OPTIONS /wd4100)
target_link_libraries(gmodpp PRIVATE d3d12 dxgi dxguid d2d1 dwrite icu lzma_dec dbghelp minhook imgui NRD "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
    "${STREAMLINE_DIR}/lib/x64/sl.interposer.lib" delayimp)
# Streamline's interposer is loaded by its full path once its signature is checked (src/streamline.cpp).
target_link_options(gmodpp PRIVATE /DELAYLOAD:sl.interposer.dll)

# The mod laid out as the game folder: the loader in the root and everything else in gmodplusplus, with the licences
# every bundled component requires. deploy copies it into the game and release zips it.
set(STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/release/GarrysModPP")
set(STAGE_MOD "${STAGE_DIR}/gmodplusplus")
set(STAGE_LICENSES "${STAGE_MOD}/licenses")
add_custom_target(stage
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_MOD}/D3D12" "${STAGE_MOD}/hlsl" "${STAGE_MOD}/twins" "${STAGE_MOD}/fonts" "${STAGE_LICENSES}"
    COMMAND ${CMAKE_COMMAND} -E copy "${LUCIDE_DIR}/lucide-font/lucide.ttf" "${STAGE_MOD}/fonts/"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gmodpp_loader> "${STAGE_DIR}/winmm.dll"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gmodpp> "${DXC_DIR}/bin/x64/dxcompiler.dll" "${DLSS_FEATURE_DLL}" ${FSR_DLLS} ${STREAMLINE_DLLS} "${STAGE_MOD}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${AGILITY_RUNTIME_DIR}/D3D12Core.dll" "${AGILITY_RUNTIME_DIR}/d3d12SDKLayers.dll" "${STAGE_MOD}/D3D12/"
    COMMAND ${CMAKE_COMMAND} -E copy ${LIGHTING_HEADERS} "${STAGE_MOD}/hlsl/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/localization" "${STAGE_MOD}/localization"
    COMMAND ${CMAKE_COMMAND} -E copy ${TWIN_SOURCES} ${SDK_SHADER_PATHS} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/twins/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/twins/thirdpartylegalnotices.txt" "${STAGE_MOD}/twins/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "${STAGE_LICENSES}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${minhook_SOURCE_DIR}/LICENSE.txt" "${STAGE_LICENSES}/MinHook.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${imgui_SOURCE_DIR}/LICENSE.txt" "${STAGE_LICENSES}/DearImGui.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${LUCIDE_DIR}/LICENSE" "${STAGE_LICENSES}/Lucide.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${DXC_DIR}/LICENSE-LLVM.txt" "${STAGE_LICENSES}/DXC-LLVM.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${DXC_DIR}/LICENSE-MIT.txt" "${STAGE_LICENSES}/DXC-MIT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${DXC_DIR}/LICENSE-MS.txt" "${STAGE_LICENSES}/DXC-Microsoft.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${AGILITY_SDK_DIR}/LICENSE.txt" "${STAGE_LICENSES}/DirectX-Agility-SDK.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${DLSS_SDK_DIR}/LICENSE.txt" "${STAGE_LICENSES}/NVIDIA-DLSS-SDK.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${SHARC_DIR}/License.md" "${STAGE_LICENSES}/NVIDIA-SHaRC.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${RTXDI_DIR}/LICENSE.txt" "${STAGE_LICENSES}/NVIDIA-RTXDI.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${FFX_DIR}/LICENSE.txt" "${STAGE_LICENSES}/AMD-FidelityFX-SDK.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${FSR_DIR}/Kits/FidelityFX/docs/license.md" "${STAGE_LICENSES}/AMD-FSR-SDK.md"
    COMMAND ${CMAKE_COMMAND} -E copy "${FSR_DIR}/Kits/OpenSource/amd/antilag2/LICENSE.txt" "${STAGE_LICENSES}/AMD-Anti-Lag-2-SDK.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${STREAMLINE_DIR}/license.txt" "${STAGE_LICENSES}/NVIDIA-Streamline.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${STREAMLINE_DIR}/bin/x64/nvngx_dlss.license.txt" "${STAGE_LICENSES}/NVIDIA-DLSS-Frame-Generation.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${STREAMLINE_DIR}/bin/x64/reflex.license.txt" "${STAGE_LICENSES}/NVIDIA-Reflex.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${nrd_SOURCE_DIR}/LICENSE.txt" "${STAGE_LICENSES}/NVIDIA-NRD.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${MATHLIB_SOURCE_DIR}/LICENSE.txt" "${STAGE_LICENSES}/MathLib.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${SHADERMAKE_SOURCE_DIR}/LICENSE.txt" "${STAGE_LICENSES}/ShaderMake.txt"
    DEPENDS gmodpp_loader gmodpp
    COMMENT "Staging the mod in ${STAGE_DIR}"
)

set(RELEASE_ZIP "${CMAKE_CURRENT_BINARY_DIR}/release/GarrysModPP.zip")
add_custom_target(release
    COMMAND ${CMAKE_COMMAND} -E rm -f "${RELEASE_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${STAGE_DIR}" ${CMAKE_COMMAND} -E tar cf "${RELEASE_ZIP}" --format=zip winmm.dll gmodplusplus
    DEPENDS stage
    COMMENT "Packaging ${RELEASE_ZIP}"
)

if(GMOD_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} "-DTARGET=${GMOD_DIR}/bin/win64/d3d9.dll" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/remove_old_proxy.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${STAGE_MOD}" "${GMOD_DIR}/gmodplusplus"
        COMMAND ${CMAKE_COMMAND} "-DSOURCE=${STAGE_DIR}/winmm.dll" "-DDESTINATION=${GMOD_DIR}/winmm.dll"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy_loader.cmake"
        DEPENDS stage
        COMMENT "Deploying the mod to ${GMOD_DIR}"
    )
endif()
