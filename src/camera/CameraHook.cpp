#include "CameraHook.hpp"
#include "UE3Scanner.hpp"
#include "SignatureScanner.hpp"
#include "../core/VRMod.hpp"
#include "../hook/MinHookWrapper.hpp"
#include "../xr/FrameLoop.hpp"
#include "../xr/OpenXRContext.hpp"
#include "../config/Config.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "psapi.lib")

namespace bl1gotyvr { namespace camera {

static std::atomic<bool> s_cameraFound{false};
static float s_location[3] = {};
static int32_t s_rotation[3] = {};
static UE3Globals s_globals = {};
static CameraInfo s_camera = {};
using ViewportDrawFn = void(__fastcall*)(void*, void*, void*);
static ViewportDrawFn s_originalViewportDraw = nullptr;
static uintptr_t s_viewportDrawTarget = 0;
using RenderSceneFn = void(__fastcall*)(void*);
static RenderSceneFn s_originalRenderScene = nullptr;
static uintptr_t s_renderSceneTarget = 0;
static bool s_poseReferenceValid = false;
static float s_poseReferencePosition[3] = {};
static float s_poseReferenceRotation[4] = {};

static void RotateByYaw(float yaw, const float local[3], float world[3]) {
    const float sine = sinf(yaw);
    const float cosine = cosf(yaw);
    world[0] = cosine * local[0] - sine * local[1];
    world[1] = sine * local[0] + cosine * local[1];
    world[2] = local[2];
}

static void RelativeQuaternion(const float reference[4], const float current[4], float result[4]) {
    const float ax = -reference[0], ay = -reference[1], az = -reference[2], aw = reference[3];
    const float bx = current[0], by = current[1], bz = current[2], bw = current[3];
    result[0] = aw * bx + ax * bw + ay * bz - az * by;
    result[1] = aw * by - ax * bz + ay * bw + az * bx;
    result[2] = aw * bz + ax * by - ay * bx + az * bw;
    result[3] = aw * bw - ax * bx - ay * by - az * bz;
}

bool IsCameraFound() { return s_cameraFound; }
float* GetCameraLocation() {
    return s_camera.found ? reinterpret_cast<float*>(s_camera.cameraCacheLocation) : s_location;
}
int32_t* GetCameraRotation() {
    return s_camera.found ? reinterpret_cast<int32_t*>(s_camera.cameraCacheRotation) : s_rotation;
}
const UE3Globals& GetUE3Globals() { return s_globals; }

static void __fastcall HookedViewportDraw(void* viewportClient, void* viewport, void* canvas) {
    static thread_local bool dispatching = false;
    static std::atomic<uint64_t> drawCount{0};
    if (dispatching || !s_originalViewportDraw) {
        if (s_originalViewportDraw) s_originalViewportDraw(viewportClient, viewport, canvas);
        return;
    }

    const uint64_t count = ++drawCount;
    if (count == 1 || count % 600 == 0) {
        Log("[Camera] GameViewportClient::Draw heartbeat: count=%llu this=%p camera=%d",
            count, viewportClient, s_camera.found);
    }

    auto& frameLoop = xr::FrameLoop::Instance();
    auto& openXR = xr::OpenXRContext::Instance();
    if (!s_camera.found || !openXR.IsInitialized()) {
        s_originalViewportDraw(viewportClient, viewport, canvas);
        return;
    }

    const bool sequential = frameLoop.BeginSequentialRender();
    const int passCount = sequential ? 2 : 1;
    dispatching = true;
    for (int pass = 0; pass < passCount; ++pass) {
        const int eye = sequential ? pass : frameLoop.GetRenderEye();
        float originalLocation[3] = {};
        int32_t originalRotation[3] = {};
        float originalFov = 0.0f;
        bool cameraOffsetApplied = false;
        if (s_camera.found && s_camera.cameraCacheLocation && s_camera.cameraCacheRotation) {
            __try {
                auto* location = reinterpret_cast<float*>(s_camera.cameraCacheLocation);
                auto* rotation = reinterpret_cast<int32_t*>(s_camera.cameraCacheRotation);
                auto* fov = reinterpret_cast<float*>(s_camera.cameraFov);
                memcpy(originalLocation, location, sizeof(originalLocation));
                memcpy(originalRotation, rotation, sizeof(originalRotation));
                originalFov = *fov;
                constexpr float kUnisToRadians = 6.2831853071795864769f / 65536.0f;
                constexpr float kRadiansToUnis = 65536.0f / 6.2831853071795864769f;
                const float yaw = rotation[1] * kUnisToRadians;

                if (openXR.HasValidPose()) {
                    const float* headPosition = openXR.GetHeadPosition();
                    const float* headRotation = openXR.GetHeadRotation();
                    if (!s_poseReferenceValid) {
                        memcpy(s_poseReferencePosition, headPosition, sizeof(s_poseReferencePosition));
                        memcpy(s_poseReferenceRotation, headRotation, sizeof(s_poseReferenceRotation));
                        s_poseReferenceValid = true;
                    }
                    const float xrDelta[3] = {
                        headPosition[0] - s_poseReferencePosition[0],
                        headPosition[1] - s_poseReferencePosition[1],
                        headPosition[2] - s_poseReferencePosition[2]
                    };
                    const float ueLocal[3] = { -xrDelta[2], xrDelta[0], xrDelta[1] };
                    float ueWorld[3] = {};
                    RotateByYaw(yaw, ueLocal, ueWorld);
                    const float positionScale = 100.0f * config::Get().positional_scale;
                    for (int axis = 0; axis < 3; ++axis) location[axis] += ueWorld[axis] * positionScale;

                    float relative[4] = {};
                    RelativeQuaternion(s_poseReferenceRotation, headRotation, relative);
                    const float forwardX = -2.0f * (relative[0] * relative[2] + relative[3] * relative[1]);
                    const float forwardY = -2.0f * (relative[1] * relative[2] - relative[3] * relative[0]);
                    const float forwardZ = -(1.0f - 2.0f * (relative[0] * relative[0] + relative[1] * relative[1]));
                    const float ueForwardX = -forwardZ;
                    const float ueForwardY = forwardX;
                    const float ueForwardZ = forwardY;
                    const float rotationScale = config::Get().rotation_scale;
                    rotation[1] += static_cast<int32_t>(
                        atan2f(ueForwardY, ueForwardX) * rotationScale * kRadiansToUnis);
                    rotation[0] += static_cast<int32_t>(
                        atan2f(ueForwardZ, sqrtf(ueForwardX * ueForwardX + ueForwardY * ueForwardY)) *
                        rotationScale * kRadiansToUnis);
                    if (config::Get().roll_enabled) {
                        const float roll = atan2f(2.0f * (relative[3] * relative[2] +
                            relative[0] * relative[1]), 1.0f - 2.0f *
                            (relative[1] * relative[1] + relative[2] * relative[2]));
                        rotation[2] += static_cast<int32_t>(roll * rotationScale * kRadiansToUnis);
                    }
                }

                const float halfIpdUnits = config::Get().ipd_mm * 0.05f;
                const float side = eye == 0 ? -halfIpdUnits : halfIpdUnits;
                location[0] += -sinf(yaw) * side;
                location[1] += cosf(yaw) * side;
                float correctedFov = config::Get().fov_degrees;
                float scaleX = 0.0f, scaleY = 0.0f, offsetX = 0.0f, offsetY = 0.0f;
                if (frameLoop.IsProjectionCorrectionEnabled()) {
                    openXR.GetProjectionCrop(eye,
                        static_cast<float>(config::Get().render_width) / config::Get().render_height,
                        scaleX, scaleY, offsetX, offsetY, correctedFov);
                }
                *fov = correctedFov;
                cameraOffsetApplied = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                cameraOffsetApplied = false;
            }
        }

        if (sequential) frameLoop.SetSequentialRenderEye(eye);
        openXR.MarkEyeRendered(eye);
        s_originalViewportDraw(viewportClient, viewport, canvas);
        if (sequential) frameLoop.CaptureSequentialEye(eye);

        if (cameraOffsetApplied) {
            __try {
                static bool loggedAfrWrite = false;
                if (!sequential && !loggedAfrWrite) {
                    const auto* renderedLocation = reinterpret_cast<const float*>(
                        s_camera.cameraCacheLocation);
                    const auto* renderedRotation = reinterpret_cast<const int32_t*>(
                        s_camera.cameraCacheRotation);
                    Log("[Camera] Geometric AFR applied: eye=%d postDrawLoc=(%.1f,%.1f,%.1f) "
                        "postDrawRot=(%d,%d,%d)", eye, renderedLocation[0], renderedLocation[1],
                        renderedLocation[2], renderedRotation[0], renderedRotation[1],
                        renderedRotation[2]);
                    loggedAfrWrite = true;
                }
                memcpy(reinterpret_cast<void*>(s_camera.cameraCacheLocation),
                       originalLocation, sizeof(originalLocation));
                memcpy(reinterpret_cast<void*>(s_camera.cameraCacheRotation),
                       originalRotation, sizeof(originalRotation));
                *reinterpret_cast<float*>(s_camera.cameraFov) = originalFov;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    if (sequential) frameLoop.FinishSequentialRender();
    dispatching = false;
}

static void ProbeRendererCamera(void* renderer) {
    if (!renderer || !s_camera.found) return;
    __try {
        const auto* expected = reinterpret_cast<const float*>(s_camera.cameraCacheLocation);
        int matches = 0;
        uintptr_t matchedView = 0;
        size_t matchedOffset = 0;
        size_t matchedScanSize = 0;
        for (int pointerOffset = 0; pointerOffset < 0x150 && matches < 8; pointerOffset += 4) {
            const uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(
                reinterpret_cast<const unsigned char*>(renderer) + pointerOffset);
            if (candidate < 0x10000) continue;
            MEMORY_BASIC_INFORMATION memory = {};
            if (!VirtualQuery(reinterpret_cast<const void*>(candidate), &memory, sizeof(memory)) ||
                memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
            const size_t available = static_cast<size_t>(
                reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize - candidate);
            const size_t scanSize = (std::min)(available, static_cast<size_t>(0x2000));
            for (size_t offset = 0; offset + sizeof(float) * 3 <= scanSize; offset += 4) {
                const auto* value = reinterpret_cast<const float*>(candidate + offset);
                if (fabsf(value[0] - expected[0]) <= 5.0f &&
                    fabsf(value[1] - expected[1]) <= 5.0f &&
                    fabsf(value[2] - expected[2]) <= 5.0f) {
                    Log("[Camera] Renderer camera candidate: renderer=%p pointer=+0x%X -> %p+0x%zX "
                        "value=(%.1f,%.1f,%.1f)", renderer, pointerOffset,
                        reinterpret_cast<void*>(candidate), offset, value[0], value[1], value[2]);
                    ++matches;
                    if (!matchedView) {
                        matchedView = candidate;
                        matchedOffset = offset;
                        matchedScanSize = scanSize;
                    }
                }
            }
        }
        if (!matches) Log("[Camera] Renderer camera probe found no direct location match");
        if (matchedView) {
            const auto* rotation = reinterpret_cast<const int32_t*>(s_camera.cameraCacheRotation);
            int rotationMatches = 0;
            const size_t viewScanSize = (std::min)(matchedScanSize, static_cast<size_t>(0x1000));
            for (size_t offset = 0; offset + sizeof(int32_t) * 3 <= viewScanSize; offset += 4) {
                const auto* value = reinterpret_cast<const int32_t*>(matchedView + offset);
                if (value[0] == rotation[0] && value[1] == rotation[1] && value[2] == rotation[2]) {
                    Log("[Camera] Renderer rotation candidate: view=%p+0x%zX value=(%d,%d,%d)",
                        reinterpret_cast<void*>(matchedView), offset, value[0], value[1], value[2]);
                    if (++rotationMatches >= 8) break;
                }
            }
            const size_t dumpStart = matchedOffset >= 0x80 ? matchedOffset - 0x80 : 0;
            const size_t dumpEnd = (std::min)(matchedOffset + 0x100, viewScanSize);
            for (size_t offset = dumpStart; offset < dumpEnd; offset += 16) {
                const auto* value = reinterpret_cast<const float*>(matchedView + offset);
                Log("[Camera] View data +0x%zX: %.6g %.6g %.6g %.6g", offset,
                    value[0], value[1], value[2], value[3]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[Camera] Renderer camera probe exception: 0x%08X", GetExceptionCode());
    }
}

static void __fastcall HookedRenderScene(void* renderer) {
    static std::atomic<uint64_t> renderCount{0};
    const uint64_t count = ++renderCount;
    if (count == 1) ProbeRendererCamera(renderer);
    if (count == 1 || count % 600 == 0) {
        Log("[Camera] RenderScene heartbeat: count=%llu renderer=%p frame=%llu",
            count, renderer, xr::FrameLoop::Instance().GetFrameCount());
    }
    s_originalRenderScene(renderer);
}

static bool InstallRenderSceneProbe(uintptr_t moduleBase) {
    constexpr uintptr_t kRenderSceneRva = 0x00468220;
    static constexpr unsigned char signature[] = {
        0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55,
        0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8
    };
    const uintptr_t target = moduleBase + kRenderSceneRva;
    if (memcmp(reinterpret_cast<const void*>(target), signature, sizeof(signature)) != 0) {
        Log("[Camera] RenderScene signature mismatch at %p", reinterpret_cast<void*>(target));
        return false;
    }
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), &HookedRenderScene,
                                     reinterpret_cast<void**>(&s_originalRenderScene));
    if (status == MH_OK) status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        Log("[Camera] RenderScene probe hook failed: %s", MH_StatusToString(status));
        s_originalRenderScene = nullptr;
        return false;
    }
    s_renderSceneTarget = target;
    Log("[Camera] Hooked RenderScene probe at %p (RVA 0x%llX)",
        reinterpret_cast<void*>(target), static_cast<unsigned long long>(kRenderSceneRva));
    return true;
}

bool InstallViewportDrawHook(uintptr_t target) {
    if (!target || s_viewportDrawTarget) return s_viewportDrawTarget == target;
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), &HookedViewportDraw,
                                    reinterpret_cast<void**>(&s_originalViewportDraw));
    if (status == MH_OK) status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        Log("[Camera] Viewport Draw hook failed at %p: %s", reinterpret_cast<void*>(target),
            MH_StatusToString(status));
        s_originalViewportDraw = nullptr;
        return false;
    }
    s_viewportDrawTarget = target;
    Log("[Camera] Hooked runtime-discovered GameViewportClient::Draw at %p",
        reinterpret_cast<void*>(target));
    return true;
}

static DWORD WINAPI ScannerThread(LPVOID) {
    Log("[Camera] Scanner thread started");
    Log("[Camera] Waiting 3 seconds for game initialization...");
    Sleep(3000);

    HMODULE gameModule = GetModuleHandleA("BorderlandsGOTY.exe");
    if (!gameModule) {
        Log("[Camera] ERROR: BorderlandsGOTY.exe module not found");
        return 1;
    }

    MODULEINFO modInfo = {};
    GetModuleInformation(GetCurrentProcess(), gameModule, &modInfo, sizeof(modInfo));
    Log("[Camera] Game module: %p, size: 0x%X", modInfo.lpBaseOfDll, modInfo.SizeOfImage);

    // Phase 1: Scan for UE3 globals and camera cache
    bool success = ScanForUE3Globals(&s_globals, &s_camera);

    if (success) {
        Log("[Camera] UE3 globals discovered successfully");

        // Log discovered addresses
        if (s_globals.gNamesValid) {
            Log("[Camera]   GNames: %p (count=%d, stringOffset=0x%X)",
                (void*)s_globals.gNamesAddress, s_globals.gNameCount, s_globals.gNameStringOffset);
        }
        if (s_globals.gObjectsValid) {
            Log("[Camera]   GObjects: %p", (void*)s_globals.gObjectsAddress);
        }
        if (s_camera.found) {
            s_cameraFound = true;
            Log("[Camera]   Camera cache: controller=%p, location=+0x%X, rotation=+0x%X",
                (void*)s_camera.controllerAddress, s_camera.locationOffset, s_camera.rotationOffset);
        }
        const bool drawHookInstalled = InstallViewportDrawHook(FindViewportDrawTarget(s_globals));
        InstallRenderSceneProbe(reinterpret_cast<uintptr_t>(gameModule));
        if (drawHookInstalled && s_camera.found) {
            config::Get().same_frame_stereo = false;
            Log("[Camera] Camera and Draw boundary validated, but double Draw is disabled: "
                "GameViewportClient reentry corrupts the UE3 heap");
        } else {
            config::Get().same_frame_stereo = false;
            Log("[Camera] Synced stereo boundary unavailable; using AFR capture fallback");
        }
    } else {
        Log("[Camera] WARNING: UE3 global scan did not find reliable candidates");
        Log("[Camera] Falling back to pattern-based camera discovery...");

        // Fallback: scan for known instruction patterns
        uintptr_t modBase = (uintptr_t)modInfo.lpBaseOfDll;
        DWORD modSize = modInfo.SizeOfImage;

        // Look for common UE3 camera patterns
        // Pattern: "CalcCamera" string reference in code
        auto results = ScanPattern(modBase, modSize,
            "48 89 ?? 24 ?? 48 89 ?? 24 ?? 48 89 ?? 24 ?? 57 48 83 EC ?? 48 8B ??");
        Log("[Camera] Found %zu potential camera-related code patterns", results.size());

        // Pattern: Float constants that look like FOV values (45.0, 60.0, 75.0, 90.0)
        // These are common in camera setup code
        float fovValues[] = { 45.0f, 60.0f, 75.0f, 90.0f, 100.0f, 120.0f };
        for (float fov : fovValues) {
            uint32_t fovBits;
            memcpy(&fovBits, &fov, 4);
            char pattern[32];
            snprintf(pattern, sizeof(pattern), "%02X %02X %02X %02X", fovBits & 0xFF,
                     (fovBits >> 8) & 0xFF, (fovBits >> 16) & 0xFF, (fovBits >> 24) & 0xFF);
            auto fovResults = ScanPattern(modBase, modSize, pattern);
            if (!fovResults.empty()) {
                Log("[Camera] Found FOV %.0f constant at %zu locations (first: %p)",
                    fov, fovResults.size(), (void*)fovResults[0].address);
            }
        }
    }

    Log("[Camera] Scanner thread complete");
    return 0;
}

void StartScanner() {
    CreateThread(nullptr, 0, ScannerThread, nullptr, 0, nullptr);
}

}} // namespace bl1gotyvr::camera
