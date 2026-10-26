cmake_minimum_required(VERSION 3.25)
project(GarrysModPP LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "GarrysMod++ targets the 64-bit branch of Garry's Mod; configure with an x64 toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GMOD_DIR "" CACHE PATH "Garry's Mod install directory; enables the deploy target")

add_library(gmodpp_d3d9 SHARED
    src/d3d9.def
    src/mod.cpp
    src/exports.cpp
    src/log.cpp
    src/d3d9_hooks.cpp
    src/d3d9_vtable.c
    src/capture.cpp
    src/watchdog.cpp
    src/presenter.cpp
    src/renderer.cpp
)
set_target_properties(gmodpp_d3d9 PROPERTIES OUTPUT_NAME d3d9)
target_compile_definitions(gmodpp_d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(gmodpp_d3d9 PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(gmodpp_d3d9 PRIVATE d3d12 dxgi dxguid)

if(GMOD_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gmodpp_d3d9> "${GMOD_DIR}/bin/win64/d3d9.dll"
        DEPENDS gmodpp_d3d9
        COMMENT "Deploying d3d9.dll to ${GMOD_DIR}/bin/win64"
    )
endif()
