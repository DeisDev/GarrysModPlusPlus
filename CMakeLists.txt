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
FetchContent_MakeAvailable(minhook)

find_program(DXC_EXECUTABLE dxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
set(SHADER_HEADERS)

function(add_shader source entry profile variable)
    set(output "${SHADER_OUTPUT_DIR}/${variable}.h")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${DXC_EXECUTABLE}" -nologo -T ${profile} -E ${entry} -O3 -HV 2021 -Fh "${output}" -Vn ${variable} "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.hlsli"
        COMMENT "Compiling ${source} (${entry})"
        VERBATIM
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} "${output}" PARENT_SCOPE)
endfunction()

add_shader(shaders/sun_shadows.hlsl main cs_6_5 g_sunShadowsCS)
add_shader(shaders/composite.hlsl VSMain vs_6_0 g_compositeVS)
add_shader(shaders/composite.hlsl PSMain ps_6_0 g_compositePS)

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
    src/entities.cpp
    src/bsp.cpp
    src/studio_model.cpp
    src/scene.cpp
    src/skinned_casters.cpp
    src/effects.cpp
    src/sun_shadows.cpp
    ${SHADER_HEADERS}
)
set_target_properties(gmodpp_d3d9 PROPERTIES OUTPUT_NAME d3d9)
target_include_directories(gmodpp_d3d9 PRIVATE "${SHADER_OUTPUT_DIR}")
target_compile_definitions(gmodpp_d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(gmodpp_d3d9 PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(gmodpp_d3d9 PRIVATE d3d12 dxgi dxguid minhook)

if(GMOD_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gmodpp_d3d9> "${GMOD_DIR}/bin/win64/d3d9.dll"
        DEPENDS gmodpp_d3d9
        COMMENT "Deploying d3d9.dll to ${GMOD_DIR}/bin/win64"
    )
endif()
