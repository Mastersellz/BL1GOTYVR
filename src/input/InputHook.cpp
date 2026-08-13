#include "InputHook.hpp"
#include "XRInput.hpp"
#include "XInputBridge.hpp"
#include "AimHook.hpp"
#include "WeaponAimSystem.hpp"
#include "../core/VRMod.hpp"
#include "../camera/CameraHook.hpp"
#include "../config/Config.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace bl1gotyvr { namespace input {

InputHook& InputHook::Instance() {
    static InputHook hook;
    return hook;
}

static bool ApplyHysteresis(float value, bool wasDown) {
    if (wasDown) return value > 0.45f;
    return value >= 0.55f;
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

    INPUT mouse[2] = {};
    int mcount = 0;
    if (m_prevTrigger) { mouse[mcount].type=INPUT_MOUSE; mouse[mcount++].mi.dwFlags=MOUSEEVENTF_LEFTUP; }
    if (m_prevButtonA) { mouse[mcount].type=INPUT_MOUSE; mouse[mcount++].mi.dwFlags=MOUSEEVENTF_RIGHTUP; }
    if (mcount) SendInput(mcount, mouse, sizeof(INPUT));
    m_prevTrigger = m_prevButtonA = 0;
    m_leftTriggerDown = m_rightTriggerDown = false;
    m_leftGripDown = m_rightGripDown = false;
    m_yWasDown = m_yChordUsed = false;
    m_recenterChordLatched = false;
    m_yPressMs = m_yTapPulseUntilMs = 0;
    m_snapTurnAccum = 0;
    if (m_outputLive) {
        Log("[Input] Output neutralized (focus, tracking, or action sync unavailable)");
        m_outputLive = false;
    }
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
    const bool foreground = IsGameForeground(m_gameWindow);

    if (m_logCounter++ % 300 == 0)
        Log("[Input] mode=%s leftValid=%d rightValid=%d foreground=%d "
            "RT=%.2f LT=%.2f sticks=L(%.2f,%.2f) R(%.2f,%.2f)",
            m_xinputActive ? "XInput" : "fallback", left.valid, right.valid, foreground,
            right.trigger, left.trigger,
            left.thumbstickX, left.thumbstickY,
            right.thumbstickX, right.thumbstickY);
    if (!left.valid || !right.valid) { ReleaseAllInput(); return; }
    if (!foreground) { ReleaseAllInput(); return; }
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
    m_leftTriggerDown = ApplyHysteresis(left.trigger, m_leftTriggerDown);
    m_leftGripDown = ApplyHysteresis(left.grip, m_leftGripDown);
    m_rightGripDown = ApplyHysteresis(right.grip, m_rightGripDown);
    WeaponAimSystem::Instance().SetFireActive(m_rightTriggerDown);

    const uint64_t now = GetTickCount64();
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
    if (bothSticksClicked && !m_recenterChordLatched) {
        m_recenterChordLatched = true;
        camera::RequestRecenter();
        Log("[Input] Camera recentered by L3+R3");
    }
    const bool suppressStickClicks = m_recenterChordLatched;
    if (m_recenterChordLatched && !left.thumbstickClick && !right.thumbstickClick)
        m_recenterChordLatched = false;

    WORD buttons = chordDirection;
    if (right.buttonA) buttons |= XINPUT_GAMEPAD_A;
    if (right.buttonB) buttons |= XINPUT_GAMEPAD_B;
    if (left.buttonX) buttons |= XINPUT_GAMEPAD_X;
    if (yTapPulse) buttons |= XINPUT_GAMEPAD_Y;
    if (m_leftGripDown) buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (m_rightGripDown) buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (left.thumbstickClick && !suppressStickClicks) buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (right.thumbstickClick && !suppressStickClicks) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (left.menuButton) buttons |= XINPUT_GAMEPAD_START;
    if (echoHeld) buttons |= XINPUT_GAMEPAD_BACK;

    const bool suppressMove = chordDirection != 0;
    if (m_xinputActive) {
        VrGamepadState state = {};
        state.moveX = suppressMove ? 0.0f : left.thumbstickX;
        state.moveY = suppressMove ? 0.0f : left.thumbstickY;
        state.turnX = right.thumbstickX;
        state.turnY = right.thumbstickY;
        state.leftTrigger = m_leftTriggerDown
            ? (std::max)(left.trigger, 0.55f) : 0.0f;
        state.rightTrigger = m_rightTriggerDown
            ? (std::max)(right.trigger, 0.55f) : 0.0f;
        state.buttons = buttons;
        state.active = true;
        XInputBridge::Instance().Publish(state);
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
            m_leftTriggerDown, m_prevButtonA);
        setKey(VK_SPACE, right.buttonA, m_prevJump);
        setKey('C', right.buttonB, m_prevCrouch);
        setKey('E', left.buttonX, m_prevUse);
        setKey('R', left.buttonX, m_prevReload);
        setKey('F', m_leftGripDown, m_prevGrip);
        setKey('G', m_rightGripDown, m_prevGrenade);
        setKey(VK_LSHIFT, left.thumbstickClick && !suppressStickClicks, m_prevSprint);
        setKey('V', right.thumbstickClick && !suppressStickClicks, m_prevMelee);
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

    /* Ctrl+Numpad shared dot and ballistic calibration */
    PollAimTuningKeys();
}

void InputHook::PollAimTuningKeys() {
    const bool ctrlDown =
        (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    if (!ctrlDown) {
        m_aimTuningKeysDown = 0;
        m_aimTrimPitch = config::Get().aim_pitch_degrees;
        m_aimTrimYaw = config::Get().aim_yaw_degrees;
        return;
    }

    auto pressed = [&](int numpadKey, int navigationKey, uint32_t bit) {
        const bool down =
            (GetAsyncKeyState(numpadKey) & 0x8000) != 0 ||
            (GetAsyncKeyState(navigationKey) & 0x8000) != 0;
        const bool wasDown = (m_aimTuningKeysDown & bit) != 0;
        if (down) m_aimTuningKeysDown |= bit;
        else m_aimTuningKeysDown &= ~bit;
        return down && !wasDown;
    };

    const bool increasePitch = pressed(VK_NUMPAD8, VK_UP, 1u << 0);
    const bool decreasePitch = pressed(VK_NUMPAD2, VK_DOWN, 1u << 1);
    const bool decreaseYaw = pressed(VK_NUMPAD4, VK_LEFT, 1u << 2);
    const bool increaseYaw = pressed(VK_NUMPAD6, VK_RIGHT, 1u << 3);
    const bool reset = pressed(VK_NUMPAD5, VK_CLEAR, 1u << 4);

    constexpr float kStepDegrees = 0.25f;
    bool changed = false;
    auto adjust = [&](bool keyPressed, float& value, float amount) {
        if (!keyPressed) return;
        value += amount;
        changed = true;
    };
    adjust(increasePitch, m_aimTrimPitch, kStepDegrees);
    adjust(decreasePitch, m_aimTrimPitch, -kStepDegrees);
    adjust(decreaseYaw, m_aimTrimYaw, -kStepDegrees);
    adjust(increaseYaw, m_aimTrimYaw, kStepDegrees);
    if (reset) {
        m_aimTrimPitch = 0.0f;
        m_aimTrimYaw = 0.0f;
        changed = true;
    }
    if (!changed) return;

    m_aimTrimPitch = (std::max)(-45.0f, (std::min)(m_aimTrimPitch, 45.0f));
    m_aimTrimYaw = (std::max)(-45.0f, (std::min)(m_aimTrimYaw, 45.0f));
    config::Get().aim_pitch_degrees = m_aimTrimPitch;
    config::Get().aim_yaw_degrees = m_aimTrimYaw;
    const bool persisted = config::SaveLoaded();
    Log("[Input] Shared aim trim: pitch=%.2f yaw=%.2f persisted=%d",
        m_aimTrimPitch, m_aimTrimYaw, persisted);
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

static void QuaternionToRotationMatrix(float qw, float qx, float qy, float qz, float out[9]) {
    float xx=qx*qx, yy=qy*qy, zz=qz*qz, xy=qx*qy, xz=qx*qz, yz=qy*qz;
    float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    out[0]=1-2*(yy+zz); out[1]=2*(xy-wz);  out[2]=2*(xz+wy);
    out[3]=2*(xy+wz);   out[4]=1-2*(xx+zz); out[5]=2*(yz-wx);
    out[6]=2*(xz-wy);   out[7]=2*(yz+wx);   out[8]=1-2*(xx+yy);
}

void InputHook::ApplyRightHand(int eye) {
    (void)eye;
    if (!m_componentCount) return;
    const auto& xr = XRInput::Instance();
    ControllerState controllers[2] = {};
    xr.GetControllerSnapshot(controllers);
    const auto& right = controllers[1];
    if (!right.valid) return;

    float vrRot[9];
    QuaternionToRotationMatrix(right.rotation[3], right.rotation[0], right.rotation[1], right.rotation[2], vrRot);

    float posScale = config::Get().weapon_position_scale;

    // Get aim offsets from decoupled aim system
    const auto& aim = AimHook::Instance();
    const bool useAimOffset = aim.IsEnabled();

    for (int i = 0; i < m_componentCount; ++i) {
        WeaponComponent& w = m_components[i];
        if (!w.valid || !w.address || !w.matrixOffset || w.hand != 0) continue;

        uintptr_t addr = w.address + w.matrixOffset;
        float matrix[16];
        ReadProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
        memcpy(w.savedMatrix, matrix, sizeof(float)*16);

        // Apply rotation (with aim offset if enabled)
        if (useAimOffset) {
            // Create rotation matrix from aim offsets (in radians)
            constexpr float kUnisToRadians = 6.2831853071795864769f / 65536.0f;
            const float pitchRad = aim.GetAimPitchOffset() * kUnisToRadians;
            const float yawRad = aim.GetAimYawOffset() * kUnisToRadians;
            const float rollRad = aim.GetAimRollOffset() * kUnisToRadians;

            const float cp = cosf(pitchRad), sp = sinf(pitchRad);
            const float cy = cosf(yawRad), sy = sinf(yawRad);
            const float cr = cosf(rollRad), sr = sinf(rollRad);

            // Build rotation matrix: roll * pitch * yaw
            float aimRot[9];
            aimRot[0] = cy * cr + sy * sp * sr;
            aimRot[1] = -cy * sr + sy * sp * cr;
            aimRot[2] = sy * cp;
            aimRot[3] = cp * sr;
            aimRot[4] = cp * cr;
            aimRot[5] = -sp;
            aimRot[6] = -sy * cr + cy * sp * sr;
            aimRot[7] = sy * sr + cy * sp * cr;
            aimRot[8] = cy * cp;

            // Combine with VR controller rotation
            float combined[9];
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    combined[row * 3 + col] =
                        aimRot[row * 3] * vrRot[col] +
                        aimRot[row * 3 + 1] * vrRot[3 + col] +
                        aimRot[row * 3 + 2] * vrRot[6 + col];
                }
            }

            matrix[0]=combined[0]; matrix[1]=combined[1]; matrix[2]=combined[2];
            matrix[4]=combined[3]; matrix[5]=combined[4]; matrix[6]=combined[5];
            matrix[8]=combined[6]; matrix[9]=combined[7]; matrix[10]=combined[8];
        } else {
            // Direct VR controller rotation (original behavior)
            matrix[0]=vrRot[0]; matrix[1]=vrRot[1]; matrix[2]=vrRot[2];
            matrix[4]=vrRot[3]; matrix[5]=vrRot[4]; matrix[6]=vrRot[5];
            matrix[8]=vrRot[6]; matrix[9]=vrRot[7]; matrix[10]=vrRot[8];
        }

        /* Position delta (6DoF) */
        if (posScale > 0) {
            float controllerPosUU[3] = { right.position[0]*200, right.position[1]*200, right.position[2]*200 };
            if (!w.hasRestPosition) {
                w.restPosition[0] = matrix[12]; w.restPosition[1] = matrix[13]; w.restPosition[2] = matrix[14];
                w.hasRestPosition = true;
            }
            float dx = controllerPosUU[0] - w.restPosition[0];
            float dy = controllerPosUU[1] - w.restPosition[1];
            float dz = controllerPosUU[2] - w.restPosition[2];
            matrix[12] = w.restPosition[0] + dx * posScale;
            matrix[13] = w.restPosition[1] + dy * posScale;
            matrix[14] = w.restPosition[2] + dz * posScale;
        }

        WriteProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
    }
    ++m_applyCalls;
}

void InputHook::ApplyLeftHand(int eye) {
    (void)eye;
    if (!m_componentCount) return;
    const auto& xr = XRInput::Instance();
    ControllerState controllers[2] = {};
    xr.GetControllerSnapshot(controllers);
    const auto& left = controllers[0];
    if (!left.valid) return;

    float vrRot[9];
    QuaternionToRotationMatrix(left.rotation[3], left.rotation[0], left.rotation[1], left.rotation[2], vrRot);

    float posScale = config::Get().weapon_position_scale;

    // Get aim offsets from decoupled aim system
    const auto& aim = AimHook::Instance();
    const bool useAimOffset = aim.IsEnabled();

    for (int i = 0; i < m_componentCount; ++i) {
        WeaponComponent& w = m_components[i];
        if (!w.valid || !w.address || !w.matrixOffset || w.hand != 1) continue;

        uintptr_t addr = w.address + w.matrixOffset;
        float matrix[16];
        ReadProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
        memcpy(w.savedMatrix, matrix, sizeof(float)*16);

        // Apply rotation (with aim offset if enabled)
        if (useAimOffset) {
            // Create rotation matrix from aim offsets (in radians)
            constexpr float kUnisToRadians = 6.2831853071795864769f / 65536.0f;
            const float pitchRad = aim.GetAimPitchOffset() * kUnisToRadians;
            const float yawRad = aim.GetAimYawOffset() * kUnisToRadians;
            const float rollRad = aim.GetAimRollOffset() * kUnisToRadians;

            const float cp = cosf(pitchRad), sp = sinf(pitchRad);
            const float cy = cosf(yawRad), sy = sinf(yawRad);
            const float cr = cosf(rollRad), sr = sinf(rollRad);

            // Build rotation matrix: roll * pitch * yaw
            float aimRot[9];
            aimRot[0] = cy * cr + sy * sp * sr;
            aimRot[1] = -cy * sr + sy * sp * cr;
            aimRot[2] = sy * cp;
            aimRot[3] = cp * sr;
            aimRot[4] = cp * cr;
            aimRot[5] = -sp;
            aimRot[6] = -sy * cr + cy * sp * sr;
            aimRot[7] = sy * sr + cy * sp * cr;
            aimRot[8] = cy * cp;

            // Combine with VR controller rotation
            float combined[9];
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    combined[row * 3 + col] =
                        aimRot[row * 3] * vrRot[col] +
                        aimRot[row * 3 + 1] * vrRot[3 + col] +
                        aimRot[row * 3 + 2] * vrRot[6 + col];
                }
            }

            matrix[0]=combined[0]; matrix[1]=combined[1]; matrix[2]=combined[2];
            matrix[4]=combined[3]; matrix[5]=combined[4]; matrix[6]=combined[5];
            matrix[8]=combined[6]; matrix[9]=combined[7]; matrix[10]=combined[8];
        } else {
            // Direct VR controller rotation (original behavior)
            matrix[0]=vrRot[0]; matrix[1]=vrRot[1]; matrix[2]=vrRot[2];
            matrix[4]=vrRot[3]; matrix[5]=vrRot[4]; matrix[6]=vrRot[5];
            matrix[8]=vrRot[6]; matrix[9]=vrRot[7]; matrix[10]=vrRot[8];
        }

        if (posScale > 0) {
            float controllerPosUU[3] = { left.position[0]*200, left.position[1]*200, left.position[2]*200 };
            if (!w.hasRestPosition) {
                w.restPosition[0] = matrix[12]; w.restPosition[1] = matrix[13]; w.restPosition[2] = matrix[14];
                w.hasRestPosition = true;
            }
            float dx = controllerPosUU[0] - w.restPosition[0];
            float dy = controllerPosUU[1] - w.restPosition[1];
            float dz = controllerPosUU[2] - w.restPosition[2];
            matrix[12] = w.restPosition[0] + dx * posScale;
            matrix[13] = w.restPosition[1] + dy * posScale;
            matrix[14] = w.restPosition[2] + dz * posScale;
        }

        WriteProcessMemory(GetCurrentProcess(), (void*)addr, matrix, sizeof(float)*16, NULL);
    }
}

void InputHook::Restore() {
    for (int i = 0; i < m_componentCount; ++i) {
        WeaponComponent& w = m_components[i];
        if (!w.valid || !w.address || !w.matrixOffset) continue;
        uintptr_t addr = w.address + w.matrixOffset;
        WriteProcessMemory(GetCurrentProcess(), (void*)addr, w.savedMatrix, sizeof(float)*16, NULL);
    }
}

void InputHook::Shutdown() {
    ReleaseAllInput();
    if (m_xinputActive) XInputBridge::Instance().Shutdown();
    m_xinputActive = false;
    AimHook::Instance().Shutdown();
    m_installed = false;
    Log("[Input] Shutdown");
}

}} // namespace bl1gotyvr::input
