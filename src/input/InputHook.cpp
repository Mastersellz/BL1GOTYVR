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
    Log("[Input] Map: sticks analog, RT fire, LT ADS, A jump, B crouch, "
        "X use/reload, Y cycle, LB skill, RB grenade, L3 sprint, R3 melee, Menu Start");
    Log("[Input] Y chord: tap=Y, hold 400ms=Back/ECHO, hold+left stick=D-pad");
    Log("[Input] B chord: tap=B, hold 400ms=toggle motion controls");
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
#undef RELEASE
    if (count) SendInput(count, inputs, sizeof(INPUT));
    m_prevGrip = m_prevW = m_prevA = m_prevS = m_prevD = 0;
    m_prevSprint = m_prevJump = m_prevMelee = 0;
    m_prevCrouch = m_prevUse = m_prevReload = m_prevGrenade = 0;
    m_prevMenu = m_prevEcho = 0;
    m_prevDpadUp = m_prevDpadDown = m_prevDpadLeft = m_prevDpadRight = 0;
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
    m_yWasDown = m_yChordUsed = false;
    m_bWasDown = m_bHoldUsed = false;
    m_recenterChordLatched = false;
    m_yPressMs = m_yTapPulseUntilMs = 0;
    m_bPressMs = m_bTapPulseUntilMs = 0;
    m_weaponGrabArmed.store(false, std::memory_order_release);
    m_weaponGrabHeld.store(false, std::memory_order_release);
    m_weaponGrabCancelRequested.store(false, std::memory_order_release);
    m_weaponGrabLatched = false;
    m_snapTurnAccum = 0;
    ResetPhysicalMelee();
    if (m_outputLive) {
        Log("[Input] Output neutralized (focus, tracking, or action sync unavailable)");
        m_outputLive = false;
    }
}

void InputHook::ResetPhysicalMelee() {
    memset(m_meleePreviousTip, 0, sizeof(m_meleePreviousTip));
    m_meleeFilteredSpeed = 0.0f;
    m_meleeTravel = 0.0f;
    m_meleePreviousSampleMs = 0;
    m_meleeBelowThresholdSinceMs = 0;
    m_physicalMeleePulseUntilMs = 0;
    m_physicalMeleeCooldownUntilMs = 0;
    m_meleeTipValid = false;
    m_physicalMeleeReady = true;
}

bool InputHook::UpdatePhysicalMelee(const ControllerState& left, uint64_t nowMs) {
    if (!left.aimValid ||
        !m_motionControlsEnabled.load(std::memory_order_acquire)) {
        ResetPhysicalMelee();
        return false;
    }

    float headPosition[3] = {};
    float headRotation[4] = {};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (!xr::OpenXRContext::Instance().GetPoseSnapshot(
            headPosition, headRotation, views)) {
        ResetPhysicalMelee();
        return false;
    }

    constexpr float kLocalForward[3] = {0.0f, 0.0f, -1.0f};
    float controllerForward[3] = {};
    float headForward[3] = {};
    RotateMeleeVector(left.aimRotation, kLocalForward, controllerForward);
    RotateMeleeVector(headRotation, kLocalForward, headForward);
    constexpr float kWeaponLengthMeters = 0.45f;
    float tip[3] = {
        left.aimPosition[0] + controllerForward[0] * kWeaponLengthMeters,
        left.aimPosition[1] + controllerForward[1] * kWeaponLengthMeters,
        left.aimPosition[2] + controllerForward[2] * kWeaponLengthMeters};
    const float headToController[3] = {
        left.aimPosition[0] - headPosition[0],
        left.aimPosition[1] - headPosition[1],
        left.aimPosition[2] - headPosition[2]};
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
        ResetPhysicalMelee();
        return false;
    }

    auto seedTip = [&]() {
        memcpy(m_meleePreviousTip, tip, sizeof(m_meleePreviousTip));
        m_meleePreviousSampleMs = nowMs;
        m_meleeTipValid = true;
    };
    const bool inFront = frontDistance >= 0.08f && controllerDistanceSq <= 2.25f;
    if (!inFront) {
        seedTip();
        m_meleeFilteredSpeed = 0.0f;
        m_meleeTravel = 0.0f;
        m_meleeBelowThresholdSinceMs = nowMs;
        if (!m_physicalMeleeReady && nowMs >= m_physicalMeleeCooldownUntilMs)
            m_physicalMeleeReady = true;
        return nowMs < m_physicalMeleePulseUntilMs;
    }
    if (!m_meleeTipValid || !m_meleePreviousSampleMs) {
        seedTip();
        return false;
    }

    const uint64_t elapsedMs = nowMs - m_meleePreviousSampleMs;
    const float delta[3] = {
        tip[0] - m_meleePreviousTip[0], tip[1] - m_meleePreviousTip[1],
        tip[2] - m_meleePreviousTip[2]};
    const float distance = sqrtf(delta[0] * delta[0] + delta[1] * delta[1] +
                                 delta[2] * delta[2]);
    if (elapsedMs < 2 || elapsedMs > 100 || !std::isfinite(distance) ||
        distance > 0.80f) {
        seedTip();
        m_meleeFilteredSpeed = 0.0f;
        m_meleeTravel = 0.0f;
        return nowMs < m_physicalMeleePulseUntilMs;
    }
    seedTip();

    const float speed = distance * 1000.0f / static_cast<float>(elapsedMs);
    m_meleeFilteredSpeed += (speed - m_meleeFilteredSpeed) * 0.45f;
    constexpr float kRearmSpeedMps = 0.65f;
    if (m_meleeFilteredSpeed <= kRearmSpeedMps) {
        m_meleeTravel = 0.0f;
        if (!m_meleeBelowThresholdSinceMs) m_meleeBelowThresholdSinceMs = nowMs;
        if (!m_physicalMeleeReady && nowMs >= m_physicalMeleeCooldownUntilMs &&
            nowMs - m_meleeBelowThresholdSinceMs >= 120) {
            m_physicalMeleeReady = true;
        }
    } else {
        m_meleeBelowThresholdSinceMs = 0;
        if (m_physicalMeleeReady) m_meleeTravel += distance;
    }

    constexpr float kTriggerSpeedMps = 1.75f;
    constexpr float kTriggerTravelMeters = 0.10f;
    if (m_physicalMeleeReady && nowMs >= m_physicalMeleeCooldownUntilMs &&
        m_meleeFilteredSpeed >= kTriggerSpeedMps &&
        m_meleeTravel >= kTriggerTravelMeters) {
        const float measuredTravel = m_meleeTravel;
        m_physicalMeleeReady = false;
        m_meleeTravel = 0.0f;
        m_physicalMeleePulseUntilMs = nowMs + 90;
        m_physicalMeleeCooldownUntilMs = nowMs + 500;
        Log("[Input] Left-arm VR melee triggered: speed=%.2fm/s travel=%.3fm front=%.2fm",
            m_meleeFilteredSpeed, measuredTravel, frontDistance);
    }
    return nowMs < m_physicalMeleePulseUntilMs;
}

void InputHook::ProcessTurn() {
    const auto& xr = XRInput::Instance();
    ControllerState controllers[2] = {};
    xr.GetControllerSnapshot(controllers);
    const auto& right = controllers[1];
    const auto& cfg = config::Get();

    if (cfg.turn_mode == 2 || !right.valid) return;

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
    const bool leftTriggerPressed = m_leftTriggerDown && !m_leftTriggerWasDown;
    const bool weaponGrabCancelled = m_weaponGrabCancelRequested.exchange(
        false, std::memory_order_acq_rel);
    if (!m_leftTriggerDown || weaponGrabCancelled || vehicleMode ||
        !m_motionControlsEnabled.load(std::memory_order_acquire)) {
        m_weaponGrabLatched = false;
    } else if (leftTriggerPressed) {
        m_weaponGrabLatched = m_weaponGrabArmed.load(std::memory_order_acquire);
        if (m_weaponGrabLatched)
            Log("[Input] Left trigger latched as two-hand weapon grab");
    }
    const bool weaponGrabHeld = m_leftTriggerDown && m_weaponGrabLatched;
    m_weaponGrabHeld.store(weaponGrabHeld, std::memory_order_release);
    const bool adsActive = m_leftTriggerDown && !weaponGrabHeld;
    const bool vehicleSecondaryFire = vehicleMode && m_rightGripDown;
    WeaponAimSystem::Instance().SetVehicleSecondaryFireActive(vehicleSecondaryFire);
    WeaponAimSystem::Instance().SetFireActive(
        m_rightTriggerDown || vehicleSecondaryFire);

    const bool physicalMeleePulse = UpdatePhysicalMelee(left, now);
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

    const bool bDown = right.buttonB;
    if (bDown && !m_bWasDown) {
        m_bPressMs = now;
        m_bHoldUsed = false;
        m_bTapPulseUntilMs = 0;
    }
    const uint64_t bHeldMs = bDown && now >= m_bPressMs ? now - m_bPressMs : 0;
    if (bDown && !m_bHoldUsed && bHeldMs >= 400) {
        m_bHoldUsed = true;
        if (m_motionControlsEnabled.exchange(false, std::memory_order_acq_rel)) {
            m_dotVisibleAtMs.store(0, std::memory_order_release);
            player::ArmIKSystem::Instance().SetEnabled(false);
            AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
            if (m_weaponNativeMatrixValid && m_renderWeaponComponent >= 0x10000 &&
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
            CancelWeaponGrab();
            WeaponAimSystem::Instance().InvalidateDirection();
            Log("[Input] Motion controls disabled by hold-B");
        } else {
            m_weaponCalibrationResetRequested.store(true, std::memory_order_release);
            m_motionControlsEnabled.store(true, std::memory_order_release);
            m_dotVisibleAtMs.store(now, std::memory_order_release);
            Log("[Input] Motion controls enabled; waiting for coherent weapon pose");
        }
    }
    if (!bDown && m_bWasDown && !m_bHoldUsed &&
        now >= m_bPressMs && now - m_bPressMs < 400) {
        m_bTapPulseUntilMs = now + 100;
    }
    m_bWasDown = bDown;
    const bool bTapPulse = !bDown && now < m_bTapPulseUntilMs;

    const bool bothSticksClicked = left.thumbstickClick && right.thumbstickClick;
    if (bothSticksClicked && !m_recenterChordLatched) {
        m_recenterChordLatched = true;
        camera::RequestRecenter();
        player::ArmIKSystem::Instance().RequestCalibrationReset();
        RequestMotionCalibrationReset();
        Log("[Input] Camera, hands, and weapon recentered by L3+R3");
    }
    const bool suppressStickClicks = m_recenterChordLatched;
    if (m_recenterChordLatched && !left.thumbstickClick && !right.thumbstickClick)
        m_recenterChordLatched = false;

    WORD buttons = chordDirection;
    if (right.buttonA) buttons |= XINPUT_GAMEPAD_A;
    if (bTapPulse) buttons |= XINPUT_GAMEPAD_B;
    if (left.buttonX) buttons |= XINPUT_GAMEPAD_X;
    if (yTapPulse) buttons |= XINPUT_GAMEPAD_Y;
    if (m_leftGripDown) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (m_rightGripDown && !vehicleMode) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (left.thumbstickClick && !suppressStickClicks) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if ((right.thumbstickClick || physicalMeleePulse) && !suppressStickClicks)
        buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (left.menuButton) buttons |= XINPUT_GAMEPAD_START;
    if (echoHeld) buttons |= XINPUT_GAMEPAD_BACK;

    const bool suppressMove = chordDirection != 0;
    if (m_xinputActive) {
        VrGamepadState state = {};
        state.moveX = suppressMove ? 0.0f : left.thumbstickX;
        state.moveY = suppressMove ? 0.0f : left.thumbstickY;
        state.turnX = right.thumbstickX;
        state.turnY = right.thumbstickY;
        state.leftTrigger = adsActive
            ? (std::max)(left.trigger, 0.55f) : 0.0f;
        state.rightTrigger = m_rightTriggerDown
            ? (std::max)(right.trigger, 0.55f) : 0.0f;
        state.buttons = buttons;
        state.active = true;
        XInputBridge::Instance().Publish(state);
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
            m_rightTriggerDown, m_prevTrigger);
        setMouse(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP,
            adsActive || vehicleSecondaryFire, m_prevButtonA);
        setKey(VK_SPACE, right.buttonA, m_prevJump);
        setKey('C', bTapPulse, m_prevCrouch);
        setKey('E', left.buttonX, m_prevUse);
        setKey('R', left.buttonX, m_prevReload);
        setKey('F', m_leftGripDown, m_prevGrip);
        setKey('G', m_rightGripDown && !vehicleMode, m_prevGrenade);
        setKey(VK_LSHIFT, left.thumbstickClick && !suppressStickClicks, m_prevSprint);
        setKey('V', (right.thumbstickClick || physicalMeleePulse) &&
            !suppressStickClicks, m_prevMelee);
        setKey(VK_ESCAPE, left.menuButton, m_prevMenu);
        setKey(VK_TAB, echoHeld, m_prevEcho);
        setKey('1', (chordDirection & XINPUT_GAMEPAD_DPAD_UP) != 0, m_prevDpadUp);
        setKey('2', (chordDirection & XINPUT_GAMEPAD_DPAD_RIGHT) != 0, m_prevDpadRight);
        setKey('3', (chordDirection & XINPUT_GAMEPAD_DPAD_DOWN) != 0, m_prevDpadDown);
        setKey('4', (chordDirection & XINPUT_GAMEPAD_DPAD_LEFT) != 0, m_prevDpadLeft);

        if (yTapPulse && !m_prevWeaponCycle)
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, WHEEL_DELTA, 0);
        m_prevWeaponCycle = yTapPulse ? 1 : 0;

        const float deadzone = cfg.locomotion_deadzone;
        setKey('W', !suppressMove && left.thumbstickY > deadzone, m_prevW);
        setKey('S', !suppressMove && left.thumbstickY < -deadzone, m_prevS);
        setKey('A', !suppressMove && left.thumbstickX < -deadzone, m_prevA);
        setKey('D', !suppressMove && left.thumbstickX > deadzone, m_prevD);
        ProcessTurn();
    }

}

void InputHook::PollAimTuningKeys() {
    const bool ctrlDown =
        (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    if (!ctrlDown) {
        if (m_handTuningDirty) {
            config::SaveLoaded();
            m_handTuningDirty = false;
        }
        if (m_aimTuningDirty) {
            SaveActiveAimProfile();
            m_aimTuningDirty = false;
        }
        m_aimTuningKeysDown = 0;
        memset(m_aimTuningNextRepeatMs, 0, sizeof(m_aimTuningNextRepeatMs));
        return;
    }

    const uint64_t now = GetTickCount64();
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
    const uint64_t visibleAt = m_dotVisibleAtMs.load(std::memory_order_acquire);
    if (visibleAt == 0 || GetTickCount64() < visibleAt ||
        !m_motionControlsEnabled.load(std::memory_order_acquire)) return false;
    return true;
}

bool InputHook::GetWeaponBarrelLocalDirection(float direction[3]) {
    if (!direction) return false;
    bool valid = false;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    if (m_weaponBarrelDirectionValid && m_renderWeaponStampActive) {
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
    if (m_renderWeaponStampActive && m_renderWeaponComponent >= 0x10000 &&
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
        }
        if (meshName && *meshName) strcpy_s(selected->name, meshName);
        else strcpy_s(selected->name, "runtime_weapon");
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
        "offset=(%.2f,%.2f,%.2f) storage=%s loaded=%d",
        static_cast<unsigned long long>(selected->stableKey),
        reinterpret_cast<void*>(weapon), reinterpret_cast<void*>(component),
        selected->name, characterMeshName ? characterMeshName : "",
        selected->pitch, selected->yaw, selected->roll,
        selected->offsetForward, selected->offsetRight, selected->offsetUp,
        selected->persistent ? "ini" : "session", loaded);
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
    if (!m_weaponPoseActive.load(std::memory_order_acquire) && armIK.IsEnabled())
        armIK.SetEnabled(false);
    if (camera::IsVehicleCameraActive()) {
        AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
        if (m_weaponNativeMatrixValid && m_renderWeaponComponent >= 0x10000 &&
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
    if (m_weaponCalibrationResetRequested.exchange(false, std::memory_order_acq_rel)) {
        bool nativeRestored = false;
        size_t preservedMounts = 0;
        for (const auto& entry : m_weaponMountCache) {
            if (entry.valid) ++preservedMounts;
        }
        AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
        m_renderWeaponStampActive = false;
        if (m_weaponNativeMatrixValid && m_renderWeaponComponent >= 0x10000 &&
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

    const auto inventory = player::ArmIKSystem::Instance().GetComponentInventory();
    const auto identity = WeaponAimSystem::Instance().GetPlayerIdentity();
    const bool identityMatches = identity.pawnValid && identity.weaponValid &&
        inventory.pawnIdentityValid && inventory.weaponIdentityValid &&
        identity.controller == inventory.controller && identity.pawn == inventory.pawn &&
        identity.weapon == inventory.weapon;
    if (!identityMatches) {
        if (identity.generation != m_lastWeaponRefreshGeneration) {
            m_lastWeaponRefreshGeneration = identity.generation;
            camera::RequestPlayerIdentityRefresh();
            player::ArmIKSystem::Instance().RequestInventoryScan();
            Log("[WeaponPose] Identity mismatch; visual write deferred: "
                "identity=%p/%p inventory=%p/%p generation=%llu",
                reinterpret_cast<void*>(identity.pawn),
                reinterpret_cast<void*>(identity.weapon),
                reinterpret_cast<void*>(inventory.pawn),
                reinterpret_cast<void*>(inventory.weapon),
                static_cast<unsigned long long>(identity.generation));
        }
        return;
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
    const char* characterMeshName = activeArms
        ? activeArms->meshName : "unknown_character";
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
        if (m_weaponNativeMatrixValid && m_renderWeaponComponent >= 0x10000 &&
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
        m_weaponNativeMatrixValid = false;
        m_weaponGripValid = false;
        m_mountIdentityGeneration = 0;
        m_weaponPoseActive.store(false, std::memory_order_release);
        Log("[WeaponPose] Waiting briefly for native mount: generation=%llu "
            "weapon=%p component=%p",
            static_cast<unsigned long long>(identity.generation),
            reinterpret_cast<void*>(inventory.weapon),
            reinterpret_cast<void*>(active->component));
        return;
    }
    constexpr uint64_t kNativeMountSettleMs = 100;
    if (now - m_weaponIdentityStableSinceMs < kNativeMountSettleMs) return;
    ActivateWeaponAimProfile(inventory.pawn, inventory.weapon,
                             characterMeshName, active->outerName,
                             active->meshName, active->component);
    uint64_t stableMountKey = 0;
    if (m_activeWeaponAimProfile >= 0 &&
        m_activeWeaponAimProfile < kWeaponAimProfileCapacity &&
        m_weaponAimProfiles[m_activeWeaponAimProfile].valid) {
        stableMountKey = m_weaponAimProfiles[m_activeWeaponAimProfile].stableKey;
    }

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
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(matrixAddress),
            driven, sizeof(driven), &bytesWritten) || bytesWritten != sizeof(driven)) return;
    AcquireSRWLockExclusive(&m_weaponPoseWriteLock);
    m_renderWeaponComponent = active->component;
    m_renderWeaponMatrixOffset = active->localToWorldOffset;
    memcpy(m_renderWeaponMatrix, driven, sizeof(m_renderWeaponMatrix));
    memcpy(m_renderWeaponGripPosition, alignedGrip,
           sizeof(m_renderWeaponGripPosition));
    memcpy(m_renderWeaponForward, barrelWorld, sizeof(m_renderWeaponForward));
    memcpy(m_renderWeaponUp, weaponUp, sizeof(m_renderWeaponUp));
    m_renderWeaponStampActive = true;
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
        Log("[WeaponPose] Native-relative pose active: weapon=%p component=%p updates=%llu "
            "matrix=+0x%X scale=(%.3f,%.3f,%.3f) gripError=%.6f barrel=%c%c "
            "local=(%.3f,%.3f,%.3f)",
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

bool InputHook::ReapplyWeaponPose(void* component) {
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
    if (!m_motionControlsEnabled.load(std::memory_order_acquire)) return false;
    bool written = false;
    AcquireSRWLockShared(&m_weaponPoseWriteLock);
    if (m_renderWeaponStampActive && m_renderWeaponComponent >= 0x10000 &&
        m_renderWeaponMatrixOffset > 0 &&
        GetTickCount64() - m_renderWeaponStampUpdatedMs <= 250) {
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
