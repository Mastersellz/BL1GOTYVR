#include "CameraHook.hpp"
#include "UE3Scanner.hpp"
#include "SignatureScanner.hpp"
#include "../core/VRMod.hpp"
#include "../hook/MinHookWrapper.hpp"
#include "../xr/FrameLoop.hpp"
#include "../xr/OpenXRContext.hpp"
#include "../config/Config.hpp"
#include "../input/InputHook.hpp"
#include "../input/XRInput.hpp"
#include "../input/WeaponAimSystem.hpp"
#include "../player/ArmIKSystem.hpp"
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "psapi.lib")

namespace bl1gotyvr { namespace camera {

static std::atomic<bool> s_cameraFound{false};
static std::atomic<bool> s_stereoReady{false};
static std::atomic<bool> s_nativeMultiviewActive{false};
static std::atomic<uint64_t> s_nativeMultiviewGeneration{0};
static float s_location[3] = {};
static int32_t s_rotation[3] = {};
static UE3Globals s_globals = {};
static CameraInfo s_camera = {};
static SRWLOCK s_globalsLock = SRWLOCK_INIT;
static SRWLOCK s_cameraLock = SRWLOCK_INIT;
static std::atomic<bool> s_cameraRefreshRequested{false};
static std::atomic<bool> s_visibilityInventoryRefreshRequested{false};

static CameraInfo GetCameraSnapshot() {
    CameraInfo camera = {};
    AcquireSRWLockShared(&s_cameraLock);
    camera = s_camera;
    ReleaseSRWLockShared(&s_cameraLock);
    return camera;
}

static bool ControllerHasLivePawn(uintptr_t controller) {
    if (controller < 0x10000) return false;
    uintptr_t pawn = 0;
    uintptr_t controllerBackReference = 0;
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
               reinterpret_cast<const void*>(controller + 0x260),
               &pawn, sizeof(pawn), &read) && read == sizeof(pawn) &&
        pawn >= 0x10000 &&
        ReadProcessMemory(GetCurrentProcess(),
               reinterpret_cast<const void*>(pawn + 0x26C),
               &controllerBackReference, sizeof(controllerBackReference), &read) &&
        read == sizeof(controllerBackReference) &&
        controllerBackReference == controller;
}

using ViewportDrawFn = void(__fastcall*)(void*, void*, void*);
static ViewportDrawFn s_originalViewportDraw = nullptr;
static uintptr_t s_viewportDrawTarget = 0;
using RenderSceneFn = void(__fastcall*)(void*);
static RenderSceneFn s_originalRenderScene = nullptr;
static uintptr_t s_renderSceneTarget = 0;
using RenderCommandConstructorFn = void*(__fastcall*)(void*, void*, void*, void*);
using ExecuteRenderCommandFn = void(__fastcall*)(void*);
static RenderCommandConstructorFn s_originalRenderCommandConstructor = nullptr;
static ExecuteRenderCommandFn s_originalExecuteRenderCommand = nullptr;
static bool s_poseReferenceValid = false;
static bool s_poseReferenceSimulated = false;
static float s_poseReferencePosition[3] = {};
static float s_poseReferenceRotation[4] = {};
static std::atomic<bool> s_recenterRequested{true};
static std::atomic<bool> s_gamePitchReferenceValid{false};
static std::atomic<int32_t> s_gamePitchReference{0};
static bool s_aimBasisValid = false;
static float s_aimBasisRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};

struct PendingViewPose {
    bool active = false;
    bool xrViewsValid = false;
    int eye = 0;
    float originalLocation[3] = {};
    float headLocation[3] = {};
    float sourceLocation[3] = {};
    float location[3] = {};
    int32_t rotation[3] = {};
    int32_t baseRotation[3] = {};
    float visualFov = 0.0f;
    float cullingFov = 0.0f;
    float roomOffsetView[3] = {};
    float deltaOrientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    XrView xrViews[2] = {};
    uint64_t armTargetGeneration = 0;
    uint64_t pairSerial = 0;
};
static PendingViewPose s_pendingViewPoses[2];
static SRWLOCK s_pendingViewPoseLock = SRWLOCK_INIT;
static std::atomic<int> s_latestPendingEye{0};

struct CommandPoseEntry {
    void* command = nullptr;
    uint64_t generation = 0;
    PendingViewPose pose = {};
};
static CommandPoseEntry s_commandPoses[64];
static SRWLOCK s_commandPoseLock = SRWLOCK_INIT;
static std::atomic<uint64_t> s_commandPoseSerial{0};
static std::atomic<uint32_t> s_principalRenderWidth{0};
static std::atomic<uint32_t> s_principalRenderHeight{0};

struct RenderPoseAck {
    bool valid = false;
    uint64_t pairSerial = 0;
    int eye = 0;
    uint64_t commandGeneration = 0;
};
static RenderPoseAck s_renderPoseAcks[2];
static SRWLOCK s_renderPoseAckLock = SRWLOCK_INIT;

static void StoreRenderPoseAck(const PendingViewPose& pose, uint64_t generation) {
    if (!pose.active || !pose.pairSerial || pose.eye < 0 || pose.eye > 1 || !generation) return;
    AcquireSRWLockExclusive(&s_renderPoseAckLock);
    s_renderPoseAcks[pose.eye] = {true, pose.pairSerial, pose.eye, generation};
    ReleaseSRWLockExclusive(&s_renderPoseAckLock);
}

bool ConsumeRenderPoseAcknowledgement(uint64_t pairSerial, int eye) {
    if (!pairSerial || eye < 0 || eye > 1) return false;
    bool matched = false;
    AcquireSRWLockExclusive(&s_renderPoseAckLock);
    auto& ack = s_renderPoseAcks[eye];
    if (ack.valid && ack.pairSerial == pairSerial && ack.eye == eye) {
        matched = true;
        ack = {};
    }
    ReleaseSRWLockExclusive(&s_renderPoseAckLock);
    return matched;
}

bool GetPrincipalRenderExtent(uint32_t& width, uint32_t& height) {
    width = s_principalRenderWidth.load(std::memory_order_acquire);
    height = s_principalRenderHeight.load(std::memory_order_acquire);
    return width != 0 && height != 0;
}

struct CompletedNativeFrameSlot {
    bool valid = false;
    uint64_t generation = 0;
    XrView renderedViews[2] = {};
};
static CompletedNativeFrameSlot s_completedNativeFrame;
static SRWLOCK s_completedNativeFrameLock = SRWLOCK_INIT;

static void StoreCommandPose(void* command, const PendingViewPose& pose) {
    if (!command || !pose.active) return;

    AcquireSRWLockExclusive(&s_commandPoseLock);
    CommandPoseEntry* selected = &s_commandPoses[0];
    for (auto& entry : s_commandPoses) {
        if (entry.command == command || !entry.command) {
            selected = &entry;
            break;
        }
        if (entry.generation < selected->generation) selected = &entry;
    }
    selected->command = command;
    selected->generation = s_commandPoseSerial.fetch_add(1) + 1;
    selected->pose = pose;
    ReleaseSRWLockExclusive(&s_commandPoseLock);
}

static bool GetCommandPose(void* command, PendingViewPose& pose, uint64_t& generation) {
    bool found = false;
    generation = 0;
    AcquireSRWLockShared(&s_commandPoseLock);
    for (const auto& entry : s_commandPoses) {
        if (entry.command != command) continue;
        pose = entry.pose;
        generation = entry.generation;
        found = true;
        break;
    }
    ReleaseSRWLockShared(&s_commandPoseLock);
    return found;
}

static void RemoveCommandPose(void* command) {
    AcquireSRWLockExclusive(&s_commandPoseLock);
    for (auto& entry : s_commandPoses) {
        if (entry.command != command) continue;
        entry.command = nullptr;
        entry.generation = 0;
        break;
    }
    ReleaseSRWLockExclusive(&s_commandPoseLock);
}

static void MultiplyMatrix(const float left[16], const float right[16], float result[16]) {
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result[row * 4 + column] =
                left[row * 4] * right[column] +
                left[row * 4 + 1] * right[4 + column] +
                left[row * 4 + 2] * right[8 + column] +
                left[row * 4 + 3] * right[12 + column];
        }
    }
}

static bool InvertMatrix(const float matrix[16], float inverse[16]) {
    float augmented[4][8] = {};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            augmented[row][column] = matrix[row * 4 + column];
        augmented[row][4 + row] = 1.0f;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row) {
            if (fabsf(augmented[row][column]) > fabsf(augmented[pivot][column])) pivot = row;
        }
        if (fabsf(augmented[pivot][column]) < 1.0e-6f) return false;
        if (pivot != column) {
            for (int index = 0; index < 8; ++index)
                std::swap(augmented[pivot][index], augmented[column][index]);
        }
        const float divisor = augmented[column][column];
        for (int index = 0; index < 8; ++index) augmented[column][index] /= divisor;
        for (int row = 0; row < 4; ++row) {
            if (row == column) continue;
            const float factor = augmented[row][column];
            for (int index = 0; index < 8; ++index)
                augmented[row][index] -= factor * augmented[column][index];
        }
    }
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            inverse[row * 4 + column] = augmented[row][4 + column];
    return true;
}

static void BuildViewRotation(const int32_t rotation[3], float matrix[16]) {
    constexpr float kUnisToRadians = 6.2831853071795864769f / 65536.0f;
    const float pitch = rotation[0] * kUnisToRadians;
    const float yaw = rotation[1] * kUnisToRadians;
    const float roll = rotation[2] * kUnisToRadians;
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cr = cosf(roll), sr = sinf(roll);
    const float right[3] = { -sy, cy, 0.0f };
    const float up[3] = { -sp * cy, -sp * sy, cp };
    const float forward[3] = { cp * cy, cp * sy, sp };
    const float rolledRight[3] = {
        right[0] * cr + up[0] * sr,
        right[1] * cr + up[1] * sr,
        right[2] * cr + up[2] * sr
    };
    const float rolledUp[3] = {
        up[0] * cr - right[0] * sr,
        up[1] * cr - right[1] * sr,
        up[2] * cr - right[2] * sr
    };
    memset(matrix, 0, sizeof(float) * 16);
    for (int axis = 0; axis < 3; ++axis) {
        matrix[axis * 4] = rolledRight[axis];
        matrix[axis * 4 + 1] = rolledUp[axis];
        matrix[axis * 4 + 2] = forward[axis];
    }
    matrix[15] = 1.0f;
}

static void BuildHeadRotation(const float orientation[4], float matrix[16]) {
    const float length = sqrtf(orientation[0] * orientation[0] +
        orientation[1] * orientation[1] + orientation[2] * orientation[2] +
        orientation[3] * orientation[3]);
    memset(matrix, 0, sizeof(float) * 16);
    matrix[15] = 1.0f;
    if (length <= 1.0e-6f) {
        matrix[0] = matrix[5] = matrix[10] = 1.0f;
        return;
    }
    const float qx = orientation[0] / length;
    const float qy = orientation[1] / length;
    const float qz = orientation[2] / length;
    const float qw = orientation[3] / length;
    const float xrRotation[3][3] = {
        {1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy - qz * qw),
         2.0f * (qx * qz + qy * qw)},
        {2.0f * (qx * qy + qz * qw), 1.0f - 2.0f * (qx * qx + qz * qz),
         2.0f * (qy * qz - qx * qw)},
        {2.0f * (qx * qz - qy * qw), 2.0f * (qy * qz + qx * qw),
         1.0f - 2.0f * (qx * qx + qy * qy)}
    };
    constexpr float basis[3] = {1.0f, 1.0f, -1.0f};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            matrix[row * 4 + column] =
                basis[row] * xrRotation[row][column] * basis[column];
}

static void ViewRotationToRotator(const float matrix[16], int32_t rotation[3]) {
    constexpr float kRadiansToUnis = 65536.0f / 6.2831853071795864769f;
    const float forward[3] = {matrix[2], matrix[6], matrix[10]};
    const float pitch = asinf(std::clamp(forward[2], -1.0f, 1.0f));
    const float yaw = atan2f(forward[1], forward[0]);
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float unrolledRight[3] = {-sy, cy, 0.0f};
    const float unrolledUp[3] = {-sp * cy, -sp * sy, cp};
    const float actualRight[3] = {matrix[0], matrix[4], matrix[8]};
    const float sinRoll = actualRight[0] * unrolledUp[0] +
        actualRight[1] * unrolledUp[1] + actualRight[2] * unrolledUp[2];
    const float cosRoll = actualRight[0] * unrolledRight[0] +
        actualRight[1] * unrolledRight[1] + actualRight[2] * unrolledRight[2];
    rotation[0] = static_cast<int32_t>(lroundf(pitch * kRadiansToUnis));
    rotation[1] = static_cast<int32_t>(lroundf(yaw * kRadiansToUnis));
    rotation[2] = static_cast<int32_t>(lroundf(atan2f(sinRoll, cosRoll) * kRadiansToUnis));
}

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

// Quaternion rotation (from BioShockVR2 ue_math.h)
static void quat_rotate(float qx, float qy, float qz, float qw, const float v[3], float out[3]) {
    float t[3] = {2.0f * (qy * v[2] - qz * v[1]), 2.0f * (qz * v[0] - qx * v[2]),
                  2.0f * (qx * v[1] - qy * v[0])};
    out[0] = v[0] + qw * t[0] + (qy * t[2] - qz * t[1]);
    out[1] = v[1] + qw * t[1] + (qz * t[0] - qx * t[2]);
    out[2] = v[2] + qw * t[2] + (qx * t[1] - qy * t[0]);
}

// XR to UE coordinate conversion (from BioShockVR2 ue_math.h)
// XR LOCAL: right +X, up +Y, forward -Z
// UE: forward +X, right +Y, up +Z
static void xr_to_ue(const float v[3], float out[3]) {
    out[0] = -v[2]; // XR -Z (forward) -> UE +X
    out[1] = v[0];  // XR +X (right)   -> UE +Y
    out[2] = v[1];  // XR +Y (up)      -> UE +Z
}

bool IsCameraFound() { return s_cameraFound && s_viewportDrawTarget != 0; }
bool IsNativeMultiviewActive() { return s_nativeMultiviewActive.load(); }
bool ConsumeCompletedNativeMultiviewFrame(CompletedNativeMultiviewFrame& frame) {
    bool available = false;
    AcquireSRWLockExclusive(&s_completedNativeFrameLock);
    if (s_completedNativeFrame.valid) {
        frame.generation = s_completedNativeFrame.generation;
        frame.renderedViews[0] = s_completedNativeFrame.renderedViews[0];
        frame.renderedViews[1] = s_completedNativeFrame.renderedViews[1];
        s_completedNativeFrame.valid = false;
        s_completedNativeFrame.generation = 0;
        available = true;
    }
    ReleaseSRWLockExclusive(&s_completedNativeFrameLock);
    return available;
}
void DiscardCompletedNativeMultiviewFrame() {
    AcquireSRWLockExclusive(&s_completedNativeFrameLock);
    s_completedNativeFrame.valid = false;
    s_completedNativeFrame.generation = 0;
    ReleaseSRWLockExclusive(&s_completedNativeFrameLock);
}
void SuspendNativeMultiview() {
    s_nativeMultiviewActive.store(false, std::memory_order_release);
    DiscardCompletedNativeMultiviewFrame();
}
float* GetCameraLocation() {
    const CameraInfo camera = GetCameraSnapshot();
    return camera.found ? reinterpret_cast<float*>(camera.cameraCacheLocation) : s_location;
}
int32_t* GetCameraRotation() {
    const CameraInfo camera = GetCameraSnapshot();
    return camera.found ? reinterpret_cast<int32_t*>(camera.cameraCacheRotation) : s_rotation;
}
UE3Globals GetUE3GlobalsSnapshot() {
    UE3Globals globals = {};
    AcquireSRWLockShared(&s_globalsLock);
    globals = s_globals;
    ReleaseSRWLockShared(&s_globalsLock);
    return globals;
}

void RequestVisibilityInventoryRefresh() {
    s_visibilityInventoryRefreshRequested.store(true, std::memory_order_release);
}

void RequestPlayerIdentityRefresh() {
    s_visibilityInventoryRefreshRequested.store(true, std::memory_order_release);
}

// Request camera recenter (call from input handler or command system)
void RequestRecenter() {
    s_recenterRequested.store(true, std::memory_order_release);
    Log("[Camera] Recenter requested");
}

static void __fastcall HookedViewportDraw(void* viewportClient, void* viewport, void* canvas) {
    static thread_local bool dispatching = false;
    static std::atomic<uint64_t> drawCount{0};
    if (dispatching || !s_originalViewportDraw) {
        if (s_originalViewportDraw) s_originalViewportDraw(viewportClient, viewport, canvas);
        return;
    }

    const CameraInfo camera = GetCameraSnapshot();
    const uint64_t count = ++drawCount;
    if (count == 1 || count % 600 == 0) {
        Log("[Camera] GameViewportClient::Draw heartbeat: count=%llu this=%p camera=%d",
            count, viewportClient, camera.found);
    }

    auto& frameLoop = xr::FrameLoop::Instance();
    auto& openXR = xr::OpenXRContext::Instance();
    const bool simulatedPose = frameLoop.IsDesktopTestMode();
    if (!camera.found || (!simulatedPose &&
        (!openXR.IsInitialized() || !s_stereoReady.load(std::memory_order_acquire)))) {
        s_originalViewportDraw(viewportClient, viewport, canvas);
        return;
    }

    xr::StereoRenderTicket renderTicket = {};
    const bool realPoseValid = !simulatedPose && frameLoop.AcquireRenderTicket(renderTicket);
    if (!simulatedPose && !realPoseValid) {
        s_originalViewportDraw(viewportClient, viewport, canvas);
        return;
    }

    const int eye = simulatedPose ? frameLoop.GetRenderEye() : renderTicket.eye;
    float originalLocation[3] = {};
    int32_t originalRotation[3] = {};
    float appliedLocation[3] = {};
    int32_t appliedRotation[3] = {};
    float roomOffsetView[3] = {};
    float relative[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float originalFov = 0.0f;
    float visualFov = 0.0f;
    float cullingFov = 0.0f;
    bool cameraStateSaved = false;
    bool cameraOffsetApplied = false;
    dispatching = true;
    if (camera.found && camera.cameraCacheLocation && camera.cameraCacheRotation) {
        __try {
            auto* location = reinterpret_cast<float*>(camera.cameraCacheLocation);
            auto* rotation = reinterpret_cast<int32_t*>(camera.cameraCacheRotation);
            auto* fov = reinterpret_cast<float*>(camera.cameraFov);
            memcpy(originalLocation, location, sizeof(originalLocation));
            memcpy(originalRotation, rotation, sizeof(originalRotation));
            originalFov = *fov;
            cameraStateSaved = true;

            if (!s_gamePitchReferenceValid.load(std::memory_order_acquire)) {
                s_gamePitchReference.store(originalRotation[0], std::memory_order_relaxed);
                s_gamePitchReferenceValid.store(true, std::memory_order_release);
                Log("[Camera] Game pitch locked at %d Unis", originalRotation[0]);
            }
            rotation[0] = s_gamePitchReference.load(std::memory_order_relaxed);

            if (!simulatedPose) {
                if (eye == 0) {
                    renderTicket.baseCameraValid = true;
                    memcpy(renderTicket.baseLocation, originalLocation,
                           sizeof(renderTicket.baseLocation));
                    memcpy(renderTicket.baseRotation, rotation,
                           sizeof(renderTicket.baseRotation));
                    renderTicket.baseFov = config::Get().fov_degrees;
                } else if (!renderTicket.baseCameraValid) {
                    __leave;
                }
                memcpy(location, renderTicket.baseLocation, sizeof(renderTicket.baseLocation));
                memcpy(rotation, renderTicket.baseRotation, sizeof(renderTicket.baseRotation));
                *fov = renderTicket.baseFov;
            }

            constexpr float kUnisToRadians = 6.2831853071795864769f / 65536.0f;
            const float gamePitch = rotation[0] * kUnisToRadians;
            const float gameYaw = rotation[1] * kUnisToRadians;

            float simulatedPosition[3] = {};
            float simulatedRotation[4] = {};
            if (simulatedPose)
                frameLoop.GetDesktopHeadPose(simulatedPosition, simulatedRotation);
            const float* headPosition = simulatedPose
                ? simulatedPosition : renderTicket.headPosition;
            const float* headRotation = simulatedPose
                ? simulatedRotation : renderTicket.headRotation;

            if (s_poseReferenceValid && s_poseReferenceSimulated != simulatedPose)
                s_poseReferenceValid = false;
            if (eye == 0 && s_recenterRequested.exchange(false, std::memory_order_acq_rel))
                s_poseReferenceValid = false;
            if (!s_poseReferenceValid) {
                memcpy(s_poseReferencePosition, headPosition, sizeof(s_poseReferencePosition));
                memcpy(s_poseReferenceRotation, headRotation, sizeof(s_poseReferenceRotation));
                memcpy(s_aimBasisRotation, headRotation, sizeof(s_aimBasisRotation));
                s_aimBasisValid = true;
                s_poseReferenceSimulated = simulatedPose;
                s_poseReferenceValid = true;
                Log("[Camera] 6DoF tracking reference captured at pair=%llu",
                    static_cast<unsigned long long>(renderTicket.pairSerial));
            }

            const float inverseReference[4] = {
                -s_poseReferenceRotation[0], -s_poseReferenceRotation[1],
                -s_poseReferenceRotation[2], s_poseReferenceRotation[3]
            };
            RelativeQuaternion(s_poseReferenceRotation, headRotation, relative);

            float renderedEyePosition[3] = {};
            if (simulatedPose) {
                const float sideMeters = (eye == 0 ? -1.0f : 1.0f) *
                    config::Get().ipd_mm * 0.0005f;
                const float localEye[3] = {sideMeters, 0.0f, 0.0f};
                float rotatedEye[3] = {};
                quat_rotate(headRotation[0], headRotation[1], headRotation[2], headRotation[3],
                            localEye, rotatedEye);
                for (int axis = 0; axis < 3; ++axis)
                    renderedEyePosition[axis] = headPosition[axis] + rotatedEye[axis];
            } else {
                renderedEyePosition[0] = renderTicket.views[eye].pose.position.x;
                renderedEyePosition[1] = renderTicket.views[eye].pose.position.y;
                renderedEyePosition[2] = renderTicket.views[eye].pose.position.z;
            }
            const float trackingDelta[3] = {
                renderedEyePosition[0] - s_poseReferencePosition[0],
                renderedEyePosition[1] - s_poseReferencePosition[1],
                renderedEyePosition[2] - s_poseReferencePosition[2]
            };
            float referenceLocalXr[3] = {};
            quat_rotate(inverseReference[0], inverseReference[1], inverseReference[2],
                        inverseReference[3], trackingDelta, referenceLocalXr);
            const float positionScale = 100.0f * config::Get().positional_scale;
            roomOffsetView[0] = referenceLocalXr[0] * positionScale;
            roomOffsetView[1] = referenceLocalXr[1] * positionScale;
            roomOffsetView[2] = -referenceLocalXr[2] * positionScale;

            const float* armCameraLocation = simulatedPose
                ? originalLocation : renderTicket.baseLocation;
            if (simulatedPose || eye == 0) {
                renderTicket.armTargetGeneration =
                    player::ArmIKSystem::Instance().UpdateTargets(
                        armCameraLocation, gamePitch, gameYaw, s_poseReferencePosition,
                        s_poseReferenceRotation);
            }
            if (renderTicket.rightAimValid) {
                if (!s_aimBasisValid) {
                    memcpy(s_aimBasisRotation, s_poseReferenceRotation,
                           sizeof(s_aimBasisRotation));
                    s_aimBasisValid = true;
                    Log("[Camera] Aim calibration basis anchored: "
                        "q=(%.3f,%.3f,%.3f,%.3f)",
                        s_aimBasisRotation[0], s_aimBasisRotation[1],
                        s_aimBasisRotation[2], s_aimBasisRotation[3]);
                }
                float relativeAim[4] = {};
                RelativeQuaternion(s_aimBasisRotation,
                    renderTicket.rightAimRotation, relativeAim);
                float localForward[3] = {};
                input::BuildCalibratedLocalForward(
                    renderTicket.aimPitchDegrees, renderTicket.aimYawDegrees,
                    localForward);
                constexpr float kDegreesToRadians = 0.01745329251994329577f;
                const float trimPitch = renderTicket.aimPitchDegrees * kDegreesToRadians;
                const float trimYaw = renderTicket.aimYawDegrees * kDegreesToRadians;
                const float localUp[3] = {
                    -sinf(trimYaw) * sinf(trimPitch),
                    cosf(trimPitch),
                    cosf(trimYaw) * sinf(trimPitch)};
                float xrForward[3] = {};
                float xrUp[3] = {};
                quat_rotate(relativeAim[0], relativeAim[1], relativeAim[2],
                            relativeAim[3], localForward, xrForward);
                quat_rotate(relativeAim[0], relativeAim[1], relativeAim[2],
                            relativeAim[3], localUp, xrUp);
                const float ueForward[3] = {-xrForward[2], xrForward[0], xrForward[1]};
                const float ueUp[3] = {-xrUp[2], xrUp[0], xrUp[1]};
                const float pitchSine = sinf(gamePitch);
                const float pitchCosine = cosf(gamePitch);
                const float pitchedForward[3] = {
                    pitchCosine * ueForward[0] - pitchSine * ueForward[2],
                    ueForward[1],
                    pitchSine * ueForward[0] + pitchCosine * ueForward[2]
                };
                const float yawSine = sinf(gameYaw);
                const float yawCosine = cosf(gameYaw);
                const float worldForward[3] = {
                    yawCosine * pitchedForward[0] - yawSine * pitchedForward[1],
                    yawSine * pitchedForward[0] + yawCosine * pitchedForward[1],
                    pitchedForward[2]
                };
                const float pitchedUp[3] = {
                    pitchCosine * ueUp[0] - pitchSine * ueUp[2],
                    ueUp[1],
                    pitchSine * ueUp[0] + pitchCosine * ueUp[2]
                };
                const float worldUp[3] = {
                    yawCosine * pitchedUp[0] - yawSine * pitchedUp[1],
                    yawSine * pitchedUp[0] + yawCosine * pitchedUp[1],
                    pitchedUp[2]
                };
                const float nativeCameraForward[3] = {
                    yawCosine * pitchCosine,
                    yawSine * pitchCosine,
                    pitchSine
                };
                const float nativeCameraUp[3] = {
                    -yawCosine * pitchSine,
                    -yawSine * pitchSine,
                    pitchCosine
                };
                const float trackingWeaponDelta[3] = {
                    renderTicket.rightAimPosition[0] - s_poseReferencePosition[0],
                    renderTicket.rightAimPosition[1] - s_poseReferencePosition[1],
                    renderTicket.rightAimPosition[2] - s_poseReferencePosition[2]
                };
                float referenceLocalWeapon[3] = {};
                quat_rotate(inverseReference[0], inverseReference[1],
                            inverseReference[2], inverseReference[3],
                            trackingWeaponDelta, referenceLocalWeapon);
                const float weaponPositionScale =
                    100.0f * config::Get().weapon_position_scale;
                const float weaponViewOffset[3] = {
                    -referenceLocalWeapon[2] * weaponPositionScale,
                    referenceLocalWeapon[0] * weaponPositionScale,
                    referenceLocalWeapon[1] * weaponPositionScale
                };
                const float pitchedWeaponOffset[3] = {
                    pitchCosine * weaponViewOffset[0] -
                        pitchSine * weaponViewOffset[2],
                    weaponViewOffset[1],
                    pitchSine * weaponViewOffset[0] +
                        pitchCosine * weaponViewOffset[2]
                };
                const float worldWeaponPosition[3] = {
                    armCameraLocation[0] + yawCosine * pitchedWeaponOffset[0] -
                        yawSine * pitchedWeaponOffset[1],
                    armCameraLocation[1] + yawSine * pitchedWeaponOffset[0] +
                        yawCosine * pitchedWeaponOffset[1],
                    armCameraLocation[2] + pitchedWeaponOffset[2]
                };
                input::InputHook::Instance().SetCanonicalWeaponPose(
                    worldWeaponPosition, worldForward, worldUp,
                    armCameraLocation, nativeCameraForward, nativeCameraUp);
                input::WeaponAimSystem::Instance().UpdateDirection(
                    armCameraLocation, worldForward);
            } else {
                input::InputHook::Instance().ClearCanonicalWeaponPose();
                input::WeaponAimSystem::Instance().InvalidateDirection();
            }

            float correctedFov = simulatedPose
                ? config::Get().fov_degrees : renderTicket.baseFov;
            float scaleX = 0.0f, scaleY = 0.0f, offsetX = 0.0f, offsetY = 0.0f;
            if (!simulatedPose && renderTicket.projectionCorrection) {
                openXR.GetProjectionCrop(renderTicket.views[eye],
                    renderTicket.renderAspect,
                    scaleX, scaleY, offsetX, offsetY, correctedFov);
            }
            visualFov = correctedFov;
            cullingFov = visualFov;
            *fov = cullingFov;
            memcpy(appliedLocation, location, sizeof(appliedLocation));
            memcpy(appliedRotation, rotation, sizeof(appliedRotation));
            cameraOffsetApplied = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cameraOffsetApplied = false;
        }
    }

    if (cameraOffsetApplied) {
        AcquireSRWLockExclusive(&s_pendingViewPoseLock);
        PendingViewPose& pendingPose = s_pendingViewPoses[eye];
        pendingPose = {};
        pendingPose.active = true;
        pendingPose.xrViewsValid = realPoseValid;
        pendingPose.eye = eye;
        pendingPose.pairSerial = renderTicket.pairSerial;
        memcpy(pendingPose.originalLocation, originalLocation,
               sizeof(pendingPose.originalLocation));
        memcpy(pendingPose.headLocation, appliedLocation,
               sizeof(pendingPose.headLocation));
        memcpy(pendingPose.sourceLocation, appliedLocation,
               sizeof(pendingPose.sourceLocation));
        memcpy(pendingPose.location, appliedLocation, sizeof(pendingPose.location));
        memcpy(pendingPose.rotation, appliedRotation, sizeof(pendingPose.rotation));
        memcpy(pendingPose.baseRotation, originalRotation, sizeof(pendingPose.baseRotation));
        pendingPose.visualFov = visualFov;
        pendingPose.cullingFov = cullingFov;
        memcpy(pendingPose.roomOffsetView, roomOffsetView,
                sizeof(pendingPose.roomOffsetView));
        memcpy(pendingPose.deltaOrientation, relative,
                sizeof(pendingPose.deltaOrientation));
        if (pendingPose.xrViewsValid) {
            pendingPose.xrViews[0] = renderTicket.views[0];
            pendingPose.xrViews[1] = renderTicket.views[1];
        }
        ReleaseSRWLockExclusive(&s_pendingViewPoseLock);
        s_latestPendingEye.store(eye, std::memory_order_release);

        static std::atomic<uint64_t> poseLogCount{0};
        const uint64_t poseCount = ++poseLogCount;
        if (poseCount == 1 || poseCount % 600 == 0) {
            Log("[Camera] HMD pose delta: pair=%llu eye=%d meters=(%.3f,%.3f,%.3f) "
                "viewUU=(%.1f,%.1f,%.1f)",
                static_cast<unsigned long long>(renderTicket.pairSerial), eye,
                renderTicket.headPosition[0] - s_poseReferencePosition[0],
                renderTicket.headPosition[1] - s_poseReferencePosition[1],
                renderTicket.headPosition[2] - s_poseReferencePosition[2],
                roomOffsetView[0], roomOffsetView[1], roomOffsetView[2]);
        }
    }

    input::InputHook::Instance().ApplyRightHand(eye);
    input::InputHook::Instance().ApplyLeftHand(eye);
    s_originalViewportDraw(viewportClient, viewport, canvas);
    // Publish new controller targets only after both AFR eyes consumed the
    // previous palette. UpdateSkelPose then prepares one coherent palette for
    // the next pair instead of advancing only the right eye.
    if (simulatedPose || eye == 1) {
        player::ArmIKSystem::Instance().SetRenderContext(
            simulatedPose ? count : renderTicket.pairSerial,
            renderTicket.armTargetGeneration);
    }
    input::InputHook::Instance().Restore();
    if (realPoseValid) {
        if (cameraOffsetApplied) frameLoop.CommitRenderedEye(renderTicket);
        else frameLoop.AbortStereoPair();
    }

    if (cameraStateSaved) {
        __try {
            static bool loggedFirstEye = false;
            if (cameraOffsetApplied && !loggedFirstEye) {
                Log("[Camera] Coherent AER write: pair=%llu eye=%d loc=(%.1f,%.1f,%.1f) "
                    "rot=(%d,%d,%d)",
                    static_cast<unsigned long long>(renderTicket.pairSerial), eye,
                    appliedLocation[0], appliedLocation[1], appliedLocation[2],
                    appliedRotation[0], appliedRotation[1], appliedRotation[2]);
                loggedFirstEye = true;
            }
            memcpy(reinterpret_cast<void*>(camera.cameraCacheLocation),
                   originalLocation, sizeof(originalLocation));
            memcpy(reinterpret_cast<void*>(camera.cameraCacheRotation),
                   originalRotation, sizeof(originalRotation));
            *reinterpret_cast<float*>(camera.cameraFov) = originalFov;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    dispatching = false;
}

static void ProbeRendererCamera(void* renderer) {
    const CameraInfo camera = GetCameraSnapshot();
    if (!renderer || !camera.found) return;
    __try {
        const auto* expected = reinterpret_cast<const float*>(camera.cameraCacheLocation);
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
            const auto* rotation = reinterpret_cast<const int32_t*>(camera.cameraCacheRotation);
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
            // UE3 keeps projection and inverse-projection matrices before the
            // camera origin. Include both in the one-shot diagnostic dump.
            const size_t dumpStart = matchedOffset >= 0x200 ? matchedOffset - 0x200 : 0;
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

struct FinalViewBackup {
    float translatedView[16] = {};
    float translatedViewProjection[16] = {};
    float inverseTranslatedViewProjection[16] = {};
    float negativeOrigin[4] = {};
    float viewProjection[16] = {};
    float inverseViewProjection[16] = {};
    float origin[4] = {};
};

static bool UpdateViewFrustum(uintptr_t viewAddress, const float viewProjection[16]) {
    // UE3 stores five world-space frustum planes followed by SIMD-friendly
    // transposes. They are consumed during RenderScene after command creation.
    const int combinations[5][2] = {
        {0, 1}, {0, -1}, {1, -1}, {1, 1}, {2, -1}
    };
    float planes[5][4] = {};
    for (int plane = 0; plane < 5; ++plane) {
        const int column = combinations[plane][0];
        const float sign = static_cast<float>(combinations[plane][1]);
        float value[4] = {};
        for (int row = 0; row < 4; ++row)
            value[row] = viewProjection[row * 4 + 3] +
                sign * viewProjection[row * 4 + column];
        const float length = sqrtf(value[0] * value[0] + value[1] * value[1] +
                                   value[2] * value[2]);
        if (!std::isfinite(length) || length < 1.0e-6f) return false;
        planes[plane][0] = -value[0] / length;
        planes[plane][1] = -value[1] / length;
        planes[plane][2] = -value[2] / length;
        planes[plane][3] = value[3] / length;
    }

    memcpy(reinterpret_cast<void*>(viewAddress + 0x2D0), planes, sizeof(planes));
    for (int component = 0; component < 4; ++component) {
        float packed[4] = {
            planes[0][component], planes[1][component],
            planes[2][component], planes[3][component]
        };
        memcpy(reinterpret_cast<void*>(viewAddress + 0x350 + component * 0x10),
               packed, sizeof(packed));
        for (float& value : packed) value = planes[4][component];
        memcpy(reinterpret_cast<void*>(viewAddress + 0x390 + component * 0x10),
               packed, sizeof(packed));
    }
    return true;
}

static bool ApplyPoseToView(uintptr_t viewAddress, const PendingViewPose& pose) {
    FinalViewBackup backup = {};
    __try {
        if (viewAddress < 0x10000) return false;
        auto* projection = reinterpret_cast<float*>(viewAddress + 0xC0);
        auto* translatedView = reinterpret_cast<float*>(viewAddress + 0x130);
        auto* translatedViewProjection = reinterpret_cast<float*>(viewAddress + 0x170);
        auto* inverseTranslatedViewProjection = reinterpret_cast<float*>(viewAddress + 0x1B0);
        auto* negativeOrigin = reinterpret_cast<float*>(viewAddress + 0x1F0);
        auto* viewProjection = reinterpret_cast<float*>(viewAddress + 0x200);
        auto* inverseViewProjection = reinterpret_cast<float*>(viewAddress + 0x280);
        auto* origin = reinterpret_cast<float*>(viewAddress + 0x2C0);
        const float originError =
            fabsf(origin[0] - pose.originalLocation[0]) +
            fabsf(origin[1] - pose.originalLocation[1]) +
            fabsf(origin[2] - pose.originalLocation[2]);
        const float sourceOriginError =
            fabsf(origin[0] - pose.sourceLocation[0]) +
            fabsf(origin[1] - pose.sourceLocation[1]) +
            fabsf(origin[2] - pose.sourceLocation[2]);
        const bool validProjection = projection[0] > 0.1f && projection[5] > 0.1f &&
                                     fabsf(projection[11] - 1.0f) < 0.01f &&
                                     fabsf(projection[15]) < 0.01f;
        if ((std::min)(originError, sourceOriginError) > 10.0f || !validProjection)
            return false;

        // UE3 used a wide FOV only to build visibility. Restore the visual
        // projection before rendering so the original stable 6DoF path and
        // headset crop remain unchanged.
        if (pose.cullingFov > pose.visualFov && pose.visualFov > 1.0f &&
            pose.cullingFov < 179.0f) {
            constexpr float kDegreesToRadians = 0.01745329251994329577f;
            const float visualTan = tanf(pose.visualFov * 0.5f * kDegreesToRadians);
            const float cullingTan = tanf(pose.cullingFov * 0.5f * kDegreesToRadians);
            if (visualTan > 0.001f && cullingTan > visualTan) {
                const float restoreScale = cullingTan / visualTan;
                projection[0] *= restoreScale;
                projection[5] *= restoreScale;
            }
        }

        // Measure the game's actual rendered-image half-tangents from the
        // FSceneView projection so the FrameLoop crop matches the true source
        // FOV instead of the eye-derived estimate.
        const float sourceTanX = 1.0f / projection[0];
        const float sourceTanY = 1.0f / projection[5];
        if (sourceTanX > 0.05f && sourceTanY > 0.05f)
            xr::OpenXRContext::Instance().SetSourceProjectionTans(sourceTanX, sourceTanY);

        float inverseProjection[16] = {};
        if (pose.eye >= 0 && pose.eye < 2) {
            // Keep UE3's validated symmetric projection. Writing OpenXR's
            // asymmetric offsets here removes the world render in this build;
            // FrameLoop performs the matching non-destructive projection crop.
            if (!InvertMatrix(projection, inverseProjection)) return false;
            memcpy(reinterpret_cast<void*>(viewAddress + 0x240), inverseProjection,
                   sizeof(inverseProjection));
            if (*reinterpret_cast<const int*>(viewAddress + 0x448) != 0)
                memcpy(reinterpret_cast<void*>(viewAddress + 0x450), projection,
                       sizeof(float) * 16);
        }

        memcpy(backup.translatedView, translatedView, sizeof(backup.translatedView));
        memcpy(backup.translatedViewProjection, translatedViewProjection,
               sizeof(backup.translatedViewProjection));
        memcpy(backup.inverseTranslatedViewProjection, inverseTranslatedViewProjection,
               sizeof(backup.inverseTranslatedViewProjection));
        memcpy(backup.negativeOrigin, negativeOrigin, sizeof(backup.negativeOrigin));
        memcpy(backup.viewProjection, viewProjection, sizeof(backup.viewProjection));
        memcpy(backup.inverseViewProjection, inverseViewProjection,
               sizeof(backup.inverseViewProjection));
        memcpy(backup.origin, origin, sizeof(backup.origin));

        float deltaOrientation[4] = {
            pose.deltaOrientation[0], pose.deltaOrientation[1],
            pose.deltaOrientation[2], pose.deltaOrientation[3]
        };
        const float quaternionLength = sqrtf(
            deltaOrientation[0] * deltaOrientation[0] +
            deltaOrientation[1] * deltaOrientation[1] +
            deltaOrientation[2] * deltaOrientation[2] +
            deltaOrientation[3] * deltaOrientation[3]);
        if (quaternionLength <= 1.0e-6f) return false;
        for (float& component : deltaOrientation) component /= quaternionLength;

        const float qx = deltaOrientation[0];
        const float qy = deltaOrientation[1];
        const float qz = deltaOrientation[2];
        const float qw = deltaOrientation[3];
        const float xrRotation[3][3] = {
            {1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy - qz * qw),
             2.0f * (qx * qz + qy * qw)},
            {2.0f * (qx * qy + qz * qw), 1.0f - 2.0f * (qx * qx + qz * qz),
             2.0f * (qy * qz - qx * qw)},
            {2.0f * (qx * qz - qy * qw), 2.0f * (qy * qz + qx * qw),
             1.0f - 2.0f * (qx * qx + qy * qy)}
        };
        constexpr float basis[3] = {1.0f, 1.0f, -1.0f};
        float headRotation[16] = {};
        headRotation[15] = 1.0f;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                headRotation[row * 4 + column] =
                    basis[row] * xrRotation[row][column] * basis[column];
            }
        }

        // CameraCache is no longer modified for culling, so preserve UE3's exact
        // source basis. Reconstructing it from integer rotators loses camera
        // adjustments and creates a small orbit when the head rotates.
        const float* baseTranslatedView = backup.translatedView;
        const float worldOffset[3] = {
            pose.roomOffsetView[0] * baseTranslatedView[0] +
                pose.roomOffsetView[1] * baseTranslatedView[1] +
                pose.roomOffsetView[2] * baseTranslatedView[2],
            pose.roomOffsetView[0] * baseTranslatedView[4] +
                pose.roomOffsetView[1] * baseTranslatedView[5] +
                pose.roomOffsetView[2] * baseTranslatedView[6],
            pose.roomOffsetView[0] * baseTranslatedView[8] +
                pose.roomOffsetView[1] * baseTranslatedView[9] +
                pose.roomOffsetView[2] * baseTranslatedView[10]
        };
        for (float component : worldOffset)
            if (!std::isfinite(component)) return false;

        float translatedWithOffset[16] = {};
        memcpy(translatedWithOffset, baseTranslatedView,
                sizeof(translatedWithOffset));
        float newTranslatedView[16] = {};
        MultiplyMatrix(translatedWithOffset, headRotation, newTranslatedView);
        float newTranslatedViewProjection[16] = {};
        float newInverseTranslatedViewProjection[16] = {};
        MultiplyMatrix(newTranslatedView, projection, newTranslatedViewProjection);
        if (!InvertMatrix(newTranslatedViewProjection,
                          newInverseTranslatedViewProjection)) return false;

        float fullView[16] = {};
        memcpy(fullView, baseTranslatedView, sizeof(fullView));
        const float newOrigin[3] = {
            backup.origin[0] + worldOffset[0],
            backup.origin[1] + worldOffset[1],
            backup.origin[2] + worldOffset[2]
        };
        fullView[12] = -(newOrigin[0] * fullView[0] + newOrigin[1] * fullView[4] +
                         newOrigin[2] * fullView[8]);
        fullView[13] = -(newOrigin[0] * fullView[1] + newOrigin[1] * fullView[5] +
                         newOrigin[2] * fullView[9]);
        fullView[14] = -(newOrigin[0] * fullView[2] + newOrigin[1] * fullView[6] +
                         newOrigin[2] * fullView[10]);
        float rotatedFullView[16] = {};
        MultiplyMatrix(fullView, headRotation, rotatedFullView);
        float newViewProjection[16] = {};
        float newInverseViewProjection[16] = {};
        MultiplyMatrix(rotatedFullView, projection, newViewProjection);
        if (!InvertMatrix(newViewProjection, newInverseViewProjection)) return false;
        if (!UpdateViewFrustum(viewAddress, newViewProjection)) return false;

        memcpy(translatedView, newTranslatedView, sizeof(newTranslatedView));
        memcpy(translatedViewProjection, newTranslatedViewProjection,
               sizeof(newTranslatedViewProjection));
        memcpy(inverseTranslatedViewProjection, newInverseTranslatedViewProjection,
               sizeof(newInverseTranslatedViewProjection));
        memcpy(viewProjection, newViewProjection, sizeof(newViewProjection));
        memcpy(inverseViewProjection, newInverseViewProjection,
               sizeof(newInverseViewProjection));
        // RenderScene phase 2 replaces the active matrices from this alternate
        // cache when +0x448 is set. Keep that phase on the same tracked pose.
        if (*reinterpret_cast<const int*>(viewAddress + 0x448) != 0) {
            memcpy(reinterpret_cast<void*>(viewAddress + 0x490), newViewProjection,
                   sizeof(newViewProjection));
            memcpy(reinterpret_cast<void*>(viewAddress + 0x4D0), newTranslatedViewProjection,
                   sizeof(newTranslatedViewProjection));
            memcpy(reinterpret_cast<void*>(viewAddress + 0x510), newInverseViewProjection,
                   sizeof(newInverseViewProjection));
            memcpy(reinterpret_cast<void*>(viewAddress + 0x550),
                   newInverseTranslatedViewProjection,
                   sizeof(newInverseTranslatedViewProjection));
        }
        for (int axis = 0; axis < 3; ++axis) {
            negativeOrigin[axis] = backup.negativeOrigin[axis] - worldOffset[axis];
            origin[axis] = newOrigin[axis];
        }
        origin[3] = 1.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int ApplyFinalViewPose(void* renderer, uintptr_t& firstViewAddress,
                               PendingViewPose& pose, int& commandViewCount,
                               bool& commandPoseMatched, uint64_t& commandGeneration) {
    firstViewAddress = 0;
    commandViewCount = 0;
    commandPoseMatched = false;
    commandGeneration = 0;
    if (!renderer) return 0;
    commandPoseMatched = GetCommandPose(renderer, pose, commandGeneration);
    if (!commandPoseMatched) {
        return 0;
    }
    if (!pose.active) return 0;

    uintptr_t viewArray = 0;
    __try {
        const auto* bytes = reinterpret_cast<const unsigned char*>(renderer);
        viewArray = *reinterpret_cast<const uintptr_t*>(bytes + 0x68);
        commandViewCount = *reinterpret_cast<const int*>(bytes + 0x70);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (viewArray < 0x10000 || commandViewCount <= 0 || commandViewCount > 16) return 0;

    int appliedCount = 0;
    constexpr uintptr_t kCommandViewStride = 0x1750;
    float stereoRotation[16] = {};
    if (commandViewCount == 2) BuildViewRotation(pose.rotation, stereoRotation);
    for (int index = 0; index < commandViewCount; ++index) {
        const uintptr_t viewAddress = viewArray + index * kCommandViewStride;
        PendingViewPose viewPose = pose;
        if (commandViewCount == 2) {
            const float side = (index == 0 ? -1.0f : 1.0f) * config::Get().ipd_mm * 0.05f;
            viewPose.eye = index;
            viewPose.location[0] = pose.headLocation[0] + stereoRotation[0] * side;
            viewPose.location[1] = pose.headLocation[1] + stereoRotation[4] * side;
            viewPose.location[2] = pose.headLocation[2] + stereoRotation[8] * side;
        }
        if (!ApplyPoseToView(viewAddress, viewPose)) continue;
        if (!firstViewAddress) firstViewAddress = viewAddress;
        ++appliedCount;
    }
    return appliedCount;
}

static void __fastcall HookedRenderScene(void* renderer) {
    static std::atomic<uint64_t> renderCount{0};
    const uint64_t count = ++renderCount;
    if (count == 1) {
        ProbeRendererCamera(renderer);
        void* frames[24] = {};
        const USHORT frameCount = CaptureStackBackTrace(1, _countof(frames), frames, nullptr);
        Log("[Camera] RenderScene call stack: frames=%u", frameCount);
        for (USHORT frame = 0; frame < frameCount; ++frame) {
            HMODULE owner = nullptr;
            char ownerPath[MAX_PATH] = "<private>";
            uintptr_t offset = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(frames[frame]), &owner) && owner) {
                GetModuleFileNameA(owner, ownerPath, MAX_PATH);
                offset = reinterpret_cast<uintptr_t>(frames[frame]) -
                         reinterpret_cast<uintptr_t>(owner);
            }
            Log("[Camera]   renderStack[%u]=%p module=%s+0x%llX", frame, frames[frame],
                ownerPath, static_cast<unsigned long long>(offset));
        }
    }
    if (count == 1 || count % 600 == 0) {
        Log("[Camera] RenderScene heartbeat: count=%llu renderer=%p frame=%llu",
            count, renderer, xr::FrameLoop::Instance().GetFrameCount());
    }
    uintptr_t viewAddress = 0;
    PendingViewPose appliedPose = {};
    int commandViewCount = 0;
    bool commandPoseMatched = false;
    uint64_t commandGeneration = 0;
    const int finalPoseCount = ApplyFinalViewPose(
        renderer, viewAddress, appliedPose, commandViewCount,
        commandPoseMatched, commandGeneration);
    if (finalPoseCount > 0) {
        static LONG loggedFinalPoseCounts[3] = {};
        const int logSlot = commandViewCount >= 1 && commandViewCount <= 2
            ? commandViewCount : 0;
        if (InterlockedCompareExchange(&loggedFinalPoseCounts[logSlot], 1, 0) == 0) {
            const bool alternateView = *reinterpret_cast<const int*>(viewAddress + 0x448) != 0;
            Log("[Camera] Final command view pose applied: eye=%d views=%d/%d first=%p alt=%d "
                "loc=(%.1f,%.1f,%.1f) rot=(%d,%d,%d)", appliedPose.eye,
                finalPoseCount, commandViewCount, reinterpret_cast<void*>(viewAddress),
                alternateView, appliedPose.location[0],
                appliedPose.location[1], appliedPose.location[2],
                appliedPose.rotation[0], appliedPose.rotation[1],
                appliedPose.rotation[2]);
        }
    }
    static uint32_t consecutivePoseRejects = 0;
    if (finalPoseCount > 0) {
        consecutivePoseRejects = 0;
    } else if (commandViewCount > 0 && ++consecutivePoseRejects >= 120) {
        s_cameraRefreshRequested.store(true, std::memory_order_release);
        consecutivePoseRejects = 0;
        Log("[Camera] Final views rejected after map transition; camera refresh requested");
    }
    if (count % 600 == 0) {
        Log("[Camera] Final pose status: applied=%d views=%d matched=%d generation=%llu",
            finalPoseCount, commandViewCount, commandPoseMatched,
            static_cast<unsigned long long>(commandGeneration));
    }
    const bool completedNativeMultiview = commandPoseMatched && commandGeneration != 0 &&
        finalPoseCount == 2 && commandViewCount == 2 && appliedPose.xrViewsValid;
    const bool completedAlternateEye = commandPoseMatched && commandGeneration != 0 &&
        finalPoseCount == 1 && commandViewCount == 1 && appliedPose.xrViewsValid;
    s_originalRenderScene(renderer);
    if (completedAlternateEye) {
        xr::FrameLoop::Instance().CaptureWorldBeforeHud(
            appliedPose.pairSerial, appliedPose.eye);
        StoreRenderPoseAck(appliedPose, commandGeneration);
    }
    RemoveCommandPose(renderer);
    if (completedNativeMultiview) {
        AcquireSRWLockExclusive(&s_completedNativeFrameLock);
        if (!s_completedNativeFrame.valid ||
            commandGeneration > s_completedNativeFrame.generation) {
            s_completedNativeFrame.valid = true;
            s_completedNativeFrame.generation = commandGeneration;
            s_completedNativeFrame.renderedViews[0] = appliedPose.xrViews[0];
            s_completedNativeFrame.renderedViews[1] = appliedPose.xrViews[1];
        }
        ReleaseSRWLockExclusive(&s_completedNativeFrameLock);
        s_nativeMultiviewActive.store(true, std::memory_order_release);
        s_nativeMultiviewGeneration.fetch_add(1, std::memory_order_release);
    }
}

static void LogRenderCommandLayout(const char* stage, void* command) {
    if (!command) return;
    __try {
        const auto* bytes = reinterpret_cast<const unsigned char*>(command);
        const uintptr_t familyViews = *reinterpret_cast<const uintptr_t*>(bytes + 0x08);
        const int familyNum = *reinterpret_cast<const int*>(bytes + 0x10);
        const int familyMax = *reinterpret_cast<const int*>(bytes + 0x14);
        const uintptr_t ownedViews = *reinterpret_cast<const uintptr_t*>(bytes + 0x68);
        const int ownedNum = *reinterpret_cast<const int*>(bytes + 0x70);
        const int ownedMax = *reinterpret_cast<const int*>(bytes + 0x74);
        Log("[StereoResearch] %s command=%p family=%p num/max=%d/%d "
            "owned=%p num/max=%d/%d thread=%u", stage, command,
            reinterpret_cast<void*>(familyViews), familyNum, familyMax,
            reinterpret_cast<void*>(ownedViews), ownedNum, ownedMax,
            GetCurrentThreadId());

        const int viewCount = (std::min)(ownedNum, 4);
        for (int index = 0; index < viewCount; ++index) {
            constexpr uintptr_t kViewStride = 0x1750;
            const uintptr_t owned = ownedViews + index * kViewStride;
            const uintptr_t familyPointer = familyViews
                ? *reinterpret_cast<const uintptr_t*>(familyViews + index * sizeof(uintptr_t)) : 0;
            const uintptr_t ownerFamily = *reinterpret_cast<const uintptr_t*>(owned);
            const float* viewport = reinterpret_cast<const float*>(owned + 0x50);
            const int* viewportPixels = reinterpret_cast<const int*>(owned + 0x68);
            const float* derivedRect = reinterpret_cast<const float*>(owned + 0x430);
            Log("[StereoResearch]   view[%d]=%p familyPtr=%p owner=%p "
                "rect=(%.1f,%.1f %.1fx%.1f) pixels=(%d,%d %dx%d) "
                "derived=(%.3f,%.3f,%.3f,%.3f) alt=%d", index,
                reinterpret_cast<void*>(owned), reinterpret_cast<void*>(familyPointer),
                reinterpret_cast<void*>(ownerFamily), viewport[0], viewport[1],
                viewport[2], viewport[3], viewportPixels[0], viewportPixels[1],
                viewportPixels[2], viewportPixels[3], derivedRect[0], derivedRect[1],
                derivedRect[2], derivedRect[3],
                *reinterpret_cast<const int*>(owned + 0x448));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[StereoResearch] %s command layout exception: 0x%08X", stage,
            GetExceptionCode());
    }
}

static void* __fastcall HookedRenderCommandConstructor(void* destination, void* sourceViewFamily,
                                                         void* argument3, void* argument4) {
    void* constructorSource = sourceViewFamily;
    alignas(16) unsigned char stereoFamily[0x60] = {};
    alignas(16) unsigned char stereoSourceViews[2][0x1750] = {};
    uintptr_t stereoViewPointers[2] = {};
    bool stereoSource = false;
    bool principalSource = false;
    PendingViewPose commandPose = {};
    const CameraInfo camera = GetCameraSnapshot();
    const int pendingEye = s_latestPendingEye.load(std::memory_order_acquire);
    AcquireSRWLockShared(&s_pendingViewPoseLock);
    commandPose = s_pendingViewPoses[pendingEye];
    ReleaseSRWLockShared(&s_pendingViewPoseLock);
    if (sourceViewFamily && camera.found) {
        __try {
            const auto* sourceBytes = reinterpret_cast<const unsigned char*>(sourceViewFamily);
            const uintptr_t sourceViews = *reinterpret_cast<const uintptr_t*>(sourceBytes);
            const int sourceViewCount = *reinterpret_cast<const int*>(sourceBytes + 0x08);
            const uintptr_t sourceView = sourceViews
                ? *reinterpret_cast<const uintptr_t*>(sourceViews) : 0;
            if (sourceViewCount == 1 && sourceView >= 0x10000) {
                const auto* origin = reinterpret_cast<const float*>(sourceView + 0x2C0);
                const auto* viewport = reinterpret_cast<const float*>(sourceView + 0x50);
                const auto* cameraLocation = reinterpret_cast<const float*>(
                    camera.cameraCacheLocation);
                const float originError = fabsf(origin[0] - cameraLocation[0]) +
                    fabsf(origin[1] - cameraLocation[1]) +
                    fabsf(origin[2] - cameraLocation[2]);
                const float poseOriginError = fabsf(origin[0] - commandPose.sourceLocation[0]) +
                    fabsf(origin[1] - commandPose.sourceLocation[1]) +
                    fabsf(origin[2] - commandPose.sourceLocation[2]);
                const bool validViewport = std::isfinite(viewport[2]) &&
                    std::isfinite(viewport[3]) && viewport[2] >= 320.0f &&
                    viewport[3] >= 180.0f;
                const float viewportAspect = validViewport
                    ? viewport[2] / viewport[3] : 0.0f;
                const bool cameraMatched = (std::min)(originError, poseOriginError) <= 10.0f;
                const bool principalViewport = validViewport && cameraMatched &&
                    viewportAspect >= 0.5f && viewportAspect <= 3.0f;
                // Camera effects and map transitions can shift the source view before
                // command construction. The render acknowledgement below prevents an
                // unmatched command from ever being submitted as a coherent eye.
                if (principalViewport && commandPose.active) {
                    principalSource = true;
                    static uint32_t loggedPrincipalWidth = 0;
                    static uint32_t loggedPrincipalHeight = 0;
                    const uint32_t principalWidth =
                        static_cast<uint32_t>(viewport[2] + 0.5f);
                    const uint32_t principalHeight =
                        static_cast<uint32_t>(viewport[3] + 0.5f);
                    if (principalWidth != loggedPrincipalWidth ||
                        principalHeight != loggedPrincipalHeight) {
                        Log("[Camera] Principal FSceneView: %ux%u aspect=%.4f "
                            "originError=%.3f poseError=%.3f",
                            principalWidth, principalHeight, viewportAspect,
                            originError, poseOriginError);
                        loggedPrincipalWidth = principalWidth;
                        loggedPrincipalHeight = principalHeight;
                    }
                    s_principalRenderWidth.store(
                        principalWidth, std::memory_order_release);
                    s_principalRenderHeight.store(
                        principalHeight, std::memory_order_release);
                    if (config::Get().same_frame_stereo) {
                        memcpy(stereoFamily, sourceViewFamily, sizeof(stereoFamily));
                        memcpy(stereoSourceViews[0], reinterpret_cast<const void*>(sourceView),
                               sizeof(stereoSourceViews[0]));
                        memcpy(stereoSourceViews[1], reinterpret_cast<const void*>(sourceView),
                               sizeof(stereoSourceViews[1]));
                        const float halfWidth = viewport[2] * 0.5f;
                        for (int eye = 0; eye < 2; ++eye) {
                            auto* eyeViewport = reinterpret_cast<float*>(stereoSourceViews[eye] + 0x50);
                            auto* eyePixels = reinterpret_cast<int*>(stereoSourceViews[eye] + 0x68);
                            eyeViewport[0] = eye == 0 ? viewport[0] : viewport[0] + halfWidth;
                            eyeViewport[1] = viewport[1];
                            eyeViewport[2] = halfWidth;
                            eyeViewport[3] = viewport[3];
                            eyePixels[0] = static_cast<int>(eyeViewport[0]);
                            eyePixels[1] = static_cast<int>(eyeViewport[1]);
                            eyePixels[2] = static_cast<int>(eyeViewport[2]);
                            eyePixels[3] = static_cast<int>(eyeViewport[3]);
                            stereoViewPointers[eye] = reinterpret_cast<uintptr_t>(
                                stereoSourceViews[eye]);
                        }
                        *reinterpret_cast<uintptr_t*>(stereoFamily) =
                            reinterpret_cast<uintptr_t>(stereoViewPointers);
                        *reinterpret_cast<int*>(stereoFamily + 0x08) = 2;
                        *reinterpret_cast<int*>(stereoFamily + 0x0C) = 2;
                        constructorSource = stereoFamily;
                        stereoSource = true;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            constructorSource = sourceViewFamily;
            stereoSource = false;
        }
    }

    void* result = s_originalRenderCommandConstructor(
        destination, constructorSource, argument3, argument4);
    if (principalSource) {
        StoreCommandPose(destination, commandPose);
    }
    if (stereoSource) {
        static LONG loggedStereoSource = 0;
        if (InterlockedCompareExchange(&loggedStereoSource, 1, 0) == 0)
            Log("[StereoResearch] Principal source family expanded to two native view copies");
    }
    static LONG logged = 0;
    if (InterlockedCompareExchange(&logged, 1, 0) == 0)
        LogRenderCommandLayout("constructed", destination);
    return result;
}

static void __fastcall HookedExecuteRenderCommand(void* command) {
    static LONG logged = 0;
    if (InterlockedCompareExchange(&logged, 1, 0) == 0)
        LogRenderCommandLayout("execute", command);
    s_originalExecuteRenderCommand(command);
}

static bool InstallRenderCommandProbes(uintptr_t moduleBase) {
    constexpr uintptr_t kConstructorRva = 0x00445280;
    constexpr uintptr_t kExecuteRva = 0x0046D630;
    static constexpr unsigned char constructorSignature[] = {
        0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x48, 0x08
    };
    static constexpr unsigned char executeSignature[] = {
        0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC
    };
    const uintptr_t constructor = moduleBase + kConstructorRva;
    const uintptr_t execute = moduleBase + kExecuteRva;
    if (memcmp(reinterpret_cast<const void*>(constructor), constructorSignature,
               sizeof(constructorSignature)) != 0 ||
        memcmp(reinterpret_cast<const void*>(execute), executeSignature,
               sizeof(executeSignature)) != 0) {
        Log("[StereoResearch] Render command signature mismatch");
        return false;
    }

    MH_STATUS constructorStatus = MH_CreateHook(
        reinterpret_cast<void*>(constructor), &HookedRenderCommandConstructor,
        reinterpret_cast<void**>(&s_originalRenderCommandConstructor));
    MH_STATUS executeStatus = MH_CreateHook(
        reinterpret_cast<void*>(execute), &HookedExecuteRenderCommand,
        reinterpret_cast<void**>(&s_originalExecuteRenderCommand));
    if (constructorStatus != MH_OK || executeStatus != MH_OK) {
        Log("[StereoResearch] Command probe creation failed: ctor=%s execute=%s",
            MH_StatusToString(constructorStatus), MH_StatusToString(executeStatus));
        return false;
    }
    MH_QueueEnableHook(reinterpret_cast<void*>(constructor));
    MH_QueueEnableHook(reinterpret_cast<void*>(execute));
    const MH_STATUS applyStatus = MH_ApplyQueued();
    if (applyStatus != MH_OK) {
        Log("[StereoResearch] Command probe enable failed: %s",
            MH_StatusToString(applyStatus));
        return false;
    }
    Log("[StereoResearch] Hooked command constructor RVA 0x%llX and execute RVA 0x%llX",
        static_cast<unsigned long long>(kConstructorRva),
        static_cast<unsigned long long>(kExecuteRva));
    return true;
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
    Log("[Camera] Waiting 500 ms for game initialization...");
    Sleep(500);

    HMODULE gameModule = GetModuleHandleA("BorderlandsGOTY.exe");
    if (!gameModule) {
        Log("[Camera] ERROR: BorderlandsGOTY.exe module not found");
        return 1;
    }

    MODULEINFO modInfo = {};
    GetModuleInformation(GetCurrentProcess(), gameModule, &modInfo, sizeof(modInfo));
    Log("[Camera] Game module: %p, size: 0x%X", modInfo.lpBaseOfDll, modInfo.SizeOfImage);

    // Loading screens do not have a live player controller or viewport yet.
    // Retry instead of permanently falling back after a single early scan.
    bool success = false;
    UE3Globals scannedGlobals = {};
    CameraInfo scannedCamera = {};
    for (int attempt = 1; attempt <= 60; ++attempt) {
        scannedGlobals = {};
        scannedCamera = {};
        success = ScanForUE3Globals(&scannedGlobals, &scannedCamera);
        if (success && scannedCamera.found) break;
        if (attempt < 60) {
            Log("[Camera] Live camera not ready (attempt %d/60); retrying in 500 ms", attempt);
            Sleep(500);
        }
    }

    if (success) {
        AcquireSRWLockExclusive(&s_globalsLock);
        s_globals = scannedGlobals;
        ReleaseSRWLockExclusive(&s_globalsLock);
        AcquireSRWLockExclusive(&s_cameraLock);
        s_camera = scannedCamera;
        ReleaseSRWLockExclusive(&s_cameraLock);
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
        if (drawHookInstalled && s_camera.found) {
            config::Get().same_frame_stereo = false;
            const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
            const DWORD moduleSize = modInfo.SizeOfImage;
            input::WeaponAimSystem::Instance().Discover(
                &s_globals, s_camera.controllerAddress, moduleBase, moduleSize);
            player::ArmIKSystem::Instance().RequestInventoryScan();
            const bool commandHookInstalled = InstallRenderCommandProbes(moduleBase);
            const bool renderSceneHookInstalled = InstallRenderSceneProbe(moduleBase);
            s_stereoReady.store(
                commandHookInstalled && renderSceneHookInstalled,
                std::memory_order_release);
            Log("[Camera] Coherent alternate-eye stereo enabled; final FSceneView 6DoF=%s "
                "native multiview=disabled double-Draw=disabled",
                commandHookInstalled && renderSceneHookInstalled ? "enabled" : "FAILED");
        } else {
            config::Get().same_frame_stereo = false;
            Log("[Camera] Stereo camera boundary unavailable; rendering remains unmodified");
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

    Log("[Camera] Scanner initialization complete");
    uint64_t nextIdentityRefreshMs = GetTickCount64() + 2000;
    while (success && s_camera.found) {
        Sleep(250);
        if (s_visibilityInventoryRefreshRequested.exchange(
                false, std::memory_order_acq_rel)) {
            const CameraInfo current = GetCameraSnapshot();
            if (current.found && current.controllerAddress) {
                input::WeaponAimSystem::Instance().Discover(
                    &s_globals, current.controllerAddress,
                    reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll), modInfo.SizeOfImage);
                player::ArmIKSystem::Instance().RequestInventoryScan();
            } else {
                Log("[VisibilityInventory] Identity refresh skipped: no live controller");
            }
        }
        const bool cameraRefreshRequested = s_cameraRefreshRequested.exchange(
            false, std::memory_order_acq_rel);
        const uint64_t nowMs = GetTickCount64();
        const CameraInfo currentCamera = GetCameraSnapshot();
        const input::PlayerIdentitySnapshot currentIdentity =
            input::WeaponAimSystem::Instance().GetPlayerIdentity();
        const bool identityRefreshDue =
            (!ControllerHasLivePawn(currentCamera.controllerAddress) ||
             !currentIdentity.pawnValid ||
             currentIdentity.controller != currentCamera.controllerAddress) &&
            nowMs >= nextIdentityRefreshMs;
        if (!cameraRefreshRequested && !identityRefreshDue) continue;
        nextIdentityRefreshMs = nowMs + 2000;

        CameraInfo refreshed = {};
        if (!RefreshCameraCache(s_globals, &refreshed)) {
            Log("[Camera] Runtime camera refresh failed; retrying after further rejections");
            continue;
        }

        const CameraInfo previous = GetCameraSnapshot();
        if (previous.controllerAddress == refreshed.controllerAddress) {
            if (!currentIdentity.pawnValid ||
                currentIdentity.controller != refreshed.controllerAddress) {
                input::WeaponAimSystem::Instance().Discover(
                    &s_globals, refreshed.controllerAddress,
                    reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll), modInfo.SizeOfImage);
                player::ArmIKSystem::Instance().RequestInventoryScan();
                Log("[Camera] Runtime identity refreshed on existing controller=%p",
                    reinterpret_cast<void*>(refreshed.controllerAddress));
            }
            Log("[Camera] Runtime camera refresh confirmed existing controller=%p",
                reinterpret_cast<void*>(refreshed.controllerAddress));
            continue;
        }
        AcquireSRWLockExclusive(&s_cameraLock);
        s_camera = refreshed;
        ReleaseSRWLockExclusive(&s_cameraLock);
        AcquireSRWLockExclusive(&s_pendingViewPoseLock);
        s_pendingViewPoses[0] = {};
        s_pendingViewPoses[1] = {};
        ReleaseSRWLockExclusive(&s_pendingViewPoseLock);
s_recenterRequested.store(true, std::memory_order_release);
        s_gamePitchReferenceValid.store(false, std::memory_order_release);
        s_aimBasisValid = false;
        xr::FrameLoop::Instance().AbortStereoPair();
        input::WeaponAimSystem::Instance().Discover(
            &s_globals, refreshed.controllerAddress,
            reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll), modInfo.SizeOfImage);
        player::ArmIKSystem::Instance().RequestRescan();
        Log("[Camera] Runtime camera refreshed: controller=%p -> %p; 6DoF recentered",
            reinterpret_cast<void*>(previous.controllerAddress),
            reinterpret_cast<void*>(refreshed.controllerAddress));
    }
    return 0;
}

void StartScanner() {
    CreateThread(nullptr, 0, ScannerThread, nullptr, 0, nullptr);
}

}} // namespace bl1gotyvr::camera
