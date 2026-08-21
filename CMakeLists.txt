cmake_minimum_required(VERSION 3.24)
project(BL1GOTYVR LANGUAGES CXX ASM_MASM)

if(NOT WIN32)
    message(FATAL_ERROR "BL1GOTYVR is Windows-only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BorderlandsGOTY.exe is 64-bit. Configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

include(FetchContent)

# OpenXR SDK
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    OpenXR
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.43
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(OpenXR)

# MinHook
FetchContent_Declare(
    MinHook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(MinHook)

# Dear ImGui for the in-game F10 tuning overlay.
FetchContent_Declare(
    ImGui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ImGui)
add_library(bl1gotyvr_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(bl1gotyvr_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(bl1gotyvr_imgui PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_link_libraries(bl1gotyvr_imgui PRIVATE d3d11 imm32)

# Main VR mod DLL
add_library(BL1GOTYVR SHARED
    src/core/VRMod.cpp
    src/core/CommandSystem.cpp
    src/d3d11/D3D11Hooks.cpp
    src/display/DisplayHooks.cpp
    src/camera/CameraHook.cpp
    src/camera/UE3Scanner.cpp
    src/camera/SignatureScanner.cpp
    src/stereo/StereoRenderer.cpp
    src/stereo/EyeManager.cpp
    src/xr/OpenXRContext.cpp
    src/xr/FrameLoop.cpp
    src/input/InputHook.cpp
    src/input/XRInput.cpp
    src/input/XInputBridge.cpp
    src/input/AimHook.cpp
    src/input/WeaponAimSystem.cpp
    src/player/TwoBoneIK.cpp
    src/player/ArmIKSystem.cpp
    src/render/HudBlitter.cpp
    src/ui/Overlay.cpp
    src/config/Config.cpp
    src/hook/MinHookWrapper.cpp)

target_include_directories(BL1GOTYVR PRIVATE src include)
target_link_libraries(BL1GOTYVR PRIVATE minhook openxr_loader bl1gotyvr_imgui
    d3d11 dxgi dxguid d3dcompiler gdi32 imm32)
target_compile_definitions(BL1GOTYVR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Automatic loader placed beside BorderlandsGOTY.exe. It forwards DXGI to
# System32 and loads BL1GOTYVR.dll before the game's first DXGI factory call.
add_library(dxgi_proxy SHARED
    src/dxgi_proxy.cpp
    src/dxgi_proxy_stubs.asm
    src/dxgi_proxy.def)
set_target_properties(dxgi_proxy PROPERTIES
    OUTPUT_NAME "dxgi"
    ARCHIVE_OUTPUT_NAME "dxgi_proxy")
target_compile_definitions(dxgi_proxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Injector
add_executable(injector injector/main.cpp)
target_compile_definitions(injector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(injector PRIVATE shell32)

# Standalone diagnostic used to inspect an unmodified DXGI Present prologue.
add_executable(clean_present tools/clean_present.cpp)
target_compile_definitions(clean_present PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(clean_present PRIVATE d3d11 dxgi)

# Native configuration utility. It writes BL1GOTYVR.ini beside the executable.
add_executable(BL1GOTYVRConfig WIN32 configurator/main.cpp)
target_compile_definitions(BL1GOTYVRConfig PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(BL1GOTYVRConfig PRIVATE shell32)
