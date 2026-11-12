cmake_minimum_required(VERSION 3.24)
project(BL1GOTYVR VERSION 0.5.7.3 LANGUAGES CXX ASM_MASM)

if(NOT WIN32)
    message(FATAL_ERROR "BL1GOTYVR is Windows-only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BorderlandsGOTY.exe is 64-bit. Configure with -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Deterministic identity for this source snapshot, including local edits. No
# wall-clock timestamp: reconfiguring identical inputs keeps the same identity.
file(GLOB_RECURSE BL1GOTYVR_IDENTITY_INPUTS CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "src/*.cpp" "src/*.hpp" "src/*.h" "src/*.asm" "src/*.def"
    "configurator/*.cpp" "configurator/*.hpp" "injector/*.cpp" "cmake/*.in")
list(APPEND BL1GOTYVR_IDENTITY_INPUTS "CMakeLists.txt" "build.bat")
list(SORT BL1GOTYVR_IDENTITY_INPUTS)
set(BL1GOTYVR_SOURCE_MANIFEST "")
foreach(input IN LISTS BL1GOTYVR_IDENTITY_INPUTS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${input}" input_hash)
    string(APPEND BL1GOTYVR_SOURCE_MANIFEST "${input}:${input_hash}\n")
endforeach()
string(SHA256 BL1GOTYVR_SOURCE_SHA256 "${BL1GOTYVR_SOURCE_MANIFEST}")
string(SUBSTRING "${BL1GOTYVR_SOURCE_SHA256}" 0 12 BL1GOTYVR_SOURCE_ID)
set(BL1GOTYVR_GIT_REVISION "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE git_revision OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE git_result)
    if(git_result EQUAL 0)
        set(BL1GOTYVR_GIT_REVISION "${git_revision}")
    endif()
endif()
configure_file(cmake/BuildInfo.hpp.in generated/BuildInfo.hpp @ONLY)
configure_file(cmake/build-info.json.in build-info.json @ONLY)

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
    GIT_TAG release-1.1.58
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
    src/player/RoomScaleBody.cpp
    src/render/HudBlitter.cpp
    src/ui/Overlay.cpp
    src/config/Config.cpp
    src/hook/MinHookWrapper.cpp)

target_include_directories(BL1GOTYVR PRIVATE src include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(BL1GOTYVR PRIVATE minhook openxr_loader bl1gotyvr_imgui
    d3d11 dxgi dxguid d3dcompiler gdi32 imm32)
target_compile_definitions(BL1GOTYVR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET BL1GOTYVR PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

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
add_executable(BL1GOTYVRConfig WIN32 configurator/main.cpp configurator/Settings.cpp)
target_compile_definitions(BL1GOTYVRConfig PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(BL1GOTYVRConfig PRIVATE shell32)

option(BL1GOTYVR_BUILD_RENDER_TESTS "Build standalone D3D11 HUD regression tests" OFF)
option(BL1GOTYVR_BUILD_CORE_TESTS "Build standalone configuration and math regression tests" OFF)
if(BL1GOTYVR_BUILD_RENDER_TESTS OR BL1GOTYVR_BUILD_CORE_TESTS)
    enable_testing()
endif()
if(BL1GOTYVR_BUILD_CORE_TESTS)
    add_executable(config_tests tests/ConfigTests.cpp
        src/config/Config.cpp configurator/Settings.cpp
        src/player/TwoBoneIK.cpp src/player/RoomScaleBody.cpp)
    target_compile_definitions(config_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    foreach(case IN ITEMS stereo_reload field_validation atomic_save write_failure config_clamps math)
        add_test(NAME config_${case} COMMAND config_tests ${case})
    endforeach()
    add_executable(turn_tests tests/TurnControllerTests.cpp)
    foreach(case IN ITEMS smooth snap safety queue rotator)
        add_test(NAME turn_${case} COMMAND turn_tests ${case})
    endforeach()
    add_executable(pose_frame_tests tests/PoseFrameTests.cpp)
    add_executable(view_anchor_tests tests/ViewAnchorTests.cpp)
    add_test(NAME view_anchor_walking COMMAND view_anchor_tests)
    add_executable(motion_pair_tests tests/MotionPairTests.cpp)
    add_executable(camera_pose_tests tests/CameraPosePolicyTests.cpp)
    add_executable(manual_reload_tests tests/ManualReloadTests.cpp src/player/TwoBoneIK.cpp)
    foreach(case IN ITEMS affine tracking identity grip projection capture owner)
        add_test(NAME manual_reload_${case} COMMAND manual_reload_tests ${case})
    endforeach()
    add_test(NAME manual_reload_pipeline COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ReloadPipelineOrder.cmake")
    foreach(case IN ITEMS reference recoil thread rotation stereo)
        add_test(NAME camera_pose_${case} COMMAND camera_pose_tests ${case})
    endforeach()
    foreach(case IN ITEMS acquire stage)
        add_test(NAME motion_pair_${case} COMMAND motion_pair_tests ${case})
    endforeach()
    foreach(case IN ITEMS inverse replay invalid)
        add_test(NAME pose_${case} COMMAND pose_frame_tests ${case})
    endforeach()
    add_test(NAME pose_pipeline_order COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PosePipelineOrder.cmake")
    set_target_properties(config_tests turn_tests pose_frame_tests view_anchor_tests motion_pair_tests camera_pose_tests manual_reload_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
if(BL1GOTYVR_BUILD_RENDER_TESTS)
    add_executable(hud_render_tests tests/HudBlitterTests.cpp src/render/HudBlitter.cpp)
    target_compile_definitions(hud_render_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(hud_render_tests PRIVATE d3d11 dxgi d3dcompiler)
    # Never run a DXGI test beside our proxy: Windows would load the mod and
    # apply game configuration changes inside the test process.
    set_target_properties(hud_render_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME hud_render_tests COMMAND hud_render_tests)
endif()
