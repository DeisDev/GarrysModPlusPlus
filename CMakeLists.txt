cmake_minimum_required(VERSION 3.25)
project(GarrysModPP LANGUAGES C CXX)

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

find_program(DXC_EXECUTABLE dxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
find_program(FXC_EXECUTABLE fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
set(SHADER_HEADERS)

function(add_shader source entry profile variable)
    set(output "${SHADER_OUTPUT_DIR}/${variable}.h")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${DXC_EXECUTABLE}" -nologo -T ${profile} -E ${entry} -O3 -HV 2021 -Fh "${output}" -Vn ${variable} "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.hlsli" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.hlsli"
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
add_shader(shaders/primary.hlsl main cs_6_6 g_primaryCS)
add_shader(shaders/probes.hlsl main cs_6_6 g_probesCS)
add_shader(shaders/ao.hlsl main cs_6_6 g_aoCS)
add_shader(shaders/volumetrics.hlsl main cs_6_6 g_volumetricsCS)
add_shader(shaders/direct.hlsl main cs_6_6 g_directCS)
add_shader(shaders/reflections.hlsl main cs_6_6 g_reflectionsCS)
add_shader(shaders/reflection_temporal.hlsl main cs_6_6 g_reflectionTemporalCS)
add_shader(shaders/dlss_inputs.hlsl main cs_6_6 g_dlssInputsCS)
add_shader(shaders/dlss_resolve.hlsl main cs_6_6 g_dlssResolveCS)
add_shader(shaders/gi_trace.hlsl main cs_6_6 g_giTraceCS)
add_shader(shaders/gi_temporal.hlsl main cs_6_6 g_giTemporalCS)
add_shader(shaders/gi_filter.hlsl main cs_6_6 g_giFilterCS)
add_shader(shaders/deferred_lighting.hlsl main cs_6_6 g_deferredLightingCS)
add_shader(shaders/taa.hlsl main cs_6_6 g_taaCS)
add_shader(shaders/taa_sharpen.hlsl main cs_6_6 g_taaSharpenCS)
add_shader(shaders/composite.hlsl VSMain vs_6_0 g_compositeVS)
add_shader(shaders/composite.hlsl PSMain ps_6_0 g_compositePS)
add_d3d9_shader(shaders/forward_snippet.hlsl main g_forwardSnippetPS)
add_d3d9_shader(shaders/forward_snippet.hlsl main g_forwardSnippetPS2 PROFILE ps_2_b /D FORWARD_PS2=1)
# G-buffer replacements (g_gbuffer*) and their forward-lit variants (g_forward*).
foreach(forward 0 1)
    if(forward)
        set(prefix forward)
    else()
        set(prefix gbuffer)
    endif()
    add_d3d9_shader(shaders/gbuffer_models.hlsl BumpMain g_${prefix}BumpPS /D FORWARD=${forward})
    add_d3d9_shader(shaders/gbuffer_models.hlsl BumpMain g_${prefix}BumpSelfIllumPS /D SELFILLUM=1 /D FORWARD=${forward})
    add_d3d9_shader(shaders/gbuffer_models.hlsl BumpMain g_${prefix}BumpSelfIllumFresnelPS /D SELFILLUM=3 /D FORWARD=${forward})
endforeach()
foreach(normalMap 0 1)
    if(normalMap)
        set(suffix "")
    else()
        set(suffix "NoBump")
    endif()
    foreach(detail 0 1)
        if(detail)
            set(name "Skin${suffix}Detail")
        else()
            set(name "Skin${suffix}")
        endif()
        foreach(forward 0 1)
            if(forward)
                set(prefix forward)
            else()
                set(prefix gbuffer)
            endif()
            set(defines /D SKIN_NORMAL_MAP=${normalMap} /D SKIN_DETAIL=${detail} /D FORWARD=${forward})
            add_d3d9_shader(shaders/gbuffer_models.hlsl SkinMain g_${prefix}${name}PS ${defines})
            add_d3d9_shader(shaders/gbuffer_models.hlsl SkinMain g_${prefix}${name}SelfIllumPS ${defines} /D SELFILLUM=2)
            add_d3d9_shader(shaders/gbuffer_models.hlsl SkinMain g_${prefix}${name}SelfIllumFresnelPS ${defines} /D SELFILLUM=3)
        endforeach()
    endforeach()
endforeach()

add_library(gmodpp_d3d9 SHARED
    src/d3d9.def
    src/mod.cpp
    src/exports.cpp
    src/log.cpp
    src/d3d9_hooks.cpp
    src/d3d9_vtable.c
    src/capture.cpp
    src/watchdog.cpp
    src/frame_trace.cpp
    src/presenter.cpp
    src/renderer.cpp
    src/module_scan.cpp
    src/client_hooks.cpp
    src/engine_interfaces.cpp
    src/light_queries.cpp
    src/entities.cpp
    src/bsp.cpp
    src/studio_model.cpp
    src/convex_hull.cpp
    src/scene.cpp
    src/skinned_casters.cpp
    src/settings.cpp
    src/ui_text.cpp
    src/overlay.cpp
    src/descriptor_heap.cpp
    src/shader_bytecode.cpp
    src/gbuffer.cpp
    src/dlss.cpp
    src/lighting.cpp
    src/effects.cpp
    ${SHADER_HEADERS}
)
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
target_include_directories(lzma_dec PUBLIC "${LZMA_SDK_DIR}")

set_target_properties(gmodpp_d3d9 PROPERTIES OUTPUT_NAME d3d9)
target_include_directories(gmodpp_d3d9 PRIVATE "${SHADER_OUTPUT_DIR}" "${DLSS_SDK_DIR}/include")
target_compile_definitions(gmodpp_d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(gmodpp_d3d9 PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(gmodpp_d3d9 PRIVATE d3d12 dxgi dxguid minhook imgui lzma_dec "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")

if(GMOD_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gmodpp_d3d9> "${GMOD_DIR}/bin/win64/d3d9.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLSS_FEATURE_DLL}" "${GMOD_DIR}/bin/win64/nvngx_dlssd.dll"
        DEPENDS gmodpp_d3d9
        COMMENT "Deploying d3d9.dll and nvngx_dlssd.dll to ${GMOD_DIR}/bin/win64"
    )
endif()
