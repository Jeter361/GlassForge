cmake_minimum_required(VERSION 3.20)
project(GlassForgeNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT: nothing extra to ship into targets

# Microsoft Detours (MIT), x64 only.
set(DETOURS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Detours/src)
add_library(detours STATIC
    ${DETOURS_DIR}/detours.cpp
    ${DETOURS_DIR}/modules.cpp
    ${DETOURS_DIR}/disasm.cpp
    ${DETOURS_DIR}/image.cpp
    ${DETOURS_DIR}/creatwth.cpp
    ${DETOURS_DIR}/disolx64.cpp)
target_include_directories(detours PUBLIC ${DETOURS_DIR})
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN _AMD64_)
set_source_files_properties(${DETOURS_DIR}/disolx64.cpp PROPERTIES COMPILE_DEFINITIONS DETOURS_TARGET_PROCESSOR=X64)

# The DLL GlassForge loads into a target app to make its background glass.
add_library(GlassForge.Hook SHARED
    hook/dllmain.cpp
    hook/xaml.cpp
    hook/exports.def
    hook/log.cpp)
target_link_libraries(GlassForge.Hook PRIVATE detours user32 gdi32 dwmapi uxtheme comctl32 d2d1 ole32 oleaut32 uuid)
target_compile_definitions(GlassForge.Hook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(GlassForge.Hook PRIVATE /W4 /permissive-)

