cmake_minimum_required(VERSION 3.25)
project(GarrysModPP LANGUAGES C CXX ASM_MASM)

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

# Extra arguments are dxc options.
function(add_shader source entry profile variable)
    set(output "${SHADER_OUTPUT_DIR}/${variable}.h")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${DXC_EXECUTABLE}" -nologo -T ${profile} -E ${entry} -O3 -HV 2021 ${ARGN} -Fh "${output}" -Vn ${variable} "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/reflection.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/water.hlsli"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/baked.hlsli"
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
# Passes that trace rays, and their Shader Model 6.9 variants that let ray queries use opacity micromaps.
foreach(pass primary probes ao volumetrics direct reflections gi_trace)
    string(REPLACE "_t" "T" variable ${pass})
    add_shader(shaders/${pass}.hlsl main cs_6_6 g_${variable}CS)
    add_shader(shaders/${pass}.hlsl main cs_6_9 g_${variable}OmmCS -D OPACITY_MICROMAPS=1)
endforeach()
add_shader(shaders/direct.hlsl main cs_6_6 g_directWideCS -D WIDE_LIGHT_SETS=1)
add_shader(shaders/direct.hlsl main cs_6_9 g_directWideOmmCS -D WIDE_LIGHT_SETS=1 -D OPACITY_MICROMAPS=1)
add_shader(shaders/reflection_temporal.hlsl main cs_6_6 g_reflectionTemporalCS)
add_shader(shaders/baked_residual.hlsl main cs_6_6 g_bakedResidualCS)
add_shader(shaders/sway.hlsl main cs_6_6 g_swayCS)
add_shader(shaders/dlss_inputs.hlsl main cs_6_6 g_dlssInputsCS)
add_shader(shaders/dlss_resolve.hlsl main cs_6_6 g_dlssResolveCS)
add_shader(shaders/gi_temporal.hlsl main cs_6_6 g_giTemporalCS)
add_shader(shaders/gi_filter.hlsl main cs_6_6 g_giFilterCS)
add_shader(shaders/deferred_lighting.hlsl main cs_6_6 g_deferredLightingCS)
add_shader(shaders/taa.hlsl main cs_6_6 g_taaCS)
add_shader(shaders/taa_sharpen.hlsl main cs_6_6 g_taaSharpenCS)
add_shader(shaders/composite.hlsl VSMain vs_6_0 g_compositeVS)
add_shader(shaders/composite.hlsl PSMain ps_6_0 g_compositePS)
add_shader(shaders/composite.hlsl DepthPS ps_6_0 g_compositeDepthPS)
add_shader(shaders/composite.hlsl CoveredPS ps_6_0 g_compositeCoveredPS)
add_shader(shaders/view_rect.hlsl main cs_6_6 g_viewRectCS)
add_shader(shaders/translucent_depth.hlsl main cs_6_6 g_translucentDepthCS)
add_d3d9_shader(shaders/forward_snippet.hlsl main g_forwardSnippetPS)
add_d3d9_shader(shaders/forward_snippet.hlsl main g_forwardSnippetPS2 PROFILE ps_2_b /D FORWARD_PS2=1)
add_d3d9_shader(shaders/water_surface.hlsl main g_waterSurfacePS PROFILE ps_2_b)
add_d3d9_shader(shaders/water_surface.hlsl main g_waterSurfaceAlphaFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfacePS PROFILE ps_2_b)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfaceAlphaFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1)
add_d3d9_shader(shaders/water_flow_surface.hlsl main g_waterFlowSurfaceLightmapFogPS PROFILE ps_2_b /D REFRACT_ALPHA_FOG=1 /D LIGHTMAP_FOG=1)

# NVIDIA DLSS SDK (Ray Reconstruction): headers, the NGX static library for the static runtime, and the
# release feature DLL, each pinned by hash.
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
)
foreach(entry IN LISTS DLSS_SDK_FILES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 file)
    list(GET parts 1 hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/NVIDIA/DLSS/${DLSS_SDK_VERSION}/${file}" "${DLSS_SDK_DIR}/${file}"
        EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON)
endforeach()
set(DLSS_FEATURE_DLL "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")

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
target_compile_options(gmodpp_loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_link_libraries(gmodpp_loader PRIVATE minhook)

# The game's shader API replacement, loaded from bin\win64\gmodpp through the loader.
add_library(gmodpp SHARED
    src/shaderapi_shim.cpp
    src/shaderapi_slots.cpp
    src/shaderapi_thunks.asm
    src/capture.cpp
    src/command_line.cpp
    src/log.cpp
    src/module_scan.cpp
    src/d3d12_device.cpp
    src/descriptor_heap.cpp
    src/shader_bytecode.cpp
    src/shader_compiler.cpp
    src/shader_cache.cpp
    src/shader_prewarm.cpp
    src/prewarm_window.cpp
    src/shader_translator.cpp
    src/ui_text.cpp
    src/vcs.cpp
    src/vpk.cpp
    src/shaderapi.cpp
    src/shaderapi_config.cpp
    src/shaderapi_device.cpp
    src/shaderapi_native.cpp
    src/shaderapi_meshes.cpp
    src/shaderapi_pipelines.cpp
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
    src/gbuffer_shaders.cpp
    src/scene.cpp
    src/bsp.cpp
    src/studio_model.cpp
    src/convex_hull.cpp
    src/opacity_micromaps.cpp
    src/skinned_casters.cpp
    src/swaying_props.cpp
    src/ropes.cpp
    src/settings.cpp
    src/dlss.cpp
    src/entities.cpp
    src/engine_interfaces.cpp
    src/client_hooks.cpp
    src/light_queries.cpp
    src/overlay.cpp
    ${SHADER_HEADERS}
)
target_compile_definitions(gmodpp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(gmodpp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_include_directories(gmodpp BEFORE PRIVATE "${AGILITY_SDK_DIR}/build/native/include")
target_include_directories(gmodpp PRIVATE "${DXC_DIR}/inc" "${SHADER_OUTPUT_DIR}" "${DLSS_SDK_DIR}/include")
target_link_libraries(gmodpp PRIVATE d3d12 dxgi dxguid lzma_dec dbghelp comctl32 minhook imgui "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")

if(GMOD_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} "-DTARGET=${GMOD_DIR}/bin/win64/d3d9.dll" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/remove_old_proxy.cmake"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLSS_FEATURE_DLL}" "${GMOD_DIR}/bin/win64/nvngx_dlssd.dll"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GMOD_DIR}/bin/win64/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AGILITY_RUNTIME_DIR}/D3D12Core.dll" "${AGILITY_RUNTIME_DIR}/d3d12SDKLayers.dll"
            "${GMOD_DIR}/bin/win64/D3D12/"
        COMMAND ${CMAKE_COMMAND} "-DSOURCE=$<TARGET_FILE:gmodpp_loader>" "-DDESTINATION=${GMOD_DIR}/winmm.dll"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy_loader.cmake"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GMOD_DIR}/bin/win64/gmodpp"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gmodpp> "${GMOD_DIR}/bin/win64/gmodpp/gmodpp.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXC_DIR}/bin/x64/dxcompiler.dll" "${GMOD_DIR}/bin/win64/gmodpp/dxcompiler.dll"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GMOD_DIR}/bin/win64/gmodpp/hlsl"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIGHTING_HEADERS} "${GMOD_DIR}/bin/win64/gmodpp/hlsl/"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GMOD_DIR}/bin/win64/gmodpp/twins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TWIN_SOURCES} ${SDK_SHADER_PATHS} "${GMOD_DIR}/bin/win64/gmodpp/twins/"
        DEPENDS gmodpp_loader gmodpp
        COMMENT "Deploying nvngx_dlssd.dll, the Agility SDK runtime, winmm.dll, gmodpp.dll and the shader twins to ${GMOD_DIR}"
    )
endif()
