#include "InputHook.hpp"
#include "XRInput.hpp"
#include "XInputBridge.hpp"
#include "AimHook.hpp"
#include "WeaponAimSystem.hpp"
#include "../core/VRMod.hpp"
#include "../camera/CameraHook.hpp"
#include "../config/Config.hpp"
#include "../player/ArmIKSystem.hpp"
#include "../ui/Overlay.hpp"
#include "../xr/FrameLoop.hpp"
#include "../xr/OpenXRContext.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace bl1gotyvr { namespace input {

static void MultiplyMatrix(const float left[16], const float right[16],
                           float output[16]);

InputHook& InputHook::Instance() {
    static InputHook hook;
    return hook;
}

static bool ApplyHysteresis(float value, bool wasDown) {
    if (wasDown) return value > 0.45f;
    return value >= 0.55f;
}

static void RotateMeleeVector(const float quaternion[4], const float vector[3],
                              float output[3]) {
    const float tx = 2.0f * (quaternion[1] * vector[2] -
        quaternion[2] * vector[1]);
    const float ty = 2.0f * (quaternion[2] * vector[0] -
        quaternion[0] * vector[2]);
    const float tz = 2.0f * (quaternion[0] * vector[1] -
        quaternion[1] * vector[0]);
    output[0] = vector[0] + quaternion[3] * tx +
        (quaternion[1] * tz - quaternion[2] * ty);
    output[1] = vector[1] + quaternion[3] * ty +
        (quaternion[2] * tx - quaternion[0] * tz);
    output[2] = vector[2] + quaternion[3] * tz +
        (quaternion[0] * ty - quaternion[1] * tx);
}

static void XrVectorToUe(const float xr[3], float ue[3]) {
    ue[0] = -xr[2];
    ue[1] = xr[0];
    ue[2] = xr[1];
}

static void RotatePitchYawVec(const float vector[3], float pitch, float yaw,
                              float output[3]) {
    const float pitchSine = sinf(pitch);
    const float pitchCosine = cosf(pitch);
    const float pitchedX = pitchCosine * vector[0] - pitchSine * vector[2];
    const float pitchedZ = pitchSine * vector[0] + pitchCosine * vector[2];
    const float sine = sinf(yaw);
    const float cosine = cosf(yaw);
    output[0] = cosine * pitchedX - sine * vector[1];
    output[1] = sine * pitchedX + cosine * vector[1];
    output[2] = pitchedZ;
}

static void QuatMultiplyFast(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void Multiply3x3(const float a[9], const float b[9], float out[9]) {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            float sum = 0.0f;
            for (int index = 0; index < 3; ++index)
                sum += a[row * 3 + index] * b[index * 3 + column];
            out[row * 3 + column] = sum;
        }
    }
}

static void Transpose3x3(const float a[9], float out[9]) {
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out[row * 3 + column] = a[column * 3 + row];
}

// Hand world rotation (UE3, row-vector 3x3) from the XR controller pose,
// mirroring the ArmIK XrControllerToWorld conversion.
static bool HandWorldRotationFromXr(const float controllerRotation[4],
                                    const float referenceRotation[4],
                                    float pitch, float yaw, float out[9]) {
    const float inverseReference[4] = {
        -referenceRotation[0], -referenceRotation[1],
        -referenceRotation[2], referenceRotation[3]};
    float relative[4] = {};
    QuatMultiplyFast(inverseReference, controllerRotation, relative);
    const float length = sqrtf(relative[0] * relative[0] +
        relative[1] * relative[1] + relative[2] * relative[2] +
        relative[3] * relative[3]);
    if (!std::isfinite(length) || length < 1.0e-6f) return false;
    for (float& value : relative) value /= length;
    constexpr float kLocalForward[3] = {0.0f, 0.0f, -1.0f};
    constexpr float kLocalUp[3] = {0.0f, 1.0f, 0.0f};
    float xrForward[3] = {};
    float xrUp[3] = {};
    RotateMeleeVector(relative, kLocalForward, xrForward);
    RotateMeleeVector(relative, kLocalUp, xrUp);
    float mapped[3] = {};
    float ueForward[3] = {};
    float ueUp[3] = {};
    XrVectorToUe(xrForward, mapped);
    RotatePitchYawVec(mapped, pitch, yaw, ueForward);
    XrVectorToUe(xrUp, mapped);
    RotatePitchYawVec(mapped, pitch, yaw, ueUp);
    const float forwardLength = sqrtf(ueForward[0] * ueForward[0] +
        ueForward[1] * ueForward[1] + ueForward[2] * ueForward[2]);
    if (!std::isfinite(forwardLength) || forwardLength < 1.0e-5f) return false;
    for (float& value : ueForward) value /= forwardLength;
    float right[3] = {
        ueUp[1] * ueForward[2] - ueUp[2] * ueForward[1],
        ueUp[2] * ueForward[0] - ueUp[0] * ueForward[2],
        ueUp[0] * ueForward[1] - ueUp[1] * ueForward[0]};
    const float rightLength = sqrtf(right[0] * right[0] + right[1] * right[1] +
        right[2] * right[2]);
    if (!std::isfinite(rightLength) || rightLength < 1.0e-5f) return false;
    for (float& value : right) value /= rightLength;
    const float up[3] = {
        ueForward[1] * right[2] - ueForward[2] * right[1],
        ueForward[2] * right[0] - ueForward[0] * right[2],
        ueForward[0] * right[1] - ueForward[1] * right[0]};
    out[0] = right[0]; out[1] = right[1]; out[2] = right[2];
    out[3] = up[0]; out[4] = up[1]; out[5] = up[2];
    out[6] = -ueForward[0]; out[7] = -ueForward[1]; out[8] = -ueForward[2];
    return true;
}

// XR stage position (meters) -> UE3 world (unit-meters*scale), mirroring the
// ArmIK world mapping.
static bool StagePositionToUeWorld(const float xr[3], float out[3]) {
    const float* cameraLocation = camera::GetCameraLocation();
    const int32_t* cameraRotation = camera::GetCameraRotation();
    float referencePosition[3] = {};
    float referenceRotation[4] = {};
    if (!cameraLocation || !cameraRotation ||
        !camera::GetPoseReference(referencePosition, referenceRotation))
        return false;
    const float inverseReference[4] = {
        -referenceRotation[0], -referenceRotation[1],
        -referenceRotation[2], referenceRotation[3]};
    const float delta[3] = {
        xr[0] - referencePosition[0],
        xr[1] - referencePosition[1],
        xr[2] - referencePosition[2]};
    float localXr[3] = {};
    RotateMeleeVector(inverseReference, delta, localXr);
    float ue[3] = {};
    XrVectorToUe(localXr, ue);
    const float yaw = static_cast<float>(cameraRotation[1]) *
        6.2831853071795864769f / 65536.0f;
    const float sine = sinf(yaw);
    const float cosine = cosf(yaw);
    const float scale = 100.0f * config::Get().positional_scale;
    out[0] = cameraLocation[0] + (cosine * ue[0] - sine * ue[1]) * scale;
    out[1] = cameraLocation[1] + (sine * ue[0] + cosine * ue[1]) * scale;
    out[2] = cameraLocation[2] + ue[2] * scale;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void InputHook::Install() {
    if (m_installed) return;
    Log("[Input] Installing VR input system...");

    m_gameWindow = FindWindowA(NULL, "BorderlandsGOTY");
    if (!m_gameWindow) m_gameWindow = FindWindowA("LaunchUnrealUWindowsClient", NULL);
    Log("[Input] Game window: %p", m_gameWindow);

    if (!QueryPerformanceFrequency(&m_perfFrequency))
        m_perfFrequency.QuadPart = 10000000LL;
    QueryPerformanceCounter(&m_lastFrameTime);
    m_perfInitialized = true;

    // Initialize decoupled aim system
    AimHook::Instance().Initialize();
    m_aimTrimPitch = config::Get().aim_pitch_degrees;
    m_aimTrimYaw = config::Get().aim_yaw_degrees;
    m_defaultAimTrimPitch = 0.0f;
    m_defaultAimTrimYaw = 0.0f;
    m_aimTuningKeysDown = 0;
    memset(m_aimTuningNextRepeatMs, 0, sizeof(m_aimTuningNextRepeatMs));
    m_handTuningHand = 1;
    m_handTuningDirty = false;
    m_handTuningLastChangedMs = 0;
    m_aimTuningDirty = false;
    m_activeWeaponTrimPitch.store(0.0f, std::memory_order_release);
    m_activeWeaponTrimYaw.store(0.0f, std::memory_order_release);
    m_activeWeaponTrimRoll.store(0.0f, std::memory_order_release);
    m_activeWeaponOffsetForward.store(0.0f, std::memory_order_release);
    m_activeWeaponOffsetRight.store(0.0f, std::memory_order_release);
    m_activeWeaponOffsetUp.store(0.0f, std::memory_order_release);
    m_activeWeaponTrimValid.store(false, std::memory_order_release);
    memset(m_weaponAimProfiles, 0, sizeof(m_weaponAimProfiles));
    m_weaponAimProfileCursor = 0;
    m_activeWeaponAimProfile = -1;
    m_weaponMountValid = false;
    m_weaponMountAbsolute = false;
    m_weaponNativeMatrixValid = false;
    m_mountIdentityGeneration = 0;
    memset(m_weaponMountCache, 0, sizeof(m_weaponMountCache));
    m_weaponMountCacheCursor = 0;
    m_pendingWeaponIdentityGeneration = 0;
    m_pendingWeaponPawn = 0;
    m_pendingWeapon = 0;
    m_pendingWeaponComponent = 0;
    m_weaponIdentityStableSinceMs = 0;
    m_motionControlsEnabled.store(true, std::memory_order_release);
    m_dotVisibleAtMs.store(GetTickCount64(), std::memory_order_release);
    m_xinputActive = XInputBridge::Instance().Initialize();

    m_installed = true;
    Log("[Input] Installed via %s (turn_mode=%d smooth=%.0f snap=%.0f deadzone=%.2f weapon_pos=%.2f)",
        m_xinputActive ? "XInput" : "keyboard/mouse fallback",
        config::Get().turn_mode, config::Get().smooth_turn_speed,
        config::Get().snap_turn_angle, config::Get().locomotion_deadzone,
        config::Get().weapon_position_scale);
    Log("[Input] Map: sticks analog, RT fire, LT ADS, A jump, B Esc/back, "
        "X use/reload (shotgun: LT grabs mag), Y cycle, LB skill, "
        "RB grenade, L3 sprint, R3 crouch");
    Log("[Input] Y chord: tap=Y, hold 400ms=Back/ECHO, hold+left stick=D-pad");
    Log("[Input] UI navigation: left stick vertical=D-pad up/down; melee=physical VR swing");
    Log("[Input] Calibration: Ctrl+Numpad 1=global left hand, 3=active weapon, "
        "0 toggles rotation/position, 8/2 4/6 7/9 adjust axes, "
        "5 resets current mode; release Ctrl to save");
}

static bool IsGameForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg) return false;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void InputHook::PressKey(int vk) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    SendInput(1, &input, sizeof(INPUT));
}

void InputHook::ReleaseKey(int vk) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputHook::ReleaseAllInput() {
    WeaponAimSystem::Instance().SetFireActive(false);
    WeaponAimSystem::Instance().SetVehicleSecondaryFireActive(false);
    XInputBridge::Instance().ReleaseVrState();
    INPUT inputs[24] = {};
    int count = 0;
#define RELEASE(s, k) if (s) { inputs[count].type=INPUT_KEYBOARD; inputs[count].ki.wVk=k; inputs[count++].ki.dwFlags=KEYEVENTF_KEYUP; }
    RELEASE(m_prevGrip, 'F')
    RELEASE(m_prevW, 'W')
    RELEASE(m_prevA, 'A')
    RELEASE(m_prevS, 'S')
    RELEASE(m_prevD, 'D')
    RELEASE(m_prevSprint, VK_LSHIFT)
    RELEASE(m_prevJump, VK_SPACE)
    RELEASE(m_prevMelee, 'V')
    RELEASE(m_prevCrouch, 'C')
    RELEASE(m_prevUse, 'E')
    RELEASE(m_prevReload, 'R')
    RELEASE(m_prevGrenade, 'G')
    RELEASE(m_prevMenu, VK_ESCAPE)
    RELEASE(m_prevEcho, VK_TAB)
    RELEASE(m_prevDpadUp, '1')
    RELEASE(m_prevDpadRight, '2')
    RELEASE(m_prevDpadDown, '3')
    RELEASE(m_prevDpadLeft, '4')
    RELEASE(m_prevMenuUp, VK_UP)
    RELEASE(m_prevMenuDown, VK_DOWN)
#undef RELEASE
    if (count) SendInput(count, inputs, sizeof(INPUT));
    m_prevGrip = m_prevW = m_prevA = m_prevS = m_prevD = 0;
    m_prevSprint = m_prevJump = m_prevMelee = 0;
    m_prevCrouch = m_prevUse = m_prevReload = m_prevGrenade = 0;
    m_prevMenu = m_prevEcho = 0;
    m_prevDpadUp = m_prevDpadDown = m_prevDpadLeft = m_prevDpadRight = 0;
    m_prevMenuUp = m_prevMenuDown = 0;
    m_prevWeaponCycle = 0;

    INPUT mouse[3] = {};
    int mcount = 0;
    if (m_prevTrigger) { mouse[mcount].type=INPUT_MOUSE; mouse[mcount++].mi.dwFlags=MOUSEEVENTF_LEFTUP; }
    if (m_prevButtonA) { mouse[mcount].type=INPUT_MOUSE; mouse[mcount++].mi.dwFlags=MOUSEEVENTF_RIGHTUP; }
    if (m_prevVehicleAltFire) { mouse[mcount].type=INPUT_MOUSE; mouse[mcount++].mi.dwFlags=MOUSEEVENTF_RIGHTUP; }
    if (mcount) SendInput(mcount, mouse, sizeof(INPUT));
    m_prevTrigger = m_prevButtonA = m_prevVehicleAltFire = 0;
    m_leftTriggerDown = m_rightTriggerDown = false;
    m_leftTriggerWasDown = false;
    m_leftGripDown = m_rightGripDown = false;
    m_skillGripWasDown = false;
    m_berserkActivationUntilMs = 0;
    m_berserkPunchMode.store(false, std::memory_order_release);
    m_yWasDown = m_yChordUsed = false;
    m_recenterChordLatched = false;
    m_recenterChordStartedMs = 0;
    m_yPressMs = m_yTapPulseUntilMs = 0;
    m_physicalCrouchPulseUntilMs = 0;
    m_physicalCrouchGameState = false;
    m_manualCrouchDesired = false;
    m_rightStickWasDown = false;
    m_rightStickTurnSuppressed = false;
    m_vrCrouchActive.store(false, std::memory_order_release);
    m_nativeCrouchCameraActive.store(false, std::memory_order_release);
    m_vrCrouchTransitionUntilMs.store(0, std::memory_order_release);
    m_weaponGrabArmed.store(false, std::memory_order_release);
    m_weaponGrabHeld.store(false, std::memory_order_release);
    m_weaponGrabCancelRequested.store(false, std::memory_order_release);
    m_weaponGrabLatched = false;
    m_snapTurnAccum = 0;
    ResetPhysicalMelee();
    ResetManualReload();
    if (m_outputLive) {
        Log("[Input] Output neutralized (focus, tracking, or action sync unavailable)");
        m_outputLive = false;
    }
}

void InputHook::ResetPhysicalMelee(int hand) {
    const int first = hand >= 0 ? hand : 0;
    const int last = hand >= 0 ? hand : 1;
    for (int index = first; index <= last; ++index)
        m_melee[index] = {};
}

bool InputHook::UpdatePhysicalMelee(const ControllerState& controller, int hand,
                                    uint64_t nowMs, bool fistMode) {
    if (hand < 0 || hand > 1) return false;
    if (!controller.aimValid ||
        !m_motionControlsEnabled.load(std::memory_order_acquire)) {
        ResetPhysicalMelee(hand);
        return false;
    }
    PhysicalMeleeTracker& tracker = m_melee[hand];

    float headPosition[3] = {};
    float headRotation[4] = {};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (!xr::OpenXRContext::Instance().GetPoseSnapshot(
            headPosition, headRotation, views)) {
        ResetPhysicalMelee(hand);
        return false;
    }

    constexpr float kLocalForward[3] = {0.0f, 0.0f, -1.0f};
    float controllerForward[3] = {};
    float headForward[3] = {};
    RotateMeleeVector(controller.aimRotation, kLocalForward, controllerForward);
    RotateMeleeVector(headRotation, kLocalForward, headForward);
    const float weaponLengthMeters = fistMode ? 0.12f : 0.45f;
    float tip[3] = {
        controller.aimPosition[0] + controllerForward[0] * weaponLengthMeters,
        controller.aimPosition[1] + controllerForward[1] * weaponLengthMeters,
        controller.aimPosition[2] + controllerForward[2] * weaponLengthMeters};
    const float headToController[3] = {
        controller.aimPosition[0] - headPosition[0],
        controller.aimPosition[1] - headPosition[1],
        controller.aimPosition[2] - headPosition[2]};
    const float frontDistance = headToController[0] * headForward[0] +
        headToController[1] * headForward[1] +
        headToController[2] * headForward[2];
    const float controllerDistanceSq =
        headToController[0] * headToController[0] +
        headToController[1] * headToController[1] +
        headToController[2] * headToController[2];
    if (!std::isfinite(tip[0]) || !std::isfinite(tip[1]) ||
        !std::isfinite(tip[2]) || !std::isfinite(frontDistance) ||
        !std::isfinite(controllerDistanceSq)) {
        ResetPhysicalMelee(hand);
        return false;
    }

    auto seedTip = [&]() {
        memcpy(tracker.previousTip, tip, sizeof(tracker.previousTip));
        tracker.previousSampleMs = nowMs;
        tracker.tipValid = true;
    };
    const bool inFront = frontDistance >= 0.08f && controllerDistanceSq <= 2.25f;
    if (!inFront) {
        seedTip();
        tracker.filteredSpeed = 0.0f;
        tracker.travel = 0.0f;
        tracker.belowThresholdSinceMs = nowMs;
        if (!tracker.ready && nowMs >= tracker.cooldownUntilMs)
            tracker.ready = true;
        return nowMs < tracker.pulseUntilMs;
    }
    if (!tracker.tipValid || !tracker.previousSampleMs) {
        seedTip();
        return false;
    }

    const uint64_t elapsedMs = nowMs - tracker.previousSampleMs;
    const float delta[3] = {
        tip[0] - tracker.previousTip[0], tip[1] - tracker.previousTip[1],
        tip[2] - tracker.previousTip[2]};
    const float distance = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] +
                                 delta[2] * delta[2]);
    if (elapsedMs < 2 || elapsedMs > 100 || !std::isfinite(distance) ||
        distance > 0.80f) {
        seedTip();
        tracker.filteredSpeed = 0.0f;
        tracker.travel = 0.0f;
        return nowMs < tracker.pulseUntilMs;
    }
    seedTip();

    const float speed = distance * 1000.0f / static_cast<float>(elapsedMs);
    const float verticalSpeed = delta[1] * 1000.0f /
        static_cast<float>(elapsedMs);
    const float downwardRatio = speed > 0.01f
        ? (-verticalSpeed / speed) : 0.0f;
    const bool downwardWeaponMelee = hand == 1 && !fistMode;
    tracker.filteredSpeed += (speed - tracker.filteredSpeed) * 0.45f;
    constexpr float kRearmSpeedMps = 0.65f;
    if (tracker.filteredSpeed <= kRearmSpeedMps) {
        tracker.travel = 0.0f;
        if (!tracker.belowThresholdSinceMs) tracker.belowThresholdSinceMs = nowMs;
        if (!tracker.ready && nowMs >= tracker.cooldownUntilMs &&
            nowMs - tracker.belowThresholdSinceMs >= 120) {
            tracker.ready = true;
        }
    } else {
        tracker.belowThresholdSinceMs = 0;
        if (tracker.ready) {
            if (downwardWeaponMelee) {
                if (delta[1] < 0.0f && downwardRatio >= 0.45f)
                    tracker.travel += -delta[1];
                else if (delta[1] >= 0.0f || downwardRatio < 0.25f)
                    tracker.travel = 0.0f;
            } else {
                tracker.travel += distance;
            }
        }
    }

    constexpr float kTriggerSpeedMps = 1.75f;
    constexpr float kTriggerTravelMeters = 0.10f;
    const bool directionValid = !downwardWeaponMelee ||
        (verticalSpeed <= -1.20f && downwardRatio >= 0.65f);
    if (tracker.ready && nowMs >= tracker.cooldownUntilMs &&
        tracker.filteredSpeed >= kTriggerSpeedMps &&
        tracker.travel >= kTriggerTravelMeters && directionValid) {
        const float measuredTravel = tracker.travel;
        tracker.ready = false;
        tracker.travel = 0.0f;
        tracker.pulseUntilMs = nowMs + 90;
        tracker.cooldownUntilMs = nowMs + (fistMode ? 300 : 500);
        Log("[Input] %s VR %s triggered: speed=%.2fm/s vertical=%.2fm/s "
            "travel=%.3fm front=%.2fm",
            hand == 0 ? "Left" : "Right", fistMode ? "Berserk punch" : "melee",
            tracker.filteredSpeed, verticalSpeed, measuredTravel, frontDistance);
    }
    return nowMs < tracker.pulseUntilMs;
}

void InputHook::ResetManualReload() {
    m_shotgunReload = {};
    PublishMagHold(0);
}

bool InputHook::UpdateShotgunReloadSequence(const ControllerState controllers[2],
                                            bool xDown, bool usableTargeted,
                                            bool actionDown, bool squeezeDown,
                                            uint64_t nowMs) {
    ShotgunReloadSequencer& seq = m_shotgunReload;
    // Releasing the magazine is a hold gesture: X must stay down long enough
    // before the magazine leaves the weapon, leaving quick taps free for the
    // game instead of swallowing them.
    constexpr uint64_t kMagReleaseHoldMs = 350;
    const bool pressed = xDown && !seq.xWasDown;
    seq.xWasDown = xDown;
    if (xDown) {
        if (seq.xHoldSinceMs == 0) seq.xHoldSinceMs = nowMs;
    } else {
        seq.xHoldSinceMs = 0;
        seq.magReleaseArmed = false;
    }
    const bool holdSatisfied = xDown && seq.xHoldSinceMs != 0 &&
        nowMs - seq.xHoldSinceMs >= kMagReleaseHoldMs;
    const bool actionWasDown = seq.gripWasDown;
    const bool gripPressed = actionDown && !seq.gripWasDown;
    const bool actionReleased = !actionDown && actionWasDown;
    seq.gripWasDown = actionDown;
    auto& xrInput = XRInput::Instance();
    if (pressed) {
        if (usableTargeted) {
            // A usable is under the crosshair: let X do its contextual use.
            seq.forwardCurrentPress = true;
        } else if (seq.phase != ShotgunReloadSequencer::Phase::Idle) {
            // Second press during an active sequence: cancel back to Idle.
            // Only a fresh edge cancels, so holding X through the release does
            // not abort it. The vanilla reload stays fully suppressed.
            seq.phase = ShotgunReloadSequencer::Phase::Idle;
            seq.cooldownUntilMs = nowMs + 300;
            seq.wellLatched = false;
            seq.pouchLatched = false;
            seq.wellAnchorValid = false;
            seq.forwardCurrentPress = false;
            seq.magReleaseArmed = false;
            PublishMagHold(0);
            xrInput.SendHaptic(0, 0.3f, 40.0f);
            Log("[ManualReload] Sequence cancelled");
        } else {
            // Start counting the hold; the magazine leaves once it completes.
            seq.magReleaseArmed = true;
        }
    }
    if (!usableTargeted && seq.magReleaseArmed && holdSatisfied &&
        seq.phase == ShotgunReloadSequencer::Phase::Idle &&
        nowMs >= seq.cooldownUntilMs) {
        seq.phase = ShotgunReloadSequencer::Phase::MagReleased;
        seq.enteredPhaseMs = nowMs;
        seq.magReleaseArmed = false;
        seq.wellLatched = false;
        seq.pouchLatched = false;
        seq.forwardCurrentPress = false;
        xrInput.SendHaptic(1, 0.6f, 70.0f);
        if (WeaponAimSystem::Instance().IsEquippedWeaponRevolver()) {
            PublishMagHold(3);
            Log("[ManualReload] Cylinder released to the side — grab it "
                "with the left hand");
        } else {
            Log("[ManualReload] Mag released — pull it out with the left hand");
        }
    }
    const bool forward = xDown && (seq.forwardCurrentPress || usableTargeted);

    // Left-hand vertical speed for the hammer flick gesture.
    float speedUp = 0.0f;
    if (controllers[0].valid && seq.lastLeftPosMs != 0 &&
        nowMs > seq.lastLeftPosMs && nowMs - seq.lastLeftPosMs < 250) {
        const float dtMs = static_cast<float>(nowMs - seq.lastLeftPosMs);
        if (dtMs > 1.0f)
            speedUp = (controllers[0].position[1] - seq.lastLeftPos[1]) /
                (dtMs / 1000.0f);
    }
    memcpy(seq.lastLeftPos, controllers[0].position, sizeof(seq.lastLeftPos));
    seq.lastLeftPosMs = nowMs;
    if (speedUp < 0.3f) seq.flickArmed = true;

    // Revolver extras while idle: spin the cylinder with the squeeze held
    // near it, or cock the hammer with an upward flick near it.
    const bool revolverTricks = config::Get().manual_reload_shotgun &&
        WeaponAimSystem::Instance().IsEquippedWeaponRevolver();
    bool spinningThisFrame = false;
    if (revolverTricks && seq.phase == ShotgunReloadSequencer::Phase::Idle &&
        controllers[0].valid) {
        float handUe[3] = {};
        if (StagePositionToUeWorld(controllers[0].position, handUe)) {
            float cylWorld[3] = {};
            const int32_t magIndex =
                WeaponAimSystem::Instance().GetClipBoneIndex();
            if (magIndex >= 0 && GetWeaponBoneWorld(magIndex, cylWorld)) {
                const float dx = handUe[0] - cylWorld[0];
                const float dy = handUe[1] - cylWorld[1];
                const float dz = handUe[2] - cylWorld[2];
                const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
                if (squeezeDown && distance <= 22.0f) {
                    spinningThisFrame = true;
                    if (!seq.spinWasActive)
                        Log("[ManualReload] Cylinder spin start d=%.2f", distance);
                    if (nowMs - seq.lastSpinHapticMs >= 150) {
                        xrInput.SendHaptic(0, 0.35f, 50.0f);
                        seq.lastSpinHapticMs = nowMs;
                    }
                }
            }
            const int32_t hammerIndex =
                WeaponAimSystem::Instance().GetHammerBoneIndex();
            if (hammerIndex >= 0 && seq.flickArmed && speedUp > 0.9f) {
                float hammerWorld[3] = {};
                if (GetWeaponBoneWorld(hammerIndex, hammerWorld)) {
                    const float hx = handUe[0] - hammerWorld[0];
                    const float hy = handUe[1] - hammerWorld[1];
                    const float hz = handUe[2] - hammerWorld[2];
                    if (sqrtf(hx * hx + hy * hy + hz * hz) <= 22.0f) {
                        // Fan: fire a shot; the vanilla shoot animation drives
                        // the hammer naturally (no manual cock override).
                        XRInput::Instance().SendHaptic(0, 1.0f, 80.0f);
                        m_fanFireUntilMs = nowMs + 130;
                        seq.flickArmed = false;
                        Log("[ManualReload] Fan fire");
                    }
                }
            }
        }
    }
    m_cylinderSpinning.store(spinningThisFrame, std::memory_order_release);
    if (!spinningThisFrame && seq.spinWasActive) RestoreMagBoneSeated();
    seq.spinWasActive = spinningThisFrame;

    if (seq.phase == ShotgunReloadSequencer::Phase::Idle) {
        seq.gripCaptured = false;
        m_reloadHighlightActive.store(false, std::memory_order_release);
        return forward;
    }

    const ControllerState& offHand = controllers[0];
    const ControllerState& weaponHand = controllers[1];
    if (!offHand.aimValid || !weaponHand.aimValid ||
        !m_motionControlsEnabled.load(std::memory_order_acquire)) {
        seq.gripCaptured = false;
        m_reloadHighlightActive.store(false, std::memory_order_release);
        return forward;
    }
    constexpr float kLocalForward[3] = {0.0f, 0.0f, -1.0f};
    constexpr float kLocalRight[3] = {1.0f, 0.0f, 0.0f};
    float weaponForward[3] = {};
    RotateMeleeVector(weaponHand.aimRotation, kLocalForward, weaponForward);
    if (!std::isfinite(weaponForward[0]) || !std::isfinite(weaponForward[1]) ||
        !std::isfinite(weaponForward[2]))
        return forward;
    constexpr float kLocalUp[3] = {0.0f, 1.0f, 0.0f};
    float weaponUp[3] = {};
    RotateMeleeVector(weaponHand.aimRotation, kLocalUp, weaponUp);
    float weaponRight[3] = {
        weaponForward[1] * weaponUp[2] - weaponForward[2] * weaponUp[1],
        weaponForward[2] * weaponUp[0] - weaponForward[0] * weaponUp[2],
        weaponForward[0] * weaponUp[1] - weaponForward[1] * weaponUp[0]};
    // Mag well: primary source is the weapon mesh's b_clip bone (index 25),
    // read in UE3 world space and mapped to XR with the same axis remap the
    // aim code uses (ue -> xr: (y, z, -x), cm -> m). Falls back to the
    // calibrated grip-space estimate when the bone read is unavailable.
    constexpr float kWellForwardMeters = 0.15f;
    constexpr float kWellUpMeters = -0.12f;
    constexpr float kWellRightMeters = -0.10f;
    float headPosition[3] = {};
    float headRotation[4] = {};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (!xr::OpenXRContext::Instance().GetPoseSnapshot(
            headPosition, headRotation, views)) {
        m_reloadHighlightActive.store(false, std::memory_order_release);
        return forward;
    }
    float well[3] = {
        weaponHand.position[0] + weaponForward[0] * kWellForwardMeters +
            weaponUp[0] * kWellUpMeters + weaponRight[0] * kWellRightMeters,
        weaponHand.position[1] + weaponForward[1] * kWellForwardMeters +
            weaponUp[1] * kWellUpMeters + weaponRight[1] * kWellRightMeters,
        weaponHand.position[2] + weaponForward[2] * kWellForwardMeters +
            weaponUp[2] * kWellUpMeters + weaponRight[2] * kWellRightMeters};
    bool wellFromBone = false;
    {
        float clipWorld[3] = {};
        const float* cameraLocation = camera::GetCameraLocation();
        const int32_t* cameraRotation = camera::GetCameraRotation();
        float referencePosition[3] = {};
        float referenceRotation[4] = {};
        if (cameraLocation && cameraRotation &&
            camera::GetPoseReference(referencePosition, referenceRotation) &&
            GetWeaponClipWorldPosition(clipWorld)) {
            const float scale = 100.0f * config::Get().positional_scale;
            const float yaw = static_cast<float>(cameraRotation[1]) *
                6.2831853071795864769f / 65536.0f;
            const float relative[3] = {
                (clipWorld[0] - cameraLocation[0]) / scale,
                (clipWorld[1] - cameraLocation[1]) / scale,
                (clipWorld[2] - cameraLocation[2]) / scale};
            if (scale > 1.0f && std::isfinite(relative[0]) &&
                std::isfinite(relative[1]) && std::isfinite(relative[2]) &&
                fabsf(relative[0]) < 10.0f && fabsf(relative[1]) < 10.0f &&
                fabsf(relative[2]) < 10.0f) {
                // Exact inverse of the ArmIK world mapping: undo yaw, apply
                // UeToXr, then the pose reference orientation/position.
                const float sine = sinf(-yaw);
                const float cosine = cosf(-yaw);
                const float yawedX = cosine * relative[0] - sine * relative[1];
                const float yawedY = sine * relative[0] + cosine * relative[1];
                const float yawedZ = relative[2];
                const float localXr[3] = {yawedY, yawedZ, -yawedX};
                float stageOffset[3] = {};
                RotateMeleeVector(referenceRotation, localXr, stageOffset);
                well[0] = referencePosition[0] + stageOffset[0];
                well[1] = referencePosition[1] + stageOffset[1];
                well[2] = referencePosition[2] + stageOffset[2];
                wellFromBone = true;
            }
        }
    }
    if (seq.phase == ShotgunReloadSequencer::Phase::Idle) seq.wellAnchorValid = false;
    if (seq.wellAnchorValid) {
        // While the mag is carried, keep the well at its seated position so
        // insertion detection and the glow do not chase the left hand.
        well[0] = seq.wellAnchor[0];
        well[1] = seq.wellAnchor[1];
        well[2] = seq.wellAnchor[2];
    } else if (wellFromBone) {
        // The bone origin sits at the mag's insertion point (top); drop the
        // well onto the visible magazine body below it.
        well[1] -= 0.08f;
    }
    float headRight[3] = {};
    RotateMeleeVector(headRotation, kLocalRight, headRight);
    float headForwardVec[3] = {};
    RotateMeleeVector(headRotation, kLocalForward, headForwardVec);
    float headUpVec[3] = {};
    RotateMeleeVector(headRotation, kLocalUp, headUpVec);
    // Ammo pouch at the natural left hip, calibrated from the in-game Grip
    // log: ~0.65m below the head, ~0.32m to the left, at the body plane.
    float pouch[3] = {
        headPosition[0] + headForwardVec[0] * 0.02f -
            headUpVec[0] * 0.65f - headRight[0] * 0.32f,
        headPosition[1] + headForwardVec[1] * 0.02f -
            headUpVec[1] * 0.65f - headRight[1] * 0.32f,
        headPosition[2] + headForwardVec[2] * 0.02f -
            headUpVec[2] * 0.65f - headRight[2] * 0.32f};
    auto distance3 = [](const float a[3], const float b[3]) {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    const float dWell = distance3(offHand.aimPosition, well);
    const float dPouch = distance3(offHand.aimPosition, pouch);
    if (!std::isfinite(dWell) || !std::isfinite(dPouch)) return forward;
    constexpr float kWellGlow = 0.30f;
    constexpr float kWellExtract = 0.26f;
    constexpr float kPouchGlow = 0.45f;
    constexpr float kPouchExtract = 0.42f;

    bool highlight = false;
    float highlightPos[3] = {};
    float highlightRadius = 0.0f;
    bool gripCaptured = false;
    const ShotgunReloadSequencer::Phase phaseBefore = seq.phase;
    if (gripPressed) {
        const float toWell[3] = {
            offHand.aimPosition[0] - well[0],
            offHand.aimPosition[1] - well[1],
            offHand.aimPosition[2] - well[2]};
        const float corrFwd = toWell[0] * weaponForward[0] +
            toWell[1] * weaponForward[1] + toWell[2] * weaponForward[2];
        const float corrUp = toWell[0] * weaponUp[0] +
            toWell[1] * weaponUp[1] + toWell[2] * weaponUp[2];
        const float corrRight = toWell[0] * weaponRight[0] +
            toWell[1] * weaponRight[1] + toWell[2] * weaponRight[2];
        float headForward[3] = {};
        float headUp[3] = {};
        float headRight[3] = {};
        RotateMeleeVector(headRotation, kLocalForward, headForward);
        RotateMeleeVector(headRotation, kLocalUp, headUp);
        RotateMeleeVector(headRotation, kLocalRight, headRight);
        const float toPouch[3] = {
            offHand.aimPosition[0] - pouch[0],
            offHand.aimPosition[1] - pouch[1],
            offHand.aimPosition[2] - pouch[2]};
        const float pouchFwd = toPouch[0] * headForward[0] +
            toPouch[1] * headForward[1] + toPouch[2] * headForward[2];
        const float pouchUp = toPouch[0] * headUp[0] +
            toPouch[1] * headUp[1] + toPouch[2] * headUp[2];
        const float pouchRight = toPouch[0] * headRight[0] +
            toPouch[1] * headRight[1] + toPouch[2] * headRight[2];
        Log("[ManualReload] Grip: phase=%d dWell=%.2f dPouch=%.2f src=%s "
            "mag(fwd=%.2f up=%.2f right=%.2f) pouch(fwd=%.2f up=%.2f right=%.2f)",
            static_cast<int>(seq.phase), dWell, dPouch,
            wellFromBone ? "bone" : "fallback",
            corrFwd, corrUp, corrRight, pouchFwd, pouchUp, pouchRight);
    }
    auto publishThrowFromHand = [&](const float velocityXr[3]) {
        float magWorld[3] = {};
        if (!GetWeaponClipWorldPosition(magWorld)) {
            PublishMagHold(0);
            return;
        }
        const int32_t* cameraRotation = camera::GetCameraRotation();
        const float yaw = cameraRotation
            ? static_cast<float>(cameraRotation[1]) *
                6.2831853071795864769f / 65536.0f
            : 0.0f;
        float velocityUe[3] = {};
        XrVectorToUe(velocityXr, velocityUe);
        const float sine = sinf(yaw);
        const float cosine = cosf(yaw);
        const float velocityX = cosine * velocityUe[0] - sine * velocityUe[1];
        const float velocityY = sine * velocityUe[0] + cosine * velocityUe[1];
        const float scale = 100.0f * config::Get().positional_scale;
        const float velocityWorld[3] = {
            velocityX * scale, velocityY * scale, velocityUe[2] * scale};
        PublishMagThrow(magWorld, velocityWorld);
    };
    auto seatMag = [&]() {
        seq.phase = ShotgunReloadSequencer::Phase::Idle;
        seq.cooldownUntilMs = nowMs + 500;
        seq.enteredPhaseMs = nowMs;
        seq.wellAnchorValid = false;
        PublishMagHold(0);
        xrInput.SendHaptic(0, 1.0f, 100.0f);
        xrInput.SendHaptic(1, 0.5f, 60.0f);
        if (WeaponAimSystem::Instance().RefillEquippedWeaponClip()) {
            Log("[ManualReload] Mag inserted -> clip refilled (no animation)");
        } else {
            seq.keyboardReloadPulseUntilMs = nowMs + 140;
            Log("[ManualReload] Mag inserted -> direct refill failed, "
                "classic reload pulse");
        }
    };
    switch (seq.phase) {
    case ShotgunReloadSequencer::Phase::MagReleased:
        if (dWell <= kWellGlow) {
            highlight = true;
            highlightRadius = 0.11f;
            memcpy(highlightPos, well, sizeof(highlightPos));
            gripCaptured = true;
            if (dWell <= kWellExtract && gripPressed) {
                seq.phase = ShotgunReloadSequencer::Phase::MagOut;
                seq.enteredPhaseMs = nowMs;
                memcpy(seq.wellAnchor, well, sizeof(seq.wellAnchor));
                seq.wellAnchorValid = true;
                PublishMagHold(1);
                xrInput.SendHaptic(0, 0.9f, 90.0f);
                Log("[ManualReload] Mag grabbed — carry it (release at the "
                    "weapon to insert, anywhere else to drop)");
            }
        }
        break;
    case ShotgunReloadSequencer::Phase::MagOut:
        if (dPouch <= kPouchGlow) {
            highlight = true;
            highlightRadius = 0.15f;
            memcpy(highlightPos, pouch, sizeof(highlightPos));
            gripCaptured = true;
        }
        if (actionReleased) {
            // The carried mag is the spent one: releasing never inserts it,
            // it always gets dropped; the fresh mag comes from the pouch.
            const float velocityXr[3] = {-0.4f, -1.3f, 0.5f};
            publishThrowFromHand(velocityXr);
            seq.phase = ShotgunReloadSequencer::Phase::NeedMag;
            seq.enteredPhaseMs = nowMs;
            xrInput.SendHaptic(0, 0.5f, 70.0f);
            Log("[ManualReload] Mag dropped — grab a fresh one from the "
                "left-hip pouch");
        }
        break;
    case ShotgunReloadSequencer::Phase::NeedMag:
        if (dPouch <= kPouchGlow) {
            highlight = true;
            highlightRadius = 0.15f;
            memcpy(highlightPos, pouch, sizeof(highlightPos));
            gripCaptured = true;
            if (dPouch <= kPouchExtract && gripPressed) {
                seq.phase = ShotgunReloadSequencer::Phase::NewMag;
                seq.enteredPhaseMs = nowMs;
                PublishMagHold(1);
                xrInput.SendHaptic(0, 0.9f, 90.0f);
                Log("[ManualReload] Fresh mag in hand — release at the weapon "
                    "to insert");
            }
        }
        break;
    case ShotgunReloadSequencer::Phase::NewMag:
        if (dWell <= kWellGlow) {
            highlight = true;
            highlightRadius = 0.11f;
            memcpy(highlightPos, well, sizeof(highlightPos));
        }
        if (actionReleased) {
            if (dWell <= 0.35f) {
                seatMag();
            } else {
                const float velocityXr[3] = {-0.4f, -1.3f, 0.5f};
                publishThrowFromHand(velocityXr);
                seq.phase = ShotgunReloadSequencer::Phase::NeedMag;
                seq.enteredPhaseMs = nowMs;
                xrInput.SendHaptic(0, 0.5f, 70.0f);
                Log("[ManualReload] Mag dropped — grab a fresh one from the "
                    "left-hip pouch");
            }
        }
        break;
    default:
        break;
    }
    if (gripPressed && phaseBefore != ShotgunReloadSequencer::Phase::Idle &&
        seq.phase == phaseBefore) {
        // Mis-press feedback: the squeeze registered but not near its target.
        xrInput.SendHaptic(0, 0.25f, 40.0f);
    }
    seq.gripCaptured = gripCaptured;
    m_reloadHighlightActive.store(highlight, std::memory_order_release);
    if (highlight) {
        m_reloadHighlightX.store(highlightPos[0], std::memory_order_relaxed);
        m_reloadHighlightY.store(highlightPos[1], std::memory_order_relaxed);
        m_reloadHighlightZ.store(highlightPos[2], std::memory_order_relaxed);
        m_reloadHighlightRadius.store(highlightRadius, std::memory_order_relaxed);
    }
    return forward;
}

bool InputHook::IsMagCarried() const {
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_magHoldLock));
    const int32_t mode = m_magHold.mode;
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_magHoldLock));
    return mode == 1;
}

bool InputHook::GetWeaponBoneWorld(int boneIndex, float out[3]) const {
    if (boneIndex < 0) return false;
    uintptr_t component = 0;
    int32_t matrixOffset = 0;
    uint64_t updatedMs = 0;
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_weaponPoseWriteLock));
    component = m_renderWeaponComponent;
    matrixOffset = m_renderWeaponMatrixOffset;
    updatedMs = m_renderWeaponStampUpdatedMs;
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_weaponPoseWriteLock));
    if (component < 0x10000 || matrixOffset < 0) return false;
    if (GetTickCount64() - updatedMs > 1000) return false;
    struct TArrayPose { uint64_t data; int32_t count; int32_t capacity; };
    constexpr uintptr_t kSpaceBasesOffset = 0x330;
    constexpr uint64_t kBoneMatrixStride = 0x40;
    float componentMatrix[16] = {};
    TArrayPose poses = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component +
                static_cast<uintptr_t>(matrixOffset)),
            componentMatrix, sizeof(componentMatrix), &read) ||
        read != sizeof(componentMatrix) ||
        !ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component + kSpaceBasesOffset),
            &poses, sizeof(poses), &read) || read != sizeof(poses) ||
        !poses.data || poses.count <= boneIndex || poses.count > 256 ||
        poses.capacity < poses.count)
        return false;
    float boneMatrix[16] = {};
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(poses.data +
                static_cast<uint64_t>(boneIndex) * kBoneMatrixStride),
            boneMatrix, sizeof(boneMatrix), &read) || read != sizeof(boneMatrix))
        return false;
    for (float value : boneMatrix)
        if (!std::isfinite(value) || fabsf(value) > 10000000.0f) return false;
    if (fabsf(boneMatrix[15] - 1.0f) > 0.05f) return false;
    float world[16] = {};
    MultiplyMatrix(boneMatrix, componentMatrix, world);
    out[0] = world[12];
    out[1] = world[13];
    out[2] = world[14];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

bool InputHook::GetReloadHighlight(float worldOut[3], float* radiusMeters) const {
    if (!m_reloadHighlightActive.load(std::memory_order_acquire)) return false;
    worldOut[0] = m_reloadHighlightX.load(std::memory_order_relaxed);
    worldOut[1] = m_reloadHighlightY.load(std::memory_order_relaxed);
    worldOut[2] = m_reloadHighlightZ.load(std::memory_order_relaxed);
    if (radiusMeters)
        *radiusMeters = m_reloadHighlightRadius.load(std::memory_order_relaxed);
    return true;
}

bool InputHook::GetWeaponClipWorldPosition(float out[3]) const {
    // The weapon mesh's b_clip bone (index 25 of 27) in UE3 world space: read
    // the live component matrix plus the component-space bone matrix array.
    uintptr_t component = 0;
    int32_t matrixOffset = 0;
    uint64_t updatedMs = 0;
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_weaponPoseWriteLock));
    component = m_renderWeaponComponent;
    matrixOffset = m_renderWeaponMatrixOffset;
    updatedMs = m_renderWeaponStampUpdatedMs;
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_weaponPoseWriteLock));
    if (component < 0x10000 || matrixOffset < 0) return false;
    if (GetTickCount64() - updatedMs > 1000) return false;
    struct TArrayPose { uint64_t data; int32_t count; int32_t capacity; };
    constexpr uintptr_t kSpaceBasesOffset = 0x330;
    const uint64_t kBoneMatrixStride = 0x40;
    const int32_t clipBoneIndex = WeaponAimSystem::Instance().GetClipBoneIndex();
    if (clipBoneIndex < 0) return false;
    float componentMatrix[16] = {};
    TArrayPose poses = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component +
                static_cast<uintptr_t>(matrixOffset)),
            componentMatrix, sizeof(componentMatrix), &read) ||
        read != sizeof(componentMatrix) ||
        !ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component + kSpaceBasesOffset),
            &poses, sizeof(poses), &read) || read != sizeof(poses) ||
        !poses.data || poses.count <= clipBoneIndex || poses.count > 256 ||
        poses.capacity < poses.count)
        return false;
    float boneMatrix[16] = {};
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(poses.data +
                static_cast<uint64_t>(clipBoneIndex) * kBoneMatrixStride),
            boneMatrix, sizeof(boneMatrix), &read) || read != sizeof(boneMatrix))
        return false;
    for (float value : boneMatrix)
        if (!std::isfinite(value) || fabsf(value) > 10000000.0f) return false;
    if (fabsf(boneMatrix[15] - 1.0f) > 0.05f) return false;
    float world[16] = {};
    MultiplyMatrix(boneMatrix, componentMatrix, world);
    out[0] = world[12];
    out[1] = world[13];
    out[2] = world[14];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void InputHook::ProcessTurn() {
    const auto& xr = XRInput::Instance();
    ControllerState controllers[2] = {};
    xr.GetControllerSnapshot(controllers);
    const auto& right = controllers[1];
    const auto& cfg = config::Get();

    if (cfg.turn_mode == 2 || !right.valid) return;
    if (m_rightStickTurnSuppressed) {
        m_snapTurnAccum = 0.0f;
        QueryPerformanceCounter(&m_lastFrameTime);
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = m_perfInitialized ?
        (float)(now.QuadPart - m_lastFrameTime.QuadPart) / (float)m_perfFrequency.QuadPart : 1.0f/60.0f;
    m_lastFrameTime = now;
    if (dt <= 0 || dt > 0.5f) dt = 1.0f/60.0f;

    float sx = right.thumbstickX;
    float deadzone = cfg.locomotion_deadzone;
    float magnitude = sx;
    if (magnitude > 0) {
        if (magnitude < deadzone) magnitude = 0;
        else magnitude = (magnitude - deadzone) / (1.0f - deadzone);
    } else {
        if (magnitude > -deadzone) magnitude = 0;
        else magnitude = (magnitude + deadzone) / (1.0f - deadzone);
    }
    if (magnitude == 0) { m_snapTurnAccum = 0; return; }

    if (cfg.turn_mode == 0) {
        /* Snap turn */
        m_snapTurnAccum += magnitude * cfg.smooth_turn_speed * dt;
        if (m_snapTurnAccum >= cfg.snap_turn_angle) {
            mouse_event(MOUSEEVENTF_MOVE, 3, 0, 0, 0);
            m_snapTurnAccum -= cfg.snap_turn_angle;
        } else if (m_snapTurnAccum <= -cfg.snap_turn_angle) {
            mouse_event(MOUSEEVENTF_MOVE, -3, 0, 0, 0);
            m_snapTurnAccum += cfg.snap_turn_angle;
        }
    } else {
        /* Smooth turn */
        float turnAmount = magnitude * cfg.smooth_turn_speed * dt;
        int dx = (int)(turnAmount * 0.5f);
        if (dx == 0) dx = (turnAmount > 0) ? 1 : -1;
        mouse_event(MOUSEEVENTF_MOVE, dx, 0, 0, 0);
    }
}

void InputHook::UpdateState(XrTime displayTime) {
    if (!m_installed) return;

    const bool foreground = IsGameForeground(m_gameWindow);
    if (foreground) PollAimTuningKeys();

    auto& xr = XRInput::Instance();
    if (!xr.SyncActions()) {
        ReleaseAllInput();
        return;
    }
    xr.UpdateControllerStates(displayTime);

    ControllerState controllers[2] = {};
    xr.GetControllerSnapshot(controllers);
    const auto& left = controllers[0];
    const auto& right = controllers[1];
    const auto& cfg = config::Get();
    if (m_logCounter++ % 300 == 0)
        Log("[Input] mode=%s leftValid=%d rightValid=%d foreground=%d "
            "RT=%.2f LT=%.2f sticks=L(%.2f,%.2f) R(%.2f,%.2f)",
            m_xinputActive ? "XInput" : "fallback", left.valid, right.valid, foreground,
            right.trigger, left.trigger,
            left.thumbstickX, left.thumbstickY,
            right.thumbstickX, right.thumbstickY);
    const uint64_t now = GetTickCount64();
    uint64_t expectedDotTime = 0;
    if (right.aimValid && m_motionControlsEnabled.load(std::memory_order_acquire) &&
        m_dotVisibleAtMs.compare_exchange_strong(
            expectedDotTime, now,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        Log("[Input] Aim dot enabled from tracked right controller");
    }
    if (!left.valid || !right.valid) { ReleaseAllInput(); return; }
    if (!foreground) { ReleaseAllInput(); return; }
    if (ui::IsVisible()) { ReleaseAllInput(); return; }
    if (!m_outputLive) {
        Log("[Input] Tracked controller output resumed via %s",
            m_xinputActive ? "XInput" : "keyboard/mouse fallback");
        m_outputLive = true;
    }

    // Update decoupled aim system with right controller orientation
    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    AimHook::Instance().UpdateAim(right.eulerPitch * kDegreesToRadians,
        right.eulerYaw * kDegreesToRadians, right.eulerRoll * kDegreesToRadians);

    m_rightTriggerDown = ApplyHysteresis(right.trigger, m_rightTriggerDown);
    m_leftTriggerWasDown = m_leftTriggerDown;
    m_leftTriggerDown = ApplyHysteresis(left.trigger, m_leftTriggerDown);
    m_leftGripDown = ApplyHysteresis(left.grip, m_leftGripDown);
    m_rightGripDown = ApplyHysteresis(right.grip, m_rightGripDown);
    const bool vehicleMode = camera::IsVehicleCameraActive();
    const bool magZoneCaptured = IsLeftGripCaptured();
    const bool leftTriggerPressed = m_leftTriggerDown && !m_leftTriggerWasDown;
    const bool weaponGrabCancelled = m_weaponGrabCancelRequested.exchange(
        false, std::memory_order_acq_rel);
    if (!m_leftTriggerDown || magZoneCaptured || weaponGrabCancelled || vehicleMode ||
        !m_motionControlsEnabled.load(std::memory_order_acquire)) {
        m_weaponGrabLatched = false;
    } else if (leftTriggerPressed) {
        m_weaponGrabLatched = m_weaponGrabArmed.load(std::memory_order_acquire);
        if (m_weaponGrabLatched)
            Log("[Input] Left trigger latched on weapon contact");
    }
    const bool weaponGrabHeld = m_leftTriggerDown && m_weaponGrabLatched &&
        !magZoneCaptured;
    m_weaponGrabHeld.store(weaponGrabHeld, std::memory_order_release);
    constexpr bool adsActive = false;
    const bool vehicleSecondaryFire = vehicleMode && m_rightGripDown;
    // The weapon cannot fire while its magazine is physically removed.
    const bool magOutBlocksFire = cfg.manual_reload_shotgun &&
        WeaponAimSystem::Instance().IsManualReloadWeapon() &&
        m_shotgunReload.phase != ShotgunReloadSequencer::Phase::Idle;
    const bool fireOutput = (m_rightTriggerDown || vehicleSecondaryFire ||
        GetTickCount64() < m_fanFireUntilMs) && !magOutBlocksFire;
    WeaponAimSystem::Instance().SetVehicleSecondaryFireActive(vehicleSecondaryFire);
    WeaponAimSystem::Instance().SetFireActive(fireOutput);
    camera::SuppressRecoilTick(fireOutput);
    static bool lastFireOutput = false;
    if (fireOutput != lastFireOutput) {
        lastFireOutput = fireOutput;
        const bool fanShot = !m_rightTriggerDown && !vehicleSecondaryFire;
        float aimDirection[3] = {0.0f, 0.0f, -1.0f};
        if (right.aimValid) {
            constexpr float kLocalForward[3] = {0.0f, 0.0f, -1.0f};
            RotateMeleeVector(right.aimRotation, kLocalForward, aimDirection);
        }
        const float aimPitchDeg = atan2f(aimDirection[1], -aimDirection[2]) *
            57.29578f;
        const float aimYawDeg = atan2f(aimDirection[0], -aimDirection[2]) *
            57.29578f;
        const float stampPitchDeg = static_cast<float>(
            WeaponAimSystem::Instance().GetAimPitchUnis()) * 360.0f / 65536.0f;
        const float stampYawDeg = static_cast<float>(
            WeaponAimSystem::Instance().GetAimYawUnis()) * 360.0f / 65536.0f;
        const int32_t* liveCamRot = camera::GetCameraRotation();
        const int32_t liveCamP = liveCamRot ? liveCamRot[0] : 0;
        const int32_t liveCamY = liveCamRot ? liveCamRot[1] : 0;
        Log("[Input] Fire %s src=%s aimValid=%d dir=(%.2f,%.2f,%.2f) pitch=%.1f yaw=%.1f stamp=(%.1f,%.1f) cam=(%d,%d) (RT=%.2f vehicle=%d)",
            fireOutput ? "pressed" : "released", fanShot ? "fan" : "trigger",
            right.aimValid ? 1 : 0, aimDirection[0], aimDirection[1],
            aimDirection[2], aimPitchDeg, aimYawDeg, stampPitchDeg, stampYawDeg,
            liveCamP, liveCamY, right.trigger,
            vehicleSecondaryFire ? 1 : 0);
    }

    const auto playerIdentity = WeaponAimSystem::Instance().GetPlayerIdentity();
    // Auto-recenter 3s after entering gameplay (pawn+weapon live), so the
    // player starts aligned without touching the recenter button.
    {
        static bool autoRecenterArmed = false;
        static bool wasInGame = false;
        static uint64_t autoRecenterAtMs = 0;
        const bool inGame = playerIdentity.pawnValid && playerIdentity.weaponValid;
        if (inGame && !wasInGame) {
            autoRecenterArmed = true;
            autoRecenterAtMs = now + 3000;
        }
        wasInGame = inGame;
        if (autoRecenterArmed && now >= autoRecenterAtMs) {
            autoRecenterArmed = false;
            camera::RequestRecenter();
            Log("[Input] Auto-recenter after gameplay entry");
        }
    }
    const bool brickRigActive =
        player::ArmIKSystem::Instance().IsBrickRigActive();
    if (brickRigActive && playerIdentity.weaponValid && m_leftGripDown &&
        !m_skillGripWasDown) {
        m_berserkActivationUntilMs = now + 5000;
    }
    m_skillGripWasDown = m_leftGripDown;
    const bool previousBerserkPunchMode =
        m_berserkPunchMode.load(std::memory_order_acquire);
    const bool berserkPunchMode = brickRigActive && !playerIdentity.weaponValid &&
        (previousBerserkPunchMode ||
         (m_berserkActivationUntilMs && now <= m_berserkActivationUntilMs));
    if (!brickRigActive || (previousBerserkPunchMode && playerIdentity.weaponValid))
        m_berserkActivationUntilMs = 0;
    if (berserkPunchMode != previousBerserkPunchMode) {
        m_berserkPunchMode.store(berserkPunchMode, std::memory_order_release);
        ResetPhysicalMelee();
        if (berserkPunchMode &&
            !player::ArmIKSystem::Instance().IsEnabled())
            player::ArmIKSystem::Instance().SetEnabled(true);
        Log("[Input] Brick physical-punch mode %s",
            berserkPunchMode ? "enabled" : "disabled");
    }
    const bool leftPunchPulse = berserkPunchMode &&
        UpdatePhysicalMelee(left, 0, now, true);
    const bool rightPunchPulse = berserkPunchMode &&
        UpdatePhysicalMelee(right, 1, now, true);
    if (leftPunchPulse || rightPunchPulse)
        m_physicalMeleeAnimationSuppressUntilMs.store(
            now + 700, std::memory_order_release);
    bool physicalMeleePulse = false;
    if (!berserkPunchMode) {
        const bool leftArmed = m_leftTriggerDown && !weaponGrabHeld &&
            !magZoneCaptured;
        const bool leftMeleePulse = leftArmed &&
            UpdatePhysicalMelee(left, 0, now, true);
        if (!leftArmed) ResetPhysicalMelee(0);
        const bool normalRightMeleeEnabled =
            m_weaponPoseActive.load(std::memory_order_acquire) &&
            !camera::IsTransientFirstPersonActionActive();
        const bool rightWeaponPulse = normalRightMeleeEnabled &&
            UpdatePhysicalMelee(right, 1, now, false);
        if (!normalRightMeleeEnabled) ResetPhysicalMelee(1);
        physicalMeleePulse = leftMeleePulse || rightWeaponPulse;
        if (physicalMeleePulse)
            m_physicalMeleeAnimationSuppressUntilMs.store(
                now + 700, std::memory_order_release);
    }
    const bool yDown = left.buttonY;
    if (yDown && !m_yWasDown) {
        m_yPressMs = now;
        m_yChordUsed = false;
        m_yTapPulseUntilMs = 0;
    }

    WORD chordDirection = 0;
    const float absX = std::fabs(left.thumbstickX);
    const float absY = std::fabs(left.thumbstickY);
    const uint64_t yHeldMs = yDown && now >= m_yPressMs ? now - m_yPressMs : 0;
    if (yDown && yHeldMs >= 400 && (std::max)(absX, absY) >= 0.55f) {
        if (absY >= absX) {
            chordDirection = left.thumbstickY >= 0.0f
                ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
        } else {
            chordDirection = left.thumbstickX >= 0.0f
                ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
        }
        m_yChordUsed = true;
    }

    const bool echoHeld = yDown && !m_yChordUsed && yHeldMs >= 400;
    if (!yDown && m_yWasDown && !m_yChordUsed &&
        now >= m_yPressMs && now - m_yPressMs < 400) {
        m_yTapPulseUntilMs = now + 100;
    }
    m_yWasDown = yDown;
    const bool yTapPulse = !yDown && now < m_yTapPulseUntilMs;

    const bool bothSticksClicked = left.thumbstickClick && right.thumbstickClick;
    if (right.thumbstickClick) {
        m_rightStickTurnSuppressed = true;
    } else if (m_rightStickTurnSuppressed &&
               (std::max)(std::fabs(right.thumbstickX),
                          std::fabs(right.thumbstickY)) < cfg.locomotion_deadzone) {
        m_rightStickTurnSuppressed = false;
    }
    if (right.thumbstickClick && !m_rightStickWasDown && !left.thumbstickClick) {
        m_manualCrouchDesired = !m_manualCrouchDesired;
        Log("[Input] Manual VR crouch %s via R3",
            m_manualCrouchDesired ? "enabled" : "disabled");
    }
    m_rightStickWasDown = right.thumbstickClick;

    if (bothSticksClicked && !m_recenterChordStartedMs)
        m_recenterChordStartedMs = now;
    if (bothSticksClicked && !m_recenterChordLatched &&
        now - m_recenterChordStartedMs >= 1000) {
        m_recenterChordLatched = true;
        camera::RequestRecenter();
        player::ArmIKSystem::Instance().RequestCalibrationReset();
        RequestMotionCalibrationReset();
        Log("[Input] Camera, hands, and weapon recentered after holding L3+R3");
    }
    const bool suppressStickClicks = bothSticksClicked;
    if (!bothSticksClicked) {
        m_recenterChordLatched = false;
        m_recenterChordStartedMs = 0;
    }

    bool physicalCrouchPulse = false;
    if (vehicleMode) {
        m_manualCrouchDesired = false;
        m_physicalCrouchGameState = false;
        m_physicalCrouchPulseUntilMs = 0;
        m_vrCrouchActive.store(false, std::memory_order_release);
        m_nativeCrouchCameraActive.store(false, std::memory_order_release);
        m_vrCrouchTransitionUntilMs.store(0, std::memory_order_release);
    } else {
        const bool trackedCrouchDesired = cfg.physical_crouch_enabled &&
            cfg.room_scale_enabled && m_motionControlsEnabled.load(std::memory_order_acquire) &&
            player::ArmIKSystem::Instance().GetPhysicalPoseIntent() !=
                player::PhysicalPose::Standing;
        const bool crouchDesired = trackedCrouchDesired || m_manualCrouchDesired;
        if (crouchDesired != m_physicalCrouchGameState) {
            m_physicalCrouchGameState = crouchDesired;
            m_physicalCrouchPulseUntilMs = now + 100;
            Log("[Input] Combined crouch %s (tracked=%d manual=%d)",
                crouchDesired ? "entered" : "exited",
                trackedCrouchDesired, m_manualCrouchDesired);
        }
        const bool previousVrCrouch = m_vrCrouchActive.exchange(
            crouchDesired, std::memory_order_acq_rel);
        if (previousVrCrouch != crouchDesired)
            m_vrCrouchTransitionUntilMs.store(now + 1500, std::memory_order_release);
        m_nativeCrouchCameraActive.store(
            m_manualCrouchDesired && !trackedCrouchDesired,
            std::memory_order_release);
        physicalCrouchPulse = now < m_physicalCrouchPulseUntilMs;
    }
    const bool crouchPulse = physicalCrouchPulse;

    // Escape is contextual in UE3: it opens pause from gameplay and backs out
    // of the current interface. Quest B is available under SteamVR, unlike the
    // left menu button reserved by the runtime.
    if (right.buttonB && !m_prevMenu) PressKey(VK_ESCAPE);
    else if (!right.buttonB && m_prevMenu) ReleaseKey(VK_ESCAPE);
    m_prevMenu = right.buttonB ? 1 : 0;

    const bool menuNavigationActive =
        xr::FrameLoop::Instance().IsTheaterFallbackActive() ||
        WeaponAimSystem::Instance().IsVehicleTerminalUiActive();
    WORD menuDirection = 0;
    if (menuNavigationActive) {
        if (left.thumbstickY >= 0.55f) menuDirection = XINPUT_GAMEPAD_DPAD_UP;
        else if (left.thumbstickY <= -0.55f) menuDirection = XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const bool manualShotgunReload = cfg.manual_reload_shotgun &&
        WeaponAimSystem::Instance().IsManualReloadWeapon();
    bool forwardReloadX = left.buttonX;
    if (manualShotgunReload) {
        forwardReloadX = UpdateShotgunReloadSequence(controllers, left.buttonX,
            WeaponAimSystem::Instance().HasUsableTarget(), m_leftTriggerDown, m_leftGripDown, now);
        // Route the pickup/use forward as the keyboard USE key: the gamepad X
        // bit is contextual (use OR reload) and leaks vanilla reloads.
        if (forwardReloadX && !m_prevUse) PressKey('E');
        else if (!forwardReloadX && m_prevUse) ReleaseKey('E');
        m_prevUse = forwardReloadX ? 1 : 0;
        forwardReloadX = false;
    } else {
        ResetManualReload();
    }
    WeaponAimSystem::Instance().DumpWeaponPropertiesOnce();
    WeaponAimSystem::Instance().DumpWeaponBonesOnce();
    WeaponAimSystem::Instance().InstallReloadBlocker();
    WeaponAimSystem::Instance().TickWeaponPropertyWatch();
    // Reload key: the sequencer pulses R when the fresh mag is seated;
    // otherwise the keyboard fallback maps X -> R as before.
    const bool reloadKeyDown = manualShotgunReload
        ? now < m_shotgunReload.keyboardReloadPulseUntilMs
        : (!m_xinputActive && left.buttonX);
    if (reloadKeyDown && !m_prevReload) PressKey('R');
    else if (!reloadKeyDown && m_prevReload) ReleaseKey('R');
    m_prevReload = reloadKeyDown ? 1 : 0;

    WORD buttons = chordDirection | menuDirection;
    if (right.buttonA) buttons |= XINPUT_GAMEPAD_A;
    // X stays usable for contextual use (loot pickups) at all times; the
    // sequence only swallows presses that start/abort a manual reload.
    if (forwardReloadX) buttons |= XINPUT_GAMEPAD_X;
    if (yTapPulse) buttons |= XINPUT_GAMEPAD_Y;
    if (m_leftGripDown && !m_cylinderSpinning.load(std::memory_order_acquire)) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (m_rightGripDown && !vehicleMode) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (left.thumbstickClick && !suppressStickClicks) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (echoHeld) buttons |= XINPUT_GAMEPAD_BACK;

    const bool suppressMove = chordDirection != 0 || menuNavigationActive;
    float moveX = left.thumbstickX;
    float moveY = left.thumbstickY;
    if (!menuNavigationActive && cfg.hmd_directed_locomotion) {
        float headYaw = 0.0f;
        if (camera::GetRelativeHeadYaw(headYaw)) {
            const float sine = sinf(headYaw);
            const float cosine = cosf(headYaw);
            const float bodyRight = moveY * sine + moveX * cosine;
            const float bodyForward = moveY * cosine - moveX * sine;
            moveX = bodyRight;
            moveY = bodyForward;
        }
    }
    if (m_xinputActive) {
        VrGamepadState state = {};
        state.moveX = suppressMove ? 0.0f : moveX;
        state.moveY = suppressMove ? 0.0f : moveY;
        state.turnX = m_rightStickTurnSuppressed ? 0.0f : right.thumbstickX;
        state.turnY = 0.0f;
        state.leftTrigger = 0.0f;
        state.rightTrigger = berserkPunchMode ? 0.0f :
            (((m_rightTriggerDown || GetTickCount64() < m_fanFireUntilMs) && !magOutBlocksFire) ? (std::max)(right.trigger, 0.55f) : 0.0f);
        state.buttons = buttons;
        state.active = true;
        XInputBridge::Instance().Publish(state);
        auto setMouse = [&](DWORD downFlag, DWORD upFlag, bool down, int& previous) {
            if (down && !previous) mouse_event(downFlag, 0, 0, 0, 0);
            else if (!down && previous) mouse_event(upFlag, 0, 0, 0, 0);
            previous = down ? 1 : 0;
        };
        setMouse(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
            berserkPunchMode && rightPunchPulse, m_prevTrigger);
        setMouse(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP,
            berserkPunchMode && leftPunchPulse, m_prevButtonA);
        if (physicalMeleePulse && !m_prevMelee) PressKey('V');
        else if (!physicalMeleePulse && m_prevMelee) ReleaseKey('V');
        m_prevMelee = physicalMeleePulse ? 1 : 0;
        if (crouchPulse && !m_prevCrouch) PressKey('C');
        else if (!crouchPulse && m_prevCrouch) ReleaseKey('C');
        m_prevCrouch = crouchPulse ? 1 : 0;
        const bool sprintHeld = left.thumbstickClick && !suppressStickClicks;
        if (sprintHeld && !m_prevSprint) PressKey(VK_LSHIFT);
        else if (!sprintHeld && m_prevSprint) ReleaseKey(VK_LSHIFT);
        m_prevSprint = sprintHeld ? 1 : 0;
        if (vehicleSecondaryFire && !m_prevVehicleAltFire)
            mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
        else if (!vehicleSecondaryFire && m_prevVehicleAltFire)
            mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
        m_prevVehicleAltFire = vehicleSecondaryFire ? 1 : 0;
    } else {
        auto setKey = [&](int key, bool down, int& previous) {
            if (down && !previous) PressKey(key);
            else if (!down && previous) ReleaseKey(key);
            previous = down ? 1 : 0;
        };
        auto setMouse = [&](DWORD downFlag, DWORD upFlag, bool down, int& previous) {
            if (down && !previous) mouse_event(downFlag, 0, 0, 0, 0);
            else if (!down && previous) mouse_event(upFlag, 0, 0, 0, 0);
            previous = down ? 1 : 0;
        };

        setMouse(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP,
            berserkPunchMode ? rightPunchPulse : ((m_rightTriggerDown || GetTickCount64() < m_fanFireUntilMs) && !magOutBlocksFire), m_prevTrigger);
        setMouse(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP,
            berserkPunchMode ? leftPunchPulse : (adsActive || vehicleSecondaryFire),
            m_prevButtonA);
        setKey(VK_SPACE, right.buttonA, m_prevJump);
        setKey('C', crouchPulse, m_prevCrouch);
        setKey('E', left.buttonX, m_prevUse);
        setKey('F', m_leftGripDown && !m_cylinderSpinning.load(std::memory_order_acquire), m_prevGrip);
        setKey('G', m_rightGripDown && !vehicleMode, m_prevGrenade);
        setKey(VK_LSHIFT, left.thumbstickClick && !suppressStickClicks, m_prevSprint);
        setKey('V', physicalMeleePulse, m_prevMelee);
        setKey(VK_TAB, echoHeld, m_prevEcho);
        setKey('1', (chordDirection & XINPUT_GAMEPAD_DPAD_UP) != 0, m_prevDpadUp);
        setKey('2', (chordDirection & XINPUT_GAMEPAD_DPAD_RIGHT) != 0, m_prevDpadRight);
        setKey('3', (chordDirection & XINPUT_GAMEPAD_DPAD_DOWN) != 0, m_prevDpadDown);
        setKey('4', (chordDirection & XINPUT_GAMEPAD_DPAD_LEFT) != 0, m_prevDpadLeft);
        setKey(VK_UP, (menuDirection & XINPUT_GAMEPAD_DPAD_UP) != 0, m_prevMenuUp);
        setKey(VK_DOWN, (menuDirection & XINPUT_GAMEPAD_DPAD_DOWN) != 0, m_prevMenuDown);

        if (yTapPulse && !m_prevWeaponCycle)
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, WHEEL_DELTA, 0);
        m_prevWeaponCycle = yTapPulse ? 1 : 0;

        const float deadzone = cfg.locomotion_deadzone;
        setKey('W', !suppressMove && moveY > deadzone, m_prevW);
        setKey('S', !suppressMove && moveY < -deadzone, m_prevS);
        setKey('A', !suppressMove && moveX < -deadzone, m_prevA);
        setKey('D', !suppressMove && moveX > deadzone, m_prevD);
        ProcessTurn();
    }

}

void InputHook::PollAimTuningKeys() {
    const uint64_t now = GetTickCount64();
    if (m_handTuningDirty && m_handTuningLastChangedMs != 0 &&
        now - m_handTuningLastChangedMs >= 350) {
        if (config::SaveLoaded()) {
            m_handTuningDirty = false;
            m_handTuningLastChangedMs = 0;
            Log("[Input] Left-hand calibration saved after tuning pause");
        } else {
            m_handTuningLastChangedMs = now;
        }
    }
    const bool ctrlDown =
        (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    if (!ctrlDown) {
        if (m_handTuningDirty) {
            if (config::SaveLoaded()) {
                m_handTuningDirty = false;
                m_handTuningLastChangedMs = 0;
                Log("[Input] Left-hand calibration saved on key release");
            }
        }
        if (m_aimTuningDirty) {
            SaveActiveAimProfile();
            m_aimTuningDirty = false;
        }
        m_aimTuningKeysDown = 0;
        memset(m_aimTuningNextRepeatMs, 0, sizeof(m_aimTuningNextRepeatMs));
        return;
    }

    auto pressed = [&](int numpadKey, uint32_t bit, int index,
                       bool allowRepeat = true) {
        const bool down = (GetAsyncKeyState(numpadKey) & 0x8000) != 0;
        const bool wasDown = (m_aimTuningKeysDown & bit) != 0;
        if (!down) {
            m_aimTuningKeysDown &= ~bit;
            m_aimTuningNextRepeatMs[index] = 0;
            return false;
        }
        m_aimTuningKeysDown |= bit;
        if (!wasDown) {
            m_aimTuningNextRepeatMs[index] = now + 300;
            return true;
        }
        if (allowRepeat && now >= m_aimTuningNextRepeatMs[index]) {
            m_aimTuningNextRepeatMs[index] = now + 50;
            return true;
        }
        return false;
    };

    const bool selectLeft = pressed(VK_NUMPAD1, 1u << 5, 5, false);
    const bool selectRight = pressed(VK_NUMPAD3, 1u << 6, 6, false);
    const bool togglePositionMode = pressed(VK_NUMPAD0, 1u << 9, 9, false);
    if (selectLeft) m_handTuningHand = 0;
    if (selectRight) m_handTuningHand = 1;
    if (togglePositionMode && m_handTuningHand == 1) {
        m_weaponTuningPositionMode = !m_weaponTuningPositionMode;
        Log("[Input] Active weapon calibration mode=%s",
            m_weaponTuningPositionMode ? "position" : "rotation");
    }

    const bool increasePitch = pressed(VK_NUMPAD8, 1u << 0, 0);
    const bool decreasePitch = pressed(VK_NUMPAD2, 1u << 1, 1);
    const bool decreaseYaw = pressed(VK_NUMPAD4, 1u << 2, 2);
    const bool increaseYaw = pressed(VK_NUMPAD6, 1u << 3, 3);
    const bool reset = pressed(VK_NUMPAD5, 1u << 4, 4, false);
    const bool decreaseRoll = pressed(VK_NUMPAD7, 1u << 7, 7);
    const bool increaseRoll = pressed(VK_NUMPAD9, 1u << 8, 8);

    constexpr float kStepDegrees = 0.25f;
    auto& settings = config::Get();
    const bool weaponProfile = m_handTuningHand == 1;
    const bool positionMode = weaponProfile && (m_weaponTuningPositionMode ||
        ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
         (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0));
    if (weaponProfile && (m_activeWeaponAimProfile < 0 ||
        m_activeWeaponAimProfile >= kWeaponAimProfileCapacity)) return;
    float& pitch = weaponProfile ? m_aimTrimPitch
                                 : settings.left_hand_rotation_pitch;
    float& yaw = weaponProfile ? m_aimTrimYaw
                               : settings.left_hand_rotation_yaw;
    float& roll = weaponProfile ? m_aimTrimRoll
                                : settings.left_hand_rotation_roll;
    bool changed = false;
    auto adjust = [&](bool keyPressed, float& value, float amount) {
        if (!keyPressed) return;
        value += amount;
        changed = true;
    };
    if (positionMode) {
        constexpr float kStepUe = 0.25f;
        adjust(increasePitch, m_weaponOffsetUp, kStepUe);
        adjust(decreasePitch, m_weaponOffsetUp, -kStepUe);
        adjust(decreaseYaw, m_weaponOffsetRight, -kStepUe);
        adjust(increaseYaw, m_weaponOffsetRight, kStepUe);
        adjust(decreaseRoll, m_weaponOffsetForward, -kStepUe);
        adjust(increaseRoll, m_weaponOffsetForward, kStepUe);
    } else {
        adjust(increasePitch, pitch, kStepDegrees);
        adjust(decreasePitch, pitch, -kStepDegrees);
        adjust(decreaseYaw, yaw, -kStepDegrees);
        adjust(increaseYaw, yaw, kStepDegrees);
        adjust(decreaseRoll, roll, -kStepDegrees);
        adjust(increaseRoll, roll, kStepDegrees);
    }
    if (reset && positionMode) {
        m_weaponOffsetForward = 0.0f;
        m_weaponOffsetRight = 0.0f;
        m_weaponOffsetUp = 0.0f;
        changed = true;
    } else if (reset) {
        pitch = 0.0f;
        yaw = 0.0f;
        roll = 0.0f;
        changed = true;
    }
    if (!changed) return;

    pitch = (std::max)(-180.0f, (std::min)(pitch, 180.0f));
    yaw = (std::max)(-180.0f, (std::min)(yaw, 180.0f));
    roll = (std::max)(-180.0f, (std::min)(roll, 180.0f));
    m_weaponOffsetForward = (std::max)(-100.0f,
        (std::min)(m_weaponOffsetForward, 100.0f));
    m_weaponOffsetRight = (std::max)(-100.0f,
        (std::min)(m_weaponOffsetRight, 100.0f));
    m_weaponOffsetUp = (std::max)(-100.0f,
        (std::min)(m_weaponOffsetUp, 100.0f));
    if (weaponProfile) {
        m_activeWeaponTrimPitch.store(pitch, std::memory_order_release);
        m_activeWeaponTrimYaw.store(yaw, std::memory_order_release);
        m_activeWeaponTrimRoll.store(roll, std::memory_order_release);
        m_activeWeaponOffsetForward.store(
            m_weaponOffsetForward, std::memory_order_release);
        m_activeWeaponOffsetRight.store(
            m_weaponOffsetRight, std::memory_order_release);
        m_activeWeaponOffsetUp.store(m_weaponOffsetUp, std::memory_order_release);
        m_aimTuningDirty = true;
    } else {
        m_handTuningDirty = true;
        m_handTuningLastChangedMs = now;
    }
    Log("[Input] Calibration live: target=%s pitch=%.2f yaw=%.2f roll=%.2f "
        "offset=(%.2f,%.2f,%.2f)",
        weaponProfile ? "active_weapon" : "left_hand", pitch, yaw, roll,
        m_weaponOffsetForward, m_weaponOffsetRight, m_weaponOffsetUp);
}

/* Weapon Motion */
void InputHook::CacheWeaponComponent(int index, uintptr_t address, int matrixOffset) {
    if (index < 0 || index >= 7) return;
    m_components[index].address = address;
    m_components[index].matrixOffset = matrixOffset;
    m_components[index].hand = (index == 0) ? 0 : 1;
    m_components[index].hasRestPosition = false;
    m_components[index].valid = true;
    if (index + 1 > m_componentCount) m_componentCount = index + 1;
}

void InputHook::SetCanonicalWeaponPose(const float controllerPosition[3],
                                       const float controllerForward[3],
                                       const float controllerUp[3],
                                       const float cameraPosition[3],
                                       const float cameraForward[3],
                                       const float cameraUp[3]) {
    if (!controllerPosition || !controllerForward || !controllerUp ||
        !cameraPosition || !cameraForward || !cameraUp) return;
    auto validVector = [](const float value[3], bool requireLength) {
        const float length = sqrtf(value[0] * value[0] + value[1] * value[1] +
            value[2] * value[2]);
        return std::isfinite(value[0]) && std::isfinite(value[1]) &&
            std::isfinite(value[2]) && (!requireLength || length > 1.0e-5f);
    };
    if (!validVector(controllerPosition, false) ||
        !validVector(controllerForward, true) || !validVector(controllerUp, true) ||
        !validVector(cameraPosition, false) || !validVector(cameraForward, true) ||
        !validVector(cameraUp, true)) {
        m_canonicalWeaponPoseValid = false;
        m_weaponPoseActive.store(false, std::memory_order_release);
        static std::atomic<uint64_t> nextInvalidPoseLogMs{0};
        const uint64_t now = GetTickCount64();
        uint64_t nextLog = nextInvalidPoseLogMs.load(std::memory_order_relaxed);
        if (now >= nextLog && nextInvalidPoseLogMs.compare_exchange_strong(
                nextLog, now + 1000, std::memory_order_relaxed)) {
            Log("[WeaponPose] Rejected canonical pose: pos=(%.3f,%.3f,%.3f) "
                "forward=(%.3f,%.3f,%.3f) up=(%.3f,%.3f,%.3f) "
                "camera=(%.3f,%.3f,%.3f)",
                controllerPosition[0], controllerPosition[1], controllerPosition[2],
                controllerForward[0], controllerForward[1], controllerForward[2],
                controllerUp[0], controllerUp[1], controllerUp[2],
                cameraPosition[0], cameraPosition[1], cameraPosition[2]);
        }
        return;
    }
    memcpy(m_canonicalWeaponPosition, controllerPosition,
           sizeof(m_canonicalWeaponPosition));
    memcpy(m_canonicalWeaponForward, controllerForward,
           sizeof(m_canonicalWeaponForward));
    memcpy(m_canonicalWeaponUp, controllerUp, sizeof(m_canonicalWeaponUp));
    memcpy(m_nativeCameraPosition, cameraPosition, sizeof(m_nativeCameraPosition));
    memcpy(m_nativeCameraForward, cameraForward, sizeof(m_nativeCameraForward));
    memcpy(m_nativeCameraUp, cameraUp, sizeof(m_nativeCameraUp));
    m_canonicalWeaponPoseValid = true;
    static std::atomic<bool> loggedFirstCanonicalPose{false};
    if (!loggedFirstCanonicalPose.exchange(true, std::memory_order_relaxed)) {
        Log("[WeaponPose] Canonical controller pose published: "
            "position=(%.1f,%.1f,%.1f)", controllerPosition[0],
            controllerPosition[1], controllerPosition[2]);
    }
}

void InputHook::ClearCanonicalWeaponPose() {
    m_canonicalWeaponPoseValid = false;
    m_weaponPoseActive.store(false, std::memory_order_release);
    CancelWeaponGrab();
}

void InputHook::RequestMotionCalibrationReset() {
    m_weaponCalibrationResetRequested.store(true, std::memory_order_release);
    Log("[WeaponPose] Preserved pre-motion weapon mount requested");
}

bool InputHook::IsAimDotVisible() const {
    if (!config::Get().dot_enabled) return false;
    const uint64_t visibleAt = m_dotVisibleAtMs.load(std::memory_order_acquire);
    if (visibleAt == 0 || GetTickCount64() < visibleAt ||
        !m_motionControlsEnabled.load(std::memory_order_acquire)) return false;
    return true;
}

bool InputHook::GetWeaponBarrelLocalDirection(float direction[3]) {
    if (!direction) return false;
    bool valid = false;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    if (m_weaponBarrelDirectionValid && m_renderWeaponComponent >= 0x10000 &&
        m_renderWeaponStampUpdatedMs != 0 &&
        GetTickCount64() - m_renderWeaponStampUpdatedMs <= 250) {
        memcpy(direction, m_weaponBarrelLocalDirection,
               sizeof(m_weaponBarrelLocalDirection));
        valid = true;
    }
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    return valid;
}

bool InputHook::GetDrivenWeaponFrame(float position[3], float forward[3],
                                     float up[3]) const {
    if (!position || !forward || !up ||
        !m_weaponPoseActive.load(std::memory_order_acquire)) return false;
    bool valid = false;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    const uint64_t now = GetTickCount64();
    // Attachment-driven weapons do not need a direct matrix stamp, but their
    // cached frame is still the authoritative grip/barrel frame.
    if (m_renderWeaponComponent >= 0x10000 &&
        m_renderWeaponStampUpdatedMs && now >= m_renderWeaponStampUpdatedMs &&
        now - m_renderWeaponStampUpdatedMs <= 250) {
        memcpy(position, m_renderWeaponGripPosition,
               sizeof(m_renderWeaponGripPosition));
        memcpy(forward, m_renderWeaponForward, sizeof(m_renderWeaponForward));
        memcpy(up, m_renderWeaponUp, sizeof(m_renderWeaponUp));
        valid = true;
    }
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    return valid;
}

void InputHook::CancelWeaponGrab() {
    m_weaponGrabArmed.store(false, std::memory_order_release);
    m_weaponGrabCancelRequested.store(true, std::memory_order_release);
    m_weaponGrabHeld.store(false, std::memory_order_release);
}

static bool BuildFrameMatrix(const float position[3], const float forwardInput[3],
                             const float upInput[3], float output[16]) {
    float forward[3] = {forwardInput[0], forwardInput[1], forwardInput[2]};
    const float forwardLength = sqrtf(
        forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    if (!std::isfinite(forwardLength) || forwardLength < 1.0e-5f) return false;
    for (float& value : forward) value /= forwardLength;
    const float upProjection = upInput[0] * forward[0] +
        upInput[1] * forward[1] + upInput[2] * forward[2];
    float up[3] = {
        upInput[0] - forward[0] * upProjection,
        upInput[1] - forward[1] * upProjection,
        upInput[2] - forward[2] * upProjection};
    const float upLength = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!std::isfinite(upLength) || upLength < 1.0e-5f) return false;
    for (float& value : up) value /= upLength;
    const float right[3] = {
        up[1] * forward[2] - up[2] * forward[1],
        up[2] * forward[0] - up[0] * forward[2],
        up[0] * forward[1] - up[1] * forward[0]};
    memset(output, 0, sizeof(float) * 16);
    for (int axis = 0; axis < 3; ++axis) {
        output[axis] = forward[axis];
        output[4 + axis] = right[axis];
        output[8 + axis] = up[axis];
        output[12 + axis] = position[axis];
    }
    output[15] = 1.0f;
    return true;
}

static void InvertRigidFrame(const float input[16], float output[16]) {
    memset(output, 0, sizeof(float) * 16);
    output[0] = input[0]; output[1] = input[4]; output[2] = input[8];
    output[4] = input[1]; output[5] = input[5]; output[6] = input[9];
    output[8] = input[2]; output[9] = input[6]; output[10] = input[10];
    output[12] = -(input[12] * output[0] + input[13] * output[4] +
        input[14] * output[8]);
    output[13] = -(input[12] * output[1] + input[13] * output[5] +
        input[14] * output[9]);
    output[14] = -(input[12] * output[2] + input[13] * output[6] +
        input[14] * output[10]);
    output[15] = 1.0f;
}

static void MultiplyMatrix(const float left[16], const float right[16],
                           float output[16]) {
    float result[16] = {};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int index = 0; index < 4; ++index)
                result[row * 4 + column] +=
                    left[row * 4 + index] * right[index * 4 + column];
        }
    }
    memcpy(output, result, sizeof(result));
}

bool InputHook::GetActiveWeaponPoseTuning(float& pitch, float& yaw, float& roll,
                                          float& forward, float& right,
                                          float& up) const {
    if (!m_activeWeaponTrimValid.load(std::memory_order_acquire)) return false;
    pitch = m_activeWeaponTrimPitch.load(std::memory_order_acquire);
    yaw = m_activeWeaponTrimYaw.load(std::memory_order_acquire);
    roll = m_activeWeaponTrimRoll.load(std::memory_order_acquire);
    forward = m_activeWeaponOffsetForward.load(std::memory_order_acquire);
    right = m_activeWeaponOffsetRight.load(std::memory_order_acquire);
    up = m_activeWeaponOffsetUp.load(std::memory_order_acquire);
    return std::isfinite(pitch) && std::isfinite(yaw) && std::isfinite(roll) &&
        std::isfinite(forward) && std::isfinite(right) && std::isfinite(up);
}

static bool InverseTransformPoint(const float matrix[16], const float point[3],
                                  float output[3]) {
    const float a00 = matrix[0], a01 = matrix[1], a02 = matrix[2];
    const float a10 = matrix[4], a11 = matrix[5], a12 = matrix[6];
    const float a20 = matrix[8], a21 = matrix[9], a22 = matrix[10];
    const float determinant =
        a00 * (a11 * a22 - a12 * a21) -
        a01 * (a10 * a22 - a12 * a20) +
        a02 * (a10 * a21 - a11 * a20);
    if (!std::isfinite(determinant) || fabsf(determinant) < 1.0e-6f) return false;
    const float inverseDeterminant = 1.0f / determinant;
    const float inverse[9] = {
        (a11 * a22 - a12 * a21) * inverseDeterminant,
        (a02 * a21 - a01 * a22) * inverseDeterminant,
        (a01 * a12 - a02 * a11) * inverseDeterminant,
        (a12 * a20 - a10 * a22) * inverseDeterminant,
        (a00 * a22 - a02 * a20) * inverseDeterminant,
        (a02 * a10 - a00 * a12) * inverseDeterminant,
        (a10 * a21 - a11 * a20) * inverseDeterminant,
        (a01 * a20 - a00 * a21) * inverseDeterminant,
        (a00 * a11 - a01 * a10) * inverseDeterminant
    };
    const float delta[3] = {
        point[0] - matrix[12], point[1] - matrix[13], point[2] - matrix[14]
    };
    output[0] = delta[0] * inverse[0] + delta[1] * inverse[3] +
        delta[2] * inverse[6];
    output[1] = delta[0] * inverse[1] + delta[1] * inverse[4] +
        delta[2] * inverse[7];
    output[2] = delta[0] * inverse[2] + delta[1] * inverse[5] +
        delta[2] * inverse[8];
    return std::isfinite(output[0]) && std::isfinite(output[1]) &&
        std::isfinite(output[2]);
}

static void TransformPoint(const float point[3], const float matrix[16],
                           float output[3]) {
    output[0] = point[0] * matrix[0] + point[1] * matrix[4] +
        point[2] * matrix[8] + matrix[12];
    output[1] = point[0] * matrix[1] + point[1] * matrix[5] +
        point[2] * matrix[9] + matrix[13];
    output[2] = point[0] * matrix[2] + point[1] * matrix[6] +
        point[2] * matrix[10] + matrix[14];
}

static uint64_t HashWeaponProfileName(const char* characterMeshName,
                                      const char* outerName,
                                      const char* meshName) {
    if (!meshName || !*meshName) return 0;
    uint64_t hash = 14695981039346656037ull;
    auto append = [&](const char* text) {
        if (!text) return;
        for (; *text; ++text) {
            hash ^= static_cast<unsigned char>(*text);
            hash *= 1099511628211ull;
        }
    };
    append(characterMeshName);
    hash ^= static_cast<unsigned char>('|');
    hash *= 1099511628211ull;
    append(outerName);
    hash ^= static_cast<unsigned char>('|');
    hash *= 1099511628211ull;
    append(meshName);
    return hash;
}

bool InputHook::SaveActiveAimProfile() {
    if (m_activeWeaponAimProfile < 0 ||
        m_activeWeaponAimProfile >= kWeaponAimProfileCapacity) return false;
    auto& profile = m_weaponAimProfiles[m_activeWeaponAimProfile];
    if (!profile.valid) return false;
    profile.pitch = m_aimTrimPitch;
    profile.yaw = m_aimTrimYaw;
    profile.roll = m_aimTrimRoll;
    profile.offsetForward = m_weaponOffsetForward;
    profile.offsetRight = m_weaponOffsetRight;
    profile.offsetUp = m_weaponOffsetUp;
    const bool persisted = profile.persistent &&
        config::SaveWeaponAimProfile(profile.stableKey, profile.pitch,
                                      profile.yaw, profile.roll,
                                      profile.offsetForward, profile.offsetRight,
                                      profile.offsetUp, profile.name);
    Log("[Input] Weapon aim trim saved: key=%016llX weapon=%p name=%s "
        "pitch=%.2f yaw=%.2f roll=%.2f offset=(%.2f,%.2f,%.2f) storage=%s",
        static_cast<unsigned long long>(profile.stableKey),
        reinterpret_cast<void*>(profile.weapon), profile.name,
        profile.pitch, profile.yaw, profile.roll, profile.offsetForward,
        profile.offsetRight, profile.offsetUp,
        persisted ? "ini" : "session");
    return persisted;
}

void InputHook::ActivateWeaponAimProfile(uintptr_t pawn, uintptr_t weapon,
                                         const char* characterMeshName,
                                         const char* outerName,
                                         const char* meshName,
                                         uintptr_t component) {
    const uint64_t stableKey = HashWeaponProfileName(
        characterMeshName, outerName, meshName);
    if (m_activeWeaponAimProfile >= 0 &&
        m_activeWeaponAimProfile < kWeaponAimProfileCapacity) {
        const auto& active = m_weaponAimProfiles[m_activeWeaponAimProfile];
        const bool sameProfile = active.valid && active.stableKey == stableKey &&
            active.component == component;
        if (sameProfile) return;
    }
    if (m_aimTuningDirty) {
        SaveActiveAimProfile();
        m_aimTuningDirty = false;
    }

    WeaponAimProfile* selected = nullptr;
    for (auto& profile : m_weaponAimProfiles) {
        const bool matches = profile.valid &&
            ((stableKey != 0 && profile.stableKey == stableKey) ||
             (stableKey == 0 && profile.pawn == pawn &&
              profile.weapon == weapon && profile.component == component));
        if (matches) {
            selected = &profile;
            break;
        }
    }
    bool loaded = false;
    bool inheritedBrickProfile = false;
    uint64_t inheritedBrickKey = 0;
    if (!selected) {
        for (auto& profile : m_weaponAimProfiles) {
            if (!profile.valid) {
                selected = &profile;
                break;
            }
        }
        if (!selected) {
            selected = &m_weaponAimProfiles[m_weaponAimProfileCursor];
            m_weaponAimProfileCursor =
                (m_weaponAimProfileCursor + 1) % kWeaponAimProfileCapacity;
        }
        *selected = {};
        selected->stableKey = stableKey;
        selected->pitch = m_defaultAimTrimPitch;
        selected->yaw = m_defaultAimTrimYaw;
        selected->persistent = stableKey != 0;
        if (selected->persistent) {
            loaded = config::LoadWeaponAimProfile(stableKey, selected->pitch,
                                                   selected->yaw, selected->roll,
                                                   selected->offsetForward,
                                                   selected->offsetRight,
                                                   selected->offsetUp);
            if (!loaded && characterMeshName &&
                strcmp(characterMeshName, "hands_brick") != 0) {
                inheritedBrickKey = HashWeaponProfileName(
                    "hands_brick", outerName, meshName);
                inheritedBrickProfile = config::LoadWeaponAimProfile(
                    inheritedBrickKey, selected->pitch, selected->yaw,
                    selected->roll, selected->offsetForward,
                    selected->offsetRight, selected->offsetUp);
                if (inheritedBrickProfile) {
                    loaded = config::SaveWeaponAimProfile(
                        stableKey, selected->pitch, selected->yaw, selected->roll,
                        selected->offsetForward, selected->offsetRight,
                        selected->offsetUp, meshName);
                }
            }
        }
    if (meshName && *meshName)
        strncpy_s(selected->name, sizeof(selected->name), meshName, _TRUNCATE);
    else strncpy_s(selected->name, sizeof(selected->name), "runtime_weapon",
                   _TRUNCATE);
        selected->valid = true;
    }
    selected->pawn = pawn;
    selected->weapon = weapon;
    selected->component = component;
    m_activeWeaponAimProfile = static_cast<int>(selected - m_weaponAimProfiles);
    m_aimTrimPitch = selected->pitch;
    m_aimTrimYaw = selected->yaw;
    m_aimTrimRoll = selected->roll;
    m_weaponOffsetForward = selected->offsetForward;
    m_weaponOffsetRight = selected->offsetRight;
    m_weaponOffsetUp = selected->offsetUp;
    m_activeWeaponTrimPitch.store(selected->pitch, std::memory_order_release);
    m_activeWeaponTrimYaw.store(selected->yaw, std::memory_order_release);
    m_activeWeaponTrimRoll.store(selected->roll, std::memory_order_release);
    m_activeWeaponOffsetForward.store(
        selected->offsetForward, std::memory_order_release);
    m_activeWeaponOffsetRight.store(
        selected->offsetRight, std::memory_order_release);
    m_activeWeaponOffsetUp.store(selected->offsetUp, std::memory_order_release);
    m_activeWeaponTrimValid.store(true, std::memory_order_release);
    config::Get().aim_pitch_degrees = selected->pitch;
    config::Get().aim_yaw_degrees = selected->yaw;
    m_aimTuningKeysDown = 0;
    memset(m_aimTuningNextRepeatMs, 0, sizeof(m_aimTuningNextRepeatMs));
    Log("[Input] Weapon aim profile active: key=%016llX weapon=%p component=%p "
        "name=%s character=%s pitch=%.2f yaw=%.2f roll=%.2f "
        "offset=(%.2f,%.2f,%.2f) storage=%s loaded=%d inheritedBrick=%d source=%016llX",
        static_cast<unsigned long long>(selected->stableKey),
        reinterpret_cast<void*>(weapon), reinterpret_cast<void*>(component),
        selected->name, characterMeshName ? characterMeshName : "",
        selected->pitch, selected->yaw, selected->roll,
        selected->offsetForward, selected->offsetRight, selected->offsetUp,
        selected->persistent ? "ini" : "session", loaded,
        inheritedBrickProfile, static_cast<unsigned long long>(inheritedBrickKey));
}

void InputHook::ApplyRightHand(int eye) {
    (void)eye;
    m_componentCount = 0;
    if (!m_motionControlsEnabled.load(std::memory_order_acquire)) {
        m_weaponPoseActive.store(false, std::memory_order_release);
        static std::atomic<bool> loggedMotionDisabled{false};
        if (!loggedMotionDisabled.exchange(true, std::memory_order_relaxed))
            Log("[WeaponPose] Blocked: motion controls disabled");
        return;
    }
    auto& armIK = player::ArmIKSystem::Instance();
    if (xr::FrameLoop::Instance().IsTheaterFallbackActive()) {
        m_weaponPoseActive.store(false, std::memory_order_release);
        return;
    }
    if (camera::IsVehicleCameraActive()) {
        AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
        if (m_renderWeaponStampActive && m_weaponNativeMatrixValid &&
            m_renderWeaponComponent >= 0x10000 &&
            m_renderWeaponMatrixOffset > 0) {
            SIZE_T bytesWritten = 0;
            WriteProcessMemory(GetCurrentProcess(),
                reinterpret_cast<void*>(m_renderWeaponComponent +
                                        m_renderWeaponMatrixOffset),
                m_weaponNativeMatrix, sizeof(m_weaponNativeMatrix), &bytesWritten);
        }
        m_renderWeaponStampActive = false;
        m_renderWeaponComponent = 0;
        m_renderWeaponMatrixOffset = 0;
        ReleaseSRWLockExclusive(&m_weaponPoseWriteLock);
        m_weaponPoseActive.store(false, std::memory_order_release);
        static std::atomic<bool> loggedVehicleBlock{false};
        if (!loggedVehicleBlock.exchange(true, std::memory_order_relaxed))
            Log("[WeaponPose] Blocked: vehicle camera active");
        return;
    }
    if (!armIK.IsEnabled()) armIK.SetEnabled(true);
    if (m_weaponCalibrationResetRequested.exchange(false, std::memory_order_acq_rel)) {
        bool nativeRestored = false;
        size_t preservedMounts = 0;
        for (const auto& entry : m_weaponMountCache) {
            if (entry.valid) ++preservedMounts;
        }
        AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
        const bool directStampActive = m_renderWeaponStampActive;
        m_renderWeaponStampActive = false;
        if (directStampActive && m_weaponNativeMatrixValid &&
            m_renderWeaponComponent >= 0x10000 &&
            m_renderWeaponMatrixOffset > 0) {
            SIZE_T bytesWritten = 0;
            nativeRestored = WriteProcessMemory(GetCurrentProcess(),
                reinterpret_cast<void*>(m_renderWeaponComponent +
                                        m_renderWeaponMatrixOffset),
                m_weaponNativeMatrix, sizeof(m_weaponNativeMatrix), &bytesWritten) &&
                bytesWritten == sizeof(m_weaponNativeMatrix);
        }
        m_renderWeaponComponent = 0;
        m_renderWeaponMatrixOffset = 0;
        ReleaseSRWLockExclusive(&m_weaponPoseWriteLock);
        m_weaponPoseActive.store(false, std::memory_order_release);
        m_weaponMountValid = false;
        m_weaponMountAbsolute = false;
        m_weaponAttachmentDriven = false;
        m_weaponNativeMatrixValid = false;
        m_mountWeapon = 0;
        m_mountComponent = 0;
        m_mountIdentityGeneration = 0;
        m_weaponBarrelDirectionValid = false;
        m_weaponGripValid = false;
        memset(m_weaponMountMatrix, 0, sizeof(m_weaponMountMatrix));
        m_weaponIdentityStableSinceMs = GetTickCount64();
        m_lastDrivenWeapon = 0;
        m_lastDrivenComponent = 0;
        Log("[WeaponPose] Controller target recalibrated; native mounts preserved: "
            "count=%zu restoredCurrent=%d", preservedMounts, nativeRestored);
        return;
    }
    if (!m_canonicalWeaponPoseValid) {
        static std::atomic<bool> loggedMissingCanonicalPose{false};
        if (!loggedMissingCanonicalPose.exchange(true, std::memory_order_relaxed)) {
            ControllerState controllers[2] = {};
            XRInput::Instance().GetControllerSnapshot(controllers);
            Log("[WeaponPose] Waiting for canonical controller pose: "
                "rightValid=%d rightAimValid=%d",
                controllers[1].valid, controllers[1].aimValid);
        }
        return;
    }

    auto inventory = player::ArmIKSystem::Instance().GetComponentInventory();
    const auto identity = WeaponAimSystem::Instance().GetPlayerIdentity();
    bool identityMatches = identity.pawnValid && identity.weaponValid &&
        inventory.pawnIdentityValid && inventory.weaponIdentityValid &&
        identity.controller == inventory.controller && identity.pawn == inventory.pawn &&
        identity.weapon == inventory.weapon;
    if (!identityMatches) {
        const bool identityGenerationChanged =
            identity.generation != m_lastWeaponRefreshGeneration;
        if (identityGenerationChanged) {
            m_lastWeaponRefreshGeneration = identity.generation;
            if (!identity.pawnValid || !identity.weaponValid)
                camera::RequestPlayerIdentityRefresh();
            player::ArmIKSystem::Instance().RequestInventoryScan();
            Log("[WeaponPose] Identity mismatch; inventory refresh requested: "
                "identity=%p/%p inventory=%p/%p generation=%llu",
                reinterpret_cast<void*>(identity.pawn),
                reinterpret_cast<void*>(identity.weapon),
                reinterpret_cast<void*>(inventory.pawn),
                reinterpret_cast<void*>(inventory.weapon),
                static_cast<unsigned long long>(identity.generation));
        }
        player::ComponentInventoryEntry observedWeapon;
        if (identity.pawnValid && identity.weaponValid &&
            player::ArmIKSystem::Instance().FindObservedWeaponComponent(
                identity.weapon, observedWeapon)) {
            player::ComponentInventoryStatus fastInventory;
            fastInventory.controller = identity.controller;
            fastInventory.pawn = identity.pawn;
            fastInventory.weapon = identity.weapon;
            fastInventory.pawnIdentityValid = true;
            fastInventory.weaponIdentityValid = true;
            bool preservedArms = false;
            if (inventory.pawn == identity.pawn) {
                for (size_t index = 0; index < inventory.count; ++index) {
                    const auto& entry = inventory.entries[index];
                    if (!entry.exactPawnOuter ||
                        entry.role == player::ComponentRole::ProtectedWeapon) continue;
                    if (fastInventory.count < fastInventory.entries.size())
                        fastInventory.entries[fastInventory.count++] = entry;
                    preservedArms = preservedArms ||
                        entry.role == player::ComponentRole::ProbableFirstPersonArms;
                }
            }
            if (preservedArms && fastInventory.count < fastInventory.entries.size()) {
                fastInventory.entries[fastInventory.count++] = observedWeapon;
                fastInventory.weaponComponentCount = 1;
                inventory = fastInventory;
                identityMatches = true;
                if (identityGenerationChanged) {
                    Log("[WeaponPose] Observed weapon component resolved immediately: "
                        "weapon=%p component=%p updates=%llu",
                        reinterpret_cast<void*>(identity.weapon),
                        reinterpret_cast<void*>(observedWeapon.component),
                        static_cast<unsigned long long>(observedWeapon.updateCount));
                }
            }
        }
        if (!identityMatches) {
            if (identityGenerationChanged)
                Log("[WeaponPose] Visual write deferred until the new component is observed");
            return;
        }
    }
    m_lastWeaponRefreshGeneration = identity.generation;
    const player::ComponentInventoryEntry* active = nullptr;
    const player::ComponentInventoryEntry* activeArms = nullptr;
    for (size_t index = 0; index < inventory.count; ++index) {
        const auto& entry = inventory.entries[index];
        if (entry.role == player::ComponentRole::ProbableFirstPersonArms &&
            entry.exactPawnOuter && entry.meshName[0] &&
            (!activeArms || entry.updateCount > activeArms->updateCount)) {
            activeArms = &entry;
        }
        if (entry.role != player::ComponentRole::ProtectedWeapon ||
            !entry.exactWeaponOuter || !entry.component ||
            entry.localToWorldOffset <= 0 ||
            strstr(entry.className, "SkeletalMeshComponent") == nullptr) continue;
        if (!active || entry.updateCount > active->updateCount) active = &entry;
    }
    if (!activeArms) {
        const uint64_t now = GetTickCount64();
        if (now >= m_nextWeaponComponentScanMs) {
            m_nextWeaponComponentScanMs = now + 1000;
            Log("[WeaponPose] Waiting for character arms before selecting a weapon preset");
        }
        return;
    }
    const char* characterMeshName = activeArms->meshName;
    if (!active) {
        const uint64_t now = GetTickCount64();
        if (now >= m_nextWeaponComponentScanMs) {
            m_nextWeaponComponentScanMs = now + 100;
            player::ArmIKSystem::Instance().RequestInventoryScan();
            Log("[WeaponPose] Waiting for active weapon component: weapon=%p",
                reinterpret_cast<void*>(inventory.weapon));
        }
        return;
    }
    m_nextWeaponComponentScanMs = 0;
    const uint64_t now = GetTickCount64();
    const bool identityChanged = m_pendingWeaponPawn != inventory.pawn ||
        m_pendingWeapon != inventory.weapon ||
        m_pendingWeaponComponent != active->component;
    if (identityChanged) {
        CancelWeaponGrab();
        m_pendingWeaponIdentityGeneration = identity.generation;
        m_pendingWeaponPawn = inventory.pawn;
        m_pendingWeapon = inventory.weapon;
        m_pendingWeaponComponent = active->component;
        m_weaponIdentityStableSinceMs = now;
        ActivateWeaponAimProfile(inventory.pawn, inventory.weapon,
                                 characterMeshName, active->outerName,
                                 active->meshName, active->component);
        AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
        if (m_renderWeaponStampActive && m_weaponNativeMatrixValid &&
            m_renderWeaponComponent >= 0x10000 &&
            m_renderWeaponMatrixOffset > 0) {
            SIZE_T bytesWritten = 0;
            WriteProcessMemory(GetCurrentProcess(),
                reinterpret_cast<void*>(m_renderWeaponComponent +
                                        m_renderWeaponMatrixOffset),
                m_weaponNativeMatrix, sizeof(m_weaponNativeMatrix), &bytesWritten);
        }
        m_renderWeaponStampActive = false;
        m_renderWeaponComponent = 0;
        m_renderWeaponMatrixOffset = 0;
        ReleaseSRWLockExclusive(&m_weaponPoseWriteLock);
        m_weaponMountValid = false;
        m_weaponMountAbsolute = false;
        m_weaponAttachmentDriven = false;
        m_weaponNativeMatrixValid = false;
        m_weaponGripValid = false;
        m_mountIdentityGeneration = 0;
        m_weaponPoseActive.store(false, std::memory_order_release);
        Log("[WeaponPose] Weapon identity switched: generation=%llu "
            "weapon=%p component=%p",
            static_cast<unsigned long long>(identity.generation),
            reinterpret_cast<void*>(inventory.weapon),
            reinterpret_cast<void*>(active->component));
    }
    ActivateWeaponAimProfile(inventory.pawn, inventory.weapon,
                             characterMeshName, active->outerName,
                             active->meshName, active->component);
    uint64_t stableMountKey = 0;
    if (m_activeWeaponAimProfile >= 0 &&
        m_activeWeaponAimProfile < kWeaponAimProfileCapacity &&
        m_weaponAimProfiles[m_activeWeaponAimProfile].valid) {
        stableMountKey = m_weaponAimProfiles[m_activeWeaponAimProfile].stableKey;
    }
    bool reusableMountAvailable = false;
    for (const auto& entry : m_weaponMountCache) {
        if (entry.valid && entry.pawn == inventory.pawn &&
            entry.weapon == inventory.weapon && entry.component == active->component &&
            entry.skeletalMesh == active->skeletalMesh) {
            reusableMountAvailable = true;
            break;
        }
    }
    float persistedAbsoluteMount[16] = {};
    if (!reusableMountAvailable && stableMountKey)
        reusableMountAvailable = config::LoadAbsoluteWeaponMount(
            stableMountKey, persistedAbsoluteMount);
    constexpr uint64_t kNativeMountSettleMs = 100;
    if (!reusableMountAvailable &&
        now - m_weaponIdentityStableSinceMs < kNativeMountSettleMs) return;

    WeaponComponent& weapon = m_components[0];
    weapon = {};
    weapon.address = active->component;
    weapon.matrixOffset = active->localToWorldOffset;
    weapon.hand = 0;
    const uintptr_t matrixAddress = weapon.address + weapon.matrixOffset;
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(matrixAddress),
            weapon.savedMatrix, sizeof(weapon.savedMatrix), &bytesRead) ||
        bytesRead != sizeof(weapon.savedMatrix)) return;

    const float* original = weapon.savedMatrix;
    const float scaleX = sqrtf(original[0] * original[0] + original[1] * original[1] +
        original[2] * original[2]);
    const float scaleY = sqrtf(original[4] * original[4] + original[5] * original[5] +
        original[6] * original[6]);
    const float scaleZ = sqrtf(original[8] * original[8] + original[9] * original[9] +
        original[10] * original[10]);
    if (!std::isfinite(original[15]) || fabsf(original[15] - 1.0f) > 0.05f ||
        scaleX < 1.0e-4f || scaleY < 1.0e-4f || scaleZ < 1.0e-4f) return;

    float controllerFrame[16] = {};
    if (!BuildFrameMatrix(m_canonicalWeaponPosition, m_canonicalWeaponForward,
            m_canonicalWeaponUp, controllerFrame)) return;

    if (!m_weaponMountValid || m_mountWeapon != inventory.weapon ||
        m_mountComponent != active->component) {
        const WeaponMountCacheEntry* cachedMount = nullptr;
        for (const auto& entry : m_weaponMountCache) {
            if (entry.valid && entry.pawn == inventory.pawn &&
                entry.weapon == inventory.weapon &&
                entry.component == active->component &&
                entry.skeletalMesh == active->skeletalMesh) {
                cachedMount = &entry;
                break;
            }
        }
        float persistedMatrix[16] = {};
        float persistedGrip[3] = {};
        int persistedBarrelAxis = 0;
        float persistedBarrelSign = 1.0f;
        float absoluteMatrix[16] = {};
        const bool absolutePersisted = !cachedMount &&
            config::LoadAbsoluteWeaponMount(stableMountKey, absoluteMatrix);
        const bool persistedMount = !cachedMount && !absolutePersisted &&
            config::LoadWeaponMountProfile(stableMountKey, persistedMatrix,
                persistedGrip, persistedBarrelAxis, persistedBarrelSign);
        if (cachedMount) {
            memcpy(m_weaponMountMatrix, cachedMount->matrix,
                   sizeof(m_weaponMountMatrix));
            memcpy(m_weaponNativeMatrix, cachedMount->nativeMatrix,
                   sizeof(m_weaponNativeMatrix));
            m_weaponNativeMatrixValid = true;
            m_weaponBarrelAxis = cachedMount->barrelAxis;
            m_weaponBarrelSign = cachedMount->barrelSign;
            memcpy(m_weaponGripLocal, cachedMount->gripLocal,
                   sizeof(m_weaponGripLocal));
            m_weaponGripValid = cachedMount->gripValid;
            m_weaponMountAbsolute = cachedMount->absolute;
            Log("[WeaponPose] Absolute mount restored: weapon=%p component=%p "
                "offset=(%.1f,%.1f,%.1f)",
                reinterpret_cast<void*>(inventory.weapon),
                reinterpret_cast<void*>(active->component),
                m_weaponMountMatrix[12], m_weaponMountMatrix[13],
                m_weaponMountMatrix[14]);
        } else if (absolutePersisted) {
            memcpy(m_weaponMountMatrix, absoluteMatrix,
                   sizeof(m_weaponMountMatrix));
            memcpy(m_weaponNativeMatrix, original, sizeof(m_weaponNativeMatrix));
            m_weaponNativeMatrixValid = true;
            m_weaponGripValid = false;
            m_weaponMountAbsolute = true;
            float bestAlignment = -1.0f;
            for (int axis = 0; axis < 3; ++axis) {
                const float* candidate = m_weaponMountMatrix + axis * 4;
                const float length = sqrtf(candidate[0] * candidate[0] +
                    candidate[1] * candidate[1] + candidate[2] * candidate[2]);
                if (length < 1.0e-5f) continue;
                const float alignment = candidate[0] / length;
                if (fabsf(alignment) <= bestAlignment) continue;
                bestAlignment = fabsf(alignment);
                m_weaponBarrelAxis = axis;
                m_weaponBarrelSign = alignment < 0.0f ? -1.0f : 1.0f;
            }
            Log("[WeaponPose] Immutable absolute mount loaded: key=%016llX "
                "weapon=%p offset=(%.2f,%.2f,%.2f)",
                static_cast<unsigned long long>(stableMountKey),
                reinterpret_cast<void*>(inventory.weapon),
                m_weaponMountMatrix[12], m_weaponMountMatrix[13],
                m_weaponMountMatrix[14]);
        } else if (persistedMount) {
            memcpy(m_weaponMountMatrix, persistedMatrix,
                   sizeof(m_weaponMountMatrix));
            memcpy(m_weaponNativeMatrix, original, sizeof(m_weaponNativeMatrix));
            m_weaponNativeMatrixValid = true;
            memcpy(m_weaponGripLocal, persistedGrip, sizeof(m_weaponGripLocal));
            m_weaponGripValid = true;
            m_weaponMountAbsolute = false;
            m_weaponBarrelAxis = persistedBarrelAxis;
            m_weaponBarrelSign = persistedBarrelSign;

            WeaponMountCacheEntry* destination = nullptr;
            for (auto& entry : m_weaponMountCache) {
                if (!entry.valid) {
                    destination = &entry;
                    break;
                }
            }
            if (!destination) {
                destination = &m_weaponMountCache[m_weaponMountCacheCursor];
                m_weaponMountCacheCursor =
                    (m_weaponMountCacheCursor + 1) % kWeaponMountCacheCapacity;
            }
            destination->pawn = inventory.pawn;
            destination->weapon = inventory.weapon;
            destination->component = active->component;
            destination->skeletalMesh = active->skeletalMesh;
            memcpy(destination->matrix, m_weaponMountMatrix,
                   sizeof(destination->matrix));
            memcpy(destination->nativeMatrix, m_weaponNativeMatrix,
                   sizeof(destination->nativeMatrix));
            memcpy(destination->gripLocal, m_weaponGripLocal,
                   sizeof(destination->gripLocal));
            destination->barrelAxis = m_weaponBarrelAxis;
            destination->barrelSign = m_weaponBarrelSign;
            destination->gripValid = true;
            destination->absolute = false;
            destination->valid = true;
            Log("[WeaponPose] Persistent mount loaded: key=%016llX "
                "weapon=%p gripLocal=(%.1f,%.1f,%.1f) barrel=%c%c",
                static_cast<unsigned long long>(stableMountKey),
                reinterpret_cast<void*>(inventory.weapon),
                m_weaponGripLocal[0], m_weaponGripLocal[1],
                m_weaponGripLocal[2], m_weaponBarrelSign < 0.0f ? '-' : '+',
                "XYZ"[m_weaponBarrelAxis]);
        } else {
            if (WeaponAimSystem::Instance().IsFireActive()) return;
            player::NativeRightHandSnapshot nativeHand;
            if (!player::ArmIKSystem::Instance().GetNativeRightHandSnapshot(nativeHand) ||
                nativeHand.pawn != identity.pawn ||
                nativeHand.weapon != identity.weapon ||
                nativeHand.identityGeneration != identity.generation ||
                nativeHand.updatedMs < m_weaponIdentityStableSinceMs ||
                now < nativeHand.updatedMs || now - nativeHand.updatedMs > 250) {
                static std::atomic<uint64_t> nextHandWaitLogMs{0};
                uint64_t nextLog = nextHandWaitLogMs.load(std::memory_order_relaxed);
                if (now >= nextLog && nextHandWaitLogMs.compare_exchange_strong(
                        nextLog, now + 1000, std::memory_order_relaxed)) {
                    Log("[WeaponPose] Waiting for matching native wrist: "
                        "identity=%p/%p generation=%llu snapshot=%d/%p/%p/%llu age=%lldms",
                        reinterpret_cast<void*>(identity.pawn),
                        reinterpret_cast<void*>(identity.weapon),
                        static_cast<unsigned long long>(identity.generation),
                        nativeHand.valid, reinterpret_cast<void*>(nativeHand.pawn),
                        reinterpret_cast<void*>(nativeHand.weapon),
                        static_cast<unsigned long long>(nativeHand.identityGeneration),
                        nativeHand.updatedMs && now >= nativeHand.updatedMs
                            ? static_cast<long long>(now - nativeHand.updatedMs) : -1LL);
                }
                return;
            }
            memcpy(m_weaponNativeMatrix, original, sizeof(m_weaponNativeMatrix));
            m_weaponNativeMatrixValid = true;
            if (!InverseTransformPoint(original, nativeHand.worldPosition,
                                       m_weaponGripLocal)) return;
            m_weaponGripValid = true;
            m_weaponMountAbsolute = false;

            m_weaponBarrelAxis = 0;
            m_weaponBarrelSign = 1.0f;
            float bestAlignment = -1.0f;
            for (int axis = 0; axis < 3; ++axis) {
                const float* candidate = original + axis * 4;
                const float length = sqrtf(candidate[0] * candidate[0] +
                    candidate[1] * candidate[1] + candidate[2] * candidate[2]);
                if (!std::isfinite(length) || length < 1.0e-5f) continue;
                const float alignment =
                    (candidate[0] * m_nativeCameraForward[0] +
                     candidate[1] * m_nativeCameraForward[1] +
                     candidate[2] * m_nativeCameraForward[2]) / length;
                if (fabsf(alignment) <= bestAlignment) continue;
                bestAlignment = fabsf(alignment);
                m_weaponBarrelAxis = axis;
                m_weaponBarrelSign = alignment < 0.0f ? -1.0f : 1.0f;
            }

            float cameraFrame[16] = {};
            float inverseCameraFrame[16] = {};
            if (!BuildFrameMatrix(m_nativeCameraPosition, m_nativeCameraForward,
                                  m_nativeCameraUp, cameraFrame)) return;
            InvertRigidFrame(cameraFrame, inverseCameraFrame);
            MultiplyMatrix(original, inverseCameraFrame, m_weaponMountMatrix);

            WeaponMountCacheEntry* destination = nullptr;
            for (auto& entry : m_weaponMountCache) {
                if (!entry.valid) {
                    destination = &entry;
                    break;
                }
            }
            if (!destination) {
                destination = &m_weaponMountCache[m_weaponMountCacheCursor];
                m_weaponMountCacheCursor =
                    (m_weaponMountCacheCursor + 1) % kWeaponMountCacheCapacity;
            }
            destination->pawn = inventory.pawn;
            destination->weapon = inventory.weapon;
            destination->component = active->component;
            destination->skeletalMesh = active->skeletalMesh;
            memcpy(destination->matrix, m_weaponMountMatrix,
                   sizeof(destination->matrix));
            memcpy(destination->nativeMatrix, m_weaponNativeMatrix,
                   sizeof(destination->nativeMatrix));
            memcpy(destination->gripLocal, m_weaponGripLocal,
                   sizeof(destination->gripLocal));
            destination->barrelAxis = m_weaponBarrelAxis;
            destination->barrelSign = m_weaponBarrelSign;
            destination->gripValid = m_weaponGripValid;
            destination->absolute = false;
            destination->valid = true;
            const bool mountSaved = config::SaveWeaponMountProfile(
                stableMountKey, m_weaponMountMatrix, m_weaponGripLocal,
                m_weaponBarrelAxis, m_weaponBarrelSign, active->meshName);
            Log("[WeaponPose] Wrist-anchored mount captured: weapon=%p component=%p "
                "gripLocal=(%.1f,%.1f,%.1f) barrel=%c%c alignment=%.3f "
                "persistent=%d key=%016llX",
                reinterpret_cast<void*>(inventory.weapon),
                reinterpret_cast<void*>(active->component),
                m_weaponGripLocal[0], m_weaponGripLocal[1], m_weaponGripLocal[2],
                m_weaponBarrelSign < 0.0f ? '-' : '+',
                "XYZ"[m_weaponBarrelAxis], bestAlignment, mountSaved,
                static_cast<unsigned long long>(stableMountKey));
        }
        m_mountWeapon = inventory.weapon;
        m_mountComponent = active->component;
        m_mountIdentityGeneration = identity.generation;
        m_weaponMountValid = true;
    }
    float driven[16] = {};
    if (!m_weaponMountAbsolute) {
        MultiplyMatrix(m_weaponMountMatrix, controllerFrame, driven);
        if (!m_weaponGripValid) return;
        float drivenGrip[3] = {};
        TransformPoint(m_weaponGripLocal, driven, drivenGrip);
        for (int axis = 0; axis < 3; ++axis)
            driven[12 + axis] += m_canonicalWeaponPosition[axis] - drivenGrip[axis];

        float inverseControllerFrame[16] = {};
        float absoluteMount[16] = {};
        InvertRigidFrame(controllerFrame, inverseControllerFrame);
        MultiplyMatrix(driven, inverseControllerFrame, absoluteMount);
        memcpy(m_weaponMountMatrix, absoluteMount, sizeof(m_weaponMountMatrix));
        m_weaponMountAbsolute = true;
        m_weaponGripValid = false;
        const bool absoluteSaved = config::SaveAbsoluteWeaponMount(
            stableMountKey, m_weaponMountMatrix, active->meshName);
        for (auto& entry : m_weaponMountCache) {
            if (!entry.valid || entry.pawn != inventory.pawn ||
                entry.weapon != inventory.weapon ||
                entry.component != active->component ||
                entry.skeletalMesh != active->skeletalMesh) continue;
            memcpy(entry.matrix, m_weaponMountMatrix, sizeof(entry.matrix));
            entry.gripValid = false;
            entry.absolute = true;
            break;
        }
        Log("[WeaponPose] Native mount converted once to immutable absolute pose: "
            "key=%016llX saved=%d offset=(%.2f,%.2f,%.2f)",
            static_cast<unsigned long long>(stableMountKey), absoluteSaved,
            m_weaponMountMatrix[12], m_weaponMountMatrix[13],
            m_weaponMountMatrix[14]);
    }

    MultiplyMatrix(m_weaponMountMatrix, controllerFrame, driven);
    float alignedGrip[3] = {
        m_canonicalWeaponPosition[0], m_canonicalWeaponPosition[1],
        m_canonicalWeaponPosition[2]};
    const float gripError = 0.0f;
    float barrelWorld[3] = {
        driven[m_weaponBarrelAxis * 4] * m_weaponBarrelSign,
        driven[m_weaponBarrelAxis * 4 + 1] * m_weaponBarrelSign,
        driven[m_weaponBarrelAxis * 4 + 2] * m_weaponBarrelSign
    };
    const float barrelLength = sqrtf(barrelWorld[0] * barrelWorld[0] +
        barrelWorld[1] * barrelWorld[1] + barrelWorld[2] * barrelWorld[2]);
    if (!std::isfinite(barrelLength) || barrelLength < 1.0e-5f) return;
    for (float& value : barrelWorld) value /= barrelLength;
    float weaponUp[3] = {
        m_canonicalWeaponUp[0], m_canonicalWeaponUp[1], m_canonicalWeaponUp[2]};
    const float upProjection = weaponUp[0] * barrelWorld[0] +
        weaponUp[1] * barrelWorld[1] + weaponUp[2] * barrelWorld[2];
    for (int axis = 0; axis < 3; ++axis)
        weaponUp[axis] -= barrelWorld[axis] * upProjection;
    const float weaponUpLength = sqrtf(weaponUp[0] * weaponUp[0] +
        weaponUp[1] * weaponUp[1] + weaponUp[2] * weaponUp[2]);
    if (!std::isfinite(weaponUpLength) || weaponUpLength < 1.0e-5f) return;
    for (float& value : weaponUp) value /= weaponUpLength;
    float barrelLocal[3] = {
        barrelWorld[0] * controllerFrame[0] +
            barrelWorld[1] * controllerFrame[1] +
            barrelWorld[2] * controllerFrame[2],
        barrelWorld[0] * controllerFrame[4] +
            barrelWorld[1] * controllerFrame[5] +
            barrelWorld[2] * controllerFrame[6],
        barrelWorld[0] * controllerFrame[8] +
            barrelWorld[1] * controllerFrame[9] +
            barrelWorld[2] * controllerFrame[10]
    };
    const player::ArmRigStatus rigStatus = armIK.GetStatus();
    if (rigStatus.poseHookInstalled && rigStatus.rigValid)
        m_weaponAttachmentDriven = true;
    if (!m_weaponAttachmentDriven) {
        SIZE_T bytesWritten = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(matrixAddress),
                driven, sizeof(driven), &bytesWritten) ||
            bytesWritten != sizeof(driven)) return;
    }
    AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
    m_renderWeaponComponent = active->component;
    m_renderWeaponMatrixOffset = active->localToWorldOffset;
    memcpy(m_renderWeaponMatrix, driven, sizeof(m_renderWeaponMatrix));
    memcpy(m_renderWeaponGripPosition, alignedGrip,
           sizeof(m_renderWeaponGripPosition));
    memcpy(m_renderWeaponForward, barrelWorld, sizeof(m_renderWeaponForward));
    memcpy(m_renderWeaponUp, weaponUp, sizeof(m_renderWeaponUp));
    m_renderWeaponStampActive = !m_weaponAttachmentDriven;
    m_renderWeaponStampUpdatedMs = GetTickCount64();
    memcpy(m_weaponBarrelLocalDirection, barrelLocal,
           sizeof(m_weaponBarrelLocalDirection));
    m_weaponBarrelDirectionValid = true;
    ReleaseSRWLockExclusive(&m_weaponPoseWriteLock);
    weapon.valid = true;
    m_componentCount = 1;
    m_weaponPoseActive.store(true, std::memory_order_release);
    if (!armIK.IsEnabled()) armIK.SetEnabled(true);
    uint64_t expectedDotTime = 0;
    const uint64_t dotVisibleAt = GetTickCount64();
    if (m_dotVisibleAtMs.compare_exchange_strong(expectedDotTime, dotVisibleAt,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        Log("[WeaponPose] Aim dot enabled with active weapon profile");
    }
    if (m_lastDrivenWeapon != inventory.weapon ||
        m_lastDrivenComponent != active->component) {
        Log("[WeaponPose] %s pose active: weapon=%p component=%p updates=%llu "
            "matrix=+0x%X scale=(%.3f,%.3f,%.3f) gripError=%.6f barrel=%c%c "
            "local=(%.3f,%.3f,%.3f)",
            m_weaponAttachmentDriven ? "IK-attachment" : "direct-component",
            reinterpret_cast<void*>(inventory.weapon),
            reinterpret_cast<void*>(active->component),
            static_cast<unsigned long long>(active->updateCount),
            active->localToWorldOffset, scaleX, scaleY, scaleZ, gripError,
            m_weaponBarrelSign < 0.0f ? '-' : '+', "XYZ"[m_weaponBarrelAxis],
            barrelLocal[0], barrelLocal[1], barrelLocal[2]);
        m_lastDrivenWeapon = inventory.weapon;
        m_lastDrivenComponent = active->component;
    }
    ++m_applyCalls;
}

void InputHook::ApplyLeftHand(int eye) {
    (void)eye;
}

void InputHook::PublishMagHold(int32_t mode) {
    AcquireSRWLockExclusive(&m_magHoldLock);
    m_magHold.mode = mode;
    if (mode != 2) m_magHold.throwStartMs = 0;
    // The rotation cache survives the whole sequence (grab -> drop -> pouch):
    // the first grab's seated orientation is reused for the fresh mag, so it
    // always looks upright in the hand.
    if (mode != 1 && mode != 2) {
        m_magHold.grabRotationValid = false;
        if (mode == 0) RestoreMagBoneSeated();
    }
    if ((mode == 1 && !m_magHold.grabRotationValid) || mode == 3) {
        // Cache the hand world rotation and the mag's world rotation at grab
        // time, so the carried mag can rotate with the hand afterwards.
        // Mode 3 (revolver swing-out) also seeds the seated matrix so the
        // swing works on the very first reload with a weapon.
        ControllerState controllers[2] = {};
        const int32_t* cameraRotation = camera::GetCameraRotation();
        float referencePosition[3] = {};
        float referenceRotation[4] = {};
        if (XRInput::Instance().GetControllerSnapshot(controllers) &&
            controllers[0].valid && cameraRotation &&
            camera::GetPoseReference(referencePosition, referenceRotation)) {
            const float pitch = static_cast<float>(cameraRotation[0]) *
                6.2831853071795864769f / 65536.0f;
            const float yaw = static_cast<float>(cameraRotation[1]) *
                6.2831853071795864769f / 65536.0f;
            float handRotation[9] = {};
            uintptr_t component = 0;
            int32_t matrixOffset = 0;
            AcquireSRWLockShared(&m_weaponPoseWriteLock);
            component = m_renderWeaponComponent;
            matrixOffset = m_renderWeaponMatrixOffset;
            ReleaseSRWLockShared(&m_weaponPoseWriteLock);
            struct TArrayPose { uint64_t data; int32_t count; int32_t capacity; };
            TArrayPose poses = {};
            float componentMatrix[16] = {};
            float boneMatrix[16] = {};
            SIZE_T read = 0;
            const int32_t clipBoneIndex =
                WeaponAimSystem::Instance().GetClipBoneIndex();
            if (clipBoneIndex >= 0 &&
                HandWorldRotationFromXr(controllers[0].rotation, referenceRotation,
                                        pitch, yaw, handRotation) &&
                component >= 0x10000 && matrixOffset > 0 &&
                ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(component +
                        static_cast<uintptr_t>(matrixOffset)),
                    componentMatrix, sizeof(componentMatrix), &read) &&
                read == sizeof(componentMatrix) &&
                ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(component + 0x330),
                    &poses, sizeof(poses), &read) && read == sizeof(poses) &&
                poses.data && poses.count > clipBoneIndex &&
                poses.count <= 256 &&
                ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(poses.data +
                        static_cast<uint64_t>(clipBoneIndex) * 0x40ull),
                    boneMatrix, sizeof(boneMatrix), &read) &&
                read == sizeof(boneMatrix)) {
                // Keep the pristine seated pose: the game does not re-animate
                // the clip bone between sequences, so our previous override
                // would otherwise poison the reference orientation.
                const uintptr_t weaponPtr =
                    WeaponAimSystem::Instance().GetPlayerIdentity().weapon;
                if (!m_magHold.seatedValid || m_magHold.seatedWeapon != weaponPtr) {
                    memcpy(m_magHold.seatedMatrix, boneMatrix,
                           sizeof(m_magHold.seatedMatrix));
                    m_magHold.seatedWeapon = weaponPtr;
                    m_magHold.seatedValid = true;
                }
                if (mode == 1) {
                    float world[16] = {};
                    MultiplyMatrix(m_magHold.seatedMatrix, componentMatrix, world);
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column)
                            m_magHold.grabMagRotation[row * 3 + column] =
                                world[row * 4 + column];
                    memcpy(m_magHold.grabHandRotation, handRotation,
                           sizeof(m_magHold.grabHandRotation));
                    m_magHold.grabRotationValid = true;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&m_magHoldLock);
}

void InputHook::PublishMagThrow(const float originWorld[3],
                                const float velocityWorld[3]) {
    AcquireSRWLockExclusive(&m_magHoldLock);
    m_magHold.mode = 2;
    m_magHold.throwStartMs = GetTickCount64();
    memcpy(m_magHold.throwOrigin, originWorld, sizeof(m_magHold.throwOrigin));
    memcpy(m_magHold.throwVelocity, velocityWorld, sizeof(m_magHold.throwVelocity));
    ReleaseSRWLockExclusive(&m_magHoldLock);
}

void InputHook::RestoreMagBoneSeated() {
    // Caller must hold m_magHoldLock exclusively.
    if (!m_magHold.seatedValid) return;
    uintptr_t component = 0;
    int32_t matrixOffset = 0;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    component = m_renderWeaponComponent;
    matrixOffset = m_renderWeaponMatrixOffset;
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    if (component < 0x10000 || matrixOffset <= 0) return;
    const int32_t clipBoneIndex = WeaponAimSystem::Instance().GetClipBoneIndex();
    if (clipBoneIndex < 0) return;
    struct TArrayPose { uint64_t data; int32_t count; int32_t capacity; };
    TArrayPose poses = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component + 0x330),
            &poses, sizeof(poses), &read) || read != sizeof(poses) ||
        !poses.data || poses.count <= clipBoneIndex || poses.count > 256)
        return;
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(),
            reinterpret_cast<void*>(poses.data +
                static_cast<uint64_t>(clipBoneIndex) * 0x40ull),
            m_magHold.seatedMatrix, sizeof(m_magHold.seatedMatrix), &written) ||
        written != sizeof(m_magHold.seatedMatrix)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            Log("[ManualReload] Seated bone restore failed");
    }
}

void InputHook::CockRevolverHammer() {
    const int32_t hammerIndex = WeaponAimSystem::Instance().GetHammerBoneIndex();
    if (hammerIndex < 0) return;
    uintptr_t component = 0;
    int32_t matrixOffset = 0;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    component = m_renderWeaponComponent;
    matrixOffset = m_renderWeaponMatrixOffset;
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    if (component < 0x10000 || matrixOffset <= 0) return;
    struct TArrayPose { uint64_t data; int32_t count; int32_t capacity; };
    constexpr uintptr_t kSpaceBasesOffset = 0x330;
    constexpr uint64_t kBoneMatrixStride = 0x40;
    auto readMatrix = [&](int bone, float out[16]) {
        TArrayPose poses = {};
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(component + kSpaceBasesOffset),
                &poses, sizeof(poses), &read) || read != sizeof(poses) ||
            !poses.data || poses.count <= bone || poses.count > 256)
            return false;
        return ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(poses.data +
                static_cast<uint64_t>(bone) * kBoneMatrixStride),
            out, sizeof(float) * 16, &read) && read == sizeof(float) * 16;
    };
    float hammer[16] = {}, muzzle[16] = {}, trigger[16] = {};
    float componentMatrix[16] = {};
    SIZE_T read = 0;
    if (!readMatrix(hammerIndex, hammer) || !readMatrix(1, muzzle) ||
        !readMatrix(3, trigger) ||
        !ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component +
                static_cast<uintptr_t>(matrixOffset)),
            componentMatrix, sizeof(componentMatrix), &read) ||
        read != sizeof(componentMatrix))
        return;
    // Hammer world pose + barrel direction, all from live data. The cock
    // rotates about the world lateral axis through the hammer pivot.
    float hammerWorld[16] = {};
    MultiplyMatrix(hammer, componentMatrix, hammerWorld);
    const float pivot[3] = {hammerWorld[12], hammerWorld[13], hammerWorld[14]};
    float muzzleWorld[16] = {};
    MultiplyMatrix(muzzle, componentMatrix, muzzleWorld);
    float barrel[3] = {muzzleWorld[12] - pivot[0], muzzleWorld[13] - pivot[1],
                       muzzleWorld[14] - pivot[2]};
    const float barrelLength = sqrtf(barrel[0] * barrel[0] + barrel[1] * barrel[1] +
        barrel[2] * barrel[2]);
    if (!std::isfinite(barrelLength) || barrelLength < 1.0e-3f) return;
    for (float& value : barrel) value /= barrelLength;
    float hinge[3] = {-barrel[1], barrel[0], 0.0f};
    const float hingeLength = sqrtf(hinge[0] * hinge[0] + hinge[1] * hinge[1]);
    if (!std::isfinite(hingeLength) || hingeLength < 1.0e-5f) return;
    for (float& value : hinge) value /= hingeLength;
    // Try both swing directions; keep the one whose nose rises highest.
    float bestMatrix[16] = {};
    float bestScore = -1.0e30f;
    for (int direction = 0; direction < 2; ++direction) {
        const float angle = (direction == 0 ? 40.0f : -40.0f) *
            0.01745329251994329577f;
        const float c = cosf(angle);
        const float s = sinf(angle);
        const float t = 1.0f - c;
        const float x = hinge[0];
        const float y = hinge[1];
        const float z = hinge[2];
        const float columnRotation[3][3] = {
            {t * x * x + c, t * x * y - s * z, t * x * z + s * y},
            {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
            {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
        float rotation[3][3] = {};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                rotation[row][column] = columnRotation[column][row];
        float pivotRotated[3] = {};
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row)
                pivotRotated[column] += pivot[row] * rotation[row][column];
    float pivotMatrix[16] = {};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                pivotMatrix[row * 4 + column] = rotation[row][column];
        pivotMatrix[12] = pivot[0] - pivotRotated[0];
        pivotMatrix[13] = pivot[1] - pivotRotated[1];
        pivotMatrix[14] = pivot[2] - pivotRotated[2];
        pivotMatrix[15] = 1.0f;
        float candidate[16] = {};
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                float sum = 0.0f;
                for (int index = 0; index < 4; ++index)
                    sum += hammerWorld[row * 4 + index] *
                        pivotMatrix[index * 4 + column];
                candidate[row * 4 + column] = sum;
            }
        }
        // Score: the barrel-direction nose height after the swing.
        float rotatedNose[3] = {};
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row)
                rotatedNose[column] += barrel[row] * rotation[row][column];
        const float score = pivot[2] + rotatedNose[2];
        if (score > bestScore) {
            bestScore = score;
            memcpy(bestMatrix, candidate, sizeof(bestMatrix));
        }
    }
    static std::atomic<bool> hammerCalibrated{false};
    if (!hammerCalibrated.exchange(true, std::memory_order_relaxed))
        Log("[ManualReload] Hammer cock v2: hinge=(%.2f,%.2f,%.2f) score=%.2f",
            hinge[0], hinge[1], hinge[2], bestScore);
    float inverseComponent[16] = {};
    InvertRigidFrame(componentMatrix, inverseComponent);
    float boneResult[16] = {};
    MultiplyMatrix(bestMatrix, inverseComponent, boneResult);
    AcquireSRWLockExclusive(&m_magHoldLock);
    memcpy(m_magHold.hammerCockedMatrix, boneResult,
           sizeof(m_magHold.hammerCockedMatrix));
    ReleaseSRWLockExclusive(&m_magHoldLock);
    m_hammerCocked.store(true, std::memory_order_release);
    XRInput::Instance().SendHaptic(0, 1.0f, 120.0f);
    Log("[ManualReload] Hammer cocked");
}

void InputHook::TickHammerStrikeSampler(bool fireOutput, uint64_t nowMs) {
    const bool fireEdge = fireOutput && !m_strikePrevFire;
    m_strikePrevFire = fireOutput;
    if (!WeaponAimSystem::Instance().IsEquippedWeaponRevolver() ||
        !config::Get().manual_reload_shotgun) {
        m_strikePending = false;
        return;
    }
    const int32_t hammerIndex = WeaponAimSystem::Instance().GetHammerBoneIndex();
    if (hammerIndex < 0) return;
    if (fireEdge && !m_hammerCocked.load(std::memory_order_acquire)) {
        float world[3] = {};
        if (GetWeaponBoneWorld(hammerIndex, world)) {
            memcpy(m_strikePos0, world, sizeof(m_strikePos0));
            m_strikeT0Ms = nowMs;
            m_strikePending = true;
        }
    }
    if (m_strikePending && nowMs >= m_strikeT0Ms + 120) {
        m_strikePending = false;
        if (nowMs - m_strikeT0Ms < 1000) {
            float world[3] = {};
            if (GetWeaponBoneWorld(hammerIndex, world)) {
                const float dx = world[0] - m_strikePos0[0];
                const float dy = world[1] - m_strikePos0[1];
                const float dz = world[2] - m_strikePos0[2];
                const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
                if (std::isfinite(distance) && distance > 0.05f) {
                    Log("[ManualReload] Strike sample: hammer moved %.2fcm dir=(%.2f,%.2f,%.2f)",
                        distance, dx / distance, dy / distance, dz / distance);
                } else {
                    Log("[ManualReload] Strike sample: hammer static (%.2fcm)",
                        distance);
                }
            }
        }
    }
}

bool InputHook::ApplyRevolverIdleExtras() {
    if (!WeaponAimSystem::Instance().IsEquippedWeaponRevolver() ||
        !config::Get().manual_reload_shotgun) return false;
    uintptr_t component = 0;
    int32_t matrixOffset = 0;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    component = m_renderWeaponComponent;
    matrixOffset = m_renderWeaponMatrixOffset;
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    if (component < 0x10000 || matrixOffset <= 0) return false;
    struct TArrayPose { uint64_t data; int32_t count; int32_t capacity; };
    constexpr uintptr_t kSpaceBasesOffset = 0x330;
    constexpr uint64_t kBoneMatrixStride = 0x40;
    TArrayPose poses = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component + kSpaceBasesOffset),
            &poses, sizeof(poses), &read) || read != sizeof(poses) ||
        !poses.data || poses.count < 4 || poses.count > 256 ||
        poses.capacity < poses.count)
        return false;
    bool did = false;
    const int32_t magIndex = WeaponAimSystem::Instance().GetClipBoneIndex();
    if (magIndex >= 0 && poses.count > magIndex &&
        m_cylinderSpinning.load(std::memory_order_acquire)) {
        float cylinder[16] = {};
        float muzzle[16] = {};
        auto readBone = [&](int bone, float out[16]) {
            SIZE_T bytesRead = 0;
            return ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(poses.data +
                    static_cast<uint64_t>(bone) * kBoneMatrixStride),
                out, sizeof(float) * 16, &bytesRead) &&
                bytesRead == sizeof(float) * 16;
        };
        if (readBone(magIndex, cylinder) && poses.count > 1 &&
            readBone(1, muzzle)) {
            float axis[3] = {muzzle[12] - cylinder[12], muzzle[13] - cylinder[13],
                             muzzle[14] - cylinder[14]};
            const float axisLength = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] +
                axis[2] * axis[2]);
            if (std::isfinite(axisLength) && axisLength > 1.0e-5f) {
                const uint64_t nowMs = GetTickCount64();
                const uint64_t prevMs = m_spinLastMs.exchange(
                    nowMs, std::memory_order_acq_rel);
                if (prevMs != 0 && nowMs >= prevMs) {
                    float dt = static_cast<float>(nowMs - prevMs) / 1000.0f;
                    if (dt > 0.1f) dt = 0.1f;
                    const float angle = dt * 8.37758040957278173812f;
                    const float c = cosf(angle);
                    const float s = sinf(angle);
                    const float t = 1.0f - c;
                    const float x = axis[0] / axisLength;
                    const float y = axis[1] / axisLength;
                    const float z = axis[2] / axisLength;
                    const float columnRotation[3][3] = {
                        {t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                        {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                        {t * x * z - s * y, t * y * z + s * x, t * z * z + c}};
                    float rotation[3][3] = {};
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column)
                            rotation[row][column] = columnRotation[column][row];
                    const float pivot[3] = {cylinder[12], cylinder[13],
                                            cylinder[14]};
                    float pivotRotated[3] = {};
                    for (int column = 0; column < 3; ++column)
                        for (int row = 0; row < 3; ++row)
                            pivotRotated[column] +=
                                pivot[row] * rotation[row][column];
                    float pivotMatrix[16] = {};
                    for (int row = 0; row < 3; ++row)
                        for (int column = 0; column < 3; ++column)
                            pivotMatrix[row * 4 + column] = rotation[row][column];
                    pivotMatrix[12] = pivot[0] - pivotRotated[0];
                    pivotMatrix[13] = pivot[1] - pivotRotated[1];
                    pivotMatrix[14] = pivot[2] - pivotRotated[2];
                    pivotMatrix[15] = 1.0f;
                    float result[16] = {};
                    for (int row = 0; row < 4; ++row) {
                        for (int column = 0; column < 4; ++column) {
                            float sum = 0.0f;
                            for (int index = 0; index < 4; ++index)
                                sum += cylinder[row * 4 + index] *
                                    pivotMatrix[index * 4 + column];
                            result[row * 4 + column] = sum;
                        }
                    }
                    SIZE_T bytesWritten = 0;
                    if (WriteProcessMemory(GetCurrentProcess(),
                            reinterpret_cast<void*>(poses.data +
                                static_cast<uint64_t>(magIndex) *
                                    kBoneMatrixStride),
                            result, sizeof(result), &bytesWritten) &&
                        bytesWritten == sizeof(result))
                        did = true;
                }
            }
        }
    }
    if (m_hammerCocked.load(std::memory_order_acquire)) {
        const int32_t hammerIndex = WeaponAimSystem::Instance().GetHammerBoneIndex();
        if (hammerIndex >= 0 && poses.count > hammerIndex) {
            MagHoldParams holdCopy = {};
            AcquireSRWLockShared(&m_magHoldLock);
            holdCopy = m_magHold;
            ReleaseSRWLockShared(&m_magHoldLock);
            SIZE_T bytesWritten = 0;
            if (WriteProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<void*>(poses.data +
                        static_cast<uint64_t>(hammerIndex) * kBoneMatrixStride),
                    holdCopy.hammerCockedMatrix,
                    sizeof(holdCopy.hammerCockedMatrix), &bytesWritten) &&
                bytesWritten == sizeof(holdCopy.hammerCockedMatrix))
                did = true;
        }
    }
    return did;
}

bool InputHook::ApplyMagBoneOverride() {
    // Move the weapon mesh's b_clip bone so the magazine is visible in the
    // left hand, flies away when discarded, and rides back for insertion.
    // Runs from the per-frame weapon stamp (render path, after animation).
    MagHoldParams hold = {};
    AcquireSRWLockShared(&m_magHoldLock);
    hold = m_magHold;
    ReleaseSRWLockShared(&m_magHoldLock);
    if (hold.mode == 0) {
        if (!m_motionControlsEnabled.load(std::memory_order_acquire)) return false;
        return ApplyRevolverIdleExtras();
    }
    if (!m_motionControlsEnabled.load(std::memory_order_acquire)) return false;
    uintptr_t component = 0;
    int32_t matrixOffset = 0;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    component = m_renderWeaponComponent;
    matrixOffset = m_renderWeaponMatrixOffset;
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    if (component < 0x10000 || matrixOffset <= 0) return false;
    float componentMatrix[16] = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component +
                static_cast<uintptr_t>(matrixOffset)),
            componentMatrix, sizeof(componentMatrix), &read) ||
        read != sizeof(componentMatrix))
        return false;
    float desiredWorld[3] = {};
    float handRotation[9] = {};
    bool handRotationReady = false;
    const uint64_t nowMs = GetTickCount64();
    if (hold.mode == 3) {
        // Revolver: keep the cylinder swung out to the weapon's left side,
        // derived from the pristine seated pose so it tracks the weapon.
        if (!hold.seatedValid) return false;
        float seatedWorldMatrix[16] = {};
        MultiplyMatrix(hold.seatedMatrix, componentMatrix, seatedWorldMatrix);
        const int32_t* cameraRotation = camera::GetCameraRotation();
        if (!cameraRotation) return false;
        const float yaw = static_cast<float>(cameraRotation[1]) *
            6.2831853071795864769f / 65536.0f;
        const float xrRight[3] = {1.0f, 0.0f, 0.0f};
        float rightUe[3] = {};
        XrVectorToUe(xrRight, rightUe);
        const float sine = sinf(yaw);
        const float cosine = cosf(yaw);
        const float yawedX = cosine * rightUe[0] - sine * rightUe[1];
        const float yawedY = sine * rightUe[0] + cosine * rightUe[1];
        desiredWorld[0] = seatedWorldMatrix[12] - yawedX * 4.0f;
        desiredWorld[1] = seatedWorldMatrix[13] - yawedY * 4.0f;
        desiredWorld[2] = seatedWorldMatrix[14] - rightUe[2] * 4.0f;
        static std::atomic<bool> swingLogged{false};
        if (!swingLogged.exchange(true, std::memory_order_relaxed)) {
            Log("[ManualReload] Swing map: seated=(%.1f,%.1f,%.1f) desired=(%.1f,%.1f,%.1f) compT=(%.1f,%.1f,%.1f)",
                seatedWorldMatrix[12], seatedWorldMatrix[13], seatedWorldMatrix[14],
                desiredWorld[0], desiredWorld[1], desiredWorld[2],
                componentMatrix[12], componentMatrix[13], componentMatrix[14]);
        }
    } else if (hold.mode == 2 && nowMs >= hold.throwStartMs) {
        float t = static_cast<float>(nowMs - hold.throwStartMs) / 1000.0f;
        if (t > 0.7f) t = 0.7f;   // parked at the end of the flight
        desiredWorld[0] = hold.throwOrigin[0] + hold.throwVelocity[0] * t;
        desiredWorld[1] = hold.throwOrigin[1] + hold.throwVelocity[1] * t;
        desiredWorld[2] = hold.throwOrigin[2] + hold.throwVelocity[2] * t -
            490.0f * t * t;
    } else {
        // In hand (mode 1), or the fresh mag riding the palm after the throw.
        ControllerState controllers[2] = {};
        if (!XRInput::Instance().GetControllerSnapshot(controllers) ||
            !controllers[0].valid)
            return false;
        const float* cameraLocation = camera::GetCameraLocation();
        const int32_t* cameraRotation = camera::GetCameraRotation();
        float referencePosition[3] = {};
        float referenceRotation[4] = {};
        if (!cameraLocation || !cameraRotation ||
            !camera::GetPoseReference(referencePosition, referenceRotation))
            return false;
        const float inverseReference[4] = {
            -referenceRotation[0], -referenceRotation[1],
            -referenceRotation[2], referenceRotation[3]};
        const float trackingDelta[3] = {
            controllers[0].position[0] - referencePosition[0],
            controllers[0].position[1] - referencePosition[1],
            controllers[0].position[2] - referencePosition[2]};
        float localXr[3] = {};
        RotateMeleeVector(inverseReference, trackingDelta, localXr);
        const float yaw = static_cast<float>(cameraRotation[1]) *
            6.2831853071795864769f / 65536.0f;
        const float pitch = static_cast<float>(cameraRotation[0]) *
            6.2831853071795864769f / 65536.0f;
        float ue[3] = {};
        XrVectorToUe(localXr, ue);
        const float sine = sinf(yaw);
        const float cosine = cosf(yaw);
        const float yawedX = cosine * ue[0] - sine * ue[1];
        const float yawedY = sine * ue[0] + cosine * ue[1];
        const float scale = 100.0f * config::Get().positional_scale;
        // Same left-hand visual offset the IK target applies, so the mag
        // lands in the visible palm instead of the raw controller origin.
        const auto& settings = config::Get();
        const float offsetLocal[3] = {
            settings.left_hand_offset_forward,
            settings.left_hand_offset_right,
            settings.left_hand_offset_up};
        const float pitchSine = sinf(pitch);
        const float pitchCosine = cosf(pitch);
        const float pitchedX = pitchCosine * offsetLocal[0] -
            pitchSine * offsetLocal[2];
        const float pitchedZ = pitchSine * offsetLocal[0] +
            pitchCosine * offsetLocal[2];
        const float adjustmentX = cosine * pitchedX - sine * offsetLocal[1];
        const float adjustmentY = sine * pitchedX + cosine * offsetLocal[1];
        desiredWorld[0] = cameraLocation[0] + yawedX * scale + adjustmentX;
        desiredWorld[1] = cameraLocation[1] + yawedY * scale + adjustmentY;
        desiredWorld[2] = cameraLocation[2] + ue[2] * scale + pitchedZ;
        handRotationReady = HandWorldRotationFromXr(controllers[0].rotation,
            referenceRotation, pitch, yaw, handRotation);
        // Prefer the real grip point (fingertip centroid pulled toward the
        // wrist) so the mag sits inside the closing fist, not beside it.
        float gripWorld[3] = {};
        if (player::ArmIKSystem::Instance().GetLeftGripCenterWorld(gripWorld)) {
            desiredWorld[0] = gripWorld[0];
            desiredWorld[1] = gripWorld[1];
            desiredWorld[2] = gripWorld[2];
        }
    }
    if (!std::isfinite(desiredWorld[0]) || !std::isfinite(desiredWorld[1]) ||
        !std::isfinite(desiredWorld[2]))
        return false;
    // Fine trim into the palm after the grip-pose mapping.
    desiredWorld[2] += 3.0f;
    struct TArrayPose { uint64_t data; int32_t count; int32_t capacity; };
    constexpr uintptr_t kSpaceBasesOffset = 0x330;
    const uint64_t kBoneMatrixStride = 0x40;
    const int32_t clipBoneIndex = WeaponAimSystem::Instance().GetClipBoneIndex();
    if (clipBoneIndex < 0) return false;
    TArrayPose poses = {};
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(component + kSpaceBasesOffset),
            &poses, sizeof(poses), &read) || read != sizeof(poses) ||
        !poses.data || poses.count <= clipBoneIndex || poses.count > 256 ||
        poses.capacity < poses.count)
        return false;
    const int32_t expectedBones =
        WeaponAimSystem::Instance().GetWeaponBoneCount();
    static std::atomic<uintptr_t> loggedPoseComponent{0};
    static std::atomic<int32_t> loggedPoseBone{-1};
    if (loggedPoseComponent.load(std::memory_order_acquire) != component ||
        loggedPoseBone.load(std::memory_order_acquire) != clipBoneIndex) {
        loggedPoseComponent.store(component, std::memory_order_release);
        loggedPoseBone.store(clipBoneIndex, std::memory_order_release);
        Log("[ManualReload] Pose check: component=%p bone=%d poses=%d expected=%d "
            "compT=(%.1f,%.1f,%.1f)",
            reinterpret_cast<void*>(component), clipBoneIndex, poses.count,
            expectedBones, componentMatrix[12], componentMatrix[13],
            componentMatrix[14]);
    }
    if (expectedBones > 0 && poses.count != expectedBones) return false;
    const uintptr_t boneAddress = poses.data +
        static_cast<uint64_t>(clipBoneIndex) * kBoneMatrixStride;
    if (handRotationReady && hold.grabRotationValid) {
        // Also rotate the mag with the hand: it keeps the orientation it had
        // at grab time, rotated by the hand's world delta since then.
        float grabInverse[9] = {};
        Transpose3x3(hold.grabHandRotation, grabInverse);
        float deltaRotation[9] = {};
        Multiply3x3(grabInverse, handRotation, deltaRotation);
        float worldRotation[9] = {};
        Multiply3x3(hold.grabMagRotation, deltaRotation, worldRotation);
        float worldMatrix[16] = {};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                worldMatrix[row * 4 + column] = worldRotation[row * 3 + column];
        worldMatrix[12] = desiredWorld[0];
        worldMatrix[13] = desiredWorld[1];
        worldMatrix[14] = desiredWorld[2];
        worldMatrix[15] = 1.0f;
        float inverseComponent[16] = {};
        InvertRigidFrame(componentMatrix, inverseComponent);
        float boneResult[16] = {};
        MultiplyMatrix(worldMatrix, inverseComponent, boneResult);
        SIZE_T writtenRotation = 0;
        return WriteProcessMemory(GetCurrentProcess(),
            reinterpret_cast<void*>(boneAddress), boneResult,
            sizeof(boneResult), &writtenRotation) &&
            writtenRotation == sizeof(boneResult);
    }
    float componentSpace[3] = {};
    if (!InverseTransformPoint(componentMatrix, desiredWorld, componentSpace))
        return false;
    float boneMatrix[16] = {};
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(boneAddress), boneMatrix,
            sizeof(boneMatrix), &read) || read != sizeof(boneMatrix))
        return false;
    for (float value : boneMatrix)
        if (!std::isfinite(value)) return false;
    boneMatrix[12] = componentSpace[0];
    boneMatrix[13] = componentSpace[1];
    boneMatrix[14] = componentSpace[2];
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(boneAddress), boneMatrix, sizeof(boneMatrix),
        &written) && written == sizeof(boneMatrix);
}

bool InputHook::ReapplyWeaponPose(void* component) {
    ApplyMagBoneOverride();
    if (!component || !m_motionControlsEnabled.load(std::memory_order_acquire))
        return false;
    bool written = false;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    if (m_renderWeaponStampActive &&
        GetTickCount64() - m_renderWeaponStampUpdatedMs <= 250 &&
        m_renderWeaponComponent == reinterpret_cast<uintptr_t>(component) &&
        m_renderWeaponMatrixOffset > 0) {
        SIZE_T bytesWritten = 0;
        written = WriteProcessMemory(GetCurrentProcess(),
            reinterpret_cast<void*>(m_renderWeaponComponent +
                                    m_renderWeaponMatrixOffset),
            m_renderWeaponMatrix, sizeof(m_renderWeaponMatrix), &bytesWritten) &&
            bytesWritten == sizeof(m_renderWeaponMatrix);
    }
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    if (written) {
        const uint64_t count = m_postAnimationWeaponWrites.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (count == 1) {
            Log("[WeaponPose] First post-animation stereo stamp applied: component=%p",
                component);
        }
    }
    return written;
}

bool InputHook::ReapplyWeaponPose() {
    ApplyMagBoneOverride();
    if (!m_motionControlsEnabled.load(std::memory_order_acquire)) return false;
    bool written = false;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    if (m_renderWeaponStampActive && m_renderWeaponComponent >= 0x10000 &&
        m_renderWeaponMatrixOffset > 0 &&
        GetTickCount64() - m_renderWeaponStampUpdatedMs <= 250) {
        float currentMatrix[16] = {};
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<void*>(m_renderWeaponComponent +
                                        m_renderWeaponMatrixOffset),
                currentMatrix, sizeof(currentMatrix), &bytesRead) &&
            bytesRead == sizeof(currentMatrix)) {
            const float dx = currentMatrix[12] - m_renderWeaponMatrix[12];
            const float dy = currentMatrix[13] - m_renderWeaponMatrix[13];
            const float dz = currentMatrix[14] - m_renderWeaponMatrix[14];
            const float drift = sqrtf(dx*dx + dy*dy + dz*dz);
            if (std::isfinite(drift) && drift > 0.05f) {
                static std::atomic<uint64_t> driftCount{0};
                const uint64_t count = driftCount.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                if (count <= 3 || count % 600 == 0) {
                    Log("[WeaponPose] Native pre-render drift corrected: "
                        "count=%llu distance=%.3f UU delta=(%.3f,%.3f,%.3f)",
                        static_cast<unsigned long long>(count), drift, dx, dy, dz);
                }
            }
        }
        SIZE_T bytesWritten = 0;
        written = WriteProcessMemory(GetCurrentProcess(),
            reinterpret_cast<void*>(m_renderWeaponComponent +
                                    m_renderWeaponMatrixOffset),
            m_renderWeaponMatrix, sizeof(m_renderWeaponMatrix), &bytesWritten) &&
            bytesWritten == sizeof(m_renderWeaponMatrix);
    }
    ReleaseSRWLockShared(&m_weaponPoseWriteLock);
    return written;
}

void InputHook::Restore() {
    AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
    for (int i = 0; i < m_componentCount; ++i) {
        WeaponComponent& w = m_components[i];
        // Keep the driven matrix resident. UpdateSkelPose may run after
        // ViewportDraw on another thread; restoring native state here lets
        // that thread alternate native and VR transforms while locomoting.
        w.valid = false;
    }
    m_componentCount = 0;
    ReleaseSRWLockExclusive(&m_weaponPoseWriteLock);
}

void InputHook::Shutdown() {
    if (m_handTuningDirty) {
        config::SaveLoaded();
        m_handTuningDirty = false;
    }
    if (m_aimTuningDirty) {
        SaveActiveAimProfile();
        m_aimTuningDirty = false;
    }
    ReleaseAllInput();
    if (m_xinputActive) XInputBridge::Instance().Shutdown();
    m_xinputActive = false;
    AimHook::Instance().Shutdown();
    m_installed = false;
    Log("[Input] Shutdown");
}

}} // namespace bl1gotyvr::input
