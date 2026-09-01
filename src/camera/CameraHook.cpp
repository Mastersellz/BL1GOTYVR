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
#include <cfloat>
#include <cmath>
#include <cstring>

#pragma comment(lib, "psapi.lib")

namespace bl1gotyvr { namespace camera {

static void ProbeLocalVehicle(const input::PlayerIdentitySnapshot& identity);
static bool GetVehicleSeatWorld(float output[3], int32_t rotation[3]);
static void UpdateVehicleLifecycleFast(const CameraInfo& camera);
static DWORD WINAPI VehicleAnchorProbeThread(void* parameter);

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
static std::atomic<bool> s_downedFirstPersonActive{false};
static std::atomic<bool> s_externalViewFirstPersonActive{false};
static std::atomic<bool> s_transientFirstPersonActionActive{false};
static SRWLOCK s_firstPersonCameraLock = SRWLOCK_INIT;
static bool s_firstPersonCameraValid = false;
static uintptr_t s_firstPersonCameraPawn = 0;
static float s_firstPersonCameraOffset[3] = {};
static float s_firstPersonCameraWorldLocation[3] = {};
static int32_t s_firstPersonCameraRotation[3] = {};
static bool s_downedCameraAnchorValid = false;
static uintptr_t s_downedCameraAnchorPawn = 0;
static float s_downedCameraAnchorLocation[3] = {};
static float s_downedCameraAnchorPawnLocation[3] = {};

static CameraInfo GetCameraSnapshot() {
    CameraInfo camera = {};
    AcquireSRWLockShared(&s_cameraLock);
    camera = s_camera;
    ReleaseSRWLockShared(&s_cameraLock);
    return camera;
}

struct CameraTArray64 {
    uintptr_t data = 0;
    int32_t count = 0;
    int32_t capacity = 0;
};

static bool CameraRead(uintptr_t address, void* output, size_t size) {
    SIZE_T bytesRead = 0;
    return address >= 0x10000 && output && size &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                          output, size, &bytesRead) && bytesRead == size;
}

static bool CameraReadName(const UE3Globals& globals, int32_t index,
                           char* output, size_t capacity) {
    if (!globals.gNamesValid || globals.gNameStringOffset < 0 || index < 0 ||
        !output || capacity < 2) return false;
    CameraTArray64 names = {};
    if (!CameraRead(globals.gNamesAddress, &names, sizeof(names)) ||
        index >= names.count || names.count > names.capacity || !names.data) return false;
    uintptr_t entry = 0;
    if (!CameraRead(names.data + static_cast<uintptr_t>(index) * sizeof(uintptr_t),
                    &entry, sizeof(entry)) || !entry) return false;
    for (size_t offset = 0; offset + 1 < capacity; ++offset) {
        if (!CameraRead(entry + globals.gNameStringOffset + offset,
                        output + offset, 1)) return false;
        if (output[offset] == '\0') return offset > 0;
    }
    output[capacity - 1] = '\0';
    return false;
}

static bool CameraReadObjectName(const UE3Globals& globals, uintptr_t object,
                                 char* output, size_t capacity) {
    int32_t index = -1;
    return globals.gObjectNameOffset >= 0 && object >= 0x10000 &&
        CameraRead(object + globals.gObjectNameOffset, &index, sizeof(index)) &&
        CameraReadName(globals, index, output, capacity);
}

static bool CameraReadClassName(const UE3Globals& globals, uintptr_t object,
                                char* output, size_t capacity) {
    uintptr_t classObject = 0;
    return globals.gObjectClassOffset >= 0 && object >= 0x10000 &&
        CameraRead(object + globals.gObjectClassOffset, &classObject,
                   sizeof(classObject)) && classObject >= 0x10000 &&
        CameraReadObjectName(globals, classObject, output, capacity);
}

static int CameraClassDistance(uintptr_t derivedClass, uintptr_t targetClass) {
    uintptr_t current = derivedClass;
    for (int depth = 0; depth < 64 && current >= 0x10000; ++depth) {
        if (current == targetClass) return depth;
        uintptr_t superClass = 0;
        if (!CameraRead(current + 0x78, &superClass, sizeof(superClass)) ||
            superClass == current) break;
        current = superClass;
    }
    return -1;
}

static bool FindCameraPropertyOffset(const UE3Globals& globals, uintptr_t objectClass,
                                     const char* propertyName, int32_t& output) {
    CameraTArray64 objects = {};
    if (!propertyName || objectClass < 0x10000 ||
        !CameraRead(globals.gObjectsAddress, &objects, sizeof(objects)) ||
        !objects.data || objects.count <= 0 || objects.count > objects.capacity) return false;
    int bestDistance = 65;
    int32_t bestOffset = -1;
    for (int32_t index = 0; index < objects.count; ++index) {
        uintptr_t object = 0;
        char name[128] = {};
        if (!CameraRead(objects.data + static_cast<uintptr_t>(index) * sizeof(uintptr_t),
                        &object, sizeof(object)) || object < 0x10000 ||
            !CameraReadObjectName(globals, object, name, sizeof(name)) ||
            strcmp(name, propertyName) != 0) continue;
        uintptr_t ownerClass = 0;
        if (globals.gObjectNameOffset < 8 ||
            !CameraRead(object + globals.gObjectNameOffset - 8, &ownerClass,
                        sizeof(ownerClass))) continue;
        const int distance = CameraClassDistance(objectClass, ownerClass);
        int32_t propertyOffset = -1;
        if (distance < 0 || distance >= bestDistance ||
            !CameraRead(object + 0x84, &propertyOffset, sizeof(propertyOffset)) ||
            propertyOffset <= 0 || propertyOffset > 0x10000) continue;
        bestDistance = distance;
        bestOffset = propertyOffset;
    }
    output = bestOffset;
    return bestOffset > 0;
}

static std::atomic<uintptr_t> s_vehiclePawn{0};
static std::atomic<uintptr_t> s_vehicleComponent{0};
static std::atomic<int> s_vehicleSeatBone{-1};
static std::atomic<int> s_vehicleAnchorKind{0};
static std::atomic<int32_t> s_vehicleLocationOffset{-1};
static std::atomic<bool> s_vehicleAnchorReady{false};
static std::atomic<bool> s_vehicleBoneProbeComplete{false};
static std::atomic<bool> s_vehicleArmIkWasEnabled{false};
static std::atomic<uint64_t> s_vehicleExitRecoveryUntilMs{0};
static std::atomic<bool> s_vehicleEnterPending{false};
static std::atomic<bool> s_vehicleAnchorProbeRunning{false};
static std::atomic<uint64_t> s_vehicleExitCandidateSinceMs{0};
static SRWLOCK s_vehiclePoseLock = SRWLOCK_INIT;
static bool s_vehiclePoseValid = false;
static float s_vehiclePoseLocation[3] = {};
static int32_t s_vehiclePoseRotation[3] = {};
static bool s_vehicleAnchorLocalValid = false;
static float s_vehicleAnchorLocal[3] = {};

static void ClearVehiclePose() {
    AcquireSRWLockExclusive(&s_vehiclePoseLock);
    s_vehiclePoseValid = false;
    s_vehicleAnchorLocalValid = false;
    ReleaseSRWLockExclusive(&s_vehiclePoseLock);
}

static void PublishVehicleAnchorLocal(const float local[3]) {
    AcquireSRWLockExclusive(&s_vehiclePoseLock);
    memcpy(s_vehicleAnchorLocal, local, sizeof(s_vehicleAnchorLocal));
    s_vehicleAnchorLocalValid = true;
    ReleaseSRWLockExclusive(&s_vehiclePoseLock);
}

static bool ReadVehicleAnchorLocal(float local[3]) {
    bool valid = false;
    AcquireSRWLockShared(&s_vehiclePoseLock);
    valid = s_vehicleAnchorLocalValid;
    if (valid) memcpy(local, s_vehicleAnchorLocal, sizeof(s_vehicleAnchorLocal));
    ReleaseSRWLockShared(&s_vehiclePoseLock);
    return valid;
}

static void PublishVehiclePose(const float location[3], const int32_t rotation[3]) {
    AcquireSRWLockExclusive(&s_vehiclePoseLock);
    memcpy(s_vehiclePoseLocation, location, sizeof(s_vehiclePoseLocation));
    memcpy(s_vehiclePoseRotation, rotation, sizeof(s_vehiclePoseRotation));
    s_vehiclePoseValid = true;
    ReleaseSRWLockExclusive(&s_vehiclePoseLock);
}

static bool ReadVehiclePose(float location[3], int32_t rotation[3]) {
    bool valid = false;
    AcquireSRWLockShared(&s_vehiclePoseLock);
    valid = s_vehiclePoseValid;
    if (valid) {
        memcpy(location, s_vehiclePoseLocation, sizeof(s_vehiclePoseLocation));
        memcpy(rotation, s_vehiclePoseRotation, sizeof(s_vehiclePoseRotation));
    }
    ReleaseSRWLockShared(&s_vehiclePoseLock);
    return valid;
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
    bool vehicleAnchor = false;
    bool firstPersonOverride = false;
    bool allowFirstPersonRecovery = false;
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
bool IsVehicleCameraActive() {
    return s_vehiclePawn.load(std::memory_order_acquire) != 0;
}
bool IsDownedFirstPersonActive() {
    return s_downedFirstPersonActive.load(std::memory_order_acquire);
}
bool IsTransientFirstPersonActionActive() {
    return s_transientFirstPersonActionActive.load(std::memory_order_acquire);
}

static void ClearCommandPoses() {
    AcquireSRWLockExclusive(&s_commandPoseLock);
    for (auto& entry : s_commandPoses) entry = {};
    ReleaseSRWLockExclusive(&s_commandPoseLock);
}
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

static bool GetPawnWorldLocation(const input::PlayerIdentitySnapshot& identity,
                                 float output[3]) {
    if (!identity.pawnValid || identity.pawn < 0x10000 || !output) return false;
    const player::ComponentInventoryStatus inventory =
        player::ArmIKSystem::Instance().GetComponentInventory();
    const player::ComponentInventoryEntry* fallback = nullptr;
    for (size_t index = 0; index < inventory.count; ++index) {
        const player::ComponentInventoryEntry& entry = inventory.entries[index];
        if (entry.outer != identity.pawn || entry.component < 0x10000 ||
            entry.localToWorldOffset <= 0) continue;
        if (entry.role == player::ComponentRole::PawnBody) {
            fallback = &entry;
            break;
        }
        if (!fallback && entry.role == player::ComponentRole::ProbableFirstPersonArms)
            fallback = &entry;
    }
    if (!fallback) return false;

    float matrix[16] = {};
    if (!CameraRead(fallback->component + fallback->localToWorldOffset,
                    matrix, sizeof(matrix)) ||
        !std::isfinite(matrix[12]) || !std::isfinite(matrix[13]) ||
        !std::isfinite(matrix[14]) || fabsf(matrix[15] - 1.0f) > 0.1f)
        return false;
    output[0] = matrix[12];
    output[1] = matrix[13];
    output[2] = matrix[14];
    return true;
}

static bool ApplyDownedFirstPersonOverride(
        const input::PlayerIdentitySnapshot& identity,
        const float gameLocation[3], const int32_t gameRotation[3],
        float outputLocation[3], int32_t outputRotation[3]) {
    float pawnLocation[3] = {};
    const bool pawnLocationValid = GetPawnWorldLocation(identity, pawnLocation);
    if (!identity.pawnValid || !pawnLocationValid) {
        s_downedFirstPersonActive.store(false, std::memory_order_release);
        s_externalViewFirstPersonActive.store(false, std::memory_order_release);
        s_transientFirstPersonActionActive.store(false, std::memory_order_release);
        AcquireSRWLockExclusive(&s_firstPersonCameraLock);
        s_downedCameraAnchorValid = false;
        ReleaseSRWLockExclusive(&s_firstPersonCameraLock);
        return false;
    }

    bool referenceValid = false;
    float expectedLocation[3] = {};
    int32_t expectedRotation[3] = {};
    AcquireSRWLockShared(&s_firstPersonCameraLock);
    referenceValid = s_firstPersonCameraValid &&
        s_firstPersonCameraPawn == identity.pawn;
    if (referenceValid) {
        for (int axis = 0; axis < 3; ++axis)
            expectedLocation[axis] = pawnLocation[axis] + s_firstPersonCameraOffset[axis];
        memcpy(expectedRotation, s_firstPersonCameraRotation,
               sizeof(expectedRotation));
    }
    ReleaseSRWLockShared(&s_firstPersonCameraLock);
    if (!referenceValid) {
        if (identity.weaponValid) {
            AcquireSRWLockExclusive(&s_firstPersonCameraLock);
            s_firstPersonCameraValid = true;
            s_firstPersonCameraPawn = identity.pawn;
            for (int axis = 0; axis < 3; ++axis)
                s_firstPersonCameraOffset[axis] = gameLocation[axis] - pawnLocation[axis];
            memcpy(s_firstPersonCameraWorldLocation, gameLocation,
                   sizeof(s_firstPersonCameraWorldLocation));
            memcpy(s_firstPersonCameraRotation, gameRotation,
                   sizeof(s_firstPersonCameraRotation));
            ReleaseSRWLockExclusive(&s_firstPersonCameraLock);
        }
        s_transientFirstPersonActionActive.store(false, std::memory_order_release);
        return false;
    }

    const float dx = gameLocation[0] - expectedLocation[0];
    const float dy = gameLocation[1] - expectedLocation[1];
    const float dz = gameLocation[2] - expectedLocation[2];
    const float cameraDisplacement = sqrtf(dx * dx + dy * dy + dz * dz);
    auto& weaponAim = input::WeaponAimSystem::Instance();
    const bool injured = weaponAim.IsPlayerInjured();
    const bool phaseWalk = weaponAim.IsPhaseWalkActive();
    bool active = s_downedFirstPersonActive.load(std::memory_order_acquire);
    constexpr float kThirdPersonCameraDisplacementUu = 50.0f;
    constexpr float kFirstPersonRecoveryDisplacementUu = 20.0f;
    constexpr float kStableReferenceDisplacementUu = 10.0f;
    if (active && !injured &&
        !s_externalViewFirstPersonActive.load(std::memory_order_acquire) &&
        cameraDisplacement <= kFirstPersonRecoveryDisplacementUu) {
        active = false;
        s_downedFirstPersonActive.store(false, std::memory_order_release);
        AcquireSRWLockExclusive(&s_firstPersonCameraLock);
        s_downedCameraAnchorValid = false;
        ReleaseSRWLockExclusive(&s_firstPersonCameraLock);
        Log("[Camera] Downed first-person override ended");
    }
    if (!active && (injured || cameraDisplacement >= kThirdPersonCameraDisplacementUu)) {
        active = true;
        AcquireSRWLockExclusive(&s_firstPersonCameraLock);
        s_downedCameraAnchorValid = true;
        s_downedCameraAnchorPawn = identity.pawn;
        memcpy(s_downedCameraAnchorLocation, s_firstPersonCameraWorldLocation,
               sizeof(s_downedCameraAnchorLocation));
        memcpy(s_downedCameraAnchorPawnLocation, pawnLocation,
               sizeof(s_downedCameraAnchorPawnLocation));
        ReleaseSRWLockExclusive(&s_firstPersonCameraLock);
        s_downedFirstPersonActive.store(true, std::memory_order_release);
        Log("[Camera] Downed first-person override enabled (native=%d camera delta %.1f UU)",
            injured, cameraDisplacement);
    }
    const bool previousTransientAction =
        s_transientFirstPersonActionActive.load(std::memory_order_acquire);
    const bool transientAction = phaseWalk || (!active && !identity.weaponValid &&
        (previousTransientAction || cameraDisplacement >= 5.0f));
    s_transientFirstPersonActionActive.store(
        transientAction, std::memory_order_release);
    if (transientAction != previousTransientAction) {
        Log("[Camera] Transient first-person action %s (camera delta %.1f UU)",
            transientAction ? "started" : "ended", cameraDisplacement);
    }
    if (active) {
        AcquireSRWLockShared(&s_firstPersonCameraLock);
        if (s_downedCameraAnchorValid && s_downedCameraAnchorPawn == identity.pawn) {
            outputLocation[0] = s_downedCameraAnchorLocation[0] +
                pawnLocation[0] - s_downedCameraAnchorPawnLocation[0];
            outputLocation[1] = s_downedCameraAnchorLocation[1] +
                pawnLocation[1] - s_downedCameraAnchorPawnLocation[1];
            outputLocation[2] = s_downedCameraAnchorLocation[2];
        } else {
            memcpy(outputLocation, expectedLocation, sizeof(expectedLocation));
        }
        ReleaseSRWLockShared(&s_firstPersonCameraLock);
        memcpy(outputRotation, expectedRotation, sizeof(expectedRotation));
        return true;
    }
    if (identity.weaponValid &&
        cameraDisplacement < kStableReferenceDisplacementUu) {
        AcquireSRWLockExclusive(&s_firstPersonCameraLock);
        s_firstPersonCameraValid = true;
        s_firstPersonCameraPawn = identity.pawn;
        for (int axis = 0; axis < 3; ++axis)
            s_firstPersonCameraOffset[axis] = gameLocation[axis] - pawnLocation[axis];
        memcpy(s_firstPersonCameraWorldLocation, gameLocation,
               sizeof(s_firstPersonCameraWorldLocation));
        memcpy(s_firstPersonCameraRotation, gameRotation,
               sizeof(s_firstPersonCameraRotation));
        ReleaseSRWLockExclusive(&s_firstPersonCameraLock);
    }
    return false;
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
    if (simulatedPose || frameLoop.GetRenderEye() == 0)
        UpdateVehicleLifecycleFast(camera);
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
    int32_t interactionRotation[3] = {};
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
            memcpy(interactionRotation, originalRotation, sizeof(interactionRotation));
            originalFov = *fov;
            cameraStateSaved = true;

            if (!s_gamePitchReferenceValid.load(std::memory_order_acquire)) {
                s_gamePitchReference.store(originalRotation[0], std::memory_order_relaxed);
                s_gamePitchReferenceValid.store(true, std::memory_order_release);
                Log("[Camera] Game pitch locked at %d Unis", originalRotation[0]);
            }
            rotation[0] = s_gamePitchReference.load(std::memory_order_relaxed);

            float vehicleSeat[3] = {};
            int32_t vehicleRotation[3] = {};
            bool vehicleAnchorActive = false;
            if (simulatedPose || IsVehicleCameraActive()) {
                vehicleAnchorActive = GetVehicleSeatWorld(vehicleSeat, vehicleRotation);
                if (vehicleAnchorActive && !simulatedPose) {
                    const float keepAlive = (renderTicket.pairSerial & 1u)
                        ? 0.05f : -0.05f;
                    vehicleSeat[2] += keepAlive;
                }
                if (vehicleAnchorActive)
                    PublishVehiclePose(vehicleSeat, vehicleRotation);
                else if (IsVehicleCameraActive())
                    vehicleAnchorActive = ReadVehiclePose(vehicleSeat, vehicleRotation);
            }
            if (vehicleAnchorActive) {
                memcpy(location, vehicleSeat, sizeof(vehicleSeat));
            }

            if (!simulatedPose) {
                if (eye == 0) {
                    float firstPersonLocation[3] = {};
                    int32_t firstPersonRotation[3] = {};
                    const input::PlayerIdentitySnapshot identity =
                        input::WeaponAimSystem::Instance().GetPlayerIdentity();
                    const bool downedFirstPerson = !vehicleAnchorActive &&
                        ApplyDownedFirstPersonOverride(identity, originalLocation,
                            rotation, firstPersonLocation,
                            firstPersonRotation);
                    renderTicket.baseCameraValid = true;
                    memcpy(renderTicket.baseLocation,
                           vehicleAnchorActive ? vehicleSeat :
                               (downedFirstPerson ? firstPersonLocation : originalLocation),
                           sizeof(renderTicket.baseLocation));
                    memcpy(renderTicket.baseRotation,
                           downedFirstPerson ? firstPersonRotation : rotation,
                           sizeof(renderTicket.baseRotation));
                    renderTicket.baseFov = config::Get().fov_degrees;
                } else if (!renderTicket.baseCameraValid) {
                    __leave;
                } else if (vehicleAnchorActive) {
                    // Keep HMD tracking frozen for stereo, but follow the vehicle's
                    // current physics transform so the second AFR eye does not lag.
                    const float dx = vehicleSeat[0] - renderTicket.baseLocation[0];
                    const float dy = vehicleSeat[1] - renderTicket.baseLocation[1];
                    const float dz = vehicleSeat[2] - renderTicket.baseLocation[2];
                    static std::atomic<uint64_t> compensationLogs{0};
                    const uint64_t compensationCount = compensationLogs.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                    if (compensationCount <= 3 || compensationCount % 1200 == 0) {
                        Log("[VehicleCamera] AFR eye-1 chassis compensation: "
                            "delta=(%.2f,%.2f,%.2f) UU", dx, dy, dz);
                    }
                    memcpy(renderTicket.baseLocation, vehicleSeat,
                           sizeof(renderTicket.baseLocation));
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
                player::ArmIKSystem::Instance().RequestRoomScaleReset();
                Log("[Camera] 6DoF tracking reference captured at pair=%llu",
                    static_cast<unsigned long long>(renderTicket.pairSerial));
            }

            const float inverseReference[4] = {
                -s_poseReferenceRotation[0], -s_poseReferenceRotation[1],
                -s_poseReferenceRotation[2], s_poseReferenceRotation[3]
            };
            RelativeQuaternion(s_poseReferenceRotation, headRotation, relative);
            float relativeHeadForwardXr[3] = {};
            const float xrHeadForward[3] = {0.0f, 0.0f, -1.0f};
            quat_rotate(relative[0], relative[1], relative[2], relative[3],
                        xrHeadForward, relativeHeadForwardXr);
            const float relativeHeadForwardUe[3] = {
                -relativeHeadForwardXr[2], relativeHeadForwardXr[0],
                relativeHeadForwardXr[1]
            };
            const float headHorizontal = sqrtf(
                relativeHeadForwardUe[0] * relativeHeadForwardUe[0] +
                relativeHeadForwardUe[1] * relativeHeadForwardUe[1]);
            constexpr float kRadiansToUnis = 65536.0f / 6.2831853071795864769f;
            const float headPitch = atan2f(relativeHeadForwardUe[2], headHorizontal);
            interactionRotation[0] = s_gamePitchReference.load(
                std::memory_order_relaxed) +
                static_cast<int32_t>(lroundf(headPitch * kRadiansToUnis));

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
                        armCameraLocation, gamePitch, gameYaw, headPosition,
                        headRotation, s_poseReferencePosition, s_poseReferenceRotation);
            }
            if (!renderTicket.rightAimValid) {
                input::ControllerState controllers[2] = {};
                if (input::XRInput::Instance().GetControllerSnapshot(controllers) &&
                    controllers[1].valid) {
                    const auto& right = controllers[1];
                    memcpy(renderTicket.rightAimPosition,
                        right.aimValid ? right.aimPosition : right.position,
                        sizeof(renderTicket.rightAimPosition));
                    memcpy(renderTicket.rightAimRotation,
                        right.aimValid ? right.aimRotation : right.rotation,
                        sizeof(renderTicket.rightAimRotation));
                    renderTicket.rightAimValid = true;
                    static std::atomic<bool> loggedAimRepair{false};
                    if (!loggedAimRepair.exchange(true, std::memory_order_relaxed)) {
                        Log("[Camera] Weapon aim ticket repaired from live right controller "
                            "snapshot (aim=%d)", right.aimValid);
                    }
                }
            }
            renderTicket.aimDotLocalValid = false;
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
                constexpr float localForward[3] = {0.0f, 0.0f, -1.0f};
                constexpr float localUp[3] = {0.0f, 1.0f, 0.0f};
                constexpr float kDegreesToRadians = 0.01745329251994329577f;
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
                const float worldRight[3] = {
                    worldUp[1] * worldForward[2] - worldUp[2] * worldForward[1],
                    worldUp[2] * worldForward[0] - worldUp[0] * worldForward[2],
                    worldUp[0] * worldForward[1] - worldUp[1] * worldForward[0]
                };
                const float rayPositionScale =
                    100.0f * config::Get().positional_scale;
                const float rayViewOffset[3] = {
                    -referenceLocalWeapon[2] * rayPositionScale,
                    referenceLocalWeapon[0] * rayPositionScale,
                    referenceLocalWeapon[1] * rayPositionScale
                };
                const float pitchedRayOffset[3] = {
                    pitchCosine * rayViewOffset[0] - pitchSine * rayViewOffset[2],
                    rayViewOffset[1],
                    pitchSine * rayViewOffset[0] + pitchCosine * rayViewOffset[2]
                };
                const auto& settings = config::Get();
                const float modelPitch = settings.weapon_rotation_pitch * kDegreesToRadians;
                const float modelYaw = settings.weapon_rotation_yaw * kDegreesToRadians;
                const float modelRoll = settings.weapon_rotation_roll * kDegreesToRadians;
                const float modelPitchSine = sinf(modelPitch);
                const float modelPitchCosine = cosf(modelPitch);
                const float modelYawSine = sinf(modelYaw);
                const float modelYawCosine = cosf(modelYaw);
                const float modelRollSine = sinf(modelRoll);
                const float modelRollCosine = cosf(modelRoll);
                const float modelForwardLocal[3] = {
                    modelPitchCosine * modelYawCosine,
                    modelPitchCosine * modelYawSine,
                    modelPitchSine
                };
                const float modelRightUnrolled[3] = {
                    -modelYawSine, modelYawCosine, 0.0f
                };
                const float modelUpUnrolled[3] = {
                    -modelPitchSine * modelYawCosine,
                    -modelPitchSine * modelYawSine,
                    modelPitchCosine
                };
                const float modelUpLocal[3] = {
                    modelUpUnrolled[0] * modelRollCosine -
                        modelRightUnrolled[0] * modelRollSine,
                    modelUpUnrolled[1] * modelRollCosine -
                        modelRightUnrolled[1] * modelRollSine,
                    modelUpUnrolled[2] * modelRollCosine -
                        modelRightUnrolled[2] * modelRollSine
                };
                float weaponForward[3] = {};
                float weaponUp[3] = {};
                for (int axis = 0; axis < 3; ++axis) {
                    weaponForward[axis] =
                        modelForwardLocal[0] * worldForward[axis] +
                        modelForwardLocal[1] * worldRight[axis] +
                        modelForwardLocal[2] * worldUp[axis];
                    weaponUp[axis] =
                        modelUpLocal[0] * worldForward[axis] +
                        modelUpLocal[1] * worldRight[axis] +
                        modelUpLocal[2] * worldUp[axis];
                }
                const float worldWeaponPosition[3] = {
                    armCameraLocation[0] + yawCosine * pitchedWeaponOffset[0] -
                        yawSine * pitchedWeaponOffset[1] +
                        worldForward[0] * settings.weapon_offset_forward +
                        worldRight[0] * settings.weapon_offset_right +
                        worldUp[0] * settings.weapon_offset_up,
                    armCameraLocation[1] + yawSine * pitchedWeaponOffset[0] +
                        yawCosine * pitchedWeaponOffset[1] +
                        worldForward[1] * settings.weapon_offset_forward +
                        worldRight[1] * settings.weapon_offset_right +
                        worldUp[1] * settings.weapon_offset_up,
                    armCameraLocation[2] + pitchedWeaponOffset[2] +
                        worldForward[2] * settings.weapon_offset_forward +
                        worldRight[2] * settings.weapon_offset_right +
                        worldUp[2] * settings.weapon_offset_up
                };
                const float worldAimOrigin[3] = {
                    armCameraLocation[0] + yawCosine * pitchedRayOffset[0] -
                        yawSine * pitchedRayOffset[1],
                    armCameraLocation[1] + yawSine * pitchedRayOffset[0] +
                        yawCosine * pitchedRayOffset[1],
                    armCameraLocation[2] + pitchedRayOffset[2]
                };
                float canonicalWeaponPosition[3] = {};
                float canonicalWeaponForward[3] = {};
                float canonicalWeaponUp[3] = {};
                bool canonicalWeaponTarget =
                    player::ArmIKSystem::Instance().GetWorldHandTarget(
                        renderTicket.armTargetGeneration, 1, canonicalWeaponPosition,
                        canonicalWeaponForward, canonicalWeaponUp);
                if (!canonicalWeaponTarget && simulatedPose) {
                    memcpy(canonicalWeaponPosition, worldWeaponPosition,
                           sizeof(canonicalWeaponPosition));
                    memcpy(canonicalWeaponForward, weaponForward,
                           sizeof(canonicalWeaponForward));
                    memcpy(canonicalWeaponUp, weaponUp, sizeof(canonicalWeaponUp));
                    canonicalWeaponTarget = true;
                }
                if (input::InputHook::Instance().IsMotionControlsEnabled() &&
                    canonicalWeaponTarget) {
                    static std::atomic<bool> loggedCanonicalCandidate{false};
                    if (!loggedCanonicalCandidate.exchange(true,
                            std::memory_order_relaxed)) {
                        Log("[Camera] Canonical weapon pose uses right-hand IK target: "
                            "position=(%.1f,%.1f,%.1f) forward=(%.3f,%.3f,%.3f) "
                            "up=(%.3f,%.3f,%.3f)",
                            canonicalWeaponPosition[0], canonicalWeaponPosition[1],
                            canonicalWeaponPosition[2], canonicalWeaponForward[0],
                            canonicalWeaponForward[1], canonicalWeaponForward[2],
                            canonicalWeaponUp[0], canonicalWeaponUp[1],
                            canonicalWeaponUp[2]);
                    }
                    input::InputHook::Instance().SetCanonicalWeaponPose(
                        canonicalWeaponPosition, canonicalWeaponForward,
                        canonicalWeaponUp,
                        armCameraLocation, nativeCameraForward, nativeCameraUp);
                    input::WeaponAimSystem::Instance().UpdateDirection(
                        worldAimOrigin, worldForward,
                        settings.dot_distance_m * settings.positional_scale);

                    // Put the compositor marker directly at the finite endpoint
                    // of the raw OpenXR aim laser. The same pose and distance
                    // feed ballistics above; visual hand/model trims never enter.
                    do {
                        float trackingForward[3] = {};
                        quat_rotate(renderTicket.rightAimRotation[0],
                                    renderTicket.rightAimRotation[1],
                                    renderTicket.rightAimRotation[2],
                                    renderTicket.rightAimRotation[3],
                                    localForward, trackingForward);
                        const float length = sqrtf(
                            trackingForward[0] * trackingForward[0] +
                            trackingForward[1] * trackingForward[1] +
                            trackingForward[2] * trackingForward[2]);
                        if (!std::isfinite(length) || length < 1.0e-5f) break;
                        for (int axis = 0; axis < 3; ++axis) {
                            trackingForward[axis] /= length;
                            renderTicket.aimDotLocalPosition[axis] =
                                renderTicket.rightAimPosition[axis] +
                                trackingForward[axis] * settings.dot_distance_m;
                            renderTicket.aimDotLocalForward[axis] =
                                trackingForward[axis];
                        }
                        renderTicket.aimDotLocalValid = true;
                    } while (false);
                } else {
                    input::InputHook::Instance().ClearCanonicalWeaponPose();
                    input::WeaponAimSystem::Instance().InvalidateDirection();
                }
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
            static std::atomic<bool> loggedCameraPoseException{false};
            if (!loggedCameraPoseException.exchange(true, std::memory_order_relaxed))
                Log("[Camera] Exception while preparing camera/controller pose");
        }
    }

    if (cameraOffsetApplied) {
        AcquireSRWLockExclusive(&s_pendingViewPoseLock);
        PendingViewPose& pendingPose = s_pendingViewPoses[eye];
        pendingPose = {};
        pendingPose.active = true;
        pendingPose.xrViewsValid = realPoseValid;
        pendingPose.vehicleAnchor =
            s_vehiclePawn.load(std::memory_order_acquire) != 0;
        pendingPose.firstPersonOverride =
            s_downedFirstPersonActive.load(std::memory_order_acquire);
        pendingPose.allowFirstPersonRecovery = !pendingPose.vehicleAnchor &&
            input::WeaponAimSystem::Instance().GetPlayerIdentity().pawnValid;
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
        memcpy(pendingPose.baseRotation, renderTicket.baseRotation,
               sizeof(pendingPose.baseRotation));
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
    // Publish only after both AFR eyes consumed the same prepared palette.
    // The next pair replays this palette identically for left and right.
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
            const bool retainVehicleLocation = cameraOffsetApplied &&
                IsVehicleCameraActive();
            if (!retainVehicleLocation) {
                memcpy(reinterpret_cast<void*>(camera.cameraCacheLocation),
                       originalLocation, sizeof(originalLocation));
            }
            memcpy(reinterpret_cast<void*>(camera.cameraCacheRotation),
                   interactionRotation, sizeof(interactionRotation));
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
        const float closestOriginError = (std::min)(originError, sourceOriginError);
        constexpr float kExternalCameraOriginErrorUu = 50.0f;
        const bool externalCameraDetected = pose.allowFirstPersonRecovery &&
            closestOriginError >= kExternalCameraOriginErrorUu;
        const bool injured = input::WeaponAimSystem::Instance().IsPlayerInjured();
        const bool externalCameraRecovered = !injured &&
            s_externalViewFirstPersonActive.load(std::memory_order_acquire) &&
            closestOriginError <= 20.0f;
        if (externalCameraRecovered) {
            s_externalViewFirstPersonActive.store(false, std::memory_order_release);
            Log("[Camera] External FSceneView ended; normal first-person restored");
        }
        const bool recoverFirstPerson = pose.firstPersonOverride ||
            (!externalCameraRecovered && externalCameraDetected);
        if (externalCameraDetected)
            s_externalViewFirstPersonActive.store(true, std::memory_order_release);
        if (recoverFirstPerson &&
            !s_downedFirstPersonActive.exchange(true, std::memory_order_acq_rel)) {
            Log("[Camera] External FSceneView detected (origin error %.1f UU); "
                "first-person override enabled", closestOriginError);
        }
        const bool validProjection = projection[0] > 0.1f && projection[5] > 0.1f &&
                                     fabsf(projection[11] - 1.0f) < 0.01f &&
                                     fabsf(projection[15]) < 0.01f;
        if ((!pose.vehicleAnchor &&
             closestOriginError > 10.0f && !recoverFirstPerson) || !validProjection)
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

        // Preserve UE3's exact source basis normally. An external/downed view
        // needs the last first-person rotator or it keeps looking back at the pawn.
        float firstPersonViewRotation[16] = {};
        if (recoverFirstPerson)
            BuildViewRotation(pose.baseRotation, firstPersonViewRotation);
        const float* baseTranslatedView = recoverFirstPerson
            ? firstPersonViewRotation : backup.translatedView;
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
        const float* anchorOrigin = pose.vehicleAnchor
            ? pose.headLocation : (recoverFirstPerson ? pose.location : backup.origin);
        const float newOrigin[3] = {
            anchorOrigin[0] + worldOffset[0],
            anchorOrigin[1] + worldOffset[1],
            anchorOrigin[2] + worldOffset[2]
        };
        if (pose.vehicleAnchor) {
            static std::atomic<uint64_t> vehicleViewLogs{0};
            const uint64_t logCount = vehicleViewLogs.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (logCount <= 3 || logCount % 1200 == 0) {
                Log("[VehicleCamera] Final FSceneView origin: game=(%.1f,%.1f,%.1f) "
                    "driver=(%.1f,%.1f,%.1f)", backup.origin[0], backup.origin[1],
                    backup.origin[2], newOrigin[0], newOrigin[1], newOrigin[2]);
            }
        }
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

    if (principalSource) {
        input::InputHook::Instance().ReapplyWeaponPose();
        player::ArmIKSystem::Instance().ReapplyRenderPalette();
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
            if (!s_vehiclePawn.load(std::memory_order_acquire))
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
    uint64_t nextIdentityRefreshMs = GetTickCount64() + 500;
    while (success && s_camera.found) {
        Sleep(250);
        if (s_vehicleEnterPending.exchange(false, std::memory_order_acq_rel) &&
            IsVehicleCameraActive()) {
            const bool armIkEnabled = player::ArmIKSystem::Instance().IsEnabled();
            s_vehicleArmIkWasEnabled.store(armIkEnabled, std::memory_order_release);
            if (armIkEnabled) player::ArmIKSystem::Instance().SetEnabled(false);
            const CameraInfo current = GetCameraSnapshot();
            const uintptr_t vehicle = s_vehiclePawn.load(std::memory_order_acquire);
            if (current.found && current.controllerAddress && vehicle) {
                if (input::WeaponAimSystem::Instance().RefreshIdentityFromLivePawn(
                        current.controllerAddress, vehicle)) {
                    Log("[VehicleCamera] Ballistic identity refreshed on entry");
                } else {
                    input::WeaponAimSystem::Instance().Discover(
                        &s_globals, current.controllerAddress,
                        reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll),
                        modInfo.SizeOfImage);
                    Log("[VehicleCamera] Ballistic identity fallback scan completed");
                }
            }
        }
        if (s_visibilityInventoryRefreshRequested.exchange(
                false, std::memory_order_acq_rel)) {
            const CameraInfo current = GetCameraSnapshot();
            if (current.found && current.controllerAddress) {
                input::WeaponAimSystem::Instance().Discover(
                    &s_globals, current.controllerAddress,
                    reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll), modInfo.SizeOfImage);
                if (!s_vehiclePawn.load(std::memory_order_acquire))
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
        if (currentIdentity.weaponValid)
            s_vehicleExitRecoveryUntilMs.store(0, std::memory_order_release);
        const bool vehicleExitRecovery = !currentIdentity.weaponValid &&
            nowMs < s_vehicleExitRecoveryUntilMs.load(std::memory_order_acquire);
        const bool identityRefreshDue =
            (!ControllerHasLivePawn(currentCamera.controllerAddress) ||
             !currentIdentity.pawnValid ||
             vehicleExitRecovery ||
             currentIdentity.controller != currentCamera.controllerAddress) &&
            nowMs >= nextIdentityRefreshMs;
        if (!cameraRefreshRequested && !identityRefreshDue) continue;
        nextIdentityRefreshMs = nowMs + 500;

        CameraInfo refreshed = {};
        if (!RefreshCameraCache(s_globals, &refreshed)) {
            Log("[Camera] Runtime camera refresh failed; retrying after further rejections");
            continue;
        }

        const CameraInfo previous = GetCameraSnapshot();
        if (previous.controllerAddress == refreshed.controllerAddress) {
            if (!currentIdentity.pawnValid || vehicleExitRecovery ||
                currentIdentity.controller != refreshed.controllerAddress) {
                input::WeaponAimSystem::Instance().Discover(
                    &s_globals, refreshed.controllerAddress,
                    reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll), modInfo.SizeOfImage);
                if (!s_vehiclePawn.load(std::memory_order_acquire))
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
        if (!s_vehiclePawn.load(std::memory_order_acquire))
            player::ArmIKSystem::Instance().RequestRescan();
        Log("[Camera] Runtime camera refreshed: controller=%p -> %p; 6DoF recentered",
            reinterpret_cast<void*>(previous.controllerAddress),
            reinterpret_cast<void*>(refreshed.controllerAddress));
    }
    return 0;
}

static void UpdateVehicleLifecycleFast(const CameraInfo& camera) {
    if (!camera.found || camera.controllerAddress < 0x10000) return;
    uintptr_t livePawn = 0;
    if (!CameraRead(camera.controllerAddress + 0x260, &livePawn, sizeof(livePawn)) ||
        livePawn < 0x10000) return;

    const uintptr_t activeVehicle = s_vehiclePawn.load(std::memory_order_acquire);
    if (livePawn == activeVehicle &&
        s_vehicleAnchorReady.load(std::memory_order_acquire)) {
        s_vehicleExitCandidateSinceMs.store(0, std::memory_order_release);
        return;
    }
    const UE3Globals globals = GetUE3GlobalsSnapshot();
    char pawnClass[128] = {};
    if (!CameraReadClassName(globals, livePawn, pawnClass, sizeof(pawnClass))) return;

    if (strstr(pawnClass, "WillowPlayerPawn") != nullptr) {
        if (!activeVehicle) {
            const input::PlayerIdentitySnapshot before =
                input::WeaponAimSystem::Instance().GetPlayerIdentity();
            input::WeaponAimSystem::Instance().RefreshIdentityFromLivePawn(
                camera.controllerAddress, livePawn);
            const input::PlayerIdentitySnapshot after =
                input::WeaponAimSystem::Instance().GetPlayerIdentity();
            if (after.generation != before.generation)
                player::ArmIKSystem::Instance().RequestInventoryScan();
            const uint64_t recoveryUntil = s_vehicleExitRecoveryUntilMs.load(
                std::memory_order_acquire);
            if (recoveryUntil && GetTickCount64() < recoveryUntil &&
                input::WeaponAimSystem::Instance().RefreshIdentityFromLivePawn(
                    camera.controllerAddress, livePawn))
                s_vehicleExitRecoveryUntilMs.store(0, std::memory_order_release);
            return;
        }
        uintptr_t vehicleController = 0;
        if (CameraRead(activeVehicle + 0x26C, &vehicleController,
                       sizeof(vehicleController)) &&
            vehicleController == camera.controllerAddress) {
            s_vehicleExitCandidateSinceMs.store(0, std::memory_order_release);
            return;
        }
        const uint64_t nowMs = GetTickCount64();
        uint64_t exitCandidateSince = s_vehicleExitCandidateSinceMs.load(
            std::memory_order_acquire);
        if (!exitCandidateSince) {
            s_vehicleExitCandidateSinceMs.store(nowMs, std::memory_order_release);
            Log("[VehicleCamera] Exit candidate started; retaining driver camera");
            return;
        }
        if (nowMs - exitCandidateSince < 300) return;
        s_vehicleExitCandidateSinceMs.store(0, std::memory_order_release);
        s_vehiclePawn.store(0, std::memory_order_release);
        s_vehicleAnchorReady.store(false, std::memory_order_release);
        s_vehicleComponent.store(0, std::memory_order_release);
        s_vehicleSeatBone.store(-1, std::memory_order_release);
        s_vehicleAnchorKind.store(0, std::memory_order_release);
        s_vehicleBoneProbeComplete.store(false, std::memory_order_release);
        ClearVehiclePose();
        s_recenterRequested.store(true, std::memory_order_release);
        s_gamePitchReferenceValid.store(false, std::memory_order_release);
        s_vehicleExitRecoveryUntilMs.store(
            GetTickCount64() + 10000, std::memory_order_release);
        s_vehicleEnterPending.store(false, std::memory_order_release);
        input::WeaponAimSystem::Instance().RefreshIdentityFromLivePawn(
            camera.controllerAddress, livePawn);
        RequestPlayerIdentityRefresh();
        const bool restoreArmIk = s_vehicleArmIkWasEnabled.exchange(
            false, std::memory_order_acq_rel);
        if (restoreArmIk && input::InputHook::Instance().IsMotionControlsEnabled())
            player::ArmIKSystem::Instance().SetEnabled(true);
        AcquireSRWLockExclusive(&s_pendingViewPoseLock);
        s_pendingViewPoses[0] = {};
        s_pendingViewPoses[1] = {};
        ReleaseSRWLockExclusive(&s_pendingViewPoseLock);
        ClearCommandPoses();
        xr::FrameLoop::Instance().AbortStereoPair();
        Log("[VehicleCamera] Fast exit: live pawn=%p(%s)",
            reinterpret_cast<void*>(livePawn), pawnClass);
        return;
    }
    if ((activeVehicle && livePawn != activeVehicle) ||
        strstr(pawnClass, "WillowVehicle_WheeledVehicle") == nullptr) {
        if (activeVehicle)
            s_vehicleExitCandidateSinceMs.store(0, std::memory_order_release);
        return;
    }

    if (!activeVehicle) {
        const bool armIkEnabled = player::ArmIKSystem::Instance().IsEnabled();
        s_vehicleArmIkWasEnabled.store(armIkEnabled, std::memory_order_release);
        s_vehiclePawn.store(livePawn, std::memory_order_release);
        s_vehicleExitCandidateSinceMs.store(0, std::memory_order_release);
        const bool ballisticReady =
            input::WeaponAimSystem::Instance().RefreshIdentityFromLivePawn(
                camera.controllerAddress, livePawn);
        Log("[VehicleCamera] Fast vehicle state entered: vehicle=%p ballistic=%d",
            reinterpret_cast<void*>(livePawn), ballisticReady);
    }

    uintptr_t component = 0;
    if (!player::ArmIKSystem::Instance().FindObservedVehicleComponent(
            livePawn, component)) {
        bool expected = false;
        if (s_vehicleAnchorProbeRunning.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            HANDLE thread = CreateThread(nullptr, 0, VehicleAnchorProbeThread,
                reinterpret_cast<void*>(livePawn), 0, nullptr);
            if (thread) CloseHandle(thread);
            else s_vehicleAnchorProbeRunning.store(false, std::memory_order_release);
        }
        return;
    }
    CameraTArray64 componentPose = {};
    float seatMatrix[16] = {};
    float componentMatrix[16] = {};
    constexpr int kSteeringBone = 24;
    if (!CameraRead(component + 0x330, &componentPose, sizeof(componentPose)) ||
        !componentPose.data || componentPose.count != 26 ||
        componentPose.capacity < componentPose.count ||
        !CameraRead(componentPose.data + kSteeringBone * 0x40,
                    seatMatrix, sizeof(seatMatrix)) ||
        !CameraRead(component + 0xA0, componentMatrix, sizeof(componentMatrix)) ||
        !std::isfinite(seatMatrix[12]) ||
        fabsf(seatMatrix[15] - 1.0f) > 0.1f ||
        fabsf(componentMatrix[15] - 1.0f) > 0.1f) return;

    const float localAnchor[3] = {seatMatrix[12], seatMatrix[13], seatMatrix[14]};
    ClearVehiclePose();
    PublishVehicleAnchorLocal(localAnchor);
    s_vehicleComponent.store(component, std::memory_order_release);
    s_vehicleSeatBone.store(kSteeringBone, std::memory_order_release);
    s_vehicleAnchorKind.store(1, std::memory_order_release);
    s_vehicleBoneProbeComplete.store(true, std::memory_order_release);
    s_vehicleAnchorReady.store(true, std::memory_order_release);
    s_recenterRequested.store(true, std::memory_order_release);
    s_gamePitchReferenceValid.store(false, std::memory_order_release);
    s_vehicleEnterPending.store(true, std::memory_order_release);
    Log("[VehicleCamera] Fast entry: vehicle=%p component=%p steeringLocal=(%.1f,%.1f,%.1f)",
        reinterpret_cast<void*>(livePawn), reinterpret_cast<void*>(component),
        localAnchor[0], localAnchor[1], localAnchor[2]);
}

static DWORD WINAPI VehicleAnchorProbeThread(void* parameter) {
    const uintptr_t vehicle = reinterpret_cast<uintptr_t>(parameter);
    const UE3Globals globals = GetUE3GlobalsSnapshot();
    constexpr int kSteeringBone = 24;
    uintptr_t resolvedComponent = 0;
    float resolvedLocal[3] = {};
    for (int pointerOffset = 0x80; pointerOffset <= 0x3000; pointerOffset += 8) {
        if (s_vehiclePawn.load(std::memory_order_acquire) != vehicle) break;
        uintptr_t component = 0;
        uintptr_t outer = 0;
        CameraTArray64 componentPose = {};
        float seatMatrix[16] = {};
        float componentMatrix[16] = {};
        if (!CameraRead(vehicle + pointerOffset, &component, sizeof(component)) ||
            component < 0x10000 || globals.gObjectNameOffset < 8 ||
            !CameraRead(component + globals.gObjectNameOffset - 8,
                        &outer, sizeof(outer)) || outer != vehicle ||
            !CameraRead(component + 0x330, &componentPose, sizeof(componentPose)) ||
            !componentPose.data || componentPose.count != 26 ||
            componentPose.capacity < componentPose.count ||
            !CameraRead(componentPose.data + kSteeringBone * 0x40,
                        seatMatrix, sizeof(seatMatrix)) ||
            !CameraRead(component + 0xA0, componentMatrix, sizeof(componentMatrix)) ||
            !std::isfinite(seatMatrix[12]) || !std::isfinite(seatMatrix[13]) ||
            !std::isfinite(seatMatrix[14]) ||
            fabsf(seatMatrix[15] - 1.0f) > 0.1f ||
            fabsf(componentMatrix[15] - 1.0f) > 0.1f) continue;
        resolvedComponent = component;
        resolvedLocal[0] = seatMatrix[12];
        resolvedLocal[1] = seatMatrix[13];
        resolvedLocal[2] = seatMatrix[14];
        break;
    }

    if (resolvedComponent &&
        s_vehiclePawn.load(std::memory_order_acquire) == vehicle) {
        ClearVehiclePose();
        PublishVehicleAnchorLocal(resolvedLocal);
        s_vehicleComponent.store(resolvedComponent, std::memory_order_release);
        s_vehicleSeatBone.store(kSteeringBone, std::memory_order_release);
        s_vehicleAnchorKind.store(1, std::memory_order_release);
        s_vehicleBoneProbeComplete.store(true, std::memory_order_release);
        s_vehicleAnchorReady.store(true, std::memory_order_release);
        s_recenterRequested.store(true, std::memory_order_release);
        s_gamePitchReferenceValid.store(false, std::memory_order_release);
        s_vehicleEnterPending.store(true, std::memory_order_release);
        Log("[VehicleCamera] Direct anchor resolved: vehicle=%p component=%p "
            "steeringLocal=(%.1f,%.1f,%.1f)", reinterpret_cast<void*>(vehicle),
            reinterpret_cast<void*>(resolvedComponent), resolvedLocal[0],
            resolvedLocal[1], resolvedLocal[2]);
    } else if (s_vehiclePawn.load(std::memory_order_acquire) == vehicle) {
        Log("[VehicleCamera] Direct anchor probe failed: vehicle=%p",
            reinterpret_cast<void*>(vehicle));
    }
    s_vehicleAnchorProbeRunning.store(false, std::memory_order_release);
    return 0;
}

static bool GetVehicleSeatWorld(float output[3], int32_t rotation[3]) {
    if (!output || !rotation ||
        !s_vehicleAnchorReady.load(std::memory_order_acquire)) return false;
    const uintptr_t pawn = s_vehiclePawn.load(std::memory_order_acquire);
    const uintptr_t component = s_vehicleComponent.load(std::memory_order_acquire);
    const int seatBone = s_vehicleSeatBone.load(std::memory_order_acquire);
    const int anchorKind = s_vehicleAnchorKind.load(std::memory_order_acquire);
    if (component < 0x10000 || seatBone < 0) {
        float componentMatrix[16] = {};
        if (component >= 0x10000 &&
            CameraRead(component + 0xA0, componentMatrix, sizeof(componentMatrix)) &&
            std::isfinite(componentMatrix[12]) &&
            std::isfinite(componentMatrix[13]) &&
            std::isfinite(componentMatrix[14]) &&
            fabsf(componentMatrix[15] - 1.0f) < 0.1f) {
            output[0] = componentMatrix[12];
            output[1] = componentMatrix[13];
            output[2] = componentMatrix[14] + 100.0f;
            constexpr float kRadiansToUnis = 65536.0f / 6.2831853071795864769f;
            const float horizontal = sqrtf(componentMatrix[0] * componentMatrix[0] +
                                             componentMatrix[1] * componentMatrix[1]);
            rotation[0] = static_cast<int32_t>(lroundf(
                atan2f(componentMatrix[2], horizontal) * kRadiansToUnis));
            rotation[1] = static_cast<int32_t>(lroundf(
                atan2f(componentMatrix[1], componentMatrix[0]) * kRadiansToUnis));
            rotation[2] = 0;
            return true;
        }
        const int32_t locationOffset = s_vehicleLocationOffset.load(std::memory_order_acquire);
        float location[3] = {};
        if (pawn < 0x10000 || locationOffset <= 0 ||
            !CameraRead(pawn + static_cast<uintptr_t>(locationOffset),
                        location, sizeof(location)) ||
            !std::isfinite(location[0]) || !std::isfinite(location[1]) ||
            !std::isfinite(location[2])) return false;
        output[0] = location[0];
        output[1] = location[1];
        output[2] = location[2] + 100.0f;
        rotation[0] = s_gamePitchReference.load(std::memory_order_relaxed);
        rotation[1] = 0;
        rotation[2] = 0;
        static std::atomic<uint64_t> fallbackLogs{0};
        const uint64_t logCount = fallbackLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 3 || logCount % 1200 == 0) {
            Log("[VehicleCamera] Vehicle-origin anchor: pawn=%p offset=0x%X "
                "world=(%.1f,%.1f,%.1f)", reinterpret_cast<void*>(pawn),
                locationOffset, output[0], output[1], output[2]);
        }
        return true;
    }

    float localToWorld[16] = {};
    float seatPosition[3] = {};
    if (!ReadVehicleAnchorLocal(seatPosition)) {
        CameraTArray64 componentPose = {};
        float seatLocal[16] = {};
        if (!CameraRead(component + 0x330, &componentPose, sizeof(componentPose)) ||
            !componentPose.data || componentPose.count <= seatBone ||
            componentPose.count > componentPose.capacity || componentPose.count > 512 ||
            !CameraRead(componentPose.data + static_cast<uintptr_t>(seatBone) * 0x40,
                        seatLocal, sizeof(seatLocal)) ||
            !std::isfinite(seatLocal[12]) || !std::isfinite(seatLocal[13]) ||
            !std::isfinite(seatLocal[14]) || fabsf(seatLocal[15] - 1.0f) > 0.1f)
            return false;
        seatPosition[0] = seatLocal[12];
        seatPosition[1] = seatLocal[13];
        seatPosition[2] = seatLocal[14];
        PublishVehicleAnchorLocal(seatPosition);
    }
    if (!CameraRead(component + 0xA0, localToWorld, sizeof(localToWorld))) return false;
    if (!std::isfinite(localToWorld[12]) ||
        !std::isfinite(localToWorld[13]) || !std::isfinite(localToWorld[14]) ||
        fabsf(localToWorld[15] - 1.0f) > 0.1f) return false;

    float world[3] = {
        localToWorld[0] * seatPosition[0] + localToWorld[4] * seatPosition[1] +
            localToWorld[8] * seatPosition[2] + localToWorld[12],
        localToWorld[1] * seatPosition[0] + localToWorld[5] * seatPosition[1] +
            localToWorld[9] * seatPosition[2] + localToWorld[13],
        localToWorld[2] * seatPosition[0] + localToWorld[6] * seatPosition[1] +
            localToWorld[10] * seatPosition[2] + localToWorld[14]};
    float forward[3] = {localToWorld[0], localToWorld[1], localToWorld[2]};
    float up[3] = {localToWorld[8], localToWorld[9], localToWorld[10]};
    const float forwardLength = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] +
                                      forward[2] * forward[2]);
    const float upLength = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!std::isfinite(forwardLength) || !std::isfinite(upLength) ||
        forwardLength < 0.01f || upLength < 0.01f) return false;
    for (int axis = 0; axis < 3; ++axis) {
        forward[axis] /= forwardLength;
        up[axis] /= upLength;
        const float forwardOffset = anchorKind == 1 ? -40.0f : 10.0f;
        const float upOffset = anchorKind == 1 ? 45.0f : 65.0f;
        world[axis] += forward[axis] * forwardOffset + up[axis] * upOffset;
    }
    const float dx = world[0] - localToWorld[12];
    const float dy = world[1] - localToWorld[13];
    const float dz = world[2] - localToWorld[14];
    const float anchorDistance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(anchorDistance) || anchorDistance > 800.0f) return false;
    memcpy(output, world, sizeof(world));
    constexpr float kRadiansToUnis = 65536.0f / 6.2831853071795864769f;
    const float horizontal = sqrtf(localToWorld[0] * localToWorld[0] +
                                   localToWorld[1] * localToWorld[1]);
    rotation[0] = static_cast<int32_t>(lroundf(
        atan2f(localToWorld[2], horizontal) * kRadiansToUnis));
    rotation[1] = static_cast<int32_t>(lroundf(
        atan2f(localToWorld[1], localToWorld[0]) * kRadiansToUnis));
    rotation[2] = 0;

    static std::atomic<uint64_t> anchorLogs{0};
    const uint64_t logCount = anchorLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logCount <= 3 || logCount % 1200 == 0) {
        Log("[VehicleCamera] Driver anchor: bone=%d world=(%.1f,%.1f,%.1f) "
            "vehicle=(%.1f,%.1f,%.1f)", seatBone, world[0], world[1], world[2],
            localToWorld[12], localToWorld[13], localToWorld[14]);
    }
    return true;
}

static void ProbeLocalVehicle(const input::PlayerIdentitySnapshot& identity) {
    const UE3Globals globals = GetUE3GlobalsSnapshot();
    if (!globals.gNamesValid || !globals.gObjectsValid ||
        !identity.pawnValid || identity.pawn < 0x10000) return;

    char pawnName[128] = {};
    char pawnClassName[128] = {};
    if (!CameraReadObjectName(globals, identity.pawn, pawnName, sizeof(pawnName)) ||
        !CameraReadClassName(globals, identity.pawn, pawnClassName,
                             sizeof(pawnClassName))) return;
    const bool isVehicle = strstr(pawnName, "Vehicle") || strstr(pawnName, "vehicle") ||
        strstr(pawnClassName, "Vehicle") || strstr(pawnClassName, "vehicle");
    if (!isVehicle) {
        const bool isVehicleWeaponPawn = strstr(pawnName, "WeaponPawn") ||
            strstr(pawnClassName, "WeaponPawn");
        if (isVehicleWeaponPawn) {
            if (s_vehiclePawn.load(std::memory_order_acquire)) return;
            const uintptr_t roots[] = {identity.pawn, identity.weapon};
            for (uintptr_t root : roots) {
                if (root < 0x10000) continue;
                for (int offset = 0x80; offset <= 0x3000; offset += 8) {
                    uintptr_t candidate = 0;
                    char candidateClass[128] = {};
                    if (!CameraRead(root + offset, &candidate, sizeof(candidate)) ||
                        candidate < 0x10000 || candidate == root ||
                        !CameraReadClassName(globals, candidate, candidateClass,
                                             sizeof(candidateClass)) ||
                        strstr(candidateClass, "WillowVehicle_WheeledVehicle") == nullptr)
                        continue;
                    char candidateName[128] = {};
                    CameraReadObjectName(globals, candidate, candidateName,
                                         sizeof(candidateName));
                    Log("[VehicleCamera] WeaponPawn vehicle link: root=%p +0x%X -> "
                        "%p(%s/%s)", reinterpret_cast<void*>(root), offset,
                        reinterpret_cast<void*>(candidate), candidateName, candidateClass);
                    input::PlayerIdentitySnapshot vehicleIdentity = identity;
                    vehicleIdentity.pawn = candidate;
                    ProbeLocalVehicle(vehicleIdentity);
                    return;
                }
            }
            const player::ComponentInventoryStatus inventory =
                player::ArmIKSystem::Instance().GetComponentInventory();
            const CameraInfo camera = GetCameraSnapshot();
            uintptr_t nearestVehicle = 0;
            float nearestDistance = FLT_MAX;
            for (size_t index = 0; index < inventory.count; ++index) {
                const player::ComponentInventoryEntry& entry = inventory.entries[index];
                if (entry.component < 0x10000 || entry.outer < 0x10000 ||
                    strstr(entry.outerName, "WillowVehicle") == nullptr ||
                    entry.localToWorldOffset <= 0) continue;
                char candidateClass[128] = {};
                float matrix[16] = {};
                float cameraLocation[3] = {};
                if (!CameraReadClassName(globals, entry.outer, candidateClass,
                                         sizeof(candidateClass)) ||
                    strstr(candidateClass, "WillowVehicle_WheeledVehicle") == nullptr ||
                    !CameraRead(entry.component + entry.localToWorldOffset,
                                matrix, sizeof(matrix)) ||
                    !CameraRead(camera.cameraCacheLocation, cameraLocation,
                                sizeof(cameraLocation))) continue;
                const float dx = matrix[12] - cameraLocation[0];
                const float dy = matrix[13] - cameraLocation[1];
                const float dz = matrix[14] - cameraLocation[2];
                const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(distance) || distance >= nearestDistance) continue;
                nearestDistance = distance;
                nearestVehicle = entry.outer;
            }
            if (nearestVehicle) {
                Log("[VehicleCamera] WeaponPawn inventory fallback: vehicle=%p "
                    "cameraDistance=%.1f", reinterpret_cast<void*>(nearestVehicle),
                    nearestDistance);
                input::PlayerIdentitySnapshot vehicleIdentity = identity;
                vehicleIdentity.pawn = nearestVehicle;
                ProbeLocalVehicle(vehicleIdentity);
                return;
            }
            static std::atomic<uint64_t> weaponPawnProbeFailures{0};
            const uint64_t failures = weaponPawnProbeFailures.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (failures <= 3 || failures % 20 == 0)
                Log("[VehicleCamera] WeaponPawn has no direct vehicle link (%llu)",
                    static_cast<unsigned long long>(failures));
            return;
        }
        if (s_vehiclePawn.exchange(0, std::memory_order_acq_rel)) {
            s_vehicleAnchorReady.store(false, std::memory_order_release);
            ClearVehiclePose();
            s_vehicleComponent.store(0, std::memory_order_release);
            s_vehicleSeatBone.store(-1, std::memory_order_release);
            s_vehicleAnchorKind.store(0, std::memory_order_release);
            s_vehicleLocationOffset.store(-1, std::memory_order_release);
            s_vehicleBoneProbeComplete.store(false, std::memory_order_release);
            s_recenterRequested.store(true, std::memory_order_release);
            s_gamePitchReferenceValid.store(false, std::memory_order_release);
            s_vehicleExitRecoveryUntilMs.store(
                GetTickCount64() + 10000, std::memory_order_release);
            const bool restoreArmIk = s_vehicleArmIkWasEnabled.exchange(
                false, std::memory_order_acq_rel);
            if (restoreArmIk && input::InputHook::Instance().IsMotionControlsEnabled()) {
                player::ArmIKSystem::Instance().SetEnabled(true);
                player::ArmIKSystem::Instance().RequestNativeCalibrationReset();
                input::InputHook::Instance().RequestMotionCalibrationReset();
            }
            Log("[VehicleCamera] First-person vehicle mode exited");
        }
        return;
    }
    if (s_vehiclePawn.load(std::memory_order_acquire) == identity.pawn &&
        s_vehicleBoneProbeComplete.load(std::memory_order_acquire)) return;
    if (s_vehiclePawn.exchange(identity.pawn, std::memory_order_acq_rel) != identity.pawn) {
        s_vehicleAnchorReady.store(false, std::memory_order_release);
        ClearVehiclePose();
        s_vehicleComponent.store(0, std::memory_order_release);
        s_vehicleSeatBone.store(-1, std::memory_order_release);
        s_vehicleAnchorKind.store(0, std::memory_order_release);
        s_vehicleLocationOffset.store(-1, std::memory_order_release);
        s_vehicleBoneProbeComplete.store(false, std::memory_order_release);
        s_recenterRequested.store(true, std::memory_order_release);
        s_gamePitchReferenceValid.store(false, std::memory_order_release);
        const bool armIkEnabled = player::ArmIKSystem::Instance().IsEnabled();
        s_vehicleArmIkWasEnabled.store(armIkEnabled, std::memory_order_release);
        if (armIkEnabled) player::ArmIKSystem::Instance().SetEnabled(false);
        const player::ComponentInventoryStatus inventory =
            player::ArmIKSystem::Instance().GetComponentInventory();
        for (size_t index = 0; index < inventory.count; ++index) {
            const player::ComponentInventoryEntry& entry = inventory.entries[index];
            if (entry.outer != identity.pawn || entry.component < 0x10000 ||
                entry.boneCount != 26) continue;
            float componentMatrix[16] = {};
            const int matrixOffset = entry.localToWorldOffset > 0
                ? entry.localToWorldOffset : 0xA0;
            if (!CameraRead(entry.component + matrixOffset, componentMatrix,
                            sizeof(componentMatrix)) ||
                !std::isfinite(componentMatrix[12]) ||
                fabsf(componentMatrix[15] - 1.0f) > 0.1f) continue;
            s_vehicleComponent.store(entry.component, std::memory_order_release);
            s_vehicleAnchorReady.store(true, std::memory_order_release);
            Log("[VehicleCamera] Immediate vehicle mesh from inventory: component=%p",
                reinterpret_cast<void*>(entry.component));
            break;
        }
        Log("[VehicleCamera] Local vehicle detected: pawn=%p object=%s class=%s",
            reinterpret_cast<void*>(identity.pawn), pawnName, pawnClassName);
    }

    for (int pointerOffset = 0x80; pointerOffset <= 0x3000; pointerOffset += 8) {
        uintptr_t component = 0;
        char componentClass[128] = {};
        if (!CameraRead(identity.pawn + pointerOffset, &component, sizeof(component)) ||
            component < 0x10000 ||
            !CameraReadClassName(globals, component, componentClass,
                                 sizeof(componentClass)) ||
            strstr(componentClass, "SkeletalMeshComponent") == nullptr) continue;
        uintptr_t outer = 0;
        if (globals.gObjectNameOffset < 8 ||
            !CameraRead(component + globals.gObjectNameOffset - 8, &outer, sizeof(outer)) ||
            outer != identity.pawn) continue;
        float componentMatrix[16] = {};
        if (CameraRead(component + 0xA0, componentMatrix, sizeof(componentMatrix)) &&
            std::isfinite(componentMatrix[12]) &&
            std::isfinite(componentMatrix[13]) &&
            std::isfinite(componentMatrix[14]) &&
            fabsf(componentMatrix[15] - 1.0f) < 0.1f) {
            s_vehicleComponent.store(component, std::memory_order_release);
            s_vehicleAnchorReady.store(true, std::memory_order_release);
        }

        for (int meshOffset = 0x80; meshOffset <= 0x700; meshOffset += 4) {
            uintptr_t mesh = 0;
            char meshClass[128] = {};
            if (!CameraRead(component + meshOffset, &mesh, sizeof(mesh)) ||
                mesh < 0x10000 ||
                !CameraReadClassName(globals, mesh, meshClass, sizeof(meshClass)) ||
                strstr(meshClass, "SkeletalMesh") == nullptr ||
                strstr(meshClass, "Component") != nullptr) continue;

            for (int skeletonOffset = 0x40; skeletonOffset <= 0x800;
                 skeletonOffset += 4) {
                CameraTArray64 skeleton = {};
                if (!CameraRead(mesh + skeletonOffset, &skeleton, sizeof(skeleton)) ||
                    !skeleton.data || skeleton.count <= 0 || skeleton.count > 256 ||
                    skeleton.count > skeleton.capacity || skeleton.count != 26) continue;
                for (const int stride : {0x50, 0x58, 0x40, 0x60, 0x68}) {
                    int steeringBone = -1;
                    int backSeatBone = -1;
                    bool namesValid = true;
                    for (int bone = 0; bone < skeleton.count; ++bone) {
                        int32_t nameIndex = -1;
                        char boneName[64] = {};
                        if (!CameraRead(skeleton.data +
                                static_cast<uintptr_t>(bone) * stride,
                                &nameIndex, sizeof(nameIndex)) ||
                            !CameraReadName(globals, nameIndex, boneName,
                                            sizeof(boneName))) {
                            namesValid = false;
                            break;
                        }
                        if (_stricmp(boneName, "b_wheel_steering") == 0)
                            steeringBone = bone;
                        else if (_stricmp(boneName, "b_back_seat") == 0)
                            backSeatBone = bone;
                    }
                    if (!namesValid) continue;
                    const int bone = steeringBone >= 0 ? steeringBone : backSeatBone;
                    const int anchorKind = steeringBone >= 0 ? 1 : 2;
                    if (bone < 0) continue;
                    CameraTArray64 componentPose = {};
                    float seatMatrix[16] = {};
                    if (!CameraRead(component + 0x330, &componentPose,
                                    sizeof(componentPose)) ||
                        !componentPose.data || componentPose.count <= bone ||
                        componentPose.count > componentPose.capacity ||
                        !CameraRead(componentPose.data +
                                static_cast<uintptr_t>(bone) * 0x40,
                                seatMatrix, sizeof(seatMatrix)) ||
                        !std::isfinite(seatMatrix[12]) ||
                        fabsf(seatMatrix[15] - 1.0f) > 0.1f) continue;
                    s_vehicleComponent.store(component, std::memory_order_release);
                    s_vehicleSeatBone.store(bone, std::memory_order_release);
                    s_vehicleAnchorKind.store(anchorKind, std::memory_order_release);
                    s_vehicleAnchorReady.store(true, std::memory_order_release);
                    s_vehicleBoneProbeComplete.store(true, std::memory_order_release);
                    Log("[VehicleCamera] Driver anchor resolved: component=%p mesh=%p "
                        "bone=%d kind=%s skeleton=+0x%X stride=0x%X poseStride=0x40",
                        reinterpret_cast<void*>(component),
                        reinterpret_cast<void*>(mesh), bone,
                        anchorKind == 1 ? "steering" : "back-seat",
                        skeletonOffset, stride);
                    return;
                }
            }
        }
    }

    static std::atomic<uint64_t> probeFailures{0};
    const uint64_t failures = probeFailures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures <= 3 || failures % 20 == 0) {
        Log("[VehicleCamera] seat bones unavailable; using vehicle origin (%llu)",
            static_cast<unsigned long long>(failures));
    }
    s_vehicleBoneProbeComplete.store(true, std::memory_order_release);
}

void StartScanner() {
    CreateThread(nullptr, 0, ScannerThread, nullptr, 0, nullptr);
}

}} // namespace bl1gotyvr::camera
